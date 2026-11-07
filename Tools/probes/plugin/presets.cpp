// PROBE name=plugin.presets timeout=180
//
// plugin.presets: the project open's presets in turn (EditorPort::presetCycle, stepPreset; the host's programs; MIDI's
// program change) through a real Processor, in a folder of the probe's own (never the user's):
//   NONE     with no project open there is nothing to cycle: stepPreset refused (why says to open one); the host's
//            list is one program, "(no project open)"
//   CYCLE    a project's three presets (Bright, Dark, Warm: by name) are the cycle in that order, the one saved last
//            applied; the next from the last wraps to the first, the one before the first to the last, each applied
//            (its dial there, project().preset it, not edited)
//   MATCH    a version opened (no preset applied) whose dials are no preset's: none ("(no preset)"); the dials turned to
//            Dark's by hand: Dark, matched (not applied, not edited); from none the next is the first and the one
//            before the last
//   EDITED   a dial moved off the preset applied: edited (the modified dot); the next applied leaves the moved value
//            behind (the next one's dials) and the preset as the store keeps it (its dials as saved)
//   OTHER    a preset another process added (the file rewritten in place) is in the cycle once the store's Reader has
//            read it (never a file read whole on the message thread: ProjectStore's parses on the poll's side none)
//   HOST     the host's programs are the presets: their count and names; setCurrentProgram applies one (the host's
//            program and project().preset it); getCurrentProgram follows a preset applied from elsewhere, and keeps the
//            last while the dials match none; a number past them changes nothing. A VST3 host's list is a fixed 128
//            (JUCE reads it once), those past the presets named "-"; another project opened whose dials match none of
//            its presets: the "-" past them, not the number the one before had (JUCE's VST3 program parameter sets a
//            program only when it is not the current one: the old number chosen again in the new project applies); the
//            AU's and the Standalone's list, which has no "-": its first
//   SESSION  a session saved with a preset applied, loaded by another processor in a store that has not read the
//            project (its file copied to a folder of its own, as in a host just started): that preset is its current
//            program (applied) as soon as the load returns, before any poll, so a host that sets the program it saved
//            changes nothing; a dial moved then: edited (the dot, dialsEdited), and the next preset's notice says the
//            changes were not saved; a session of a version with a dial moved off it: dialsEdited
//   MIDI     (the synth) a program change on channel 1 applies the preset of that number (0 the first) at the next
//            poll; one on channel 2 changes nothing, and nor does a number past them. The effect takes no MIDI
//   FACTORY  a factory project open cycles its presets the same way; their category stays (setPresetCategory refused)
//   CATEGORY setPresetCategory sets a preset's alone (tidied), the cycle shows it, its JSON has it ("category"); a
//            preset saved over keeps it; "" clears it (no field written); a preset that is not there refused; a file
//            from before categories reads none
//   RAMP     stepping from Bright (level 0.8) to Dark (0.2) while it plays: the output glides there as a dial turned
//            does, the Engine's linear 20 ms (0.6 / 960 = 0.000625 a sample: no step between two samples over 0.001;
//            from the first sample off Bright's level to the first at Dark's 20 ms, within 1; half way at 10 ms; at
//            Dark's 30 ms on)
#include "Probe.h"

#include "editor/PresetPicker.h"
#include "plugin/EditorPort.h"
#include "plugin/ProjectStore.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;
    constexpr double kFs = 48000.0;
    constexpr int kBlock = 256;

    // Two dials, level and tone; the output is level itself (the effect's, whatever its input; the synth's voice's
    // while its key is held), so a level's glide is the output's.
    pg::Sources program()
    {
        const std::string params = "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                   "    param float tone [[fminus::range(100, 10000)]] [[fminus::default(1000)]];\n";
        const std::string text = kInstrument
            ? "module Voice\n{\n" + params
                  + "    bool held;\n    void start(float n, float v) { held = true; }\n    void stop() { held = false; }\n"
                    "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                    "        for (int i = 0; i < n; ++i) { left[i] += level[i]; right[i] += level[i]; }\n"
                    "        return held;\n    }\n};\n"
            : "module Effect\n{\n" + params
                  + "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                    "        for (int i = 0; i < n; ++i) { left[i] = level[i]; right[i] = level[i]; }\n    }\n};\n";
        return { { pg::kProgramName, text } };
    }

    float dial(pg::Processor& p, int slot) { return pg::plainOf(p.control(slot), p.getParameters()[slot]->getValue()); }

    void setDial(pg::Processor& p, int slot, float plain)
    {
        p.getParameters()[slot]->setValueNotifyingHost(pg::normOf(p.control(slot), plain));
    }

    std::string nameAt(const pg::PresetCycle& c)
    {
        return c.current >= 0 && c.current < static_cast<int>(c.presets.size()) ? c.presets[static_cast<std::size_t>(c.current)].name
                                                                                 : std::string("(none)");
    }

    std::vector<std::string> namesOf(const pg::PresetCycle& c)
    {
        std::vector<std::string> out;
        for (const pg::PresetEntry& e : c.presets)
            out.push_back(e.name);
        return out;
    }

    // `n` samples of the processor's output (the left channel), a key held for the synth from the first block.
    std::vector<float> render(pg::Processor& p, int n, bool noteOn = false)
    {
        std::vector<float> out;
        for (int done = 0; done < n; done += kBlock)
        {
            juce::AudioBuffer<float> buffer(2, kBlock);
            buffer.clear();
            juce::MidiBuffer midi;
            if (noteOn && done == 0)
                midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 0);
            p.processBlock(buffer, midi);
            for (int i = 0; i < kBlock; ++i)
                out.push_back(buffer.getSample(0, i));
        }
        return out;
    }

    [[maybe_unused]] void midiBlock(pg::Processor& p, const juce::MidiMessage& m)
    {
        juce::AudioBuffer<float> buffer(2, kBlock);
        buffer.clear();
        juce::MidiBuffer midi;
        midi.addEvent(m, 3);
        p.processBlock(buffer, midi);
    }
}

PROBE(plugin, presets)
{
    const funkgui::HeadlessGuiScope gui;
    namespace ps = pg::projectstore;
    const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile(juce::String(pg::product::kEnvPrefix) + "presets-probe-" + juce::String(juce::Time::currentTimeMillis()));
    dir.deleteRecursively();
    ps::setDirectoryForTests(dir);

    pg::Processor a;
    pg::EditorPort& pa = a.editorPort();
    a.setRateAndBufferSizeDetails(kFs, kBlock);
    a.prepareToPlay(kFs, kBlock);
    const pg::Sources files = program();
    pa.setDraft({ files, 0 });
    pa.run(files);
    a.settle();
    P.eq("setup.runs", pa.status().ok ? 1 : 0, 1);
    std::string why;

    // NONE
    P.eq("none.no_cycle", pa.presetCycle().project.empty() && pa.presetCycle().presets.empty() ? 1 : 0, 1);
    P.eq("none.step_refused", !pa.stepPreset(1, why) && why.find("open a project first") != std::string::npos ? 1 : 0, 1);
    P.eq("none.host_one", a.getNumPrograms() == 1 && a.getProgramName(0) == "(no project open)" ? 1 : 0, 1);

    // CYCLE: Glass Bloom, version 1 at level 0.4, its presets Warm (0.5, tone 500), Bright (0.8) and Dark (0.2), Dark
    // saved last.
    setDial(a, 0, 0.4f);
    pg::SavedVersion saved;
    P.eq("cycle.saved", pa.saveVersion("Glass Bloom", saved, why) ? 1 : 0, 1);
    setDial(a, 0, 0.5f);
    setDial(a, 1, 500.0f);
    bool presetsSaved = pa.savePreset("Warm", why);
    setDial(a, 1, 1000.0f);
    setDial(a, 0, 0.8f);
    presetsSaved = presetsSaved && pa.savePreset("Bright", why);
    setDial(a, 0, 0.2f);
    presetsSaved = presetsSaved && pa.savePreset("Dark", why);
    P.eq("cycle.presets_saved", presetsSaved ? 1 : 0, 1);
    a.settle();
    pg::PresetCycle c = pa.presetCycle();
    std::printf("cycle: %s, %zu presets, current %s%s\n", c.project.c_str(), c.presets.size(), nameAt(c).c_str(), c.applied ? " (applied)" : "");
    P.eq("cycle.listed", c.project == "Glass Bloom" && namesOf(c) == std::vector<std::string>{ "Bright", "Dark", "Warm" } ? 1 : 0, 1);
    P.eq("cycle.saved_last_current", c.current == 1 && c.applied && !c.edited ? 1 : 0, 1);
    P.eq("cycle.next", pa.stepPreset(1, why) && nameAt(pa.presetCycle()) == "Warm" && pa.project().preset == "Warm" ? 1 : 0, 1);
    P.near("cycle.next_dial", static_cast<double>(dial(a, 1)), 500.0, 0.05);
    P.eq("cycle.wraps_next", pa.stepPreset(1, why) && nameAt(pa.presetCycle()) == "Bright" ? 1 : 0, 1);
    P.near("cycle.wraps_next_dial", static_cast<double>(dial(a, 0)), 0.8, 1e-4);
    P.eq("cycle.wraps_back", pa.stepPreset(-1, why) && nameAt(pa.presetCycle()) == "Warm" ? 1 : 0, 1);
    P.eq("cycle.back", pa.stepPreset(-1, why) && nameAt(pa.presetCycle()) == "Dark" && !pa.project().dialsEdited ? 1 : 0, 1);
    P.near("cycle.back_dial", static_cast<double>(dial(a, 0)), 0.2, 1e-4);

    // MATCH: version 1 (level 0.4, tone 1000) is none of them.
    P.eq("match.open", pa.openProject("Glass Bloom", 1, why) ? 1 : 0, 1);
    a.settle();
    c = pa.presetCycle();
    P.eq("match.none", c.current == -1 && !c.applied && c.presets.size() == 3 ? 1 : 0, 1);
    setDial(a, 0, 0.2f);                                  // Dark's level; its tone is 1000 already
    c = pa.presetCycle();
    P.eq("match.found", nameAt(c) == "Dark" && !c.applied && !c.edited && pa.project().preset.empty() ? 1 : 0, 1);
    setDial(a, 0, 0.33f);
    P.eq("match.from_none_next", pa.presetCycle().current == -1 && pa.stepPreset(1, why) && nameAt(pa.presetCycle()) == "Bright" ? 1 : 0, 1);
    setDial(a, 0, 0.33f);
    {
        // The preset applied stays current while edited: none only once nothing is applied (a version opened).
        P.eq("match.reopen", pa.openProject("Glass Bloom", 1, why) ? 1 : 0, 1);
        a.settle();
        setDial(a, 0, 0.33f);
    }
    P.eq("match.from_none_back", pa.presetCycle().current == -1 && pa.stepPreset(-1, why) && nameAt(pa.presetCycle()) == "Warm" ? 1 : 0, 1);

    // EDITED: Bright applied, its level moved.
    P.eq("edited.apply", pa.applyPreset("Glass Bloom", "Bright", why) ? 1 : 0, 1);
    setDial(a, 0, 0.6f);
    c = pa.presetCycle();
    P.eq("edited.dot", nameAt(c) == "Bright" && c.applied && c.edited ? 1 : 0, 1);
    P.eq("edited.step_away", pa.stepPreset(1, why) && nameAt(pa.presetCycle()) == "Dark" && !pa.presetCycle().edited ? 1 : 0, 1);
    P.near("edited.next_dial", static_cast<double>(dial(a, 0)), 0.2, 1e-4);
    {
        const std::optional<ps::Project> p = ps::load("Glass Bloom");
        const ps::Preset* bright = p ? p->preset("Bright") : nullptr;
        bool kept = bright != nullptr;
        for (const ps::Dial& d : bright != nullptr ? bright->dials : std::vector<ps::Dial>())
            if (d.name == "level")
                kept = kept && std::fabs(d.value - 0.8f) < 1e-6f;
        P.eq("edited.not_kept", kept ? 1 : 0, 1);
    }

    // HOST
    P.eq("host.count", a.getNumPrograms(), 3);
    P.eq("host.names", a.getProgramName(0) == "Bright" && a.getProgramName(1) == "Dark" && a.getProgramName(2) == "Warm" ? 1 : 0, 1);
    P.eq("host.current", a.getCurrentProgram(), 1);
    a.setCurrentProgram(2);
    a.settle();
    P.eq("host.set", pa.project().preset == "Warm" && a.getCurrentProgram() == 2 ? 1 : 0, 1);
    P.near("host.set_dial", static_cast<double>(dial(a, 1)), 500.0, 0.05);
    P.eq("host.follows", pa.applyPreset("Glass Bloom", "Bright", why) && a.getCurrentProgram() == 0 ? 1 : 0, 1);
    pa.openProject("Glass Bloom", 1, why);
    a.settle();
    setDial(a, 0, 0.33f);
    a.settle();
    P.eq("host.kept_on_none", pa.presetCycle().current == -1 && a.getCurrentProgram() == 0 ? 1 : 0, 1);
    a.setCurrentProgram(7);
    a.settle();
    P.near("host.past_them_nothing", static_cast<double>(dial(a, 0)), 0.33, 1e-4);
    {
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
        pg::Processor v;
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
        v.editorPort().setDraft({ files, 0 });
        v.editorPort().run(files);
        v.settle();
        const int before = v.getNumPrograms();
        v.editorPort().openProject("Glass Bloom", 0, why);
        v.settle();
        std::printf("vst3: %d programs, then %d: %s, %s, %s\n", before, v.getNumPrograms(), v.getProgramName(0).toRawUTF8(),
                    v.getProgramName(2).toRawUTF8(), v.getProgramName(3).toRawUTF8());
        P.eq("host.vst3_fixed", before == 128 && v.getNumPrograms() == 128 ? 1 : 0, 1);
        P.eq("host.vst3_names", v.getProgramName(0) == "Bright" && v.getProgramName(2) == "Warm" && v.getProgramName(3) == "-"
                                    && v.getProgramName(127) == "-" ? 1 : 0, 1);
        // Its third (Warm), then another project whose version's dials match none of its presets (One, Three, Two).
        v.setCurrentProgram(2);
        v.settle();
        P.eq("host.vst3_set", v.editorPort().project().preset == "Warm" && v.getCurrentProgram() == 2 ? 1 : 0, 1);
        bool other = ps::saveVersion("Other Song", files, 0, { { "level", 0.1f }, { "tone", 3000.0f } }, {}, &why).has_value();
        other = other && ps::savePreset("Other Song", "One", { { "level", 0.7f } }, 1, &why);
        other = other && ps::savePreset("Other Song", "Two", { { "level", 0.75f } }, 1, &why);
        other = other && ps::savePreset("Other Song", "Three", { { "level", 0.85f } }, 1, &why);
        P.eq("host.other_saved", other && v.editorPort().openProject("Other Song", 0, why) ? 1 : 0, 1);
        v.settle();
        const pg::PresetCycle oc = v.editorPort().presetCycle();
        std::printf("vst3, another project: %s, current %s, program %d (%s)\n", oc.project.c_str(), nameAt(oc).c_str(),
                    v.getCurrentProgram(), v.getProgramName(v.getCurrentProgram()).toRawUTF8());
        P.eq("host.vst3_other_none", oc.project == "Other Song" && oc.current == -1 && v.getCurrentProgram() == 3
                                         && v.getProgramName(3) == "-" ? 1 : 0, 1);
        // As JUCE's VST3 program parameter does (setNormalized): a program set only when it is not the current one.
        if (v.getCurrentProgram() != 2)
            v.setCurrentProgram(2);
        v.settle();
        P.eq("host.vst3_other_applies", v.editorPort().project().preset == "Two" && v.getCurrentProgram() == 2 ? 1 : 0, 1);
    }
    {
        // The AU's and the Standalone's list has no "-": another project matching none is at its first.
        pg::Processor u;
        u.editorPort().setDraft({ files, 0 });
        u.editorPort().run(files);
        u.settle();
        u.editorPort().openProject("Glass Bloom", 0, why);
        u.settle();
        u.setCurrentProgram(2);
        u.settle();
        const bool third = u.getCurrentProgram() == 2;
        u.editorPort().openProject("Other Song", 0, why);
        u.settle();
        P.eq("host.other_project_first", third && u.editorPort().presetCycle().current == -1 && u.getCurrentProgram() == 0 ? 1 : 0, 1);
    }

    // SESSION: Dark applied, the session loaded elsewhere, in a store whose cache has none of the project (its file
    // copied to a folder of its own: another path), as in a host just started.
    pa.applyPreset("Glass Bloom", "Dark", why);
    {
        juce::MemoryBlock state;
        a.getStateInformation(state);
        pa.openProject("Glass Bloom", 1, why);              // version 1 (level 0.4), a dial moved off it
        a.settle();
        setDial(a, 0, 0.66f);
        juce::MemoryBlock versionState;
        a.getStateInformation(versionState);
        const juce::File cold = dir.getSiblingFile(dir.getFileName() + "-cold");
        cold.deleteRecursively();
        const bool copied = cold.createDirectory().wasOk() && dir.getChildFile("Glass Bloom.fmproject").copyFileTo(cold.getChildFile("Glass Bloom.fmproject"));
        ps::setDirectoryForTests(cold);
        {
            pg::Processor b;
            pg::EditorPort& pb = b.editorPort();
            b.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            const pg::PresetCycle at = pb.presetCycle();  // before any poll
            const int program = b.getCurrentProgram();
            std::printf("session at once: %s, %zu presets, current %s, program %d\n", at.project.c_str(), at.presets.size(),
                        nameAt(at).c_str(), program);
            P.eq("session.at_once", copied && at.presets.size() == 3 && nameAt(at) == "Dark" && at.applied && program == 1 ? 1 : 0, 1);
            b.settle();
            const pg::PresetCycle bc = pb.presetCycle();
            std::printf("session: %s, current %s, program %d\n", bc.project.c_str(), nameAt(bc).c_str(), b.getCurrentProgram());
            P.eq("session.current", bc.project == "Glass Bloom" && nameAt(bc) == "Dark" && bc.applied && !bc.edited
                                        && !pb.project().dialsEdited && b.getCurrentProgram() == 1 ? 1 : 0, 1);
            setDial(b, 0, 0.6f);
            const pg::PresetCycle moved = pb.presetCycle();
            P.eq("session.edited", nameAt(moved) == "Dark" && moved.applied && moved.edited && pb.project().dialsEdited ? 1 : 0, 1);
            const bool stepped = pb.stepPreset(1, why);
            const std::string notice = pg::ui::presetNotice(moved, pb.presetCycle());
            std::printf("session step: %s\n", notice.c_str());
            P.eq("session.step_not_saved", stepped && nameAt(pb.presetCycle()) == "Warm"
                                               && notice.find("THE CHANGES TO DARK WERE NOT SAVED") != std::string::npos ? 1 : 0, 1);
        }
        {
            pg::Processor b;
            b.setStateInformation(versionState.getData(), static_cast<int>(versionState.getSize()));
            b.settle();
            const pg::ProjectState st = b.editorPort().project();
            P.eq("session.version_edited", st.name == "Glass Bloom" && st.version == 1 && st.preset.empty() && st.dialsEdited ? 1 : 0, 1);
            setDial(b, 0, 0.4f);
            P.eq("session.version_back", b.editorPort().project().dialsEdited ? 0 : 1, 1);
        }
        ps::setDirectoryForTests(dir);
        cold.deleteRecursively();
    }
    pa.applyPreset("Glass Bloom", "Dark", why);          // as the session had it, for the rows below

    // OTHER: a preset added to the file by another process (rewritten in place).
    {
        juce::var root = juce::JSON::parse(dir.getChildFile("Glass Bloom.fmproject").loadFileAsString());
        auto* o = new juce::DynamicObject();
        o->setProperty("name", "Airy");
        o->setProperty("saved", static_cast<juce::int64>(juce::Time::currentTimeMillis()));
        o->setProperty("from", 1);
        juce::Array<juce::var> dials;
        auto* d = new juce::DynamicObject();
        d->setProperty("name", "level");
        d->setProperty("value", 0.9);
        dials.add(juce::var(d));
        o->setProperty("dials", dials);
        root["presets"].getArray()->add(juce::var(o));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));   // a time of its own
        dir.getChildFile("Glass Bloom.fmproject").replaceWithText(juce::JSON::toString(root));
        bool seen = false;
        for (int k = 0; k < 100 && !seen; ++k)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            a.settle();
            seen = pa.presetCycle().presets.size() == 4;
        }
        P.eq("other.seen", seen && pa.presetCycle().presets.front().name == "Airy" ? 1 : 0, 1);
        P.eq("other.host_names", a.getNumPrograms() == 4 && a.getProgramName(0) == "Airy" ? 1 : 0, 1);
    }

    // MIDI
    if constexpr (kInstrument)
    {
        P.eq("midi.takes_it", a.acceptsMidi() ? 1 : 0, 1);
        midiBlock(a, juce::MidiMessage::programChange(1, 2));   // the third: Dark (Airy, Bright, Dark, Warm)
        a.settle();
        P.eq("midi.channel_1", pa.project().preset == "Dark" ? 1 : 0, 1);
        P.near("midi.channel_1_dial", static_cast<double>(dial(a, 0)), 0.2, 1e-4);
        midiBlock(a, juce::MidiMessage::programChange(2, 0));
        a.settle();
        P.eq("midi.channel_2_nothing", pa.project().preset == "Dark" ? 1 : 0, 1);
        midiBlock(a, juce::MidiMessage::programChange(1, 40));
        a.settle();
        P.eq("midi.past_them_nothing", pa.project().preset == "Dark" ? 1 : 0, 1);
        midiBlock(a, juce::MidiMessage::programChange(1, 3));
        a.settle();
        P.eq("midi.another", pa.project().preset == "Warm" && a.getCurrentProgram() == 3 ? 1 : 0, 1);
    }
    else
        P.eq("midi.effect_takes_none", a.acceptsMidi() ? 0 : 1, 1);

    // CATEGORY
    P.eq("category.set", pa.setPresetCategory("Glass Bloom", "dark", "  Moody   Things ", why) ? 1 : 0, 1);
    c = pa.presetCycle();
    P.eq("category.shown", c.presets.size() == 4 && c.presets[2].name == "Dark" && c.presets[2].category == "Moody Things"
                               && c.presets[0].category.empty() ? 1 : 0, 1);
    {
        const juce::var root = juce::JSON::parse(dir.getChildFile("Glass Bloom.fmproject").loadFileAsString());
        bool inJson = false, noneWritten = true;
        if (const juce::Array<juce::var>* presets = root["presets"].getArray())
            for (const juce::var& p : *presets)
            {
                if (p["name"].toString() == "Dark")
                    inJson = p["category"].toString() == "Moody Things";
                else if (p.hasProperty("category"))
                    noneWritten = false;
            }
        P.eq("category.json", inJson && noneWritten ? 1 : 0, 1);
    }
    pa.applyPreset("Glass Bloom", "Dark", why);
    P.eq("category.kept_over", pa.savePreset("DARK", why) && pa.presetCycle().presets[2].category == "Moody Things"
                                   && pa.presetCycle().presets[2].name == "DARK" ? 1 : 0, 1);
    P.eq("category.presets_entry", pa.presets("Glass Bloom").size() == 4 && pa.presets("Glass Bloom")[2].category == "Moody Things" ? 1 : 0, 1);
    P.eq("category.cleared", pa.setPresetCategory("Glass Bloom", "Dark", "", why) && pa.presetCycle().presets[2].category.empty() ? 1 : 0, 1);
    why.clear();
    P.eq("category.missing_refused", !pa.setPresetCategory("Glass Bloom", "Nope", "X", why) && why.find("no preset named") != std::string::npos ? 1 : 0, 1);
    {
        // A file from before categories (the field taken out of every preset): none.
        pa.setPresetCategory("Glass Bloom", "Warm", "Soft", why);
        juce::var root = juce::JSON::parse(dir.getChildFile("Glass Bloom.fmproject").loadFileAsString());
        for (juce::var& p : *root["presets"].getArray())
            if (auto* o = p.getDynamicObject())
                o->removeProperty("category");
        dir.getChildFile("Older.fmproject").replaceWithText(juce::JSON::toString(root));
        const std::optional<ps::Project> older = ps::load("Older");
        P.eq("category.older_file", older && older->presets.size() == 4
                                        && std::all_of(older->presets.begin(), older->presets.end(), [](const ps::Preset& p) { return p.category.empty(); })
                                        ? 1 : 0, 1);
    }

    // FACTORY
    {
        const std::vector<pg::ProjectEntry> factory = pa.factoryProjects();
        const auto it = std::find_if(factory.begin(), factory.end(), [](const pg::ProjectEntry& e) { return e.presets > 0; });
        P.eq("factory.has_presets", it != factory.end() ? 1 : 0, 1);
        if (it != factory.end())
        {
            pg::Processor f;
            pg::EditorPort& pf = f.editorPort();
            P.eq("factory.open", pf.openProject(it->name, 0, why) ? 1 : 0, 1);
            f.settle();
            const pg::PresetCycle fc = pf.presetCycle();
            std::printf("factory: %s, %zu presets\n", fc.project.c_str(), fc.presets.size());
            P.eq("factory.cycle", fc.project == it->name && static_cast<int>(fc.presets.size()) == it->presets
                                      && f.getNumPrograms() == it->presets ? 1 : 0, 1);
            P.eq("factory.steps", pf.stepPreset(1, why) && pf.presetCycle().current >= 0 && pf.presetCycle().applied ? 1 : 0, 1);
            why.clear();
            P.eq("factory.category_refused", !pf.setPresetCategory(it->name, fc.presets[0].name, "Mine", why) && !why.empty() ? 1 : 0, 1);
        }
    }

    // RAMP: Bright (0.8) to Dark (0.2) while it plays.
    {
        pg::Processor r;
        pg::EditorPort& pr = r.editorPort();
        r.setRateAndBufferSizeDetails(kFs, kBlock);
        r.prepareToPlay(kFs, kBlock);
        pr.openProject("Glass Bloom", 0, why);
        r.settle();
        pr.applyPreset("Glass Bloom", "Bright", why);
        (void)render(r, 24 * kBlock, true);              // the program in, its key held, Bright's level reached
        const std::vector<float> before = render(r, kBlock);
        P.near("ramp.at_bright", static_cast<double>(before.back()), 0.8, 1e-3);
        P.eq("ramp.step", pr.stepPreset(1, why) && pr.project().preset == "DARK" ? 1 : 0, 1);
        const std::vector<float> after = render(r, 8 * kBlock);
        float most = std::fabs(after.front() - before.back());
        for (std::size_t i = 1; i < after.size(); ++i)
            most = std::max(most, std::fabs(after[i] - after[i - 1]));
        // The glide's length: from the first sample off Bright's level to the first at Dark's (each within 1e-3).
        int left = -1, landed = -1;
        for (std::size_t i = 0; i < after.size(); ++i)
        {
            if (left < 0 && std::fabs(after[i] - 0.8f) > 1e-3f)
                left = static_cast<int>(i);
            if (left >= 0 && landed < 0 && std::fabs(after[i] - 0.2f) < 1e-3f)
                landed = static_cast<int>(i);
        }
        const double ms = left >= 0 && landed > left ? (landed - left) * 1000.0 / kFs : 0.0;
        const std::size_t at30 = static_cast<std::size_t>(0.030 * kFs);
        std::printf("ramp: the largest step %.6f, from sample %d to %d (%.2f ms), at 30 ms %.5f\n", static_cast<double>(most),
                    left, landed, ms, static_cast<double>(after[at30]));
        P.le("ramp.no_step", static_cast<double>(most), 0.001);     // 20 ms's linear glide: 0.6 / 960 = 0.000625
        P.near("ramp.length_ms", ms, 20.0, 1.0);
        const std::size_t half = static_cast<std::size_t>(std::max(left, 0)) + static_cast<std::size_t>(0.010 * kFs);
        P.near("ramp.halfway", left >= 0 && half < after.size() ? static_cast<double>(after[half]) : 0.0, 0.5, 0.02);
        P.near("ramp.there", static_cast<double>(after[at30]), 0.2, 1e-3);
    }

    ps::setDirectoryForTests({});
    dir.deleteRecursively();
    return P.finish();
}
