// Source/editor/DisplayItem.cpp: a display's motion and its drawing (DisplayItem.h).
#include "editor/DisplayItem.h"

#include "editor/GuiItems.h"
#include "editor/Looks.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>

namespace playground::ui
{
    namespace
    {
        constexpr float kFloorDb = -48.0f, kTopDb = 6.0f;      // a meter's scale
        constexpr float kHoldSeconds = 1.0f, kFallDbPerSecond = 20.0f;
        constexpr float kVuTau = 0.065f;                       // seconds: about 300 ms to 99 %
        constexpr float kVuReferenceDb = -18.0f;               // 0 VU, as dBFS RMS
        constexpr float kClipHold = 0.4f;
        constexpr float kReadoutEvery = 0.125f;
        constexpr int kScopeWindow = 1024;                     // samples across the screen (21 ms at 48 kHz)
        constexpr int kScopePoints = 148;

        float dbOf(float x) { return 20.0f * std::log10(std::max(x, 1.0e-6f)); }
        float meterPosition(float linear) { return std::clamp((dbOf(linear) - kFloorDb) / (kTopDb - kFloorDb), 0.0f, 1.0f); }

        // A field's value as a readout: three or four significant figures.
        std::string figures(float v)
        {
            char buf[32];
            const float a = std::fabs(v);
            if (a >= 1000.0f)
                std::snprintf(buf, sizeof buf, "%.0f", static_cast<double>(v));
            else if (a >= 100.0f)
                std::snprintf(buf, sizeof buf, "%.1f", static_cast<double>(v));
            else if (a >= 10.0f)
                std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(v));
            else
                std::snprintf(buf, sizeof buf, "%.3f", static_cast<double>(v));
            return buf;
        }

        // A field's readout as display d shows it: a number's unit=, decimals= and scale= as a knob's value reads
        // (formatValue: an int's places 0 unless said); else an int's or a bool's whole number, a float's figures.
        std::string fieldText(const PanelDisplay& d, float v)
        {
            if (d.kind == DisplayKind::number && d.formatted())
            {
                Control c;
                std::snprintf(c.unit, sizeof c.unit, "%s", d.unit.c_str());
                c.decimals = d.decimals >= 0 ? d.decimals : (d.integer ? int8_t{ 0 } : int8_t{ -1 });
                c.scale = d.scale;
                char buf[48];
                formatValue(c, v, buf, sizeof buf);
                return buf;
            }
            if (d.integer)
                return std::to_string(std::lround(v));
            return figures(v);
        }

        std::string decibels(float linear)
        {
            if (linear < 1.0e-5f)
                return "-INF";
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.1f", static_cast<double>(dbOf(linear)));
            return buf;
        }

        const char* kindName(DisplayKind k)
        {
            switch (k)
            {
                case DisplayKind::meter:    return "METER";
                case DisplayKind::vu:       return "VU";
                case DisplayKind::led:      return "LIGHT";
                case DisplayKind::number:   return "READOUT";
                case DisplayKind::scope:    return "SCOPE";
                case DisplayKind::spectrum: return "SPECTRUM";
            }
            return "";
        }

        // The spectrum's FFT: kScopeSamples real samples as kHalf complex ones (the even samples real, the odd
        // imaginary), a radix-2 FFT of those, then split into the real signal's bins 0 to kHalf.
        constexpr int kFftSize = DisplayItem::kScopeSamples;
        constexpr int kHalf = kFftSize / 2;
        constexpr int kHalfBits = std::countr_zero(static_cast<unsigned>(kHalf));
        static_assert(std::has_single_bit(static_cast<unsigned>(kFftSize)) && kHalf >= 8);

        // The tables every Spectrum reads, made once for the process (the size is fixed) and never written after.
        struct FftTables
        {
            std::array<float, kFftSize> window{};       // Hann, as the spectrum always had it: 0 at both ends
            float norm = 0.0f;                          // 2 / the window's sum: a full-scale sine's bin reads 1
            std::array<uint16_t, kHalf> reverse{};      // each index's bits reversed (kHalfBits of them)
            // Each stage's twiddles in a row, e^(-i pi j / h) for j < h: the stage whose butterflies span h at h - 1
            // (h = 4 .. kHalf / 2; the stages of 1 and 2 need none).
            std::array<float, kHalf> twRe{}, twIm{};
            std::array<float, kHalf + 1> splitRe{}, splitIm{};   // e^(-2 pi i k / kFftSize), k = 0 .. kHalf
        };

        FftTables makeTables()
        {
            FftTables t;
            constexpr double pi = std::numbers::pi;
            double sum = 0.0;
            for (int i = 0; i < kFftSize; ++i)
            {
                const double w = 0.5 - 0.5 * std::cos(2.0 * pi * i / (kFftSize - 1));
                t.window[static_cast<std::size_t>(i)] = static_cast<float>(w);
                sum += w;
            }
            t.norm = static_cast<float>(2.0 / sum);
            for (int i = 0; i < kHalf; ++i)
            {
                unsigned r = 0;
                for (int b = 0; b < kHalfBits; ++b)
                    r |= ((static_cast<unsigned>(i) >> b) & 1u) << (kHalfBits - 1 - b);
                t.reverse[static_cast<std::size_t>(i)] = static_cast<uint16_t>(r);
            }
            for (int h = 4; h < kHalf; h *= 2)
                for (int j = 0; j < h; ++j)
                {
                    t.twRe[static_cast<std::size_t>(h - 1 + j)] = static_cast<float>(std::cos(pi * j / h));
                    t.twIm[static_cast<std::size_t>(h - 1 + j)] = static_cast<float>(-std::sin(pi * j / h));
                }
            for (int k = 0; k <= kHalf; ++k)
            {
                t.splitRe[static_cast<std::size_t>(k)] = static_cast<float>(std::cos(2.0 * pi * k / kFftSize));
                t.splitIm[static_cast<std::size_t>(k)] = static_cast<float>(-std::sin(2.0 * pi * k / kFftSize));
            }
            return t;
        }

        const FftTables& fftTables()
        {
            static const FftTables tables = makeTables();
            return tables;
        }
    }

    Spectrum::Spectrum()
        : last_(static_cast<std::size_t>(kFftSize)), re_(static_cast<std::size_t>(kHalf)),
          im_(static_cast<std::size_t>(kHalf)), power_(static_cast<std::size_t>(kHalf + 1))
    {
        points_.fill(-120.0f);
    }

    bool Spectrum::update(const float* x, float rate)
    {
        // Compared as values (a gain of 0 makes -0 and 0 of a signal's signs: the same silence).
        if (taken_ && rate == rate_ && std::equal(last_.begin(), last_.end(), x))
            return false;
        compute(x, rate);
        return true;
    }

    // Each point's share of the bins at `rate`: from 20 Hz to 20 kHz, spaced evenly in octaves, a point's share half a
    // spacing either side of it (in single precision, as the spectrum always worked them out).
    void Spectrum::binsFor(float rate)
    {
        constexpr int points = DisplayItem::kSpectrumPoints;
        const float binHz = rate / static_cast<float>(kFftSize);
        const float half = std::pow(1000.0f, 0.5f / static_cast<float>(points - 1));
        for (int k = 0; k < points; ++k)
        {
            const float hz = 20.0f * std::pow(1000.0f, static_cast<float>(k) / static_cast<float>(points - 1));
            const float lo = hz / half, hi = hz * half;
            int b0 = static_cast<int>(std::floor(lo / binHz)), b1 = static_cast<int>(std::ceil(hi / binHz));
            b0 = std::clamp(b0, 1, kHalf);
            b1 = std::clamp(b1, b0, kHalf);
            bins_[static_cast<std::size_t>(2 * k)] = static_cast<int16_t>(b0);
            bins_[static_cast<std::size_t>(2 * k + 1)] = static_cast<int16_t>(b1);
        }
        rate_ = rate;
    }

    void Spectrum::compute(const float* x, float rate)
    {
        const FftTables& t = fftTables();
        std::memcpy(last_.data(), x, sizeof(float) * last_.size());
        taken_ = true;
        if (rate != rate_)
            binsFor(rate);
        ++computed_;
        float* const re = re_.data();
        float* const im = im_.data();
        // The samples windowed, as kHalf complex ones in bit-reversed order.
        for (int k = 0; k < kHalf; ++k)
        {
            const std::size_t r = t.reverse[static_cast<std::size_t>(k)];
            re[r] = x[2 * k] * t.window[static_cast<std::size_t>(2 * k)];
            im[r] = x[2 * k + 1] * t.window[static_cast<std::size_t>(2 * k + 1)];
        }
        // Radix-2 butterflies, decimation in time: the stages of 1 and 2 (twiddles 1 and -i), then the tables'.
        for (int i = 0; i < kHalf; i += 2)
        {
            const float ar = re[i], ai = im[i], br = re[i + 1], bi = im[i + 1];
            re[i] = ar + br, im[i] = ai + bi;
            re[i + 1] = ar - br, im[i + 1] = ai - bi;
        }
        for (int i = 0; i < kHalf; i += 4)
        {
            float ar = re[i], ai = im[i], br = re[i + 2], bi = im[i + 2];
            re[i] = ar + br, im[i] = ai + bi;
            re[i + 2] = ar - br, im[i + 2] = ai - bi;
            ar = re[i + 1], ai = im[i + 1], br = im[i + 3], bi = -re[i + 3];   // b times -i
            re[i + 1] = ar + br, im[i + 1] = ai + bi;
            re[i + 3] = ar - br, im[i + 3] = ai - bi;
        }
        for (int h = 4; h < kHalf; h *= 2)
        {
            const float* const wr = t.twRe.data() + (h - 1);
            const float* const wi = t.twIm.data() + (h - 1);
            for (int i = 0; i < kHalf; i += 2 * h)
            {
                float* const ar = re + i;
                float* const ai = im + i;
                float* const br = ar + h;
                float* const bi = ai + h;
                for (int j = 0; j < h; ++j)
                {
                    const float tr = wr[j] * br[j] - wi[j] * bi[j];
                    const float ti = wr[j] * bi[j] + wi[j] * br[j];
                    br[j] = ar[j] - tr;
                    bi[j] = ai[j] - ti;
                    ar[j] += tr;
                    ai[j] += ti;
                }
            }
        }
        // The real signal's bins from Z, the complex FFT: X[k] = E[k] + e^(-2 pi i k / N) O[k], where the even samples'
        // E[k] = (Z[k] + conj Z[kHalf - k]) / 2 and the odd samples' O[k] = (Z[k] - conj Z[kHalf - k]) / 2i; only
        // their power is kept.
        float* const power = power_.data();
        for (int k = 1; k <= kHalf; ++k)
        {
            const int a = k & (kHalf - 1), b = (kHalf - k) & (kHalf - 1);
            const float zr = re[a], zi = im[a], cr = re[b], ci = -im[b];
            const float er = 0.5f * (zr + cr), ei = 0.5f * (zi + ci);
            const float orr = 0.5f * (zi - ci), oi = -0.5f * (zr - cr);
            const float sr = t.splitRe[static_cast<std::size_t>(k)], si = t.splitIm[static_cast<std::size_t>(k)];
            const float xr = er + (sr * orr - si * oi), xi = ei + (sr * oi + si * orr);
            power[k] = xr * xr + xi * xi;
        }
        // Each point: its strongest bin's power, in dB against a full-scale sine (floored at -120 dB).
        const float scale = t.norm * t.norm;
        for (int k = 0; k < DisplayItem::kSpectrumPoints; ++k)
        {
            const int b0 = bins_[static_cast<std::size_t>(2 * k)], b1 = bins_[static_cast<std::size_t>(2 * k + 1)];
            float top = 0.0f;
            for (int b = b0; b <= b1; ++b)
                top = std::max(top, power[b]);
            points_[static_cast<std::size_t>(k)] = 10.0f * std::log10(std::max(top * scale, 1.0e-12f));
        }
    }

    void DisplayItem::setDisplay(const PanelDisplay* d)
    {
        shown_ = d != nullptr;
        if (d == nullptr)
        {
            moving_ = false;
            return;
        }
        if (!(*d == display_))
        {
            display_ = *d;
            level_ = hold_ = holdFor_ = {};
            needle_ = glow_ = 0.0f;
            readout_.clear();
            trace_.clear();
            historyAt_ = historyCount_ = historySame_ = 0;
            spectrum_.clear();
            readoutIn_ = 0.0f;
        }
    }

    bool DisplayItem::wantsScope(bool& input) const noexcept
    {
        input = display_.source == "in";
        return shown_ && (display_.kind == DisplayKind::scope || display_.kind == DisplayKind::spectrum) && display_.audio();
    }

    float DisplayItem::fieldPosition(float v) const noexcept
    {
        const float span = display_.hi - display_.lo;
        return span > 0.0f ? std::clamp((v - display_.lo) / span, 0.0f, 1.0f) : 0.0f;
    }

    void DisplayItem::pushHistory(float v) noexcept
    {
        const bool same = historyCount_ > 0 && v == history()[historyCount_ - 1];
        history_[static_cast<std::size_t>(historyAt_)] = v;
        history_[static_cast<std::size_t>(historyAt_ + kHistory)] = v;
        historyAt_ = (historyAt_ + 1) % kHistory;
        historyCount_ = std::min(historyCount_ + 1, kHistory);
        historySame_ = same ? std::min(historySame_ + 1, kHistory) : 1;
    }

    float DisplayItem::level() const noexcept
    {
        switch (display_.kind)
        {
            case DisplayKind::meter: return level_[0];
            case DisplayKind::vu:    return needle_;
            case DisplayKind::led:   return glow_;
            case DisplayKind::number:
            case DisplayKind::scope:
            case DisplayKind::spectrum: break;
        }
        return 0.0f;
    }

    void DisplayItem::update(const Meters& m, const float* scope, const float* spectrum, float dt)
    {
        if (!shown_)
            return;
        const bool fresh = m.frame != lastFrame_;
        lastFrame_ = m.frame;
        const bool audio = display_.audio();
        const bool input = display_.source == "in";
        const float* peak = input ? m.inPeak : m.outPeak;
        const float* rms = input ? m.inRms : m.outRms;
        const float field = display_.tap >= 0 && display_.tap < kMaxDisplays ? m.taps[display_.tap] : 0.0f;
        const float step = std::clamp(dt, 0.0f, 0.25f);
        bool moved = false;
        const auto ease = [&](float& v, float target, float tau) {
            const float before = v;
            v += (target - v) * (tau > 0.0f ? 1.0f - std::exp(-step / tau) : 1.0f);
            if (std::fabs(v - target) < 1.0e-4f)
                v = target;
            moved = moved || std::fabs(v - before) > 1.0e-5f;
        };

        switch (display_.kind)
        {
            case DisplayKind::meter:
            {
                const int bars = audio ? 2 : 1;
                for (int b = 0; b < bars; ++b)
                {
                    const float target = audio ? meterPosition(peak[b]) : fieldPosition(field);
                    ease(level_[static_cast<std::size_t>(b)], target, audio ? 0.0f : 0.03f);
                    float& hold = hold_[static_cast<std::size_t>(b)];
                    float& held = holdFor_[static_cast<std::size_t>(b)];
                    const float before = hold;
                    if (level_[static_cast<std::size_t>(b)] >= hold)
                    {
                        hold = level_[static_cast<std::size_t>(b)];
                        held = kHoldSeconds;
                    }
                    else if ((held -= step) <= 0.0f)
                        hold = std::max(level_[static_cast<std::size_t>(b)], hold - step * kFallDbPerSecond / (kTopDb - kFloorDb));
                    moved = moved || hold != before;
                }
                break;
            }
            case DisplayKind::vu:
            {
                const float target = audio ? looks::vuPosition(dbOf(std::max(rms[0], rms[1])) - kVuReferenceDb) : fieldPosition(field);
                ease(needle_, audio && std::max(rms[0], rms[1]) < 1.0e-5f ? 0.0f : target, kVuTau);
                break;
            }
            case DisplayKind::led:
            {
                const float before = glow_;
                if (audio)
                    glow_ = std::max(std::max(peak[0], peak[1]) >= 0.98f ? 1.0f : 0.0f, glow_ - step / kClipHold);
                else
                    ease(glow_, fieldPosition(field), 0.015f);
                glow_ = std::max(glow_, 0.0f);
                moved = moved || glow_ != before;
                break;
            }
            case DisplayKind::number:
                break;
            case DisplayKind::spectrum:
            {
                // Its source's points, the same for every display of it (Spectrum): a peak at once, a fall of 30 dB a
                // second, each frame whether a block came or not.
                if (spectrum == nullptr)
                    break;
                if (spectrum_.size() != static_cast<std::size_t>(kSpectrumPoints))
                    spectrum_.assign(static_cast<std::size_t>(kSpectrumPoints), -120.0f);   // its capacity kept
                for (std::size_t k = 0; k < spectrum_.size(); ++k)
                {
                    const float before = spectrum_[k], next = spectrum[k];
                    spectrum_[k] = next >= before ? next : std::max(next, before - 30.0f * step);
                    moved = moved || std::fabs(spectrum_[k] - before) > 0.01f;
                }
                break;
            }
            case DisplayKind::scope:
            {
                if (audio && scope != nullptr)
                {
                    // The window that starts at the last rising zero crossing before the newest window's room, where
                    // the line between its two samples crosses 0: a steady tone's window starts at the same phase
                    // each frame, so its trace stands still.
                    float start = static_cast<float>(kScopeSamples - kScopeWindow);
                    for (int i = kScopeSamples - kScopeWindow; i > 0; --i)
                        if (scope[i - 1] < 0.0f && scope[i] >= 0.0f)
                        {
                            start = static_cast<float>(i - 1) + scope[i - 1] / (scope[i - 1] - scope[i]);
                            break;
                        }
                    std::array<float, kScopePoints> next{};
                    constexpr float kSpacing = static_cast<float>(kScopeWindow - 1) / static_cast<float>(kScopePoints - 1);
                    for (int k = 0; k < kScopePoints; ++k)
                    {
                        const float x = start + kSpacing * static_cast<float>(k);
                        const int i = std::min(static_cast<int>(x), kScopeSamples - 2);
                        const float t = x - static_cast<float>(i);
                        next[static_cast<std::size_t>(k)] = scope[i] + (scope[i + 1] - scope[i]) * t;
                    }
                    // Moved when a point would move by more than half a px; the trace drawn stays the last that did, so
                    // a slow drift moves it once it is half a px off.
                    const float tolerance = 0.5f / std::max(looks::scopeScale(cell_), 1.0f);
                    bool off = trace_.size() != next.size();
                    for (std::size_t k = 0; k < next.size() && !off; ++k)
                        off = !(std::fabs(next[k] - trace_[k]) <= tolerance);
                    if (off)
                    {
                        trace_.assign(next.begin(), next.end());
                        moved = true;
                    }
                }
                else if (!audio && fresh)
                {
                    // A flat line shifted along by a value equal to its own is the same line: no move.
                    const float v = 2.0f * fieldPosition(field) - 1.0f;
                    const bool flat = historyCount_ >= 2 && historySame_ >= historyCount_ && v == history()[historyCount_ - 1];
                    pushHistory(v);
                    moved = moved || !flat;
                }
                break;
            }
        }

        // The readout, eight times a second: a meter and a number draw it (drawKey, not a move: one frame for each new
        // one, while a move holds the full rate a quarter second, longer than the readout's own 1/8 s); every display
        // speaks it and the footer shows it (the Panel watches the footer's line).
        if ((readoutIn_ -= step) <= 0.0f || readout_.empty())
        {
            readoutIn_ = kReadoutEvery;
            if (audio)
                readout_ = decibels(std::max(peak[0], peak[1]));
            else
                readout_ = fieldText(display_, field);
        }
        moving_ = moved;
    }

    void DisplayItem::draw(funkgui::Canvas& c, const funkgui::Theme& th, bool) const
    {
        if (!shown_)
            return;
        const std::string label = upper(asciiText(display_.label.empty() ? display_.name : display_.label));
        looks::Display d;
        d.cell = cell_;
        d.label = label.c_str();
        d.tinted = !display_.color.empty();
        d.color = looks::panelColor(display_.color, th, th.accent);
        const bool audio = display_.audio();
        switch (display_.kind)
        {
            case DisplayKind::meter:
                looks::meter(c, th, d, audio ? 2 : 1, level_.data(), hold_.data(), readout_.c_str());
                break;
            case DisplayKind::vu:
                looks::vu(c, th, d, needle_, audio, figures(display_.lo).c_str(), figures(display_.hi).c_str());
                break;
            case DisplayKind::led:
                looks::led(c, th, d, glow_);
                break;
            case DisplayKind::number:
                looks::number(c, th, d, readout_.c_str());
                break;
            case DisplayKind::scope:
                if (audio)
                    looks::scope(c, th, d, trace_.empty() ? nullptr : trace_.data(), static_cast<int>(trace_.size()));
                else
                    looks::scope(c, th, d, historyCount_ == 0 ? nullptr : history(), historyCount_);
                break;
            case DisplayKind::spectrum:
                // Past drawSpectrum's limit it would draw only the low points, stretched across 20 Hz to 20 kHz.
                static_assert(kSpectrumPoints <= kSpectrumMostPoints, "drawSpectrum draws kSpectrumMostPoints at most");
                drawSpectrum(c, th, cell_, label.c_str(), spectrum_.empty() ? nullptr : spectrum_.data(),
                             static_cast<int>(spectrum_.size()), d.color, d.tinted);
                break;
        }
    }

    void DisplayItem::accessibility(std::vector<funkgui::A11yItem>& out) const
    {
        if (!shown_)
            return;
        funkgui::A11yItem item;
        item.id = id_;
        item.role = funkgui::A11yRole::staticText;
        item.bounds = cell_;
        item.title = (display_.label.empty() ? display_.name : display_.label) + " " + kindName(display_.kind);
        item.value = readout_;
        item.readOnly = true;
        out.push_back(item);
    }

    void DisplayItem::specLine(char* out, std::size_t n) const
    {
        const std::string label = upper(display_.label.empty() ? display_.name : display_.label);
        const std::string source = display_.audio() ? (display_.source == "in" ? "THE INPUT" : "THE OUTPUT")
                                                    : "THE FIELD " + upper(display_.source);
        std::snprintf(out, n, "%s   %s OF %s   %s%s", label.c_str(), kindName(display_.kind), source.c_str(),
                      readout_.c_str(), display_.audio() ? " DBFS" : "");
    }
}
