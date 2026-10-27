// Source/dsp/SoundView.cpp: see SoundView.h.
#include "dsp/SoundView.h"

#include "Product.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>

namespace testingplayground::measure
{
    namespace
    {
        constexpr bool kInstrument = product::kIsInstrument;
        constexpr double kFloorDb = -120.0;

        std::string fmt(const char* f, double v)
        {
            char buf[64];
            std::snprintf(buf, sizeof buf, f, v);
            return buf;
        }

        double toDb(double a) { return a > 1e-12 ? 20.0 * std::log10(a) : -240.0; }

        [[maybe_unused]] std::string noteName(int n)
        {
            static constexpr const char* kNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
            const int pc = ((n % 12) + 12) % 12;
            return std::string(kNames[pc]) + std::to_string(n / 12 - 1);
        }

        [[maybe_unused]] double noteHz(int n) { return 440.0 * std::exp2((n - 69) / 12.0); }

        // The power spectrum of a Blackman-Harris-windowed frame of `size` (a power of two) of the mono sum about
        // `centre`: bins 0 to size / 2.
        std::vector<double> power(const std::vector<float>& l, const std::vector<float>& r, long centre, std::size_t size)
        {
            std::vector<std::complex<double>> a(size);
            const long half = static_cast<long>(size / 2);
            for (std::size_t i = 0; i < size; ++i)
            {
                const long at = centre - half + static_cast<long>(i);
                const double x = at >= 0 && at < static_cast<long>(l.size())
                                     ? 0.5 * (static_cast<double>(l[static_cast<std::size_t>(at)]) + r[static_cast<std::size_t>(at)])
                                     : 0.0;
                // A four-term Blackman-Harris window: its sidelobes under -92 dB, so a pure tone shows as one line.
                const double t = 2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(size);
                const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2.0 * t) - 0.01168 * std::cos(3.0 * t);
                a[i] = { x * w, 0.0 };
            }
            fft(a);
            std::vector<double> p(size / 2 + 1);
            for (std::size_t k = 0; k < p.size(); ++k)
                p[k] = std::norm(a[k]);
            return p;
        }

        // A power spectrum onto `count` cells of a log axis from lo to hi: each cell the largest bin in its range, or
        // the spectrum read between bins where the range is narrower than a bin. In dB (power), unnormalised.
        std::vector<float> onAxis(const std::vector<double>& p, double rate, std::size_t size, double lo, double hi, int count)
        {
            std::vector<float> out(static_cast<std::size_t>(std::max(count, 0)));
            const double hzPerBin = rate / static_cast<double>(size);
            const double ratio = std::log(hi / lo);
            for (int c = 0; c < count; ++c)
            {
                const double f0 = lo * std::exp(ratio * static_cast<double>(c) / count);
                const double f1 = lo * std::exp(ratio * static_cast<double>(c + 1) / count);
                const double b0 = f0 / hzPerBin, b1 = f1 / hzPerBin;
                double v = 0.0;
                if (std::floor(b1) - std::ceil(b0) >= 0.0)
                {
                    const auto first = static_cast<std::size_t>(std::ceil(b0)), last = static_cast<std::size_t>(std::floor(b1));
                    for (std::size_t k = first; k <= last && k < p.size(); ++k)
                        v = std::max(v, p[k]);
                }
                else
                {
                    const double b = 0.5 * (b0 + b1);
                    const auto k = static_cast<std::size_t>(std::floor(b));
                    const double t = b - std::floor(b);
                    if (k + 1 < p.size())
                        v = (1.0 - t) * p[k] + t * p[k + 1];
                }
                out[static_cast<std::size_t>(c)] = static_cast<float>(v > 1e-30 ? 10.0 * std::log10(v) : -300.0);
            }
            return out;
        }

        void normalise(std::vector<float>& db)
        {
            float top = -300.0f;
            for (const float v : db)
                top = std::max(top, v);
            for (float& v : db)
                v = std::max(static_cast<float>(kFloorDb), v - top);
        }
    }

    double rowOf(const View& v, double hz)
    {
        return std::log(hz / v.loHz) / std::log(v.hiHz / v.loHz) * static_cast<double>(v.rows);
    }

    double hzOfRow(const View& v, double row)
    {
        return v.loHz * std::exp(std::log(v.hiHz / v.loHz) * row / static_cast<double>(v.rows));
    }

    View view(const Sources& files, const ViewRequest& request)
    {
        View v;
        Session session(files, request.render.rate);
        v.rate = session.rate();
        if (!session.compiled())
        {
            v.errors = session.errors();
            return v;
        }
        v.compiled = true;
        const double rate = v.rate;
        const float hold = std::clamp(request.render.holdSeconds, 0.05f, 10.0f);
        const float tail = std::clamp(request.render.tailSeconds, 0.0f, 10.0f);
        const std::size_t holdN = static_cast<std::size_t>(hold * rate);
        const std::size_t total = holdN + static_cast<std::size_t>(tail * rate);
        v.length = total;
        v.hiHz = std::min(20000.0, 0.5 * rate);

        double cycleHz = 0.0;                                 // what the close-up shows four cycles of
        Session::Render out;
        if constexpr (kInstrument)
        {
            std::vector<int> notes;
            for (const int n : request.render.notes)
                if (n >= 0 && n <= 127 && std::find(notes.begin(), notes.end(), n) == notes.end() && notes.size() < 16)
                    notes.push_back(n);
            if (notes.empty())
                notes.push_back(48);
            std::string what;
            for (const int n : notes)
                what += (what.empty() ? "" : " ") + std::to_string(n) + " (" + noteName(n) + ")";
            v.title = (notes.size() == 1 ? "Note " : "Notes ") + what + " at velocity "
                    + fmt("%.2f", std::clamp(request.render.velocity, 0.0f, 1.0f)) + ", held " + fmt("%.2f s", hold)
                    + ", then released for " + fmt("%.2f s", tail);
            v.heldUntil = hold;
            cycleHz = noteHz(*std::min_element(notes.begin(), notes.end()));
            if (notes.size() == 1)
                for (int k = 1; k <= 40 && cycleHz * k < v.hiHz; ++k)
                    v.harmonics.push_back(cycleHz * k);
            Request r = request.render;
            r.notes = notes;
            out = session.render(r, nullptr, nullptr, total);
        }
        else
        {
            std::vector<float> in(total, 0.0f);
            const double amp = std::pow(10.0, -12.0 / 20.0);
            switch (request.input)
            {
                case ViewInput::sweep:
                {
                    // A log sweep: the phase of a frequency that rises from f1 to f2 over the held part.
                    const double f1 = 20.0, f2 = v.hiHz, T = static_cast<double>(holdN) / rate, k = std::log(f2 / f1);
                    for (std::size_t i = 0; i < holdN; ++i)
                    {
                        const double t = static_cast<double>(i) / rate;
                        in[i] = static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * f1 * T / k * (std::exp(t / T * k) - 1.0)));
                    }
                    v.title = "A sweep from 20 Hz to " + fmt("%.0f Hz", f2) + " at -12 dBFS for " + fmt("%.2f s", hold)
                            + ", then " + fmt("%.2f s", tail) + " of silence";
                    v.heldUntil = hold;
                    break;
                }
                case ViewInput::noise:
                {
                    uint32_t seed = 22222;
                    const float noiseAmp = static_cast<float>(std::pow(10.0, -18.0 / 20.0) * std::sqrt(3.0));
                    for (std::size_t i = 0; i < holdN; ++i)
                    {
                        seed = seed * 1664525u + 1013904223u;
                        in[i] = noiseAmp * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
                    }
                    v.title = "Noise at -18 dBFS RMS for " + fmt("%.2f s", hold) + ", then " + fmt("%.2f s", tail) + " of silence";
                    v.heldUntil = hold;
                    break;
                }
                case ViewInput::impulse:
                    if (total > 0)
                        in[0] = 0.5f;
                    v.title = "An impulse (0.5), then " + fmt("%.2f s", hold + tail) + " of silence";
                    break;
                case ViewInput::sine:
                {
                    const double hz = std::clamp(static_cast<double>(request.frequency), 10.0, 0.45 * rate);
                    for (std::size_t i = 0; i < holdN; ++i)
                        in[i] = static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate));
                    v.title = "A sine of " + fmt(hz >= 1000.0 ? "%.0f Hz" : "%.1f Hz", hz) + " at -12 dBFS for "
                            + fmt("%.2f s", hold) + ", then " + fmt("%.2f s", tail) + " of silence";
                    v.heldUntil = hold;
                    cycleHz = hz;
                    for (int k = 1; k <= 40 && hz * k < v.hiHz; ++k)
                        v.harmonics.push_back(hz * k);
                    break;
                }
                case ViewInput::silence:
                    v.title = "Silence for " + fmt("%.2f s", hold + tail);
                    break;
            }
            out = session.render(request.render, &in, &in, total);
        }
        v.dials = out.dials;
        v.alerts = out.alerts;
        const std::vector<float>& L = out.l;
        const std::vector<float>& R = out.r;

        // The waveform and the level, a column at a time.
        const int cols = std::max(16, request.columns);
        v.minL.assign(static_cast<std::size_t>(cols), 0.0f);
        v.maxL = v.minL;
        v.minR = v.minL;
        v.maxR = v.minL;
        v.levelDb.assign(static_cast<std::size_t>(cols), -240.0f);
        double top = 0.0;
        for (std::size_t i = 0; i < total; ++i)
        {
            top = std::max({ top, static_cast<double>(std::fabs(L[i])), static_cast<double>(std::fabs(R[i])) });
            v.stereo = v.stereo || L[i] != R[i];
        }
        v.peakDb = toDb(top);
        {
            const std::size_t settle = std::min(holdN / 2, static_cast<std::size_t>(0.05 * rate));
            const std::size_t end = v.heldUntil > 0.0 ? holdN : total;
            double e = 0.0;
            for (std::size_t i = settle; i < end; ++i)
                e += 0.5 * (static_cast<double>(L[i]) * L[i] + static_cast<double>(R[i]) * R[i]);
            v.rmsDb = end > settle ? toDb(std::sqrt(e / static_cast<double>(end - settle))) : -240.0;
        }
        const std::size_t w10 = static_cast<std::size_t>(0.01 * rate);
        for (int c = 0; c < cols && total > 0; ++c)
        {
            const std::size_t a = total * static_cast<std::size_t>(c) / static_cast<std::size_t>(cols);
            const std::size_t b = std::max(a + 1, total * static_cast<std::size_t>(c + 1) / static_cast<std::size_t>(cols));
            float lo = L[a], hi = L[a], lo2 = R[a], hi2 = R[a];
            for (std::size_t i = a; i < b && i < total; ++i)
            {
                lo = std::min(lo, L[i]);
                hi = std::max(hi, L[i]);
                lo2 = std::min(lo2, R[i]);
                hi2 = std::max(hi2, R[i]);
            }
            const auto k = static_cast<std::size_t>(c);
            v.minL[k] = lo;
            v.maxL[k] = hi;
            v.minR[k] = lo2;
            v.maxR[k] = hi2;
            const std::size_t mid = (a + b) / 2;
            const std::size_t f = mid > w10 / 2 ? mid - w10 / 2 : 0, t = std::min(total, f + w10);
            double e = 0.0;
            for (std::size_t i = f; i < t; ++i)
                e += 0.5 * (static_cast<double>(L[i]) * L[i] + static_cast<double>(R[i]) * R[i]);
            v.levelDb[k] = static_cast<float>(t > f ? toDb(std::sqrt(e / static_cast<double>(t - f))) : -240.0);
        }

        // The spectrogram: a 4096-point window about each column's middle (85 ms at 48 kHz: a low note's harmonics
        // apart, an envelope's shape still clear).
        v.columns = cols;
        v.rows = std::max(32, request.rows);
        v.cells.assign(static_cast<std::size_t>(v.rows) * static_cast<std::size_t>(cols), static_cast<float>(kFloorDb));
        constexpr std::size_t kFrame = 4096;
        std::vector<float> all;
        all.reserve(v.cells.size());
        for (int c = 0; c < cols && total > 0; ++c)
        {
            const long centre = static_cast<long>((static_cast<double>(c) + 0.5) / cols * static_cast<double>(total));
            const std::vector<float> col = onAxis(power(L, R, centre, kFrame), rate, kFrame, v.loHz, v.hiHz, v.rows);
            for (int r = 0; r < v.rows; ++r)
                v.cells[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols) + static_cast<std::size_t>(c)] =
                    col[static_cast<std::size_t>(r)];
        }
        normalise(v.cells);

        // The average spectrum of the held part (an impulse's or silence's: all of it).
        {
            constexpr std::size_t kWelch = 8192;
            const std::size_t settle = std::min(holdN / 2, static_cast<std::size_t>(0.05 * rate));
            const std::size_t from = v.heldUntil > 0.0 ? settle : 0, to = v.heldUntil > 0.0 ? holdN : total;
            std::vector<double> sum(kWelch / 2 + 1, 0.0);
            int frames = 0;
            for (std::size_t a = from; a + kWelch <= to || frames == 0; a += kWelch / 2)
            {
                const std::vector<double> p = power(L, R, static_cast<long>(a + kWelch / 2), kWelch);
                for (std::size_t k = 0; k < sum.size(); ++k)
                    sum[k] += p[k];
                ++frames;
                if (a + kWelch > to)
                    break;
            }
            v.spectrum = onAxis(sum, rate, kWelch, v.loHz, v.hiHz, std::max(32, request.spectrumColumns));
            normalise(v.spectrum);
        }

        // The close-up.
        {
            std::size_t a = 0, b = 0;
            if (request.from >= 0.0f && request.to > request.from)
            {
                a = std::min(total, static_cast<std::size_t>(std::lround(static_cast<double>(request.from) * rate)));
                b = std::min(total, static_cast<std::size_t>(
                                        std::lround(static_cast<double>(std::min(request.to, request.from + 2.0f)) * rate)));
            }
            else
            {
                const std::size_t span = cycleHz > 0.0
                                             ? std::clamp(static_cast<std::size_t>(4.0 * rate / cycleHz),
                                                          static_cast<std::size_t>(0.002 * rate), static_cast<std::size_t>(0.05 * rate))
                                             : static_cast<std::size_t>(0.01 * rate);
                const std::size_t middle = v.heldUntil > 0.0 ? holdN / 2
                                                             : std::min(total, static_cast<std::size_t>(0.002 * rate) + span / 2);
                a = middle > span / 2 ? middle - span / 2 : 0;
                b = std::min(total, a + span);
                if (!kInstrument && request.input == ViewInput::impulse)
                {
                    a = 0;                                   // the response's start
                    b = std::min(total, static_cast<std::size_t>(0.02 * rate));
                }
            }
            if (b <= a)
                b = std::min(total, a + 1);
            v.zoomFrom = a;
            v.zoomTo = b;
            v.zoomL.assign(L.begin() + static_cast<std::ptrdiff_t>(a), L.begin() + static_cast<std::ptrdiff_t>(b));
            v.zoomR.assign(R.begin() + static_cast<std::ptrdiff_t>(a), R.begin() + static_cast<std::ptrdiff_t>(b));
        }
        return v;
    }
}
