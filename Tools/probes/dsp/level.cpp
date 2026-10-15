// PROBE name=dsp.level timeout=120
//
// dsp.level: how loud a note and a chord are, at 48 kHz, measured over one second of the sustain ([0.5, 1.5) s).
//   NOTE     A4 at full velocity peaks at 0.25 (-12.04 dBFS, the legacy headroom) through GAIN 0 dB: a single-bin DFT
//            at 440 Hz, within 0.02 dB; the other channel is the same bit for bit
//   VELOCITY at 0.5 it is 6.02 dB down; at 0.1, 20 dB down (the level is linear in the velocity), within 0.02 dB
//   GAIN     +6 dB and -24 dB move it by as much, within 0.02 dB
//   SUSTAIN  50 % holds it 6.02 dB down, within 0.02 dB
//   CHORD    C4 E4 G4 B4 at velocity 0.8: each note at 0.2 (-13.98 dBFS; Hann-windowed DFT, within 0.05 dB) and the
//            chord's RMS at sqrt(4 x 0.2^2 / 2) = 0.283 (-10.97 dBFS, within 0.05 dB: unrelated sines add in power)
//   SIXTEEN  sixteen notes at once, a major third apart (C2 to C7, velocity 0.25), each in the output at 0.0625
//            (-24.08 dBFS, Hann, within 0.05 dB): every voice reaches the mix
//   MODEL    A4 at velocity 0.8, sample by sample against a model in double, 0.25 x 0.8 x env(k) x sin(2 pi phase(k)),
//            phase(k) = k x inc / 2^32 (inc the note's uint32 increment), env(k) = min(1, (k + 1) / 240) (ATTACK 5 ms,
//            SUSTAIN 100 %), within 1e-6 on both channels over the first second: the pitch, the phase, the polarity
//            and the attack are the model's
#include "Probe.h"
#include "Rig.h"

#include "dsp/SineOscillator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <string>
#include <vector>

namespace
{
    constexpr double kFs = 48000.0;

    std::size_t at(double seconds) { return static_cast<std::size_t>(seconds * kFs); }

    probe::Stereo play(const std::vector<int>& notes, float velocity, const probe::tps::ParamValues& v)
    {
        std::vector<probe::ScoreEvent> score;
        for (const int note : notes)
            score.push_back(probe::on(0, note, velocity));
        return probe::runSynth(score, at(1.6), kFs, 256, v);
    }

    double toneDb(const probe::Stereo& s, int note)
    {
        return probe::dbFromLin(probe::toneAmplitude(s.l, probe::noteHz(note), kFs, at(0.5), at(1.0)));
    }

    double toneDbHann(const probe::Stereo& s, int note)
    {
        return probe::dbFromLin(probe::toneAmplitudeHann(s.l, probe::noteHz(note), kFs, at(0.5), at(1.0)));
    }
}

PROBE(dsp, level)
{
    namespace tps = probe::tps;
    const tps::ParamValues defaults = tps::defaultValues();
    const double full = probe::dbFromLin(0.25);

    {
        const probe::Stereo out = play({ 69 }, 1.0f, defaults);
        P.near("note.a4.velocity1.dbfs", toneDb(out, 69), full, 0.02);
        P.eq("note.left_equals_right", probe::sameBits(out.l, out.r) ? 1 : 0, 1);
        P.near("velocity.0_5.rel_db", toneDb(play({ 69 }, 0.5f, defaults), 69) - full, probe::dbFromLin(0.5), 0.02);
        P.near("velocity.0_1.rel_db", toneDb(play({ 69 }, 0.1f, defaults), 69) - full, -20.0, 0.02);
    }

    for (const float gain : { 6.0f, -24.0f })
    {
        tps::ParamValues v = defaults;
        v[tps::param::gain] = gain;
        P.near("gain." + std::string(gain > 0.0f ? "plus6" : "minus24") + ".rel_db", toneDb(play({ 69 }, 1.0f, v), 69)
               - full, static_cast<double>(gain), 0.02);
    }

    {
        tps::ParamValues v = defaults;
        v[tps::param::sustain] = 50.0f;
        P.near("sustain.50pct.rel_db", toneDb(play({ 69 }, 1.0f, v), 69) - full, probe::dbFromLin(0.5), 0.02);
    }

    {
        const std::vector<int> chord { 60, 64, 67, 71 };
        const probe::Stereo out = play(chord, 0.8f, defaults);
        for (const int note : chord)
            P.near("chord.note" + std::to_string(note) + ".dbfs", toneDbHann(out, note), probe::dbFromLin(0.2), 0.05);
        P.near("chord.rms_dbfs", probe::rmsDb(out.l, at(0.5), at(1.0)), probe::dbFromLin(0.2 * std::sqrt(2.0)), 0.05);
    }

    {
        std::vector<int> notes;
        for (int k = 0; k < tps::Engine::kVoices; ++k)
            notes.push_back(36 + 4 * k);
        const probe::Stereo out = play(notes, 0.25f, defaults);
        double worst = 0.0;
        for (const int note : notes)
            worst = std::max(worst, std::fabs(toneDbHann(out, note) - probe::dbFromLin(0.0625)));
        P.le("sixteen.worst_note_error_db", worst, 0.05);
    }
    {
        const probe::Stereo out = play({ 69 }, 0.8f, defaults);
        const uint32_t inc = tps::SineOscillator4::increment(69, tps::SineOscillator4::pitchOffset(kFs));
        double worst = 0.0;
        for (std::size_t k = 0; k < at(1.0); ++k)
        {
            const double phase = static_cast<double>(static_cast<uint32_t>(inc * static_cast<uint32_t>(k)));
            const double env = std::min(1.0, static_cast<double>(k + 1) / 240.0);
            const double model = 0.25 * static_cast<double>(0.8f) * env
                                 * std::sin(2.0 * std::numbers::pi * phase / 4294967296.0);
            worst = std::max({ worst, std::fabs(static_cast<double>(out.l[k]) - model),
                               std::fabs(static_cast<double>(out.r[k]) - model) });
        }
        P.le("model.a4.max_abs_error", worst, 1e-6);
    }
    return P.finish();
}
