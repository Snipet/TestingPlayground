// Source/editor/DisplayItem.cpp: a display's motion and its drawing (DisplayItem.h).
#include "editor/DisplayItem.h"

#include "editor/ChatItems.h"
#include "editor/GuiItems.h"
#include "editor/Looks.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>

namespace testingplaygroundsynth::ui
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

        // An in-place radix-2 FFT of n (a power of two) complex values.
        void fft(std::vector<std::complex<float>>& a)
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
                const float ang = -2.0f * std::numbers::pi_v<float> / static_cast<float>(len);
                const std::complex<float> w(std::cos(ang), std::sin(ang));
                for (std::size_t i = 0; i < n; i += len)
                {
                    std::complex<float> wk(1.0f, 0.0f);
                    for (std::size_t k = 0; k < len / 2; ++k)
                    {
                        const std::complex<float> u = a[i + k], v = a[i + k + len / 2] * wk;
                        a[i + k] = u + v;
                        a[i + k + len / 2] = u - v;
                        wk *= w;
                    }
                }
            }
        }

        // The last DisplayItem::kScopeSamples samples' levels, dB against a full-scale sine, at `points` frequencies
        // from 20 Hz to 20 kHz, spaced evenly in octaves: the strongest bin within each point's share of them.
        std::vector<float> spectrumOf(const float* x, int n, float rate, int points)
        {
            std::vector<std::complex<float>> a(static_cast<std::size_t>(n));
            float windowSum = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                const float w = 0.5f - 0.5f * std::cos(2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(n - 1));
                windowSum += w;
                a[static_cast<std::size_t>(i)] = { x[i] * w, 0.0f };
            }
            fft(a);
            const float norm = 2.0f / std::max(windowSum, 1.0f);
            const float binHz = rate / static_cast<float>(n);
            std::vector<float> out(static_cast<std::size_t>(points), -120.0f);
            for (int k = 0; k < points; ++k)
            {
                const float hz = 20.0f * std::pow(1000.0f, static_cast<float>(k) / static_cast<float>(points - 1));
                const float half = std::pow(1000.0f, 0.5f / static_cast<float>(points - 1));   // half a point's share
                const float lo = hz / half, hi = hz * half;
                int b0 = static_cast<int>(std::floor(lo / binHz)), b1 = static_cast<int>(std::ceil(hi / binHz));
                b0 = std::clamp(b0, 1, n / 2);
                b1 = std::clamp(b1, b0, n / 2);
                float peak = 0.0f;
                for (int b = b0; b <= b1; ++b)
                    peak = std::max(peak, std::abs(a[static_cast<std::size_t>(b)]) * norm);
                out[static_cast<std::size_t>(k)] = dbOf(std::max(peak, 1.0e-6f));
            }
            return out;
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
            history_.clear();
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

    void DisplayItem::update(const Meters& m, const float* scope, float dt)
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
                if (scope == nullptr)
                    break;
                const std::vector<float> next = spectrumOf(scope, kScopeSamples, m.rate > 0.0f ? m.rate : 48000.0f, kSpectrumPoints);
                if (spectrum_.size() != next.size())
                    spectrum_.assign(next.size(), -120.0f);
                for (std::size_t k = 0; k < next.size(); ++k)
                {
                    const float before = spectrum_[k];
                    spectrum_[k] = next[k] >= before ? next[k] : std::max(next[k], before - 30.0f * step);
                    moved = moved || std::fabs(spectrum_[k] - before) > 0.01f;
                }
                break;
            }
            case DisplayKind::scope:
            {
                if (audio && scope != nullptr)
                {
                    // The window that starts at the last rising zero crossing before the newest window's room.
                    int start = kScopeSamples - kScopeWindow;
                    for (int i = kScopeSamples - kScopeWindow; i > 0; --i)
                        if (scope[i - 1] < 0.0f && scope[i] >= 0.0f)
                        {
                            start = i;
                            break;
                        }
                    std::vector<float> next(kScopePoints);
                    for (int k = 0; k < kScopePoints; ++k)
                        next[static_cast<std::size_t>(k)] = scope[start + k * (kScopeWindow - 1) / (kScopePoints - 1)];
                    moved = moved || next != trace_;
                    trace_ = std::move(next);
                }
                else if (!audio && fresh)
                {
                    history_.push_back(2.0f * fieldPosition(field) - 1.0f);
                    if (static_cast<int>(history_.size()) > kHistory)
                        history_.erase(history_.begin());
                    moved = true;
                }
                break;
            }
        }

        // The readout, eight times a second (a meter's and a number's).
        if ((readoutIn_ -= step) <= 0.0f || readout_.empty())
        {
            readoutIn_ = kReadoutEvery;
            std::string next;
            if (audio)
                next = decibels(std::max(peak[0], peak[1]));
            else
                next = figures(field);
            moved = moved || next != readout_;
            readout_ = std::move(next);
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
                    looks::scope(c, th, d, history_.empty() ? nullptr : history_.data(), static_cast<int>(history_.size()));
                break;
            case DisplayKind::spectrum:
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
