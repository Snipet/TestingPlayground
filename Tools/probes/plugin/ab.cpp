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
//            (0.4) kept for B after a turn on A; measured again: B 7.6 dB over A (2.4 times); a move on B (0.6) makes the
//            match out of date at once (measuring, in get_status too), and once the dials rest it is measured again
//            (13.6 dB), with nothing switched
//   KEEP     B kept: the draft is version 1's files (the draft's revision moved: an open editor merges it), the project
//            open is version 1, not edited; the A/B off, the output untrimmed (B's level 12.04 dB over the matched)
//   END      begun again (A Current, now version 1's files, playing; B version 2), B played, another project's begun
//            over it (its sides), then ended: version 1's files (what played before the first began) play again, the
//            draft untouched
//   DROP     begun again, and another program run (the folder's run, Claude's): the A/B ends on its own, ended saying
//            why, the other program playing, nothing restored
//   CLAUDE   begun again, B played, and Claude's run_program (the draft): it ends the same way, the draft's files playing
//   SWITCH   B playing, matched; the loudest sample over the matched level, in the host's blocks with no poll() between
//            (what the audio thread does alone), while A plays and B again (the trim turned up only once B has gone,
//            down before B comes in), while A and B again are asked for within one compile (B's old program
//            plays until the new one lands), while B is moved to version 2 (6 dB quieter than version 1, measured again
//            after) and while Close puts back what played before: never more than 0.5 dB, but for the synth's 20 ms when
//            its old voices fade under the new one's (each at most matched: 6.02 dB at most, as on any run); the old
//            code turned B up 12 dB in each
//   REFINE   B a version with a netlist, its configuration the self-test's: B plays provisionally, the self-test's
//            refine (a new program) plays before poll() sees it, still matched (the synth's after its note, which a
//            refine waits for); poll() then sees the refined status first, and B plays (not "on its way" for good)
//   SESSION  a session saved while B (each side its own dials) plays keeps what played before it began and those dials;
//            a session loaded (setStateInformation) ends the A/B, saying so, its program playing untrimmed and its
//            files the draft, and Close then does nothing; loaded again while B is on its way: B never plays over it
#include "Probe.h"

#include "plugin/EditorPort.h"
#include "plugin/ProjectStore.h"
#include "plugin/Processor.h"

#include "AutoClipper.h"
#include "Product.h"

#include "fminus/Circuits.h"

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

    // The program: a 1 kHz sine at level * K (gain.fm's), its input ignored; the synth's while a note is held. With
    // `circuit`, it also runs AutoClipper.h's netlist (whose configuration is the self-test's) on silence, adding
    // nothing to the sine.
    pg::Sources program(const std::string& k, bool circuit = false)
    {
        const std::string body = std::string(circuit ? "    Clipper clip;\n" : "")
                                 + "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                   "    float phase;\n    float step;\n"
                                   "    void init(float sampleRate) { step = 1000 / sampleRate;"
                                 + (circuit ? " clip.init(sampleRate);" : "") + " }\n";
        const std::string loop = "        for (int i = 0; i < n; ++i)\n        {\n"
                                 "            phase += step;\n            if (phase >= 1) phase -= 1;\n"
                                 "            const float y = gainK() * level[i] * sinf(6.28318531f * phase)"
                                 + std::string(circuit ? " + 0.0f * clip.tick(0.0f)" : "") + ";\n";
        const std::string head = "// AB: a sine\n#include \"gain.fm\"\n" + std::string(circuit ? "#include \"clipper.cir\"\n" : "");
        const std::string text = kInstrument
            ? head + "module Voice\n{\n" + body
                  + "    bool held;\n    void start(float note, float v) { held = true; phase = 0; }\n"
                    "    void stop() { held = false; }\n"
                    "    bool process(float* restrict left, float* restrict right, int n)\n    {\n" + loop
                  + "            left[i] += y;\n            right[i] += y;\n        }\n        return held;\n    }\n};\n"
            : head + "module Effect\n{\n" + body
                  + "    void process(float* restrict left, float* restrict right, int n)\n    {\n" + loop
                  + "            left[i] = y;\n            right[i] = y;\n        }\n    }\n};\n";
        pg::Sources out = { { pg::kProgramName, text }, { "gain.fm", "float gainK() { return " + k + "f; }\n" } };
        if (circuit)
            out.push_back({ "clipper.cir", probe::kAutoClipper });
        return out;
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

    // The host: blocks of kBlock, the synth's note held (sent once, `noteOn`; `release` lets it go at the next block, and
    // the next note-on comes after `noteSent` is cleared); the left channel's samples.
    struct Host
    {
        pg::Processor& p;
        juce::AudioBuffer<float> buffer{ 2, kBlock };
        bool noteSent = false;
        bool release = false;

        std::vector<float> play(int blocks)
        {
            std::vector<float> out;
            for (int b = 0; b < blocks; ++b)
            {
                buffer.clear();
                juce::MidiBuffer midi;
                if (kInstrument && release)
                {
                    midi.addEvent(juce::MidiMessage::noteOff(1, 72), 0);
                    release = false;
                }
                else if (kInstrument && !noteSent)
                {
                    midi.addEvent(juce::MidiMessage::noteOn(1, 72, 0.8f), 0);
                    noteSent = true;
                }
                p.processBlock(buffer, midi);
                out.insert(out.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + kBlock);
            }
            return out;
        }
        // Blocks, paced as a host would (a block each 2 ms), while the compiler has a request and `after` more, with no
        // poll() between (the audio thread alone): the loudest sample of the blocks where one program sounds, and of
        // those where an instrument's old voices fade under the new program's (a swap's 20 ms, and the block after).
        struct Loudest
        {
            float alone = 0.0f, overlap = 0.0f;
        };
        Loudest loudestWhileCompiling(int after)
        {
            Loudest out;
            bool fadedBefore = false;
            const auto keep = [&] {
                const std::vector<float> x = play(1);
                pg::Meters m;
                const bool fading = p.editorPort().meters(m) && m.fading != 0;
                float& peak = fading || fadedBefore ? out.overlap : out.alone;
                fadedBefore = fading;
                for (const float v : x)
                    peak = std::max(peak, std::fabs(v));
            };
            const auto from = std::chrono::steady_clock::now();
            while (p.compiler().busy() && std::chrono::steady_clock::now() - from < std::chrono::seconds(60))
            {
                keep();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            for (int b = 0; b < after; ++b)
                keep();
            return out;
        }
        float loudest(int blocks)
        {
            float peak = 0.0f;
            for (const float v : play(blocks))
                peak = std::max(peak, std::fabs(v));
            return peak;
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
    processor.compiler().setSelfTesting(false);          // REFINE's refine is the probe's (selfTestNow)
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
        // A move on B with nothing switched: out of date at once, measured again once the dials rest.
        const uint64_t ran = port.statusRevision();
        setDial(processor, 0, 0.6f);
        processor.poll();
        P.eq("own.move_stale", port.ab().measuring && contains(statusText(processor), "measuring the loudness") ? 1 : 0, 1);
        const bool followed = pump(processor, 60.0, [&] { return !port.ab().measuring; });
        st = port.ab();
        P.eq("own.followed", followed && st.playing == 1 && st.pending < 0 && port.statusRevision() == ran ? 1 : 0, 1);
        P.near("own.followed_db", st.lufs[1] - st.lufs[0], 20.0 * std::log10(4.8), 0.1);
        P.near("own.followed_trim_db", st.trimDb[1], -20.0 * std::log10(4.8), 0.1);
        setDial(processor, 0, 0.3f);                     // back, for KEEP
        processor.poll();
        (void)pump(processor, 60.0, [&] { return !port.ab().measuring; });
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

    // ---- CLAUDE --------------------------------------------------------------------------------------------------------
    {
        P.eq("claude.started", port.startAb(project, 0, 2, why) ? 1 : 0, 1);
        port.playAb(1);
        processor.settle();
        const pg::Sources draft = port.draft().files;
        const pg::ChatAgent::McpResult r = processor.chatAgent().mcpCall("run_program", juce::var(new juce::DynamicObject()));
        processor.settle();
        const pg::AbState s = port.ab();
        P.eq("claude.ended", !r.isError && !s.on && contains(s.ended, "Another program runs now") ? 1 : 0, 1);
        P.eq("claude.draft_plays", port.running() == draft ? 1 : 0, 1);
    }

    // The A/B as START has it: A Current (K 0.25), B version 1 (K 1, 12.04 dB over), the same dials, B playing matched.
    const auto beginB = [&](const std::string& name) {
        runDraft(current, 0.5f);
        bool ok = port.startAb(name, 0, 1, why);
        ok = ok && pump(processor, 60.0, [&] { return !port.ab().measuring; });
        port.playAb(1);
        processor.settle();
        ok = ok && port.ab().playing == 1;
        (void)host.play(8);
        return ok;
    };
    const auto overDb = [](float peak, float matched) { return 20.0 * std::log10(std::max(1.0e-9f, peak) / matched); };
    // Never over the matched level by more than 0.5 dB; while an instrument's old voices fade (each at most the matched
    // level) under the new program's (at most it too), their sum by at most 6.02 dB (as on any run).
    const auto judge = [&](const std::string& row, const Host::Loudest& l, float matched) {
        P.le(row + "_db", overDb(l.alone, matched), 0.5);
        if (kInstrument)
            P.le(row + "_overlap_db", overDb(l.overlap, matched), 6.2);
    };

    // ---- SWITCH --------------------------------------------------------------------------------------------------------
    {
        P.eq("switch.begun", beginB(project) ? 1 : 0, 1);
        const float matched = host.loudest(8);
        P.near("switch.matched_db", overDb(matched, 0.125f), 0.0, 0.2);
        // A (untrimmed) and B (12 dB down) in turn: the trim moves up once B is out, down before B comes in.
        port.playAb(0);
        judge("switch.to_a", host.loudestWhileCompiling(30), matched);
        processor.settle();
        port.playAb(1);
        judge("switch.to_b", host.loudestWhileCompiling(30), matched);
        processor.settle();
        port.playAb(0);                                  // A, then B again, within one compile
        port.playAb(1);
        judge("switch.flip", host.loudestWhileCompiling(30), matched);
        processor.settle();
        P.eq("switch.flip_lands", port.ab().playing == 1 && port.ab().pending < 0 && port.running() == v1 ? 1 : 0, 1);
        P.le("switch.flip_after_db", overDb(host.loudest(8), matched), 0.2);
        // B moved to version 2 while it plays: its old program, then the new one at B's trim until measured again.
        P.eq("switch.moved", port.setAbSides(0, 2, why) ? 1 : 0, 1);
        judge("switch.moved", host.loudestWhileCompiling(30), matched);
        const bool remeasured = pump(processor, 60.0, [&] { return !port.ab().measuring; });
        P.eq("switch.moved_lands", remeasured && port.ab().playing == 1 && port.running() == v2 ? 1 : 0, 1);
        P.near("switch.moved_matched_db", overDb(host.loudest(20), matched), 0.0, 0.2);
        // Close while B plays: B keeps its trim until what played before (A's files, untrimmed) lands.
        port.endAb();
        judge("switch.close", host.loudestWhileCompiling(30), matched);
        processor.settle();
        P.eq("switch.closed", !port.ab().on && port.running() == current ? 1 : 0, 1);
        P.near("switch.closed_level_db", overDb(host.loudest(20), matched), 0.0, 0.2);
    }

    // ---- REFINE --------------------------------------------------------------------------------------------------------
    {
        pg::circuits::answers().clearForTests();         // the netlist's configuration unknown: B compiles provisionally
        const pg::Sources withCircuit = program("1.0", true);
        runDraft(withCircuit, 0.5f);
        P.eq("refine.setup", port.saveVersion("AB Circuit", saved, why) && saved.number == 1 ? 1 : 0, 1);
        P.eq("refine.begun", beginB("AB Circuit") && port.running() == withCircuit ? 1 : 0, 1);
        const float matched = host.loudest(8);
        P.near("refine.matched_db", overDb(matched, 0.125f), 0.0, 0.2);
        // B's compile and its refine before poll() sees either: B played again (the newest compile's status unseen).
        port.playAb(0);
        processor.settle();
        port.playAb(1);
        processor.compiler().wait();
        (void)host.play(8);                              // B (provisional) plays
        if (kInstrument)
        {
            host.release = true;                         // a refine waits for an instrument's silence
            (void)host.play(8);
        }
        const bool refined = processor.compiler().selfTestNow();
        if (kInstrument)
            host.noteSent = false;
        P.eq("refine.refined", refined ? 1 : 0, 1);
        P.le("refine.before_poll_db", overDb(host.loudest(20), matched), 0.2);
        P.near("refine.before_poll_level_db", overDb(host.loudest(20), matched), 0.0, 0.2);
        processor.poll();
        const pg::AbState s = port.ab();
        P.eq("refine.lands", s.playing == 1 && s.pending < 0 && port.status().refined ? 1 : 0, 1);
        P.near("refine.after_poll_level_db", overDb(host.loudest(20), matched), 0.0, 0.2);
        port.endAb();
        processor.settle();
    }

    // ---- SESSION -------------------------------------------------------------------------------------------------------
    {
        runDraft(elsewhere, 0.5f);
        juce::MemoryBlock other;
        processor.getStateInformation(other);           // a session of its own: K 0.75, 9.54 dB over Current
        runDraft(current, 0.5f);
        juce::MemoryBlock plain;
        processor.getStateInformation(plain);           // Current's
        P.eq("session.begun", beginB(project) ? 1 : 0, 1);
        const float matched = host.loudest(8);
        port.setAbOwnDials(true);
        processor.settle();                              // B's own dials (version 1's level 0.3)
        P.near("session.b_dial", dial(processor, 0), 0.3, 1.0e-4);
        {
            juce::MemoryBlock saving;
            processor.getStateInformation(saving);
            pg::Processor reopened;
            reopened.setStateInformation(saving.getData(), static_cast<int>(saving.getSize()));
            reopened.poll();
            P.eq("session.saved_before", reopened.editorPort().running() == current && reopened.editorPort().draft().files == current ? 1 : 0, 1);
            P.near("session.saved_before_dial", dial(reopened, 0), 0.5, 1.0e-4);
        }
        processor.setStateInformation(other.getData(), static_cast<int>(other.getSize()));
        processor.poll();
        pg::AbState s = port.ab();
        P.eq("session.ended", !s.on && contains(s.ended, "saved state was loaded") ? 1 : 0, 1);
        P.eq("session.plays", port.running() == elsewhere && port.draft().files == elsewhere ? 1 : 0, 1);
        P.near("session.untrimmed_db", overDb(host.loudest(20), matched), 20.0 * std::log10(3.0), 0.2);
        port.endAb();                                    // nothing: it ended
        processor.settle();
        P.eq("session.close_nothing", port.running() == elsewhere && !port.compiling() ? 1 : 0, 1);
        // Loaded while B is on its way: B never plays over the session's program.
        P.eq("session.begun_again", port.startAb(project, 0, 1, why) && pump(processor, 60.0, [&] { return !port.ab().measuring; }) ? 1 : 0, 1);
        port.playAb(1);
        processor.setStateInformation(plain.getData(), static_cast<int>(plain.getSize()));
        processor.settle();
        (void)host.play(30);
        processor.settle();
        s = port.ab();
        P.eq("session.pending_ended", !s.on && s.pending < 0 && contains(s.ended, "saved state was loaded") ? 1 : 0, 1);
        P.eq("session.pending_plays", port.running() == current ? 1 : 0, 1);
        P.near("session.pending_level_db", overDb(host.loudest(20), matched), 0.0, 0.2);
    }

    processor.releaseResources();
    pg::projectstore::setDirectoryForTests({});
    temp.deleteRecursively();
    return P.finish();
}
