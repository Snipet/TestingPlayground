// PROBE name=dsp.response timeout=120
//
// dsp.response: the instrument against what it promises.
//   PITCH    a held note is at 440 * 2^((n - 69) / 12) Hz within 0.005 Hz (A4, C4, A5), from its zero crossings
//   LEVEL    a note at full velocity peaks at 0.25 (-12.04 dBFS) through GAIN 0 dB, and follows GAIN (+6 dB) and the
//            velocity (0.5: -6.02 dB); the triangle's fundamental is 8 / pi^2 of its peak; single-bin DFT over one
//            second, within 0.02 dB
//   ENVELOPE ATTACK is linear: halfway through a 100 ms attack the level is -6.02 dB (within 0.25 dB); RELEASE falls
//            80 dB in its time: 150 ms into a 300 ms release it is -40 dB (within 1 dB)
//   VOICES   eight notes held at once (C3 E3 G3 C4 E4 G4 C5 E5, velocity 0.5) are each in the output at -18.06 dBFS
//            (Hann-windowed DFT, within 0.05 dB), so every voice reaches the mix; ten notes at once sound on eight
//            voices (the oldest stolen); all are silent after their release
//   CLICKS   nothing steps when a voice is reused: a key retriggered 10 ms into its release at a tenth of its velocity,
//            and a ninth note (velocity 0.05) stealing from eight held at full velocity, step from sample to sample no
//            more than the steady sound before them did (a level that jumps instead steps 7 to 8 times as far)
#include "Probe.h"
#include "Rig.h"

#include <cmath>
#include <numbers>
#include <string>
#include <vector>

namespace fpt = testingplayground;

namespace
{
    constexpr double kFs = 48000.0;

    double noteHz(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

    std::size_t at(double seconds) { return static_cast<std::size_t>(seconds * kFs); }

    // A note held from 0 for `seconds` (never released), at the given parameter values.
    probe::Stereo held(int note, double seconds, const fpt::ParamValues& v, float velocity = 1.0f)
    {
        return probe::runSynth({ probe::on(0, note, velocity) }, at(seconds), kFs, 256, v);
    }

    double levelDb(const probe::Stereo& s, double hz)
    {
        return probe::dbFromLin(probe::toneAmplitude(s.l, hz, kFs, at(0.5), at(1.0)));
    }
}

PROBE(dsp, response)
{
    const fpt::ParamValues defaults = fpt::defaultValues();
    const double full = probe::dbFromLin(0.25);

    for (const int note : { 69, 60, 81 })
    {
        const probe::Stereo out = held(note, 1.6, defaults);
        P.near("pitch.note" + std::to_string(note) + ".hz", probe::frequencyHz(out.l, kFs, at(0.5), at(1.0)),
               noteHz(note), 0.005);
    }

    {
        const probe::Stereo out = held(69, 1.6, defaults);
        P.near("level.a4.gain0.peak_dbfs", levelDb(out, 440.0), full, 0.02);
        P.eq("level.left_equals_right", probe::sameBits(out.l, out.r), 1);
        fpt::ParamValues v = defaults;
        v[fpt::param::gain] = 6.0f;
        P.near("level.a4.gain6.peak_dbfs", levelDb(held(69, 1.6, v), 440.0), full + 6.0, 0.02);
        P.near("level.a4.velocity0.5.rel_db", levelDb(held(69, 1.6, defaults, 0.5f), 440.0) - full,
               probe::dbFromLin(0.5), 0.02);
        v = defaults;
        v[fpt::param::wave] = 1.0f;
        P.near("level.a4.triangle.fundamental_dbfs", levelDb(held(69, 1.6, v), 440.0),
               probe::dbFromLin(0.25 * 8.0 / (std::numbers::pi * std::numbers::pi)), 0.02);
    }

    {
        fpt::ParamValues v = defaults;
        v[fpt::param::attack] = 100.0f;
        const probe::Stereo out = held(69, 0.5, v);
        // Two cycles of 440 Hz (218 samples) centred at 50 ms: the linear ramp averages to its middle.
        const double halfway = probe::toneAmplitude(out.l, 440.0, kFs, at(0.05) - 109, 218);
        P.near("env.attack100ms.at50ms_rel_db", probe::dbFromLin(halfway / 0.25), -6.0206, 0.25);
    }

    {
        fpt::ParamValues v = defaults;
        v[fpt::param::release] = 300.0f;
        const probe::Stereo out = probe::runSynth({ probe::on(0, 69, 1.0f), probe::off(at(0.5), 69) }, at(1.0), kFs,
                                                  256, v);
        const double mid = probe::toneAmplitude(out.l, 440.0, kFs, at(0.65) - 54, 109);
        P.near("env.release300ms.at150ms_rel_db", probe::dbFromLin(mid / 0.25), -40.0, 1.0);
        P.le("env.release300ms.after_600ms_dbfs", probe::dbFromLin(probe::peak(std::vector<float>(
                                                      out.l.begin() + static_cast<std::ptrdiff_t>(at(0.9)), out.l.end()))),
             -90.0);
    }

    // VOICES: eight notes, each in the output.
    {
        const int chord[fpt::Engine::kVoices] = { 48, 52, 55, 60, 64, 67, 72, 76 };
        std::vector<probe::ScoreEvent> score;
        for (const int note : chord)
            score.push_back(probe::on(0, note, 0.5f));
        const probe::Stereo out = probe::runSynth(score, at(1.6), kFs, 256, defaults);
        for (const int note : chord)
            P.near("voices.eight_notes.note" + std::to_string(note) + ".level_dbfs",
                   probe::dbFromLin(probe::toneAmplitudeHann(out.l, noteHz(note), kFs, at(0.5), at(1.0))),
                   full + probe::dbFromLin(0.5), 0.05);
    }

    {
        std::vector<probe::ScoreEvent> score;
        for (int k = 0; k < 10; ++k)
            score.push_back(probe::on(static_cast<std::size_t>(k), 60 + k, 0.5f));
        fpt::Engine engine;
        probe::runSynth(score, at(0.2), kFs, 256, defaults, &engine);
        P.eq("voices.ten_notes.active", engine.activeVoices(), fpt::Engine::kVoices);
        for (int k = 0; k < 10; ++k)
            score.push_back(probe::off(at(0.2), 60 + k));
        probe::runSynth(score, at(1.0), kFs, 256, defaults, &engine);
        P.eq("voices.after_release.active", engine.activeVoices(), 0);
    }

    // CLICKS: the largest sample step over 10 ms from the retrigger or the steal, against the steady sound's before.
    {
        const std::size_t release = at(0.5), again = release + at(0.010);
        const probe::Stereo out = probe::runSynth({ probe::on(0, 69, 1.0f), probe::off(release, 69),
                                                    probe::on(again, 69, 0.1f) }, at(0.7), kFs, 256, defaults);
        const double steady = probe::maxStep(out.l, at(0.2), release - at(0.2));
        P.le("clicks.retrigger.max_step_rel_steady", probe::maxStep(out.l, again, at(0.010)) / steady, 1.0);
    }
    {
        std::vector<probe::ScoreEvent> score;
        for (int k = 0; k < fpt::Engine::kVoices; ++k)
            score.push_back(probe::on(static_cast<std::size_t>(k), 48 + k, 1.0f));
        const std::size_t steal = at(0.5) + 3;
        score.push_back(probe::on(steal, 90, 0.05f));
        const probe::Stereo out = probe::runSynth(score, at(0.7), kFs, 256, defaults);
        const double steady = probe::maxStep(out.l, at(0.3), steal - at(0.3));
        P.le("clicks.steal.max_step_rel_steady", probe::maxStep(out.l, steal, at(0.010)) / steady, 1.0);
    }
    return P.finish();
}
