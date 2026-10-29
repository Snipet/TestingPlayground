// Source/dsp/Measure.cpp: see Measure.h.
#include "dsp/Measure.h"

#include "dsp/Controls.h"
#include "dsp/Engine.h"
#include "dsp/Events.h"
#include "fminus/Patch.h"

#include "Product.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <memory>
#include <numbers>
#include <set>

#include <time.h>

namespace playground::measure
{
    namespace
    {
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

        // The click locator's line: the largest steps, each with its time and its nearest moment.
        [[maybe_unused]] std::string stepsText(const std::vector<Step>& steps, const Timeline& tl, double rate, const char* where = nullptr)
        {
            std::string out;
            for (const Step& st : steps)
            {
                char buf[48];
                std::snprintf(buf, sizeof buf, "%.3f at %.3f s", st.size, static_cast<double>(st.at) / rate);
                out += (out.empty() ? "" : "; ") + std::string(buf) + (where != nullptr ? std::string(" in ") + where : "")
                     + ", " + nearestMark(tl, st.at, rate);
            }
            return out.empty() ? std::string("none (silent)") : out;
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
            std::array<float, kMaxControls> plain{};         // the same, in the parameters' units
            std::string text;                                // "cutoff 1200, resonance 0.3"
            std::vector<std::pair<std::string, float>> values;   // the same, by name
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
                d.plain[static_cast<std::size_t>(c)] = v;
                d.values.emplace_back(k.name, v);
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
        // The CPU figures are this thread's own time (CLOCK_THREAD_CPUTIME_ID), read at a render's few boundaries: what
        // the render cost, however often the thread was put aside meanwhile (the plugin playing, other apps), which a
        // wall clock would count. The Engine runs as the audio thread runs it with no editor open: flush-to-zero, no
        // meters. It times the program's own code with its dials held still (render() sets the same values every
        // block, so no dial glides): every call is flat (Engine.h), FMinus's flat copy of each method, with what it
        // computes from a dial lifted out of its loop. A dial that moves plays the stream copy, which costs more; once
        // the dials have held still a while, the plugin plays the frozen copy (Patch.h), usually cheaper.
        double threadMs()
        {
            timespec ts{};
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
            return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1.0e6;
        }

        struct Rendered
        {
            Stereo out;
            double cpuMs = 0.0;                              // the CPU time of the samples timed (render(): timeFrom
            std::size_t cpuSamples = 0;                      // to timeUntil), and how many they were
            uint32_t alerts = 0;
            long endedAt = -1;                               // the sample after which no voice was left (-1: never)
            std::vector<int> voicesAt;                       // per timeline event: the voices sounding as a note-on's
                                                             // call started (-1 for the others)
        };

        double cpuShare(double ms, double audioSeconds)
        {
            return audioSeconds > 0.0 ? ms / (audioSeconds * 1000.0) * 100.0 : 0.0;
        }

        std::string percent(double share) { return fmt(share < 0.1 ? "%.3f%%" : "%.2f%%", share); }

        constexpr const char* kCpuWhat = "the rendering thread's own time (not a wall clock's) in the plugin's engine, "
                                         "running the program's own code with its dials held still (what it computes "
                                         "from a dial is lifted out of its loop; while a dial moves it is computed per "
                                         "sample, which costs more; once the dials have held still a while the plugin "
                                         "plays a frozen copy, usually cheaper)";
        constexpr const char* kCpuMovingWhat = "the rendering thread's own time (not a wall clock's) in the plugin's engine, "
                                               "running the program's own code while the scenario moves its dials (a dial that "
                                               "moves is computed per sample, which costs more than one held still)";

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
            { "cpu_percent", "%",
              "the notes' share of one core in real time while held, the program's own code, dials still", 0.1, true,
              0.01 },
            { "cpu16_percent", "%", "the share of sixteen voices at once, measured with sixteen notes held", 0.1, true,
              0.01 },
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
            { "cpu_percent", "%", "the renders' share of one core in real time, the program's own code, dials still", 0.1,
              true, 0.01 },
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
        const auto sameEvent = [](const std::pair<std::size_t, Event>& a, const std::pair<std::size_t, Event>& b) {
            return a.first == b.first && a.second.kind == b.second.kind && a.second.note == b.second.note
                && a.second.velocity == b.second.velocity;
        };
        const auto sameTransport = [](const std::pair<std::size_t, Transport>& a, const std::pair<std::size_t, Transport>& b) {
            const Transport &x = a.second, &y = b.second;
            return a.first == b.first && x.known == y.known && x.bpm == y.bpm && x.beat == y.beat && x.barStart == y.barStart
                && x.beatsPerBar == y.beatsPerBar && x.beatUnit == y.beatUnit && x.playing == y.playing;
        };
        return notes == o.notes && velocity == o.velocity && holdSeconds == o.holdSeconds && tailSeconds == o.tailSeconds
            && dials == o.dials && current == o.current && rate == o.rate && bpm == o.bpm && modwheel == o.modwheel
            && pitchbend == o.pitchbend && block == o.block && input == o.input && signal == o.signal
            && std::equal(phrase.begin(), phrase.end(), o.phrase.begin(), o.phrase.end(),
                          [](const PhraseNote& a, const PhraseNote& b) {
                              return a.note == b.note && a.at == b.at && a.length == b.length && a.velocity == b.velocity;
                          })
            && std::equal(automate.begin(), automate.end(), o.automate.begin(), o.automate.end(),
                          [](const DialMove& a, const DialMove& b) {
                              return a.dial == b.dial && a.at == b.at && a.to == b.to && a.over == b.over;
                          })
            && std::equal(wheels.begin(), wheels.end(), o.wheels.begin(), o.wheels.end(),
                          [](const WheelMove& a, const WheelMove& b) {
                              return a.wheel == b.wheel && a.at == b.at && a.value == b.value;
                          })
            && hostEdits.stopAt == o.hostEdits.stopAt && hostEdits.jumpAt == o.hostEdits.jumpAt
            && hostEdits.jumpToBeat == o.hostEdits.jumpToBeat
            && std::equal(events.begin(), events.end(), o.events.begin(), o.events.end(), sameEvent)
            && std::equal(transport.begin(), transport.end(), o.transport.begin(), o.transport.end(), sameTransport)
            && blocks == o.blocks && frames == o.frames && frameControls == o.frameControls && clock == o.clock
            && part == o.part;
    }

    bool Request::scenario() const
    {
        return !phrase.empty() || !automate.empty() || !wheels.empty() || hostEdits.stopAt >= 0.0 || hostEdits.jumpAt >= 0.0
            || !events.empty() || !transport.empty() || !blocks.empty() || input != nullptr || !frames.empty();
    }

    std::string scenarioText(const Request& r)
    {
        std::vector<std::string> parts;
        if (!r.phrase.empty())
        {
            int lo = 127, hi = 0, most = 0;
            double end = 0.0;
            std::vector<std::pair<double, int>> edges;           // +1 at a note-on, -1 at its note-off
            for (const PhraseNote& n : r.phrase)
            {
                lo = std::min(lo, n.note);
                hi = std::max(hi, n.note);
                end = std::max(end, n.at + n.length);
                edges.push_back({ n.at + n.length, -1 });
                edges.push_back({ n.at, +1 });
            }
            std::sort(edges.begin(), edges.end());
            int now = 0;
            for (const auto& e : edges)
                most = std::max(most, now += e.second);
            parts.push_back("a phrase of " + std::to_string(r.phrase.size()) + (r.phrase.size() == 1 ? " note" : " notes")
                            + (lo == hi ? " (" + noteName(lo) + ")" : " (" + noteName(lo) + " to " + noteName(hi) + ", up to "
                                                                          + std::to_string(most) + " at once)")
                            + " over " + seconds(end));
        }
        std::map<std::string, int> moved;
        for (const DialMove& m : r.automate)
            ++moved[m.dial];
        for (const auto& [dial, n] : moved)
            parts.push_back(dial + (n == 1 ? " moves once" : n == 2 ? " moves twice" : " moves " + std::to_string(n) + " times"));
        int counts[4] = {};
        for (const WheelMove& w : r.wheels)
            ++counts[static_cast<int>(w.wheel)];
        const char* wheelNames[4] = { "the mod wheel", "the pitch wheel", "pressure", "the pedal" };
        for (int k = 0; k < 4; ++k)
            if (counts[k] > 0)
                parts.push_back(std::string(wheelNames[k]) + " " + std::to_string(counts[k])
                                + (counts[k] == 1 ? " time" : " times"));
        if (r.hostEdits.jumpAt >= 0.0)
            parts.push_back(r.hostEdits.jumpAt <= 0.0 ? "the song from beat " + fmt("%g", r.hostEdits.jumpToBeat)
                                                      : "the playhead jumps to beat " + fmt("%g", r.hostEdits.jumpToBeat)
                                                            + " at " + seconds(r.hostEdits.jumpAt));
        if (r.hostEdits.stopAt >= 0.0)
            parts.push_back("the host stops at " + seconds(r.hostEdits.stopAt));
        if (!r.recording.empty())
            parts.push_back(r.recording);
        else if (!r.events.empty() || !r.transport.empty() || !r.blocks.empty())
            parts.push_back("a recording's " + std::to_string(r.events.size()) + " events");
        if (r.input != nullptr && r.recording.empty())
            parts.push_back("its own input (" + seconds(static_cast<double>(r.input->l.size()) / std::max(r.rate, 1.0)) + ")");
        std::string out;
        for (const std::string& p : parts)
            out += (out.empty() ? "" : "; ") + p;
        return out;
    }

    std::vector<Step> largestSteps(const std::vector<float>& l, const std::vector<float>& r, std::size_t count, double rate)
    {
        // Each 10 ms window's largest step, then the largest windows' steps, each at least a window from the others.
        const std::size_t w = std::max<std::size_t>(1, static_cast<std::size_t>(0.01 * rate));
        const std::size_t n = std::min(l.size(), r.size());
        std::vector<Step> windows;
        for (std::size_t a = 1; a < n; a += w)
        {
            Step best;
            for (std::size_t i = a; i < std::min(n, a + w); ++i)
            {
                const double d = std::max(std::fabs(static_cast<double>(l[i]) - l[i - 1]), std::fabs(static_cast<double>(r[i]) - r[i - 1]));
                if (d > best.size)
                {
                    best.size = d;
                    best.at = i;
                }
            }
            if (best.size > 0.0)
                windows.push_back(best);
        }
        std::stable_sort(windows.begin(), windows.end(), [](const Step& a, const Step& b) { return a.size > b.size; });
        // The sound's own steps: the median window's largest. After the largest, only the steps that stand out from them.
        const double usual = windows.empty() ? 0.0 : windows[windows.size() / 2].size;
        std::vector<Step> out;
        for (const Step& st : windows)
        {
            if (out.size() >= count || (!out.empty() && st.size <= 1.5 * usual))
                break;
            const bool near = std::any_of(out.begin(), out.end(), [&](const Step& o) {
                return (st.at > o.at ? st.at - o.at : o.at - st.at) < w;
            });
            if (!near)
                out.push_back(st);
        }
        return out;
    }

    std::string nearestMark(const Timeline& tl, std::size_t at, double rate)
    {
        const auto after = std::upper_bound(tl.marks.begin(), tl.marks.end(), at,
                                            [](std::size_t x, const Mark& m) { return x < m.at; });
        const Mark* best = nullptr;
        if (after != tl.marks.begin())
            best = &*std::prev(after);                       // the last at or before it
        if (after != tl.marks.end() && (best == nullptr || after->at - at < at - best->at))
            best = &*after;
        std::string out;
        if (best == nullptr || (best->at > at ? best->at - at : at - best->at) > static_cast<std::size_t>(0.05 * rate))
            out = "no event within 50 ms";
        else
        {
            const double ms = (static_cast<double>(best->at) - static_cast<double>(at)) / rate * 1000.0;
            out = std::fabs(ms) < 0.05 ? "at " + best->what
                                       : fmt("%.1f ms ", std::fabs(ms)) + (ms < 0.0 ? "after " : "before ") + best->what;
        }
        if (!tl.moves.empty() && tl.blocks.empty() && tl.block > 1 && at % static_cast<std::size_t>(tl.block) == 0)
            out += "; at a block's start, where a moving dial takes its next value";
        return out;
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
    namespace
    {
        // `t` seconds at `rate` as a sample, at least 0.
        std::size_t sampleOf(double t, double rate)
        {
            return t > 0.0 && std::isfinite(t) ? static_cast<std::size_t>(std::llround(t * rate)) : 0;
        }

        std::string plainText(float v) { return fmt("%g", static_cast<double>(v)); }

        // An instrument's notes as a request asks for them (default one, C3).
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

        // A slot's moves at sample p (Timeline::Move), evenly through its normalised range: into norm.
        void movedNorm(std::array<float, kMaxControls>& norm, const std::array<float, kMaxControls>& base, const Controls& controls,
                       const Timeline& tl, std::size_t p)
        {
            norm = base;
            for (const Timeline::Move& m : tl.moves)             // by slot, then by sample: the last begun counts
            {
                if (m.a > p)
                    continue;
                const Control& k = controls[static_cast<std::size_t>(m.slot)];
                const float to = normOf(k, m.to);
                if (p >= m.b || m.b <= m.a)
                    norm[static_cast<std::size_t>(m.slot)] = to;
                else
                {
                    const float from = normOf(k, m.from);
                    const double t = static_cast<double>(p - m.a) / static_cast<double>(m.b - m.a);
                    norm[static_cast<std::size_t>(m.slot)] = from + static_cast<float>(t) * (to - from);
                }
            }
        }

        // A recording's parts (Request::frames, clock, part, transport): its dials for this program's slots (the same dial
        // as it is, another by name through its plain value, one `dials` names as asked); a mark where a dial moves
        // (changes 50 ms or more apart are two moves) and where the host starts, stops or jumps.
        void recordingParts(Timeline& tl, const Request& req, const Controls& controls, const std::array<float, kMaxControls>& base,
                            double rate)
        {
            tl.clock = req.clock;
            tl.part = std::max(0, req.part);
            for (std::size_t i = 1; i < req.transport.size(); ++i)
            {
                const auto& [at0, a] = req.transport[i - 1];
                const auto& [at, b] = req.transport[i];
                if (!a.known || !b.known)
                    continue;
                if (a.playing != b.playing)
                    tl.marks.push_back({ at, b.playing ? "the host starts playing" : "the host stops", -1, false });
                else if (b.playing && std::fabs(b.beat - (a.beat + static_cast<double>(at - at0) * a.bpm / (60.0 * rate))) > 0.01)
                    tl.marks.push_back({ at, "the playhead jumps to beat " + fmt("%.2f", b.beat), -1, false });
            }
            if (req.frames.empty())
                return;
            std::array<int, kMaxControls> from;
            std::array<bool, kMaxControls> same{};
            from.fill(-1);
            for (int c = 0; c < kMaxControls; ++c)
            {
                const Control& k = controls[static_cast<std::size_t>(c)];
                if (!k.used || req.dials.count(k.name) != 0)
                    continue;
                if (req.frameControls == nullptr)
                {
                    from[static_cast<std::size_t>(c)] = c;
                    same[static_cast<std::size_t>(c)] = true;
                    continue;
                }
                for (int o = 0; o < kMaxControls && from[static_cast<std::size_t>(c)] < 0; ++o)
                {
                    const Control& r = (*req.frameControls)[static_cast<std::size_t>(o)];
                    if (r.used && sameName(r, k))
                    {
                        from[static_cast<std::size_t>(c)] = o;
                        same[static_cast<std::size_t>(c)] = r.type == k.type && r.min == k.min && r.max == k.max && r.taper == k.taper;
                    }
                }
            }
            for (const auto& [at, f] : req.frames)
            {
                std::array<float, kMaxControls> norm = base;
                for (std::size_t c = 0; c < norm.size(); ++c)
                {
                    const int o = from[c];
                    if (o < 0)
                        continue;
                    const float v = f[static_cast<std::size_t>(o)];
                    norm[c] = same[c] ? v : normOf(controls[c], plainOf((*req.frameControls)[static_cast<std::size_t>(o)], v));
                }
                tl.frames.push_back({ at, norm });
            }
            const auto gap = static_cast<std::size_t>(0.05 * rate);
            for (std::size_t c = 0; c < controls.size(); ++c)
            {
                if (from[c] < 0)
                    continue;
                const Control& k = controls[c];
                std::size_t start = 0, last = 0;
                float before = 0.0f, after = 0.0f;
                bool open = false;
                const auto close = [&] {
                    tl.marks.push_back({ start,
                                         std::string(k.name) + (last == start ? (k.type == ControlType::real ? " set to " : " steps to ")
                                                                                    + plainText(after)
                                                                              : " moves from " + plainText(before) + " to " + plainText(after)
                                                                                    + " over " + seconds(static_cast<double>(last - start) / rate)),
                                         -1, false });
                };
                for (std::size_t i = 1; i < tl.frames.size(); ++i)
                {
                    const float a = tl.frames[i - 1].second[c], b = tl.frames[i].second[c];
                    if (a == b)
                        continue;
                    const std::size_t at = tl.frames[i].first;
                    if (open && at - last >= gap)
                    {
                        close();
                        open = false;
                    }
                    if (!open)
                    {
                        open = true;
                        start = at;
                        before = plainOf(k, a);
                    }
                    last = at;
                    after = plainOf(k, b);
                }
                if (open)
                    close();
            }
        }

        // The parts of a timeline every render of a request shares: the host's blocks, its transport (the song's start
        // at the request's tempo, the edits, or a recording's), the dials' moves; with their marks.
        void hostParts(Timeline& tl, const Request& req, const Controls& controls, const std::array<float, kMaxControls>& base,
                       double rate)
        {
            tl.block = std::clamp(req.block, 1, kMaxBlock);
            for (const int b : req.blocks)
                tl.blocks.push_back(std::clamp(b, 1, kMaxBlock));
            // Where a dial's move or the transport's edit lands: the first host block at or after it (the marks say so).
            const auto landing = [&tl](std::size_t at) {
                std::size_t start = 0;
                for (const int b : tl.blocks)
                {
                    if (start >= at)
                        return start;
                    start += static_cast<std::size_t>(b);
                }
                const auto block = static_cast<std::size_t>(tl.block);
                return start >= at ? start : start + (at - start + block - 1) / block * block;
            };
            Transport t;                                     // the host plays from the song's start, at the tempo asked
            t.known = true;
            t.bpm = std::clamp(static_cast<double>(req.bpm), 20.0, 400.0);
            t.playing = true;
            if (!req.transport.empty())
                for (const auto& [at, x] : req.transport)
                    tl.transport.push_back({ at, x, false });
            else
            {
                const HostEdits& e = req.hostEdits;
                Timeline::Send start{ 0, t, false };
                if (e.jumpAt >= 0.0 && sampleOf(e.jumpAt, rate) == 0)
                    start.transport.beat = e.jumpToBeat;
                if (e.stopAt >= 0.0 && sampleOf(e.stopAt, rate) == 0)
                    start.transport.playing = false;
                tl.transport.push_back(start);
                if (e.jumpAt >= 0.0 && sampleOf(e.jumpAt, rate) > 0)
                {
                    Transport j = t;
                    j.beat = e.jumpToBeat;
                    j.playing = !(e.stopAt >= 0.0 && e.stopAt <= e.jumpAt);
                    tl.transport.push_back({ sampleOf(e.jumpAt, rate), j, false });
                    tl.marks.push_back({ landing(sampleOf(e.jumpAt, rate)), "the playhead jumps to beat " + fmt("%g", e.jumpToBeat), -1, false });
                }
                if (e.stopAt >= 0.0 && sampleOf(e.stopAt, rate) > 0)
                {
                    Transport st = t;
                    st.playing = false;
                    tl.transport.push_back({ sampleOf(e.stopAt, rate), st, true });
                    tl.marks.push_back({ landing(sampleOf(e.stopAt, rate)), "the host stops", -1, false });
                }
                std::stable_sort(tl.transport.begin(), tl.transport.end(),
                                 [](const Timeline::Send& x, const Timeline::Send& y) { return x.at < y.at; });
            }
            std::vector<DialMove> moves = req.automate;
            std::stable_sort(moves.begin(), moves.end(), [](const DialMove& x, const DialMove& y) { return x.at < y.at; });
            std::array<float, kMaxControls> norm{};
            for (const DialMove& m : moves)
            {
                int slot = -1;
                for (int c = 0; c < kMaxControls && slot < 0; ++c)
                    if (controls[static_cast<std::size_t>(c)].used && m.dial == controls[static_cast<std::size_t>(c)].name)
                        slot = c;
                if (slot < 0)
                {
                    if (std::find(tl.unknown.begin(), tl.unknown.end(), m.dial) == tl.unknown.end())
                        tl.unknown.push_back(m.dial);
                    continue;
                }
                const Control& k = controls[static_cast<std::size_t>(slot)];
                Timeline::Move mv;
                mv.slot = slot;
                mv.a = sampleOf(std::min(m.at, kMaxPlaySeconds), rate);
                const bool glide = k.type == ControlType::real && m.over > 0.0;
                mv.b = glide ? std::max(mv.a + 1, sampleOf(std::min(m.at + m.over, kMaxPlaySeconds), rate)) : mv.a;
                movedNorm(norm, base, controls, tl, mv.a);   // where the moves so far leave it
                mv.from = plainOf(k, norm[static_cast<std::size_t>(slot)]);
                mv.to = std::clamp(m.to, std::min(k.min, k.max), std::max(k.min, k.max));
                if (k.type != ControlType::real)
                    mv.to = std::round(mv.to);
                tl.moves.push_back(mv);
                std::stable_sort(tl.moves.begin(), tl.moves.end(), [](const Timeline::Move& x, const Timeline::Move& y) {
                    return x.slot != y.slot ? x.slot < y.slot : x.a < y.a;
                });
                tl.marks.push_back({ landing(mv.a),
                                     std::string(k.name) + (glide ? " moves to " + plainText(mv.to) + " over " + seconds(m.over)
                                                                  : (k.type == ControlType::real ? " set to " : " steps to ")
                                                                        + plainText(mv.to)),
                                     -1, false });
            }
            recordingParts(tl, req, controls, base, rate);
        }

        // An instrument's MIDI: `keys` held from the start until offAt (the wheels where the request puts them, at the
        // start, before the notes), or (keys null) the request's scenario: its phrase (else its notes held for holdN), the
        // wheels after those, a recording's events in place of the notes; with their marks.
        [[maybe_unused]] void noteParts(Timeline& tl, const Request& req, const std::vector<int>* keys, std::size_t offAt, double rate)
        {
            struct Ev
            {
                std::size_t at;
                int rank;                                    // at one sample: the controllers, the note-offs, the note-ons
                Event e;
            };
            std::vector<Ev> evs;
            evs.push_back({ 0, 0, Event{ 0, EventKind::controller, 1, std::clamp(req.modwheel, 0.0f, 1.0f) } });
            evs.push_back({ 0, 0, Event{ 0, EventKind::pitchBend, 0, std::clamp(req.pitchbend, -1.0f, 1.0f) } });
            const float vel = std::clamp(req.velocity, 0.0f, 1.0f);
            const bool recorded = keys == nullptr && !req.events.empty();
            if (keys != nullptr || (req.phrase.empty() && !recorded))
            {
                const std::vector<int> ks = keys != nullptr ? *keys : notesOf(req);
                for (const int n : ks)
                    evs.push_back({ 0, 2, Event{ 0, EventKind::noteOn, static_cast<uint8_t>(n), vel } });
                for (const int n : ks)
                    evs.push_back({ offAt, 1, Event{ 0, EventKind::noteOff, static_cast<uint8_t>(n), 0.0f } });
            }
            else if (!recorded)
                for (const PhraseNote& n : req.phrase)
                {
                    const std::size_t on = sampleOf(std::min(n.at, kMaxPlaySeconds), rate);
                    const std::size_t off = std::max(on + 1, sampleOf(std::min(n.at + std::max(n.length, 0.0), kMaxPlaySeconds), rate));
                    const auto key = static_cast<uint8_t>(std::clamp(n.note, 0, 127));
                    evs.push_back({ on, 2, Event{ 0, EventKind::noteOn, key, std::clamp(n.velocity, 0.0f, 1.0f) } });
                    evs.push_back({ off, 1, Event{ 0, EventKind::noteOff, key, 0.0f } });
                }
            if (keys == nullptr)
                for (const WheelMove& w : req.wheels)
                {
                    const std::size_t at = sampleOf(std::min(w.at, kMaxPlaySeconds), rate);
                    switch (w.wheel)
                    {
                        case Wheel::mod: evs.push_back({ at, 0, Event{ 0, EventKind::controller, 1, std::clamp(w.value, 0.0f, 1.0f) } }); break;
                        case Wheel::bend: evs.push_back({ at, 0, Event{ 0, EventKind::pitchBend, 0, std::clamp(w.value, -1.0f, 1.0f) } }); break;
                        case Wheel::pressure: evs.push_back({ at, 0, Event{ 0, EventKind::pressure, 0, std::clamp(w.value, 0.0f, 1.0f) } }); break;
                        case Wheel::sustain: evs.push_back({ at, 0, Event{ 0, EventKind::controller, 64, w.value >= 0.5f ? 1.0f : 0.0f } }); break;
                    }
                }
            std::stable_sort(evs.begin(), evs.end(), [](const Ev& x, const Ev& y) { return x.at != y.at ? x.at < y.at : x.rank < y.rank; });
            for (const Ev& e : evs)
                tl.events.push_back({ e.at, e.e });
            if (recorded)                                    // as they came, after ours at the same sample
            {
                std::vector<std::pair<std::size_t, Event>> merged;
                std::merge(tl.events.begin(), tl.events.end(), req.events.begin(), req.events.end(), std::back_inserter(merged),
                           [](const auto& x, const auto& y) { return x.first < y.first; });
                tl.events = std::move(merged);
            }
            // The marks: each note with what it does to the voices, as far as the events show it.
            std::array<bool, 128> held{};
            std::array<long, 128> lastOff;
            lastOff.fill(-1);
            int heldCount = 0;
            bool first = true;
            for (const auto& [at, e] : tl.events)
            {
                const std::size_t k = e.note;
                switch (e.kind)
                {
                    case EventKind::noteOn:
                    {
                        std::string what = "note-on " + std::to_string(e.note);
                        if (held[k])
                            what += " (the same key again while held: its voice is stopped first)";
                        else if (lastOff[k] >= 0)
                            what += " (the same key again " + seconds((static_cast<double>(at) - static_cast<double>(lastOff[k])) / rate)
                                  + " after its release)";
                        if (heldCount >= Patch::kVoices && !held[k])
                            what += " (a 17th key held: a voice is stolen)";
                        tl.marks.push_back({ at, what, e.note, true });
                        if (first)
                            tl.firstNote = at;
                        first = false;
                        heldCount += held[k] ? 0 : 1;
                        held[k] = true;
                        break;
                    }
                    case EventKind::noteOff:
                        tl.marks.push_back({ at, "note-off " + std::to_string(e.note), e.note, false });
                        tl.heldUntil = std::max(tl.heldUntil, at);
                        if (held[k])
                        {
                            held[k] = false;
                            --heldCount;
                            lastOff[k] = static_cast<long>(at);
                        }
                        break;
                    case EventKind::allNotesOff:
                        tl.marks.push_back({ at, "all notes off", -1, false });
                        held.fill(false);
                        heldCount = 0;
                        break;
                    case EventKind::controller:
                        if (e.note == 64)
                        {
                            if (at > 0 || e.velocity >= 0.5f)        // (up at the start: at rest, no moment)
                                tl.marks.push_back({ at, e.velocity >= 0.5f ? "the pedal down" : "the pedal up", -1, false });
                        }
                        else if (at > 0 || e.velocity != 0.0f)
                            tl.marks.push_back({ at, (e.note == 1 ? std::string("the mod wheel") : "controller " + std::to_string(e.note))
                                                         + " to " + fmt("%.2f", static_cast<double>(e.velocity)),
                                                 -1, false });
                        break;
                    case EventKind::pitchBend:
                        if (at > 0 || e.velocity != 0.0f)
                            tl.marks.push_back({ at, "the pitch wheel to " + fmt("%.2f", static_cast<double>(e.velocity)), -1, false });
                        break;
                    case EventKind::pressure:
                        if (at > 0 || e.velocity != 0.0f)
                            tl.marks.push_back({ at, "pressure to " + fmt("%.2f", static_cast<double>(e.velocity)), -1, false });
                        break;
                }
            }
        }

        void sortMarks(Timeline& tl)
        {
            std::stable_sort(tl.marks.begin(), tl.marks.end(), [](const Mark& x, const Mark& y) { return x.at < y.at; });
        }

        // Where the timeline's last event, move or edit is (lastChange), or its held part's end if later (lastMoment).
        std::size_t lastChange(const Timeline& tl)
        {
            std::size_t end = 0;
            for (const auto& e : tl.events)
                end = std::max(end, e.first);
            for (const Timeline::Move& m : tl.moves)
                end = std::max(end, m.b);
            for (const Timeline::Send& t : tl.transport)
                end = std::max(end, t.at);
            return end;
        }

        std::size_t lastMoment(const Timeline& tl) { return std::max(tl.heldUntil, lastChange(tl)); }
    }

    struct Session::Impl
    {
        Engine engine;
        Controls controls{};
        double rate = 48000.0;
        int prepared = kDefaultBlock;                        // the largest call engine.prepare() made room for
        bool host = false;                                   // the program reads the Host block
        bool ok = false;

        // An instrument's plan: the request's scenario (or keys held until offAt) over `length` samples (0: as long as it
        // plays, and the tail); an effect's: the host's parts, over `length` (0: holdSeconds and the tail).
        Timeline plan(const Dials& dials, const Request& req, std::size_t length, const std::vector<int>* keys = nullptr,
                      std::size_t offAt = 0) const
        {
            Timeline tl;
            hostParts(tl, req, controls, dials.norm, rate);
            const auto tailN = static_cast<std::size_t>(std::clamp(req.tailSeconds, 0.0f, 10.0f) * rate);
            const auto holdN = static_cast<std::size_t>(std::clamp(req.holdSeconds, 0.05f, 10.0f) * rate);
            if constexpr (kInstrument)
            {
                noteParts(tl, req, keys, keys != nullptr ? offAt : holdN, rate);
                tl.length = length > 0 ? length : lastMoment(tl) + tailN;
            }
            else
            {
                tl.heldUntil = req.input != nullptr ? std::min(req.input->l.size(), sampleOf(2.0 * kMaxPlaySeconds, rate)) : holdN;
                tl.length = length > 0 ? length : std::max(tl.heldUntil, lastMoment(tl)) + tailN;
            }
            sortMarks(tl);
            return tl;
        }

        // The program as new over `in` (an effect) or the timeline's events (an instrument), as the timeline plans it:
        // host blocks, each in one call or in those `hooks` ask for; the CPU time of the calls from the one at `timeFrom`
        // to the one before `timeUntil` (the whole render: 0, and its length or more).
        Rendered render(const Dials& dials, const Stereo* in, const Timeline& tl, std::size_t watchFrom, std::size_t timeFrom = 0,
                        std::size_t timeUntil = static_cast<std::size_t>(-1), RenderHooks* hooks = nullptr)
        {
            Rendered r;
            int most = tl.block;
            for (const int b : tl.blocks)
                most = std::max(most, b);
            if (most > prepared)
            {
                engine.prepare(rate, most);
                prepared = most;
            }
            engine.restart();
            engine.takeAlerts();
            engine.setClock(tl.clock);                       // a recording's grid phase (0 otherwise)
            if (!tl.transport.empty() && tl.transport.front().at == 0 && !tl.transport.front().transport.known)
                engine.setBeat(tl.transport.front().transport.beat);   // a recording without the host's: the Engine's own
            const std::size_t total = tl.length;
            r.out.l.assign(total, 0.0f);
            r.out.r.assign(total, 0.0f);
            if (in != nullptr)
            {
                std::copy_n(in->l.begin(), std::min(total, in->size()), r.out.l.begin());
                std::copy_n(in->r.begin(), std::min(total, in->size()), r.out.r.begin());
            }
            r.voicesAt.assign(tl.events.size(), -1);
            std::vector<Event> block;
            std::array<float, kMaxControls> norm = dials.norm;
            std::array<float, kMaxControls> base = dials.norm;   // a recording's dials where its frames say (else these)
            std::size_t nextFrame = 0;
            std::size_t next = 0, nextSend = 0, hostAt = 0, hostEnd = 0, blockIndex = 0;
            double segBeat = 0.0, segBpm = 120.0;                // the transport last sent, for a stop where it is
            std::size_t segAt = 0;
            bool segPlaying = false;
            double t0 = 0.0;
            std::size_t timedFrom = 0;
            bool timing = false;
            for (std::size_t pos = 0; pos < total;)
            {
                if (pos >= hostEnd)                              // a host block: the transport, then the dials, once
                {
                    hostAt = pos;
                    const int len = blockIndex < tl.blocks.size() ? tl.blocks[blockIndex] : tl.block;
                    ++blockIndex;
                    hostEnd = std::min(total, pos + static_cast<std::size_t>(len));
                    while (nextSend < tl.transport.size() && tl.transport[nextSend].at <= pos)
                    {
                        Transport t = tl.transport[nextSend].transport;
                        if (tl.transport[nextSend].here)
                            t.beat = segBeat + (segPlaying ? static_cast<double>(pos - segAt) * segBpm / (60.0 * rate) : 0.0);
                        engine.setTransport(t);
                        segBeat = t.beat;
                        segBpm = t.bpm;
                        segAt = pos;
                        segPlaying = t.playing;
                        ++nextSend;
                    }
                    while (nextFrame < tl.frames.size() && tl.frames[nextFrame].first <= pos)
                        base = tl.frames[nextFrame++].second;
                    if (!tl.moves.empty())
                        movedNorm(norm, base, controls, tl, pos);
                    else if (!tl.frames.empty())
                        norm = base;
                    engine.setControls(norm);
                }
                if (!timing && r.cpuSamples == 0 && pos >= timeFrom && pos < timeUntil)
                {
                    timing = true;
                    timedFrom = pos;
                    t0 = threadMs();
                }
                else if (timing && pos >= timeUntil)
                {
                    timing = false;
                    r.cpuMs = threadMs() - t0;
                    r.cpuSamples = pos - timedFrom;
                }
                std::size_t n = hostEnd - pos;
                if (tl.part > 0)                                 // the processor's parts (a recording's)
                    n = std::min(n, static_cast<std::size_t>(tl.part));
                if (hooks != nullptr)
                    if (const int want = hooks->callLength(pos); want > 0)
                        n = std::min(n, static_cast<std::size_t>(want));
                block.clear();
                while (next < tl.events.size() && tl.events[next].first < pos + n)
                {
                    if (block.size() >= static_cast<std::size_t>(kMaxEventsPerBlock))
                    {
                        n = tl.events[next].first > pos ? tl.events[next].first - pos : 0;   // the rest in the next call
                        break;
                    }
                    Event ev = tl.events[next].second;
                    ev.offset = static_cast<uint32_t>(tl.events[next].first - pos);
                    if (ev.kind == EventKind::noteOn)
                        r.voicesAt[next] = engine.activeVoices();   // as the call starts
                    block.push_back(ev);
                    ++next;
                }
                float* ch[2] = { r.out.l.data() + pos, r.out.r.data() + pos };
                engine.process(ch, 2, static_cast<int>(n), block);
                if (hooks != nullptr)
                {
                    const uint32_t a = engine.takeAlerts();
                    r.alerts |= a;
                    const CallInfo call{ pos, static_cast<int>(n), hostAt, engine, std::span<const Event>(block), ch[0], ch[1], a, tl };
                    if (!hooks->afterCall(call))
                    {
                        pos += n;
                        const auto rest = static_cast<std::ptrdiff_t>(pos);   // silent (an effect's held its input)
                        std::fill(r.out.l.begin() + rest, r.out.l.end(), 0.0f);
                        std::fill(r.out.r.begin() + rest, r.out.r.end(), 0.0f);
                        break;
                    }
                }
                pos += n;
                if (kInstrument && r.endedAt < 0 && pos > watchFrom && engine.activeVoices() == 0)
                    r.endedAt = static_cast<long>(pos);
            }
            if (timing)
            {
                r.cpuMs = threadMs() - t0;
                r.cpuSamples = std::min(total, timeUntil) - timedFrom;
            }
            r.alerts |= engine.takeAlerts();
            engine.collect();
            return r;
        }
    };

    namespace
    {
        // The marks of an instrument's render with what the voices did: a note-on that came while every voice sounded.
        [[maybe_unused]] void voiceMarks(Timeline& tl, const Rendered& r)
        {
            for (std::size_t i = 0; i < tl.events.size() && i < r.voicesAt.size(); ++i)
            {
                if (r.voicesAt[i] < Patch::kVoices)
                    continue;
                for (Mark& m : tl.marks)
                    if (m.on && m.at == tl.events[i].first && m.note == tl.events[i].second.note
                        && m.what.find("stolen") == std::string::npos)
                        m.what += " (" + std::to_string(r.voicesAt[i]) + " voices sounding: one is stolen, fading over 2 ms)";
            }
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
        impl_->rate = rate_;
        impl_->engine.prepare(rate_, kDefaultBlock);
        impl_->engine.offer(std::move(r.patch));
        impl_->engine.prepare(rate_, kDefaultBlock);         // takes it at once, without a fade
        impl_->ok = true;
    }

    Session::~Session() = default;

    bool Session::compiled() const noexcept { return impl_->ok; }
    const Controls& Session::controls() const noexcept { return impl_->controls; }
    const Patch* Session::patch() const noexcept { return impl_->ok ? impl_->engine.playingPatch() : nullptr; }

    Session::Render Session::render(const Request& request, const std::vector<float>* inputL,
                                    const std::vector<float>* inputR, std::size_t length, RenderHooks* hooks)
    {
        Render out;
        if (!impl_->ok)
            return out;
        const Dials dials = dialsFor(impl_->controls, request);
        out.dials = dials.text;
        out.values = dials.values;
        const Timeline tl = impl_->plan(dials, request, length);
        Rendered r;
        if constexpr (kInstrument)
            r = impl_->render(dials, nullptr, tl, tl.heldUntil, 0, static_cast<std::size_t>(-1), hooks);
        else
        {
            Stereo in;
            if (request.input != nullptr)                    // its own input, in place of the one given
            {
                in.l = request.input->l;
                in.r = request.input->r.empty() ? in.l : request.input->r;
            }
            else
            {
                in.l = inputL != nullptr ? *inputL : std::vector<float>(tl.length, 0.0f);
                in.r = inputR != nullptr ? *inputR : in.l;
            }
            in.l.resize(tl.length, 0.0f);
            in.r.resize(tl.length, 0.0f);
            r = impl_->render(dials, &in, tl, 0, 0, static_cast<std::size_t>(-1), hooks);
        }
        out.l = std::move(r.out.l);
        out.r = std::move(r.out.r);
        out.alerts = r.alerts;
        out.heldUntil = tl.heldUntil;
        return out;
    }

    Timeline Session::timeline(const Request& request, std::size_t length) const
    {
        return impl_->plan(dialsFor(impl_->controls, request), request, length);
    }

    std::size_t Session::lengthOf(const Request& request) const { return timeline(request).length; }

    Timeline timeline(const Request& req, const Controls& controls, double rate, std::size_t length)
    {
        // Session::Impl::plan's, for these dials and this rate.
        Timeline tl;
        const Dials dials = dialsFor(controls, req);
        hostParts(tl, req, controls, dials.norm, rate);
        const auto tailN = static_cast<std::size_t>(std::clamp(req.tailSeconds, 0.0f, 10.0f) * rate);
        const auto holdN = static_cast<std::size_t>(std::clamp(req.holdSeconds, 0.05f, 10.0f) * rate);
        if constexpr (kInstrument)
        {
            noteParts(tl, req, nullptr, holdN, rate);
            tl.length = length > 0 ? length : lastMoment(tl) + tailN;
        }
        else
        {
            tl.heldUntil = req.input != nullptr ? std::min(req.input->l.size(), sampleOf(2.0 * kMaxPlaySeconds, rate)) : holdN;
            tl.length = length > 0 ? length : std::max(tl.heldUntil, lastMoment(tl)) + tailN;
        }
        sortMarks(tl);
        return tl;
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

        const Timeline plan = impl_->plan(dials, request, 0);
        const int block = plan.blocks.empty() ? plan.block : -1;
        std::string out = "Rendered offline at " + fmt("%.0f", rate) + " Hz, in blocks of "
                        + (block > 0 ? std::to_string(block) : std::string("the recording's sizes"))
                        + (request.hostBlock && block > 0 ? " (the host's)" : "") + ". Dials: " + dials.text + ".\n";
        if (impl_->host && !request.transport.empty())
            out += "Host: the recording's, as the host sent it a block at a time.\n";
        else if (impl_->host)
            out += "Host: playing from the song's start at " + fmt("%.0f", std::clamp(request.bpm, 20.0f, 400.0f)) + " BPM"
                 + (kInstrument ? ", mod wheel " + fmt("%.2f", std::clamp(request.modwheel, 0.0f, 1.0f))
                                  + ", pitch bend " + fmt("%.2f", std::clamp(request.pitchbend, -1.0f, 1.0f))
                                : std::string())
                 + ".\n";
        if (const std::string sc = scenarioText(request); !sc.empty())
            out += "Scenario: " + sc + ".\n";
        std::vector<std::string> unknown = dials.unknown;
        for (const std::string& u : plan.unknown)
            if (std::find(unknown.begin(), unknown.end(), u) == unknown.end())
                unknown.push_back(u);
        res.unknown = unknown;
        if (!unknown.empty())
        {
            out += "No dial is named";
            for (const std::string& u : unknown)
                out += " '" + u + "'";
            out += " (they were ignored).\n";
        }
        if (!kInstrument && request.signal >= 0)
            out += "(input is for view_sound and compare: measure_output plays its own test signals.)\n";
        const std::size_t holdN = static_cast<std::size_t>(hold * rate);
        const std::size_t tailN = static_cast<std::size_t>(tail * rate);
        std::vector<std::string> warnings;
        const char* const cpuWhat = request.automate.empty() ? kCpuWhat : kCpuMovingWhat;

        if constexpr (kInstrument)
        {
            // The notes held together from the start (or the scenario's phrase): the held part ends at the last note-off.
            const bool phrased = !request.phrase.empty() || !request.events.empty();
            std::vector<int> notes;
            if (phrased)
            {
                for (const auto& [at, e] : plan.events)
                    if (e.kind == EventKind::noteOn && std::find(notes.begin(), notes.end(), static_cast<int>(e.note)) == notes.end())
                        notes.push_back(e.note);
                std::sort(notes.begin(), notes.end());
                if (notes.empty())
                    notes.push_back(48);
            }
            else
                notes = notesOf(request);
            const float vel = std::clamp(request.velocity, 0.0f, 1.0f);
            const std::size_t from = plan.firstNote, heldEnd = std::max(plan.heldUntil, from + 1);
            Timeline tl = plan;
            const Rendered r = impl_->render(dials, nullptr, tl, heldEnd, from, heldEnd);
            voiceMarks(tl, r);

            if (!phrased)
            {
                std::string what;
                for (const int n : notes)
                    what += (what.empty() ? "" : " ") + std::to_string(n) + " (" + noteName(n) + ", "
                          + fmt("%.1f Hz", noteHz(n)) + ")";
                out += (notes.size() == 1 ? "Note " : "Notes ") + what + " at velocity " + fmt("%.2f", vel) + ", held "
                     + seconds(hold) + ", then released for " + seconds(tail) + ".\n";
            }
            else
                out += "Played: from the first note-on (" + seconds(static_cast<double>(from) / rate) + ") to the last note-off ("
                     + seconds(static_cast<double>(heldEnd) / rate) + "), then " + seconds(tail) + " more.\n";

            const std::size_t settle = from + std::min((heldEnd - from) / 2, static_cast<std::size_t>(0.05 * rate));
            const double heldPeak = peak(r.out, from, heldEnd), heldRms = rms(r.out, settle, heldEnd);
            double corr = 1.0, bal = 0.0;
            stereo(r.out, settle, heldEnd, corr, bal);
            const double heldDc = dc(r.out, settle, heldEnd);
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
                const std::vector<Peak> peaks = spectrumPeaks(mono(r.out, settle, heldEnd), rate, 8, &centroid);
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
                // The largest steps, each with its nearest moment of the render (the click locator).
                const std::vector<Step> steps = largestSteps(r.out.l, r.out.r, 3, rate);
                const double step = steps.empty() ? 0.0 : steps.front().size;
                // A wave's own edges come every cycle: windows of a cycle and a half of the lowest note, at least 10 ms.
                const std::size_t window = static_cast<std::size_t>(std::max(0.01 * rate, 1.5 * rate / lowestHz));
                const double usual = usualStep(r.out, settle, heldEnd, window);
                const bool click = heldPeak > 0.0 && step > 0.35 * heldPeak && step > 0.05 && step > 2.0 * usual;
                v["step"] = step;
                v["click"] = click ? 1.0 : 0.0;
                out += std::string(steps.size() > 1 ? "Largest steps between two samples: " : "Largest step between two samples: ")
                     + stepsText(steps, tl, rate)
                     + (heldPeak > 0.0 ? fmt(" (the largest %.0f%% of the held peak", step / heldPeak * 100.0) : std::string(" ("))
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
                const std::vector<double> pk = windowPeaks(r.out, from, heldEnd, w);
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

            const long fall = fallTime(r.out, heldEnd, toDb(heldRms), 60.0, rate);
            v["release_s"] = heldRms >= 1e-5 && fall >= 0 ? static_cast<double>(fall) / rate : heldRms < 1e-5 ? 0.0 : kInf;
            v["voice_end_s"] = r.endedAt >= 0 ? static_cast<double>(r.endedAt - static_cast<long>(heldEnd)) / rate : kInf;
            out += "Release: ";
            if (heldRms < 1e-5)
                out += "nothing to release.";
            else if (fall < 0)
                out += "still within 60 dB of the held level " + seconds(tail) + " after the note-off.";
            else
                out += "falls 60 dB below the held level " + seconds(static_cast<double>(fall) / rate) + " after the note-off.";
            if (r.endedAt >= 0)
                out += " The last voice ended " + seconds(static_cast<double>(r.endedAt - static_cast<long>(heldEnd)) / rate)
                     + " after it (process returned false).\n";
            else
            {
                out += " No voice ended within " + seconds(tail) + " of the note-off: process kept returning true.\n";
                warnings.push_back("A voice that never returns false holds its slot forever: let the release end (or "
                                   "render with a longer tail_seconds if the release is meant to be that long).");
            }

            // A plain request without a scenario's moves: the chord's and the sixteen voices' renders.
            Request plainReq = request;
            plainReq.automate.clear();
            plainReq.hostEdits = {};
            plainReq.transport.clear();
            plainReq.blocks.clear();
            if (notes.size() == 1 && !phrased)
            {
                const int n = notes.front();
                if (need({ "inharmonic_db" }))
                {
                    // What is no harmonic of the note: aliasing, unless the sound means it.
                    const Peak odd = inharmonic(mono(r.out, settle, heldEnd), rate, noteHz(n), 20000.0);
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
                    const Rendered c = impl_->render(dials, nullptr, impl_->plan(dials, plainReq, chordN, &chord, chordN), chordN);
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
            out += "Alerts: " + alertsText(r.alerts) + ".\n";
            // The CPU: the notes asked for while held, and sixteen voices at once, measured: sixteen keys from the lowest
            // note up held for 0.15 s, timed after the first 0.03 s, the least of two renders.
            const double heldSeconds = static_cast<double>(r.cpuSamples) / rate;
            const double share = cpuShare(r.cpuMs, heldSeconds);
            v["cpu_percent"] = share;
            out += "CPU: the notes held take " + percent(share) + " of one core (" + fmt("%.2f ms", r.cpuMs) + " for "
                 + seconds(heldSeconds) + ")";
            if (need({ "cpu16_percent" }))
            {
                const int low = std::min(*std::min_element(notes.begin(), notes.end()), 127 - 15);
                std::vector<int> keys;
                for (int k = 0; k < 16; ++k)
                    keys.push_back(low + k);
                const std::size_t warm = static_cast<std::size_t>(0.03 * rate), len = static_cast<std::size_t>(0.15 * rate);
                double best = kInf;
                for (int k = 0; k < 2; ++k)
                {
                    const Rendered sixteen = impl_->render(dials, nullptr, impl_->plan(dials, plainReq, len, &keys, len), len, warm, len);
                    best = std::min(best, cpuShare(sixteen.cpuMs, static_cast<double>(sixteen.cpuSamples) / rate));
                }
                v["cpu16_percent"] = best;
                out += "; sixteen voices at once take " + percent(best) + " (measured, sixteen notes held)";
            }
            out += "; " + std::string(cpuWhat) + ".\n";
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
            // The largest steps of every signal, each with its signal and its nearest moment (the click locator).
            struct Found
            {
                Step step;
                std::string where, near;
            };
            std::vector<Found> found;
            // A move or an edit at or after a signal's end: the signal, then silence until it and the tail.
            const std::size_t late = lastChange(plan);
            const auto through = [&](const Stereo& signalIn, const char* name, std::size_t ends = 0, bool starts = false) {
                Stereo padded;
                if (late >= signalIn.size())
                {
                    padded = signalIn;
                    padded.l.resize(late + tailN, 0.0f);
                    padded.r.resize(late + tailN, 0.0f);
                }
                const Stereo& in = late >= signalIn.size() ? padded : signalIn;
                Timeline tl = impl_->plan(dials, request, in.size());
                if (ends > 0)
                    tl.marks.push_back({ ends, std::string(name) + "'s end", -1, false });
                if (starts)
                    tl.marks.push_back({ 0, name, -1, false });
                sortMarks(tl);
                Rendered r = impl_->render(dials, &in, tl, 0);
                cpu += r.cpuMs;
                audio += static_cast<double>(r.cpuSamples) / rate;
                alerts |= r.alerts;
                for (const Step& st : largestSteps(r.out.l, r.out.r, 3, rate))
                    found.push_back({ st, name, nearestMark(tl, st.at, rate) });
                return std::move(r.out);
            };

            const std::size_t settle = std::min(holdN / 2, static_cast<std::size_t>(0.05 * rate));
            if (everything || need({ "sine_peak_db", "sine_rms_db", "sine_gain_db", "thd_db", "dc", "correlation", "balance_db" }))
            {
                const Stereo sine = signal(holdN, [&](std::size_t i) {
                    return sineAmp * static_cast<float>(std::sin(2.0 * std::numbers::pi * 220.0 * static_cast<double>(i) / rate));
                });
                const Stereo o = through(sine, "the sine", holdN);
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
                const Stereo o = through(noise, "the noise", holdN);
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
                const Stereo o = through(impulse, "the impulse", 0, true);
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
                out += "An impulse (0.5), then " + seconds(o.size() > impulseN ? static_cast<double>(o.size()) / rate - 0.01 : tail)
                     + " of silence: ";
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
            if (late >= holdN && request.input == nullptr)
                warnings.push_back("The scenario's last move or edit, at " + seconds(static_cast<double>(late) / rate)
                                   + ", comes after the sine and the noise end (" + seconds(hold)
                                   + ", hold_seconds): it plays over silence there. A longer hold_seconds plays it over them.");
            if (request.input != nullptr)                    // its own input (a recording's), as it is
            {
                Stereo own;
                own.l = request.input->l;
                own.r = request.input->r.empty() ? own.l : request.input->r;
                const std::size_t n = std::min(own.l.size(), static_cast<std::size_t>(2.0 * kMaxPlaySeconds * rate));
                own.l.resize(n + tailN, 0.0f);
                own.r.resize(n + tailN, 0.0f);
                const Stereo o = through(own, "its own input", n);
                const double inRms = rms(own, 0, n), outRms = rms(o, 0, n);
                double cin = 0.0, cout = 0.0;
                spectrumPeaks(mono(own, 0, n), rate, 0, &cin);
                spectrumPeaks(mono(o, 0, n), rate, 0, &cout);
                out += "Its own input, " + seconds(static_cast<double>(n) / rate) + ": in peak " + dbfs(peak(own, 0, n)) + ", RMS "
                     + dbfs(inRms) + "; out peak " + dbfs(peak(o, 0, n)) + ", RMS " + dbfs(outRms)
                     + (inRms > 1e-9 && outRms > 1e-9 ? fmt(" (gain %+.1f dB)", toDb(outRms / inRms)) : std::string())
                     + "; spectral centroid " + fmt("%.0f Hz", cin) + " in, " + fmt("%.0f Hz", cout) + " out.\n";
            }
            if (everything)
            {
                std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.step.size > b.step.size; });
                if (found.size() > 3)
                    found.resize(3);
                v["step"] = found.empty() ? 0.0 : found.front().step.size;
                v["alerts"] = static_cast<double>(alerts & 3u);
                v["cpu_percent"] = cpuShare(cpu, audio);
                std::string steps;
                for (const Found& f : found)
                    steps += (steps.empty() ? "" : "; ") + fmt("%.3f", f.step.size) + " in " + f.where + " at "
                           + seconds(static_cast<double>(f.step.at) / rate) + ", " + f.near;
                out += std::string(found.size() > 1 ? "Largest steps between two samples: " : "Largest step between two samples: ")
                     + (steps.empty() ? std::string("none (silent)") : steps) + ".\n";
                out += "Alerts: " + alertsText(alerts) + ".\n";
                out += "CPU: " + percent(cpuShare(cpu, audio)) + " of one core (" + fmt("%.2f ms", cpu) + " to render "
                     + seconds(audio) + "); " + cpuWhat + ".\n";
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
