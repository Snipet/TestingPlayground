// PROBE name=dsp.modulation timeout=60
//
// dsp.modulation: the LFO moves each line's read as Fdn.h says (48 kHz, SIZE 12 m held, so the delays do not glide):
// after t seconds line i of n reads at
//
//   D_i + depth_i m(RATE t + i / n),      depth_i = min(DEPTH fs, 0.45 D_i) samples
//
// with D_i the layout's prime and m the shape's table (LfoShape.h: 256 segments, read linearly, one cycle per period).
// Fdn::readDelay(i) is where line i read on the last sample. Checked on every line at 16 and at 4 lines, at RATE
// 0.5 Hz (the default) and 7 Hz, DEPTH 2 ms and 0.5 ms, the default shape and a lopsided one, at 1.3 s and 2.71 s:
// within 0.02 samples. What may separate the two: the phase increment rounded to 2^-32 of a cycle (under 2e-5 cycles
// in 3 s) and float rounding, 0.006 samples on the default shape's slope at 2 ms. A RATE that is not followed, or
// lines that move in step instead of i / n of a cycle apart, miss by up to 2 depth_i (tens of samples).
#include "Probe.h"
#include "Rig.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace tp = testingplayground;

namespace
{
    constexpr double kFs = 48000.0;

    // The table at `cycles` (any real number: its fractional part is the phase), read linearly as the Fdn reads it.
    double tableAt(const tp::LfoTable& t, double cycles)
    {
        const double x = (cycles - std::floor(cycles)) * tp::kLfoTableSize;
        const auto i = std::min(static_cast<std::size_t>(x), static_cast<std::size_t>(tp::kLfoTableSize - 1));
        const double f = x - static_cast<double>(i);
        return static_cast<double>(t.v[i]) + f * (static_cast<double>(t.v[i + 1]) - static_cast<double>(t.v[i]));
    }

    tp::LfoShape lopsided()
    {
        tp::LfoShape s;
        s.points[0] = { 0.0f, 0.4f, 0.0f };
        s.points[1] = { 0.15f, 1.0f, 2.0f };
        s.points[2] = { 0.55f, 0.1f, -3.0f };
        s.points[3] = { 1.0f, 0.4f, 0.0f };
        s.count = 4;
        tp::sanitize(s);
        return s;
    }
}

PROBE(dsp, modulation)
{
    struct Case { int lines; float rate, depthMs; bool lopsided; };
    for (const Case c : { Case{ 16, 0.5f, 2.0f, false }, Case{ 16, 7.0f, 2.0f, false }, Case{ 4, 7.0f, 0.5f, true },
                          Case{ 4, 0.5f, 2.0f, false } })
    {
        tp::LfoTable table;
        tp::renderTable(c.lopsided ? lopsided() : tp::defaultShape(), table);
        tp::ParamValues v = probe::wetOnly();
        v[tp::param::lines] = c.lines == 4 ? 0.0f : (c.lines == 8 ? 1.0f : 2.0f);
        v[tp::param::modrate] = c.rate;
        v[tp::param::moddepth] = c.depthMs;
        const tp::FdnLayout layout = tp::makeLayout(static_cast<float>(kFs), v[tp::param::size], c.lines);

        tp::Engine e;
        e.prepare(kFs, 256);
        e.setLfoTable(table);
        std::vector<float> l(256, 0.0f), r(256, 0.0f);
        std::size_t done = 0;
        char key[64];
        std::snprintf(key, sizeof key, "mod.n%d.rate%g.depth%g%s", c.lines, static_cast<double>(c.rate),
                      static_cast<double>(c.depthMs), c.lopsided ? ".lopsided" : "");
        for (const double at : { 1.3, 2.71 })
        {
            const auto until = static_cast<std::size_t>(at * kFs);
            while (done < until)
            {
                const auto len = std::min<std::size_t>(256, until - done);
                e.setParams(v);
                float* ch[2] = { l.data(), r.data() };
                e.process(ch, 2, static_cast<int>(len), {});
                std::fill(l.begin(), l.end(), 0.0f);
                std::fill(r.begin(), r.end(), 0.0f);
                done += len;
            }
            const double cycles = static_cast<double>(done) * static_cast<double>(c.rate) / kFs;
            double err = 0.0;
            int worst = 0;
            for (int i = 0; i < c.lines; ++i)
            {
                const double d = static_cast<double>(layout.delay[static_cast<std::size_t>(i)]);
                const double depth = std::min(static_cast<double>(c.depthMs) * 0.001 * kFs, 0.45 * d);
                const double want = d + depth * tableAt(table, cycles + static_cast<double>(i) / c.lines);
                const double got = static_cast<double>(e.fdn().readDelay(i));
                if (std::fabs(got - want) > err)
                {
                    err = std::fabs(got - want);
                    worst = i;
                }
            }
            std::printf("%s at %.2f s: worst line %d, %.4f samples off\n", key, at, worst, err);
            P.le(std::string(key) + ".at" + std::to_string(static_cast<int>(at * 100.0)) + "cs.read_err_samples",
                 err, 0.02);
        }
    }
    return P.finish();
}
