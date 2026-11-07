// PROBE name=plugin.wheel timeout=60
//
// plugin.wheel: a Logitech G29 racing wheel made MIDI (plugin/WheelMap.h), with made-up HID values (the device code,
// standalone/RacingWheel.cpp, is the Standalone's and needs the wheel: README, "A racing wheel"), and those control
// changes through the processor into the Host block:
//   KIND      vendor and product ids and bcdDevice told apart as hid-lg4ff does: 0xC24F read, 0xC294 a G29's
//             compatibility mode by its bcdDevice (0x1350 under 0xFFF8, 0x8900 under 0xFF00; a G27's 0x1230 is none),
//             0xC260 its PS4 mode, another vendor none
//   COMMANDS  hid-lg4ff's bytes: the switch to its own mode, 900 degrees, the spring (moderate and full), the spring off
//   LIGHTS    the shift lights' report (f8 12, the mask's low five bits, 00 00 00 01) and a program's shiftLights as the
//             mask: an int's low five bits (-1 all five); a float's floor(5 v) from bit 0 (0 none, 0.2 one, 0.6 three,
//             0.99 four, 1 and past it all five, below 0 none, NaN none); a bool's 1 all five
//   FIRST     a fresh map's first flush says the whole state (at rest: steering 64, the rest 0; an LSB of 0 never
//             follows its MSB), and a second sends nothing
//   STEERING  16-bit X to 14 bits: full right 127/127, the centre 64 alone (its LSB reset by the MSB), a few steps right
//             of it the LSB alone, full left 0; -1 and 1 as the synth reads them; a 10-bit range's middle is the centre
//   JITTER    a change under kJitter waits until it grows (then the whole of it goes), across the centre's MSB too,
//             and one to the centre always goes
//   PEDALS    Z, Rz and Y at their maximum read 0 (released); at their minimum 16383 (127/127); half way about 0.5 (past
//             the deadband); a pedal whose rest is its minimum the other way round; within 1 % of rest, rest (kDeadband)
//   GEAR      the shifter's buttons 13 to 18 gears 1 to 6, 19 reverse (127), none neutral
//   BUTTONS   cross bit 0 (CC 21), + bit 12 (CC 22), PS bit 17 (CC 23); a button the table has no row for, nothing
//   START     the PS button is the start key too: note-on 36 at velocity 100 after the control changes as it goes down,
//             nothing while held, note-off as it comes up; rest() lets go of a held one
//   HAT       the d-pad (0 to 7, 8 the centre): right, up-right and left as bits 18 to 21, the centre none
//   CHANGE    a value set again sends nothing; an element of another page is refused
//   REST      rest() sends only what brings the synth back to rest (centred, released, neutral, no button), then
//             nothing more (an MSB resets its LSB: no LSB needed); forget() makes the next flush send everything
//   RESEND    resend(): every control change again, and the start key's note-off when it is up and its note was ever
//             sent; held, no second note-on; a map that never sent a note sends none
//   RANGE     a 16-bit maximum written as a signed -1 read as 65535
//   ENGINE    the instrument: the map's control changes, through Processor::processBlock (toEvent) and the Engine, land in
//             the Host block as the map says they do (steering, the three pedals, the gear, the buttons); a source
//             sending only MSBs reads 7 bits; an effect's Host block stays at rest
//   SCENARIO  (an instrument) a render's scenario moving the steering, the pedals and the gear (ChatScenario's wheels:
//             a gear past 7 and an unknown name refused, the names written back): a voice playing the fields as DC
//             reads them at their times (the steering's -1 exactly), and the marks name each move; the buttons, a
//             mask (listed out of order: played in time order), each part's control change sent only when its 7 bits
//             change, as the wheel sends them, read as host->buttons at their times, the same bits in blocks of 333,
//             a mark a move naming the buttons down; a fraction and a 25th bit refused, "buttons" written back; bit 17
//             the start key, as the wheel plays it: note 36 at velocity 100 after its press's control changes, its
//             note-off after its release's, a second voice meanwhile, measure_output's report then a phrase's
//   DRIVER    plugin/WheelDriver.h's decisions over a fake transport whose devices record what they are sent:
//     READ      on, a G29 in its own mode: 900 degrees, then the spring, then read; the first flush takes only values
//               a report carried (the seed rule: IOKit's zeros before any report, a pedal pressed, the steering left and
//               the hat up, read as rest); a value after a quiet spell goes kGather later, the next no sooner than
//               kInterval after the last flush
//     OFF       switched off: the spring off, listening stops, the rest messages; on again: the range and spring again
//               and the whole state; its spring's stop failing is on the card while off
//     UNPLUG    the wheel gone while read: nothing sent to it, the rest messages, the entry gone
//     RANGE     the range failing: no spring sent, the card says so, the wheel read all the same
//     OTHERS    a G29 at PS4 sent nothing (the card: the switch to PS3), a device of another kind not even listed
//     SWITCH    a compatibility-mode G29, on: hid-lg4ff's two reports and the 8 s timer; it leaving keeps the entry
//               listed, switching, and the timer armed; the G29 coming back ends it and is read; plugged in again in its
//               compatibility mode, not switched again until switched off and on (at most one switch each "on")
//     FAILED    back in its compatibility mode while switching: no second switch, the card says what to do; gone and
//               the timer firing: the same, listed until switched off; its first report failing: the same, no timer
//     DENIED    macOS refusing the manager: nothing switched, the card says where to allow it
//     QUIT      quit() with a G29 read: the spring off and nothing else; after it nothing is sent, switched or read
//     RESEND    resend() while read: the whole state again; nothing while off
//     LIGHTS    the shift lights: read with a program that asks none, none sent and the timer kept every
//               kLightsInterval; a mask asked goes at the timer's next firing, once (asked again: nothing), a change
//               goes, a program that then asks none (-1) turns them off once; switched off with lights lit: the spring
//               off, then the lights off; unplugged: nothing sent, and plugged in again none assumed lit; a report that
//               fails is said on the card, not tried again while the mask holds, and tried when it changes; quit() with
//               lights lit: the spring off, the lights off, the timer disarmed; a program that never lights them sends
//               no lights report through any of it
#include "Probe.h"

#include "dsp/Measure.h"
#include "dsp/Meters.h"
#include "plugin/ChatScenario.h"
#include "plugin/Processor.h"
#include "plugin/WheelDriver.h"
#include "plugin/WheelMap.h"

#include "Product.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pg = playground;
namespace wh = playground::wheel;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;
    constexpr uint32_t kDesk = wh::kDesktop, kBtn = wh::kButtonPage;
    constexpr uint32_t kX = 0x30, kY = 0x31, kZ = 0x32, kRz = 0x35;

    std::string text(const std::vector<wh::Message>& ccs)
    {
        std::string s;
        for (const wh::Message& c : ccs)
            s += std::string(s.empty() ? "" : " ")
               + (c.status == wh::Message::kCc ? "" : c.status == wh::Message::kNoteOn ? "on" : "off")
               + std::to_string(c.number) + ":" + std::to_string(c.value);
        return s;
    }

    std::string text(const wh::Report& r)
    {
        std::string s;
        char b[4];
        for (const uint8_t x : r)
        {
            std::snprintf(b, sizeof b, "%02x", x);
            s += (s.empty() ? "" : " ") + std::string(b);
        }
        return s;
    }

    // The map's flush, as text.
    std::string flushed(wh::Map& map)
    {
        std::vector<wh::Message> out;
        map.flush(out);
        return text(out);
    }

    void same(funkgui::test::Probe& P, const std::string& name, const std::string& got, const std::string& want)
    {
        if (got != want)
            std::printf("%s: got \"%s\", wanted \"%s\"\n", name.c_str(), got.c_str(), want.c_str());
        P.eq(name, got == want ? 1 : 0, 1);
    }

    // A transport of made-up devices (WheelDriver.h): each named, holding the values the probe gives it; what the
    // driver does, in order, as lines ("g29: f8 81 ...", "listen g29", "arm flush 0.008", "send 16:64 ...").
    struct Fake final : wh::Transport
    {
        std::map<wh::Device, std::string> names;
        std::map<wh::Device, std::vector<wh::Value>> held;
        std::vector<std::string> log;
        int failing = 0;                                     // a report whose first two bytes are `failing`'s fails
        double clock = 100.0;
        bool armed[3] = { false, false, false };
        int changes = 0;
        int asked = -1;                                      // the shift lights the program asks for (lights())

        static const char* timerName(wh::Timer t)
        {
            return t == wh::Timer::flush ? "flush" : t == wh::Timer::switched ? "switch" : "lights";
        }

        std::string name(wh::Device d) const { return d == nullptr ? "none" : names.at(d); }

        int report(wh::Device d, const wh::Report& r) override
        {
            log.push_back(name(d) + ": " + text(r));
            return ((r[0] << 8) | r[1]) == failing ? static_cast<int>(0xE00002BCu) : 0;   // kIOReturnError
        }
        std::vector<wh::Value> values(wh::Device d) override { return held[d]; }
        void listen(wh::Device d) override { log.push_back("listen " + name(d)); }
        void arm(wh::Timer t, double seconds) override
        {
            armed[static_cast<int>(t)] = true;
            char b[64];
            std::snprintf(b, sizeof b, "arm %s %.3f", timerName(t), seconds);
            log.emplace_back(b);
        }
        void disarm(wh::Timer t) override
        {
            if (armed[static_cast<int>(t)])
                log.push_back(std::string("disarm ") + timerName(t));
            armed[static_cast<int>(t)] = false;
        }
        double now() override { return clock; }
        // A timer going off: no longer armed, then the driver told.
        void fire(wh::Driver& d, wh::Timer t)
        {
            armed[static_cast<int>(t)] = false;
            d.fire(t);
        }
        void send(const std::vector<wh::Message>& m) override { log.push_back("send " + text(m)); }
        void changed() override { ++changes; }
        int lights() override { return asked; }

        // The lines since the last look, joined by " | " (the timers' and changed's left out unless asked).
        std::string take(bool timers = false)
        {
            std::string s;
            for (const std::string& l : log)
                if (timers || (l.rfind("arm ", 0) != 0 && l.rfind("disarm ", 0) != 0))
                    s += (s.empty() ? "" : " | ") + l;
            log.clear();
            return s;
        }
    };

    [[maybe_unused]] juce::var move(const char* wheel, double at, double value)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("wheel", wheel);
        o->setProperty("at", at);
        o->setProperty("value", value);
        return juce::var(o);
    }

    // A tool's input whose scenario moves these wheels.
    [[maybe_unused]] juce::var wheelsScenario(const juce::Array<juce::var>& moves)
    {
        auto* sc = new juce::DynamicObject();
        sc->setProperty("wheels", moves);
        auto* in = new juce::DynamicObject();
        in->setProperty("scenario", juce::var(sc));
        return juce::var(in);
    }

    // A voice that plays a racing wheel's buttons' mask as DC, 1e-7 a unit (bit 20 about 0.105).
    constexpr const char* kButtonsVoice =
        "#include \"host.fm\"\nmodule Voice\n{\n    Host* host;\n    bool held;\n"
        "    void start(float note, float velocity) { held = velocity > 0 || note > 0; }\n    void stop() { held = false; }\n"
        "    bool process(float* restrict left, float* restrict right, int n)\n    {\n        for (int i = 0; i < n; ++i)\n        {\n"
        "            left[i] += 1e-7f * host->buttons;\n            right[i] += 1e-7f * host->buttons;\n"
        "        }\n        return held;\n    }\n};\n";

    // A voice that plays a racing wheel's fields as DC: the steering on the left, the pedals and the gear on the right.
    constexpr const char* kFieldsVoice =
        "#include \"host.fm\"\nmodule Voice\n{\n    Host* host;\n    bool held;\n"
        "    void start(float note, float velocity) { held = velocity > 0 || note > 0; }\n    void stop() { held = false; }\n"
        "    bool process(float* restrict left, float* restrict right, int n)\n    {\n        for (int i = 0; i < n; ++i)\n        {\n"
        "            left[i] += host->steering;\n"
        "            right[i] += 0.1f * host->throttle + 0.2f * host->brake + 0.4f * host->clutch + 0.05f * host->gear;\n"
        "        }\n        return held;\n    }\n};\n";

    wh::Value hid(uint32_t page, uint32_t usage, int64_t value, int64_t min, int64_t max, uint64_t timestamp)
    {
        return { page, usage, value, min, max, 16, timestamp };
    }
}

PROBE(plugin, wheel)
{
    // ---- KIND ------------------------------------------------------------------------------------------------------
    P.eq("kind.native", wh::kindOf(0x046D, 0xC24F, 0x0001) == wh::Kind::g29 ? 1 : 0, 1);
    P.eq("kind.compat_1350", wh::kindOf(0x046D, 0xC294, 0x1350) == wh::Kind::compat ? 1 : 0, 1);
    P.eq("kind.compat_1357", wh::kindOf(0x046D, 0xC294, 0x1357) == wh::Kind::compat ? 1 : 0, 1);
    P.eq("kind.compat_8900", wh::kindOf(0x046D, 0xC294, 0x89A1) == wh::Kind::compat ? 1 : 0, 1);
    P.eq("kind.g27_compat_none", wh::kindOf(0x046D, 0xC294, 0x1230) == wh::Kind::none ? 1 : 0, 1);
    P.eq("kind.driving_force_none", wh::kindOf(0x046D, 0xC294, 0x0000) == wh::Kind::none ? 1 : 0, 1);
    P.eq("kind.ps4", wh::kindOf(0x046D, 0xC260, 0x8900) == wh::Kind::ps4 ? 1 : 0, 1);
    P.eq("kind.other_vendor", wh::kindOf(0x054C, 0xC24F, 0) == wh::Kind::none ? 1 : 0, 1);

    // ---- COMMANDS --------------------------------------------------------------------------------------------------
    same(P, "commands.revert", text(wh::kToNative[0]), "f8 0a 00 00 00 00 00");
    same(P, "commands.to_g29", text(wh::kToNative[1]), "f8 09 05 01 01 00 00");
    same(P, "commands.range_900", text(wh::rangeReport(900)), "f8 81 84 03 00 00 00");
    same(P, "commands.range_clamped", text(wh::rangeReport(2000)), "f8 81 84 03 00 00 00");
    same(P, "commands.spring", text(wh::springReports(wh::kSpring)[0]), "fe 0d 03 03 48 00 00");
    same(P, "commands.spring_full", text(wh::springReports(0xFFFF)[0]), "fe 0d 07 07 ff 00 00");
    same(P, "commands.spring_on", text(wh::springReports(wh::kSpring)[1]), "14 00 00 00 00 00 00");
    same(P, "commands.spring_off", text(wh::kSpringOff), "f5 00 00 00 00 00 00");

    // ---- LIGHTS ------------------------------------------------------------------------------------------------------
    same(P, "lights.report", text(wh::lightsReport(0x15)), "f8 12 15 00 00 00 01");
    same(P, "lights.report_low_bits", text(wh::lightsReport(0xFF)), "f8 12 1f 00 00 00 01");
    same(P, "lights.report_off", text(wh::lightsReport(0)), "f8 12 00 00 00 00 01");
    P.eq("lights.int_mask", wh::lightsMask(21.0f, true), 21);
    P.eq("lights.int_low_bits", wh::lightsMask(35.0f, true), 3);
    P.eq("lights.int_minus_one", wh::lightsMask(-1.0f, true), 31);
    P.eq("lights.level_none", wh::lightsMask(0.0f, false), 0);
    P.eq("lights.level_fifth", wh::lightsMask(0.2f, false), 1);
    P.eq("lights.level_under_a_fifth", wh::lightsMask(0.19f, false), 0);
    P.eq("lights.level_three", wh::lightsMask(0.6f, false), 7);
    P.eq("lights.level_four", wh::lightsMask(0.99f, false), 15);
    P.eq("lights.level_all", wh::lightsMask(1.0f, false), 31);
    P.eq("lights.level_past", wh::lightsMask(7.5f, false), 31);
    P.eq("lights.level_below", wh::lightsMask(-0.5f, false), 0);
    P.eq("lights.level_nan", wh::lightsMask(std::numeric_limits<float>::quiet_NaN(), false), 0);
    P.eq("lights.bool_all", wh::lightsMask(1.0f, false), 31);

    // ---- FIRST, STEERING, PEDALS, GEAR, BUTTONS, HAT, CHANGE -----------------------------------------------------------
    wh::Map map;
    same(P, "first.whole_state", flushed(map), "16:64 17:0 18:0 19:0 20:0 21:0 22:0 23:0 24:0");
    same(P, "first.then_nothing", flushed(map), "");

    P.eq("steering.known", map.input(kDesk, kX, 65535, 0, 65535) ? 1 : 0, 1);
    same(P, "steering.full_right", flushed(map), "16:127 48:127");
    P.near("steering.right_reads", static_cast<double>(map.steering()), 1.0, 1e-6);
    map.input(kDesk, kX, 32768, 0, 65535);
    same(P, "steering.centre_msb_alone", flushed(map), "16:64");
    P.near("steering.centre_reads", static_cast<double>(map.steering()), 0.0, 1e-6);
    map.input(kDesk, kX, 32788, 0, 65535);                          // 8197
    same(P, "steering.step_lsb_alone", flushed(map), "48:5");
    map.input(kDesk, kX, 32796, 0, 65535);                          // 8199: two steps, under kJitter
    same(P, "jitter.small_waits", flushed(map), "");
    map.input(kDesk, kX, 32800, 0, 65535);                          // 8200: three from what was sent
    same(P, "jitter.grown_goes", flushed(map), "48:8");
    map.input(kDesk, kX, 32772, 0, 65535);                          // 8193
    same(P, "jitter.back_down", flushed(map), "48:1");
    map.input(kDesk, kX, 32768, 0, 65535);                          // the centre: a step, but rest
    same(P, "jitter.rest_always", flushed(map), "48:0");
    map.input(kDesk, kX, 32767, 0, 65535);                          // 8191: a step left, its MSB 63
    same(P, "jitter.across_msb_waits", flushed(map), "");
    map.input(kDesk, kX, 32756, 0, 65535);                          // 8189: three from the centre
    same(P, "jitter.across_msb_grown", flushed(map), "16:63 48:125");
    map.input(kDesk, kX, 32768, 0, 65535);
    same(P, "jitter.across_msb_back", flushed(map), "16:64");
    map.input(kDesk, kX, 0, 0, 65535);
    same(P, "steering.full_left", flushed(map), "16:0");
    P.near("steering.left_reads", static_cast<double>(map.steering()), -1.0, 1e-6);
    P.eq("steering.ten_bit_middle", wh::Map::axisValue(wh::Axis::steering, false, 512, 0, 1023), 8192);
    P.eq("steering.ten_bit_right", wh::Map::axisValue(wh::Axis::steering, false, 1023, 0, 1023), 16368);

    map.input(kDesk, kZ, 255, 0, 255);
    same(P, "pedals.throttle_released", flushed(map), "");
    map.input(kDesk, kZ, 0, 0, 255);
    same(P, "pedals.throttle_down", flushed(map), "17:127 49:127");
    P.near("pedals.throttle_reads", static_cast<double>(map.pedal(wh::Axis::throttle)), 1.0, 1e-6);
    map.input(kDesk, kZ, 128, 0, 255);
    flushed(map);
    P.near("pedals.throttle_half", static_cast<double>(map.pedal(wh::Axis::throttle)), 124.0 / 252.0, 1e-3);   // past 3 of 255
    map.input(kDesk, kRz, 0, 0, 255);
    map.input(kDesk, kY, 255, 0, 255);
    same(P, "pedals.brake_down_clutch_released", flushed(map), "18:127 50:127");
    map.input(kDesk, kY, 0, 0, 255);
    same(P, "pedals.clutch_down", flushed(map), "19:127 51:127");
    P.eq("pedals.rest_at_min", wh::Map::axisValue(wh::Axis::throttle, false, 255, 0, 255), 16383);
    P.eq("pedals.deadband_rest", wh::Map::axisValue(wh::Axis::brake, true, 252, 0, 255), 0);       // 3 of 255 down
    P.eq("pedals.deadband_past", wh::Map::axisValue(wh::Axis::brake, true, 251, 0, 255), 65);      // a step past it
    P.eq("pedals.deadband_10_bit", wh::Map::axisValue(wh::Axis::clutch, true, 1013, 0, 1023), 0);  // 10 of 1023
    P.eq("pedals.deadband_full", wh::Map::axisValue(wh::Axis::clutch, true, 0, 0, 1023), 16383);

    for (int b = 13; b <= 18; ++b)
    {
        map.input(kBtn, static_cast<uint32_t>(b), 1, 0, 1);
        same(P, "gear." + std::to_string(b - 12), flushed(map), "20:" + std::to_string(b - 12));
        map.input(kBtn, static_cast<uint32_t>(b), 0, 0, 1);
    }
    same(P, "gear.neutral", flushed(map), "20:0");
    map.input(kBtn, 19, 1, 0, 1);
    same(P, "gear.reverse", flushed(map), "20:127");
    P.eq("gear.reverse_state", map.state().gear, -1);
    map.input(kBtn, 19, 0, 0, 1);
    same(P, "gear.reverse_out", flushed(map), "20:0");

    map.input(kBtn, 1, 1, 0, 1);
    same(P, "buttons.cross", flushed(map), "21:1");
    map.input(kBtn, 20, 1, 0, 1);
    same(P, "buttons.plus", flushed(map), "22:32");                 // bit 12: CC 22's bit 5
    map.input(kBtn, 25, 1, 0, 1);
    same(P, "buttons.ps_start_key", flushed(map), "23:8 on36:100");    // bit 17: CC 23's bit 3; the start key's note
    same(P, "buttons.start_key_held", flushed(map), "");
    P.eq("buttons.mask", static_cast<int64_t>(map.state().buttons), static_cast<int64_t>(1u | (1u << 12) | (1u << 17)));
    P.eq("buttons.no_row", map.input(kBtn, 30, 1, 0, 1) ? 1 : 0, 0);
    same(P, "buttons.no_row_nothing", flushed(map), "");

    map.input(kDesk, wh::kHat, 2, 0, 7);
    same(P, "hat.right", flushed(map), "23:40");                    // bit 19 (32) beside PS's 8
    map.input(kDesk, wh::kHat, 1, 0, 7);
    same(P, "hat.up_right", flushed(map), "23:56");                 // bits 18 and 19
    map.input(kDesk, wh::kHat, 6, 0, 7);
    same(P, "hat.left", flushed(map), "23:8 24:1");                 // bit 21: CC 24's bit 0
    map.input(kDesk, wh::kHat, 8, 0, 7);
    same(P, "hat.centre", flushed(map), "24:0");

    map.input(kBtn, 25, 0, 0, 1);
    same(P, "start.released_note_off", flushed(map), "23:0 off36:0");
    map.input(kBtn, 25, 1, 0, 1);
    same(P, "start.pressed_again", flushed(map), "23:8 on36:100");

    map.input(kDesk, kY, 0, 0, 255);
    map.input(kBtn, 1, 1, 0, 1);
    same(P, "change.same_again_nothing", flushed(map), "");
    P.eq("change.other_page", map.input(0xFF00, 0x01, 5, 0, 255) ? 1 : 0, 0);

    // ---- REST ------------------------------------------------------------------------------------------------------
    {
        std::vector<wh::Message> out;
        map.rest(out);
        // What stands away from rest now: steering full left (0), throttle half, brake and clutch down, cross, +, PS.
        same(P, "rest.back_to_rest", text(out), "16:64 17:0 18:0 19:0 21:0 22:0 23:0 off36:0");   // each MSB resets its LSB
        out.clear();
        map.rest(out);
        same(P, "rest.then_nothing", text(out), "");
        P.near("rest.steering_centred", static_cast<double>(map.steering()), 0.0, 1e-6);
        same(P, "rest.flush_nothing", flushed(map), "");
        map.forget();
        same(P, "rest.forget_says_all", flushed(map), "16:64 17:0 18:0 19:0 20:0 21:0 22:0 23:0 24:0");
    }

    // ---- RESEND ----------------------------------------------------------------------------------------------------
    {
        wh::Map m;
        flushed(m);
        m.resend();
        same(P, "resend.no_note_ever", flushed(m), "16:64 17:0 18:0 19:0 20:0 21:0 22:0 23:0 24:0");
        m.input(kDesk, kZ, 0, 0, 255);
        m.input(kBtn, 25, 1, 0, 1);
        flushed(m);
        m.resend();
        same(P, "resend.held_no_note_again", flushed(m), "16:64 17:127 49:127 18:0 19:0 20:0 21:0 22:0 23:8 24:0");
        m.input(kBtn, 25, 0, 0, 1);
        same(P, "resend.let_go", flushed(m), "23:0 off36:0");
        m.resend();
        same(P, "resend.up_note_off_again", flushed(m), "16:64 17:127 49:127 18:0 19:0 20:0 21:0 22:0 23:0 24:0 off36:0");
        same(P, "resend.then_nothing", flushed(m), "");
    }

    // ---- RANGE -----------------------------------------------------------------------------------------------------
    {
        int64_t v = -1, lo = 0, hi = -1;
        wh::normalise(v, lo, hi, 16);
        P.eq("range.max", hi, 65535);
        P.eq("range.value", v, 65535);
        int64_t v2 = 300, lo2 = 0, hi2 = 1023;
        wh::normalise(v2, lo2, hi2, 10);
        P.eq("range.left_alone", hi2 == 1023 && v2 == 300 ? 1 : 0, 1);
    }

    // ---- ENGINE: the map's control changes through the processor ----------------------------------------------------
    {
        wh::Map m;
        m.input(kDesk, kX, 49152, 0, 65535);                         // three quarters right: 12288
        m.input(kDesk, kZ, 64, 0, 255);                              // throttle three quarters down
        m.input(kDesk, kRz, 255, 0, 255);                            // brake released
        m.input(kDesk, kY, 0, 0, 255);                               // clutch down
        m.input(kBtn, 19, 1, 0, 1);                                  // reverse
        m.input(kBtn, 5, 1, 0, 1);                                   // the right paddle
        m.input(kDesk, wh::kHat, 4, 0, 7);                           // the d-pad down
        std::vector<wh::Message> ccs;
        m.flush(ccs);

        pg::Processor p;
        p.setRateAndBufferSizeDetails(48000.0, 256);
        p.prepareToPlay(48000.0, 256);
        p.settle();
        juce::AudioBuffer<float> buffer(2, 256);
        juce::MidiBuffer midi;
        int at = 0;
        for (const wh::Message& c : ccs)
            midi.addEvent(juce::MidiMessage::controllerEvent(1, c.number, c.value), at++);
        p.processBlock(buffer, midi);
        const pg::HostBlock& h = p.engine().host();
        if constexpr (kInstrument)
        {
            P.near("engine.steering", static_cast<double>(h.steering), static_cast<double>(m.steering()), 1e-6);
            P.near("engine.steering_value", static_cast<double>(h.steering), 4096.0 / 8191.0, 1e-6);
            P.near("engine.throttle", static_cast<double>(h.throttle), static_cast<double>(m.pedal(wh::Axis::throttle)), 1e-6);
            P.near("engine.brake", static_cast<double>(h.brake), 0.0, 1e-9);
            P.near("engine.clutch", static_cast<double>(h.clutch), 1.0, 1e-6);
            P.eq("engine.gear", h.gear, -1);
            P.eq("engine.buttons", static_cast<int64_t>(h.buttons), static_cast<int64_t>(m.state().buttons));
            P.eq("engine.buttons_bits", static_cast<int64_t>(h.buttons), static_cast<int64_t>((1u << 4) | (1u << 20)));
            // A source of 7-bit controllers alone (a knob on CC 17): its MSB, the LSB reset.
            midi.clear();
            midi.addEvent(juce::MidiMessage::controllerEvent(1, 17, 127), 0);
            midi.addEvent(juce::MidiMessage::controllerEvent(1, 16, 0), 1);
            p.processBlock(buffer, midi);
            P.near("engine.msb_alone", static_cast<double>(p.engine().host().throttle), 16256.0 / 16383.0, 1e-6);
            P.near("engine.msb_alone_left", static_cast<double>(p.engine().host().steering), -1.0, 1e-6);
            // And the values stay while nothing changes them.
            midi.clear();
            p.processBlock(buffer, midi);
            P.near("engine.stays", static_cast<double>(p.engine().host().throttle), 16256.0 / 16383.0, 1e-6);
            // The start key: a note while the PS button is held.
            P.eq("engine.no_voice_yet", p.engine().activeVoices(), 0);
            const auto play = [&](bool down) {
                m.input(kBtn, static_cast<uint32_t>(wh::kStartButton), down ? 1 : 0, 0, 1);
                std::vector<wh::Message> out;
                m.flush(out);
                midi.clear();
                for (const wh::Message& x : out)
                    midi.addEvent(juce::MidiMessage(x.status, x.number, x.value), 0);
                p.processBlock(buffer, midi);
            };
            const auto keys = [&] {
                pg::Meters mt;
                p.engine().meters(mt);
                return std::make_pair(static_cast<int>(mt.held), static_cast<int>(mt.lastNote));
            };
            play(true);
            P.eq("engine.start_key_voice", p.engine().activeVoices(), 1);
            P.eq("engine.start_key_bit", (p.engine().host().buttons >> 17) & 1, 1);
            P.eq("engine.start_key_held", keys().first, 1);
            P.eq("engine.start_key_note", keys().second, wh::kStartNote);
            play(false);
            P.eq("engine.start_key_let_go", keys().first, 0);
        }
        else
            P.eq("engine.effect_at_rest", h.steering == 0.0f && h.throttle == 0.0f && h.brake == 0.0f && h.clutch == 0.0f
                                              && h.gear == 0 && h.buttons == 0 ? 1 : 0, 1);
    }

    // ---- SCENARIO: a render's racing wheel (ChatScenario's wheels) ----------------------------------------------------
    if constexpr (kInstrument)
    {
        pg::measure::Request req;
        req.notes = { 60 };
        req.holdSeconds = 0.6f;
        req.tailSeconds = 0.1f;
        std::string why;
        const bool read = pg::scenario::read(wheelsScenario({ move("steering", 0.1, 0.5), move("throttle", 0.15, 0.8),
                                                              move("brake", 0.2, 1.0), move("clutch", 0.2, 0.25),
                                                              move("gear", 0.25, -1.0), move("steering", 0.3, -1.0) }),
                                             req, &why);
        P.eq("scenario.read", read && req.wheels.size() == 6 ? 1 : 0, 1);
        pg::measure::Request bad;
        std::string gearWhy, nameWhy;
        P.eq("scenario.gear_range", !pg::scenario::read(wheelsScenario({ move("gear", 0.1, 9.0) }), bad, &gearWhy)
                                        && gearWhy.find("wheels[0]") != std::string::npos ? 1 : 0, 1);
        P.eq("scenario.unknown", !pg::scenario::read(wheelsScenario({ move("horn", 0.1, 1.0) }), bad, &nameWhy)
                                     && nameWhy.find("steering, throttle, brake, clutch, gear or buttons") != std::string::npos ? 1 : 0, 1);
        P.eq("scenario.written_back", pg::scenario::toVar(req)["wheels"][4]["wheel"].toString() == "gear" ? 1 : 0, 1);
        pg::measure::Session session(pg::singleFile(kFieldsVoice));
        P.eq("scenario.compiles", session.compiled() ? 1 : 0, 1);
        if (session.compiled())
        {
            const pg::measure::Session::Render out = session.render(req, nullptr, nullptr, session.lengthOf(req));
            const auto at = [&](const std::vector<float>& ch, double t) {
                const auto i = static_cast<std::size_t>(t * 48000.0);
                return i < ch.size() ? static_cast<double>(ch[i]) : 1e9;
            };
            P.near("scenario.at_rest", at(out.r, 0.05) + std::fabs(at(out.l, 0.05)), 0.0, 1e-9);
            P.near("scenario.steering", at(out.l, 0.12), 4096.0 / 8191.0, 1e-6);   // 12288
            P.near("scenario.throttle", at(out.r, 0.17), 0.1 * 13106.0 / 16383.0, 1e-6);
            P.near("scenario.pedals", at(out.r, 0.22), 0.1 * 13106.0 / 16383.0 + 0.2 + 0.4 * 4096.0 / 16383.0, 1e-6);
            P.near("scenario.reverse", at(out.r, 0.27), 0.1 * 13106.0 / 16383.0 + 0.2 + 0.4 * 4096.0 / 16383.0 - 0.05, 1e-6);
            P.near("scenario.full_left", at(out.l, 0.32), -1.0, 1e-9);
            std::string marks;
            for (const pg::measure::Mark& mk : session.timeline(req).marks)
                if (mk.note < 0)
                    marks += (marks.empty() ? "" : "; ") + mk.what;
            same(P, "scenario.marks", marks, "steering to +0.50; the throttle to 0.80; the brake to 1.00; the clutch to 0.25; "
                                             "the gear to reverse; steering to -1.00");
        }

        // The buttons: a mask, listed out of order and played in time order, each part sent as the wheel sends it.
        pg::measure::Request pressed;
        pressed.notes = { 60 };
        pressed.holdSeconds = 0.6f;
        pressed.tailSeconds = 0.1f;
        const double bits45 = (1 << 4) | (1 << 5), bit20 = 1 << 20, bit17 = 1 << 17;
        const bool readButtons = pg::scenario::read(wheelsScenario({ move("buttons", 0.3, bit17), move("buttons", 0.1, bits45),
                                                                     move("buttons", 0.2, bits45 + bit20) }),
                                                    pressed, &why);
        P.eq("scenario.buttons_read", readButtons && pressed.wheels.size() == 3 && pressed.wheels[0].wheel == pg::measure::Wheel::buttons ? 1 : 0, 1);
        std::string fractionWhy, highWhy;
        P.eq("scenario.buttons_fraction_refused", !pg::scenario::read(wheelsScenario({ move("buttons", 0.1, 1.5) }), bad, &fractionWhy)
                                             && fractionWhy.find("whole number") != std::string::npos ? 1 : 0, 1);
        P.eq("scenario.buttons_bit_24_refused", !pg::scenario::read(wheelsScenario({ move("buttons", 0.1, 16777216.0) }), bad, &highWhy)
                                           && highWhy.find("wheels[0]") != std::string::npos ? 1 : 0, 1);
        P.eq("scenario.buttons_written_back", pg::scenario::toVar(pressed)["wheels"][0]["wheel"].toString() == "buttons"
                                         && static_cast<double>(pg::scenario::toVar(pressed)["wheels"][0]["value"]) == bit17 ? 1 : 0, 1);
        pg::measure::Session buttons(pg::singleFile(kButtonsVoice));
        P.eq("scenario.buttons_compiles", buttons.compiled() ? 1 : 0, 1);
        if (buttons.compiled())
        {
            const pg::measure::Session::Render out = buttons.render(pressed, nullptr, nullptr, buttons.lengthOf(pressed));
            const auto at = [&](double t) {
                const auto i = static_cast<std::size_t>(t * 48000.0);
                return i < out.l.size() ? static_cast<double>(out.l[i]) : 1e9;
            };
            P.near("scenario.buttons_at_rest", at(0.05), 0.0, 1e-12);
            P.near("scenario.buttons_four_five", at(0.15), 1e-7 * bits45, 1e-9);
            P.near("scenario.buttons_and_twenty", at(0.25), 1e-7 * (bits45 + bit20), 1e-8);
            P.near("scenario.buttons_seventeen", at(0.35), 2e-7 * bit17, 1e-8);   // two voices: note 60's and the start key's
            // As the wheel sends them: 21 at 0.1 s, 23 at 0.2 s, 21 and 23 at 0.3 s (22 and 24 never change), none at the
            // start (none down is the Engine's rest).
            std::string sent;
            const pg::measure::Timeline tl = buttons.timeline(pressed);
            for (const auto& [sample, e] : tl.events)
                if (e.kind == pg::EventKind::controller && e.note >= pg::wheelcc::kButtons && e.note < pg::wheelcc::kButtons + pg::wheelcc::kButtonCcs)
                    sent += (sent.empty() ? "" : " ") + std::to_string(sample) + ":" + std::to_string(e.note) + "="
                          + std::to_string(static_cast<int>(e.velocity * 127.0f + 0.5f));
            same(P, "scenario.buttons_sent", sent, "4800:21=48 9600:23=64 14400:21=0 14400:23=8");
            std::string marks;
            for (const pg::measure::Mark& mk : tl.marks)
                if (mk.note < 0)
                    marks += (marks.empty() ? "" : "; ") + mk.what;
            same(P, "scenario.buttons_marks", marks, "buttons 4 and 5 down; buttons 4, 5 and 20 down; button 17 down");
            pg::measure::Request odd = pressed;
            odd.block = 333;
            const pg::measure::Session::Render other = buttons.render(odd, nullptr, nullptr, buttons.lengthOf(odd));
            P.eq("scenario.buttons_blocks_same_bits", other.l == out.l && other.r == out.r ? 1 : 0, 1);

            // The start key, as the wheel plays it: bit 17 down sends note 36 at velocity 100 after the control changes of
            // its press, and its note-off after those of its release; no other bit does. A second voice while it is
            // down, and measure_output's report takes what plays as a phrase (the notes held and the start key's).
            pg::measure::Request start;
            start.notes = { 60 };
            start.holdSeconds = 0.6f;
            start.tailSeconds = 0.1f;
            const double bit4 = 1 << 4;
            const bool readStart = pg::scenario::read(wheelsScenario({ move("buttons", 0.1, bit17 + bit4), move("buttons", 0.2, bit4),
                                                                       move("buttons", 0.3, 0.0) }),
                                                      start, &why);
            pg::measure::Request others;
            others.wheels.push_back({ pg::measure::Wheel::buttons, 0.1, static_cast<float>(pg::measure::kMaxButtons - bit17) });
            P.eq("scenario.start_key_read", readStart && start.startKey() && !others.startKey() ? 1 : 0, 1);
            std::string played;
            const pg::measure::Timeline startPlan = buttons.timeline(start);
            for (const auto& [sample, e] : startPlan.events)
            {
                const std::string when = std::to_string(sample) + ":";
                if (e.kind == pg::EventKind::controller && e.note >= pg::wheelcc::kButtons && e.note < pg::wheelcc::kButtons + pg::wheelcc::kButtonCcs)
                    played += (played.empty() ? "" : " ") + when + std::to_string(e.note) + "=" + std::to_string(static_cast<int>(e.velocity * 127.0f + 0.5f));
                else if (e.kind == pg::EventKind::noteOn && e.note != 60)
                    played += (played.empty() ? "" : " ") + when + "on" + std::to_string(e.note) + "@" + std::to_string(static_cast<int>(e.velocity * 127.0f + 0.5f));
                else if (e.kind == pg::EventKind::noteOff && e.note != 60)
                    played += (played.empty() ? "" : " ") + when + "off" + std::to_string(e.note);
            }
            same(P, "scenario.start_key_sent", played, "4800:21=16 4800:23=8 4800:on36@100 9600:23=0 9600:off36 14400:21=0");
            const pg::measure::Session::Render started = buttons.render(start, nullptr, nullptr, buttons.lengthOf(start));
            const auto startAt = [&](double t) {
                const auto i = static_cast<std::size_t>(t * 48000.0);
                return i < started.l.size() ? static_cast<double>(started.l[i]) : 1e9;
            };
            P.near("scenario.start_key_two_voices", startAt(0.15), 2e-7 * (bit17 + bit4), 1e-8);
            P.near("scenario.start_key_let_go", startAt(0.25), 1e-7 * bit4, 1e-9);
            const std::string report = pg::measure::run(pg::singleFile(kButtonsVoice), start);
            P.eq("scenario.start_key_phrase", report.find("Played: from the first note-on") != std::string::npos
                                                  && report.find("Note 60 (") == std::string::npos ? 1 : 0, 1);
        }
    }

    // ---- DRIVER ------------------------------------------------------------------------------------------------------
    {
        int tags[5]{};
        const wh::Device g29 = &tags[0], compat = &tags[1], compat2 = &tags[2], ps4 = &tags[3], other = &tags[4];
        Fake f;
        f.names = { { g29, "g29" }, { compat, "compat" }, { compat2, "compat2" }, { ps4, "ps4" }, { other, "other" } };
        // What IOKit holds before reports: zeros with no timestamp (a pedal at 0 is pressed, the steering full left, the
        // hat up, the PS button down); the steering from a report (three quarters right: 12288, MSB 96).
        f.held[g29] = { hid(kDesk, kX, 49152, 0, 65535, 7), hid(kDesk, kZ, 0, 0, 255, 0), hid(kDesk, kRz, 0, 0, 255, 0),
                        hid(kDesk, kY, 0, 0, 255, 0), hid(kDesk, wh::kHat, 0, 0, 7, 0), hid(kBtn, 25, 1, 0, 1, 0) };
        const std::string range = "g29: f8 81 84 03 00 00 00", spring = "g29: fe 0d 03 03 48 00 00 | g29: 14 00 00 00 00 00 00";
        const std::string off = "g29: f5 00 00 00 00 00 00", whole = "send 16:96 17:0 18:0 19:0 20:0 21:0 22:0 23:0 24:0";
        const std::string toNative = ": f8 0a 00 00 00 00 00 | ", toNative2 = ": f8 09 05 01 01 00 00";
        const std::string failed = "it did not switch to its full mode: plug it in again, then turn this off and on";

        // READ
        wh::Driver d(f, true);
        d.opened(0);
        P.eq("driver.nothing_listed", d.present() ? 0 : 1, 1);
        d.added(g29, wh::Kind::g29);
        same(P, "driver.read.range_spring_seed", f.take(), range + " | " + spring + " | listen g29 | " + whole);
        same(P, "driver.read.status", d.status(), "");
        f.clock = 100.002;                                           // 2 ms after the first flush
        d.value(g29, hid(kDesk, kZ, 0, 0, 255, 9));                  // the throttle down, from a report
        same(P, "driver.rate.interval", f.take(true), "arm flush 0.006");
        f.clock = 100.008;
        f.fire(d, wh::Timer::flush);
        same(P, "driver.rate.flushed", f.take(true), "send 17:127 49:127");
        f.clock = 101.0;                                             // after a quiet spell
        d.value(g29, hid(kDesk, kZ, 255, 0, 255, 10));
        d.value(g29, hid(kDesk, kRz, 255, 0, 255, 10));              // one report's values, one flush
        f.fire(d, wh::Timer::flush);
        same(P, "driver.rate.gather", f.take(true), "arm flush 0.001 | send 17:0");
        d.value(g29, hid(0xFF00, 1, 5, 0, 255, 11));                 // no row
        d.value(compat, hid(kDesk, kZ, 0, 0, 255, 11));              // not the one read
        same(P, "driver.read.others_nothing", f.take(true), "");

        // OFF
        d.setOn(false);
        same(P, "driver.off.spring_off_rest", f.take(), "listen none | " + off + " | send 16:64");
        P.eq("driver.off.listed", d.present() && !d.on() && d.status().empty() ? 1 : 0, 1);
        d.setOn(true);
        same(P, "driver.off.on_again", f.take(), range + " | " + spring + " | listen g29 | " + whole);
        f.failing = 0xf500;
        d.setOn(false);
        f.failing = 0;
        same(P, "driver.off.spring_failing", d.status(), "its spring may still be on (0xe00002bc): unplug it to stop it");
        d.setOn(true);
        f.take();
        same(P, "driver.off.cleared_on", d.status(), "");

        // UNPLUG
        d.removed(g29);
        same(P, "driver.unplug.rest_nothing_to_it", f.take(), "listen none | send 16:64");
        P.eq("driver.unplug.gone", d.present() ? 0 : 1, 1);

        // RANGE
        f.failing = 0xf881;
        d.added(g29, wh::Kind::g29);
        f.failing = 0;
        same(P, "driver.range.no_spring", f.take(), range + " | listen g29 | " + whole);
        same(P, "driver.range.said", d.status(), "its range and spring were not set (0xe00002bc)");
        d.removed(g29);
        f.take();

        // OTHERS
        d.added(ps4, wh::Kind::ps4);
        same(P, "driver.others.ps4_nothing_sent", f.take(true), "");
        same(P, "driver.others.ps4_said", d.status(), "set the switch on the wheel to PS3");
        d.removed(ps4);
        d.added(other, wh::Kind::none);
        P.eq("driver.others.other_not_listed", d.present() ? 0 : 1, 1);
        same(P, "driver.others.other_nothing_sent", f.take(true), "");

        // SWITCH
        d.added(compat, wh::Kind::compat);
        same(P, "driver.switch.reports_timer", f.take(true), "compat" + toNative + "compat" + toNative2 + " | arm switch 8.000");
        same(P, "driver.switch.said", d.status(), "switching to its full mode");
        d.removed(compat);                                           // re-enumerating
        P.eq("driver.switch.listed_while_away", d.present() && f.armed[1] ? 1 : 0, 1);
        same(P, "driver.switch.still_said", d.status(), "switching to its full mode");
        same(P, "driver.switch.timer_kept", f.take(true), "");
        d.added(g29, wh::Kind::g29);
        same(P, "driver.switch.back_read", f.take(true),
             "disarm switch | " + range + " | " + spring + " | listen g29 | " + whole + " | arm lights 0.025");
        same(P, "driver.switch.done", d.status(), "");
        d.removed(g29);                                              // unplugged and plugged in again: compatibility mode
        d.added(compat2, wh::Kind::compat);
        same(P, "driver.switch.once_each_on", f.take(true), "listen none | disarm lights | send 16:64");
        same(P, "driver.switch.once_said", d.status(), "back in its PS3 mode: turn this off and on to switch it");
        d.setOn(false);
        same(P, "driver.switch.off_said", d.status(), "in its PS3 mode: switched to its full mode when on");
        d.setOn(true);
        same(P, "driver.switch.on_again", f.take(true), "compat2" + toNative + "compat2" + toNative2 + " | arm switch 8.000");

        // FAILED
        d.removed(compat2);
        d.added(compat, wh::Kind::compat);                           // it came back in its compatibility mode
        same(P, "driver.failed.back_no_second", f.take(true), "disarm switch");
        same(P, "driver.failed.back_said", d.status(), failed);
        d.setOn(false);
        d.setOn(true);
        f.take();
        d.removed(compat);
        f.fire(d, wh::Timer::switched);                                 // 8 s and nothing came back
        P.eq("driver.failed.late_listed", d.present() ? 1 : 0, 1);
        same(P, "driver.failed.late_said", d.status(), failed);
        d.setOn(false);
        P.eq("driver.failed.off_gone", d.present() ? 0 : 1, 1);
        f.failing = 0xf80a;
        d.added(compat, wh::Kind::compat);
        d.setOn(true);
        f.failing = 0;
        same(P, "driver.failed.first_report", f.take(true), "compat: f8 0a 00 00 00 00 00");
        same(P, "driver.failed.first_report_said", d.status(),
             "it could not be switched to its full mode (0xe00002bc): plug it in again, then turn this off and on");
        d.removed(compat);
        d.setOn(false);
        f.take(true);

        // DENIED
        {
            wh::Driver denied(f, true);
            denied.opened(wh::Driver::kNotPermitted);
            denied.added(compat, wh::Kind::compat);
            same(P, "driver.denied.nothing_sent", f.take(true), "");
            same(P, "driver.denied.said", denied.status(), "macOS keeps it from this app: allow it in Privacy & Security, Input Monitoring");
            denied.quit();
        }

        // RESEND
        d.setOn(true);
        d.added(g29, wh::Kind::g29);
        f.clock = 200.0;
        d.value(g29, hid(kDesk, kZ, 0, 0, 255, 20));
        f.fire(d, wh::Timer::flush);
        f.take(true);
        d.resend();
        same(P, "driver.resend.whole", f.take(true), "send 16:96 17:127 49:127 18:0 19:0 20:0 21:0 22:0 23:0 24:0");

        // LIGHTS (still read, from RESEND)
        {
            const std::string lights = "g29: f8 12 ";
            f.fire(d, wh::Timer::lights);                        // the program asks none
            same(P, "driver.lights.none_asked", f.take(true), "arm lights 0.025");
            f.asked = 0x07;
            f.fire(d, wh::Timer::lights);
            same(P, "driver.lights.sent", f.take(true), lights + "07 00 00 00 01 | arm lights 0.025");
            f.fire(d, wh::Timer::lights);
            same(P, "driver.lights.held_nothing", f.take(), "");
            f.asked = 0x1F;
            f.fire(d, wh::Timer::lights);
            same(P, "driver.lights.changed", f.take(), lights + "1f 00 00 00 01");
            f.asked = -1;
            f.fire(d, wh::Timer::lights);
            f.fire(d, wh::Timer::lights);
            same(P, "driver.lights.none_then_off_once", f.take(), lights + "00 00 00 00 01");
            f.asked = 0x03;
            f.fire(d, wh::Timer::lights);
            f.take();
            d.setOn(false);
            same(P, "driver.lights.off_spring_then_lights", f.take(true),
                 "listen none | " + off + " | " + lights + "00 00 00 00 01 | disarm lights | send 16:64 17:0");
            P.eq("driver.lights.off_disarmed", f.armed[static_cast<int>(wh::Timer::lights)] ? 0 : 1, 1);
            d.setOn(true);                                       // read again: the lights asked go at once
            same(P, "driver.lights.on_again_at_once", f.take(true),
                 range + " | " + spring + " | listen g29 | " + whole + " | " + lights + "03 00 00 00 01 | arm lights 0.025");
            d.removed(g29);
            same(P, "driver.lights.unplug_nothing_to_it", f.take(true), "listen none | disarm lights | send 16:64");
            f.asked = 0;
            d.added(g29, wh::Kind::g29);                         // none lit assumed: nothing sent for 0
            same(P, "driver.lights.plugged_none_assumed", f.take(true),
                 range + " | " + spring + " | listen g29 | " + whole + " | arm lights 0.025");
            f.failing = 0xf812;
            f.asked = 0x01;
            f.fire(d, wh::Timer::lights);
            f.fire(d, wh::Timer::lights);
            same(P, "driver.lights.failing_once", f.take(), lights + "01 00 00 00 01");
            same(P, "driver.lights.failing_said", d.status(), "its shift lights were not set (0xe00002bc)");
            f.failing = 0;
            f.asked = 0x02;
            f.fire(d, wh::Timer::lights);
            same(P, "driver.lights.tried_on_change", f.take(), lights + "02 00 00 00 01");
        }

        // QUIT
        d.quit();
        same(P, "driver.quit.spring_off_lights_off", f.take(true), "listen none | " + off + " | g29: f8 12 00 00 00 00 01 | disarm lights");
        f.asked = 0x1F;
        f.fire(d, wh::Timer::lights);
        d.value(g29, hid(kDesk, kZ, 255, 0, 255, 21));
        f.fire(d, wh::Timer::flush);
        d.added(compat, wh::Kind::compat);
        d.setOn(false);
        d.resend();
        same(P, "driver.quit.then_nothing", f.take(true), "");

        wh::Driver idle(f, false);
        idle.added(g29, wh::Kind::g29);
        idle.resend();
        same(P, "driver.resend.off_nothing", f.take(true), "");
        idle.quit();
        same(P, "driver.quit.off_nothing", f.take(true), "");
    }
    return P.finish();
}
