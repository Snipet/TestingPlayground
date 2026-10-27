// Source/dsp/Measure.cpp: see Measure.h.
#include "dsp/Measure.h"

#include "dsp/Controls.h"
#include "dsp/Engine.h"
#include "dsp/Events.h"
#include "fminus/Patch.h"

#include "Product.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <memory>
#include <numbers>
#include <set>

namespace testingplayground::measure
{
    namespace
    {
        constexpr int kBlock = 256;
        constexpr bool kInstrument = product::kIsInstrument;

        // ---- text --------------------------------------------------------------------------------------------------
        std::string fmt(const char* f, double v)
        {
            char buf[64];
            std::snprintf(buf, sizeof buf, f, v);
            return buf;
        }

        double toDb(double a) { return a > 1e-12 ? 20.0 * std::log10(a) : -240.0; }

        // A level in dBFS, or "silent" below -150.
        std::string dbfs(double a)
        {
            const double d = toDb(a);
            return d < -150.0 ? std::string("silent") : fmt("%.1f dBFS", d);
        }

        std::string seconds(double s) { return fmt("%.2f s", s); }

        [[maybe_unused]] std::string noteName(int n)
        {
            static constexpr const char* kNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
            const int pc = ((n % 12) + 12) % 12;
            return std::string(kNames[pc]) + std::to_string(n / 12 - 1);
        }

        [[maybe_unused]] double noteHz(int n) { return 440.0 * std::exp2((n - 69) / 12.0); }

        // ---- measures over [from, to) of a channel pair ------------------------------------------------------------
        struct Stereo
        {
            std::vector<float> l, r;
            std::size_t size() const { return l.size(); }
        };

        double peak(const Stereo& s, std::size_t from, std::size_t to)
        {
            double p = 0.0;
            for (std::size_t i = from; i < to && i < s.size(); ++i)
                p = std::max({ p, static_cast<double>(std::fabs(s.l[i])), static_cast<double>(std::fabs(s.r[i])) });
            return p;
        }

        double rms(const Stereo& s, std::size_t from, std::size_t to)
        {
            double e = 0.0;
            std::size_t n = 0;
            for (std::size_t i = from; i < to && i < s.size(); ++i, ++n)
                e += 0.5 * (static_cast<double>(s.l[i]) * s.l[i] + static_cast<double>(s.r[i]) * s.r[i]);
            return n > 0 ? std::sqrt(e / static_cast<double>(n)) : 0.0;
        }

        double dc(const Stereo& s, std::size_t from, std::size_t to)
        {
            double m = 0.0;
            std::size_t n = 0;
            for (std::size_t i = from; i < to && i < s.size(); ++i, ++n)
                m += 0.5 * (static_cast<double>(s.l[i]) + s.r[i]);
            return n > 0 ? m / static_cast<double>(n) : 0.0;
        }

        // The left/right correlation (1: mono, 0: unrelated, -1: out of phase), and the balance in dB (left over right).
        void stereo(const Stereo& s, std::size_t from, std::size_t to, double& corr, double& balanceDb)
        {
            double ll = 0.0, rr = 0.0, lr = 0.0;
            for (std::size_t i = from; i < to && i < s.size(); ++i)
            {
                ll += static_cast<double>(s.l[i]) * s.l[i];
                rr += static_cast<double>(s.r[i]) * s.r[i];
                lr += static_cast<double>(s.l[i]) * s.r[i];
            }
            corr = ll > 0.0 && rr > 0.0 ? lr / std::sqrt(ll * rr) : (ll == rr ? 1.0 : 0.0);
            balanceDb = ll > 0.0 && rr > 0.0 ? 10.0 * std::log10(ll / rr) : 0.0;
        }

        // The largest step between two neighbouring samples of either channel, and where.
        double largestStep(const Stereo& s, std::size_t& at)
        {
            double m = 0.0;
            at = 0;
            for (std::size_t i = 1; i < s.size(); ++i)
            {
                const double d = std::max(std::fabs(static_cast<double>(s.l[i]) - s.l[i - 1]),
                                          std::fabs(static_cast<double>(s.r[i]) - s.r[i - 1]));
                if (d > m)
                {
                    m = d;
                    at = i;
                }
            }
            return m;
        }

        // The steps a sound makes every cycle: the median over [from, to) of each window's largest step between two
        // neighbouring samples (windows of w samples), against which a click is the one step far larger.
        [[maybe_unused]] double usualStep(const Stereo& s, std::size_t from, std::size_t to, std::size_t w)
        {
            std::vector<double> maxima;
            for (std::size_t a = std::max<std::size_t>(from, 1); w > 0 && a + w <= std::min(to, s.size()); a += w)
            {
                double m = 0.0;
                for (std::size_t i = a; i < a + w; ++i)
                    m = std::max(m, std::max(std::fabs(static_cast<double>(s.l[i]) - s.l[i - 1]),
                                             std::fabs(static_cast<double>(s.r[i]) - s.r[i - 1])));
                maxima.push_back(m);
            }
            if (maxima.empty())
                return 0.0;
            std::nth_element(maxima.begin(), maxima.begin() + static_cast<std::ptrdiff_t>(maxima.size() / 2), maxima.end());
            return maxima[maxima.size() / 2];
        }

        // When the level (10 ms windows) has fallen `belowDb` under `refDb` for good, in samples after `from`; -1 when it
        // has not by the end.
        long fallTime(const Stereo& s, std::size_t from, double refDb, double belowDb, double rate)
        {
            const std::size_t w = std::max<std::size_t>(1, static_cast<std::size_t>(rate * 0.01));
            long last = -1;                                  // the end of the last window above the threshold
            bool any = false;
            for (std::size_t i = from; i + w <= s.size(); i += w)
            {
                any = true;
                if (toDb(rms(s, i, i + w)) > refDb - belowDb)
                    last = static_cast<long>(i + w - from);
            }
            if (!any)
                return -1;
            if (last < 0)
                return 0;
            return last + static_cast<long>(w) <= static_cast<long>(s.size() - from) ? last : -1;
        }

        std::vector<float> mono(const Stereo& s, std::size_t from, std::size_t to)
        {
            std::vector<float> m;
            for (std::size_t i = from; i < to && i < s.size(); ++i)
                m.push_back(0.5f * (s.l[i] + s.r[i]));
            return m;
        }

        void fftInPlace(std::vector<std::complex<double>>& a)
        {
            const std::size_t n = a.size();
            for (std::size_t i = 1, j = 0; i < n; ++i)
            {
                std::size_t bit = n >> 1;
                for (; (j & bit) != 0; bit >>= 1)
                    j ^= bit;
                j ^= bit;
                if (i < j)
                    std::swap(a[i], a[j]);
            }
            for (std::size_t len = 2; len <= n; len <<= 1)
            {
                const double ang = -2.0 * std::numbers::pi / static_cast<double>(len);
                const std::complex<double> wl(std::cos(ang), std::sin(ang));
                for (std::size_t i = 0; i < n; i += len)
                {
                    std::complex<double> w(1.0, 0.0);
                    for (std::size_t k = 0; k < len / 2; ++k)
                    {
                        const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
                        a[i + k] = u + v;
                        a[i + k + len / 2] = u - v;
                        w *= wl;
                    }
                }
            }
        }

        std::string partials(const std::vector<Peak>& peaks)
        {
            std::string out;
            for (const Peak& p : peaks)
                out += (out.empty() ? "" : ", ") + fmt(p.hz >= 1000.0 ? "%.0f Hz" : "%.1f Hz", p.hz)
                     + fmt(" (%.0f dB)", p.db);
            return out.empty() ? std::string("none") : out;
        }

        std::string alertsText(uint32_t a)
        {
            if (a == 0)
                return "none";
            std::string out;
            if ((a & Engine::nonFinite) != 0)
                out += "NaN or infinite samples, replaced by 0 (a NaN stays in the program's state)";
            if ((a & Engine::clipped) != 0)
                out += std::string(out.empty() ? "" : "; ") + "samples beyond +-4 (+12 dBFS), clipped";
            return out;
        }

        // ---- the dials -------------------------------------------------------------------------------------------
        struct Dials
        {
            std::array<float, kMaxControls> norm{};
            std::string text;                                // "cutoff 1200, resonance 0.3"
            std::vector<std::string> unknown;                // names asked for that are no dial
        };

        Dials dialsFor(const Controls& controls, const Request& req)
        {
            Dials d;
            std::set<std::string> used;
            for (int c = 0; c < kMaxControls; ++c)
            {
                const Control& k = controls[static_cast<std::size_t>(c)];
                if (!k.used)
                    continue;
                float v = k.def;
                if (const auto it = req.dials.find(k.name); it != req.dials.end())
                {
                    v = it->second;
                    used.insert(k.name);
                }
                else if (const auto cur = req.current.find(k.name); cur != req.current.end())
                    v = cur->second;
                v = std::clamp(v, std::min(k.min, k.max), std::max(k.min, k.max));
                if (k.type != ControlType::real)
                    v = std::round(v);
                d.norm[static_cast<std::size_t>(c)] = normOf(k, v);
                d.text += (d.text.empty() ? "" : ", ") + std::string(k.name) + " " + fmt("%g", static_cast<double>(v));
            }
            for (const auto& [name, v] : req.dials)
                if (used.count(name) == 0)
                    d.unknown.push_back(name);
            if (d.text.empty())
                d.text = "none";
            return d;
        }

        // ---- rendering ---------------------------------------------------------------------------------------------
        struct Rendered
        {
            Stereo out;
            double cpuMs = 0.0;
            uint32_t alerts = 0;
            long endedAt = -1;                               // the sample after which no voice was left (-1: never)
        };

        double cpuShare(double ms, double audioSeconds)
        {
            return audioSeconds > 0.0 ? ms / (audioSeconds * 1000.0) * 100.0 : 0.0;
        }

        std::string cpuLine(double ms, double audioSeconds, int voices)
        {
            const double share = cpuShare(ms, audioSeconds);
            std::string s = "CPU: " + fmt("%.1f ms", ms) + " to render " + seconds(audioSeconds) + " ("
                          + fmt(share < 0.1 ? "%.3f" : "%.2f", share) + "% of one core in real time";
            if (kInstrument && voices > 0)
                s += fmt(", about %.2f%% for 16 voices at once", share / voices * 16.0);
            return s + ").";
        }

        // The peak of each window of w samples over [from, to), of either channel.
        [[maybe_unused]] std::vector<double> windowPeaks(const Stereo& s, std::size_t from, std::size_t to, std::size_t w)
        {
            std::vector<double> out;
            for (std::size_t a = from; w > 0 && a + w <= std::min(to, s.size()); a += w)
                out.push_back(peak(s, a, a + w));
            return out;
        }

        // The energy of x farther than six bins from hz (a four-term Blackman-Harris window over the largest power of
        // two that fits, up to 32768 samples) against all of it, in dB: a sine's THD+N (its harmonics, aliases and
        // noise); -240 when nothing is away from it.
        [[maybe_unused]] double awayDb(const std::vector<float>& x, double rate, double hz)
        {
            std::size_t n = 1;
            while (n * 2 <= x.size() && n < 32768)
                n *= 2;
            if (n < 256)
                return -240.0;
            const std::size_t off = (x.size() - n) / 2;
            std::vector<std::complex<double>> a(n);
            const double step = 2.0 * std::numbers::pi / static_cast<double>(n - 1);
            for (std::size_t i = 0; i < n; ++i)
            {
                const double t = step * static_cast<double>(i);
                const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2.0 * t) - 0.01168 * std::cos(3.0 * t);
                a[i] = { static_cast<double>(x[off + i]) * w, 0.0 };
            }
            fftInPlace(a);
            const double hzPerBin = rate / static_cast<double>(n);
            double all = 0.0, away = 0.0;
            for (std::size_t k = 1; k < n / 2; ++k)
            {
                const double e = std::norm(a[k]);
                all += e;
                if (std::fabs(static_cast<double>(k) * hzPerBin - hz) > 6.0 * hzPerBin)
                    away += e;
            }
            return all > 0.0 && away > 0.0 ? 10.0 * std::log10(away / all) : -240.0;
        }

        // ---- the metrics ---------------------------------------------------------------------------------------------
        constexpr double kInf = std::numeric_limits<double>::infinity();

        constexpr MetricInfo kInstrumentMetrics[] = {
            { "peak_db", "dBFS", "the held part's peak (the notes together)", 1.0, false, 0.0 },
            { "rms_db", "dBFS", "the held part's RMS level", 1.0, false, 0.0 },
            { "dc", "", "the held part's DC offset (its mean sample)", 0.002, false, 0.0 },
            { "correlation", "", "left against right while held: 1 mono, 0 unrelated, -1 out of phase", 0.05, false, 0.0 },
            { "balance_db", "dB", "left over right while held", 0.5, false, 0.0 },
            { "centroid_hz", "Hz", "the spectral centroid while held: how bright it is", 0.05, true, 1.0 },
            { "fundamental_hz", "Hz", "the lowest partial within 30 dB of the strongest while held (0: none)", 0.005, true, 0.5 },
            { "pitch_cents", "cents", "that partial against the lowest note's frequency", 5.0, false, 0.0 },
            { "inharmonic_db", "dB",
              "one note only: the strongest component below 20 kHz that is no harmonic of it, under the strongest "
              "(aliasing, or the sound's own detuning, FM or noise; -200: none above -90)",
              3.0, false, 0.0 },
            { "step", "", "the largest step between two samples", 0.01, false, 0.0 },
            { "click", "", "1 when that step looks like a click (far larger than the sound's own steps), else 0", 0.5, false, 0.0 },
            { "attack_s", "s", "from the note-on until its peaks (windows of a cycle) are within 3 dB of their loudest while held",
              0.1, true, 0.002 },
            { "decay_s", "s",
              "from its loudest moment until the level (10 ms windows) is 60 dB under it for good: a pluck's or a bell's "
              "decay (infinity: not by the end)",
              0.1, true, 0.002 },
            { "release_s", "s",
              "from the note-off until the level is 60 dB under the held level for good (infinity: not by the end)", 0.1,
              true, 0.002 },
            { "voice_end_s", "s", "from the note-off until the last voice ended (process returned false; infinity: never)",
              0.1, true, 0.002 },
            { "chord_peak_db", "dBFS",
              "one note only: the peak of a chord (the note, a third, a fifth and an octave above) at the same velocity", 1.0,
              false, 0.0 },
            { "alerts", "", "the output guard's alerts: 0 none, 1 NaN or infinite samples, 2 samples clipped, 3 both", 0.5,
              false, 0.0 },
            { "cpu_percent", "%", "the render's share of one core in real time", 0.1, true, 0.01 },
            { "cpu16_percent", "%", "about that for sixteen voices at once", 0.1, true, 0.01 },
        };

        constexpr MetricInfo kEffectMetrics[] = {
            { "sine_peak_db", "dBFS", "a 220 Hz sine's (-12 dBFS) output peak", 1.0, false, 0.0 },
            { "sine_rms_db", "dBFS", "its output RMS", 1.0, false, 0.0 },
            { "sine_gain_db", "dB", "its output RMS over its input's", 0.5, false, 0.0 },
            { "thd_db", "dB",
              "its output's energy away from 220 Hz (harmonics, aliases, noise) against all of it: THD+N, how much the "
              "effect distorts (-240: none)",
              1.0, false, 0.0 },
            { "dc", "", "the sine's output DC offset (its mean sample)", 0.002, false, 0.0 },
            { "correlation", "", "the sine's output, left against right: 1 mono, 0 unrelated, -1 out of phase", 0.05, false, 0.0 },
            { "balance_db", "dB", "the sine's output, left over right", 0.5, false, 0.0 },
            { "noise_gain_db", "dB", "noise's (-18 dBFS RMS) output RMS over its input's", 0.5, false, 0.0 },
            { "centroid_in_hz", "Hz", "the noise's spectral centroid going in", 0.05, true, 1.0 },
            { "centroid_out_hz", "Hz", "the noise's spectral centroid coming out: how bright the effect leaves it", 0.05, true, 1.0 },
            { "impulse_peak_db", "dBFS", "the impulse response's peak (the impulse is 0.5, -6 dBFS)", 1.0, false, 0.0 },
            { "impulse_delay_ms", "ms", "when the impulse response peaks", 0.05, true, 0.1 },
            { "impulse_decay_s", "s",
              "from that peak until the response is 60 dB under it for good (infinity: not by the end of the tail)", 0.1, true,
              0.002 },
            { "silence_db", "dBFS", "the output's RMS from silence in: self-oscillation, DC or noise of its own", 3.0, false, 0.0 },
            { "step", "", "the largest step between two samples, of any of the signals", 0.01, false, 0.0 },
            { "alerts", "", "the output guard's alerts: 0 none, 1 NaN or infinite samples, 2 samples clipped, 3 both", 0.5,
              false, 0.0 },
            { "cpu_percent", "%", "the renders' share of one core in real time", 0.1, true, 0.01 },
        };
    }

    void fft(std::vector<std::complex<double>>& a) { fftInPlace(a); }

    std::span<const MetricInfo> metrics()
    {
        if constexpr (kInstrument)
            return kInstrumentMetrics;
        else
            return kEffectMetrics;
    }

    const MetricInfo* metricInfo(std::string_view name)
    {
        for (const MetricInfo& m : metrics())
            if (name == m.name)
                return &m;
        return nullptr;
    }

    std::string valueText(std::string_view metric, double v)
    {
        if (std::isinf(v))
            return v > 0.0 ? "never" : "-infinity";
        if (std::isnan(v))
            return "not a number";
        const MetricInfo* m = metricInfo(metric);
        const std::string unit = m != nullptr ? m->unit : "";
        if (unit == "dBFS")
            return v <= -150.0 ? std::string("silent") : fmt("%.1f dBFS", v);
        if (unit == "dB")
            return v <= -199.0 ? std::string("none") : fmt("%.1f dB", v);
        if (unit == "Hz")
            return fmt(std::fabs(v) >= 1000.0 ? "%.0f Hz" : "%.1f Hz", v);
        if (unit == "s")
            return fmt(std::fabs(v) < 0.1 ? "%.3f s" : "%.2f s", v);
        if (unit == "ms")
            return fmt("%.1f ms", v);
        if (unit == "cents")
            return fmt("%+.1f cents", v);
        if (unit == "%")
            return fmt(std::fabs(v) < 0.1 ? "%.3f%%" : "%.2f%%", v);
        return fmt("%.3g", v);
    }

    bool Request::sameRender(const Request& o) const
    {
        return notes == o.notes && velocity == o.velocity && holdSeconds == o.holdSeconds && tailSeconds == o.tailSeconds
            && dials == o.dials && current == o.current && rate == o.rate && bpm == o.bpm && modwheel == o.modwheel
            && pitchbend == o.pitchbend;
    }

    std::vector<Peak> spectrumPeaks(const std::vector<float>& x, double rate, int count, double* centroidHz)
    {
        std::size_t n = 1;
        while (n * 2 <= x.size() && n < 16384)
            n *= 2;
        if (centroidHz != nullptr)
            *centroidHz = 0.0;
        if (n < 64)
            return {};
        const std::size_t off = (x.size() - n) / 2;          // the middle of what was given
        std::vector<std::complex<double>> a(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n - 1));
            a[i] = { static_cast<double>(x[off + i]) * w, 0.0 };
        }
        fftInPlace(a);
        const std::size_t bins = n / 2;
        std::vector<double> mag(bins);
        double top = 0.0, sum = 0.0, weighted = 0.0;
        for (std::size_t k = 0; k < bins; ++k)
        {
            mag[k] = std::abs(a[k]);
            top = std::max(top, mag[k]);
            if (k > 0)
            {
                sum += mag[k];
                weighted += mag[k] * static_cast<double>(k) * rate / static_cast<double>(n);
            }
        }
        if (centroidHz != nullptr && sum > 0.0)
            *centroidHz = weighted / sum;
        if (!(top > 1e-9))
            return {};
        std::vector<Peak> found;
        for (std::size_t k = 2; k + 2 < bins; ++k)
        {
            const double m = mag[k];
            if (m < top * 0.01 || m < mag[k - 1] || m < mag[k + 1] || m < mag[k - 2] || m < mag[k + 2])
                continue;                                    // 40 dB under the strongest, or not a local maximum
            // Parabolic interpolation on the log magnitude.
            const double y0 = std::log(mag[k - 1] + 1e-30), y1 = std::log(m + 1e-30), y2 = std::log(mag[k + 1] + 1e-30);
            const double den = y0 - 2.0 * y1 + y2;
            const double d = den != 0.0 ? 0.5 * (y0 - y2) / den : 0.0;
            found.push_back({ (static_cast<double>(k) + std::clamp(d, -0.5, 0.5)) * rate / static_cast<double>(n),
                              toDb(m / top) });
        }
        std::sort(found.begin(), found.end(), [](const Peak& x0, const Peak& x1) { return x0.db > x1.db; });
        if (found.size() > static_cast<std::size_t>(std::max(0, count)))
            found.resize(static_cast<std::size_t>(std::max(0, count)));
        std::sort(found.begin(), found.end(), [](const Peak& x0, const Peak& x1) { return x0.hz < x1.hz; });
        return found;
    }

    Peak inharmonic(const std::vector<float>& x, double rate, double f0, double maxHz)
    {
        std::size_t n = 1;
        while (n * 2 <= x.size() && n < 32768)
            n *= 2;
        Peak worst{ 0.0, -200.0 };
        if (n < 256 || !(f0 > 0.0))
            return worst;
        const std::size_t off = (x.size() - n) / 2;
        std::vector<std::complex<double>> a(n);
        const double step = 2.0 * std::numbers::pi / static_cast<double>(n - 1);
        for (std::size_t i = 0; i < n; ++i)
        {
            const double t = step * static_cast<double>(i);
            const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2.0 * t) - 0.01168 * std::cos(3.0 * t);
            a[i] = { static_cast<double>(x[off + i]) * w, 0.0 };
        }
        fftInPlace(a);
        const std::size_t bins = n / 2;
        const double hzPerBin = rate / static_cast<double>(n);
        std::vector<double> mag(bins);
        double top = 0.0;
        for (std::size_t k = 0; k < bins; ++k)
            top = std::max(top, mag[k] = std::abs(a[k]));
        if (!(top > 1e-12))
            return worst;
        const std::size_t last = std::min(bins - 2, static_cast<std::size_t>(std::min(maxHz, 0.5 * rate) / hzPerBin));
        std::size_t at = 0;
        for (std::size_t k = std::max<std::size_t>(2, static_cast<std::size_t>(20.0 / hzPerBin)); k <= last; ++k)
        {
            const double hz = static_cast<double>(k) * hzPerBin;
            const double h = std::max(1.0, std::round(hz / f0));         // the nearest harmonic (below f0: f0)
            if (std::fabs(hz - h * f0) <= 6.0 * hzPerBin)
                continue;                                                // within a harmonic's main lobe
            if (at == 0 || mag[k] > mag[at])
                at = k;
        }
        if (at == 0 || !(mag[at] > 0.0))
            return worst;
        const double y0 = std::log(mag[at - 1] + 1e-300), y1 = std::log(mag[at] + 1e-300), y2 = std::log(mag[at + 1] + 1e-300);
        const double den = y0 - 2.0 * y1 + y2;
        const double d = den != 0.0 ? std::clamp(0.5 * (y0 - y2) / den, -0.5, 0.5) : 0.0;
        worst.hz = (static_cast<double>(at) + d) * hzPerBin;
        worst.db = toDb(mag[at] / top);
        return worst;
    }

    // ---- the session ----------------------------------------------------------------------------------------------------
    struct Session::Impl
    {
        Engine engine;
        Controls controls{};
        bool host = false;                                   // the program reads the Host block
        bool ok = false;

        // The program as new over `in` (an effect) or `total` samples (an instrument, `events` by absolute sample).
        Rendered render(const Dials& dials, const Stereo* in, std::size_t total,
                        const std::vector<std::pair<std::size_t, Event>>& events, std::size_t watchFrom,
                        const Request& request)
        {
            Rendered r;
            engine.restart();
            engine.takeAlerts();
            Transport t;                                     // the host plays from the song's start, at the tempo asked
            t.known = true;
            t.bpm = std::clamp(static_cast<double>(request.bpm), 20.0, 400.0);
            t.playing = true;
            engine.setTransport(t);
            r.out.l.assign(total, 0.0f);
            r.out.r.assign(total, 0.0f);
            if (in != nullptr)
            {
                std::copy_n(in->l.begin(), std::min(total, in->size()), r.out.l.begin());
                std::copy_n(in->r.begin(), std::min(total, in->size()), r.out.r.begin());
            }
            std::vector<Event> block;
            std::size_t next = 0;
            for (std::size_t pos = 0; pos < total;)
            {
                const int n = static_cast<int>(std::min<std::size_t>(kBlock, total - pos));
                block.clear();
                while (next < events.size() && events[next].first < pos + static_cast<std::size_t>(n))
                {
                    Event ev = events[next].second;
                    ev.offset = static_cast<uint32_t>(events[next].first - pos);
                    block.push_back(ev);
                    ++next;
                }
                float* ch[2] = { r.out.l.data() + pos, r.out.r.data() + pos };
                const auto t0 = std::chrono::steady_clock::now();
                engine.setControls(dials.norm);
                engine.process(ch, 2, n, block);
                r.cpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                pos += static_cast<std::size_t>(n);
                if (kInstrument && r.endedAt < 0 && pos > watchFrom && engine.activeVoices() == 0)
                    r.endedAt = static_cast<long>(pos);
            }
            r.alerts = engine.takeAlerts();
            engine.collect();
            return r;
        }
    };

    namespace
    {
        // An instrument's notes as a request asks for them (default one, C3), and its events: the wheels, the notes
        // together at the start, their offs at `offAt`.
        [[maybe_unused]] std::vector<int> notesOf(const Request& request)
        {
            std::vector<int> notes;
            for (const int n : request.notes)
                if (n >= 0 && n <= 127 && std::find(notes.begin(), notes.end(), n) == notes.end()
                    && notes.size() < static_cast<std::size_t>(Patch::kVoices))
                    notes.push_back(n);
            if (notes.empty())
                notes.push_back(48);
            return notes;
        }

        [[maybe_unused]] std::vector<std::pair<std::size_t, Event>> eventsFor(const Request& request,
                                                                              const std::vector<int>& ns, std::size_t offAt)
        {
            const float vel = std::clamp(request.velocity, 0.0f, 1.0f);
            std::vector<std::pair<std::size_t, Event>> ev;
            // The wheels where the request puts them, before the notes.
            ev.push_back({ 0, Event{ 0, EventKind::controller, 1, std::clamp(request.modwheel, 0.0f, 1.0f) } });
            ev.push_back({ 0, Event{ 0, EventKind::pitchBend, 0, std::clamp(request.pitchbend, -1.0f, 1.0f) } });
            for (const int n : ns)
                ev.push_back({ 0, Event{ 0, EventKind::noteOn, static_cast<uint8_t>(n), vel } });
            for (const int n : ns)
                ev.push_back({ offAt, Event{ 0, EventKind::noteOff, static_cast<uint8_t>(n), 0.0f } });
            return ev;
        }
    }

    Session::Session(const Sources& files, double rate)
        : impl_(std::make_unique<Impl>()), rate_(rate > 1000.0 ? rate : 48000.0)
    {
        Patch::Result r = Patch::compile(files, static_cast<float>(rate_));
        if (!r.patch)
        {
            errors_ = r.errors + r.notes;
            return;
        }
        impl_->controls = r.patch->controls();
        impl_->host = r.patch->host();
        impl_->engine.prepare(rate_, kBlock);
        impl_->engine.offer(std::move(r.patch));
        impl_->engine.prepare(rate_, kBlock);                // takes it at once, without a fade
        impl_->ok = true;
    }

    Session::~Session() = default;

    bool Session::compiled() const noexcept { return impl_->ok; }
    const Controls& Session::controls() const noexcept { return impl_->controls; }

    Session::Render Session::render(const Request& request, const std::vector<float>* inputL,
                                    const std::vector<float>* inputR, std::size_t length)
    {
        Render out;
        if (!impl_->ok)
            return out;
        const Dials dials = dialsFor(impl_->controls, request);
        out.dials = dials.text;
        Rendered r;
        if constexpr (kInstrument)
        {
            const std::size_t holdN = static_cast<std::size_t>(std::clamp(request.holdSeconds, 0.05f, 10.0f) * rate_);
            r = impl_->render(dials, nullptr, length, eventsFor(request, notesOf(request), holdN), holdN, request);
        }
        else
        {
            Stereo in;
            in.l = inputL != nullptr ? *inputL : std::vector<float>(length, 0.0f);
            in.r = inputR != nullptr ? *inputR : in.l;
            in.l.resize(length, 0.0f);
            in.r.resize(length, 0.0f);
            r = impl_->render(dials, &in, length, {}, 0, request);
        }
        out.l = std::move(r.out.l);
        out.r = std::move(r.out.r);
        out.alerts = r.alerts;
        return out;
    }

    Result Session::measure(const Request& request)
    {
        Result res;
        if (!impl_->ok)
        {
            res.report = "It did not compile, so nothing was rendered:\n" + errors_;
            return res;
        }
        res.compiled = true;
        const double rate = rate_;
        const float hold = std::clamp(request.holdSeconds, 0.05f, 10.0f);
        const float tail = std::clamp(request.tailSeconds, 0.0f, 10.0f);
        const bool all = request.want.empty();
        const auto need = [&](std::initializer_list<const char*> names) {
            if (all)
                return true;
            for (const char* n : names)
                if (request.want.count(n) != 0)
                    return true;
            return false;
        };
        Metrics& v = res.values;
        const Dials dials = dialsFor(impl_->controls, request);
        res.dials = dials.text;
        res.unknown = dials.unknown;

        std::string out = "Rendered offline at " + fmt("%.0f", rate) + " Hz, in blocks of " + std::to_string(kBlock)
                        + ". Dials: " + dials.text + ".\n";
        if (impl_->host)
            out += "Host: playing from the song's start at " + fmt("%.0f", std::clamp(request.bpm, 20.0f, 400.0f)) + " BPM"
                 + (kInstrument ? ", mod wheel " + fmt("%.2f", std::clamp(request.modwheel, 0.0f, 1.0f))
                                  + ", pitch bend " + fmt("%.2f", std::clamp(request.pitchbend, -1.0f, 1.0f))
                                : std::string())
                 + ".\n";
        if (!dials.unknown.empty())
        {
            out += "No dial is named";
            for (const std::string& u : dials.unknown)
                out += " '" + u + "'";
            out += " (they were ignored).\n";
        }
        const std::size_t holdN = static_cast<std::size_t>(hold * rate);
        const std::size_t tailN = static_cast<std::size_t>(tail * rate);
        std::vector<std::string> warnings;

        if constexpr (kInstrument)
        {
            const std::vector<int> notes = notesOf(request);
            const float vel = std::clamp(request.velocity, 0.0f, 1.0f);
            const std::size_t total = holdN + tailN;
            const Rendered r = impl_->render(dials, nullptr, total, eventsFor(request, notes, holdN), holdN, request);

            std::string what;
            for (const int n : notes)
                what += (what.empty() ? "" : " ") + std::to_string(n) + " (" + noteName(n) + ", "
                      + fmt("%.1f Hz", noteHz(n)) + ")";
            out += (notes.size() == 1 ? "Note " : "Notes ") + what + " at velocity " + fmt("%.2f", vel) + ", held "
                 + seconds(hold) + ", then released for " + seconds(tail) + ".\n";

            const std::size_t settle = std::min(holdN / 2, static_cast<std::size_t>(0.05 * rate));
            const double heldPeak = peak(r.out, 0, holdN), heldRms = rms(r.out, settle, holdN);
            double corr = 1.0, bal = 0.0;
            stereo(r.out, settle, holdN, corr, bal);
            const double heldDc = dc(r.out, settle, holdN);
            v["peak_db"] = toDb(heldPeak);
            v["rms_db"] = toDb(heldRms);
            v["dc"] = heldDc;
            v["correlation"] = corr;
            v["balance_db"] = bal;
            out += "Held: peak " + dbfs(heldPeak) + ", RMS " + dbfs(heldRms) + ", DC " + fmt("%.4f", heldDc)
                 + ", left/right correlation " + fmt("%.2f", corr) + fmt(" (balance %+.1f dB).\n", bal);
            if (heldRms < 1e-5)
                warnings.push_back("It is silent (or nearly) while the note is held.");
            if (heldPeak > 1.0)
                warnings.push_back("Peaks above 0 dBFS with " + std::to_string(notes.size())
                                   + " note(s): a voice should peak near 0.2 to 0.3, since sixteen can sound at once.");

            const double lowestHz = noteHz(*std::min_element(notes.begin(), notes.end()));
            if (need({ "centroid_hz", "fundamental_hz", "pitch_cents" }))
            {
                double centroid = 0.0;
                const std::vector<Peak> peaks = spectrumPeaks(mono(r.out, settle, holdN), rate, 8, &centroid);
                v["centroid_hz"] = centroid;
                out += "Spectrum while held: centroid " + fmt("%.0f Hz", centroid) + "; strongest partials "
                     + partials(peaks) + ".";
                const Peak* lowest = nullptr;
                for (const Peak& p : peaks)
                    if (p.db > -30.0 && p.hz > 15.0)
                    {
                        lowest = &p;
                        break;
                    }
                v["fundamental_hz"] = lowest != nullptr ? lowest->hz : 0.0;
                if (lowest != nullptr)
                {
                    v["pitch_cents"] = 1200.0 * std::log2(lowest->hz / lowestHz);
                    out += " The lowest within 30 dB of the strongest is " + fmt("%.1f Hz", lowest->hz)
                         + "; the lowest note is " + fmt("%.1f Hz", lowestHz) + ".";
                }
                out += "\n";
            }

            if (need({ "step", "click" }))
            {
                std::size_t at = 0;
                const double step = largestStep(r.out, at);
                // A wave's own edges come every cycle: windows of a cycle and a half of the lowest note, at least 10 ms.
                const std::size_t window = static_cast<std::size_t>(std::max(0.01 * rate, 1.5 * rate / lowestHz));
                const double usual = usualStep(r.out, settle, holdN, window);
                const bool click = heldPeak > 0.0 && step > 0.35 * heldPeak && step > 0.05 && step > 2.0 * usual;
                v["step"] = step;
                v["click"] = click ? 1.0 : 0.0;
                out += "Largest step between two samples: " + fmt("%.3f", step) + " at "
                     + seconds(static_cast<double>(at) / rate)
                     + (heldPeak > 0.0 ? fmt(" (%.0f%% of the held peak", step / heldPeak * 100.0) : std::string(" ("))
                     + (heldPeak > 0.0 ? "; " : "") + "the held sound's own largest steps, every cycle: "
                     + fmt("%.3f", usual) + ").\n";
                if (click)
                    warnings.push_back("A step that large, and far larger than the sound's own, is likely a click (an "
                                       "envelope or a phase that jumps).");
            }

            // The envelope: its peaks in windows of a cycle of the lowest note (at least 1 ms) while held, and the level
            // (10 ms windows) from its loudest moment on.
            {
                const std::size_t w = std::max<std::size_t>(static_cast<std::size_t>(0.001 * rate),
                                                            static_cast<std::size_t>(rate / lowestHz));
                const std::vector<double> pk = windowPeaks(r.out, 0, holdN, w);
                double attack = kInf;
                if (!pk.empty())
                {
                    const double top = *std::max_element(pk.begin(), pk.end());
                    for (std::size_t k = 0; k < pk.size() && top > 1e-9; ++k)
                        if (pk[k] >= top * 0.70794578)              // -3 dB
                        {
                            attack = (static_cast<double>(k) + 0.5) * static_cast<double>(w) / rate;
                            break;
                        }
                }
                const std::size_t w10 = std::max<std::size_t>(1, static_cast<std::size_t>(0.01 * rate));
                std::size_t loudAt = 0;
                double loud = 0.0;
                for (std::size_t i = 0; i + w10 <= r.out.size(); i += w10)
                    if (const double x = rms(r.out, i, i + w10); x > loud)
                    {
                        loud = x;
                        loudAt = i;
                    }
                double decay = kInf;
                if (loud > 1e-9)
                {
                    const long fall = fallTime(r.out, loudAt, toDb(loud), 60.0, rate);
                    if (fall >= 0)
                        decay = static_cast<double>(fall) / rate;
                }
                v["attack_s"] = attack;
                v["decay_s"] = decay;
                if (heldRms >= 1e-5)
                    out += "Envelope: its peaks come within 3 dB of their loudest " + valueText("attack_s", attack)
                         + " after the note-on; from its loudest moment it falls 60 dB in "
                         + (std::isinf(decay) ? std::string("more than the render") : valueText("decay_s", decay))
                         + ".\n";
            }

            const long fall = fallTime(r.out, holdN, toDb(heldRms), 60.0, rate);
            v["release_s"] = heldRms >= 1e-5 && fall >= 0 ? static_cast<double>(fall) / rate : heldRms < 1e-5 ? 0.0 : kInf;
            v["voice_end_s"] = r.endedAt >= 0 ? static_cast<double>(r.endedAt - static_cast<long>(holdN)) / rate : kInf;
            out += "Release: ";
            if (heldRms < 1e-5)
                out += "nothing to release.";
            else if (fall < 0)
                out += "still within 60 dB of the held level " + seconds(tail) + " after the note-off.";
            else
                out += "falls 60 dB below the held level " + seconds(static_cast<double>(fall) / rate) + " after the note-off.";
            if (r.endedAt >= 0)
                out += " The last voice ended " + seconds(static_cast<double>(r.endedAt - static_cast<long>(holdN)) / rate)
                     + " after it (process returned false).\n";
            else
            {
                out += " No voice ended within " + seconds(tail) + " of the note-off: process kept returning true.\n";
                warnings.push_back("A voice that never returns false holds its slot forever: let the release end (or "
                                   "render with a longer tail_seconds if the release is meant to be that long).");
            }

            if (notes.size() == 1)
            {
                const int n = notes.front();
                if (need({ "inharmonic_db" }))
                {
                    // What is no harmonic of the note: aliasing, unless the sound means it.
                    const Peak odd = inharmonic(mono(r.out, settle, holdN), rate, noteHz(n), 20000.0);
                    v["inharmonic_db"] = odd.db;
                    if (heldRms >= 1e-5)
                    {
                        out += "Inharmonic: ";
                        if (odd.db < -90.0)
                            out += "nothing below 20 kHz that is no harmonic of the note, down to -90 dB under the "
                                   "strongest component (the measurement's floor).\n";
                        else
                            out += "the strongest component below 20 kHz that is no harmonic of the note is "
                                 + fmt("%.0f dB", odd.db) + " under the strongest, at " + fmt("%.0f Hz", odd.hz)
                                 + ": aliasing, or the sound's own (detuning, FM, noise). To test an oscillator's "
                                   "aliasing, measure a high note (96 and up) with its detune and noise at 0.\n";
                    }
                }
                if (need({ "chord_peak_db" }))
                {
                    std::vector<int> chord{ n };
                    for (const int k : { 4, 7, 12 })
                        if (n + k <= 127)
                            chord.push_back(n + k);
                    const std::size_t chordN = std::min(holdN, static_cast<std::size_t>(0.6 * rate));
                    const Rendered c = impl_->render(dials, nullptr, chordN, eventsFor(request, chord, chordN), chordN,
                                                     request);
                    std::string names;
                    for (const int k : chord)
                        names += (names.empty() ? "" : " ") + std::to_string(k);
                    const double cp = peak(c.out, 0, chordN);
                    v["chord_peak_db"] = toDb(cp);
                    out += "A chord (" + names + ") at the same velocity: peak " + dbfs(cp) + ", RMS "
                         + dbfs(rms(c.out, std::min(chordN / 2, settle), chordN)) + ".\n";
                    if (cp > 1.0)
                        warnings.push_back("The chord peaks above 0 dBFS: lower the voice's level.");
                }
            }
            v["alerts"] = static_cast<double>(r.alerts & 3u);
            const double share = cpuShare(r.cpuMs, static_cast<double>(total) / rate);
            v["cpu_percent"] = share;
            v["cpu16_percent"] = share / static_cast<double>(notes.size()) * 16.0;
            out += "Alerts: " + alertsText(r.alerts) + ".\n";
            out += cpuLine(r.cpuMs, static_cast<double>(total) / rate, static_cast<int>(notes.size())) + "\n";
        }
        else
        {
            // The test signals, each through the program as new.
            const auto signal = [&](std::size_t n, const auto& fill) {
                Stereo s;
                s.l.resize(n);
                s.r.resize(n);
                for (std::size_t i = 0; i < n; ++i)
                    s.l[i] = s.r[i] = fill(i);
                return s;
            };
            const bool everything = need({ "step", "alerts", "cpu_percent" });   // of every signal
            const float sineAmp = static_cast<float>(std::pow(10.0, -12.0 / 20.0));
            double cpu = 0.0, audio = 0.0;
            uint32_t alerts = 0;
            double worstStep = 0.0, worstStepAt = 0.0;
            std::string worstStepWhere;
            const auto through = [&](const Stereo& in, const char* name) {
                Rendered r = impl_->render(dials, &in, in.size(), {}, 0, request);
                cpu += r.cpuMs;
                audio += static_cast<double>(in.size()) / rate;
                alerts |= r.alerts;
                std::size_t at = 0;
                const double s = largestStep(r.out, at);
                if (s > worstStep || worstStepWhere.empty())
                {
                    worstStep = s;
                    worstStepAt = static_cast<double>(at) / rate;
                    worstStepWhere = name;
                }
                return std::move(r.out);
            };

            const std::size_t settle = std::min(holdN / 2, static_cast<std::size_t>(0.05 * rate));
            if (everything || need({ "sine_peak_db", "sine_rms_db", "sine_gain_db", "thd_db", "dc", "correlation", "balance_db" }))
            {
                const Stereo sine = signal(holdN, [&](std::size_t i) {
                    return sineAmp * static_cast<float>(std::sin(2.0 * std::numbers::pi * 220.0 * static_cast<double>(i) / rate));
                });
                const Stereo o = through(sine, "the sine");
                const double inRms = rms(sine, settle, holdN), outRms = rms(o, settle, holdN);
                double corr = 1.0, bal = 0.0;
                stereo(o, settle, holdN, corr, bal);
                const double outDc = dc(o, settle, holdN);
                const double thd = outRms > 1e-9 ? awayDb(mono(o, settle, holdN), rate, 220.0) : -240.0;
                v["sine_peak_db"] = toDb(peak(o, 0, holdN));
                v["sine_rms_db"] = toDb(outRms);
                v["sine_gain_db"] = outRms > 1e-12 ? toDb(outRms / inRms) : -240.0;
                v["thd_db"] = thd;
                v["dc"] = outDc;
                v["correlation"] = corr;
                v["balance_db"] = bal;
                out += "Sine 220 Hz at -12.0 dBFS for " + seconds(hold) + ": out peak " + dbfs(peak(o, 0, holdN)) + ", RMS "
                     + dbfs(outRms) + (outRms > 1e-9 ? fmt(" (gain %+.1f dB)", toDb(outRms / inRms)) : std::string())
                     + ", DC " + fmt("%.4f", outDc) + ", left/right correlation " + fmt("%.2f", corr)
                     + fmt(" (balance %+.1f dB)", bal) + ". Partials out: "
                     + partials(spectrumPeaks(mono(o, settle, holdN), rate, 8))
                     + (outRms > 1e-9 ? "; THD+N (all but 220 Hz) " + valueText("thd_db", thd) : std::string()) + ".\n";
                if (outRms < 1e-5)
                    warnings.push_back("The sine comes out silent (or nearly).");
                if (peak(o, 0, holdN) > 1.0)
                    warnings.push_back("Peaks above 0 dBFS from a -12 dBFS sine: the effect adds a lot of gain.");
            }
            if (everything || need({ "noise_gain_db", "centroid_in_hz", "centroid_out_hz" }))
            {
                uint32_t seed = 22222;                       // a fixed noise: the same report each time
                const float noiseAmp = static_cast<float>(std::pow(10.0, -18.0 / 20.0) * std::sqrt(3.0));
                const Stereo noise = signal(holdN, [&](std::size_t) {
                    seed = seed * 1664525u + 1013904223u;
                    return noiseAmp * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
                });
                const Stereo o = through(noise, "the noise");
                const double inRms = rms(noise, settle, holdN), outRms = rms(o, settle, holdN);
                double corr = 1.0, bal = 0.0;
                stereo(o, settle, holdN, corr, bal);
                double cin = 0.0, cout = 0.0;
                spectrumPeaks(mono(noise, settle, holdN), rate, 0, &cin);
                spectrumPeaks(mono(o, settle, holdN), rate, 0, &cout);
                v["noise_gain_db"] = outRms > 1e-12 ? toDb(outRms / inRms) : -240.0;
                v["centroid_in_hz"] = cin;
                v["centroid_out_hz"] = cout;
                out += "Noise at -18.0 dBFS RMS for " + seconds(hold) + ": out RMS " + dbfs(outRms)
                     + (outRms > 1e-9 ? fmt(" (gain %+.1f dB)", toDb(outRms / inRms)) : std::string())
                     + ", left/right correlation " + fmt("%.2f", corr) + "; spectral centroid " + fmt("%.0f Hz", cin)
                     + " in, " + fmt("%.0f Hz", cout) + " out.\n";
            }
            if (everything || need({ "impulse_peak_db", "impulse_delay_ms", "impulse_decay_s" }))
            {
                const std::size_t impulseN = static_cast<std::size_t>(0.01 * rate) + tailN;
                const Stereo impulse = signal(impulseN, [&](std::size_t i) { return i == 0 ? 0.5f : 0.0f; });
                const Stereo o = through(impulse, "the impulse");
                std::size_t at = 0;
                double pk = 0.0;
                for (std::size_t i = 0; i < o.size(); ++i)
                {
                    const double x = std::max(std::fabs(static_cast<double>(o.l[i])), std::fabs(static_cast<double>(o.r[i])));
                    if (x > pk)
                    {
                        pk = x;
                        at = i;
                    }
                }
                out += "An impulse (0.5), then " + seconds(tail) + " of silence: ";
                v["impulse_peak_db"] = toDb(pk);
                if (pk < 1e-7)
                {
                    v["impulse_delay_ms"] = kInf;
                    v["impulse_decay_s"] = 0.0;
                    out += "no response.\n";
                }
                else
                {
                    const long fall = fallTime(o, at, toDb(pk), 60.0, rate);
                    v["impulse_delay_ms"] = static_cast<double>(at) / rate * 1000.0;
                    v["impulse_decay_s"] = fall < 0 ? kInf : static_cast<double>(fall) / rate;
                    out += "the response peaks at " + dbfs(pk) + " after " + fmt("%.1f ms", static_cast<double>(at) / rate * 1000.0)
                         + (fall < 0 ? "; still within 60 dB of that peak at the end (a long tail, or one that does not decay)"
                                     : "; it falls 60 dB below that peak " + seconds(static_cast<double>(fall) / rate) + " later")
                         + ".\n";
                }
            }
            if (everything || need({ "silence_db" }))
            {
                const std::size_t silenceN = static_cast<std::size_t>(0.5 * rate);
                const Stereo silence = signal(silenceN, [](std::size_t) { return 0.0f; });
                const Stereo o = through(silence, "silence");
                const double r = rms(o, 0, silenceN);
                v["silence_db"] = toDb(r);
                out += "Silence in, " + seconds(0.5) + ": out RMS " + dbfs(r) + ".\n";
                if (r > 1e-4)
                    warnings.push_back("It makes sound from silence (self-oscillation, DC or noise of its own).");
            }
            if (everything)
            {
                v["step"] = worstStep;
                v["alerts"] = static_cast<double>(alerts & 3u);
                v["cpu_percent"] = cpuShare(cpu, audio);
                out += "Largest step between two samples: " + fmt("%.3f", worstStep) + " (in " + worstStepWhere + ", at "
                     + seconds(worstStepAt) + ").\n";
                out += "Alerts: " + alertsText(alerts) + ".\n";
                out += cpuLine(cpu, audio, 0) + "\n";
            }
        }
        for (const std::string& w : warnings)
            out += "Note: " + w + "\n";
        res.report = std::move(out);
        return res;
    }

    std::string run(const Sources& files, const Request& request)
    {
        Session session(files, request.rate);
        return session.measure(request).report;
    }
}
