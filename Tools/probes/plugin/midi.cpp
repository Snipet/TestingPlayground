// PROBE name=plugin.midi timeout=120
//
// plugin.midi: MIDI learn for the dials (plugin/MidiLearn.h), headless over real Processors, the MIDI in host blocks and
// the message thread's side (Processor::pollMidi) at a clock of the probe's own. The instrument's; the effect learns
// nothing (EFFECT).
//   TABLE   MidiLearn alone: the channel mode messages (120 to 127) refused in words on every channel; the racing
//           wheel's controls (16 to 24, 48 to 51) refused on channel 1 while the Standalone's wheel is on, and learned
//           as any other elsewhere (another channel, no wheel on); describe(); tidy() keeps one control a dial and one
//           dial a control (the later wins), a 14-bit pair only from 0 to 31, a channel 1 to 16, a wheel's control
//   LEARN   learnMidi(cutoff), then CC 74 on channel 3: cutoff learns it (the port's view: its words, learning over), the
//           control that taught it moving nothing; the journal says so
//   MOVE    CC 74 moves cutoff (127: 1, 0: 0) through the dial's port: one host gesture for the touch, ended kTouchMs
//           after its last control change; another channel's CC 74 moves nothing; an int dial learned lands on its
//           values (64 of 127: the middle of three); the moves are the user's in the journal
//   FINE    a 14-bit pair (CC 7, then CC 39) learned as one: (MSB << 7 | LSB) / 16383, an MSB alone resetting its LSB;
//           a control 0 to 31 with no LSB is learned as 7 bits after kPairMs
//   AGAIN   learning a control another dial has takes it from that dial; a dial learns one control (its old one goes)
//   WHEEL   in a host (no racing wheel) a dial learns CC 16 as any control; in the Standalone with its wheel on (made-up
//           devices), CC 17 on channel 1 (its throttle) and CC 123 (All Notes Off) are refused, the note saying why
//           (get_status says it too), and learning goes on until CC 75 comes; the wheel switched off while a dial
//           learns, CC 17 on channel 1 is learned
//   AUTO    the host's automation and a learned control on one dial: the last writer wins, both ways, and a touch still
//           open writes nothing over the automation until its control moves again
//   NEST    the editor's hand and a learned control on one dial at once: one host gesture, from the first begin to the
//           last end (GesturePort)
//   WRITE   Claude's set_dial written while a MIDI touch holds the dial: nested in the touch's gesture (no begin or end
//           of its own), the touch going on and its one end when it ends
//   FORGET  forgetMidi: that dial's control moves it no more, the others still do; forgetAllMidi: none
//   CHAT    get_status's dial lines name each dial's control ("moved by MIDI CC 74 on channel 3"); a mapping whose dial
//           the program lacks is named as waiting, and moves nothing until a program has that dial again; a run that
//           puts a new dial before the mapped ones, read before the message thread has applied it (the mappings still
//           bound to the old slots): run_program's and get_status's lines match the mappings to their dials by name
//   STATE   a session keeps the mappings (<MIDI><MAP/>): a fresh processor loads them and its dials move with them; a
//           state without <MIDI> loads none; a mapping the session should not hold (a channel mode message, a channel
//           99) is left out, a wheel's control kept
//   PROJECT a version saved keeps them ("midi"; "MIDI map" in the note of one whose mappings changed), opening it makes
//           them the mappings, a version with none leaves them as they are; an export and its import keep them; the
//           version with none saved as it was then adds no version for the mappings it left, and one changed since
//           is a change
//   EFFECT  the effect learns nothing (learnMidi false), and a control change moves nothing
#include "Probe.h"

#include "plugin/ChatAgent.h"
#include "plugin/EditorPort.h"
#include "plugin/MidiLearn.h"
#include "plugin/Processor.h"
#include "plugin/ProjectExport.h"
#include "plugin/ProjectStore.h"
#include "plugin/WheelMap.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace pg = playground;
namespace ml = playground::midilearn;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr int kBlock = 256;
    constexpr bool kInstrument = pg::product::kIsInstrument;

    std::string program()
    {
        const std::string params = "    param float cutoff [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                   "    param int mode [[fminus::range(0, 2)]] [[fminus::default(0)]];\n"
                                   "    param float drive [[fminus::range(0, 1)]] [[fminus::default(0)]];\n"
                                   "    param float tone [[fminus::range(0, 1)]] [[fminus::default(0)]];\n"
                                   "    param float level [[fminus::range(0, 1)]] [[fminus::default(0)]];\n";
        if constexpr (kInstrument)
            return "// MIDI: a program\nmodule Voice\n{\n" + params
                 + "    void start(float note, float v) {}\n"
                   "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n";
        else
            return "// MIDI: a program\nmodule Effect\n{\n" + params
                 + "    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
    }

    // The same program without `tone` (a mapping of it then waits).
    std::string programWithoutTone()
    {
        std::string s = program();
        const std::size_t at = s.find("    param float tone");
        s.erase(at, s.find('\n', at) + 1 - at);
        return s;
    }

    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    // The host's side of the parameters: the gestures and the values it is told of.
    struct HostListener final : juce::AudioProcessorListener
    {
        int begins = 0, ends = 0, changes = 0;
        void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override { ++changes; }
        void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override {}
        void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int) override { ++begins; }
        void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int) override { ++ends; }
    };

    void load(pg::Processor& p, const std::string& source)
    {
        p.editorPort().setDraft({ pg::singleFile(source), 0 });   // the editor's files: what a version saves
        p.editorPort().run(pg::singleFile(source));
        p.settle();
        p.setRateAndBufferSizeDetails(kFs, kBlock);
        p.prepareToPlay(kFs, kBlock);
    }

    // One host block with these control changes (channel 1 to 16, number, value) at its start.
    void block(pg::Processor& p, std::initializer_list<std::tuple<int, int, int>> ccs = {})
    {
        juce::AudioBuffer<float> buffer(2, kBlock);
        buffer.clear();
        juce::MidiBuffer midi;
        for (const auto& [ch, cc, v] : ccs)
            midi.addEvent(juce::MidiMessage::controllerEvent(ch, cc, v), 0);
        p.processBlock(buffer, midi);
    }

    int slotOf(pg::Processor& p, const char* name)
    {
        for (int c = 0; c < pg::kMaxControls; ++c)
            if (std::string(p.control(c).name) == name)
                return c;
        return -1;
    }

    float valueOf(pg::Processor& p, int slot) { return p.ports()[static_cast<std::size_t>(slot)]->value01(); }

    juce::var object(std::initializer_list<std::pair<const char*, juce::var>> fields)
    {
        auto* o = new juce::DynamicObject();
        for (const auto& [k, v] : fields)
            o->setProperty(k, v);
        return juce::var(o);
    }

    std::string textOf(const pg::ChatAgent::McpResult& r)
    {
        std::string out;
        if (const juce::Array<juce::var>* c = r.content.getArray())
            for (const juce::var& b : *c)
                if (b["type"].toString() == "text")
                    out += b["text"].toString().toStdString();
        return out;
    }

    std::string status(pg::Processor& p) { return textOf(p.chatAgent().mcpCall("get_status", juce::var(new juce::DynamicObject()))); }

    std::string tokenOf(const std::string& s)
    {
        const std::size_t at = s.rfind("Token: ");
        return at == std::string::npos ? std::string() : s.substr(at + 7, s.find(' ', at + 7) - at - 7);
    }

    std::string wordsOf(pg::Processor& p, int slot) { return p.editorPort().midi().words[static_cast<std::size_t>(slot)]; }

    // The line of `s` that starts with `start` ("" for none).
    std::string lineOf(const std::string& s, const std::string& start)
    {
        const std::size_t at = s.rfind(start, 0) == 0 ? 0 : s.find("\n" + start);
        if (at == std::string::npos)
            return {};
        const std::size_t from = at == 0 ? 0 : at + 1;
        return s.substr(from, s.find('\n', from) - from);
    }

    // The Standalone's devices, made up: a MIDI controller and the racing wheel, on or off (serial() moves when it
    // switches). Nothing else is asked of them here.
    struct Devices final : pg::DevicePort
    {
        bool wheelOn = true;
        uint64_t serialNo = 1;
        void switchWheel(bool on)
        {
            wheelOn = on;
            ++serialNo;
        }
        pg::DeviceSetup setup() const override
        {
            pg::DeviceSetup s;
            s.takesMidi = true;
            s.midi.push_back({ "A controller", "controller", true, "" });
            s.midi.push_back({ pg::wheel::kName, pg::wheel::kIdentifier, wheelOn, "" });
            s.serial = serialNo;
            return s;
        }
        uint64_t serial() const override { return serialNo; }
        void chooseType(const std::string&) override {}
        void chooseOutput(const std::string&) override {}
        void chooseInput(const std::string&) override {}
        void chooseRate(double) override {}
        void chooseBuffer(int) override {}
        void switchPair(bool, int, bool) override {}
        void switchMidi(const std::string&, bool) override {}
        void setMuted(bool) override {}
        void playTestTone() override {}
    };
}

PROBE(plugin, midi)
{
    const funkgui::HeadlessGuiScope gui;

    // ---- TABLE ---------------------------------------------------------------------------------------------------------
    {
        // A racing wheel's controls: refused on channel 1 (the wheel's) while the Standalone's wheel is on, and learned
        // as any other on another channel or with no wheel on (a host has none): a controller's knob on CC 16.
        int wheel = 0, open = 0;
        for (const int cc : { 16, 17, 18, 19, 20, 21, 22, 23, 24, 48, 49, 50, 51 })
        {
            wheel += has(ml::refusal(1, cc, true), "racing wheel") && ml::refused(1, cc, true) ? 1 : 0;
            open += ml::refusal(1, cc, false).empty() && !ml::refused(1, cc, false) && ml::refusal(2, cc, true).empty()
                         && !ml::refused(16, cc, true) ? 1 : 0;
        }
        P.eq("table.wheel_refused_on", wheel, 13);
        P.eq("table.wheel_open_else", open, 13);
        P.eq("table.wheel_named", has(ml::refusal(1, 17, true), "CC 17 on channel 1 is the racing wheel's throttle") ? 1 : 0, 1);
        int others = 0;
        for (const int cc : { 0, 1, 7, 15, 25, 31, 32, 47, 52, 64, 74, 119 })
            others += ml::refusal(1, cc, true).empty() && !ml::refused(1, cc, true) ? 1 : 0;
        P.eq("table.others_open", others, 12);
        int modes = 0;
        for (int cc = 120; cc < 128; ++cc)
            modes += ml::refused(1, cc, false) && ml::refused(9, cc, true) && has(ml::refusal(5, cc, false), "channel mode message") ? 1 : 0;
        P.eq("table.mode_refused", has(ml::refusal(3, 123, false), "CC 123 is a channel mode message (All Notes Off)") && modes == 8 ? 1 : 0, 1);
        P.eq("table.describe", ml::describe({ "cutoff", 1, 74, false }) == "CC 74 on channel 1"
                                       && ml::describe({ "drive", 3, 2, true }) == "CC 2 and 34 (14-bit) on channel 3" ? 1 : 0, 1);
        const std::vector<ml::Mapping> t = ml::tidy({ { "a", 1, 74, false },   // its control taken by e: gone
                                                      { "b", 1, 75, false },   // its dial taken by the later b: gone
                                                      { "c", 0, 76, false },   // no channel 0
                                                      { "d", 1, 40, true },    // 14 bits from 40: no
                                                      { "h", 2, 35, false },   // f's LSB, f later: gone
                                                      { "e", 1, 74, false },
                                                      { "f", 2, 3, true },
                                                      { "b", 1, 77, false },
                                                      { "g", 1, 123, false },  // a channel mode message
                                                      { "w", 1, 16, false },   // the wheel's steering: a mapping as any
                                                      { "", 1, 90, false } });
        P.eq("table.tidy", t == std::vector<ml::Mapping>{ { "e", 1, 74, false }, { "f", 2, 3, true }, { "b", 1, 77, false },
                                                          { "w", 1, 16, false } } ? 1 : 0, 1);
    }

    if constexpr (!kInstrument)
    {
        // ---- EFFECT ----------------------------------------------------------------------------------------------------
        pg::Processor p;
        load(p, program());
        pg::EditorPort& port = p.editorPort();
        const int cutoff = slotOf(p, "cutoff");
        P.eq("effect.not_learnable", port.midiLearnable() ? 0 : 1, 1);
        P.eq("effect.learn_refused", port.learnMidi(cutoff) ? 0 : 1, 1);
        const float before = valueOf(p, cutoff);
        block(p, { { 1, 74, 127 } });
        p.pollMidi(0.0);
        block(p, { { 1, 74, 0 } });
        p.pollMidi(10.0);
        P.eq("effect.nothing_moves", valueOf(p, cutoff) == before && port.midi().learning < 0 ? 1 : 0, 1);
        juce::MemoryBlock state;
        p.getStateInformation(state);
        const std::unique_ptr<juce::XmlElement> xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
        P.eq("effect.no_midi_kept", xml != nullptr && xml->getChildByName("MIDI") == nullptr ? 1 : 0, 1);
        return P.finish();
    }

    const juce::File temp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getChildFile(juce::String(pg::product::kEnvPrefix) + "midi-" + juce::String(juce::Time::currentTimeMillis()));
    temp.deleteRecursively();
    pg::projectstore::setDirectoryForTests(temp);

    pg::Processor p;
    HostListener host;
    p.addListener(&host);
    load(p, program());
    pg::EditorPort& port = p.editorPort();
    const int cutoff = slotOf(p, "cutoff"), mode = slotOf(p, "mode"), drive = slotOf(p, "drive"), tone = slotOf(p, "tone"),
              level = slotOf(p, "level");
    P.eq("setup.dials", cutoff >= 0 && mode >= 0 && drive >= 0 && tone >= 0 && level >= 0 ? 1 : 0, 1);
    P.eq("setup.learnable", port.midiLearnable() ? 1 : 0, 1);
    const std::string token = tokenOf(status(p));
    double now = 1000.0;

    // ---- LEARN ---------------------------------------------------------------------------------------------------------
    {
        P.eq("learn.armed", port.learnMidi(cutoff) && port.midi().learning == cutoff ? 1 : 0, 1);
        P.eq("learn.armed_name", port.midi().learningDial == "cutoff" ? 1 : 0, 1);
        const float before = valueOf(p, cutoff);
        block(p, { { 3, 74, 100 } });
        p.pollMidi(now);
        const pg::midilearn::View v = port.midi();
        P.eq("learn.learned", v.learning == -1 && v.words[static_cast<std::size_t>(cutoff)] == "CC 74 on channel 3" ? 1 : 0, 1);
        const ml::Mapping* m = v.forDial("cutoff");
        P.eq("learn.by_name", v.mappings.size() == 1u && m != nullptr && *m == ml::Mapping{ "cutoff", 3, 74, false }
                                  && v.forDial("drive") == nullptr ? 1 : 0, 1);
        P.eq("learn.mapping", p.editorPort().midi().waiting.empty() ? 1 : 0, 1);
        P.eq("learn.teacher_moves_nothing", valueOf(p, cutoff) == before ? 1 : 0, 1);
        p.poll();
        const std::string since = textOf(p.chatAgent().mcpCall("get_status", object({ { "since", juce::String(token) } })));
        P.eq("learn.journal", has(since, "learned MIDI CC 74 on channel 3 for cutoff") ? 1 : 0, 1);
    }

    // ---- MOVE ----------------------------------------------------------------------------------------------------------
    {
        const std::string before = tokenOf(status(p));
        const int begins = host.begins, ends = host.ends;
        block(p, { { 3, 74, 127 } });
        p.pollMidi(now += 10.0);
        P.near("move.top", static_cast<double>(valueOf(p, cutoff)), 1.0, 1e-7);
        P.near("move.host_value", static_cast<double>(p.getParameters()[cutoff]->getValue()), 1.0, 1e-7);
        P.eq("move.gesture_begun", host.begins - begins, 1);
        block(p, { { 3, 74, 0 } });
        p.pollMidi(now += 10.0);
        P.near("move.bottom", static_cast<double>(valueOf(p, cutoff)), 0.0, 1e-7);
        P.eq("move.one_gesture", host.begins - begins == 1 && host.ends == ends ? 1 : 0, 1);
        p.pollMidi(now += ml::kTouchMs - 1.0);
        P.eq("move.touch_held", host.ends - ends, 0);
        p.pollMidi(now += 2.0);
        P.eq("move.touch_ended", host.ends - ends == 1 && p.midiGestures() == 0 ? 1 : 0, 1);
        block(p, { { 1, 74, 127 }, { 3, 75, 127 } });   // another channel's CC 74, another control
        p.pollMidi(now += 10.0);
        P.near("move.other_channel_nothing", static_cast<double>(valueOf(p, cutoff)), 0.0, 1e-7);
        P.eq("move.other_channel_no_gesture", host.begins - begins, 1);
        // An int dial: on one of its values.
        P.eq("move.int_learned", port.learnMidi(mode) ? 1 : 0, 1);
        block(p, { { 1, 70, 0 } });
        p.pollMidi(now += 10.0);
        block(p, { { 1, 70, 64 } });
        p.pollMidi(now += 10.0);
        P.near("move.int_middle", static_cast<double>(valueOf(p, mode)), 0.5, 1e-7);
        P.near("move.int_plain", static_cast<double>(pg::plainOf(p.control(mode), valueOf(p, mode))), 1.0, 1e-7);
        block(p, { { 1, 70, 100 } });                   // 1.57 of 2: rounds to 2
        p.pollMidi(now += 10.0);
        P.near("move.int_top", static_cast<double>(valueOf(p, mode)), 1.0, 1e-7);
        p.pollMidi(now += ml::kTouchMs + 1.0);
        p.poll();
        const std::string since = textOf(p.chatAgent().mcpCall("get_status", object({ { "since", juce::String(before) } })));
        std::printf("since the moves:\n%s", since.c_str());
        P.eq("move.journal_user", has(since, "the user moved cutoff") && has(since, "the user moved mode") ? 1 : 0, 1);
    }

    // ---- FINE ----------------------------------------------------------------------------------------------------------
    {
        P.eq("fine.armed", port.learnMidi(drive) ? 1 : 0, 1);
        block(p, { { 1, 7, 10 }, { 1, 39, 20 } });
        p.pollMidi(now += 10.0);
        P.eq("fine.learned", wordsOf(p, drive) == "CC 7 and 39 (14-bit) on channel 1" ? 1 : 0, 1);
        block(p, { { 1, 7, 64 }, { 1, 39, 64 } });
        p.pollMidi(now += 10.0);
        P.near("fine.value", static_cast<double>(valueOf(p, drive)), (64.0 * 128.0 + 64.0) / 16383.0, 1e-6);
        block(p, { { 1, 7, 32 } });                     // an MSB alone: its LSB 0
        p.pollMidi(now += 10.0);
        P.near("fine.msb_resets_lsb", static_cast<double>(valueOf(p, drive)), 32.0 * 128.0 / 16383.0, 1e-6);
        block(p, { { 1, 39, 127 } });
        p.pollMidi(now += 10.0);
        P.near("fine.lsb", static_cast<double>(valueOf(p, drive)), (32.0 * 128.0 + 127.0) / 16383.0, 1e-6);
        // A control 0 to 31 with no LSB: 7 bits, once kPairMs has passed.
        P.eq("fine.seven_armed", port.learnMidi(level) ? 1 : 0, 1);
        block(p, { { 2, 5, 64 } });
        p.pollMidi(now += 10.0);
        P.eq("fine.seven_waits", port.midi().learning == level && wordsOf(p, level).empty() ? 1 : 0, 1);
        p.pollMidi(now += ml::kPairMs);
        P.eq("fine.seven_learned", port.midi().learning == -1 && wordsOf(p, level) == "CC 5 on channel 2" ? 1 : 0, 1);
        block(p, { { 2, 5, 127 } });
        p.pollMidi(now += 10.0);
        P.near("fine.seven_moves", static_cast<double>(valueOf(p, level)), 1.0, 1e-7);
        p.pollMidi(now += ml::kTouchMs + 1.0);
    }

    // ---- AGAIN ---------------------------------------------------------------------------------------------------------
    {
        // level learns cutoff's control: cutoff has none then; level's CC 5 goes.
        P.eq("again.armed", port.learnMidi(level) ? 1 : 0, 1);
        block(p, { { 3, 74, 1 } });
        p.pollMidi(now += 10.0);
        P.eq("again.taken", wordsOf(p, level) == "CC 74 on channel 3" && wordsOf(p, cutoff).empty() ? 1 : 0, 1);
        P.eq("again.one_a_dial", port.midi().waiting.empty() && port.midi().forDial("level") != nullptr && port.midi().forDial("cutoff") == nullptr ? 1 : 0, 1);
        // cutoff learns it back (level then has none), for the rows below.
        (void)port.learnMidi(cutoff);
        block(p, { { 3, 74, 1 } });
        p.pollMidi(now += 10.0);
        P.eq("again.back", wordsOf(p, cutoff) == "CC 74 on channel 3" && wordsOf(p, level).empty() ? 1 : 0, 1);
        p.pollMidi(now += ml::kTouchMs + 1.0);
    }

    // ---- WHEEL ---------------------------------------------------------------------------------------------------------
    {
        // In a host there is no racing wheel: a controller's knob on CC 16 is learned, 7 bits once kPairMs
        // has passed with no LSB.
        P.eq("wheel.host_armed", port.learnMidi(tone) ? 1 : 0, 1);
        block(p, { { 1, 16, 64 } });
        p.pollMidi(now += 10.0);
        p.pollMidi(now += ml::kPairMs);
        P.eq("wheel.host_learned", port.midi().learning == -1 && wordsOf(p, tone) == "CC 16 on channel 1" ? 1 : 0, 1);

        // The Standalone with its racing wheel on: its controls on channel 1 are refused while a dial learns.
        Devices devices;
        pg::Processor::WindowHooks hooks;
        hooks.devices = &devices;
        p.setWindowHooks(hooks);
        P.eq("wheel.armed", port.learnMidi(tone) ? 1 : 0, 1);
        block(p, { { 1, 17, 64 } });
        p.pollMidi(now += 10.0);
        pg::midilearn::View v = port.midi();
        P.eq("wheel.refused", v.learning == tone && wordsOf(p, tone) == "CC 16 on channel 1" ? 1 : 0, 1);
        P.eq("wheel.said", has(v.note, "CC 17 on channel 1 is the racing wheel's throttle") ? 1 : 0, 1);
        const std::string st = status(p);
        P.eq("wheel.get_status", has(st, "MIDI learn: tone waits for the user to move a control") && has(st, "racing wheel's throttle") ? 1 : 0, 1);
        block(p, { { 1, 123, 0 } });
        p.pollMidi(now += 10.0);
        v = port.midi();
        P.eq("wheel.mode_refused", v.learning == tone && has(v.note, "All Notes Off") ? 1 : 0, 1);
        block(p, { { 1, 75, 64 } });
        p.pollMidi(now += 10.0);
        v = port.midi();
        P.eq("wheel.then_learned", v.learning == -1 && v.note.empty() && wordsOf(p, tone) == "CC 75 on channel 1" ? 1 : 0, 1);
        // The wheel switched off while a dial learns (its devices' serial moves): CC 17 on channel 1 is learned.
        P.eq("wheel.off_armed", port.learnMidi(level) ? 1 : 0, 1);
        devices.switchWheel(false);
        p.pollMidi(now += 10.0);
        block(p, { { 1, 17, 30 } });
        p.pollMidi(now += 10.0);
        p.pollMidi(now += ml::kPairMs);
        P.eq("wheel.off_learns", port.midi().learning == -1 && port.midi().note.empty() && wordsOf(p, level) == "CC 17 on channel 1" ? 1 : 0, 1);
        (void)port.forgetMidi(level);                    // none for the rows below
        p.setWindowHooks({});
        // Learning stopped (the menu's Stop learning): nothing learns.
        P.eq("wheel.cancel_armed", port.learnMidi(mode) ? 1 : 0, 1);
        port.cancelMidiLearn();
        block(p, { { 1, 80, 64 } });
        p.pollMidi(now += 200.0);
        P.eq("wheel.cancelled", port.midi().learning == -1 && wordsOf(p, mode) == "CC 70 on channel 1" ? 1 : 0, 1);
        p.pollMidi(now += ml::kTouchMs + 1.0);
    }

    // ---- AUTO ----------------------------------------------------------------------------------------------------------
    {
        juce::AudioProcessorParameter& param = *p.getParameters()[cutoff];
        block(p, { { 3, 74, 127 } });
        p.pollMidi(now += 10.0);
        P.near("auto.midi_first", static_cast<double>(param.getValue()), 1.0, 1e-7);
        param.setValueNotifyingHost(0.25f);              // the host's automation: no gesture
        p.pollMidi(now += 10.0);                         // the touch still open: no control moved since
        P.near("auto.automation_wins", static_cast<double>(param.getValue()), 0.25, 1e-7);
        block(p, { { 3, 74, 64 } });
        p.pollMidi(now += 10.0);
        P.near("auto.midi_wins", static_cast<double>(param.getValue()), 64.0 / 127.0, 1e-6);
        p.pollMidi(now += ml::kTouchMs + 1.0);
        param.setValueNotifyingHost(0.75f);
        p.pollMidi(now += 10.0);
        P.near("auto.automation_stays", static_cast<double>(param.getValue()), 0.75, 1e-7);
        block(p, { { 3, 74, 0 } });
        p.pollMidi(now += 10.0);
        P.near("auto.midi_again", static_cast<double>(param.getValue()), 0.0, 1e-7);
        p.pollMidi(now += ml::kTouchMs + 1.0);
        P.eq("auto.no_gesture_left", p.midiGestures() == 0 ? 1 : 0, 1);
    }

    // ---- NEST ----------------------------------------------------------------------------------------------------------
    {
        funkgui::ParamPort& knob = *p.ports()[static_cast<std::size_t>(cutoff)];
        const int begins = host.begins, ends = host.ends;
        knob.beginGesture();                             // the editor's hand
        knob.setValue01(0.4f);
        block(p, { { 3, 74, 100 } });
        p.pollMidi(now += 10.0);
        knob.endGesture();
        P.eq("nest.one_begin", host.begins - begins == 1 && host.ends == ends ? 1 : 0, 1);
        p.pollMidi(now += ml::kTouchMs + 1.0);
        P.eq("nest.one_end", host.begins - begins == 1 && host.ends - ends == 1 ? 1 : 0, 1);
    }

    // ---- WRITE ---------------------------------------------------------------------------------------------------------
    {
        // Claude's set_dial lands (poll(): Processor::writeDial) while a MIDI touch holds cutoff: a gesture of the
        // processor's own nested in the touch's, so the host sees one gesture, which goes on, and one end.
        juce::AudioProcessorParameter& param = *p.getParameters()[cutoff];
        const int begins = host.begins, ends = host.ends;
        block(p, { { 3, 74, 20 } });
        p.pollMidi(now += 10.0);
        P.eq("write.touch_begun", host.begins - begins, 1);
        const pg::ChatAgent::McpResult set = p.chatAgent().mcpCall("set_dial", object({ { "name", "cutoff" }, { "value", 0.75 } }));
        p.poll();
        P.near("write.set", static_cast<double>(param.getValue()), 0.75, 1e-6);
        P.eq("write.nested", !set.isError && host.begins - begins == 1 && host.ends == ends ? 1 : 0, 1);
        block(p, { { 3, 74, 40 } });
        p.pollMidi(now += 10.0);
        P.near("write.touch_goes_on", static_cast<double>(param.getValue()), 40.0 / 127.0, 1e-6);
        P.eq("write.still_one", host.begins - begins == 1 && host.ends == ends ? 1 : 0, 1);
        p.pollMidi(now += ml::kTouchMs + 1.0);
        P.eq("write.one_end", host.begins - begins == 1 && host.ends - ends == 1 && p.midiGestures() == 0 ? 1 : 0, 1);
        // With no touch, set_dial's write is a gesture of its own, begun and ended.
        (void)p.chatAgent().mcpCall("set_dial", object({ { "name", "cutoff" }, { "value", 0.5 } }));
        p.poll();
        P.eq("write.alone", host.begins - begins == 2 && host.ends - ends == 2 ? 1 : 0, 1);
    }

    // ---- CHAT ----------------------------------------------------------------------------------------------------------
    {
        const std::string st = status(p);
        P.eq("chat.cutoff", has(st, ", moved by MIDI CC 74 on channel 3\n") ? 1 : 0, 1);
        P.eq("chat.drive", has(st, "moved by MIDI CC 7 and 39 (14-bit) on channel 1") ? 1 : 0, 1);
        P.eq("chat.no_learning_line", has(st, "MIDI learn:") ? 0 : 1, 1);
        // A program without tone: its mapping waits, named, and moves nothing; back, it moves tone again.
        load(p, programWithoutTone());
        const std::string waiting = status(p);
        P.eq("chat.waiting", has(waiting, "MIDI mappings waiting for a dial of their name: tone (CC 75 on channel 1)") ? 1 : 0, 1);
        P.eq("chat.waiting_view", port.midi().waiting.size() == 1u ? 1 : 0, 1);
        const int toneSlot = slotOf(p, "tone");
        P.eq("chat.tone_gone", toneSlot, -1);
        load(p, program());
        const int back = slotOf(p, "tone");
        block(p, { { 1, 75, 127 } });
        p.pollMidi(now += 10.0);
        P.near("chat.tone_back", static_cast<double>(valueOf(p, back)), 1.0, 1e-7);
        p.pollMidi(now += ml::kTouchMs + 1.0);

        // A run that puts a new dial first, read before poll() applies it: the mappings are bound to the old slots
        // still (cutoff's to slot 1), and the lines match them by name.
        std::string first = program();
        first.insert(first.find("    param float cutoff"), "    param float first [[fminus::range(0, 1)]] [[fminus::default(0)]];\n");
        port.setDraft({ pg::singleFile(first), 0 });
        const std::string ran = textOf(p.chatAgent().mcpCall("run_program", juce::var(new juce::DynamicObject())));
        const std::string statusNow = status(p);
        std::printf("run_program with a dial put first:\n%s\n", ran.c_str());
        P.eq("chat.order_not_applied", slotOf(p, "cutoff") == cutoff && wordsOf(p, cutoff) == "CC 74 on channel 3" ? 1 : 0, 1);
        const std::string firstLine = lineOf(ran, "1 first: "), cutoffLine = lineOf(ran, "2 cutoff: ");
        P.eq("chat.order_run", !firstLine.empty() && !has(firstLine, "MIDI") && has(cutoffLine, ", moved by MIDI CC 74 on channel 3")
                                   && has(lineOf(ran, "4 drive: "), "moved by MIDI CC 7 and 39") ? 1 : 0, 1);
        P.eq("chat.order_status", !has(lineOf(statusNow, "1 first: "), "MIDI") && has(lineOf(statusNow, "2 cutoff: "), "moved by MIDI CC 74 on channel 3")
                                      && !has(statusNow, "MIDI mappings waiting") ? 1 : 0, 1);
        p.settle();
        load(p, program());
    }

    // ---- STATE ---------------------------------------------------------------------------------------------------------
    {
        juce::MemoryBlock state;
        p.getStateInformation(state);
        const std::unique_ptr<juce::XmlElement> xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
        const juce::XmlElement* midi = xml != nullptr ? xml->getChildByName("MIDI") : nullptr;
        P.eq("state.kept", midi != nullptr && midi->getNumChildElements() == 4 ? 1 : 0, 1);

        pg::Processor q;
        q.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        q.poll();
        q.setRateAndBufferSizeDetails(kFs, kBlock);
        q.prepareToPlay(kFs, kBlock);
        P.eq("state.loaded", q.editorPort().midi().words == port.midi().words ? 1 : 0, 1);
        const int qc = slotOf(q, "cutoff");
        block(q, { { 3, 74, 127 } });
        q.pollMidi(10.0);
        P.near("state.moves", static_cast<double>(valueOf(q, qc)), 1.0, 1e-7);
        q.pollMidi(10.0 + ml::kTouchMs + 1.0);

        // A state without <MIDI> (one saved before MIDI learn) loads none, its dials as saved.
        std::unique_ptr<juce::XmlElement> old = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
        old->removeChildElement(old->getChildByName("MIDI"), true);
        juce::MemoryBlock oldState;
        juce::AudioProcessor::copyXmlToBinary(*old, oldState);
        pg::Processor r;
        r.setStateInformation(oldState.getData(), static_cast<int>(oldState.getSize()));
        r.poll();
        int none = 0;
        for (const std::string& w : r.editorPort().midi().words)
            none += w.empty() ? 1 : 0;
        P.eq("state.old_none", none, pg::kMaxControls);
        P.near("state.old_dials", static_cast<double>(r.getParameters()[slotOf(r, "cutoff")]->getValue()),
               static_cast<double>(p.getParameters()[cutoff]->getValue()), 1e-7);

        // A mapping a session should not hold (a channel mode message), and a damaged one: left out; a racing wheel's
        // control is a mapping as any (its refusal is learning's, while the Standalone's wheel is on).
        juce::XmlElement* map = old->createNewChildElement("MIDI")->createNewChildElement("MAP");
        map->setAttribute("dial", "tone");
        map->setAttribute("channel", 1);
        map->setAttribute("cc", 121);
        juce::XmlElement* wheel = old->getChildByName("MIDI")->createNewChildElement("MAP");
        wheel->setAttribute("dial", "mode");
        wheel->setAttribute("channel", 1);
        wheel->setAttribute("cc", 17);
        juce::XmlElement* bad = old->getChildByName("MIDI")->createNewChildElement("MAP");
        bad->setAttribute("dial", "drive");
        bad->setAttribute("channel", 99);
        bad->setAttribute("cc", 9);
        juce::XmlElement* good = old->getChildByName("MIDI")->createNewChildElement("MAP");
        good->setAttribute("dial", "level");
        good->setAttribute("channel", 16);
        good->setAttribute("cc", 9);
        juce::MemoryBlock crafted;
        juce::AudioProcessor::copyXmlToBinary(*old, crafted);
        pg::Processor s;
        s.setStateInformation(crafted.getData(), static_cast<int>(crafted.getSize()));
        s.poll();
        const pg::midilearn::View sv = s.editorPort().midi();
        P.eq("state.refused_left_out", sv.words[static_cast<std::size_t>(slotOf(s, "tone"))].empty()
                                           && sv.words[static_cast<std::size_t>(slotOf(s, "drive"))].empty()
                                           && sv.words[static_cast<std::size_t>(slotOf(s, "level"))] == "CC 9 on channel 16" ? 1 : 0, 1);
        P.eq("state.wheel_kept", sv.words[static_cast<std::size_t>(slotOf(s, "mode"))] == "CC 17 on channel 1" ? 1 : 0, 1);
    }

    // ---- PROJECT -------------------------------------------------------------------------------------------------------
    {
        pg::SavedVersion saved;
        std::string why;
        const std::array<std::string, pg::kMaxControls> words = port.midi().words;
        P.eq("project.saved", port.saveVersion("Midi Learn", saved, why) && saved.number == 1 ? 1 : 0, 1);
        std::optional<pg::projectstore::Project> pr = pg::projectstore::load("Midi Learn", &why);
        P.eq("project.kept", pr && pr->latest().midi.size() == 4u ? 1 : 0, 1);
        P.eq("project.unchanged", port.saveVersion("Midi Learn", saved, why) && saved.unchanged ? 1 : 0, 1);
        P.eq("project.forgot_all", port.forgetAllMidi(), 4);
        P.eq("project.second", port.saveVersion("Midi Learn", saved, why) && saved.number == 2 && !saved.unchanged ? 1 : 0, 1);
        pr = pg::projectstore::load("Midi Learn", &why);
        P.eq("project.second_none", pr && pr->latest().midi.empty() && has(pr->latest().note, "MIDI map") ? 1 : 0, 1);
        P.eq("project.open_v1", port.openProject("Midi Learn", 1, why) ? 1 : 0, 1);
        p.settle();
        P.eq("project.v1_mappings", port.midi().words == words ? 1 : 0, 1);
        P.eq("project.open_v2", port.openProject("Midi Learn", 2, why) ? 1 : 0, 1);
        p.settle();
        P.eq("project.v2_leaves_them", port.midi().words == words ? 1 : 0, 1);
        // The JSON: "midi" only where there are some; an export and its import keep them.
        const std::string json = pg::projectstore::toJson(*pg::projectstore::load("Midi Learn", &why));
        P.eq("project.json", has(json, "\"midi\"") && has(json, "\"cc\"") ? 1 : 0, 1);
        const juce::File pack = temp.getChildFile("Midi Learn.fmpack");
        pg::projectexport::Result res;
        P.eq("project.exported", pg::projectexport::writePackage(*pg::projectstore::load("Midi Learn", &why), true, pack, res, &why) ? 1 : 0, 1);
        const std::optional<pg::projectstore::Project> back = pg::projectexport::read(pack, &why);
        P.eq("project.import_keeps", back && back->versions.size() == 2u && back->versions[0].midi == pr->versions[0].midi
                                         && back->versions[0].midi.size() == 4u ? 1 : 0, 1);
        const std::optional<pg::projectstore::Project> noMidi = pg::projectstore::fromJson(
            "{ \"format\": \"funk-playground-project\", \"version\": 1, \"plugin\": \"" + std::string(pg::product::kName)
                + "\", \"kind\": \"instrument\", \"versions\": [ { \"number\": 1, \"active\": 0, \"files\": [ { \"name\": "
                  "\"program.fm\", \"text\": \"x\" } ], \"dials\": [] } ] }",
            "Old", &why);
        P.eq("project.older_reads", noMidi && noMidi->latest().midi.empty() ? 1 : 0, 1);

        // Version 2 (none) open, after version 1: the mappings it left are 1's. Saving it as it was adds no version for
        // them; one of them forgotten since is a change, which a version keeps ("MIDI map").
        P.eq("project.inherited_unchanged", port.saveVersion("Midi Learn", saved, why) && saved.unchanged && saved.number == 2 ? 1 : 0, 1);
        P.eq("project.inherited_forgot", port.forgetMidi(cutoff) ? 1 : 0, 1);
        P.eq("project.touched_saves", port.saveVersion("Midi Learn", saved, why) && !saved.unchanged && saved.number == 3 ? 1 : 0, 1);
        pr = pg::projectstore::load("Midi Learn", &why);
        P.eq("project.touched_kept", pr && pr->latest().midi.size() == 3u && has(pr->latest().note, "MIDI map") ? 1 : 0, 1);
        P.eq("project.touched_unchanged", port.saveVersion("Midi Learn", saved, why) && saved.unchanged && saved.number == 3 ? 1 : 0, 1);
        // The store alone: mappings a version with none left are no change while the latest has none, else they are.
        const pg::Sources files = pg::singleFile(program());
        const std::vector<ml::Mapping> maps{ { "cutoff", 1, 74, false } };
        P.eq("project.store_first", pg::projectstore::saveVersion("Midi Inherit", files, 0, {}).has_value() ? 1 : 0, 1);
        const auto inherited = pg::projectstore::saveVersion("Midi Inherit", files, 0, {}, {}, { maps, true });
        P.eq("project.store_inherited", inherited && inherited->unchanged ? 1 : 0, 1);
        const auto own = pg::projectstore::saveVersion("Midi Inherit", files, 0, {}, {}, { maps, false });
        P.eq("project.store_own", own && !own->unchanged && own->number == 2 ? 1 : 0, 1);
        const auto latestHas = pg::projectstore::saveVersion("Midi Inherit", files, 0, {}, {}, { {}, true });
        P.eq("project.store_latest_has", latestHas && !latestHas->unchanged && latestHas->number == 3 ? 1 : 0, 1);
        // Version 1's mappings back for the rows below.
        P.eq("project.back_v1", port.openProject("Midi Learn", 1, why) ? 1 : 0, 1);
        p.settle();
        P.eq("project.back_v1_mappings", port.midi().words == words ? 1 : 0, 1);
    }

    // ---- FORGET --------------------------------------------------------------------------------------------------------
    {
        p.pollMidi(now += ml::kTouchMs + 1.0);
        const float driveBefore = valueOf(p, drive);
        P.eq("forget.one", port.forgetMidi(cutoff) ? 1 : 0, 1);
        P.eq("forget.again_none", port.forgetMidi(cutoff) ? 0 : 1, 1);
        const float before = valueOf(p, cutoff);
        block(p, { { 3, 74, before > 0.5f ? 0 : 127 }, { 1, 7, 100 } });
        p.pollMidi(now += 10.0);
        P.eq("forget.no_move", valueOf(p, cutoff) == before ? 1 : 0, 1);
        P.eq("forget.others_move", valueOf(p, drive) != driveBefore ? 1 : 0, 1);
        p.pollMidi(now += ml::kTouchMs + 1.0);
        P.eq("forget.all", port.forgetAllMidi(), 3);
        const float d = valueOf(p, drive);
        block(p, { { 1, 7, d > 0.5f ? 0 : 127 } });
        p.pollMidi(now += 10.0);
        P.eq("forget.all_no_move", valueOf(p, drive) == d && p.midiGestures() == 0 ? 1 : 0, 1);
    }

    p.removeListener(&host);
    pg::projectstore::setDirectoryForTests({});
    temp.deleteRecursively();
    return P.finish();
}
