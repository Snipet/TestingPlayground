// Source/dsp/SoundView.cpp: see SoundView.h.
#include "dsp/SoundView.h"

#include "Product.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>

namespace playground::measure
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

        // Power spectra of Blackman-Harris-windowed frames of one size (a power of two, at least 4) of the mono sum:
        // the window, the FFT's tables and its buffer are made once, for every frame. The real frame goes through a
        // complex FFT of half its size (its even samples the real parts, its odd ones the imaginary), whose bins are
        // then split into the real frame's.
        class Spectra
        {
        public:
            explicit Spectra(std::size_t size)
                : size_(size), half_(size / 2), window_(size), z_(size / 2), twiddle_(size / 4), split_(size / 2 + 1),
                  reverse_(size / 2)
            {
                // A four-term Blackman-Harris window: its sidelobes under -92 dB, so a pure tone shows as one line.
                for (std::size_t i = 0; i < size_; ++i)
                {
                    const double t = 2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(size_);
                    window_[i] = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2.0 * t) - 0.01168 * std::cos(3.0 * t);
                }
                for (std::size_t k = 0; k < twiddle_.size(); ++k)     // e^(-2 pi i k / half), the half-size FFT's
                {
                    const double a = -2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(half_);
                    twiddle_[k] = { std::cos(a), std::sin(a) };
                }
                for (std::size_t k = 0; k < split_.size(); ++k)       // e^(-2 pi i k / size), the split's
                {
                    const double a = -2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(size_);
                    split_[k] = { std::cos(a), std::sin(a) };
                }
                for (std::size_t i = 1, j = 0; i < half_; ++i)
                {
                    std::size_t bit = half_ >> 1;
                    for (; (j & bit) != 0; bit >>= 1)
                        j ^= bit;
                    j ^= bit;
                    reverse_[i] = j;
                }
            }

            // The frame about `centre` (0 outside the signal) into `p`: bins 0 to size / 2.
            void power(const std::vector<float>& l, const std::vector<float>& r, long centre, std::vector<double>& p)
            {
                const long from = centre - static_cast<long>(half_);
                const auto x = [&](std::size_t i) {
                    const long at = from + static_cast<long>(i);
                    return at >= 0 && at < static_cast<long>(l.size())
                               ? 0.5 * (static_cast<double>(l[static_cast<std::size_t>(at)]) + r[static_cast<std::size_t>(at)])
                                     * window_[i]
                               : 0.0;
                };
                for (std::size_t m = 0; m < half_; ++m)               // in bit-reversed order, for the FFT in place
                    z_[reverse_[m]] = { x(2 * m), x(2 * m + 1) };
                for (std::size_t len = 2; len <= half_; len <<= 1)
                {
                    const std::size_t stride = half_ / len;
                    for (std::size_t i = 0; i < half_; i += len)
                        for (std::size_t k = 0; k < len / 2; ++k)
                        {
                            const std::complex<double> u = z_[i + k], v = z_[i + k + len / 2] * twiddle_[k * stride];
                            z_[i + k] = u + v;
                            z_[i + k + len / 2] = u - v;
                        }
                }
                p.resize(half_ + 1);
                for (std::size_t k = 0; k <= half_; ++k)
                {
                    // X[k] = E[k] + e^(-2 pi i k / size) O[k], E and O the even and odd samples' spectra, from Z = E + iO.
                    const std::complex<double> zk = z_[k % half_], zc = std::conj(z_[(half_ - k) % half_]);
                    const std::complex<double> even = 0.5 * (zk + zc), odd = std::complex<double>(0.0, -0.5) * (zk - zc);
                    p[k] = std::norm(even + split_[k] * odd);
                }
            }

        private:
            std::size_t size_, half_;
            std::vector<double> window_;
            std::vector<std::complex<double>> z_, twiddle_, split_;
            std::vector<std::size_t> reverse_;
        };

        // A log axis of `count` cells from lo to hi over the bins of a spectrum of `size` points: each cell the
        // largest bin in its range, or the spectrum read between bins where the range is narrower than a bin. Its
        // edges are worked out once, for every frame.
        class Axis
        {
        public:
            Axis(double rate, std::size_t size, double lo, double hi, int count) : cells_(static_cast<std::size_t>(std::max(count, 0)))
            {
                const double hzPerBin = rate / static_cast<double>(size);
                const double ratio = std::log(hi / lo);
                for (int c = 0; c < count; ++c)
                {
                    const double f0 = lo * std::exp(ratio * static_cast<double>(c) / count);
                    const double f1 = lo * std::exp(ratio * static_cast<double>(c + 1) / count);
                    const double b0 = f0 / hzPerBin, b1 = f1 / hzPerBin;
                    Cell& cell = cells_[static_cast<std::size_t>(c)];
                    cell.span = std::floor(b1) - std::ceil(b0) >= 0.0;
                    if (cell.span)
                    {
                        cell.first = static_cast<std::size_t>(std::ceil(b0));
                        cell.last = static_cast<std::size_t>(std::floor(b1));
                    }
                    else
                    {
                        const double b = 0.5 * (b0 + b1);
                        cell.first = static_cast<std::size_t>(std::floor(b));
                        cell.t = b - std::floor(b);
                    }
                }
            }

            // A power spectrum onto the cells, in dB (power), unnormalised.
            void place(const std::vector<double>& p, float* out) const
            {
                for (std::size_t c = 0; c < cells_.size(); ++c)
                {
                    const Cell& cell = cells_[c];
                    double v = 0.0;
                    if (cell.span)
                        for (std::size_t k = cell.first; k <= cell.last && k < p.size(); ++k)
                            v = std::max(v, p[k]);
                    else if (cell.first + 1 < p.size())
                        v = (1.0 - cell.t) * p[cell.first] + cell.t * p[cell.first + 1];
                    out[c] = static_cast<float>(v > 1e-30 ? 10.0 * std::log10(v) : -300.0);
                }
            }

        private:
            struct Cell
            {
                bool span = false;                    // a range of bins (first to last), else between first and first + 1
                std::size_t first = 0, last = 0;
                double t = 0.0;
            };
            std::vector<Cell> cells_;
        };

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

    Sound renderSound(const Sources& files, const ViewRequest& request)
    {
        Session session(files, request.render.rate);
        return renderSound(session, request);
    }

    Sound renderSound(Session& session, const ViewRequest& request) { return renderSound(session, request, nullptr); }

    Sound renderSound(Session& session, const ViewRequest& request, RenderHooks* hooks)
    {
        Sound out;
        out.rate = session.rate();
        if (!session.compiled())
        {
            out.errors = session.errors();
            return out;
        }
        out.compiled = true;
        const double rate = out.rate;
        const float hold = std::clamp(request.render.holdSeconds, 0.05f, 10.0f);
        const float tail = std::clamp(request.render.tailSeconds, 0.0f, 10.0f);
        const std::size_t holdN = static_cast<std::size_t>(hold * rate);
        std::size_t total = holdN + static_cast<std::size_t>(tail * rate);
        const double hiHz = std::min(20000.0, 0.5 * rate);
        const std::string scenario = scenarioText(request.render);

        Session::Render played;
        if constexpr (kInstrument)
        {
            Request r = request.render;
            if (r.phrase.empty() && r.events.empty())
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
                r.notes = notes;
                total = request.length > 0 ? request.length : std::max(total, session.lengthOf(r));   // a late move too
                out.title = (notes.size() == 1 ? "Note " : "Notes ") + what + " at velocity "
                          + fmt("%.2f", std::clamp(request.render.velocity, 0.0f, 1.0f)) + ", held " + fmt("%.2f s", hold)
                          + ", then released for " + fmt("%.2f s", static_cast<double>(total - std::min(total, holdN)) / rate)
                          + (scenario.empty() ? "" : "; " + scenario);
                out.heldUntil = hold;
                out.cycleHz = noteHz(*std::min_element(notes.begin(), notes.end()));
                out.harmonics = notes.size() == 1;
                played = session.render(r, nullptr, nullptr, total, hooks);
            }
            else
            {
                // A phrase (or a recording): as long as it plays, and the tail; held until its last note-off.
                const Timeline tl = session.timeline(r, request.length);
                total = tl.length;
                int lowest = 127;
                for (const auto& [at, e] : tl.events)
                    if (e.kind == EventKind::noteOn)
                        lowest = std::min(lowest, static_cast<int>(e.note));
                out.title = "A scenario: " + scenario + ", then " + fmt("%.2f s", tail);
                out.cycleHz = lowest <= 127 && lowest >= 0 && !tl.events.empty() ? noteHz(lowest) : 0.0;
                played = session.render(r, nullptr, nullptr, total, hooks);
                out.heldUntil = static_cast<double>(played.heldUntil) / rate;
            }
        }
        else
        {
            // As long as the input and the tail, or until the scenario's last move or edit and the tail (silence there).
            total = request.length > 0                  ? request.length
                  : request.render.input != nullptr     ? session.lengthOf(request.render)
                                                        : std::max(total, session.lengthOf(request.render));
            const double after = static_cast<double>(total - std::min(total, holdN)) / rate;   // the silence after it
            std::vector<float> in(std::max(total, holdN), 0.0f);
            const double amp = std::pow(10.0, -12.0 / 20.0);
            const ViewInput input = request.render.signal >= 0 && request.render.signal <= static_cast<int>(ViewInput::silence)
                                        ? static_cast<ViewInput>(request.render.signal)
                                        : request.input;
            if (request.render.input != nullptr)
            {
                out.title = "Its own input, " + fmt("%.2f s", static_cast<double>(request.render.input->l.size()) / rate);
                out.heldUntil = static_cast<double>(std::min(total, request.render.input->l.size())) / rate;
            }
            else
                switch (input)
                {
                    case ViewInput::sweep:
                    {
                        // A log sweep: the phase of a frequency that rises from f1 to f2 over the held part.
                        const double f1 = 20.0, f2 = hiHz, T = static_cast<double>(holdN) / rate, k = std::log(f2 / f1);
                        for (std::size_t i = 0; i < holdN; ++i)
                        {
                            const double t = static_cast<double>(i) / rate;
                            in[i] = static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * f1 * T / k * (std::exp(t / T * k) - 1.0)));
                        }
                        out.title = "A sweep from 20 Hz to " + fmt("%.0f Hz", f2) + " at -12 dBFS for " + fmt("%.2f s", hold)
                                  + ", then " + fmt("%.2f s", after) + " of silence";
                        out.heldUntil = hold;
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
                        out.title = "Noise at -18 dBFS RMS for " + fmt("%.2f s", hold) + ", then " + fmt("%.2f s", after) + " of silence";
                        out.heldUntil = hold;
                        break;
                    }
                    case ViewInput::impulse:
                        if (total > 0)
                            in[0] = 0.5f;
                        out.title = "An impulse (0.5), then " + fmt("%.2f s", static_cast<double>(total) / rate) + " of silence";
                        out.fromStart = true;
                        break;
                    case ViewInput::sine:
                    {
                        const double hz = std::clamp(static_cast<double>(request.frequency), 10.0, 0.45 * rate);
                        for (std::size_t i = 0; i < holdN; ++i)
                            in[i] = static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate));
                        out.title = "A sine of " + fmt(hz >= 1000.0 ? "%.0f Hz" : "%.1f Hz", hz) + " at -12 dBFS for "
                                  + fmt("%.2f s", hold) + ", then " + fmt("%.2f s", after) + " of silence";
                        out.heldUntil = hold;
                        out.cycleHz = hz;
                        out.harmonics = true;
                        break;
                    }
                    case ViewInput::silence:
                        out.title = "Silence for " + fmt("%.2f s", static_cast<double>(total) / rate);
                        break;
                }
            if (!scenario.empty() && request.render.input == nullptr)
                out.title += "; " + scenario;
            played = session.render(request.render, &in, &in, total, hooks);
        }
        out.dials = played.dials;
        out.values = played.values;
        out.alerts = played.alerts;
        out.l = std::move(played.l);
        out.r = std::move(played.r);
        return out;
    }

    View view(const Sources& files, const ViewRequest& request)
    {
        Sound s = renderSound(files, request);
        if (!s.compiled)
        {
            View v;
            v.rate = s.rate;
            v.errors = s.errors;
            return v;
        }
        View v = analyse(s.l, s.r, s.rate, s.heldUntil, request, s.cycleHz, s.harmonics, s.fromStart);
        v.title = std::move(s.title);
        v.dials = std::move(s.dials);
        v.alerts = s.alerts;
        return v;
    }

    View analyse(const std::vector<float>& L, const std::vector<float>& R, double rate, double heldUntil,
                 const ViewRequest& request, double cycleHz, bool harmonics, bool fromStart)
    {
        View v;
        v.compiled = true;
        v.rate = rate;
        const std::size_t total = std::min(L.size(), R.size());
        v.length = total;
        v.hiHz = std::min(20000.0, 0.5 * rate);
        v.heldUntil = heldUntil;
        const std::size_t holdN = heldUntil > 0.0 ? std::min(total, static_cast<std::size_t>(heldUntil * rate)) : total;
        if (harmonics && cycleHz > 0.0)
            for (int k = 1; k <= 40 && cycleHz * k < v.hiHz; ++k)
                v.harmonics.push_back(cycleHz * k);

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
        {
            Spectra spectra(kFrame);
            const Axis axis(rate, kFrame, v.loHz, v.hiHz, v.rows);
            std::vector<double> p;
            std::vector<float> col(static_cast<std::size_t>(v.rows));
            for (int c = 0; c < cols && total > 0; ++c)
            {
                const long centre = static_cast<long>((static_cast<double>(c) + 0.5) / cols * static_cast<double>(total));
                spectra.power(L, R, centre, p);
                axis.place(p, col.data());
                for (int r = 0; r < v.rows; ++r)
                    v.cells[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols) + static_cast<std::size_t>(c)] =
                        col[static_cast<std::size_t>(r)];
            }
        }
        normalise(v.cells);

        // The average spectrum of the held part (an impulse's or silence's: all of it).
        {
            constexpr std::size_t kWelch = 8192;
            const std::size_t settle = std::min(holdN / 2, static_cast<std::size_t>(0.05 * rate));
            const std::size_t from = v.heldUntil > 0.0 ? settle : 0, to = v.heldUntil > 0.0 ? holdN : total;
            std::vector<double> sum(kWelch / 2 + 1, 0.0), p;
            Spectra spectra(kWelch);
            int frames = 0;
            for (std::size_t a = from; a + kWelch <= to || frames == 0; a += kWelch / 2)
            {
                spectra.power(L, R, static_cast<long>(a + kWelch / 2), p);
                for (std::size_t k = 0; k < sum.size(); ++k)
                    sum[k] += p[k];
                ++frames;
                if (a + kWelch > to)
                    break;
            }
            const int columns = std::max(32, request.spectrumColumns);
            v.spectrum.assign(static_cast<std::size_t>(columns), 0.0f);
            Axis(rate, kWelch, v.loHz, v.hiHz, columns).place(sum, v.spectrum.data());
            for (const float x : v.spectrum)
                v.spectrumTop = std::max(v.spectrumTop, x - static_cast<float>(10.0 * std::log10(std::max(frames, 1))));
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
                if (fromStart)
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
