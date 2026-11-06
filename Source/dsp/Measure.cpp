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

#if defined(_WIN32)
#include <chrono>
#include <thread>
#include <intrin.h>
#include <windows.h>
#undef near                                          // windows.h's: this file names a flag so
#undef far
#endif

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

        // Over spans (the held part): the RMS of both channels, the mean sample, the left/right correlation (1: mono, 0:
        // unrelated, -1: out of phase) and the balance in dB (left's energy over right's).
        double rmsOver(const Stereo& s, const std::vector<Span>& spans)
        {
            double e = 0.0;
            std::size_t n = 0;
            for (const auto& [from, to] : spans)
                for (std::size_t i = from; i < to && i < s.size(); ++i, ++n)
                    e += 0.5 * (static_cast<double>(s.l[i]) * s.l[i] + static_cast<double>(s.r[i]) * s.r[i]);
            return n > 0 ? std::sqrt(e / static_cast<double>(n)) : 0.0;
        }

        double dcOver(const Stereo& s, const std::vector<Span>& spans)
        {
            double m = 0.0;
            std::size_t n = 0;
            for (const auto& [from, to] : spans)
                for (std::size_t i = from; i < to && i < s.size(); ++i, ++n)
                    m += 0.5 * (static_cast<double>(s.l[i]) + s.r[i]);
            return n > 0 ? m / static_cast<double>(n) : 0.0;
        }

        void stereoOver(const Stereo& s, const std::vector<Span>& spans, double& corr, double& balanceDb)
        {
            double ll = 0.0, rr = 0.0, lr = 0.0;
            for (const auto& [from, to] : spans)
                for (std::size_t i = from; i < to && i < s.size(); ++i)
                {
                    ll += static_cast<double>(s.l[i]) * s.l[i];
                    rr += static_cast<double>(s.r[i]) * s.r[i];
                    lr += static_cast<double>(s.l[i]) * s.r[i];
                }
            corr = ll > 0.0 && rr > 0.0 ? lr / std::sqrt(ll * rr) : (ll == rr ? 1.0 : 0.0);
            balanceDb = ll > 0.0 && rr > 0.0 ? 10.0 * std::log10(ll / rr) : 0.0;
        }

        // The click locator's line: the largest steps, each with its time, its nearest moment and whether it is a click.
        [[maybe_unused]] std::string stepsText(const std::vector<Step>& steps, const Timeline& tl, double rate, const char* where = nullptr)
        {
            std::string out;
            for (const Step& st : steps)
            {
                char buf[48];
                std::snprintf(buf, sizeof buf, "%.3f at %.3f s", st.size, static_cast<double>(st.at) / rate);
                out += (out.empty() ? "" : "; ") + std::string(buf) + (where != nullptr ? std::string(" in ") + where : "")
                     + ", " + nearestMark(tl, st.at, rate) + (st.click ? " (a click)" : "");
            }
            return out.empty() ? std::string("none (silent)") : out;
        }

        // The second largest (the largest of one; 0 of none): a step that comes back twice in a neighbourhood is the
        // sound's own (a low saw's wrap), where one alone stands out.
        double secondLargest(const std::vector<double>& v)
        {
            double a = 0.0, b = 0.0;
            for (const double x : v)
                if (x > a)
                {
                    b = a;
                    a = x;
                }
                else if (x > b)
                    b = x;
            return v.size() < 2 ? a : b;
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

        // The report's bands (Request::spectrum), `what` the part they are of ("while held", "of the noise"): each band's
        // level after its centre up to the Nyquist frequency ("31.5: -62.1, 63: -48.0"), or with `in` (an effect's) its
        // gain, out against in ("none" where nothing goes in); a semitone's an octave a group, after its first note
        // ("C3 131: -20 -61 -48 ..."), a level in whole dB.
        std::string bandsLine(Bands kind, const std::string& what, const std::vector<double>& out,
                              const std::vector<double>* in = nullptr)
        {
            const std::vector<Band> bands = bandsOf(kind);
            const bool semitone = kind == Bands::semitone;
            const std::string text = std::string(kind == Bands::octave ? "Octave" : kind == Bands::third ? "Third-octave" : "Semitone")
                                   + " bands " + what + (in != nullptr ? ", out against in" : "")
                                   + (semitone ? ", a point a MIDI note, an octave a group after its first note" : "") + " ("
                                   + (semitone ? "" : "Hz: ") + (in != nullptr ? "gain dB" : "dBFS") + "; "
                                   + fmt("%g s Hann frames): ", kBandSeconds);
            std::string list;
            for (std::size_t k = 0; k < bands.size() && k < out.size(); ++k)
            {
                if (std::isnan(out[k]))
                    break;                                   // above the Nyquist frequency
                const bool none = in != nullptr && !((*in)[k] > -200.0);
                const double v = in != nullptr ? out[k] - (*in)[k] : out[k];
                const std::string value = none ? std::string("none") : fmt(semitone && in == nullptr ? "%.0f" : "%.1f", v);
                if (!semitone)
                    list += (list.empty() ? "" : ", ") + bandName(bands[k].hz) + ": " + value;
                else if (k == 0 || bands[k].note % 12 == 0)
                    list += (list.empty() ? "" : "; ") + noteName(bands[k].note) + " "
                          + fmt(bands[k].hz < 100.0 ? "%.1f" : "%.0f", bands[k].hz) + ": " + value;
                else
                    list += " " + value;
            }
            return text + list + ".\n";
        }

        // A metric's value as the metrics line writes it: a number in its unit ("inf" for one that never came).
        std::string numberText(const MetricInfo& m, double v)
        {
            if (std::isinf(v))
                return v > 0.0 ? "inf" : "-inf";
            if (std::isnan(v))
                return "nan";
            const std::string unit = m.unit;
            if (unit == "dBFS" || unit == "dB" || unit == "cents")
                return fmt("%.1f", v);
            if (unit == "Hz")
                return fmt(std::fabs(v) >= 1000.0 ? "%.0f" : "%.1f", v);
            if (unit == "s")
                return fmt("%.3f", v);
            if (unit == "ms")
                return fmt("%.2f", v);
            return fmt("%.4g", v);                           // "%" and a ratio or a flag
        }

        // The values measured, name=value in the metrics' order: the report's last line, in the names checks use.
        std::string metricsLine(const Metrics& values)
        {
            std::string out;
            for (const MetricInfo& m : metrics())
                if (const auto it = values.find(m.name); it != values.end())
                    out += (out.empty() ? "" : " ") + std::string(m.name) + "=" + numberText(m, it->second);
            return out.empty() ? std::string() : "Metrics: " + out + "\n";
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
#if defined(_WIN32)
        // Windows: the thread's cycles (QueryThreadCycleTime, the time-stamp counter's while the thread ran: exact,
        // where GetThreadTimes counts whole scheduler ticks of about 15.6 ms) over the counter's rate, measured once
        // against the performance counter.
        double tscPerMs()
        {
            static const double rate = [] {
                LARGE_INTEGER f{}, a{}, b{};
                QueryPerformanceFrequency(&f);
                QueryPerformanceCounter(&a);
                const unsigned long long t0 = __rdtsc();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                QueryPerformanceCounter(&b);
                const unsigned long long t1 = __rdtsc();
                const double ms = static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
                return ms > 0.0 ? static_cast<double>(t1 - t0) / ms : 1.0e6;
            }();
            return rate;
        }

        double threadMs()
        {
            ULONG64 cycles = 0;
            QueryThreadCycleTime(GetCurrentThread(), &cycles);
            return static_cast<double>(cycles) / tscPerMs();
        }
#else
        double threadMs()
        {
            timespec ts{};
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
            return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1.0e6;
        }
#endif

        struct Rendered
        {
            Stereo out;
            double cpuMs = 0.0;                              // the CPU time of the calls timed (render(): those that
            std::size_t cpuSamples = 0;                      // render timeFrom to timeUntil), and their samples
            uint32_t alerts = 0;
            uint32_t plainAlerts = 0;                        // those by Timeline::plain (when it is set)
            long endedAt = -1;                               // the sample after which no voice was left (-1: never)
            std::vector<int> voicesAt;                       // per timeline event: the voices sounding as a note-on's
                                                             // call started (-1 for the others)
            int mostVoices = 0;                              // an instrument's: the most voices sounding after a call
        };

        double cpuShare(double ms, double audioSeconds)
        {
            return audioSeconds > 0.0 ? ms / (audioSeconds * 1000.0) * 100.0 : 0.0;
        }

        std::string percent(double share) { return fmt(share < 0.1 ? "%.3f%%" : "%.2f%%", share); }


        // The peak of each window of w samples over [from, to), of either channel.
        [[maybe_unused]] std::vector<double> windowPeaks(const Stereo& s, std::size_t from, std::size_t to, std::size_t w)
        {
            std::vector<double> out;
            for (std::size_t a = from; w > 0 && a + w <= std::min(to, s.size()); a += w)
                out.push_back(peak(s, a, a + w));
            return out;
        }

        // ---- the metrics ---------------------------------------------------------------------------------------------
        constexpr double kInf = std::numeric_limits<double>::infinity();

        // The words every line uses (metricTerms): "held" and "the spectrum" are defined there, once.
        constexpr const char* kInstrumentTerms =
            "Held: from 50 ms after each note-on (half the note if shorter) to its note-off, a phrase's notes together; a "
            "window {from, to} (seconds of the render) replaces it, except in attack_s and release_s. The spectrum: the "
            "held part's power, Welch's average of 0.1 s Hann frames overlapped by half, the same at every rate. The band: "
            "20 Hz to 20 kHz. After each unit, the tolerance a target takes unless its goal gives one.\n";
        constexpr const char* kEffectTerms =
            "Held: a test signal from 50 ms after it starts to its end (hold_seconds); a window {from, to} (seconds of each "
            "signal's render) replaces it, cut at hold_seconds. The spectrum: the held part's power, Welch's average of "
            "0.1 s Hann frames overlapped by half, the same at every rate. The band: 20 Hz to 20 kHz. After each unit, the "
            "tolerance a target takes unless its goal gives one.\n";

        constexpr MetricInfo kInstrumentMetrics[] = {
            { "peak_db", "dBFS", "the largest sample from the first note-on to the last note-off, or the window's", 1.0,
              false, 0.0 },
            { "rms_db", "dBFS", "the RMS of both channels while held, DC and all", 1.0, false, 0.0 },
            { "dc", "", "the mean sample while held", 0.002, false, 0.0 },
            { "correlation", "", "left against right while held: 1 mono, 0 unrelated, -1 out of phase", 0.05, false, 0.0 },
            { "balance_db", "dB", "left's energy over right's while held", 0.5, false, 0.0 },
            { "centroid_hz", "Hz", "the spectrum's power-weighted mean frequency in the band: how bright", 0.05, true, 1.0 },
            { "fundamental_hz", "Hz",
              "in 0.4 s Hann frames while held, the strongest partial within 600 cents of the expected pitch (the lowest "
              "note, bent by the pitch wheel's start at 2 semitones; nearer it than another note), else its harmonics' best "
              "sum there (0: silent)",
              0.005, true, 0.5 },
            { "pitch_cents", "cents", "fundamental_hz against the expected pitch", 5.0, false, 0.0 },
            { "inharmonic_db", "dB",
              "one note only: in 0.4 s Blackman-Harris frames while held, the strongest component in the band farther than "
              "15 Hz from every harmonic of the expected pitch, under the strongest (aliasing, or the sound's own detuning, "
              "FM or noise; under -90: none, the floor)",
              3.0, false, 0.0 },
            { "step", "", "the largest step between two samples of a channel, the whole render (a jump's is the same at any "
                          "rate, a smooth wave's smaller at a higher one)",
              0.01, false, 0.0 },
            { "click", "",
              "1 when the render has a click, else 0: of its 10 ms windows, one whose largest step is over 0.05, 3 times the "
              "second largest of the 10 windows each side and 0.35 of the peak within 5 windows (trace's rule)",
              0.5, false, 0.0 },
            { "attack_s", "s",
              "from the first note-on until the peaks of one-cycle windows (of the lowest note) first come within 3 dB of "
              "their loudest while held",
              0.1, true, 0.002 },
            { "decay_s", "s",
              "from the render's loudest 10 ms (RMS) until the 10 ms RMS stays 60 dB under it: a pluck's or a bell's "
              "decay",
              0.1, true, 0.002 },
            { "release_s", "s", "from the last note-off until the 10 ms RMS stays 60 dB under the RMS while held",
              0.1, true, 0.002 },
            { "voice_end_s", "s", "from the last note-off until the last voice ended (process returned false)",
              0.1, true, 0.002 },
            { "chord_peak_db", "dBFS",
              "one note only: the largest sample of a chord (the note, a third, a fifth and an octave above) held 0.6 s at "
              "the same velocity",
              1.0, false, 0.0 },
            { "alerts", "", "the output guard's alerts in the render: 0 none, 1 NaN or infinite samples, 2 samples clipped, "
                            "3 both",
              0.5, false, 0.0 },
            { "cpu_percent", "%",
              "the notes' share of one core in real time while held: the rendering thread's own time, of the program's own "
              "code with its dials still (a moving dial costs more; the frozen copy the plugin plays once they rest, usually "
              "less)",
              0.1, true, 0.01 },
            { "cpu16_percent", "%", "the same of sixteen notes held 0.15 s at once (the least of two renders)", 0.1, true,
              0.01 },
        };

        constexpr MetricInfo kEffectMetrics[] = {
            { "sine_peak_db", "dBFS", "the largest output sample of a 220 Hz sine at -12 dBFS, or the window's", 1.0, false,
              0.0 },
            { "sine_rms_db", "dBFS", "its output's RMS of both channels while held", 1.0, false, 0.0 },
            { "sine_gain_db", "dB", "its output RMS over its input's, while held", 0.5, false, 0.0 },
            { "thd_db", "dB",
              "in 0.4 s Blackman-Harris frames while held, its output's power in the band farther than 15 Hz from 220 Hz "
              "(harmonics, aliases, noise) against all of it there: THD+N, how much the effect distorts (-240: none)",
              1.0, false, 0.0 },
            { "dc", "", "the sine's output mean sample while held", 0.002, false, 0.0 },
            { "correlation", "", "the sine's output, left against right while held: 1 mono, 0 unrelated, -1 out of phase",
              0.05, false, 0.0 },
            { "balance_db", "dB", "the sine's output, left's energy over right's while held", 0.5, false, 0.0 },
            { "noise_gain_db", "dB", "white noise's (-18 dBFS RMS) output RMS over its input's, while held", 0.5, false, 0.0 },
            { "centroid_in_hz", "Hz", "the noise's spectrum going in, its power-weighted mean frequency in the band (about 10 kHz)",
              0.05, true, 1.0 },
            { "centroid_out_hz", "Hz", "the same of the noise coming out: how bright the effect leaves it", 0.05, true, 1.0 },
            { "impulse_peak_db", "dBFS", "the largest output sample after an impulse of 0.5 (-6 dBFS)", 1.0, false, 0.0 },
            { "impulse_delay_ms", "ms", "when that sample comes", 0.05, true, 0.1 },
            { "impulse_decay_s", "s",
              "from that sample until its render's 10 ms RMS stays 60 dB under it", 0.1, true, 0.002 },
            { "silence_db", "dBFS", "the output's RMS over 0.5 s of silence in: self-oscillation, DC or noise of its own", 3.0,
              false, 0.0 },
            { "step", "", "the largest step between two samples of a channel, in any signal's render (a jump's is the same "
                          "at any rate, a smooth wave's smaller at a higher one)",
              0.01, false, 0.0 },
            { "alerts", "", "the output guard's alerts in the renders: 0 none, 1 NaN or infinite samples, 2 samples clipped, "
                            "3 both",
              0.5, false, 0.0 },
            { "cpu_percent", "%",
              "the renders' share of one core in real time: the rendering thread's own time, of the program's own code with "
              "its dials still (a moving dial costs more; the frozen copy the plugin plays once they rest, usually less)",
              0.1, true, 0.01 },
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

    std::string toleranceText(const MetricInfo& m)
    {
        // In the metric's unit (a level's in dB), or a share of the target, never under its least.
        if (m.relative)
            return "+-" + fmt("%g", m.tolerance * 100.0) + " %, at least " + fmt("%g", m.floor);
        return "+-" + fmt("%g", m.tolerance);
    }

    const char* metricTerms() { return kInstrument ? kInstrumentTerms : kEffectTerms; }

    // ---- the spectra and the steps --------------------------------------------------------------------------------------
    namespace
    {
        // The taper's value at i of m samples (symmetric: 0 and m - 1 its ends).
        double taperAt(Taper t, std::size_t i, std::size_t m)
        {
            const double x = 2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(std::max<std::size_t>(m - 1, 1));
            if (t == Taper::hann)
                return 0.5 - 0.5 * std::cos(x);
            return 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2.0 * x) - 0.01168 * std::cos(3.0 * x);
        }

        // A bin's log power, never minus infinity.
        double logPower(double p) { return std::log(p + 1e-300); }

        // A local maximum over two bins each side.
        bool localMax(const std::vector<double>& p, std::size_t k)
        {
            return k >= 2 && k + 2 < p.size() && p[k] >= p[k - 1] && p[k] >= p[k + 1] && p[k] >= p[k - 2] && p[k] >= p[k + 2];
        }
    }

    std::vector<Span> heldSpans(const Timeline& tl, std::size_t heldEnd, double rate)
    {
        std::vector<Span> spans;
        std::array<long, 128> on;
        on.fill(-1);
        const auto settle = static_cast<std::size_t>(kSettleSeconds * rate);
        const auto close = [&](std::size_t key, std::size_t at) {
            if (on[key] < 0)
                return;
            const auto a = static_cast<std::size_t>(on[key]);
            on[key] = -1;
            if (at > a)
                spans.push_back({ a + std::min((at - a) / 2, settle), at });
        };
        for (const auto& [at, e] : tl.events)
        {
            const std::size_t key = e.note;
            if ((e.kind == EventKind::noteOn || e.kind == EventKind::noteOff) && key >= on.size())
                continue;                                    // no MIDI note (a malformed message): the Engine plays none
            if (e.kind == EventKind::noteOn)
            {
                close(key, at);                              // pressed again while held: the next starts here
                on[key] = static_cast<long>(at);
            }
            else if (e.kind == EventKind::noteOff)
                close(key, at);
            else if (e.kind == EventKind::allNotesOff)
                for (std::size_t k = 0; k < on.size(); ++k)
                    close(k, at);
        }
        for (std::size_t k = 0; k < on.size(); ++k)
            close(k, heldEnd);                               // still down at the end
        std::sort(spans.begin(), spans.end());
        std::vector<Span> joined;
        for (const Span& s : spans)
        {
            if (s.second <= s.first)
                continue;
            if (!joined.empty() && s.first <= joined.back().second)
                joined.back().second = std::max(joined.back().second, s.second);
            else
                joined.push_back(s);
        }
        return joined;
    }

    namespace
    {
        // spectrum()'s and stereoSpectrum()'s Welch: with `y` null, x's power; else the mean of x's and y's (the channels
        // as one FFT's real and imaginary parts: bin k's (|Z_k|^2 + |Z_(n-k)|^2) / 4).
        Spectrum welch(const std::vector<float>& x, const std::vector<float>* y, double rate, const std::vector<Span>& spans,
                       Taper taper, double seconds)
        {
            constexpr std::size_t kLeast = 16;               // a span shorter than this has no frame
            Spectrum s;
            s.rate = rate;
            const std::size_t frame = std::max<std::size_t>(kLeast, static_cast<std::size_t>(std::llround(seconds * rate)));
            std::size_t n = 1;
            while (n < frame)
                n *= 2;
            s.hzPerBin = rate / static_cast<double>(n);
            s.power.assign(n / 2 + 1, 0.0);
            const std::size_t size = y != nullptr ? std::min(x.size(), y->size()) : x.size();
            std::size_t shortest = frame;
            for (const auto& [from, to] : spans)
                if (const std::size_t end = std::min(to, size); end >= from + kLeast)
                    shortest = std::min(shortest, end - from);
            s.resolutionHz = rate / static_cast<double>(shortest);
            std::vector<std::complex<double>> a(n);
            std::vector<double> w;
            double w2 = 0.0, total = 0.0;
            for (const auto& [from, to] : spans)
            {
                const std::size_t end = std::min(to, size);
                if (end < from + kLeast)
                    continue;
                const std::size_t len = end - from, m = std::min(len, frame);
                // Frames spread evenly from the span's start to its end, at most half a frame apart.
                const std::size_t count = len <= frame ? 1 : (len - frame + frame / 2 - 1) / (frame / 2) + 1;
                if (w.size() != m)
                {
                    w.resize(m);
                    w2 = 0.0;
                    for (std::size_t i = 0; i < m; ++i)
                    {
                        w[i] = taperAt(taper, i, m);
                        w2 += w[i] * w[i];
                    }
                }
                const double weight = static_cast<double>(len) / static_cast<double>(count) / w2;
                for (std::size_t f = 0; f < count; ++f)
                {
                    const std::size_t at = from + (count == 1 ? 0 : (len - m) * f / (count - 1));
                    if (y == nullptr)
                    {
                        for (std::size_t i = 0; i < n; ++i)
                            a[i] = i < m ? std::complex<double>(static_cast<double>(x[at + i]) * w[i], 0.0) : std::complex<double>();
                        fftInPlace(a);
                        for (std::size_t k = 0; k < s.power.size(); ++k)
                            s.power[k] += weight * std::norm(a[k]);
                    }
                    else
                    {
                        for (std::size_t i = 0; i < n; ++i)
                            a[i] = i < m ? std::complex<double>(static_cast<double>(x[at + i]) * w[i],
                                                                static_cast<double>((*y)[at + i]) * w[i])
                                         : std::complex<double>();
                        fftInPlace(a);
                        for (std::size_t k = 0; k < s.power.size(); ++k)
                            s.power[k] += weight * 0.25 * (std::norm(a[k]) + std::norm(a[(n - k) % n]));
                    }
                    ++s.frames;
                }
                total += static_cast<double>(len);
            }
            if (total > 0.0)
                for (double& p : s.power)
                    p /= total;
            return s;
        }
    }

    Spectrum spectrum(const std::vector<float>& x, double rate, const std::vector<Span>& spans, Taper taper, double seconds)
    {
        return welch(x, nullptr, rate, spans, taper, seconds);
    }

    Spectrum stereoSpectrum(const std::vector<float>& l, const std::vector<float>& r, double rate, const std::vector<Span>& spans,
                            Taper taper, double seconds)
    {
        return welch(l, &r, rate, spans, taper, seconds);
    }

    double Spectrum::bandPower(double lo, double hi) const
    {
        const double nyquist = 0.5 * rate;
        if (power.size() < 2 || !(hzPerBin > 0.0) || !(lo < nyquist))
            return std::nan("");
        // Bin k spans k - 0.5 to k + 0.5 bins (the last, the Nyquist frequency's, half of that), and the band takes its
        // share; the one-sided bins count twice (Parseval's 2 / n), the Nyquist frequency's half once.
        const double top = static_cast<double>(power.size() - 1);
        const double a = lo / hzPerBin, b = std::min(hi, nyquist) / hzPerBin;
        double sum = 0.0;
        for (auto k = std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(a + 0.5))); k < power.size(); ++k)
        {
            const double k0 = static_cast<double>(k) - 0.5, k1 = std::min(static_cast<double>(k) + 0.5, top);
            if (k0 >= b)
                break;
            if (const double share = std::min(k1, b) - std::max(k0, a); share > 0.0)
                sum += share * power[k];
        }
        return sum / top;                                    // 2 / n, n = 2 top
    }

    std::vector<Band> bandsOf(Bands kind)
    {
        // A third's nominal centres (IEC 61260's names) for 1 kHz x 2^(k/3), k = -17 (20 Hz) to 13 (20 kHz).
        constexpr std::array<double, 31> kThirds{ 20.0,   25.0,   31.5,   40.0,   50.0,   63.0,   80.0,    100.0,   125.0,  160.0, 200.0,
                                                  250.0,  315.0,  400.0,  500.0,  630.0,  800.0,  1000.0,  1250.0,  1600.0, 2000.0,
                                                  2500.0, 3150.0, 4000.0, 5000.0, 6300.0, 8000.0, 10000.0, 12500.0, 16000.0, 20000.0 };
        std::vector<Band> out;
        const auto add = [&](double nominal, double centre, double half, int note) {
            out.push_back({ nominal, centre / half, centre * half, note });
        };
        if (kind == Bands::octave)
            for (std::size_t k = 0; k < kOctaveBands.size(); ++k)
                add(kOctaveBands[k], 1000.0 * std::exp2(static_cast<double>(k) - 5.0), std::numbers::sqrt2, -1);
        else if (kind == Bands::third)
            for (std::size_t k = 0; k < kThirds.size(); ++k)
                add(kThirds[k], 1000.0 * std::exp2((static_cast<double>(k) - 17.0) / 3.0), std::exp2(1.0 / 6.0), -1);
        else if (kind == Bands::semitone)
            for (int note = 16; note <= 135; ++note)
            {
                const double hz = 440.0 * std::exp2(static_cast<double>(note - 69) / 12.0);
                add(hz, hz, std::exp2(1.0 / 24.0), note);
            }
        return out;
    }

    std::vector<double> bandLevels(const Spectrum& s, Bands kind)
    {
        std::vector<double> out;
        for (const Band& b : bandsOf(kind))
        {
            const double p = s.bandPower(b.lo, b.hi);
            out.push_back(std::isnan(p) ? p : p > 1e-24 ? 10.0 * std::log10(p) : -240.0);
        }
        return out;
    }

    std::string bandName(double hz) { return hz >= 1000.0 ? fmt("%g", hz / 1000.0) + "k" : fmt("%g", hz); }

    double Spectrum::centroid(double lo, double hi) const
    {
        const double top = std::min(hi, 0.5 * rate);
        double num = 0.0, den = 0.0;
        for (std::size_t k = 1; k < power.size(); ++k)
        {
            const double hz = static_cast<double>(k) * hzPerBin;
            if (hz < lo)
                continue;
            if (hz > top)
                break;
            num += hz * power[k];
            den += power[k];
        }
        return den > 0.0 ? num / den : 0.0;
    }

    double Spectrum::strongest() const
    {
        double top = 0.0;
        for (std::size_t k = 1; k < power.size(); ++k)
            top = std::max(top, power[k]);
        return top;
    }

    void Spectrum::refine(std::size_t k, double& hz, double& lp) const
    {
        hz = static_cast<double>(k) * hzPerBin;
        lp = k < power.size() ? logPower(power[k]) : logPower(0.0);
        if (k < 1 || k + 1 >= power.size())
            return;
        const double y0 = logPower(power[k - 1]), y1 = logPower(power[k]), y2 = logPower(power[k + 1]);
        const double den = y0 - 2.0 * y1 + y2;
        const double d = den < 0.0 ? std::clamp(0.5 * (y0 - y2) / den, -0.5, 0.5) : 0.0;
        hz = (static_cast<double>(k) + d) * hzPerBin;
        lp = y1 - 0.25 * (y0 - y2) * d;
    }

    std::vector<Peak> Spectrum::peaks(int count, double withinDb) const
    {
        const double top = strongest();
        if (!(top > 1e-30) || count <= 0)
            return {};
        const double least = top * std::pow(10.0, -withinDb / 10.0);
        std::vector<Peak> found;
        double loudest = logPower(0.0);
        for (std::size_t k = 2; k + 2 < power.size(); ++k)
        {
            if (power[k] < least || !localMax(power, k))
                continue;
            Peak p;
            double lp = 0.0;
            refine(k, p.hz, lp);
            p.db = lp;                                       // its log power, until the loudest is known
            loudest = std::max(loudest, lp);
            found.push_back(p);
        }
        for (Peak& p : found)
            p.db = 10.0 * (p.db - loudest) / std::numbers::ln10;
        std::stable_sort(found.begin(), found.end(), [](const Peak& x0, const Peak& x1) { return x0.db > x1.db; });
        if (found.size() > static_cast<std::size_t>(count))
            found.resize(static_cast<std::size_t>(count));
        std::sort(found.begin(), found.end(), [](const Peak& x0, const Peak& x1) { return x0.hz < x1.hz; });
        return found;
    }

    Pitch pitchOf(const Spectrum& s, double expectedHz, double loHz, double hiHz)
    {
        const double top = s.strongest();
        if (!(top > 1e-30) || !(expectedHz > 0.0) || !(s.hzPerBin > 0.0) || s.power.size() < 8)
            return {};
        const std::vector<double>& p = s.power;
        // The strongest partial in the range: a local maximum within 60 dB of the strongest, 10 dB over the range's median.
        const std::size_t klo = std::max<std::size_t>(2, static_cast<std::size_t>(std::ceil(loHz / s.hzPerBin)));
        const std::size_t khi = std::min(p.size() - 3, static_cast<std::size_t>(std::max(0.0, std::floor(hiHz / s.hzPerBin))));
        if (klo <= khi)
        {
            std::vector<double> range(p.begin() + static_cast<std::ptrdiff_t>(klo), p.begin() + static_cast<std::ptrdiff_t>(khi + 1));
            std::nth_element(range.begin(), range.begin() + static_cast<std::ptrdiff_t>(range.size() / 2), range.end());
            const double median = range[range.size() / 2];
            std::size_t best = 0;
            for (std::size_t k = klo; k <= khi; ++k)
                if (localMax(p, k) && p[k] >= top * 1e-6 && p[k] >= 10.0 * median && (best == 0 || p[k] > p[best]))
                    best = k;
            if (best > 0)
            {
                Pitch out;
                double lp = 0.0;
                s.refine(best, out.hz, lp);
                out.partial = true;
                return out;
            }
        }
        // None: the f0 whose harmonics in the band sum the most magnitude, a cent at a time.
        const double bandTop = std::min(kBandHighHz, 0.5 * s.rate - 2.0 * s.hzPerBin);
        const auto at = [&](double hz) {
            const double b = hz / s.hzPerBin;
            const auto k = static_cast<std::size_t>(b);
            if (k + 1 >= p.size())
                return 0.0;
            const double t = b - static_cast<double>(k);
            return std::sqrt((1.0 - t) * p[k] + t * p[k + 1]);
        };
        double bestF = 0.0, bestScore = 0.0;
        for (int c = -static_cast<int>(kPitchCents); c <= static_cast<int>(kPitchCents); ++c)
        {
            const double f0 = expectedHz * std::exp2(static_cast<double>(c) / 1200.0);
            if (f0 <= loHz || f0 >= hiHz)
                continue;
            double score = 0.0;
            for (int h = 1; h <= 20 && h * f0 <= bandTop; ++h)
                score += at(h * f0);
            if (score > bestScore)
            {
                bestScore = score;
                bestF = f0;
            }
        }
        if (!(bestF > 0.0))
            return {};
        // Placed by its strongest harmonic: the largest bin within a resolution bin of it, refined.
        int strongest = 1;
        for (int h = 2; h <= 20 && h * bestF <= bandTop; ++h)
            if (at(h * bestF) > at(strongest * bestF))
                strongest = h;
        const auto reach = static_cast<std::size_t>(std::ceil(s.resolutionHz / s.hzPerBin));
        const auto c0 = static_cast<std::size_t>(std::llround(strongest * bestF / s.hzPerBin));
        std::size_t k = std::min(c0, p.size() - 1);
        for (std::size_t j = c0 > reach ? c0 - reach : 1; j <= c0 + reach && j + 1 < p.size(); ++j)
            if (p[j] > p[k])
                k = j;
        Pitch out;
        out.hz = bestF;
        if (k >= 1 && k + 1 < p.size() && p[k] >= p[k - 1] && p[k] >= p[k + 1])
        {
            double hz = 0.0, lp = 0.0;
            s.refine(k, hz, lp);
            out.hz = hz / strongest;
        }
        return out;
    }

    Peak inharmonic(const Spectrum& s, double f0, double maxHz)
    {
        Peak worst{ 0.0, -200.0 };
        const double top = s.strongest();
        if (!(f0 > 0.0) || !(top > 1e-30) || s.power.size() < 8)
            return worst;
        const std::vector<double>& p = s.power;
        const double away = 6.0 * s.resolutionHz;
        const std::size_t first = std::max<std::size_t>(2, static_cast<std::size_t>(kBandLowHz / s.hzPerBin));
        const std::size_t last = std::min(p.size() - 2, static_cast<std::size_t>(std::min(maxHz, 0.5 * s.rate) / s.hzPerBin));
        std::size_t at = 0;
        for (std::size_t k = first; k <= last; ++k)
        {
            const double hz = static_cast<double>(k) * s.hzPerBin;
            const double h = std::max(1.0, std::round(hz / f0));         // the nearest harmonic (below f0: f0)
            if (std::fabs(hz - h * f0) <= away)
                continue;                                                // within a harmonic's main lobe
            if (at == 0 || p[k] > p[at])
                at = k;
        }
        if (at == 0 || !(p[at] > 0.0))
            return worst;
        double lp = 0.0;
        s.refine(at, worst.hz, lp);
        worst.db = 10.0 * std::log10(p[at] / top);
        return worst;
    }

    Peak inharmonic(const std::vector<float>& x, double rate, double f0, double maxHz)
    {
        return inharmonic(spectrum(x, rate, { { 0, x.size() } }, Taper::blackmanHarris, kPuritySeconds), f0, maxHz);
    }

    double awayDb(const Spectrum& s, double hz)
    {
        const double away = 6.0 * s.resolutionHz, top = std::min(kBandHighHz, 0.5 * s.rate);
        double all = 0.0, off = 0.0;
        for (std::size_t k = 1; k < s.power.size(); ++k)
        {
            const double f = static_cast<double>(k) * s.hzPerBin;
            if (f < kBandLowHz)
                continue;
            if (f > top)
                break;
            all += s.power[k];
            if (std::fabs(f - hz) > away)
                off += s.power[k];
        }
        return all > 0.0 && off > 0.0 ? 10.0 * std::log10(off / all) : -240.0;
    }

    std::vector<Peak> spectrumPeaks(const std::vector<float>& x, double rate, int count, double* centroidHz)
    {
        const Spectrum s = spectrum(x, rate, { { 0, x.size() } });
        if (centroidHz != nullptr)
            *centroidHz = s.centroid();
        return s.peaks(count);
    }

    std::vector<Click> clicks(const std::vector<float>& l, const std::vector<float>& r, double rate)
    {
        const std::size_t n = std::min(l.size(), r.size());
        const std::size_t w = std::max<std::size_t>(1, static_cast<std::size_t>(0.01 * rate));
        std::vector<double> step, peak;
        std::vector<std::size_t> at;
        for (std::size_t a = 0; a < n; a += w)
        {
            double s = 0.0, p = 0.0;
            std::size_t where = a;
            for (std::size_t i = a; i < std::min(n, a + w); ++i)
            {
                p = std::max({ p, static_cast<double>(std::fabs(l[i])), static_cast<double>(std::fabs(r[i])) });
                if (i == 0)
                    continue;
                const double d = std::max(std::fabs(static_cast<double>(l[i]) - l[i - 1]), std::fabs(static_cast<double>(r[i]) - r[i - 1]));
                if (d > s)
                {
                    s = d;
                    where = i;
                }
            }
            step.push_back(s);
            peak.push_back(p);
            at.push_back(where);
        }
        std::vector<Click> out;
        std::vector<double> around;
        for (std::size_t k = 0; k < step.size(); ++k)
        {
            around.clear();
            double local = 0.0;
            for (std::size_t j = k >= 10 ? k - 10 : 0; j < std::min(step.size(), k + 11); ++j)
            {
                if (j + 5 >= k && j <= k + 5)
                    local = std::max(local, peak[j]);
                if (j != k)
                    around.push_back(step[j]);
            }
            const double usual = secondLargest(around);
            if (step[k] > 0.05 && step[k] > 3.0 * usual && step[k] > 0.35 * local)
                out.push_back({ at[k], step[k], usual });
        }
        std::stable_sort(out.begin(), out.end(), [](const Click& a, const Click& b) {
            return a.size / std::max(a.usual, 1e-9) > b.size / std::max(b.usual, 1e-9);
        });
        return out;
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
            && pitchbend == o.pitchbend && block == o.block && input == o.input && signal == o.signal && window == o.window
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

    std::string windowText(const Window& w) { return fmt("%.2f", w.from) + " to " + fmt("%.2f s", w.to); }

    bool Request::scenario() const
    {
        return !phrase.empty() || !automate.empty() || !wheels.empty() || hostEdits.stopAt >= 0.0 || hostEdits.jumpAt >= 0.0
            || !events.empty() || !transport.empty() || !blocks.empty() || input != nullptr || !frames.empty();
    }

    bool Request::startKey() const
    {
        return std::any_of(wheels.begin(), wheels.end(), [](const WheelMove& w) {
            return w.wheel == Wheel::buttons && ((std::clamp(std::lround(w.value), 0L, static_cast<long>(kMaxButtons)) >> wheelcc::kStartBit) & 1L) != 0;
        });
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
        int counts[kWheelKinds] = {};
        for (const WheelMove& w : r.wheels)
            ++counts[static_cast<int>(w.wheel)];
        const char* wheelNames[kWheelKinds] = { "the mod wheel", "the pitch wheel", "pressure", "the pedal", "the steering",
                                                "the throttle", "the brake", "the clutch", "the gear", "the buttons" };
        for (int k = 0; k < kWheelKinds; ++k)
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
        // The largest step, then the clicks (largest first), each at least 10 ms from those before it.
        const std::size_t w = std::max<std::size_t>(1, static_cast<std::size_t>(0.01 * rate));
        const std::size_t n = std::min(l.size(), r.size());
        Step top;
        for (std::size_t i = 1; i < n; ++i)
        {
            const double d = std::max(std::fabs(static_cast<double>(l[i]) - l[i - 1]), std::fabs(static_cast<double>(r[i]) - r[i - 1]));
            if (d > top.size)
            {
                top.size = d;
                top.at = i;
            }
        }
        std::vector<Step> out;
        if (count == 0 || !(top.size > 0.0))
            return out;
        std::vector<Click> found = clicks(l, r, rate);
        top.click = std::any_of(found.begin(), found.end(), [&](const Click& c) { return c.at == top.at; });
        out.push_back(top);
        std::stable_sort(found.begin(), found.end(), [](const Click& a, const Click& b) { return a.size > b.size; });
        for (const Click& c : found)
        {
            if (out.size() >= count)
                break;
            const bool near = std::any_of(out.begin(), out.end(), [&](const Step& o) {
                return (c.at > o.at ? c.at - o.at : o.at - c.at) < w;
            });
            if (!near)
                out.push_back({ c.size, c.at, true });
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

        // A control change's 7-bit value, as Engine::controller reads it.
        [[maybe_unused]] int sevenBits(const Event& e)
        {
            return std::clamp(static_cast<int>(e.velocity * 127.0f + 0.5f), 0, 127);
        }

        // A racing wheel's axis (wheelcc: 0 the steering, 1 to 3 the pedals) at its 14-bit value, as a mark's words.
        constexpr const char* kPedalNames[] = { "the throttle", "the brake", "the clutch" };

        [[maybe_unused]] std::string axisMark(int axis, int v14)
        {
            if (axis == 0)
                return "steering to " + fmt("%+.2f", static_cast<double>(wheelcc::steeringOf(v14)));
            return std::string(kPedalNames[axis - 1]) + " to " + fmt("%.2f", static_cast<double>(wheelcc::pedalOf(v14)));
        }

        // A racing wheel's buttons' mask (wheelcc's four parts) as a mark's words: "buttons 4 and 5 down".
        [[maybe_unused]] std::string buttonsMark(uint32_t mask)
        {
            std::vector<int> down;
            for (int b = 0; b < wheelcc::kButtonCcs * wheelcc::kButtonBits; ++b)
                if (((mask >> b) & 1u) != 0)
                    down.push_back(b);
            if (down.empty())
                return "every button up";
            std::string list;
            for (std::size_t i = 0; i < down.size(); ++i)
                list += (i == 0 ? "" : i + 1 == down.size() ? " and " : ", ") + std::to_string(down[i]);
            return (down.size() == 1 ? "button " : "buttons ") + list + " down";
        }

        // Another control change's mark (the sustain pedal's, the axes' and the buttons' apart), "" for none: the mod
        // wheel, a racing wheel's gear by name, any other by number. At the start a value at rest is no moment.
        [[maybe_unused]] std::string controllerMark(std::size_t at, const Event& e)
        {
            using namespace wheelcc;
            const int cc = e.note, v = sevenBits(e);
            if (cc == kGear)
            {
                const int g = gearOf(v);
                return at > 0 || g != 0 ? "the gear to " + (g < 0 ? std::string("reverse") : g == 0 ? std::string("neutral") : std::to_string(g))
                                        : std::string();
            }
            if (at > 0 || e.velocity != 0.0f)
                return (cc == 1 ? std::string("the mod wheel") : "controller " + std::to_string(cc)) + " to "
                     + fmt("%.2f", static_cast<double>(e.velocity));
            return {};
        }

        // An instrument's MIDI: `keys` held from the start until offAt (the wheels where the request puts them, at the
        // start, before the notes), or (keys null) the request's scenario: its phrase (else its notes held for holdN), the
        // wheels after those (the start key's note among them: Request::startKey), a recording's events in place of the
        // notes; with their marks.
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
            {
                // The buttons as the wheel sends them: in time order, a part's control change where its 7 bits change
                // (from none down, the Engine's rest), and the start key's note while its bit is down (after the
                // controls at its sample: the ranks).
                std::vector<const WheelMove*> pressed;
                for (const WheelMove& w : req.wheels)
                    if (w.wheel == Wheel::buttons)
                        pressed.push_back(&w);
                std::stable_sort(pressed.begin(), pressed.end(), [](const WheelMove* x, const WheelMove* y) { return x->at < y->at; });
                uint32_t mask = 0;
                for (const WheelMove* w : pressed)
                {
                    using namespace wheelcc;
                    const std::size_t at = sampleOf(std::min(w->at, kMaxPlaySeconds), rate);
                    const auto to = static_cast<uint32_t>(std::clamp(std::lround(w->value), 0L, static_cast<long>(kMaxButtons)));
                    for (int b = 0; b < kButtonCcs; ++b)
                        if (const uint32_t part = (to >> (b * kButtonBits)) & 0x7Fu; part != ((mask >> (b * kButtonBits)) & 0x7Fu))
                            evs.push_back({ at, 0, Event{ 0, EventKind::controller, static_cast<uint8_t>(kButtons + b),
                                                          static_cast<float>(part) / 127.0f } });
                    if (const bool down = ((to >> kStartBit) & 1u) != 0; down != (((mask >> kStartBit) & 1u) != 0))
                        evs.push_back(down ? Ev{ at, 2, Event{ 0, EventKind::noteOn, kStartNote, static_cast<float>(kStartVelocity) / 127.0f } }
                                           : Ev{ at, 1, Event{ 0, EventKind::noteOff, kStartNote, 0.0f } });
                    mask = to;
                }
                for (const WheelMove& w : req.wheels)
                {
                    const std::size_t at = sampleOf(std::min(w.at, kMaxPlaySeconds), rate);
                    switch (w.wheel)
                    {
                        case Wheel::mod: evs.push_back({ at, 0, Event{ 0, EventKind::controller, 1, std::clamp(w.value, 0.0f, 1.0f) } }); break;
                        case Wheel::bend: evs.push_back({ at, 0, Event{ 0, EventKind::pitchBend, 0, std::clamp(w.value, -1.0f, 1.0f) } }); break;
                        case Wheel::pressure: evs.push_back({ at, 0, Event{ 0, EventKind::pressure, 0, std::clamp(w.value, 0.0f, 1.0f) } }); break;
                        case Wheel::sustain: evs.push_back({ at, 0, Event{ 0, EventKind::controller, 64, w.value >= 0.5f ? 1.0f : 0.0f } }); break;
                        case Wheel::steering:
                        case Wheel::throttle:
                        case Wheel::brake:
                        case Wheel::clutch:
                        {
                            // As a racing wheel sends it (wheelcc): the 14 bits' MSB, then the LSB unless 0 (the MSB
                            // reset it); -1 is 0, which the Engine reads as -1 exactly.
                            using namespace wheelcc;
                            const int a = static_cast<int>(w.wheel) - static_cast<int>(Wheel::steering);
                            const double v = static_cast<double>(w.value);
                            const int v14 = a == 0 ? (v <= -1.0 ? 0 : static_cast<int>(std::lround(kCentre + std::min(v, 1.0) * (kMost - kCentre))))
                                                   : static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * kMost));
                            evs.push_back({ at, 0, Event{ 0, EventKind::controller, static_cast<uint8_t>(kSteering + a),
                                                          static_cast<float>(v14 >> 7) / 127.0f } });
                            if ((v14 & 0x7F) != 0)
                                evs.push_back({ at, 0, Event{ 0, EventKind::controller, static_cast<uint8_t>(kSteering + kLsb + a),
                                                              static_cast<float>(v14 & 0x7F) / 127.0f } });
                            break;
                        }
                        case Wheel::gear:
                        {
                            const int g = std::clamp(static_cast<int>(std::lround(static_cast<double>(w.value))), -1, 7);
                            evs.push_back({ at, 0, Event{ 0, EventKind::controller, wheelcc::kGear, static_cast<float>(g & 0x7F) / 127.0f } });
                            break;
                        }
                        case Wheel::buttons:                 // above, in time order
                            break;
                    }
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
            // A racing wheel's axes as the Engine holds them (wheelcc), and each one's last mark (its MSB's).
            std::array<int, wheelcc::kAxes> v14{ { wheelcc::kCentre, 0, 0, 0 } };
            std::array<std::size_t, wheelcc::kAxes> axisMarkAt;
            axisMarkAt.fill(~std::size_t{ 0 });
            // Its buttons' mask as the Engine holds it, and its last mark.
            uint32_t buttons = 0;
            std::size_t buttonsMarkAt = ~std::size_t{ 0 };
            for (const auto& [at, e] : tl.events)
            {
                const std::size_t k = e.note;
                if ((e.kind == EventKind::noteOn || e.kind == EventKind::noteOff) && k >= held.size())
                    continue;                                // no MIDI note (a malformed message): the Engine plays none
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
                        else if (const int cc = e.note; (cc >= wheelcc::kSteering && cc < wheelcc::kSteering + wheelcc::kAxes)
                                                        || (cc >= wheelcc::kSteering + wheelcc::kLsb && cc < wheelcc::kSteering + wheelcc::kLsb + wheelcc::kAxes))
                        {
                            // An axis: its MSB a moment (none at the start at rest), its LSB at that moment the finer
                            // value of it; an LSB alone (a fine move) none.
                            const bool msb = cc < wheelcc::kSteering + wheelcc::kLsb;
                            const int a = cc - wheelcc::kSteering - (msb ? 0 : wheelcc::kLsb);
                            const auto ua = static_cast<std::size_t>(a);
                            v14[ua] = msb ? sevenBits(e) << 7 : (v14[ua] & ~0x7F) | sevenBits(e);
                            if (msb && (at > 0 || v14[ua] != (a == 0 ? wheelcc::kCentre : 0)))
                            {
                                axisMarkAt[ua] = tl.marks.size();
                                tl.marks.push_back({ at, axisMark(a, v14[ua]), -1, false });
                            }
                            else if (!msb && axisMarkAt[ua] < tl.marks.size() && tl.marks[axisMarkAt[ua]].at == at)
                                tl.marks[axisMarkAt[ua]].what = axisMark(a, v14[ua]);
                        }
                        else if (cc >= wheelcc::kButtons && cc < wheelcc::kButtons + wheelcc::kButtonCcs)
                        {
                            // A part of the buttons' mask: one moment a sample, however many parts change in it (none
                            // at the start at rest).
                            const int shift = (cc - wheelcc::kButtons) * wheelcc::kButtonBits;
                            buttons = (buttons & ~(0x7Fu << shift)) | (static_cast<uint32_t>(sevenBits(e)) << shift);
                            if (buttonsMarkAt < tl.marks.size() && tl.marks[buttonsMarkAt].at == at)
                                tl.marks[buttonsMarkAt].what = buttonsMark(buttons);
                            else if (at > 0 || buttons != 0)
                            {
                                buttonsMarkAt = tl.marks.size();
                                tl.marks.push_back({ at, buttonsMark(buttons), -1, false });
                            }
                        }
                        else if (std::string what = controllerMark(at, e); !what.empty())
                            tl.marks.push_back({ at, std::move(what), -1, false });
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

        // Where a request's window ends (0: none), which its render reaches at least.
        std::size_t windowEnd(const Request& req, double rate)
        {
            return req.window.set() ? sampleOf(std::min(req.window.to, kMaxWindowSeconds), rate) : 0;
        }

        // A window's samples within a render of `length`: at least one when it starts inside it, none (first ==
        // second == length) when it starts at or after its end.
        Span windowSpan(const Window& w, std::size_t length, double rate)
        {
            const std::size_t a = std::min(sampleOf(w.from, rate), length);
            return { a, std::clamp(sampleOf(std::min(w.to, kMaxWindowSeconds), rate), std::min(a + 1, length), length) };
        }

        // Whether a window's span holds less than kMinWindowSeconds (each end rounded to a sample: one short of it
        // still holds it).
        [[maybe_unused]] bool tooShort(const Span& s, double rate)
        {
            return s.second < s.first + sampleOf(kMinWindowSeconds, rate) - 1;
        }

        // A timeline's length: `length` when given, else `natural` (what it plays, and the tail) or the window's end when
        // that is later, the natural length then kept as Timeline::plain.
        void setLength(Timeline& tl, std::size_t length, std::size_t natural, const Request& req, double rate)
        {
            tl.length = length > 0 ? length : std::max(natural, windowEnd(req, rate));
            tl.plain = length == 0 && tl.length > natural ? natural : 0;
        }
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
                setLength(tl, length, lastMoment(tl) + tailN, req, rate);
            }
            else
            {
                tl.heldUntil = req.input != nullptr ? std::min(req.input->l.size(), sampleOf(2.0 * kMaxPlaySeconds, rate)) : holdN;
                setLength(tl, length, std::max(tl.heldUntil, lastMoment(tl)) + tailN, req, rate);
            }
            sortMarks(tl);
            return tl;
        }

        // The program as new over `in` (an effect) or the timeline's events (an instrument), as the timeline plans it:
        // host blocks, each in one call or in those `hooks` ask for, a call ending at Timeline::plain (so the samples
        // before it are those a render without the window gives, and the alerts by then are kept); the CPU time of the
        // calls from the first at or after `timeFrom` to the one before `timeUntil`, or of the call that renders both
        // when none starts between them (the whole render: 0, and its length or more).
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
            std::size_t pos = 0;
            while (pos < total)
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
                std::size_t n = hostEnd - pos;
                if (tl.part > 0)                                 // the processor's parts (a recording's)
                    n = std::min(n, static_cast<std::size_t>(tl.part));
                if (hooks != nullptr)
                    if (const int want = hooks->callLength(pos); want > 0)
                        n = std::min(n, static_cast<std::size_t>(want));
                if (tl.plain > pos)                              // the window's extra render: from a call of its own
                    n = std::min(n, tl.plain - pos);
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
                // The timed calls: from the first that starts in [timeFrom, timeUntil), or this one when it renders
                // past both (no call starts between them), to the first that starts at or after timeUntil.
                if (!timing && r.cpuSamples == 0 && pos < timeUntil && pos + n > timeFrom && (pos >= timeFrom || pos + n >= timeUntil))
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
                if (kInstrument)
                    r.mostVoices = std::max(r.mostVoices, engine.activeVoices());
                if (kInstrument && r.endedAt < 0 && pos > watchFrom && engine.activeVoices() == 0)
                    r.endedAt = static_cast<long>(pos);
                if (tl.plain > 0 && pos == tl.plain)
                    r.plainAlerts = r.alerts |= engine.takeAlerts();
            }
            if (timing)
            {
                r.cpuMs = threadMs() - t0;
                r.cpuSamples = pos - timedFrom;
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

    Session::Session(const Sources& files, double rate, const CircuitPins* pins)
        : impl_(std::make_unique<Impl>()), rate_(rate > 1000.0 ? rate : 48000.0)
    {
        Patch::Result r = Patch::compile(files, static_cast<float>(rate_), false, pins);
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
            setLength(tl, length, lastMoment(tl) + tailN, req, rate);
        }
        else
        {
            tl.heldUntil = req.input != nullptr ? std::min(req.input->l.size(), sampleOf(2.0 * kMaxPlaySeconds, rate)) : holdN;
            setLength(tl, length, std::max(tl.heldUntil, lastMoment(tl)) + tailN, req, rate);
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
        // The window (Measure.h): the metrics measured while held measure it instead.
        const bool windowed = request.window.set();
        if (windowed)
            out += "Window: " + windowText(request.window)
                 + (kInstrument ? " of the render, measured in place of the held part (attack_s and release_s keep the notes')"
                                : " of the sine's and the noise's renders, measured in place of their held part")
                 + (kInstrument && plan.plain > 0 ? "; the render goes on to its end, the steps, the decay, the release and the "
                                                    "alerts measured over the " + seconds(static_cast<double>(plan.plain) / rate)
                                                    + " it has without it"
                                                  : std::string())
                 + ".\n";
        const std::size_t holdN = static_cast<std::size_t>(hold * rate);
        const std::size_t tailN = static_cast<std::size_t>(tail * rate);
        std::vector<std::string> warnings;
        // What the CPU figures are is the metrics' (cpu_percent's line in the guide): the report gives the numbers, and
        // says when the scenario moved a dial (which costs more than one held still).
        const std::string cpuMoving = request.automate.empty() ? "" : " (the scenario's dials moving)";

        if constexpr (kInstrument)
        {
            // The notes held together from the start (or the scenario's phrase): the held part ends at the last note-off.
            const bool phrased = !request.phrase.empty() || !request.events.empty() || request.startKey();
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
            // The window's samples (the render reaches its end: plan()), else the notes'.
            const Span win = windowed ? windowSpan(request.window, plan.length, rate) : Span{ from, heldEnd };
            Timeline tl = plan;
            const Rendered r = impl_->render(dials, nullptr, tl, heldEnd, win.first, win.second);
            voiceMarks(tl, r);
            // The whole render's metrics (the steps, the clicks, the decay, the release, the voices' end, the alerts) over
            // the length it has without the window (Timeline::plain): a window past it changes none of them.
            const std::size_t plainN = plan.plain > 0 ? plan.plain : r.out.size();
            Stereo cut;
            if (plainN < r.out.size())
            {
                cut.l.assign(r.out.l.begin(), r.out.l.begin() + static_cast<std::ptrdiff_t>(plainN));
                cut.r.assign(r.out.r.begin(), r.out.r.begin() + static_cast<std::ptrdiff_t>(plainN));
            }
            const Stereo& whole = plainN < r.out.size() ? cut : r.out;
            const long endedAt = r.endedAt >= 0 && static_cast<std::size_t>(r.endedAt) <= plainN ? r.endedAt : -1;
            const uint32_t alerts = plan.plain > 0 ? r.plainAlerts : r.alerts;

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

            // The held part: each note from 50 ms after its note-on to its note-off (Measure.h), the spectra all of it;
            // or the window. The notes' own (the envelope's, the release's reference) stays theirs.
            std::vector<Span> notesHeld = heldSpans(plan, heldEnd, rate);
            if (notesHeld.empty())
                notesHeld.push_back({ from, heldEnd });
            const std::vector<Span> held = windowed ? std::vector<Span>{ win } : notesHeld;
            const std::vector<float> m = mono(r.out, 0, r.out.size());
            const double heldPeak = peak(r.out, win.first, win.second), heldRms = rmsOver(r.out, held);
            const double notesRms = windowed ? rmsOver(r.out, notesHeld) : heldRms;
            const double notesPeak = windowed ? peak(r.out, from, heldEnd) : heldPeak;
            double corr = 1.0, bal = 0.0;
            stereoOver(r.out, held, corr, bal);
            const double heldDc = dcOver(r.out, held);
            v["peak_db"] = toDb(heldPeak);
            v["rms_db"] = toDb(heldRms);
            v["dc"] = heldDc;
            v["correlation"] = corr;
            v["balance_db"] = bal;
            out += std::string(windowed ? "In the window: peak " : "Held: peak ") + dbfs(heldPeak) + ", RMS " + dbfs(heldRms)
                 + ", DC " + fmt("%.4f", heldDc) + ", left/right correlation " + fmt("%.2f", corr)
                 + fmt(" (balance %+.1f dB).\n", bal);
            if (heldRms < 1e-5)
                warnings.push_back(windowed ? "It is silent (or nearly) in the window." : "It is silent (or nearly) while the note is held.");
            if (heldPeak > 1.0)
                warnings.push_back("Peaks above 0 dBFS with " + std::to_string(notes.size())
                                   + " note(s): a voice should peak near 0.2 to 0.3, since sixteen can sound at once.");

            // A plain request without a scenario's moves: the pitch's notes alone, the chord's and the sixteen voices' renders.
            Request plainReq = request;
            plainReq.automate.clear();
            plainReq.hostEdits = {};
            plainReq.transport.clear();
            plainReq.blocks.clear();

            // The expected pitch: the lowest note, bent by the pitch wheel where the request starts it, at the usual 2
            // semitones (host.fm); the pitch looks within 600 cents of it, and nearer it than the next note up.
            const int lowest = *std::min_element(notes.begin(), notes.end());
            const double lowestHz = noteHz(lowest);
            const double bend = std::exp2(2.0 * static_cast<double>(std::clamp(request.pitchbend, -1.0f, 1.0f)) / 12.0);
            const double expectedHz = lowestHz * bend;
            double loHz = expectedHz * std::exp2(-kPitchCents / 1200.0), hiHz = expectedHz * std::exp2(kPitchCents / 1200.0);
            for (const int n : notes)
                if (n > lowest)
                    hiHz = std::min(hiHz, std::sqrt(expectedHz * noteHz(n) * bend));
            bool follows = true;                             // the pitch follows the note (below): no line says otherwise
            if (need({ "centroid_hz", "fundamental_hz", "pitch_cents" }))
            {
                const Spectrum sp = spectrum(m, rate, held);
                const double centroid = sp.centroid();
                v["centroid_hz"] = centroid;
                out += std::string(windowed ? "Spectrum in the window" : "Spectrum while held") + ": centroid "
                     + fmt("%.0f Hz", centroid) + "; strongest partials "
                     + partials(sp.peaks(8)) + ".";
                // The pitch from frames of its own, long enough to part a low chord's notes (Measure.h).
                const Pitch pitch = pitchOf(spectrum(m, rate, held, Taper::hann, kPitchSeconds), expectedHz, loHz, hiHz);
                v["fundamental_hz"] = pitch.hz;
                if (pitch.hz > 0.0)
                {
                    v["pitch_cents"] = 1200.0 * std::log2(pitch.hz / expectedHz);
                    // Whether the pitch follows the note, for the report's words alone (the metrics keep their meaning,
                    // and a check or a tune, which asks for some, renders nothing more): the lowest note and the note a
                    // whole tone up (down from 126), each alone, held 0.6 s with none of the scenario's moves, measured
                    // as this, must come within 50 cents of a whole tone apart. Both alone, a single note held too: its
                    // own render plays the scenario's wheels and moves for hold_seconds, which a bend or a pitch that
                    // drifts would part from the other's. Not an octave: a sound of a pitch of its own has its second
                    // harmonic there. A sound whose pitch does not follow (noise, most drums, an engine whose pitch is
                    // its own) is named unpitched, with both pitches, in place of cents from a note it does not play
                    // (and the Inharmonic line, of the note's harmonics, is left out); a phrase or a chord of a pitched
                    // sound keeps its line.
                    const int other = lowest + 2 <= 127 ? lowest + 2 : lowest - 2;
                    double baseAt = 0.0, otherAt = 0.0;
                    if (all)
                    {
                        const auto alone = [&](int key) {
                            const double hz = expectedHz * std::exp2(static_cast<double>(key - lowest) / 12.0);
                            const std::vector<int> keys{ key };
                            const std::size_t n = static_cast<std::size_t>(0.6 * rate);
                            const Rendered o = impl_->render(dials, nullptr, impl_->plan(dials, plainReq, n, &keys, n), n);
                            const std::vector<Span> span{ { std::min(n / 2, static_cast<std::size_t>(kSettleSeconds * rate)), n } };
                            return pitchOf(spectrum(mono(o.out, 0, o.out.size()), rate, span, Taper::hann, kPitchSeconds), hz,
                                           hz * std::exp2(-kPitchCents / 1200.0), hz * std::exp2(kPitchCents / 1200.0))
                                .hz;
                        };
                        baseAt = alone(lowest);
                        otherAt = alone(other);
                        follows = baseAt > 0.0 && otherAt > 0.0
                               && std::fabs(1200.0 * std::log2(otherAt / baseAt) - 100.0 * static_cast<double>(other - lowest)) <= 50.0;
                    }
                    if (follows)
                        out += " Pitch: " + fmt("%.2f Hz", pitch.hz) + fmt(", %+.1f cents from the expected ", v["pitch_cents"])
                             + fmt("%.2f Hz", expectedHz) + " (note " + std::to_string(lowest)
                             + (bend != 1.0 ? ", bent" : "") + ")"
                             + (pitch.partial ? std::string() : std::string(": no partial near it, its harmonics' sum")) + ".";
                    else
                        out += " Unpitched: its pitch does not follow the note ("
                             + (baseAt > 0.0 ? fmt("%.2f Hz", baseAt) : std::string("none")) + " at note " + std::to_string(lowest) + ", "
                             + (otherAt > 0.0 ? fmt("%.2f Hz", otherAt) : std::string("none")) + " at note " + std::to_string(other)
                             + ", each alone).";
                }
                out += "\n";
            }
            // The band levels asked for (Request::spectrum), of both channels over the held part (or the window).
            if (request.spectrum != Bands::none)
                out += bandsLine(request.spectrum, windowed ? "in the window" : "while held",
                                 bandLevels(stereoSpectrum(r.out.l, r.out.r, rate, held), request.spectrum));

            if (need({ "step", "click" }))
            {
                // The largest step, then the clicks (trace's rule), each with its nearest moment (the click locator).
                const std::vector<Step> steps = largestSteps(whole.l, whole.r, 3, rate);
                const double step = steps.empty() ? 0.0 : steps.front().size;
                const bool click = !clicks(whole.l, whole.r, rate).empty();   // any, as trace finds them
                v["step"] = step;
                v["click"] = click ? 1.0 : 0.0;
                out += "Largest step between two samples"
                     + (notesPeak > 0.0 ? fmt(" (%.0f%% of the held peak)", step / notesPeak * 100.0) : std::string())
                     + (steps.size() > 1 ? ", then the clicks: " : ": ") + stepsText(steps, tl, rate)
                     + (click ? "" : "; no click (no step far above those around it)") + ".\n";
                if (click)
                    warnings.push_back("A step far above those around it is likely a click (an envelope or a phase that "
                                       "jumps): trace with find click names the fields that jumped there.");
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
                for (std::size_t i = 0; i + w10 <= whole.size(); i += w10)
                    if (const double x = rms(whole, i, i + w10); x > loud)
                    {
                        loud = x;
                        loudAt = i;
                    }
                double decay = kInf;
                if (loud > 1e-9)
                {
                    const long fall = fallTime(whole, loudAt, toDb(loud), 60.0, rate);
                    if (fall >= 0)
                        decay = static_cast<double>(fall) / rate;
                }
                v["attack_s"] = attack;
                v["decay_s"] = decay;
                if (notesRms >= 1e-5)
                    out += "Envelope: its peaks come within 3 dB of their loudest " + valueText("attack_s", attack)
                         + " after the note-on; from its loudest moment it falls 60 dB in "
                         + (std::isinf(decay) ? std::string("more than the render") : valueText("decay_s", decay))
                         + ".\n";
            }

            // The release under the notes' held level, whatever the window.
            const long fall = fallTime(whole, heldEnd, toDb(notesRms), 60.0, rate);
            v["release_s"] = notesRms >= 1e-5 && fall >= 0 ? static_cast<double>(fall) / rate : notesRms < 1e-5 ? 0.0 : kInf;
            v["voice_end_s"] = endedAt >= 0 ? static_cast<double>(endedAt - static_cast<long>(heldEnd)) / rate : kInf;
            out += "Release: ";
            if (notesRms < 1e-5)
                out += "nothing to release.";
            else if (fall < 0)
                out += "still within 60 dB of the held level " + seconds(tail) + " after the note-off.";
            else
                out += "falls 60 dB below the held level " + seconds(static_cast<double>(fall) / rate) + " after the note-off.";
            if (endedAt >= 0)
                out += " The last voice ended " + seconds(static_cast<double>(endedAt - static_cast<long>(heldEnd)) / rate)
                     + " after it (process returned false).\n";
            else
                out += " No voice ended within " + seconds(tail) + ": process kept returning true (end the release, or give "
                       "a longer tail_seconds).\n";

            if (notes.size() == 1 && !phrased)
            {
                const int n = notes.front();
                if (need({ "inharmonic_db" }))
                {
                    // What is no harmonic of the note: aliasing, unless the sound means it.
                    const Peak odd = inharmonic(spectrum(m, rate, held, Taper::blackmanHarris, kPuritySeconds), expectedHz, kBandHighHz);
                    v["inharmonic_db"] = odd.db;
                    if (heldRms >= 1e-5 && follows)          // an unpitched sound has no harmonics of the note to judge
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
                         + dbfs(rms(c.out, std::min(chordN / 2, static_cast<std::size_t>(kSettleSeconds * rate)), chordN)) + ".\n";
                    if (cp > 1.0)
                        warnings.push_back("The chord peaks above 0 dBFS: lower the voice's level.");
                }
            }
            v["alerts"] = static_cast<double>(alerts & 3u);
            out += "Alerts: " + alertsText(alerts) + ".\n";
            // The CPU: the notes asked for while held, and sixteen voices at once, measured: sixteen keys from the lowest
            // note up held for 0.15 s, timed after the first 0.03 s, the least of two renders.
            const double heldSeconds = static_cast<double>(r.cpuSamples) / rate;
            const double share = cpuShare(r.cpuMs, heldSeconds);
            v["cpu_percent"] = share;
            out += std::string(windowed ? "CPU: the window takes " : "CPU: the notes held take ") + percent(share) + " of one core ("
                 + fmt("%.2f ms", r.cpuMs) + " for " + seconds(heldSeconds) + ")";
            if (need({ "cpu16_percent" }))
            {
                const int low = std::min(*std::min_element(notes.begin(), notes.end()), 127 - 15);
                std::vector<int> keys;
                for (int k = 0; k < 16; ++k)
                    keys.push_back(low + k);
                const std::size_t warm = static_cast<std::size_t>(0.03 * rate), len = static_cast<std::size_t>(0.15 * rate);
                double best = kInf;
                int most = 0;
                for (int k = 0; k < 2; ++k)
                {
                    const Rendered sixteen = impl_->render(dials, nullptr, impl_->plan(dials, plainReq, len, &keys, len), len, warm, len);
                    best = std::min(best, cpuShare(sixteen.cpuMs, static_cast<double>(sixteen.cpuSamples) / rate));
                    most = std::max(most, sixteen.mostVoices);
                }
                v["cpu16_percent"] = best;
                // Its words only when more than one voice sounds (a program that plays one voice, its others ending
                // at once, has no sixteen to time), saying how many when fewer than sixteen.
                if (most >= Patch::kVoices)
                    out += "; sixteen voices at once take " + percent(best);
                else if (most > 1)
                    out += "; sixteen keys at once take " + percent(best) + " (" + std::to_string(most) + " voices sound)";
            }
            out += cpuMoving + ".\n";
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
                // The impulse's own steps (the impulse and its echoes, the response it is there to show) are no click.
                for (Step st : largestSteps(r.out.l, r.out.r, 3, rate))
                {
                    st.click = st.click && !starts;
                    found.push_back({ st, name, nearestMark(tl, st.at, rate) });
                }
                return std::move(r.out);
            };

            // A signal's held part: from 50 ms after it starts (half of it when shorter) to its end (Measure.h); or the
            // window, cut at its end (too little of it when it starts there or after, or under kMinWindowSeconds before:
            // those metrics are then not measured).
            const std::size_t settle = std::min(holdN / 2, static_cast<std::size_t>(kSettleSeconds * rate));
            const Span win = windowed ? windowSpan(request.window, holdN, rate) : Span{ 0, holdN };
            const std::vector<Span> held{ windowed ? win : Span{ settle, holdN } };
            const bool outside = windowed && (win.first >= win.second || tooShort(win, rate));
            const std::string outsideWhy = win.first >= win.second
                                             ? "the window starts after the sine and the noise end (hold_seconds, " + seconds(hold) + ")"
                                             : "the window holds less than " + seconds(kMinWindowSeconds)
                                                   + " of the sine and the noise (they end at hold_seconds, " + seconds(hold) + ")";
            const auto unmeasured = [&](std::initializer_list<const char*> names) {
                for (const char* n : names)
                    res.unmeasured[n] = outsideWhy;
            };
            // A window outside the signals: their metrics are not measured (the signals still play for the steps, the
            // alerts and the CPU of every signal).
            const bool sineWanted = need({ "sine_peak_db", "sine_rms_db", "sine_gain_db", "thd_db", "dc", "correlation", "balance_db" });
            if (sineWanted && outside)
            {
                unmeasured({ "sine_peak_db", "sine_rms_db", "sine_gain_db", "thd_db", "dc", "correlation", "balance_db" });
                out += "Sine 220 Hz at -12.0 dBFS for " + seconds(hold) + ": not measured, " + outsideWhy + ".\n";
            }
            if (everything || (sineWanted && !outside))
            {
                const Stereo sine = signal(holdN, [&](std::size_t i) {
                    return sineAmp * static_cast<float>(std::sin(2.0 * std::numbers::pi * 220.0 * static_cast<double>(i) / rate));
                });
                const Stereo o = through(sine, "the sine", holdN);
                if (sineWanted && !outside)
                {
                    const double inRms = rmsOver(sine, held), outRms = rmsOver(o, held);
                    double corr = 1.0, bal = 0.0;
                    stereoOver(o, held, corr, bal);
                    const double outDc = dcOver(o, held);
                    const std::vector<float> mo = mono(o, 0, holdN);
                    const double thd = outRms > 1e-9 ? awayDb(spectrum(mo, rate, held, Taper::blackmanHarris, kPuritySeconds), 220.0) : -240.0;
                    const double outPeak = peak(o, win.first, win.second);
                    v["sine_peak_db"] = toDb(outPeak);
                    v["sine_rms_db"] = toDb(outRms);
                    v["sine_gain_db"] = outRms > 1e-12 ? toDb(outRms / inRms) : -240.0;
                    v["thd_db"] = thd;
                    v["dc"] = outDc;
                    v["correlation"] = corr;
                    v["balance_db"] = bal;
                    out += "Sine 220 Hz at -12.0 dBFS for " + seconds(hold) + (windowed ? ", in the window" : "") + ": out peak "
                         + dbfs(outPeak) + ", RMS " + dbfs(outRms)
                         + (outRms > 1e-9 ? fmt(" (gain %+.1f dB)", toDb(outRms / inRms)) : std::string()) + ", DC "
                         + fmt("%.4f", outDc) + ", left/right correlation " + fmt("%.2f", corr)
                         + fmt(" (balance %+.1f dB)", bal) + ". Partials out: " + partials(spectrum(mo, rate, held).peaks(8))
                         + (outRms > 1e-9 ? "; THD+N (all but 220 Hz, in the band) " + valueText("thd_db", thd) : std::string())
                         + ".\n";
                    if (outRms < 1e-5)
                        warnings.push_back(windowed ? "The sine comes out silent (or nearly) in the window."
                                                    : "The sine comes out silent (or nearly).");
                    if (outPeak > 1.0)
                        warnings.push_back("Peaks above 0 dBFS from a -12 dBFS sine: the effect adds a lot of gain.");
                }
            }
            const bool noiseWanted = need({ "noise_gain_db", "centroid_in_hz", "centroid_out_hz" }) || request.spectrum != Bands::none;
            if (noiseWanted && outside)
            {
                unmeasured({ "noise_gain_db", "centroid_in_hz", "centroid_out_hz" });
                out += "Noise at -18.0 dBFS RMS for " + seconds(hold) + ": not measured, " + outsideWhy + ".\n";
            }
            if (everything || (noiseWanted && !outside))
            {
                uint32_t seed = 22222;                       // a fixed noise: the same report each time
                const float noiseAmp = static_cast<float>(std::pow(10.0, -18.0 / 20.0) * std::sqrt(3.0));
                const Stereo noise = signal(holdN, [&](std::size_t) {
                    seed = seed * 1664525u + 1013904223u;
                    return noiseAmp * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
                });
                const Stereo o = through(noise, "the noise", holdN);
                if (noiseWanted && !outside)
                {
                    const double inRms = rmsOver(noise, held), outRms = rmsOver(o, held);
                    double corr = 1.0, bal = 0.0;
                    stereoOver(o, held, corr, bal);
                    const double cin = spectrum(mono(noise, 0, holdN), rate, held).centroid();
                    const double cout = spectrum(mono(o, 0, holdN), rate, held).centroid();
                    v["noise_gain_db"] = outRms > 1e-12 ? toDb(outRms / inRms) : -240.0;
                    v["centroid_in_hz"] = cin;
                    v["centroid_out_hz"] = cout;
                    out += "Noise at -18.0 dBFS RMS for " + seconds(hold) + (windowed ? ", in the window" : "") + ": out RMS "
                         + dbfs(outRms) + (outRms > 1e-9 ? fmt(" (gain %+.1f dB)", toDb(outRms / inRms)) : std::string())
                         + ", left/right correlation " + fmt("%.2f", corr) + "; spectral centroid " + fmt("%.0f Hz", cin)
                         + " in, " + fmt("%.0f Hz", cout) + " out.\n";
                    // Its band levels asked for (Request::spectrum): the effect's gain in each, out against in.
                    if (request.spectrum != Bands::none)
                    {
                        const std::vector<double> bandsIn = bandLevels(stereoSpectrum(noise.l, noise.r, rate, held), request.spectrum);
                        out += bandsLine(request.spectrum, windowed ? "of the noise in the window" : "of the noise",
                                         bandLevels(stereoSpectrum(o.l, o.r, rate, held), request.spectrum), &bandsIn);
                    }
                }
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
                // All of it, or the window's part of it.
                const Span part = windowed ? windowSpan(request.window, n, rate) : Span{ 0, n };
                const auto [a, b] = part;
                const double inRms = rms(own, a, b), outRms = rms(o, a, b);
                const double cin = spectrum(mono(own, 0, n), rate, { part }).centroid();
                const double cout = spectrum(mono(o, 0, n), rate, { part }).centroid();
                out += "Its own input, " + seconds(static_cast<double>(n) / rate);
                if (a >= b)
                    out += ": the window starts after it ends.\n";
                else if (windowed && tooShort(part, rate))
                    out += ": the window holds less than " + seconds(kMinWindowSeconds) + " of it.\n";
                else
                    out += (windowed ? ", in the window" : "") + std::string(": in peak ") + dbfs(peak(own, a, b)) + ", RMS "
                         + dbfs(inRms) + "; out peak " + dbfs(peak(o, a, b)) + ", RMS " + dbfs(outRms)
                         + (inRms > 1e-9 && outRms > 1e-9 ? fmt(" (gain %+.1f dB)", toDb(outRms / inRms)) : std::string())
                         + "; spectral centroid " + fmt("%.0f Hz", cin) + " in, " + fmt("%.0f Hz", cout) + " out.\n";
                if (request.spectrum != Bands::none && a < b && !(windowed && tooShort(part, rate)))
                {
                    const std::vector<double> bandsIn = bandLevels(stereoSpectrum(own.l, own.r, rate, { part }), request.spectrum);
                    out += bandsLine(request.spectrum, windowed ? "of its own input in the window" : "of its own input",
                                     bandLevels(stereoSpectrum(o.l, o.r, rate, { part }), request.spectrum), &bandsIn);
                }
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
                           + seconds(static_cast<double>(f.step.at) / rate) + ", " + f.near + (f.step.click ? " (a click)" : "");
                out += std::string(found.size() > 1 ? "Largest steps between two samples (each signal's largest, and its clicks): "
                                                     : "Largest step between two samples: ")
                     + (steps.empty() ? std::string("none (silent)") : steps) + ".\n";
                out += "Alerts: " + alertsText(alerts) + ".\n";
                out += "CPU: " + percent(cpuShare(cpu, audio)) + " of one core (" + fmt("%.2f ms", cpu) + " to render "
                     + seconds(audio) + ")" + cpuMoving + ".\n";
            }
        }
        for (const std::string& w : warnings)
            out += "Note: " + w + "\n";
        out += metricsLine(v);                               // the numbers, by the names checks use
        res.report = std::move(out);
        return res;
    }

    std::string run(const Sources& files, const Request& request)
    {
        Session session(files, request.rate);
        return session.measure(request).report;
    }
}
