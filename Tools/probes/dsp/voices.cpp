// PROBE name=dsp.voices timeout=120
//
// dsp.voices: which voice plays which note (the legacy policy, VoiceScheduler.h), read from the Engine's voices
// (voice(v)) after rendering up to a sample, at 48 kHz in blocks of 256.
//   SIXTEEN   sixteen notes, a major third apart from C2 (36, 40, ..., 96; velocity 0.25), started 10 samples apart:
//             voice v plays the v-th note (free voices are taken lowest first), all sixteen held
//   STEAL     a seventeenth note (D7, 98) steals the voice of the oldest note-on (voice 0, C2): that voice fades out
//             over rampSamples() (2 ms, 96 samples) and starts D7 on the next sample, no other voice changes; later,
//             D7 sounds at its level (-24.08 dBFS, Hann-windowed DFT, within 0.05 dB) and C2 is gone (under -90 dBFS)
//   AGAIN     a held note played again (E2, voice 1) is retriggered on its own voice (still sixteen sounding) and
//             counts as the newest note: the next steal takes voice 2 (G#2), not voice 1
//   OFF       a note-off releases exactly the voice playing that note: after C4's (voice 6) only voice 6 is let go, and
//             50 ms later (RELEASE) it is free and C4 is gone from the output, the other notes still sound; a note-off
//             for the stolen C2 changes nothing (voice 0 plays D7 and stays held)
//   FREE      the next note takes the voice C4 left (the only free one) and steals nothing
//   ALL OFF   after every note-off and the release, no voice is left sounding
//   CLICKS    reusing a voice never steps: a note stealing (velocity 0.05) from sixteen held at full velocity, and a
//             key retriggered 10 ms into its release at a tenth of its velocity, step from sample to sample no more
//             than the steady sound before them did (the legacy synth jumped there to the new pitch and velocity)
//   PANIC     All Notes Off (EventKind::allNotesOff) 10 samples into a steal's fade: every key is let go at once, the
//             stolen voice still fades out and then starts its new note released (silent); 0.2 s later no voice is
//             left and the output is exactly 0
#include "Probe.h"
#include "Rig.h"

#include <cmath>
#include <string>
#include <vector>

namespace
{
    namespace tps = probe::tps;

    constexpr double kFs = 48000.0;
    constexpr int kBlock = 256;
    constexpr int kN = tps::Engine::kVoices;

    std::size_t at(double seconds) { return static_cast<std::size_t>(seconds * kFs); }

    int noteOf(int k) { return 36 + 4 * k; }

    // The engine after `n` samples of `score`.
    probe::Stereo runTo(const std::vector<probe::ScoreEvent>& score, std::size_t n, tps::Engine& engine)
    {
        return probe::runSynth(score, n, kFs, kBlock, tps::defaultValues(), &engine);
    }

    int held(const tps::Engine& e)
    {
        int h = 0;
        for (int v = 0; v < kN; ++v)
            h += e.voice(v).held ? 1 : 0;
        return h;
    }

    double toneDb(const probe::Stereo& s, int note, double fromSec, double lenSec)
    {
        return probe::dbFromLin(probe::toneAmplitudeHann(s.l, probe::noteHz(note), kFs, at(fromSec), at(lenSec)));
    }
}

PROBE(dsp, voices)
{
    tps::Engine engine;
    std::vector<probe::ScoreEvent> score;
    for (int k = 0; k < kN; ++k)
        score.push_back(probe::on(static_cast<std::size_t>(10 * k), noteOf(k), 0.25f));

    // SIXTEEN
    {
        runTo(score, at(0.1), engine);
        int mismatched = 0;
        for (int v = 0; v < kN; ++v)
            mismatched += engine.voice(v).note == noteOf(v) && engine.voice(v).held ? 0 : 1;
        P.eq("sixteen.voice_v_plays_note_v.mismatches", mismatched, 0);
        P.eq("sixteen.sounding", engine.activeVoices(), kN);
    }

    // STEAL: D7 at 0.2 s.
    const std::size_t steal = at(0.2);
    score.push_back(probe::on(steal, 98, 0.25f));
    {
        runTo(score, steal + 1, engine);
        int changed = 0;
        for (int v = 1; v < kN; ++v)
            changed += engine.voice(v).note == noteOf(v) && engine.voice(v).next < 0 ? 0 : 1;
        P.eq("steal.voice0.next_note", engine.voice(0).next, 98);
        P.eq("steal.voice0.still_sounds_old_note", engine.voice(0).note, noteOf(0));
        P.eq("steal.other_voices_changed", changed, 0);
        P.eq("steal.sounding", engine.activeVoices(), kN);
        const int ramp = engine.rampSamples();
        P.eq("steal.ramp_samples", ramp, 96);
        runTo(score, steal + static_cast<std::size_t>(ramp) - 1, engine);
        P.eq("steal.fading_until_ramp_end.next_note", engine.voice(0).next, 98);
        runTo(score, steal + static_cast<std::size_t>(ramp), engine);
        P.eq("steal.starts_after_ramp.note", engine.voice(0).note, 98);
        P.eq("steal.starts_after_ramp.held", engine.voice(0).held ? 1 : 0, 1);
    }

    // AGAIN: E2 (voice 1) played again at 0.25 s; then C#7 (97) at 0.3 s steals voice 2, not voice 1.
    score.push_back(probe::on(at(0.25), noteOf(1), 0.25f));
    score.push_back(probe::on(at(0.3), 97, 0.25f));
    {
        runTo(score, at(0.25) + 1, engine);
        P.eq("again.same_voice.note", engine.voice(1).note, noteOf(1));
        P.eq("again.sounding", engine.activeVoices(), kN);
        runTo(score, at(0.3) + 1, engine);
        P.eq("again.counts_as_newest.voice1_kept", engine.voice(1).next, -1);
        P.eq("again.counts_as_newest.voice2_stolen", engine.voice(2).next, 97);
    }

    // OFF: C4 (noteOf(6) = 60, voice 6) at 0.4 s; the stolen C2 at 0.45 s.
    score.push_back(probe::off(at(0.4), noteOf(6)));
    score.push_back(probe::off(at(0.45), noteOf(0)));
    {
        runTo(score, at(0.4) + 1, engine);
        P.eq("off.voice6.held", engine.voice(6).held ? 1 : 0, 0);
        P.eq("off.others.held", held(engine), kN - 1);
        runTo(score, at(0.4) + at(0.05) + 2, engine);
        P.eq("off.voice6.free_after_release", engine.voice(6).sounding ? 1 : 0, 0);
        P.eq("off.sounding_after_release", engine.activeVoices(), kN - 1);
        runTo(score, at(0.45) + 1, engine);
        P.eq("off.stolen_note.voice0_unchanged", engine.voice(0).note == 98 && engine.voice(0).held ? 1 : 0, 1);
    }

    // FREE: D#7 (99) at 0.48 s takes voice 6, the one free voice, and steals nothing.
    score.push_back(probe::on(at(0.48), 99, 0.25f));
    {
        runTo(score, at(0.48) + 1, engine);
        int stealing = 0;
        for (int v = 0; v < kN; ++v)
            stealing += engine.voice(v).next >= 0 ? 1 : 0;
        P.eq("free.voice6.note", engine.voice(6).note, 99);
        P.eq("free.no_steal", stealing, 0);
        P.eq("free.sounding", engine.activeVoices(), kN);
    }

    // The output: D7 in, C2 out, C4 out after its release, the others still in.
    {
        const probe::Stereo out = runTo(score, at(1.6), engine);
        P.near("out.d7.dbfs", toneDb(out, 98, 0.5, 1.0), probe::dbFromLin(0.0625), 0.05);
        P.le("out.c2_stolen.dbfs", toneDb(out, noteOf(0), 0.5, 1.0), -90.0);
        P.le("out.c4_released.dbfs", toneDb(out, noteOf(6), 0.5, 1.0), -90.0);
        P.near("out.e2_retriggered.dbfs", toneDb(out, noteOf(1), 0.5, 1.0), probe::dbFromLin(0.0625), 0.05);
        P.near("out.c7_still_held.dbfs", toneDb(out, noteOf(15), 0.5, 1.0), probe::dbFromLin(0.0625), 0.05);
        P.near("out.ds7_on_freed_voice.dbfs", toneDb(out, 99, 0.5, 1.0), probe::dbFromLin(0.0625), 0.05);
    }

    // ALL OFF
    {
        for (int k = 0; k < kN; ++k)
            score.push_back(probe::off(at(1.0), noteOf(k)));
        score.push_back(probe::off(at(1.0), 98));
        score.push_back(probe::off(at(1.0), 97));
        score.push_back(probe::off(at(1.0), 99));
        runTo(score, at(1.2), engine);
        P.eq("all_off.sounding_after_release", engine.activeVoices(), 0);
    }

    // CLICKS: the largest sample step over 10 ms from the steal or the retrigger, against the steady sound's before.
    {
        std::vector<probe::ScoreEvent> s;
        for (int k = 0; k < kN; ++k)
            s.push_back(probe::on(static_cast<std::size_t>(k), noteOf(k), 1.0f));
        const std::size_t at2 = at(0.5) + 3;
        s.push_back(probe::on(at2, 90, 0.05f));
        const probe::Stereo out = probe::runSynth(s, at(0.7), kFs, kBlock, tps::defaultValues());
        const double steady = probe::maxStep(out.l, at(0.2), at2 - at(0.2));
        P.le("clicks.steal.max_step_rel_steady", probe::maxStep(out.l, at2, at(0.010)) / steady, 1.0);
    }
    {
        const std::size_t release = at(0.5), again = release + at(0.010);
        const probe::Stereo out = probe::runSynth({ probe::on(0, 69, 1.0f), probe::off(release, 69),
                                                    probe::on(again, 69, 0.1f) }, at(0.7), kFs, kBlock,
                                                  tps::defaultValues());
        const double steady = probe::maxStep(out.l, at(0.2), release - at(0.2));
        P.le("clicks.retrigger.max_step_rel_steady", probe::maxStep(out.l, again, at(0.010)) / steady, 1.0);
    }

    // PANIC: sixteen held, D7 steals voice 0 at 0.1 s, All Notes Off 10 samples later.
    {
        std::vector<probe::ScoreEvent> s;
        for (int k = 0; k < kN; ++k)
            s.push_back(probe::on(static_cast<std::size_t>(k), noteOf(k), 0.25f));
        const std::size_t panicSteal = at(0.1);
        s.push_back(probe::on(panicSteal, 98, 0.25f));
        s.push_back({ panicSteal + 10, tps::EventKind::allNotesOff, 0, 0.0f });
        tps::Engine e;
        runTo(s, panicSteal + 11, e);
        P.eq("panic.during_steal.held", held(e), 0);
        P.eq("panic.during_steal.voice0_still_fading", e.voice(0).next, 98);
        const probe::Stereo out = runTo(s, at(0.5), e);
        P.eq("panic.voice0_starts_released.note", e.voice(0).note, 98);
        P.eq("panic.voices_left", e.activeVoices(), 0);
        P.le("panic.peak_after", static_cast<double>(probe::peak(probe::slice(out.l, at(0.3), at(0.2)))), 0.0);
    }
    return P.finish();
}
