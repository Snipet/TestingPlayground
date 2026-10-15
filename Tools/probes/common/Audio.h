// Tools/probes/common/Audio.h: small, exact helpers for audio probes (header-only, JUCE-free): a stereo buffer, test
// signals, levels, a single-bin DFT (plain or under a Hann window), the largest sample step and the steps of a switch
// (a click), where a transition settles, a frequency estimate, a 32-bit float WAV writer, and for reverbs the energy
// decay curve with its T60 fit and a band-pass. Analysis runs in double.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <string>
#include <vector>

namespace probe
{
    // Two channels of equal length.
    struct Stereo
    {
        std::vector<float> l, r;

        explicit Stereo(std::size_t n = 0) : l(n, 0.0f), r(n, 0.0f) {}
        std::size_t size() const noexcept { return l.size(); }
    };

    inline double dbFromLin(double x) { return 20.0 * std::log10(std::max(x, 1e-30)); }

    // A sine of `amp` at `hz`, `n` samples at `fs`, starting at phase 0.
    inline std::vector<float> sine(double hz, double amp, double fs, std::size_t n)
    {
        std::vector<float> v(n);
        for (std::size_t i = 0; i < n; ++i)
            v[i] = static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / fs));
        return v;
    }

    // RMS over [from, from + count), in dBFS.
    inline double rmsDb(const std::vector<float>& x, std::size_t from, std::size_t count)
    {
        double s = 0.0;
        const std::size_t end = std::min(x.size(), from + count);
        for (std::size_t i = from; i < end; ++i)
            s += static_cast<double>(x[i]) * static_cast<double>(x[i]);
        const std::size_t n = end > from ? end - from : 1;
        return dbFromLin(std::sqrt(s / static_cast<double>(n)));
    }

    // The amplitude of the component at `hz` over [from, from + count), from a single DFT bin (Goertzel-free, exact in
    // double). Use a whole number of cycles for an exact result; returns linear amplitude.
    inline double toneAmplitude(const std::vector<float>& x, double hz, double fs, std::size_t from, std::size_t count)
    {
        double re = 0.0, im = 0.0;
        const std::size_t end = std::min(x.size(), from + count);
        for (std::size_t i = from; i < end; ++i)
        {
            const double ph = 2.0 * std::numbers::pi * hz * static_cast<double>(i) / fs;
            re += static_cast<double>(x[i]) * std::cos(ph);
            im += static_cast<double>(x[i]) * std::sin(ph);
        }
        const std::size_t n = end > from ? end - from : 1;
        return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(n);
    }

    // As toneAmplitude, under a Hann window: other components leak far less (a tone 20 Hz away over one second
    // contributes under -100 dB), so it separates the notes of a chord. Exact for a steady tone at `hz` whatever the
    // number of cycles, but for its own image at -hz (negligible over many cycles); returns linear amplitude.
    inline double toneAmplitudeHann(const std::vector<float>& x, double hz, double fs, std::size_t from,
                                    std::size_t count)
    {
        double re = 0.0, im = 0.0, wsum = 0.0;
        const std::size_t end = std::min(x.size(), from + count);
        const double len = static_cast<double>(end > from ? end - from : 1);
        for (std::size_t i = from; i < end; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * (static_cast<double>(i - from) + 0.5) / len);
            const double ph = 2.0 * std::numbers::pi * hz * static_cast<double>(i) / fs;
            re += w * static_cast<double>(x[i]) * std::cos(ph);
            im += w * static_cast<double>(x[i]) * std::sin(ph);
            wsum += w;
        }
        return wsum > 0.0 ? 2.0 * std::sqrt(re * re + im * im) / wsum : 0.0;
    }

    // The largest step between neighbouring samples over [from, from + count): a click shows as a step far above a
    // steady signal's (a sine of amplitude A at f steps at most 2 pi f A / fs).
    inline double maxStep(const std::vector<float>& x, std::size_t from, std::size_t count)
    {
        double m = 0.0;
        const std::size_t end = std::min(x.size(), from + count);
        for (std::size_t i = std::max<std::size_t>(from, 1); i < end; ++i)
            m = std::max(m, std::fabs(static_cast<double>(x[i]) - static_cast<double>(x[i - 1])));
        return m;
    }

    // A switch at sample `at` (the first sample of the new state) from the steady signal `before` to the steady signal
    // `after`, as `x` makes it: the largest step of `x` from `pad` samples before the switch to `pad` samples after its
    // fade (`fade` samples long), over the larger of the two steady signals' own largest steps in that span. A switch
    // that does not click reads about 1 (a crossfade adds only its own slow change); a hard switch reads the jump it
    // makes over that slope. `abruptOverOwn` is that jump, |after[at] - before[at - 1]|, over the same slope: what a
    // hard switch at this sample would read, which shows that a row on stepOverOwn can fail there.
    struct EdgeSteps
    {
        double stepOverOwn = 0.0;
        double abruptOverOwn = 0.0;
    };

    inline EdgeSteps edgeSteps(const std::vector<float>& x, const std::vector<float>& before,
                               const std::vector<float>& after, std::size_t at, std::size_t fade, std::size_t pad)
    {
        const std::size_t from = at > pad ? at - pad : 0, count = at - from + fade + pad;
        const double own = std::max({ maxStep(before, from, count), maxStep(after, from, count), 1e-30 });
        const double abrupt = at > 0 && at < after.size() && at <= before.size()
            ? std::fabs(static_cast<double>(after[at]) - static_cast<double>(before[at - 1])) : 0.0;
        return { maxStep(x, from, count) / own, abrupt / own };
    }

    // Where a transition that began at `from` has settled: the first index in [from, to] from which `x` equals `want`,
    // bit for bit, up to `to` (`to` itself when x[to - 1] differs).
    inline std::size_t settledFrom(const std::vector<float>& x, const std::vector<float>& want, std::size_t from,
                                   std::size_t to)
    {
        std::size_t s = std::min({ to, x.size(), want.size() });
        while (s > from && std::memcmp(&x[s - 1], &want[s - 1], sizeof(float)) == 0)
            --s;
        return s;
    }

    // The frequency of a periodic signal from its rising zero crossings over [from, from + count), each crossing placed
    // by linear interpolation; 0 when fewer than two crossings.
    inline double frequencyHz(const std::vector<float>& x, double fs, std::size_t from, std::size_t count)
    {
        double first = -1.0, last = -1.0;
        int crossings = 0;
        const std::size_t end = std::min(x.size(), from + count);
        for (std::size_t i = from + 1; i < end; ++i)
        {
            const double a = x[i - 1], b = x[i];
            if (a < 0.0 && b >= 0.0)
            {
                const double t = static_cast<double>(i - 1) + a / (a - b);
                if (first < 0.0)
                    first = t;
                last = t;
                ++crossings;
            }
        }
        if (crossings < 2)
            return 0.0;
        return fs * static_cast<double>(crossings - 1) / (last - first);
    }

    inline float peak(const std::vector<float>& x)
    {
        float p = 0.0f;
        for (const float v : x)
            p = std::max(p, std::fabs(v));
        return p;
    }

    inline bool allFinite(const std::vector<float>& x)
    {
        return std::all_of(x.begin(), x.end(), [](float v) { return std::isfinite(v); });
    }

    inline bool sameBits(const std::vector<float>& a, const std::vector<float>& b)
    {
        return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
    }

    // A 32-bit float WAV (WAVE_FORMAT_IEEE_FLOAT), stereo. Returns false on a write error.
    inline bool writeWav(const std::string& path, const Stereo& s, int sampleRate)
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (f == nullptr)
            return false;
        const uint32_t frames = static_cast<uint32_t>(s.size());
        const uint16_t channels = 2, bits = 32, format = 3;
        const uint32_t rate = static_cast<uint32_t>(sampleRate);
        const uint32_t dataBytes = frames * channels * (bits / 8u);
        const uint32_t byteRate = rate * channels * (bits / 8u);
        const uint16_t blockAlign = static_cast<uint16_t>(channels * (bits / 8u));
        const auto u32 = [f](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        const auto u16 = [f](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        std::fwrite("RIFF", 1, 4, f);
        u32(36u + dataBytes);
        std::fwrite("WAVEfmt ", 1, 8, f);
        u32(16u);
        u16(format);
        u16(channels);
        u32(rate);
        u32(byteRate);
        u16(blockAlign);
        u16(bits);
        std::fwrite("data", 1, 4, f);
        u32(dataBytes);
        for (std::size_t i = 0; i < s.size(); ++i)
        {
            const float frame[2] = { s.l[i], s.r[i] };
            std::fwrite(frame, sizeof(float), 2, f);
        }
        const bool ok = std::ferror(f) == 0;
        return std::fclose(f) == 0 && ok;
    }

    // ---- reverb analysis ------------------------------------------------------------------------------------------------

    // A decay time from the energy decay curve (Schroeder's backward integration of the per-sample energy `e`): the
    // least-squares line through the curve, in dB, from where it first falls to `hiDb` to where it first falls to
    // `loDb`, gives the slope; T60 = -60 / slope. With hiDb -5 and loDb -35 this is the T30 extrapolated to 60 dB
    // (ISO 3382). `t60` is 0 when the curve never reaches loDb; `r` is the fit's correlation (-1: a perfect decay).
    struct DecayFit
    {
        double t60 = 0.0;
        double r = 0.0;
        double fromSec = 0.0, toSec = 0.0;
    };

    inline DecayFit decayFit(const std::vector<double>& e, double fs, double hiDb = -5.0, double loDb = -35.0)
    {
        DecayFit fit;
        const std::size_t n = e.size();
        if (n < 2)
            return fit;
        std::vector<double> edc(n);
        double acc = 0.0;
        for (std::size_t i = n; i-- > 0;)
        {
            acc += e[i];
            edc[i] = acc;
        }
        if (!(acc > 0.0))
            return fit;
        std::size_t a = n, b = n;
        for (std::size_t i = 0; i < n; ++i)
        {
            const double db = 10.0 * std::log10(std::max(edc[i] / acc, 1e-300));
            if (a == n && db <= hiDb)
                a = i;
            if (db <= loDb)
            {
                b = i;
                break;
            }
        }
        if (a >= n || b >= n || b <= a + 1)
            return fit;
        double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, syy = 0.0;
        const double m = static_cast<double>(b - a + 1);
        for (std::size_t i = a; i <= b; ++i)
        {
            const double x = static_cast<double>(i) / fs;
            const double y = 10.0 * std::log10(edc[i] / acc);
            sx += x;
            sy += y;
            sxx += x * x;
            sxy += x * y;
            syy += y * y;
        }
        const double cov = sxy - sx * sy / m, vx = sxx - sx * sx / m, vy = syy - sy * sy / m;
        const double slope = cov / vx;
        fit.t60 = slope < 0.0 ? -60.0 / slope : 0.0;
        fit.r = cov / std::sqrt(vx * vy);
        fit.fromSec = static_cast<double>(a) / fs;
        fit.toSec = static_cast<double>(b) / fs;
        return fit;
    }

    // A band-pass of `x` around `hz`: two identical RBJ band-pass biquads (constant 0 dB peak gain) of quality q in
    // series, in double, run forwards over the whole signal.
    inline std::vector<double> bandPass(const std::vector<double>& x, double hz, double q, double fs)
    {
        const double w = 2.0 * std::numbers::pi * hz / fs, alpha = std::sin(w) / (2.0 * q), cw = std::cos(w);
        const double a0 = 1.0 + alpha;
        const double b0 = alpha / a0, b2 = -alpha / a0, a1 = -2.0 * cw / a0, a2 = (1.0 - alpha) / a0;
        std::vector<double> y = x;
        for (int pass = 0; pass < 2; ++pass)
        {
            double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
            for (double& v : y)
            {
                const double in = v;
                const double out = b0 * in + b2 * x2 - a1 * y1 - a2 * y2;
                x2 = x1;
                x1 = in;
                y2 = y1;
                y1 = out;
                v = out;
            }
        }
        return y;
    }
}
