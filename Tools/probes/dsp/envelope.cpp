// PROBE name=dsp.envelope timeout=120
//
// dsp.envelope: the ADSR's shape, its timing to the sample, and parameter changes that never step. 48 kHz, blocks of
// 256; ATTACK 100 ms (4,800 samples), DECAY 200 ms, SUSTAIN 50 %, RELEASE 300 ms (14,400 samples); A4 at full
// velocity from sample 1,000 (offset 232 of the fourth block) to sample 49,000. The envelope is read from the Engine
// (voice(0).envelope) after rendering k samples from the note-on, so a segment's value is known to the sample.
//   ONSET    the output is exactly 0 up to the note-on sample and in it (the sine starts at phase 0), and not 0 on the
//            next sample: the note starts at its offset within the block, not at the block's start
//   ATTACK   linear: 0.5 after 2,400 samples, 1 after 4,800 (within 1e-4; one sample is 2.1e-4)
//   DECAY    linear: 0.75 halfway (within 1e-4), exactly SUSTAIN (0.5) from its end on
//   LEVEL    the sustain's tone at 0.25 x 0.5 (-18.06 dBFS), single-bin DFT, within 0.02 dB
//   RELEASE  starts on the note-off sample: 100 samples after it, 0.5 x (1 - 100 / 14,400) within 1e-5 (one sample
//            is 3.5e-5); 0.25 halfway; exactly 0 at its end, the voice free, the output exactly 0 from there on
//   AGAIN    the key pressed again halfway through the release rises from where it is (0.25), on the same voice
//   SMOOTH   while A4 is held at SUSTAIN 100 %, parameter changes at 0.5 s (the block from sample 24,064) never step:
//            GAIN 0 -> +12 dB, SUSTAIN 100 -> 20 % and 20 -> 100 %, RELEASE 300 -> 10 ms during a release. The largest
//            sample-to-sample step in the 100 ms around the change is at most that of the steady tone at the louder of
//            the two settings (a level that jumps steps up to 13 times as far); GAIN is between its old and new values
//            5 ms after the change (smoothed: neither stepped nor stuck) and settles on +12 dB within 0.02 dB
#include "Probe.h"
#include "Rig.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
    namespace tps = probe::tps;

    constexpr double kFs = 48000.0;
    constexpr int kBlock = 256;
    constexpr std::size_t kOn = 1000, kOff = kOn + 48000;
    constexpr std::size_t kAttack = 4800, kDecay = 9600, kRelease = 14400;

    std::size_t at(double seconds) { return static_cast<std::size_t>(seconds * kFs); }

    tps::ParamValues adsr()
    {
        tps::ParamValues v = tps::defaultValues();
        v[tps::param::attack] = 100.0f;
        v[tps::param::decay] = 200.0f;
        v[tps::param::sustain] = 50.0f;
        v[tps::param::release] = 300.0f;
        return v;
    }

    // The score, rendered up to `n` samples; the engine as it is there.
    probe::Stereo upTo(std::size_t n, const std::vector<probe::ScoreEvent>& score, tps::Engine& engine)
    {
        return probe::runSynth(score, n, kFs, kBlock, adsr(), &engine);
    }

    double maxStepIn(const std::vector<float>& x, double fromSec, double lenSec)
    {
        return probe::maxStep(x, at(fromSec), at(lenSec));
    }

    // The first block boundary from 0.5 s on (256 x 94): where a parameter change reaches the Engine.
    constexpr std::size_t kChange = 94 * kBlock;

    // One second of A4 held from 0 at the values `before`, then `after` from kChange on; with `releaseAt`, the note-off
    // there.
    probe::Stereo change(const tps::ParamValues& before, const tps::ParamValues& after, std::size_t releaseAt = 0)
    {
        std::vector<probe::ScoreEvent> score { probe::on(0, 69, 1.0f) };
        if (releaseAt > 0)
            score.push_back(probe::off(releaseAt, 69));
        return probe::runSynth(score, at(1.0), kFs, kBlock, before, nullptr, { { kChange, after } });
    }
}

PROBE(dsp, envelope)
{
    const std::vector<probe::ScoreEvent> score { probe::on(kOn, 69, 1.0f), probe::off(kOff, 69) };
    tps::Engine engine;
    const auto envAfter = [&](std::size_t k) {
        upTo(kOn + k, score, engine);
        return static_cast<double>(engine.voice(0).envelope);
    };

    // ONSET
    {
        const probe::Stereo out = upTo(kOn + 100, score, engine);
        std::size_t first = 0;
        while (first < out.size() && out.l[first] == 0.0f)
            ++first;
        P.eq("onset.first_nonzero_sample", static_cast<int64_t>(first), static_cast<int64_t>(kOn + 1));
    }

    // ATTACK, DECAY
    P.near("attack.half.envelope", envAfter(kAttack / 2), 0.5, 1e-4);
    P.near("attack.end.envelope", envAfter(kAttack), 1.0, 1e-4);
    P.near("decay.half.envelope", envAfter(kAttack + kDecay / 2), 0.75, 1e-4);
    P.near("decay.end.envelope", envAfter(kAttack + kDecay + 2), 0.5, 0.0);
    P.near("sustain.at_0_9s.envelope", envAfter(at(0.9) - kOn), 0.5, 0.0);

    // LEVEL
    {
        const probe::Stereo out = upTo(kOff, score, engine);
        P.near("sustain.level_dbfs", probe::dbFromLin(probe::toneAmplitude(out.l, 440.0, kFs, at(0.5), at(0.5))),
               probe::dbFromLin(0.125), 0.02);
    }

    // RELEASE
    const std::size_t held = kOff - kOn;
    P.near("release.after_100_samples.envelope", envAfter(held + 100), 0.5 * (1.0 - 100.0 / kRelease), 1e-5);
    P.near("release.half.envelope", envAfter(held + kRelease / 2), 0.25, 1e-4);
    {
        const std::size_t end = kOff + kRelease + 2;
        const probe::Stereo out = upTo(end + at(0.2), score, engine);
        P.near("release.end.envelope", static_cast<double>(engine.voice(0).envelope), 0.0, 0.0);
        P.eq("release.end.voices_sounding", engine.activeVoices(), 0);
        P.le("release.end.peak_after", static_cast<double>(probe::peak(probe::slice(out.l, end, at(0.2)))), 0.0);
    }

    // AGAIN: the key once more, halfway through the release.
    {
        std::vector<probe::ScoreEvent> again = score;
        again.push_back(probe::on(kOff + kRelease / 2, 69, 1.0f));
        probe::runSynth(again, kOff + kRelease / 2 + 100, kFs, kBlock, adsr(), &engine);
        P.near("again.rises_from_release.envelope", static_cast<double>(engine.voice(0).envelope),
               0.25 + 100.0 / kAttack, 1e-4);
        P.eq("again.same_voice", engine.voice(0).note == 69 && engine.activeVoices() == 1 ? 1 : 0, 1);
    }

    // SMOOTH: GAIN 0 -> +12 dB.
    {
        tps::ParamValues loud = tps::defaultValues();
        loud[tps::param::gain] = 12.0f;
        const probe::Stereo out = change(tps::defaultValues(), loud);
        const double steadyLoud = maxStepIn(out.l, 0.8, 0.2);
        P.le("smooth.gain_up.max_step_rel_steady", maxStepIn(out.l, 0.5, 0.1) / steadyLoud, 1.0);
        const double after5ms = probe::dbFromLin(probe::toneAmplitude(out.l, 440.0, kFs, kChange + 240 - 109, 218));
        P.in("smooth.gain_up.after_5ms_dbfs", after5ms, probe::dbFromLin(0.25) + 0.5, probe::dbFromLin(0.25) + 11.5);
        P.near("smooth.gain_up.settled_rel_db",
               probe::dbFromLin(probe::toneAmplitude(out.l, 440.0, kFs, at(0.8), at(0.2))) - probe::dbFromLin(0.25),
               12.0, 0.02);
    }

    // SMOOTH: SUSTAIN 100 -> 20 % and back.
    {
        tps::ParamValues low = tps::defaultValues();
        low[tps::param::sustain] = 20.0f;
        const probe::Stereo down = change(tps::defaultValues(), low);
        P.le("smooth.sustain_down.max_step_rel_steady", maxStepIn(down.l, 0.5, 0.1) / maxStepIn(down.l, 0.3, 0.2),
             1.0);
        P.near("smooth.sustain_down.settled_rel_db",
               probe::dbFromLin(probe::toneAmplitude(down.l, 440.0, kFs, at(0.8), at(0.2))) - probe::dbFromLin(0.25),
               probe::dbFromLin(0.2), 0.02);
        const probe::Stereo up = change(low, tps::defaultValues());
        P.le("smooth.sustain_up.max_step_rel_steady", maxStepIn(up.l, 0.5, 0.1) / maxStepIn(up.l, 0.8, 0.2), 1.0);
    }

    // SMOOTH: RELEASE 300 -> 10 ms while the note releases (from 0.45 s).
    {
        tps::ParamValues slow = tps::defaultValues(), fast = tps::defaultValues();
        slow[tps::param::release] = 300.0f;
        fast[tps::param::release] = 10.0f;
        const probe::Stereo out = change(slow, fast, at(0.45));
        P.le("smooth.release_change.max_step_rel_steady", maxStepIn(out.l, 0.5, 0.1) / maxStepIn(out.l, 0.2, 0.2),
             1.0);
        P.le("smooth.release_change.silent_after_dbfs",
             probe::dbFromLin(static_cast<double>(probe::peak(probe::slice(out.l, at(0.6), at(0.4))))), -200.0);
    }
    return P.finish();
}
