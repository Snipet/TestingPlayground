// PROBE name=plugin.wheel timeout=60
//
// plugin.wheel: a Logitech G29 racing wheel made MIDI (plugin/WheelMap.h), with made-up HID values (the device code,
// standalone/RacingWheel.cpp, is the Standalone's and needs the wheel: README, "A racing wheel"), and those control
// changes through the processor into the Host block:
//   KIND      vendor and product ids and bcdDevice told apart as hid-lg4ff does: 0xC24F read, 0xC294 a G29's
//             compatibility mode by its bcdDevice (0x1350 under 0xFFF8, 0x8900 under 0xFF00; a G27's 0x1230 is none),
//             0xC260 its PS4 mode, another vendor none
//   COMMANDS  hid-lg4ff's bytes: the switch to its own mode, 900 degrees, the spring (moderate and full), the spring off
//   FIRST     a fresh map's first flush says the whole state (at rest: steering 64, the rest 0; an LSB of 0 never
//             follows its MSB), and a second sends nothing
//   STEERING  16-bit X to 14 bits: full right 127/127, the centre 64 alone (its LSB reset by the MSB), one step right of
//             it the LSB alone, full left 0; -1 and 1 as the synth reads them; a 10-bit range's middle is the centre
//   PEDALS    Z, Rz and Y at their maximum read 0 (released); at their minimum 16383 (127/127); half way about 0.5; a
//             pedal whose rest is its minimum the other way round
//   GEAR      the shifter's buttons 13 to 18 gears 1 to 6, 19 reverse (127), none neutral
//   BUTTONS   cross bit 0 (CC 21), + bit 12 (CC 22), PS bit 17 (CC 23); a button the table has no row for, nothing
//   START     the PS button is the start key too: note-on 36 at velocity 100 after the control changes as it goes down,
//             nothing while held, note-off as it comes up; rest() lets go of a held one
//   HAT       the d-pad (0 to 7, 8 the centre): right, up-right and left as bits 18 to 21, the centre none
//   CHANGE    a value set again sends nothing; an element of another page is refused
//   REST      rest() sends only what brings the synth back to rest (centred, released, neutral, no button), then
//             nothing more (an MSB resets its LSB: no LSB needed); forget() makes the next flush send everything
//   RANGE     a 16-bit maximum written as a signed -1 read as 65535
//   ENGINE    the instrument: the map's control changes, through Processor::processBlock (toEvent) and the Engine, land in
//             the Host block as the map says they do (steering, the three pedals, the gear, the buttons); a source
//             sending only MSBs reads 7 bits; an effect's Host block stays at rest
#include "Probe.h"

#include "dsp/Meters.h"
#include "plugin/Processor.h"
#include "plugin/WheelMap.h"

#include "Product.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <utility>
#include <cstdio>
#include <string>
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
    map.input(kDesk, kX, 32772, 0, 65535);
    same(P, "steering.step_lsb_alone", flushed(map), "48:1");
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
    P.near("pedals.throttle_half", static_cast<double>(map.pedal(wh::Axis::throttle)), 127.0 / 255.0, 1e-3);
    map.input(kDesk, kRz, 0, 0, 255);
    map.input(kDesk, kY, 255, 0, 255);
    same(P, "pedals.brake_down_clutch_released", flushed(map), "18:127 50:127");
    map.input(kDesk, kY, 0, 0, 255);
    same(P, "pedals.clutch_down", flushed(map), "19:127 51:127");
    P.eq("pedals.rest_at_min", wh::Map::axisValue(wh::Axis::throttle, false, 255, 0, 255), 16383);

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
    return P.finish();
}
