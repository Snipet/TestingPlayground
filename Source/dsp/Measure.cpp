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

        void fft(std::vector<std::complex<double>>& a)
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

        // The program over `in` (an effect) or `total` samples (an instrument, `events` by absolute sample).
        Rendered render(std::unique_ptr<Patch> patch, double rate, const Dials& dials, const Stereo* in,
                        std::size_t total, const std::vector<std::pair<std::size_t, Event>>& events,
                        std::size_t watchFrom)
        {
            Rendered r;
            Engine e;
            e.prepare(rate, kBlock);
            e.offer(std::move(patch));
            e.prepare(rate, kBlock);                     // takes it at once, without a fade
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
                e.setControls(dials.norm);
                e.process(ch, 2, n, block);
                r.cpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                pos += static_cast<std::size_t>(n);
                if (kInstrument && r.endedAt < 0 && pos > watchFrom && e.activeVoices() == 0)
                    r.endedAt = static_cast<long>(pos);
            }
            r.alerts = e.takeAlerts();
            e.collect();
            return r;
        }

        std::string cpuLine(double ms, double audioSeconds, int voices)
        {
            const double share = audioSeconds > 0.0 ? ms / (audioSeconds * 1000.0) * 100.0 : 0.0;
            std::string s = "CPU: " + fmt("%.1f ms", ms) + " to render " + seconds(audioSeconds) + " ("
                          + fmt(share < 0.1 ? "%.3f" : "%.2f", share) + "% of one core in real time";
            if (kInstrument && voices > 0)
                s += fmt(", about %.2f%% for 16 voices at once", share / voices * 16.0);
            return s + ").";
        }
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
        fft(a);
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
        fft(a);
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

    std::string run(const Sources& files, const Request& request)
    {
        const double rate = request.rate > 1000.0 ? request.rate : 48000.0;
        const float hold = std::clamp(request.holdSeconds, 0.05f, 10.0f);
        const float tail = std::clamp(request.tailSeconds, 0.0f, 10.0f);
        const auto compile = [&](std::string* errors) {
            Patch::Result r = Patch::compile(files, static_cast<float>(rate));
            if (errors != nullptr && !r.patch)
                *errors = r.errors + r.notes;
            return std::move(r.patch);
        };
        std::string errors;
        std::unique_ptr<Patch> first = compile(&errors);
        if (!first)
            return "It did not compile, so nothing was rendered:\n" + errors;
        const Dials dials = dialsFor(first->controls(), request);

        std::string out = "Rendered offline at " + fmt("%.0f", rate) + " Hz, in blocks of " + std::to_string(kBlock)
                        + ". Dials: " + dials.text + ".\n";
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
            std::vector<int> notes;
            for (const int n : request.notes)
                if (n >= 0 && n <= 127 && std::find(notes.begin(), notes.end(), n) == notes.end()
                    && notes.size() < static_cast<std::size_t>(Patch::kVoices))
                    notes.push_back(n);
            if (notes.empty())
                notes.push_back(48);
            const float vel = std::clamp(request.velocity, 0.0f, 1.0f);
            const auto eventsFor = [&](const std::vector<int>& ns, std::size_t offAt) {
                std::vector<std::pair<std::size_t, Event>> ev;
                for (const int n : ns)
                    ev.push_back({ 0, Event{ 0, EventKind::noteOn, static_cast<uint8_t>(n), vel } });
                for (const int n : ns)
                    ev.push_back({ offAt, Event{ 0, EventKind::noteOff, static_cast<uint8_t>(n), 0.0f } });
                return ev;
            };
            const std::size_t total = holdN + tailN;
            const Rendered r = render(std::move(first), rate, dials, nullptr, total, eventsFor(notes, holdN), holdN);

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
            out += "Held: peak " + dbfs(heldPeak) + ", RMS " + dbfs(heldRms) + ", DC " + fmt("%.4f", dc(r.out, settle, holdN))
                 + ", left/right correlation " + fmt("%.2f", corr) + fmt(" (balance %+.1f dB).\n", bal);
            if (heldRms < 1e-5)
                warnings.push_back("It is silent (or nearly) while the note is held.");
            if (heldPeak > 1.0)
                warnings.push_back("Peaks above 0 dBFS with " + std::to_string(notes.size())
                                   + " note(s): a voice should peak near 0.2 to 0.3, since sixteen can sound at once.");

            double centroid = 0.0;
            const std::vector<Peak> peaks = spectrumPeaks(mono(r.out, settle, holdN), rate, 8, &centroid);
            out += "Spectrum while held: centroid " + fmt("%.0f Hz", centroid) + "; strongest partials " + partials(peaks)
                 + ".";
            const Peak* lowest = nullptr;
            for (const Peak& p : peaks)
                if (p.db > -30.0 && p.hz > 15.0)
                {
                    lowest = &p;
                    break;
                }
            if (lowest != nullptr)
                out += " The lowest within 30 dB of the strongest is " + fmt("%.1f Hz", lowest->hz) + "; the lowest note is "
                     + fmt("%.1f Hz", noteHz(*std::min_element(notes.begin(), notes.end()))) + ".";
            out += "\n";

            std::size_t at = 0;
            const double step = largestStep(r.out, at);
            // A wave's own edges come every cycle: windows of a cycle and a half of the lowest note, at least 10 ms.
            const double lowestHz = noteHz(*std::min_element(notes.begin(), notes.end()));
            const std::size_t window = static_cast<std::size_t>(std::max(0.01 * rate, 1.5 * rate / lowestHz));
            const double usual = usualStep(r.out, settle, holdN, window);
            out += "Largest step between two samples: " + fmt("%.3f", step) + " at " + seconds(static_cast<double>(at) / rate)
                 + (heldPeak > 0.0 ? fmt(" (%.0f%% of the held peak", step / heldPeak * 100.0) : std::string(" ("))
                 + (heldPeak > 0.0 ? "; " : "") + "the held sound's own largest steps, every cycle: " + fmt("%.3f", usual)
                 + ").\n";
            if (heldPeak > 0.0 && step > 0.35 * heldPeak && step > 0.05 && step > 2.0 * usual)
                warnings.push_back("A step that large, and far larger than the sound's own, is likely a click (an envelope "
                                   "or a phase that jumps).");

            const long fall = fallTime(r.out, holdN, toDb(heldRms), 60.0, rate);
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
                // What is no harmonic of the note: aliasing, unless the sound means it.
                const Peak odd = inharmonic(mono(r.out, settle, holdN), rate, noteHz(n), 20000.0);
                if (heldRms >= 1e-5)
                {
                    out += "Inharmonic: ";
                    if (odd.db < -90.0)
                        out += "nothing below 20 kHz that is no harmonic of the note, down to -90 dB under the strongest "
                               "component (the measurement's floor).\n";
                    else
                        out += "the strongest component below 20 kHz that is no harmonic of the note is " + fmt("%.0f dB", odd.db)
                             + " under the strongest, at " + fmt("%.0f Hz", odd.hz)
                             + ": aliasing, or the sound's own (detuning, FM, noise). To test an oscillator's aliasing, measure"
                               " a high note (96 and up) with its detune and noise at 0.\n";
                }
                std::vector<int> chord{ n };
                for (const int k : { 4, 7, 12 })
                    if (n + k <= 127)
                        chord.push_back(n + k);
                if (std::unique_ptr<Patch> again = compile(nullptr))
                {
                    const std::size_t chordN = std::min(holdN, static_cast<std::size_t>(0.6 * rate));
                    const Rendered c = render(std::move(again), rate, dials, nullptr, chordN, eventsFor(chord, chordN), chordN);
                    std::string names;
                    for (const int k : chord)
                        names += (names.empty() ? "" : " ") + std::to_string(k);
                    const double cp = peak(c.out, 0, chordN);
                    out += "A chord (" + names + ") at the same velocity: peak " + dbfs(cp) + ", RMS "
                         + dbfs(rms(c.out, std::min(chordN / 2, settle), chordN)) + ".\n";
                    if (cp > 1.0)
                        warnings.push_back("The chord peaks above 0 dBFS: lower the voice's level.");
                }
            }
            out += "Alerts: " + alertsText(r.alerts) + ".\n";
            out += cpuLine(r.cpuMs, static_cast<double>(total) / rate, static_cast<int>(notes.size())) + "\n";
        }
        else
        {
            // The test signals, each through a fresh copy of the program.
            const auto signal = [&](std::size_t n, const auto& fill) {
                Stereo s;
                s.l.resize(n);
                s.r.resize(n);
                for (std::size_t i = 0; i < n; ++i)
                    s.l[i] = s.r[i] = fill(i);
                return s;
            };
            const float sineAmp = static_cast<float>(std::pow(10.0, -12.0 / 20.0));
            const Stereo sine = signal(holdN, [&](std::size_t i) {
                return sineAmp * static_cast<float>(std::sin(2.0 * std::numbers::pi * 220.0 * static_cast<double>(i) / rate));
            });
            uint32_t seed = 22222;                           // a fixed noise: the same report each time
            const float noiseAmp = static_cast<float>(std::pow(10.0, -18.0 / 20.0) * std::sqrt(3.0));
            Stereo noise = signal(holdN, [&](std::size_t) {
                seed = seed * 1664525u + 1013904223u;
                return noiseAmp * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
            });
            const std::size_t impulseN = static_cast<std::size_t>(0.01 * rate) + tailN;
            const Stereo impulse = signal(impulseN, [&](std::size_t i) { return i == 0 ? 0.5f : 0.0f; });
            const std::size_t silenceN = static_cast<std::size_t>(0.5 * rate);
            const Stereo silence = signal(silenceN, [](std::size_t) { return 0.0f; });

            double cpu = 0.0, audio = 0.0;
            uint32_t alerts = 0;
            double worstStep = 0.0, worstStepAt = 0.0;
            std::string worstStepWhere;
            const auto through = [&](const Stereo& in, std::unique_ptr<Patch> p, const char* name) {
                Rendered r = render(std::move(p), rate, dials, &in, in.size(), {}, 0);
                cpu += r.cpuMs;
                audio += static_cast<double>(in.size()) / rate;
                alerts |= r.alerts;
                std::size_t at = 0;
                const double s = largestStep(r.out, at);
                if (s > worstStep)
                {
                    worstStep = s;
                    worstStepAt = static_cast<double>(at) / rate;
                    worstStepWhere = name;
                }
                return std::move(r.out);
            };

            const std::size_t settle = std::min(holdN / 2, static_cast<std::size_t>(0.05 * rate));
            {
                const Stereo o = through(sine, std::move(first), "the sine");
                const double inRms = rms(sine, settle, holdN), outRms = rms(o, settle, holdN);
                double corr = 1.0, bal = 0.0;
                stereo(o, settle, holdN, corr, bal);
                out += "Sine 220 Hz at -12.0 dBFS for " + seconds(hold) + ": out peak " + dbfs(peak(o, 0, holdN)) + ", RMS "
                     + dbfs(outRms) + (outRms > 1e-9 ? fmt(" (gain %+.1f dB)", toDb(outRms / inRms)) : std::string())
                     + ", DC " + fmt("%.4f", dc(o, settle, holdN)) + ", left/right correlation " + fmt("%.2f", corr)
                     + fmt(" (balance %+.1f dB)", bal) + ". Partials out: "
                     + partials(spectrumPeaks(mono(o, settle, holdN), rate, 8)) + ".\n";
                if (outRms < 1e-5)
                    warnings.push_back("The sine comes out silent (or nearly).");
                if (peak(o, 0, holdN) > 1.0)
                    warnings.push_back("Peaks above 0 dBFS from a -12 dBFS sine: the effect adds a lot of gain.");
            }
            if (std::unique_ptr<Patch> p = compile(nullptr))
            {
                const Stereo o = through(noise, std::move(p), "the noise");
                const double inRms = rms(noise, settle, holdN), outRms = rms(o, settle, holdN);
                double corr = 1.0, bal = 0.0;
                stereo(o, settle, holdN, corr, bal);
                double cin = 0.0, cout = 0.0;
                spectrumPeaks(mono(noise, settle, holdN), rate, 0, &cin);
                spectrumPeaks(mono(o, settle, holdN), rate, 0, &cout);
                out += "Noise at -18.0 dBFS RMS for " + seconds(hold) + ": out RMS " + dbfs(outRms)
                     + (outRms > 1e-9 ? fmt(" (gain %+.1f dB)", toDb(outRms / inRms)) : std::string())
                     + ", left/right correlation " + fmt("%.2f", corr) + "; spectral centroid " + fmt("%.0f Hz", cin)
                     + " in, " + fmt("%.0f Hz", cout) + " out.\n";
            }
            if (std::unique_ptr<Patch> p = compile(nullptr))
            {
                const Stereo o = through(impulse, std::move(p), "the impulse");
                std::size_t at = 0;
                double pk = 0.0;
                for (std::size_t i = 0; i < o.size(); ++i)
                {
                    const double v = std::max(std::fabs(static_cast<double>(o.l[i])), std::fabs(static_cast<double>(o.r[i])));
                    if (v > pk)
                    {
                        pk = v;
                        at = i;
                    }
                }
                out += "An impulse (0.5), then " + seconds(tail) + " of silence: ";
                if (pk < 1e-7)
                    out += "no response.\n";
                else
                {
                    const long fall = fallTime(o, at, toDb(pk), 60.0, rate);
                    out += "the response peaks at " + dbfs(pk) + " after " + fmt("%.1f ms", static_cast<double>(at) / rate * 1000.0)
                         + (fall < 0 ? "; still within 60 dB of that peak at the end (a long tail, or one that does not decay)"
                                     : "; it falls 60 dB below that peak " + seconds(static_cast<double>(fall) / rate) + " later")
                         + ".\n";
                }
            }
            if (std::unique_ptr<Patch> p = compile(nullptr))
            {
                const Stereo o = through(silence, std::move(p), "silence");
                const double r = rms(o, 0, silenceN);
                out += "Silence in, " + seconds(0.5) + ": out RMS " + dbfs(r) + ".\n";
                if (r > 1e-4)
                    warnings.push_back("It makes sound from silence (self-oscillation, DC or noise of its own).");
            }
            out += "Largest step between two samples: " + fmt("%.3f", worstStep) + " (in " + worstStepWhere + ", at "
                 + seconds(worstStepAt) + ").\n";
            out += "Alerts: " + alertsText(alerts) + ".\n";
            out += cpuLine(cpu, audio, 0) + "\n";
        }
        for (const std::string& w : warnings)
            out += "Note: " + w + "\n";
        return out;
    }
}
