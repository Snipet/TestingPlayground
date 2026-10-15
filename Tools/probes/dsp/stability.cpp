// PROBE name=dsp.stability timeout=600
//
// dsp.stability: the reverb at every corner of its parameter range (each of the 8 parameters at its minimum or its
// maximum: 256 settings; LINES at 4 and 16), 10 s of white noise in (uniform, independent on the two inputs, RMS
// -10.8 dBFS), at 48 kHz in blocks of 256:
//   finite    every output sample finite
//   no growth the RMS of the last 2.5 s is no more than 2 dB above that of 2.5 to 5 s, at every corner. A loop whose
//             gain reached 1 would grow without bound (tens of dB by 10 s); a stable one only builds up towards its
//             steady state (DECAY 20 s: 1 - 10^(-6 t / 20), +0.3 dB between the two windows' centres) and fluctuates
//             around it: a few sharp modes (1 m, 4 lines, 20 s) swing the RMS of a second by +-1.5 dB with no trend
//             (checked over 30 s while writing this probe)
//   level     with MIX 100 %, every one-second RMS of the wet output from 2 s on, less GAIN, lies within -12 dB and
//             +4 dB of the input's (measured: -6.5 to +1.3 dB): the input gain (Fdn.h, k = sqrt(1 - mean g^2)) holds
//             the steady level near the input's however long the decay; HF DECAY 10 % takes the most off (the top of
//             the noise decays ten times faster)
// The rows name the worst corner.
#include "Probe.h"
#include "Rig.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>

namespace tp = testingplayground;

namespace
{
    std::string describe(const tp::ParamValues& v)
    {
        char buf[200];
        std::snprintf(buf, sizeof buf, "size %.0f decay %.0f hf %.0f mix %.0f gain %.0f lines %d rate %.2f depth %.1f",
                      static_cast<double>(v[tp::param::size]), static_cast<double>(v[tp::param::decay]),
                      static_cast<double>(v[tp::param::hfdecay]), static_cast<double>(v[tp::param::mix]),
                      static_cast<double>(v[tp::param::gain]), tp::linesFromChoice(v[tp::param::lines]),
                      static_cast<double>(v[tp::param::modrate]), static_cast<double>(v[tp::param::moddepth]));
        return buf;
    }
}

PROBE(dsp, stability)
{
    constexpr double fs = 48000.0;
    const std::size_t n = static_cast<std::size_t>(10.0 * fs), sec = static_cast<std::size_t>(fs);
    probe::Stereo noise(n);
    std::mt19937 rng(11u);
    std::uniform_real_distribution<float> u(-0.5f, 0.5f);
    for (std::size_t i = 0; i < n; ++i)
    {
        noise.l[i] = u(rng);
        noise.r[i] = u(rng);
    }
    const double inDb = probe::rmsDb(noise.l, 0, n);

    bool finite = true;
    double worstGrowth = -1e9, lowLevel = 1e9, highLevel = -1e9;
    std::string worstGrowthAt, lowAt, highAt, nanAt;
    int settings = 0;
    for (unsigned corner = 0; corner < (1u << tp::param::count); ++corner)
    {
        tp::ParamValues v{};
        for (std::size_t i = 0; i < tp::param::count; ++i)
            v[i] = (corner >> i) & 1u ? tp::kParams[i].max : tp::kParams[i].min;
        const probe::Stereo out = probe::runEffect(noise, fs, 256, v);
        ++settings;
        if (!probe::allFinite(out.l) || !probe::allFinite(out.r))
        {
            finite = false;
            nanAt = describe(v);
            continue;
        }
        const std::size_t half = sec * 5 / 2;
        for (const std::vector<float>* ch : { &out.l, &out.r })
        {
            const double growth = probe::rmsDb(*ch, 3 * half, half) - probe::rmsDb(*ch, half, half);
            if (growth > worstGrowth)
            {
                worstGrowth = growth;
                worstGrowthAt = describe(v);
            }
            if (v[tp::param::mix] != 100.0f)
                continue;
            for (std::size_t s = 2; s < 10; ++s)
            {
                const double level = probe::rmsDb(*ch, s * sec, sec) - inDb - static_cast<double>(v[tp::param::gain]);
                if (level < lowLevel)
                {
                    lowLevel = level;
                    lowAt = describe(v);
                }
                if (level > highLevel)
                {
                    highLevel = level;
                    highAt = describe(v);
                }
            }
        }
    }
    std::printf("%d settings; worst growth %+.3f dB at %s\n", settings, worstGrowth, worstGrowthAt.c_str());
    std::printf("wet level %+.2f dB (at %s) to %+.2f dB (at %s)\n", lowLevel, lowAt.c_str(), highLevel, highAt.c_str());
    if (!finite)
        std::printf("non-finite output at %s\n", nanAt.c_str());

    P.eq("corners.count", static_cast<int64_t>(settings), 256);
    P.eq("corners.finite", finite ? 1 : 0, 1);
    P.le("corners.growth_7s5_10s_vs_2s5_5s_db", worstGrowth, 2.0);
    P.ge("corners.wet_level_min_db", lowLevel, -12.0);
    P.le("corners.wet_level_max_db", highLevel, 4.0);
    return P.finish();
}
