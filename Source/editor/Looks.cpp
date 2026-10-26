// Source/editor/Looks.cpp: the panel's hardware, drawn (Looks.h).
#include "editor/Looks.h"

#include <funkgui/core/TypeScale.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace testingplaygroundsynth::ui::looks
{
    namespace
    {
        using funkgui::Col;
        using funkgui::Rect;
        namespace type = funkgui::type;

        constexpr float kPi = std::numbers::pi_v<float>;
        constexpr funkgui::TextStyle kReadout{ 15.0f, 0.6f, 0.14f, true };   // a number's digits
        constexpr funkgui::TextStyle kPrint{ 7.0f, 0.4f, 0.10f, true };      // a VU face's figures

        struct Named
        {
            const char* name;
            Col onDark, onLight;
        };
        constexpr Named kColors[] = {
            { "red", { 0xE5, 0x53, 0x3D }, { 0xC8, 0x40, 0x2D } },
            { "orange", { 0xF0, 0x8A, 0x24 }, { 0xD4, 0x6F, 0x0C } },
            { "yellow", { 0xE8, 0xC5, 0x47 }, { 0xB8, 0x94, 0x1C } },
            { "green", { 0x57, 0xC2, 0x6E }, { 0x2E, 0x9A, 0x4B } },
            { "teal", { 0x3F, 0xBF, 0xB0 }, { 0x1F, 0x8F, 0x83 } },
            { "blue", { 0x4A, 0x90, 0xE2 }, { 0x2E, 0x6F, 0xBF } },
            { "violet", { 0x9B, 0x7B, 0xE0 }, { 0x73, 0x50, 0xC2 } },
            { "pink", { 0xE8, 0x6A, 0xA6 }, { 0xC4, 0x46, 0x82 } },
            { "cream", { 0xE9, 0xDF, 0xC4 }, { 0xA8, 0x9A, 0x73 } },
            { "grey", { 0x9A, 0xA0, 0xA6 }, { 0x7C, 0x81, 0x87 } },
        };

        funkgui::Point at(funkgui::Point c, float r, float a) { return { c.x + r * std::cos(a), c.y + r * std::sin(a) }; }

        void radial(funkgui::Canvas& c, funkgui::Point centre, float r0, float r1, float a, float width, Col col)
        {
            const funkgui::Point p0 = at(centre, r0, a), p1 = at(centre, r1, a);
            c.segment(p0.x, p0.y, p1.x, p1.y, width, col);
        }

        void label(funkgui::Canvas& c, const funkgui::Theme& th, const Rect& cell, const char* text, bool hover)
        {
            c.text(text, cell.centreX(), cell.y, type::kCaption, hover ? th.ink70 : th.ink52, funkgui::Align::centre);
        }

        Col capBlack(const funkgui::Theme& th) { return dark(th) ? Col{ 0x0D, 0x0E, 0x10 } : Col{ 0x1E, 0x20, 0x23 }; }

        // A dial's value as FunkGui's Dial draws it: dim at the default, the accent under the hand.
        Col valueColour(const funkgui::Theme& th, const Knob& k)
        {
            return k.hover ? th.accent : (k.atDefault ? th.ink32 : th.ink100);
        }
    }

    bool dark(const funkgui::Theme& th)
    {
        return static_cast<int>(th.ground.r) + static_cast<int>(th.ground.g) + static_cast<int>(th.ground.b) < 3 * 128;
    }

    Col panelColor(const std::string& name, const funkgui::Theme& th, Col fallback)
    {
        for (const Named& n : kColors)
            if (name == n.name)
                return dark(th) ? n.onDark : n.onLight;
        return fallback;
    }

    float knobAngle(float t) { return (135.0f + 270.0f * std::clamp(t, 0.0f, 1.0f)) * kPi / 180.0f; }

    float vuPosition(float vu) { return std::clamp(std::pow(10.0f, (vu - 3.0f) / 20.0f), 0.0f, 1.0f); }

    void focusRing(funkgui::Canvas& c, const funkgui::Theme& th, const Rect& cell)
    {
        c.rrect(cell.x - 2.0f, cell.y - 2.0f, cell.w + 4.0f, cell.h + 4.0f, 0.0f, th.ground.withAlpha(0.0f), 1.0f, th.accent);
    }

    // ---- controls ----------------------------------------------------------------------------------------------------
    void vintage(funkgui::Canvas& c, const funkgui::Theme& th, const Knob& k)
    {
        const Rect& r = k.cell;
        const float cx = r.centreX();
        const float cy = k.small ? r.y + 18.0f : r.y + 36.0f;
        const float skirt = k.small ? 11.0f : 15.5f, cap = k.small ? 8.0f : 11.0f;
        const float a = knobAngle(k.t);
        const funkgui::Point centre{ cx, cy };
        if (!k.small)
            label(c, th, r, k.label, k.hover);
        // The printed scale: eleven ticks, the ends and the middle longer.
        const Col print = k.hover ? th.ink52 : th.ink32;
        for (int i = 0; i <= 10; ++i)
        {
            const bool major = i == 0 || i == 5 || i == 10;
            radial(c, centre, skirt + 2.0f, skirt + (major ? 6.0f : 4.0f), knobAngle(static_cast<float>(i) / 10.0f), 1.0f, print);
        }
        // The skirt, its line, the cap and its pointer.
        const Col black = capBlack(th);
        c.disc(cx, cy, skirt, funkgui::mix(black, th.ink16, 0.55f), 1.0f, th.ink32);
        radial(c, centre, cap + 1.0f, skirt - 0.5f, a, 2.0f, k.hover ? th.accent : funkgui::mix(th.ink70, th.ink100, 0.5f));
        const Col capCol = k.tinted ? funkgui::mix(black, k.color, 0.85f) : black;
        c.disc(cx, cy, cap, capCol, 1.0f, funkgui::mix(capCol, Col{ 0xFF, 0xFF, 0xFF }, 0.18f));
        radial(c, centre, k.small ? 2.0f : 3.0f, cap - 1.5f, a, 2.0f, Col{ 0xF2, 0xEF, 0xE8 });
        if (k.small)
            c.text(k.label, cx, r.y + 38.0f, type::kMicro, k.hover ? th.ink70 : th.ink52, funkgui::Align::centre);
        else
            c.text(k.value, cx, r.y + 62.0f, type::kMicro, valueColour(th, k), funkgui::Align::centre);
        if (k.focus)
            focusRing(c, th, r);
    }

    void fader(funkgui::Canvas& c, const funkgui::Theme& th, const Knob& k)
    {
        const Rect& r = k.cell;
        const float cx = r.centreX();
        constexpr float kTop = 17.0f, kLength = 44.0f;
        const float y0 = r.y + kTop;
        label(c, th, r, k.label, k.hover);
        for (int i = 0; i <= 4; ++i)
        {
            const float y = y0 + kLength * static_cast<float>(i) / 4.0f;
            c.hairlineH(cx - 13.0f, y, 5.0f, th.ink32);
            c.hairlineH(cx + 8.0f, y, 5.0f, th.ink32);
        }
        c.rrect(cx - 2.0f, y0, 4.0f, kLength, 2.0f, funkgui::mix(capBlack(th), th.ink16, 0.5f), 1.0f, th.ink32);
        const float y = y0 + kLength * (1.0f - std::clamp(k.t, 0.0f, 1.0f));
        const Col capCol = k.tinted ? k.color : (dark(th) ? th.ink70 : th.ink52);
        c.rrect(cx - 10.0f, y - 5.0f, 20.0f, 10.0f, 2.0f, k.hover ? funkgui::mix(capCol, th.accent, 0.5f) : capCol, 1.0f,
                funkgui::mix(capCol, capBlack(th), 0.4f));
        c.hairlineH(cx - 8.0f, y, 16.0f, capBlack(th));
        c.text(k.value, cx, r.y + 68.0f, type::kMicro, valueColour(th, k), funkgui::Align::centre);
        if (k.focus)
            focusRing(c, th, r);
    }

    void toggle(funkgui::Canvas& c, const funkgui::Theme& th, const Knob& k, const char* state)
    {
        const Rect& r = k.cell;
        const float cx = r.centreX();
        const bool on = k.t >= 0.5f;
        label(c, th, r, k.label, k.hover);
        c.rrect(cx - 8.0f, r.y + 17.0f, 16.0f, 34.0f, 3.0f, funkgui::mix(capBlack(th), th.ink16, 0.6f), 1.0f, th.ink32);
        const Col lever = on && k.tinted ? k.color : (k.hover ? th.accent : (dark(th) ? th.ink70 : th.ink52));
        c.rrect(cx - 4.0f, on ? r.y + 20.0f : r.y + 34.0f, 8.0f, 14.0f, 2.0f, lever);
        c.text(state, cx, r.y + 60.0f, type::kMicro, on ? (k.tinted ? k.color : th.ink100) : th.ink52, funkgui::Align::centre);
        if (k.focus)
            focusRing(c, th, r);
    }

    void buttons(funkgui::Canvas& c, const funkgui::Theme& th, const Knob& k, const std::vector<std::string>& options,
                 int chosen, int hoverRow)
    {
        const Rect& r = k.cell;
        label(c, th, r, k.label, k.hover);
        const Col lit = k.tinted ? k.color : th.accent;
        for (int i = 0; i < static_cast<int>(options.size()); ++i)
        {
            const float y = r.y + 16.0f + static_cast<float>(i) * 14.0f;
            if (i == hoverRow)
                c.rrect(r.x, y, r.w, 14.0f, 0.0f, funkgui::mix(th.ground, th.ink16, 0.6f));
            const bool on = i == chosen;
            c.rrect(r.x + 6.0f, y + 3.0f, 8.0f, 8.0f, 1.0f, on ? lit : funkgui::premix(th.ground, lit, 0.16f), 1.0f,
                    on ? funkgui::mix(lit, Col{ 0xFF, 0xFF, 0xFF }, 0.25f) : th.ink32);
            c.text(options[static_cast<std::size_t>(i)].c_str(), r.x + 20.0f, c.capCentreTop(y + 7.0f, type::kMicro), type::kMicro,
                   on ? th.ink100 : th.ink52);
        }
        if (k.focus)
            focusRing(c, th, r);
    }

    // ---- displays ----------------------------------------------------------------------------------------------------
    void meter(funkgui::Canvas& c, const funkgui::Theme& th, const Display& d, int count, const float* levels,
               const float* holds, const char* readout)
    {
        const Rect& r = d.cell;
        label(c, th, r, d.label, false);
        constexpr int kSegments = 15;
        constexpr float kBarW = 8.0f, kBarGap = 4.0f, kTop = 16.0f, kHeight = 46.0f, kSegGap = 1.0f;
        const float segH = (kHeight - kSegGap * static_cast<float>(kSegments - 1)) / static_cast<float>(kSegments);
        const float total = static_cast<float>(count) * kBarW + static_cast<float>(count - 1) * kBarGap;
        const Col base = d.tinted ? d.color : panelColor("green", th, th.accent);
        const Col warm = panelColor("yellow", th, th.accent), hot = panelColor("red", th, th.accent);
        for (int b = 0; b < count; ++b)
        {
            const float x = std::round(r.centreX() - 0.5f * total) + static_cast<float>(b) * (kBarW + kBarGap);
            const int litTo = static_cast<int>(std::floor(std::clamp(levels[b], 0.0f, 1.0f) * kSegments + 0.0001f));
            const int hold = static_cast<int>(std::ceil(std::clamp(holds[b], 0.0f, 1.0f) * kSegments)) - 1;
            for (int s = 0; s < kSegments; ++s)
            {
                const float p = (static_cast<float>(s) + 0.5f) / static_cast<float>(kSegments);
                const Col col = p > 0.87f ? hot : (p > 0.7f ? warm : base);
                const bool on = s < litTo || s == hold;
                const float y = r.y + kTop + kHeight - static_cast<float>(s + 1) * segH - static_cast<float>(s) * kSegGap;
                c.rrect(x, y, kBarW, segH, 0.0f, on ? col : funkgui::premix(th.ground, col, 0.14f));
            }
        }
        c.text(readout, r.centreX(), r.y + 68.0f, type::kMicro, th.ink52, funkgui::Align::centre);
    }

    void vu(funkgui::Canvas& c, const funkgui::Theme& th, const Display& d, float needle, bool audio, const char* lo,
            const char* hi)
    {
        const Rect& r = d.cell;
        label(c, th, r, d.label, false);
        const Rect face{ r.x + 2.0f, r.y + 15.0f, r.w - 4.0f, 52.0f };
        const Col cream = d.tinted ? funkgui::mix(Col{ 0xEC, 0xE3, 0xC8 }, d.color, 0.35f) : Col{ 0xEC, 0xE3, 0xC8 };
        const Col ink{ 0x2B, 0x26, 0x20 }, red{ 0xC2, 0x3B, 0x22 };
        c.rrect(face.x, face.y, face.w, face.h, 3.0f, cream, 1.0f, th.ink32);
        const funkgui::Canvas::ClipScope clip(c, face);
        const funkgui::Point pivot{ face.centreX(), face.bottom() + 18.0f };
        constexpr float kR = 60.0f, kSwing = 48.0f * kPi / 180.0f;
        const auto angle = [&](float p) { return -0.5f * kPi + (std::clamp(p, 0.0f, 1.0f) - 0.5f) * 2.0f * kSwing; };
        if (audio)
        {
            const float zero = vuPosition(0.0f);
            c.arc(pivot, kR, angle(0.0f), angle(zero), 1.0f, ink);
            c.arc(pivot, kR, angle(zero), angle(1.0f), 2.5f, red);
            const float marks[] = { -20.0f, -10.0f, -7.0f, -5.0f, -3.0f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f, 3.0f };
            for (const float m : marks)
            {
                const bool major = m == -20.0f || m == -10.0f || m == -5.0f || m == 0.0f || m == 3.0f;
                radial(c, pivot, kR, kR + (major ? 6.0f : 4.0f), angle(vuPosition(m)), 1.0f, m >= 0.0f ? red : ink);
            }
            const struct
            {
                float vu;
                const char* text;
            } figures[] = { { -20.0f, "20" }, { -10.0f, "10" }, { -5.0f, "5" }, { 0.0f, "0" }, { 3.0f, "+3" } };
            for (const auto& f : figures)
            {
                const funkgui::Point p = at(pivot, kR - 9.0f, angle(vuPosition(f.vu)));
                c.text(f.text, p.x, c.capCentreTop(p.y, kPrint), kPrint, f.vu >= 0.0f ? red : ink, funkgui::Align::centre);
            }
        }
        else
        {
            c.arc(pivot, kR, angle(0.0f), angle(1.0f), 1.0f, ink);
            for (int i = 0; i <= 4; ++i)
                radial(c, pivot, kR, kR + (i % 2 == 0 ? 6.0f : 4.0f), angle(static_cast<float>(i) / 4.0f), 1.0f, ink);
            const funkgui::Point a0 = at(pivot, kR - 9.0f, angle(0.0f)), a1 = at(pivot, kR - 9.0f, angle(1.0f));
            c.text(lo, a0.x + 4.0f, c.capCentreTop(a0.y, kPrint), kPrint, ink, funkgui::Align::centre);
            c.text(hi, a1.x - 4.0f, c.capCentreTop(a1.y, kPrint), kPrint, ink, funkgui::Align::centre);
        }
        c.text("VU", face.centreX(), c.capCentreTop(face.y + 33.0f, kPrint), kPrint, ink, funkgui::Align::centre);
        // The needle, and the cover over its pivot.
        radial(c, pivot, 12.0f, kR + 5.0f, angle(needle), 1.2f, Col{ 0x1A, 0x17, 0x14 });
        c.rrect(face.x, face.bottom() - 8.0f, face.w, 8.0f, 0.0f, funkgui::mix(cream, ink, 0.22f));
    }

    void led(funkgui::Canvas& c, const funkgui::Theme& th, const Display& d, float brightness)
    {
        const Rect& r = d.cell;
        label(c, th, r, d.label, false);
        const float b = std::clamp(brightness, 0.0f, 1.0f);
        const Col lens = d.tinted ? d.color : panelColor("red", th, th.accent);
        const float cx = r.centreX(), cy = r.y + 38.0f;
        if (b > 0.02f)
            c.rrect(cx - 13.0f, cy - 13.0f, 26.0f, 26.0f, 13.0f, lens.withAlpha(0.45f * b), 0.0f, {}, 9.0f);
        c.disc(cx, cy, 6.5f, funkgui::mix(funkgui::premix(th.ground, lens, 0.22f), lens, b), 1.0f, th.ink32);
        if (b > 0.02f)
            c.disc(cx - 1.8f, cy - 1.8f, 1.6f, funkgui::mix(lens, Col{ 0xFF, 0xFF, 0xFF }, 0.6f * b));
    }

    void number(funkgui::Canvas& c, const funkgui::Theme& th, const Display& d, const char* text)
    {
        const Rect& r = d.cell;
        label(c, th, r, d.label, false);
        const Rect box{ r.x + 2.0f, r.y + 18.0f, r.w - 4.0f, 30.0f };
        c.rrect(box.x, box.y, box.w, box.h, 3.0f, dark(th) ? Col{ 0x0A, 0x0C, 0x0B } : Col{ 0x1B, 0x1E, 0x1C }, 1.0f, th.ink32);
        const Col digits = d.tinted ? d.color : Col{ 0xFF, 0xB3, 0x47 };
        c.text(text, box.centreX(), c.capCentreTop(box.centreY(), kReadout), kReadout, digits, funkgui::Align::centre);
    }

    void scope(funkgui::Canvas& c, const funkgui::Theme& th, const Display& d, const float* ys, int n)
    {
        const Rect& r = d.cell;
        label(c, th, r, d.label, false);
        const Rect screen{ r.x + 1.0f, r.y + 15.0f, r.w - 2.0f, std::max(24.0f, r.h - 28.0f) };   // 52 in an 80 cell
        const Col bg{ 0x0B, 0x13, 0x0F };
        const Col trace = d.tinted ? d.color : Col{ 0x6E, 0xEB, 0x83 };
        c.rrect(screen.x, screen.y, screen.w, screen.h, 3.0f, bg, 1.0f, th.ink32);
        const Col grid = funkgui::mix(bg, trace, 0.16f);
        c.hairlineH(screen.x + 2.0f, screen.centreY(), screen.w - 4.0f, funkgui::mix(bg, trace, 0.26f));
        for (int i = 1; i < 4; ++i)
            c.hairlineV(screen.x + screen.w * static_cast<float>(i) / 4.0f, screen.y + 2.0f, screen.h - 4.0f, grid);
        c.hairlineH(screen.x + 2.0f, screen.y + screen.h * 0.25f, screen.w - 4.0f, grid);
        c.hairlineH(screen.x + 2.0f, screen.y + screen.h * 0.75f, screen.w - 4.0f, grid);
        if (n < 2 || ys == nullptr)
            return;
        constexpr int kMaxPoints = 256;
        float xs[kMaxPoints], yy[kMaxPoints];
        const int m = std::min(n, kMaxPoints);
        const float half = 0.5f * screen.h - 3.0f;
        for (int i = 0; i < m; ++i)
        {
            const float v = std::clamp(ys[static_cast<std::size_t>(i * (n - 1) / std::max(m - 1, 1))], -1.0f, 1.0f);
            xs[i] = screen.x + 2.0f + (screen.w - 4.0f) * static_cast<float>(i) / static_cast<float>(m - 1);
            yy[i] = screen.centreY() - v * half;
        }
        const funkgui::Canvas::ClipScope clip(c, screen);
        c.polyline(xs, yy, m, 1.3f, trace);
    }
}
