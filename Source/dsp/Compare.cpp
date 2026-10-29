// Source/dsp/Compare.cpp: see Compare.h.
#include "dsp/Compare.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <numbers>

namespace playground::measure
{
    namespace
    {
        std::string fmt(const char* f, double v)
        {
            char buf[64];
            std::snprintf(buf, sizeof buf, f, v);
            return buf;
        }

        double toDb(double a) { return a > 1e-12 ? 20.0 * std::log10(a) : -240.0; }

        std::string dbfs(double a)
        {
            const double d = toDb(a);
            return d < -150.0 ? std::string("silent") : fmt("%.1f dBFS", d);
        }

        std::string seconds(double s) { return fmt(std::fabs(s) < 10.0 ? "%.3f s" : "%.2f s", s); }

        double rmsOf(const std::vector<float>& l, const std::vector<float>& r, std::size_t n)
        {
            double e = 0.0;
            for (std::size_t i = 0; i < n; ++i)
                e += 0.5 * (static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i]);
            return n > 0 ? std::sqrt(e / static_cast<double>(n)) : 0.0;
        }

        double peakOf(const std::vector<float>& l, const std::vector<float>& r, std::size_t n)
        {
            double p = 0.0;
            for (std::size_t i = 0; i < n; ++i)
                p = std::max({ p, static_cast<double>(std::fabs(l[i])), static_cast<double>(std::fabs(r[i])) });
            return p;
        }

        // The lag of B against A (b(t) ~ a(t - lag)) from the cross-correlation of their mono sums over the first 2^17
        // samples, within half a second (or half what is compared): at each lag normalised by the energies of the samples
        // that overlap there (a pure delay scores 1 at its lag, a period's alias less, even where the cut falls while a
        // note sounds); the highest |score|, the smaller lag on a tie. A lag whose overlap holds less than a millionth of
        // either side's energy is passed over.
        int bestLag(const Sound& a, const Sound& b, std::size_t n, double rate)
        {
            const std::size_t seg = std::min<std::size_t>(n, std::size_t{ 1 } << 17);
            if (seg < 64)
                return 0;
            std::size_t size = 1;
            while (size < 2 * seg)
                size *= 2;
            std::vector<std::complex<double>> x(size), y(size);
            std::vector<double> ex(seg + 1, 0.0), ey(seg + 1, 0.0);   // the energies before each sample
            for (std::size_t i = 0; i < seg; ++i)
            {
                const double u = 0.5 * (static_cast<double>(a.l[i]) + a.r[i]), v = 0.5 * (static_cast<double>(b.l[i]) + b.r[i]);
                x[i] = { u, 0.0 };
                y[i] = { v, 0.0 };
                ex[i + 1] = ex[i] + u * u;
                ey[i + 1] = ey[i] + v * v;
            }
            fft(x);
            fft(y);
            for (std::size_t k = 0; k < size; ++k)               // the inverse of conj(X) Y, through the forward FFT
                x[k] = std::conj(std::conj(x[k]) * y[k]);
            fft(x);
            const double least = 1e-6;
            const auto score = [&](long k) {
                const auto m = static_cast<std::size_t>(std::labs(k));
                // k >= 0: a[0, seg - k) against b[k, seg); k < 0: a[m, seg) against b[0, seg - m).
                const double fx = k >= 0 ? ex[seg - m] : ex[seg] - ex[m], fy = k >= 0 ? ey[seg] - ey[m] : ey[seg - m];
                if (!(fx > least * ex[seg] && fy > least * ey[seg] && fx * fy > 1e-60))
                    return 0.0;
                return std::fabs(x[static_cast<std::size_t>(k >= 0 ? k : static_cast<long>(size) + k)].real()) / std::sqrt(fx * fy);
            };
            const long most = static_cast<long>(std::min<double>(static_cast<double>(seg / 2), 0.5 * rate));
            long best = 0;
            double top = score(0);
            for (long k = 1; k <= most; ++k)
                for (const long s : { k, -k })
                    if (const double v = score(s); v > top * (1.0 + 1e-9))
                    {
                        top = v;
                        best = s;
                    }
            return static_cast<int>(best);
        }

        // Ten octave bands' energy of the mono sum (Welch: 8192-point Hann windows, half overlapped).
        std::array<double, 10> bandEnergy(const Sound& s, std::size_t n, double rate)
        {
            std::array<double, 10> e{};
            std::size_t size = 8192;
            while (size > 256 && size > n)
                size /= 2;
            std::vector<std::complex<double>> f(size);
            for (std::size_t a = 0; a == 0 || a + size <= n; a += size / 2)
            {
                for (std::size_t i = 0; i < size; ++i)
                {
                    const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(size));
                    const std::size_t t = a + i;
                    f[i] = { t < n ? 0.5 * (static_cast<double>(s.l[t]) + s.r[t]) * w : 0.0, 0.0 };
                }
                fft(f);
                for (std::size_t k = 1; k < size / 2; ++k)
                {
                    const double hz = static_cast<double>(k) * rate / static_cast<double>(size);
                    for (std::size_t band = 0; band < e.size(); ++band)
                    {
                        const double c = Comparison::kBands[band];
                        if (hz >= c / std::numbers::sqrt2 && hz < c * std::numbers::sqrt2)
                            e[band] += std::norm(f[k]);
                    }
                }
                if (a + size >= n)
                    break;
            }
            return e;
        }

        // A side rendered: its Session (null for a recording) and its sound.
        struct Rendered
        {
            std::unique_ptr<Session> session;
            Sound sound;
            std::string errors;
        };

        // `length` samples (0: as its request plays): a recording's, when the other side is one.
        Rendered renderSide(const CompareSide& side, double rate, std::size_t length)
        {
            Rendered out;
            if (side.recordedL != nullptr)
            {
                out.sound.compiled = true;
                out.sound.rate = rate;
                out.sound.l = *side.recordedL;
                out.sound.r = side.recordedR != nullptr ? *side.recordedR : out.sound.l;
                out.sound.r.resize(out.sound.l.size(), 0.0f);
                out.sound.title = "the recording";
                return out;
            }
            out.session = std::make_unique<Session>(side.files, rate);
            if (!out.session->compiled())
            {
                out.errors = out.session->errors();
                return out;
            }
            ViewRequest vr;
            vr.render = side.request;
            vr.render.rate = rate;
            vr.input = ViewInput::noise;                     // an effect's, unless the request names one
            vr.length = length;
            out.sound = renderSound(*out.session, vr);
            return out;
        }

        std::string hzText(double hz) { return hz >= 1000.0 ? fmt("%.0fk", hz / 1000.0) : fmt("%.0f", hz); }
    }

    Comparison compare(const CompareSide& sa, const CompareSide& sb, const CompareOptions& options)
    {
        Comparison c;
        const double rate = sa.request.rate > 1000.0 ? sa.request.rate : 48000.0;
        c.rate = rate;
        const std::size_t recorded = sa.recordedL != nullptr ? sa.recordedL->size() : sb.recordedL != nullptr ? sb.recordedL->size() : 0;
        Rendered ra = renderSide(sa, rate, recorded), rb = renderSide(sb, rate, recorded);
        if (!ra.errors.empty() || !rb.errors.empty())
        {
            c.error = (!ra.errors.empty() ? "A does not compile:\n" + ra.errors : std::string())
                    + (!rb.errors.empty() ? "B does not compile:\n" + rb.errors : std::string());
            return c;
        }
        const Sound& a = ra.sound;
        const Sound& b = rb.sound;
        c.lengthA = a.l.size();
        c.lengthB = b.l.size();
        const std::size_t n = std::min(c.lengthA, c.lengthB);
        c.length = n;
        if (c.lengthA != c.lengthB)
            c.lines.push_back("B is " + seconds(static_cast<double>(c.lengthB) / rate) + " long and A "
                              + seconds(static_cast<double>(c.lengthA) / rate) + ": compared over the first "
                              + seconds(static_cast<double>(n) / rate) + ".");
        c.identical = c.lengthA == c.lengthB && std::equal(a.l.begin(), a.l.end(), b.l.begin())
                   && std::equal(a.r.begin(), a.r.end(), b.r.begin());
        const double rmsA = rmsOf(a.l, a.r, n), rmsB = rmsOf(b.l, b.r, n);
        const std::string levels = "A peak " + dbfs(peakOf(a.l, a.r, n)) + ", RMS " + dbfs(rmsA) + "; B peak "
                                 + dbfs(peakOf(b.l, b.r, n)) + ", RMS " + dbfs(rmsB);
        const bool overlap = c.identical || (std::equal(a.l.begin(), a.l.begin() + static_cast<std::ptrdiff_t>(n), b.l.begin())
                                             && std::equal(a.r.begin(), a.r.begin() + static_cast<std::ptrdiff_t>(n), b.r.begin()));
        if (overlap)
        {
            c.lines.push_back(std::string(c.identical ? "Identical bits: every sample of both channels is the same, over "
                                                      : "Identical bits over the shorter one: every sample of both channels is "
                                                        "the same, over the first ")
                              + seconds(static_cast<double>(n) / rate) + " (peak " + dbfs(peakOf(a.l, a.r, n)) + ", RMS "
                              + dbfs(rmsA) + ").");
            c.a = std::move(ra.sound);
            c.b = std::move(rb.sound);
            return c;
        }

        // The null test.
        double e = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            const double dl = static_cast<double>(a.l[i]) - b.l[i], dr = static_cast<double>(a.r[i]) - b.r[i];
            e += 0.5 * (dl * dl + dr * dr);
            const double d = std::max(std::fabs(dl), std::fabs(dr));
            if (d > c.peakDiff)
            {
                c.peakDiff = d;
                c.peakAt = i;
            }
            if (d > 1e-6)
            {
                if (c.firstDiff < 0)
                    c.firstDiff = static_cast<long>(i);
                c.lastDiff = static_cast<long>(i);
            }
        }
        const double rmsD = n > 0 ? std::sqrt(e / static_cast<double>(n)) : 0.0;
        c.diffDb = rmsA > 1e-12 ? toDb(rmsD / rmsA) : toDb(rmsD);
        const std::string level = rmsA > 1e-12 ? fmt("%.1f dB", c.diffDb) + " against A (the RMS of A - B over A's)"
                                               : fmt("%.1f dBFS", c.diffDb) + " (A is silent)";
        if (c.firstDiff < 0)
            c.lines.push_back("They differ only below -120 dB (their last bits): the difference is " + level + ".");
        else
            c.lines.push_back("They differ: the difference is " + level + ", at most "
                              + fmt("%.4f", c.peakDiff) + " at " + seconds(static_cast<double>(c.peakAt) / rate)
                              + "; samples differ (above -120 dB) from " + seconds(static_cast<double>(c.firstDiff) / rate)
                              + " to " + seconds(static_cast<double>(c.lastDiff) / rate) + ".");
        c.lines.push_back("Levels: " + levels + ".");

        // The gain and the lag that fit B to A, and what is left.
        c.lag = bestLag(a, b, n, rate);
        {
            double ab = 0.0, aa = 0.0;
            const long lag = c.lag;
            for (std::size_t t = 0; t < n; ++t)
            {
                const long s = static_cast<long>(t) - lag;
                if (s < 0 || s >= static_cast<long>(n))
                    continue;
                const auto u = static_cast<std::size_t>(s);
                ab += static_cast<double>(b.l[t]) * a.l[u] + static_cast<double>(b.r[t]) * a.r[u];
                aa += static_cast<double>(a.l[u]) * a.l[u] + static_cast<double>(a.r[u]) * a.r[u];
            }
            c.gain = aa > 1e-30 ? ab / aa : 0.0;
            double left = 0.0, bb = 0.0;
            for (std::size_t t = 0; t < n; ++t)
            {
                const long s = static_cast<long>(t) - lag;
                const bool in = s >= 0 && s < static_cast<long>(n);
                const auto u = static_cast<std::size_t>(in ? s : 0);
                const double fl = in ? c.gain * a.l[u] : 0.0, fr = in ? c.gain * a.r[u] : 0.0;
                left += (b.l[t] - fl) * (b.l[t] - fl) + (b.r[t] - fr) * (b.r[t] - fr);
                bb += static_cast<double>(b.l[t]) * b.l[t] + static_cast<double>(b.r[t]) * b.r[t];
            }
            c.residualDb = bb > 1e-30 ? 10.0 * std::log10(std::max(left, 1e-300) / bb) : -240.0;
            std::string fit = "Best fit: B = ";
            if (std::fabs(c.gain - 1.0) < 1e-4)
                fit += "A";
            else
                fit += fmt("%.3f", c.gain) + " x A (" + fmt("%+.1f dB", toDb(std::fabs(c.gain))) + (c.gain < 0.0 ? ", inverted" : "")
                     + ")";
            fit += c.lag == 0 ? ", no delay" : (c.lag > 0 ? ", delayed by " : ", ahead by ") + std::to_string(std::abs(c.lag))
                                                  + (std::abs(c.lag) == 1 ? " sample (" : " samples (")
                                                  + fmt("%.2f ms)", std::abs(c.lag) / rate * 1000.0);
            fit += "; what is left after that is " + (c.residualDb < -200.0 ? std::string("nothing")
                                                                           : fmt("%.1f dB", c.residualDb) + " under B")
                 + (c.residualDb < -60.0 ? ": a gain and a delay explain the difference."
                                         : ": they differ in more than a gain and a delay.");
            c.lines.push_back(fit);
        }

        // The octave bands.
        {
            const std::array<double, 10> ea = bandEnergy(a, n, rate), eb = bandEnergy(b, n, rate);
            std::string bands;
            for (std::size_t k = 0; k < ea.size(); ++k)
            {
                if (Comparison::kBands[k] / std::numbers::sqrt2 >= 0.5 * rate)   // above the Nyquist frequency
                {
                    c.bandsDb[k] = std::nan("");
                    continue;
                }
                const bool silent = ea[k] < 1e-18 && eb[k] < 1e-18;
                c.bandsDb[k] = silent ? std::nan("") : std::clamp(10.0 * std::log10(std::max(eb[k], 1e-30) / std::max(ea[k], 1e-30)), -120.0, 120.0);
                bands += (bands.empty() ? "" : ", ") + hzText(Comparison::kBands[k]) + " "
                       + (silent ? std::string("silent") : fmt(std::fabs(c.bandsDb[k]) < 0.05 ? "%.1f" : "%+.1f", c.bandsDb[k]));
            }
            c.lines.push_back("Octave bands, B against A (dB, Hz): " + bands + ".");
        }

        // The metrics, side by side where they differ beyond their tolerance.
        if (options.metrics && ra.session != nullptr && rb.session != nullptr)
        {
            Request qa = sa.request, qb = sb.request;
            qa.rate = qb.rate = rate;
            for (const MetricInfo& m : metrics())
                if (std::string(m.name).rfind("cpu", 0) != 0)
                {
                    qa.want.insert(m.name);
                    qb.want.insert(m.name);
                }
            const Result ma = ra.session->measure(qa), mb = rb.session->measure(qb);
            c.metricsMeasured = true;
            for (const MetricInfo& m : metrics())
            {
                const auto ia = ma.values.find(m.name), ib = mb.values.find(m.name);
                if (std::string(m.name).rfind("cpu", 0) == 0 || ia == ma.values.end() || ib == mb.values.end())
                    continue;
                const double x = ia->second, y = ib->second;
                if (std::isnan(x) || std::isnan(y))
                    continue;
                const double tol = m.relative ? std::max(m.tolerance * std::max(std::fabs(x), std::fabs(y)), m.floor) : m.tolerance;
                const bool differs = std::isinf(x) || std::isinf(y) ? x != y : std::fabs(x - y) > tol;
                if (differs)
                    c.metrics.push_back({ m.name, x, y });
            }
            std::string line;
            for (std::size_t k = 0; k < c.metrics.size() && k < 6; ++k)
                line += (line.empty() ? "" : "; ") + c.metrics[k].metric + " " + valueText(c.metrics[k].metric, c.metrics[k].a) + " | "
                      + valueText(c.metrics[k].metric, c.metrics[k].b);
            if (c.metrics.size() > 6)
                line += "; and " + std::to_string(c.metrics.size() - 6) + " more";
            c.lines.push_back(c.metrics.empty() ? "The metrics agree within their tolerances."
                                                : "Metrics beyond their tolerance (A | B): " + line + ".");
        }
        else if (options.metrics)
            c.lines.push_back("The metrics are not compared: a side is a recording, not a render.");

        if (options.detail && n > 0)
        {
            std::string env;
            for (std::size_t part = 0; part < 32; ++part)
            {
                const std::size_t from = n * part / 32, to = std::max(from + 1, n * (part + 1) / 32);
                double d = 0.0, ref = 0.0;
                for (std::size_t i = from; i < to && i < n; ++i)
                {
                    const double dl = static_cast<double>(a.l[i]) - b.l[i], dr = static_cast<double>(a.r[i]) - b.r[i];
                    d += dl * dl + dr * dr;
                    ref += static_cast<double>(a.l[i]) * a.l[i] + static_cast<double>(a.r[i]) * a.r[i];
                }
                const double db = d <= 0.0 ? -240.0 : ref > 0.0 ? 10.0 * std::log10(d / ref) : 10.0 * std::log10(d / static_cast<double>(to - from));
                c.envelopeDb.push_back(static_cast<float>(db));
                env += (part == 0 ? "" : " ") + (db < -200.0 ? std::string("same") : fmt("%.0f", db));
            }
            c.lines.push_back("The difference over time (32 parts of " + seconds(static_cast<double>(n) / 32.0 / rate)
                              + ", dB under A's level there; same: no difference): " + env + ".");
        }
        c.a = std::move(ra.sound);
        c.b = std::move(rb.sound);
        return c;
    }
}
