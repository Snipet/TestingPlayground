// Source/plugin/SoundPicture.cpp: see SoundPicture.h.
#include "plugin/SoundPicture.h"

#include "plugin/ChatImages.h"

#include "Product.h"

#include <juce_graphics/juce_graphics.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace playground::soundpicture
{
    namespace
    {
        constexpr bool kInstrument = product::kIsInstrument;

        // The layout (SoundPicture.h).
        constexpr int kLeft = 64;                    // the plots' left edge (labels before it)
        constexpr int kPlotW = 1000;                 // the waveform's and the spectrogram's width: one column a pixel
        constexpr int kWaveTop = 52, kWaveH = 180;
        constexpr int kSpecTop = 262, kSpecH = 300;  // one row a pixel
        constexpr int kBottomTop = 604, kBottomH = 170;
        constexpr int kSpectrumW = 520;              // the average spectrum: one column a pixel
        constexpr int kZoomLeft = 640, kZoomW = 424;

        const juce::Colour kBg(0xff0f1318), kPanel(0xff161b22), kGrid(0xff262d36), kText(0xffd0d7de), kDim(0xff8b949e),
            kWaveL(0xff58a6ff), kWaveR(0xffff9e57), kLevel(0xff3fb950), kMarker(0xfff778ba), kHarmonic(0xffe3b341);

        std::string fmt(const char* f, double v)
        {
            char buf[64];
            std::snprintf(buf, sizeof buf, f, v);
            return buf;
        }

        juce::String js(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

        // A plot's title at its top left, on a dark backdrop so that it reads over the plot.
        void title(juce::Graphics& g, const juce::String& text, int x, int y)
        {
            const juce::Font font(juce::FontOptions(12.0f, juce::Font::bold));
            const float w = juce::GlyphArrangement::getStringWidth(font, text);
            g.setColour(kBg.withAlpha(0.78f));
            g.fillRoundedRectangle(static_cast<float>(x) - 4.0f, static_cast<float>(y), w + 10.0f, 17.0f, 4.0f);
            g.setColour(kText);
            g.setFont(font);
            g.drawText(text, x, y, static_cast<int>(std::ceil(w)) + 4, 17, juce::Justification::centredLeft);
        }

        // dB under the loudest (0 to -120) as a colour: black, purple, red, orange, pale yellow (magma).
        juce::Colour heat(float db)
        {
            static constexpr std::array<std::array<float, 4>, 9> kStops{ { { 0.00f, 0, 0, 4 },
                                                                           { 0.13f, 28, 16, 68 },
                                                                           { 0.25f, 79, 18, 123 },
                                                                           { 0.38f, 129, 37, 129 },
                                                                           { 0.50f, 181, 54, 122 },
                                                                           { 0.63f, 229, 80, 100 },
                                                                           { 0.75f, 251, 135, 97 },
                                                                           { 0.88f, 254, 194, 135 },
                                                                           { 1.00f, 252, 253, 191 } } };
            const float t = std::clamp((db + 120.0f) / 120.0f, 0.0f, 1.0f);
            std::size_t k = 1;
            while (k + 1 < kStops.size() && kStops[k][0] < t)
                ++k;
            const auto& a = kStops[k - 1];
            const auto& b = kStops[k];
            const float u = b[0] > a[0] ? (t - a[0]) / (b[0] - a[0]) : 0.0f;
            const auto mix = [u](float x, float y) { return static_cast<uint8_t>(std::lround(x + (y - x) * u)); };
            return juce::Colour(mix(a[1], b[1]), mix(a[2], b[2]), mix(a[3], b[3]));
        }

        // The smallest scale of 1, 1/2, 1/4, ... that holds `peak`.
        float scaleFor(float peak)
        {
            float s = 1.0f;
            while (s * 0.5f >= peak && s > 1.0f / 1024.0f)
                s *= 0.5f;
            return std::max(s, peak);
        }

        std::string amplitude(float v) { return fmt("%.3g", static_cast<double>(v)); }

        // Time ticks: a step of 1, 2 or 5 times a power of ten that gives 5 to 12 of them.
        double tickStep(double seconds)
        {
            for (const double s : { 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.25, 0.5, 1.0, 2.0, 5.0 })
                if (seconds / s <= 12.0)
                    return s;
            return 10.0;
        }

        std::string secondsText(double t, double step)
        {
            return fmt(step < 0.01 ? "%.3f s" : step < 0.1 ? "%.2f s" : "%.1f s", t);
        }

        void timeAxis(juce::Graphics& g, double seconds, int y)
        {
            const double step = tickStep(seconds);
            g.setFont(juce::FontOptions(11.0f));
            for (double t = 0.0; t <= seconds + 1e-9; t += step)
            {
                const float x = static_cast<float>(kLeft) + static_cast<float>(t / seconds) * kPlotW;
                g.setColour(kGrid);
                g.drawVerticalLine(static_cast<int>(x), static_cast<float>(y - 4), static_cast<float>(y));
                g.setColour(kDim);
                g.drawText(js(secondsText(t, step)), static_cast<int>(x) - 30, y, 60, 14, juce::Justification::centred);
            }
        }

        void marker(juce::Graphics& g, const measure::View& v, int top, int h)
        {
            if (v.heldUntil <= 0.0 || v.length == 0)
                return;
            const float at = static_cast<float>(v.heldUntil * v.rate / static_cast<double>(v.length));
            const float x = static_cast<float>(kLeft) + at * kPlotW;
            g.setColour(kMarker);
            for (float y = static_cast<float>(top); y < static_cast<float>(top + h); y += 8.0f)
                g.drawLine(x, y, x, std::min(y + 4.0f, static_cast<float>(top + h)), 1.5f);
        }

        void waveform(juce::Graphics& g, const measure::View& v)
        {
            g.setColour(kPanel);
            g.fillRect(kLeft, kWaveTop, kPlotW, kWaveH);
            float peak = 0.0f;
            for (std::size_t c = 0; c < v.maxL.size(); ++c)
                peak = std::max({ peak, std::fabs(v.maxL[c]), std::fabs(v.minL[c]), std::fabs(v.maxR[c]), std::fabs(v.minR[c]) });
            const float scale = scaleFor(std::max(peak, 1e-6f));
            const float mid = static_cast<float>(kWaveTop) + kWaveH * 0.5f;
            const float half = kWaveH * 0.5f - 4.0f;
            const auto yOf = [&](float s) { return mid - std::clamp(s / scale, -1.0f, 1.0f) * half; };
            g.setFont(juce::FontOptions(11.0f));
            for (const float f : { 1.0f, 0.5f, 0.0f, -0.5f, -1.0f })
            {
                const float y = yOf(f * scale);
                g.setColour(f == 0.0f ? kGrid.brighter(0.3f) : kGrid);
                g.drawHorizontalLine(static_cast<int>(y), static_cast<float>(kLeft), static_cast<float>(kLeft + kPlotW));
                g.setColour(kDim);
                g.drawText(js(amplitude(f * scale)), 4, static_cast<int>(y) - 7, kLeft - 10, 14, juce::Justification::centredRight);
            }
            // Each column's extent: the right channel first where it differs, the left over it.
            for (int pass = v.stereo ? 0 : 1; pass < 2; ++pass)
            {
                const std::vector<float>& lo = pass == 0 ? v.minR : v.minL;
                const std::vector<float>& hi = pass == 0 ? v.maxR : v.maxL;
                g.setColour(pass == 0 ? kWaveR.withAlpha(0.75f) : kWaveL);
                for (std::size_t c = 0; c < lo.size() && c < static_cast<std::size_t>(kPlotW); ++c)
                {
                    const float y0 = yOf(hi[c]), y1 = yOf(lo[c]);
                    g.fillRect(static_cast<float>(kLeft) + static_cast<float>(c), y0, 1.0f, std::max(1.0f, y1 - y0));
                }
            }
            // The level: 0 dBFS at the top, -60 at the bottom.
            juce::Path level;
            bool started = false;
            for (std::size_t c = 0; c < v.levelDb.size() && c < static_cast<std::size_t>(kPlotW); ++c)
            {
                const float db = std::clamp(v.levelDb[c], -60.0f, 0.0f);
                const float y = static_cast<float>(kWaveTop) + 2.0f + (-db / 60.0f) * (kWaveH - 4.0f);
                const float x = static_cast<float>(kLeft) + static_cast<float>(c) + 0.5f;
                if (!started)
                    level.startNewSubPath(x, y);
                else
                    level.lineTo(x, y);
                started = true;
            }
            g.setColour(kLevel);
            g.strokePath(level, juce::PathStrokeType(1.5f));
            g.setFont(juce::FontOptions(10.5f));
            for (const int db : { 0, -30, -60 })
            {
                const float y = static_cast<float>(kWaveTop) + 2.0f + (static_cast<float>(-db) / 60.0f) * (kWaveH - 4.0f);
                g.drawText(js(std::to_string(db) + " dB"), kLeft + kPlotW + 4, static_cast<int>(y) - 7, 48, 14,
                           juce::Justification::centredLeft);
            }
            marker(g, v, kWaveTop, kWaveH);
            title(g, "Waveform" + juce::String(v.stereo ? " (left blue, right orange)" : "") + " and level (green)", kLeft + 6,
                  kWaveTop + 2);
        }

        void spectrogram(juce::Graphics& g, const measure::View& v)
        {
            juce::Image cells(juce::Image::RGB, std::max(1, v.columns), std::max(1, v.rows), false);
            {
                juce::Image::BitmapData bd(cells, juce::Image::BitmapData::writeOnly);
                for (int r = 0; r < v.rows; ++r)
                    for (int c = 0; c < v.columns; ++c)
                        bd.setPixelColour(c, v.rows - 1 - r,
                                          heat(v.cells[static_cast<std::size_t>(r) * static_cast<std::size_t>(v.columns)
                                                       + static_cast<std::size_t>(c)]));
            }
            g.drawImage(cells, kLeft, kSpecTop, kPlotW, kSpecH, 0, 0, v.columns, v.rows);
            g.setFont(juce::FontOptions(11.0f));
            for (const double hz : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
            {
                if (hz > v.hiHz)
                    continue;
                const float y = static_cast<float>(kSpecTop + kSpecH) - static_cast<float>(measure::rowOf(v, hz) / v.rows) * kSpecH;
                g.setColour(juce::Colours::white.withAlpha(0.18f));
                g.drawHorizontalLine(static_cast<int>(y), static_cast<float>(kLeft), static_cast<float>(kLeft + kPlotW));
                g.setColour(kDim);
                g.drawText(js(hz >= 1000.0 ? fmt("%.0fk", hz / 1000.0) : fmt("%.0f", hz)), 4, static_cast<int>(y) - 7,
                           kLeft - 10, 14, juce::Justification::centredRight);
            }
            marker(g, v, kSpecTop, kSpecH);
            // The colour scale.
            const int bx = kLeft + kPlotW + 6;
            for (int y = 0; y < kSpecH; ++y)
            {
                g.setColour(heat(-120.0f * static_cast<float>(y) / static_cast<float>(kSpecH - 1)));
                g.fillRect(bx, kSpecTop + y, 10, 1);
            }
            g.setColour(kDim);
            g.setFont(juce::FontOptions(10.0f));
            for (const int db : { 0, -60, -120 })
                g.drawText(js(std::to_string(db)), bx + 12, kSpecTop + (-db) * (kSpecH - 1) / 120 - 6, 34, 12,
                           juce::Justification::centredLeft);
            title(g, "Spectrogram (Hz, log; colour: dB under the loudest)", kLeft + 6, kSpecTop + 2);
        }

        void spectrum(juce::Graphics& g, const measure::View& v)
        {
            const int top = kBottomTop, h = kBottomH;
            g.setColour(kPanel);
            g.fillRect(kLeft, top, kSpectrumW, h);
            const auto yOf = [&](float db) { return static_cast<float>(top) + 18.0f + std::clamp(-db / 120.0f, 0.0f, 1.0f) * (h - 22.0f); };
            const auto xOf = [&](double hz) {
                return static_cast<float>(kLeft) + static_cast<float>(std::log(hz / v.loHz) / std::log(v.hiHz / v.loHz)) * kSpectrumW;
            };
            g.setFont(juce::FontOptions(10.5f));
            for (const int db : { 0, -40, -80, -120 })
            {
                const float y = yOf(static_cast<float>(db));
                g.setColour(kGrid);
                g.drawHorizontalLine(static_cast<int>(y), static_cast<float>(kLeft), static_cast<float>(kLeft + kSpectrumW));
                g.setColour(kDim);
                g.drawText(js(std::to_string(db)), 4, static_cast<int>(y) - 7, kLeft - 10, 14, juce::Justification::centredRight);
            }
            for (const double hz : { 100.0, 1000.0, 10000.0 })
            {
                const float x = xOf(hz);
                g.setColour(kGrid);
                g.drawVerticalLine(static_cast<int>(x), static_cast<float>(top + 18), static_cast<float>(top + h));
                g.setColour(kDim);
                g.drawText(js(hz >= 1000.0 ? fmt("%.0fk", hz / 1000.0) : fmt("%.0f", hz)), static_cast<int>(x) - 20, top + h + 2,
                           40, 14, juce::Justification::centred);
            }
            g.setColour(kHarmonic.withAlpha(0.8f));
            for (const double hz : v.harmonics)
            {
                const float x = xOf(hz);
                g.drawLine(x, static_cast<float>(top + 18), x, static_cast<float>(top + 26), 1.0f);
            }
            juce::Path p;
            for (std::size_t c = 0; c < v.spectrum.size(); ++c)
            {
                const float x = static_cast<float>(kLeft) + (static_cast<float>(c) + 0.5f) * kSpectrumW / static_cast<float>(v.spectrum.size());
                const float y = yOf(v.spectrum[c]);
                if (c == 0)
                    p.startNewSubPath(x, y);
                else
                    p.lineTo(x, y);
            }
            g.setColour(kWaveL);
            g.strokePath(p, juce::PathStrokeType(1.3f));
            title(g, v.heldUntil > 0.0 ? (kInstrument ? "Spectrum while held (dB)" : "Spectrum while the input plays (dB)")
                                       : "Spectrum (dB)",
                  kLeft + 6, top + 1);
        }

        void closeUp(juce::Graphics& g, const measure::View& v)
        {
            const int top = kBottomTop, h = kBottomH;
            g.setColour(kPanel);
            g.fillRect(kZoomLeft, top, kZoomW, h);
            const std::size_t n = v.zoomL.size();
            float peak = 0.0f;
            for (std::size_t i = 0; i < n; ++i)
                peak = std::max({ peak, std::fabs(v.zoomL[i]), std::fabs(v.zoomR[i]) });
            const float scale = scaleFor(std::max(peak, 1e-6f));
            const float mid = static_cast<float>(top) + 18.0f + (h - 22.0f) * 0.5f, half = (h - 22.0f) * 0.5f - 2.0f;
            const auto yOf = [&](float s) { return mid - std::clamp(s / scale, -1.0f, 1.0f) * half; };
            g.setColour(kGrid);
            g.drawHorizontalLine(static_cast<int>(mid), static_cast<float>(kZoomLeft), static_cast<float>(kZoomLeft + kZoomW));
            g.setFont(juce::FontOptions(10.5f));
            g.setColour(kDim);
            for (const float f : { 1.0f, -1.0f })
                g.drawText(js(amplitude(f * scale)), kZoomLeft + kZoomW + 4, static_cast<int>(yOf(f * scale)) - 7, 44, 14,
                           juce::Justification::centredLeft);
            for (int pass = v.stereo ? 0 : 1; pass < 2; ++pass)
            {
                const std::vector<float>& x = pass == 0 ? v.zoomR : v.zoomL;
                juce::Path p;
                for (std::size_t i = 0; i < n; ++i)
                {
                    const float px = static_cast<float>(kZoomLeft) + (n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.5f) * kZoomW;
                    if (i == 0)
                        p.startNewSubPath(px, yOf(x[i]));
                    else
                        p.lineTo(px, yOf(x[i]));
                }
                g.setColour(pass == 0 ? kWaveR.withAlpha(0.75f) : kWaveL);
                g.strokePath(p, juce::PathStrokeType(1.4f));
                if (n > 0 && n <= static_cast<std::size_t>(kZoomW / 4))      // few enough to show the samples
                    for (std::size_t i = 0; i < n; ++i)
                    {
                        const float px = static_cast<float>(kZoomLeft) + (n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.5f) * kZoomW;
                        g.fillEllipse(px - 1.5f, yOf(x[i]) - 1.5f, 3.0f, 3.0f);
                    }
            }
            const double from = static_cast<double>(v.zoomFrom) / v.rate, to = static_cast<double>(v.zoomTo) / v.rate;
            title(g, js("Close-up, " + fmt("%.4f", from) + " to " + fmt("%.4f s", to)), kZoomLeft + 6, top + 1);
        }

        std::vector<uint8_t> encode(const juce::Image& img, bool png, float quality)
        {
            juce::MemoryOutputStream out;
            if (png)
            {
                juce::PNGImageFormat f;
                f.writeImageToStream(img, out);
            }
            else
            {
                juce::JPEGImageFormat f;
                f.setQuality(quality);
                f.writeImageToStream(img, out);
            }
            const auto* p = static_cast<const uint8_t*>(out.getData());
            return { p, p + out.getDataSize() };
        }
    }

    measure::ViewRequest request()
    {
        measure::ViewRequest r;
        r.columns = kPlotW;
        r.rows = kSpecH;
        r.spectrumColumns = kSpectrumW;
        return r;
    }

    Drawn draw(const measure::View& v)
    {
        Drawn out;
        juce::Image img(juce::Image::RGB, kWidth, kHeight, true);
        {
            juce::Graphics g(img);
            g.fillAll(kBg);
            g.setColour(kText);
            g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
            g.drawText(js(v.title), kLeft, 6, 760, 18, juce::Justification::centredLeft);
            g.setColour(kDim);
            g.setFont(juce::FontOptions(12.0f));
            g.drawText(js("Dials: " + v.dials), kLeft, 26, kPlotW, 16, juce::Justification::centredLeft);
            const std::string level = "peak " + (v.peakDb <= -150.0 ? std::string("silent") : fmt("%.1f dBFS", v.peakDb))
                                    + ", RMS " + (v.rmsDb <= -150.0 ? std::string("silent") : fmt("%.1f dBFS", v.rmsDb))
                                    + (v.alerts != 0 ? "; ALERTS" : "");
            g.setColour(v.alerts != 0 ? kMarker : kText);
            g.setFont(juce::FontOptions(12.0f));
            g.drawText(js(level), kLeft + kPlotW - 300, 6, 300, 18, juce::Justification::centredRight);
            waveform(g, v);
            timeAxis(g, static_cast<double>(v.length) / v.rate, kWaveTop + kWaveH + 2);
            spectrogram(g, v);
            timeAxis(g, static_cast<double>(v.length) / v.rate, kSpecTop + kSpecH + 2);
            spectrum(g, v);
            closeUp(g, v);
        }
        out.png = encode(img, true, 0.0f);

        // The chat's thumbnail: 480 px wide.
        auto pic = std::make_shared<ChatPicture>();
        pic->id = chatimages::nextId();
        pic->name = "sound.png";
        pic->width = kWidth;
        pic->height = kHeight;
        const juce::Image thumb = img.rescaled(480, 480 * kHeight / kWidth, juce::Graphics::highResamplingQuality);
        pic->thumbWidth = thumb.getWidth();
        pic->thumbHeight = thumb.getHeight();
        {
            const juce::Image::BitmapData bd(thumb, juce::Image::BitmapData::readOnly);
            pic->thumb.resize(static_cast<std::size_t>(bd.width) * static_cast<std::size_t>(bd.height) * 4u);
            std::size_t i = 0;
            for (int y = 0; y < bd.height; ++y)
                for (int x = 0; x < bd.width; ++x)
                {
                    const juce::Colour c = bd.getPixelColour(x, y);
                    pic->thumb[i++] = c.getRed();
                    pic->thumb[i++] = c.getGreen();
                    pic->thumb[i++] = c.getBlue();
                    pic->thumb[i++] = 255;
                }
        }
        pic->thumbData = encode(thumb, false, 0.85f);
        pic->thumbType = "image/jpeg";
        out.thumb = std::move(pic);

        const double seconds = static_cast<double>(v.length) / v.rate;
        out.about = "A picture of the program's sound, rendered offline from the editor's files (nothing that plays "
                    "changed): "
                  + v.title + ", " + fmt("%.2f s", seconds) + " in all. Dials: " + v.dials + ".\n"
                  + "Top: the waveform, " + (v.stereo ? "left in blue and right in orange, " : "left and right the same, ")
                  + "on a scale that fits its peak (the labels at the left), and the level in green (RMS over 10 ms; 0 "
                    "dBFS at the top to -60 at the bottom, the labels at the right)."
                  + (v.heldUntil > 0.0 ? std::string(kInstrument ? " The dashed pink line is the note-off."
                                                                 : " The dashed pink line is where the input stops.")
                                       : std::string())
                  + "\nMiddle: a spectrogram, 20 Hz at the bottom to " + fmt("%.0f Hz", v.hiHz)
                  + " at the top on a log scale (the labels at the left), time across as in the waveform; its colour is dB "
                    "under the loudest moment: pale yellow 0, orange -30, purple -60, black -120 and below.\n"
                  + "Bottom left: the average spectrum" + (v.heldUntil > 0.0 ? (kInstrument ? " while held" : " while the input plays") : "")
                  + ", in dB under its loudest (0 to -120)"
                  + (v.harmonics.empty() ? std::string() : ", with the harmonics of " + fmt("%.1f Hz", v.harmonics.front())
                                                               + " marked in yellow at its top")
                  + ". Bottom right: a close-up from " + fmt("%.4f", static_cast<double>(v.zoomFrom) / v.rate) + " to "
                  + fmt("%.4f s", static_cast<double>(v.zoomTo) / v.rate) + " (" + std::to_string(v.zoomTo - v.zoomFrom)
                  + " samples).\n"
                  + "Peak " + (v.peakDb <= -150.0 ? std::string("silent") : fmt("%.1f dBFS", v.peakDb)) + ", RMS "
                  + (v.rmsDb <= -150.0 ? std::string("silent") : fmt("%.1f dBFS", v.rmsDb))
                  + (kInstrument ? " while held" : "") + "; the output guard's alerts: "
                  + (v.alerts == 0 ? std::string("none") : "NaN, infinite or clipped samples (" + std::to_string(v.alerts) + ")")
                  + ".";
        return out;
    }
}
