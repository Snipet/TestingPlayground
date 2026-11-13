// Source/dsp/Compare.cpp: see Compare.h.
#include "dsp/Compare.h"

#include "dsp/Export.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string_view>

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

        // The octave bands' levels (Measure.h's bandLevels) of both channels over the first n samples, in kBandSeconds
        // Hann frames: the same stretch of sound at every rate.
        std::vector<double> bandsOver(const Sound& s, std::size_t n, double rate)
        {
            return bandLevels(stereoSpectrum(s.l, s.r, rate, { { 0, n } }), Bands::octave);
        }

        // A side rendered: its Session (null for a recording) and its sound.
        struct Rendered
        {
            std::unique_ptr<Session> session;
            Sound sound;
            std::string errors;
        };

        // The octave bands, B against A, each side over its first samples (na of A at its rate, nb of B at its), into
        // c.bandsDb and a line. A band above either side's Nyquist frequency, or silent in both, is NaN.
        void compareBands(Comparison& c, const Sound& a, std::size_t na, const Sound& b, std::size_t nb)
        {
            const std::vector<double> la = bandsOver(a, na, c.rate), lb = bandsOver(b, nb, c.rateB);
            std::string bands;
            for (std::size_t k = 0; k < c.bandsDb.size() && k < la.size() && k < lb.size(); ++k)
            {
                if (std::isnan(la[k]) || std::isnan(lb[k]))   // above the Nyquist frequency
                {
                    c.bandsDb[k] = std::nan("");
                    continue;
                }
                const bool silent = la[k] < -220.0 && lb[k] < -220.0;
                c.bandsDb[k] = silent ? std::nan("") : std::clamp(lb[k] - la[k], -120.0, 120.0);
                bands += (bands.empty() ? "" : ", ") + bandName(Comparison::kBands[k]) + " "
                       + (silent ? std::string("silent") : fmt(std::fabs(c.bandsDb[k]) < 0.05 ? "%.1f" : "%+.1f", c.bandsDb[k]));
            }
            c.lines.push_back("Octave bands, B against A (dB, Hz): " + bands + ".");
        }

        // The metrics a rate changes by their definition, set aside at two rates: step (a smooth wave's steps are
        // smaller at a higher rate), and an effect's noise_gain_db, noise_lufs and impulse_peak_db (white noise and a
        // one-sample impulse spread over the rate's whole band, so a filter's share of them moves with the rate).
        constexpr const char* kRateBound[] = { "step", "noise_gain_db", "noise_lufs", "impulse_peak_db" };

        bool rateBound(const std::string& name)
        {
            return std::any_of(std::begin(kRateBound), std::end(kRateBound), [&](const char* m) { return name == m; });
        }

        // The metrics, side by side where they differ beyond their tolerance (the CPU's aside, which no two renders
        // share; at two rates those rateBound too), into c.metrics and a line.
        void compareMetrics(Comparison& c, Session& sa, Session& sb, Request qa, Request qb)
        {
            const auto aside = [&](const std::string& name) { return name.rfind("cpu", 0) == 0 || (c.twoRates() && rateBound(name)); };
            qa.rate = c.rate;
            qb.rate = c.rateB;
            for (const MetricInfo& m : metrics())
                if (!aside(m.name))
                {
                    qa.want.insert(m.name);
                    qb.want.insert(m.name);
                }
            const Result ma = sa.measure(qa);
            const Result mb = ma.cancelled ? Result{} : sb.measure(qb);
            if (ma.cancelled || mb.cancelled)                // a cancel (CompareOptions::cancel): nothing compared
            {
                c.error = (ma.cancelled ? sa.cancelWords() : sb.cancelWords()) + " Nothing was compared.";
                return;
            }
            c.metricsMeasured = true;
            for (const MetricInfo& m : metrics())
            {
                const auto ia = ma.values.find(m.name), ib = mb.values.find(m.name);
                if (aside(m.name) || ia == ma.values.end() || ib == mb.values.end())
                    continue;
                const double x = ia->second, y = ib->second;
                if (std::isnan(x) || std::isnan(y))
                    continue;
                const double tol = m.relative ? std::max(m.tolerance * std::max(std::fabs(x), std::fabs(y)), m.floor) : m.tolerance;
                // At two rates inharmonic_db under its floor (-90 dB: none) on both sides agrees: what is left there is
                // the frames' own leakage, which the rate moves.
                const bool floored = c.twoRates() && std::string_view(m.name) == "inharmonic_db" && x < -90.0 && y < -90.0;
                const bool differs = std::isinf(x) || std::isinf(y) ? x != y : std::fabs(x - y) > tol && !floored;
                if (differs)
                    c.metrics.push_back({ m.name, x, y });
            }
            std::string line;
            for (std::size_t k = 0; k < c.metrics.size() && k < 6; ++k)
                line += (line.empty() ? "" : "; ") + c.metrics[k].metric + " " + valueText(c.metrics[k].metric, c.metrics[k].a) + " | "
                      + valueText(c.metrics[k].metric, c.metrics[k].b);
            if (c.metrics.size() > 6)
                line += "; and " + std::to_string(c.metrics.size() - 6) + " more";
            std::string stepAside;                           // the rate's own metrics, named
            if (c.twoRates())
            {
                std::vector<std::string> names;
                for (const MetricInfo& m : metrics())
                    if (rateBound(m.name))
                        names.emplace_back(m.name);
                std::string list;
                for (std::size_t k = 0; k < names.size(); ++k)
                    list += (k == 0 ? "" : k + 1 == names.size() ? " and " : ", ") + names[k];
                stepAside = " (" + list + (names.size() == 1 ? " is" : " are") + " set aside: the rate moves "
                          + (names.size() == 1 ? "it" : "them") + " by definition)";
            }
            c.lines.push_back(c.metrics.empty() ? "The metrics agree within their tolerances" + stepAside + "."
                                                : "Metrics beyond their tolerance (A | B): " + line + stepAside + ".");
        }

        // `length` samples (0: as its request plays): a recording's, when the other side is one. An effect's input is the
        // request's, else `input`.
        Rendered renderSide(const CompareSide& side, double rate, std::size_t length, ViewInput input, const Cancel& cancel)
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
            out.session = std::make_unique<Session>(side.files, rate, side.pins.empty() ? nullptr : &side.pins,
                                                    side.request.assets);
            if (!out.session->compiled())
            {
                out.errors = out.session->errors();
                return out;
            }
            out.session->setCancel(cancel);
            ViewRequest vr;
            vr.render = side.request;
            vr.render.rate = rate;
            vr.input = input;                                // an effect's, unless the request names one
            vr.length = length;
            out.sound = renderSound(*out.session, vr);
            return out;
        }

        // Two rates (Comparison::twoRates): no sample lines up with another, so the sides are compared by what is the
        // same at every rate, over the shorter side's seconds: the levels, the octave bands, the metrics and, with
        // detail, B's level against A's in 32 parts.
        void acrossRates(Comparison& c, const Rendered& ra, const Rendered& rb, const CompareSide& sa, const CompareSide& sb,
                         const CompareOptions& options)
        {
            const Sound& a = ra.sound;
            const Sound& b = rb.sound;
            c.lengthA = a.l.size();
            c.lengthB = b.l.size();
            const double secondsA = static_cast<double>(c.lengthA) / c.rate, secondsB = static_cast<double>(c.lengthB) / c.rateB;
            const double both = std::min(secondsA, secondsB);
            const std::size_t na = std::min(c.lengthA, static_cast<std::size_t>(std::llround(both * c.rate)));
            const std::size_t nb = std::min(c.lengthB, static_cast<std::size_t>(std::llround(both * c.rateB)));
            c.length = na;
            c.lines.push_back("A renders at " + fmt("%g", c.rate) + " Hz and B at " + fmt("%g", c.rateB)
                              + " Hz: compared by what is the same at every rate (the levels, the octave bands, the metrics), "
                                "not sample by sample.");
            if (std::fabs(secondsA - secondsB) > 1.0 / std::min(c.rate, c.rateB))
                c.lines.push_back("B is " + seconds(secondsB) + " long and A " + seconds(secondsA) + ": compared over the first "
                                  + seconds(both) + ".");
            const double rmsA = rmsOf(a.l, a.r, na), rmsB = rmsOf(b.l, b.r, nb);
            c.lines.push_back("Levels: A peak " + dbfs(peakOf(a.l, a.r, na)) + ", RMS " + dbfs(rmsA) + "; B peak "
                              + dbfs(peakOf(b.l, b.r, nb)) + ", RMS " + dbfs(rmsB)
                              + (rmsA > 1e-12 && rmsB > 1e-12 ? fmt(" (B's RMS %+.1f dB against A's)", toDb(rmsB / rmsA)) : std::string())
                              + ".");
            compareBands(c, a, na, b, nb);
            if (options.metrics && ra.session != nullptr && rb.session != nullptr)
                compareMetrics(c, *ra.session, *rb.session, sa.request, sb.request);
            else if (options.metrics)
                c.lines.push_back("The metrics are not compared: a side is a recording or a file, not a render.");
            if (options.detail && na > 0 && nb > 0)
            {
                std::string env;
                for (std::size_t part = 0; part < 32; ++part)
                {
                    const auto energy = [part](const Sound& s, std::size_t n) {
                        const std::size_t from = n * part / 32, to = std::max(from + 1, n * (part + 1) / 32);
                        double e = 0.0;
                        for (std::size_t i = from; i < to && i < n; ++i)
                            e += static_cast<double>(s.l[i]) * s.l[i] + static_cast<double>(s.r[i]) * s.r[i];
                        return e / static_cast<double>(to - from);
                    };
                    const double ea = energy(a, na), eb = energy(b, nb);
                    const bool silent = ea <= 1e-24 && eb <= 1e-24;
                    const double db = silent ? 0.0
                                             : std::clamp(10.0 * std::log10(std::max(eb, 1e-30) / std::max(ea, 1e-30)), -120.0, 120.0);
                    c.envelopeDb.push_back(static_cast<float>(db));
                    env += (part == 0 ? "" : " ") + (silent ? std::string("silent") : fmt(std::fabs(db) < 0.05 ? "%.1f" : "%+.1f", db));
                }
                c.lines.push_back("B's level against A's over time (32 parts of " + seconds(both / 32.0) + ", dB): " + env + ".");
            }
        }
    }

    Comparison compare(const CompareSide& sa, const CompareSide& sb, const CompareOptions& options)
    {
        Comparison c;
        const double rate = sa.request.rate > 1000.0 ? sa.request.rate : 48000.0;
        c.rate = rate;
        c.rateB = sb.request.rate > 1000.0 ? sb.request.rate : rate;
        if (c.twoRates() && std::min(c.rate, c.rateB) < kMinRateAcross)
        {
            c.error = "At two rates both are " + fmt("%g", kMinRateAcross) + " Hz or more (20 Hz to 20 kHz on both sides): A is at "
                    + fmt("%g", c.rate) + " Hz and B at " + fmt("%g", c.rateB) + " Hz.";
            return c;
        }
        // A recording's length sets both sides' (in seconds: each in its own rate's samples).
        const double recorded = sa.recordedL != nullptr   ? static_cast<double>(sa.recordedL->size()) / c.rate
                              : sb.recordedL != nullptr ? static_cast<double>(sb.recordedL->size()) / c.rateB
                                                        : 0.0;
        const auto lengthAt = [recorded](double r) { return static_cast<std::size_t>(std::llround(recorded * r)); };
        // An effect's input unless the request names one: noise, or at two rates the sweep (20 Hz to 20 kHz at -12
        // dBFS, the same at every rate; white noise's level per hertz falls as the rate rises).
        const ViewInput input = c.twoRates() ? ViewInput::sweep : ViewInput::noise;
        // The cancel, its time limit the call's over both sides (Cancel::started), each side's renders named its own.
        const auto sideCancel = [k = options.cancel.started()](const char* who) {
            Cancel s = k;
            s.who = who;
            return s;
        };
        Rendered ra = renderSide(sa, c.rate, lengthAt(c.rate), input, sideCancel("A"));
        Rendered rb = ra.sound.cancelled ? Rendered{} : renderSide(sb, c.rateB, lengthAt(c.rateB), input, sideCancel("B"));
        if (ra.sound.cancelled || rb.sound.cancelled)
        {
            c.error = (ra.sound.cancelled ? ra.session->cancelWords() : rb.session->cancelWords()) + " Nothing was compared.";
            return c;
        }
        if (!ra.errors.empty() || !rb.errors.empty())
        {
            c.error = (!ra.errors.empty() ? "A does not compile:\n" + ra.errors : std::string())
                    + (!rb.errors.empty() ? "B does not compile:\n" + rb.errors : std::string());
            return c;
        }
        c.hashA = renderHash(ra.sound.l, ra.sound.r);
        c.hashB = renderHash(rb.sound.l, rb.sound.r);
        if (c.twoRates())
        {
            acrossRates(c, ra, rb, sa, sb, options);
            c.a = std::move(ra.sound);
            c.b = std::move(rb.sound);
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

        compareBands(c, a, n, b, n);
        if (options.metrics && ra.session != nullptr && rb.session != nullptr)
            compareMetrics(c, *ra.session, *rb.session, sa.request, sb.request);
        else if (options.metrics)
            c.lines.push_back("The metrics are not compared: a side is a recording or a file, not a render.");

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
