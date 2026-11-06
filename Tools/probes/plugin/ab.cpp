// PROBE name=plugin.ab timeout=240
//
// plugin.ab: A/B listening (EditorPort::ab; plugin/ProcessorAb.cpp, AbMatch.h) through the processor, in a projects
// folder of the probe's own. A project of two versions of a program of two files, a sine at 1 kHz whose level is the
// dial `level` times gain.fm's K (version 1: K 1, level 0.3; version 2: K 0.5, level 0.6), and the editor's files, K
// 0.25 at level 0.5, playing (the synth's a note held):
//   START    begun with A Current and B version 1: on, its sides named, A playing at once with nothing compiled (it is
//            what plays), the draft untouched; a version the project does not keep refused, naming it
//   MATCH    measured offline: both loudnesses, B 12.04 dB louder (four times A's K at the same dials), B's trim that
//            much down, A's none
//   PLAY     B played: on its way, then playing: the program that plays is version 1's files, the editor's draft still
//            A's and the project open as it was; Claude's get_status says the A/B, its sides and that B plays
//   LEVEL    the output (a host's blocks): B matched within 0.2 dB of A, B unmatched 12.04 dB over it; the trim's change
//            a 20 ms ramp, no sample step larger than the sine's own, the level falling period by period
//   OWN      each its own dials: A's as they were (level 0.5) at once; B's version 1's (0.3) when it lands; a move on B
//            (0.4) kept for B after a turn on A; measured again: B 7.6 dB over A (2.4 times)
//   KEEP     B kept: the draft is version 1's files (the draft's revision moved: an open editor merges it), the project
//            open is version 1, not edited; the A/B off, the output untrimmed (B's level 12.04 dB over the matched)
//   END      begun again (A Current, now version 1's files, playing; B version 2), B played, another project's begun
//            over it (its sides), then ended: version 1's files (what played before the first began) play again, the
//            draft untouched
//   DROP     begun again, and another program run (the folder's run, Claude's): the A/B ends on its own, ended saying
//            why, the other program playing, nothing restored
#include "Probe.h"

#include "plugin/EditorPort.h"
#include "plugin/ProjectStore.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <juce_audio_processors/juce_audio_processors.h>

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
    constexpr double kRate = 48000.0;
    constexpr int kBlock = 512;

    // The program: a 1 kHz sine at level * K (gain.fm's), its input ignored; the synth's while a note is held.
    pg::Sources program(const std::string& k)
    {
        const std::string body = "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                 "    float phase;\n    float step;\n"
                                 "    void init(float sampleRate) { step = 1000 / sampleRate; }\n";
        const std::string loop = "        for (int i = 0; i < n; ++i)\n        {\n"
                                 "            phase += step;\n            if (phase >= 1) phase -= 1;\n"
                                 "            const float y = gainK() * level[i] * sinf(6.28318531f * phase);\n";
        const std::string text = kInstrument
            ? "// AB: a sine\n#include \"gain.fm\"\nmodule Voice\n{\n" + body
                  + "    bool held;\n    void start(float note, float v) { held = true; phase = 0; }\n"
                    "    void stop() { held = false; }\n"
                    "    bool process(float* restrict left, float* restrict right, int n)\n    {\n" + loop
                  + "            left[i] += y;\n            right[i] += y;\n        }\n        return held;\n    }\n};\n"
            : "// AB: a sine\n#include \"gain.fm\"\nmodule Effect\n{\n" + body
                  + "    void process(float* restrict left, float* restrict right, int n)\n    {\n" + loop
                  + "            left[i] = y;\n            right[i] = y;\n        }\n    }\n};\n";
        return { { pg::kProgramName, text }, { "gain.fm", "float gainK() { return " + k + "f; }\n" } };
    }

    float dial(pg::Processor& p, int slot) { return pg::plainOf(p.control(slot), p.getParameters()[slot]->getValue()); }
    void setDial(pg::Processor& p, int slot, float plain)
    {
        p.getParameters()[slot]->setValueNotifyingHost(pg::normOf(p.control(slot), plain));
    }

    // The processor's timer, until `done` or `seconds`.
    template <typename Done>
    bool pump(pg::Processor& p, double seconds, Done&& done)
    {
        const auto from = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count() < seconds)
        {
            p.poll();
            if (done())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return done();
    }

    // The host: blocks of kBlock, the synth's note held (sent once, `noteOn`); the left channel's samples.
    struct Host
    {
        pg::Processor& p;
        juce::AudioBuffer<float> buffer{ 2, kBlock };
        bool noteSent = false;

        std::vector<float> play(int blocks)
        {
            std::vector<float> out;
            for (int b = 0; b < blocks; ++b)
            {
                buffer.clear();
                juce::MidiBuffer midi;
                if (kInstrument && !noteSent)
                {
                    midi.addEvent(juce::MidiMessage::noteOn(1, 72, 0.8f), 0);
                    noteSent = true;
                }
                p.processBlock(buffer, midi);
                out.insert(out.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + kBlock);
            }
            return out;
        }
        // The level in dBFS (RMS) after a program's swap and the trim's ramp have settled.
        double level()
        {
            (void)play(8);
            const std::vector<float> x = play(20);
            double e = 0.0;
            for (const float v : x)
                e += static_cast<double>(v) * v;
            return 10.0 * std::log10(std::max(1.0e-30, e / static_cast<double>(x.size())));
        }
    };

    bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    std::string statusText(pg::Processor& p)
    {
        const pg::ChatAgent::McpResult r = p.chatAgent().mcpCall("get_status", juce::var(new juce::DynamicObject()));
        std::string out;
        if (const juce::Array<juce::var>* blocks = r.content.getArray())
            for (const juce::var& b : *blocks)
                if (b.getProperty("type", "").toString() == "text")
                    out += b.getProperty("text", "").toString().toStdString() + "\n";
        return out;
    }
}

PROBE(plugin, ab)
{
    const funkgui::HeadlessGuiScope gui;
    const juce::File temp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getChildFile(juce::String(pg::product::kEnvPrefix) + "plugin-ab-" + juce::String(juce::Time::currentTimeMillis()));
    temp.deleteRecursively();
    pg::projectstore::setDirectoryForTests(temp);

    pg::Processor processor;
    processor.compiler().setFreezing(false);             // the program's own code throughout (no frozen copy)
    pg::EditorPort& port = processor.editorPort();
    processor.prepareToPlay(kRate, kBlock);
    Host host{ processor };
    const auto runDraft = [&](const pg::Sources& files, float level) {
        pg::Draft d;
        d.files = files;
        port.setDraft(d);
        port.run(files);
        processor.settle();
        setDial(processor, 0, level);
        processor.poll();
    };
    const std::string project = "AB Test";
    pg::SavedVersion saved;
    std::string why;
    const pg::Sources v1 = program("1.0"), v2 = program("0.5"), current = program("0.25"), elsewhere = program("0.75");
    runDraft(elsewhere, 0.5f);
    P.eq("setup.other", port.saveVersion("AB Other", saved, why) && saved.number == 1 ? 1 : 0, 1);
    runDraft(v1, 0.3f);
    P.eq("setup.v1", port.saveVersion(project, saved, why) && saved.number == 1 ? 1 : 0, 1);
    runDraft(v2, 0.6f);
    P.eq("setup.v2", port.saveVersion(project, saved, why) && saved.number == 2 ? 1 : 0, 1);
    runDraft(current, 0.5f);
    (void)host.play(4);

    // ---- START ---------------------------------------------------------------------------------------------------------
    P.eq("start.refused_missing", !port.startAb(project, 0, 99, why) && contains(why, "keeps no version 99") && !port.ab().on ? 1 : 0, 1);
    const uint64_t compiles = port.statusRevision();
    P.eq("start.ok", port.startAb(project, 0, 1, why) ? 1 : 0, 1);
    pg::AbState st = port.ab();
    P.eq("start.on", st.on && st.project == project ? 1 : 0, 1);
    P.eq("start.sides", st.sides[0].label == "Current" && st.sides[0].version == 0 && st.sides[1].label == "v1"
                            && st.sides[1].version == 1 && st.sides[1].files == v1 ? 1 : 0, 1);
    P.eq("start.a_plays_uncompiled", st.playing == 0 && st.pending < 0 && port.statusRevision() == compiles && !port.compiling() ? 1 : 0, 1);
    P.eq("start.draft_untouched", port.draft().files == current ? 1 : 0, 1);

    // ---- MATCH ---------------------------------------------------------------------------------------------------------
    const bool measured = pump(processor, 60.0, [&] { return !port.ab().measuring; });
    st = port.ab();
    P.eq("match.measured", measured && std::isfinite(st.lufs[0]) && std::isfinite(st.lufs[1]) && st.unmatched.empty() ? 1 : 0, 1);
    P.near("match.b_louder_db", st.lufs[1] - st.lufs[0], 20.0 * std::log10(4.0), 0.1);
    P.near("match.b_trim_db", st.trimDb[1], -20.0 * std::log10(4.0), 0.1);
    P.near("match.a_trim_db", st.trimDb[0], 0.0, 1.0e-6);
    const double levelA = host.level();

    // ---- PLAY ----------------------------------------------------------------------------------------------------------
    const pg::ProjectState before = port.project();
    port.playAb(1);
    P.eq("play.pending", port.ab().pending == 1 ? 1 : 0, 1);
    processor.settle();
    st = port.ab();
    P.eq("play.lands", st.playing == 1 && st.pending < 0 && st.why.empty() ? 1 : 0, 1);
    P.eq("play.running_b", port.running() == v1 ? 1 : 0, 1);
    P.eq("play.draft_untouched", port.draft().files == current ? 1 : 0, 1);
    {
        const pg::ProjectState now = port.project();
        P.eq("play.project_unchanged", now.name == before.name && now.version == before.version && now.edited == before.edited ? 1 : 0, 1);
    }
    {
        const std::string s = statusText(processor);
        P.eq("play.get_status", contains(s, "A/B listening (the user's") && contains(s, "B is version 1 of \"AB Test\"") && contains(s, "B plays")
                                    && contains(s, "a side of the user's A/B listening") ? 1 : 0, 1);
    }

    // ---- LEVEL ---------------------------------------------------------------------------------------------------------
    const double levelB = host.level();
    P.near("level.b_matched_to_a_db", levelB - levelA, 0.0, 0.2);
    {
        // The trim off: B at its own level; on again: a 20 ms ramp down, the sine's period peaks falling, no step.
        port.setAbMatch(false);
        const double loud = host.level();
        P.near("level.b_unmatched_db", loud - levelA, 20.0 * std::log10(4.0), 0.2);
        port.setAbMatch(true);
        const std::vector<float> x = host.play(4);
        std::vector<float> peaks;
        for (std::size_t at = 0; at + 48 <= x.size(); at += 48)
        {
            float m = 0.0f;
            for (std::size_t i = at; i < at + 48; ++i)
                m = std::max(m, std::fabs(x[i]));
            peaks.push_back(m);
        }
        bool falling = true;
        for (std::size_t i = 1; i < peaks.size(); ++i)
            falling = falling && peaks[i] <= peaks[i - 1] + 1.0e-4f;
        P.eq("level.ramp_falls", falling ? 1 : 0, 1);
        P.near("level.ramp_ends_db", 20.0 * std::log10(peaks.back() / peaks.front()), -20.0 * std::log10(4.0), 0.2);
        // half way through 20 ms (period 10) the gain is half way in amplitude (smootherstep's middle), not at an end
        P.in("level.ramp_middle", static_cast<double>(peaks[10] / peaks.front()), 0.45, 0.80);
        float steady = 0.0f, ramp = 0.0f;
        for (std::size_t i = 1; i < x.size(); ++i)
            (i < 48 ? steady : ramp) = std::max(i < 48 ? steady : ramp, std::fabs(x[i] - x[i - 1]));
        P.le("level.no_step", ramp, steady * 1.01f);
    }

    // ---- OWN -----------------------------------------------------------------------------------------------------------
    {
        port.playAb(0);
        processor.settle();
        P.eq("own.a_plays", port.ab().playing == 0 ? 1 : 0, 1);
        setDial(processor, 0, 0.8f);                     // the same dials: a move on A
        processor.poll();
        port.setAbOwnDials(true);
        processor.poll();
        P.near("own.a_as_it_was", dial(processor, 0), 0.5, 1.0e-4);
        port.playAb(1);
        processor.settle();
        P.near("own.b_saved", dial(processor, 0), 0.3, 1.0e-4);
        setDial(processor, 0, 0.4f);
        processor.poll();
        port.playAb(0);
        processor.settle();
        P.near("own.back_on_a", dial(processor, 0), 0.5, 1.0e-4);
        port.playAb(1);
        processor.settle();
        P.near("own.b_kept", dial(processor, 0), 0.4, 1.0e-4);
        setDial(processor, 0, 0.3f);                     // B's saved value, for the measure below
        processor.poll();
        port.playAb(0);
        processor.settle();
        port.playAb(1);
        processor.settle();
        const bool again = pump(processor, 60.0, [&] { return !port.ab().measuring; });
        st = port.ab();
        P.eq("own.measured", again && st.ownDials ? 1 : 0, 1);
        P.near("own.b_louder_db", st.lufs[1] - st.lufs[0], 20.0 * std::log10(2.4), 0.1);
    }

    // ---- KEEP ----------------------------------------------------------------------------------------------------------
    {
        const uint64_t revision = port.draftRevision();
        P.eq("keep.ok", port.keepAb(1, why) ? 1 : 0, 1);
        processor.settle();
        P.eq("keep.draft_b", port.draft().files == v1 && port.draftRevision() != revision ? 1 : 0, 1);
        const pg::ProjectState now = port.project();
        P.eq("keep.project", now.name == project && now.version == 1 && !now.edited ? 1 : 0, 1);
        P.eq("keep.off", !port.ab().on && port.ab().ended.empty() ? 1 : 0, 1);
        P.near("keep.b_dial", dial(processor, 0), 0.3, 1.0e-4);
        P.near("keep.untrimmed_db", host.level() - levelA, 20.0 * std::log10(4.0 * 0.3 / 0.5), 0.2);
        P.eq("keep.refused_after", !port.keepAb(0, why) ? 1 : 0, 1);
        P.eq("keep.status_none", contains(statusText(processor), "A/B listening (the user's") ? 0 : 1, 1);
    }

    // ---- END -----------------------------------------------------------------------------------------------------------
    {
        P.eq("end.started", port.startAb(project, 0, 2, why) && port.ab().playing == 0 ? 1 : 0, 1);
        port.playAb(1);
        processor.settle();
        P.eq("end.b_plays", port.running() == v2 ? 1 : 0, 1);
        // Another project's A/B while this one is on: its sides anew, what played before this one began kept.
        P.eq("end.other_project", port.startAb("AB Other", 0, 1, why) && port.ab().project == "AB Other"
                                      && port.ab().sides[1].files == elsewhere ? 1 : 0, 1);
        processor.settle();
        port.endAb();
        processor.settle();
        P.eq("end.before_plays", port.running() == v1 && !port.ab().on ? 1 : 0, 1);
        P.eq("end.draft_untouched", port.draft().files == v1 ? 1 : 0, 1);
    }

    // ---- DROP ----------------------------------------------------------------------------------------------------------
    {
        P.eq("drop.started", port.startAb(project, 0, 2, why) ? 1 : 0, 1);
        port.playAb(1);
        processor.settle();
        const pg::Sources other = program("0.75");
        port.run(other);
        processor.settle();
        const pg::AbState s = port.ab();
        P.eq("drop.ended", !s.on && contains(s.ended, "Another program runs now") ? 1 : 0, 1);
        P.eq("drop.other_plays", port.running() == other ? 1 : 0, 1);
    }

    processor.releaseResources();
    pg::projectstore::setDirectoryForTests({});
    temp.deleteRecursively();
    return P.finish();
}
