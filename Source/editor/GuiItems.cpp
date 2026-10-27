// Source/editor/GuiItems.cpp: a .gui file's controls, pads, envelopes, tabs and decorations (GuiItems.h).
#include "editor/GuiItems.h"

#include "editor/ChatItems.h"
#include "editor/Looks.h"
#include "fminus/GuiSpec.h"

#include <funkgui/core/TypeScale.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace testingplaygroundsynth::ui
{
    namespace
    {
        using funkgui::Col;
        using funkgui::Rect;
        namespace type = funkgui::type;

        constexpr float kDragRange = 200.0f;                         // px for a knob's whole range (Shift: ten times)
        constexpr float kPi = 3.14159265358979f;
        constexpr float kDeg = kPi / 180.0f;
        constexpr uint32_t kTabIdStep = 1u;                          // a tab's accessibility id: the item's + 1 + its index

        float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

        funkgui::Point at(funkgui::Point c, float r, float a) { return { c.x + r * std::cos(a), c.y + r * std::sin(a) }; }

        int hexDigit(char c)
        {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        }

        // A colour a .gui file names: #rrggbb, or a name of kPanelColors (tuned for the theme).
        bool hexColor(const std::string& s, Col& out)
        {
            if (s.size() != 7 || s[0] != '#')
                return false;
            int v[6];
            for (int k = 0; k < 6; ++k)
                if ((v[k] = hexDigit(s[static_cast<std::size_t>(k + 1)])) < 0)
                    return false;
            out = Col{ static_cast<uint8_t>(v[0] * 16 + v[1]), static_cast<uint8_t>(v[2] * 16 + v[3]),
                       static_cast<uint8_t>(v[4] * 16 + v[5]) };
            return true;
        }

        Col colorOf(const std::string& name, const funkgui::Theme& th, Col fallback)
        {
            Col c;
            if (hexColor(name, c))
                return c;
            return looks::panelColor(name, th, fallback);
        }

        bool darkCol(Col c) { return static_cast<int>(c.r) * 3 + static_cast<int>(c.g) * 6 + static_cast<int>(c.b) < 128 * 10; }

        Col capBlack(const funkgui::Theme& th) { return looks::dark(th) ? Col{ 0x0D, 0x0E, 0x10 } : Col{ 0x1E, 0x20, 0x23 }; }

        Col valueInk(const funkgui::Theme& th, bool hover, bool atDefault)
        {
            return hover ? th.accent : (atDefault ? th.ink32 : th.ink100);
        }

        void radial(funkgui::Canvas& c, funkgui::Point centre, float r0, float r1, float a, float width, Col col)
        {
            const funkgui::Point p0 = at(centre, r0, a), p1 = at(centre, r1, a);
            c.segment(p0.x, p0.y, p1.x, p1.y, width, col);
        }

        // The ring around a focused control (the panel's: square).
        void ring(funkgui::Canvas& c, const funkgui::Theme& th, const Rect& r)
        {
            c.rrect(r.x - 2.0f, r.y - 2.0f, r.w + 4.0f, r.h + 4.0f, 0.0f, th.ground.withAlpha(0.0f), 1.0f, th.accent);
        }
    }

    // ---- GuiValue ---------------------------------------------------------------------------------------------------------
    void GuiValue::set(const Control& c, const std::string& label, const std::vector<std::string>& options)
    {
        control_ = c;
        label_ = upper(asciiText(label.empty() ? std::string(c.name) : label));
        options_.clear();
        for (const std::string& o : options)
            options_.push_back(upper(asciiText(o)));
    }

    float GuiValue::value01() const
    {
        const float raw = port_.value01();
        if (std::isfinite(raw))
            last01_ = raw;
        return last01_;
    }

    int GuiValue::count() const
    {
        if (control_.type == ControlType::toggle)
            return 2;
        if (control_.type == ControlType::integer)
            return static_cast<int>(std::lround(control_.max) - std::lround(control_.min)) + 1;
        return 0;
    }

    int GuiValue::index() const
    {
        return std::clamp(static_cast<int>(std::lround(plain()) - std::lround(control_.min)), 0, std::max(0, count() - 1));
    }

    std::string GuiValue::nameOf(int k) const
    {
        if (k >= 0 && static_cast<std::size_t>(k) < options_.size() && !options_[static_cast<std::size_t>(k)].empty())
            return options_[static_cast<std::size_t>(k)];
        if (control_.type == ControlType::toggle)
            return k == 0 ? "OFF" : "ON";
        return std::to_string(std::lround(control_.min) + k);
    }

    std::string GuiValue::text() const
    {
        if (control_.type != ControlType::real)
            return nameOf(index());
        char buf[32];
        formatValue(control_, plain(), buf, sizeof buf);
        return buf;
    }

    float GuiValue::step() const
    {
        const int n = count();
        return n > 1 ? 1.0f / static_cast<float>(n - 1) : 0.01f;
    }

    // ---- GuiControlItem -------------------------------------------------------------------------------------------------
    void GuiControlItem::place(const GuiNode& node, const Rect& cell, const Rect& area, bool shown, const std::string& color)
    {
        kind_ = node.kind;
        size_ = node.size;
        vertical_ = node.vertical;
        vintage_ = node.vintage;
        style_ = node.style;
        if (vintage_ && style_.look.empty())
            style_.look = "vintage";
        liveDisplay_ = node.live;
        if (liveDisplay_ < 0)
            live01_ = -1.0f;
        cell_ = cell;
        area_ = area;
        shown_ = shown;
        color_ = color;
        if (kind_ == GuiKind::knob)
        {
            // The cell the layout gave is the knob's as guiKnobGeom makes it, centred in what the layout gave.
            const float lw = funkgui::text::width(funkgui::FontService::get().atlas(), value_.label().c_str(), type::kCaption) + 6.0f;
            geom_ = guiKnobGeom(node, lw);
            geom_.cx = 0.5f * cell_.w;
        }
        if (!shown_)
            dragging_ = false;
    }

    void GuiControlItem::setLive(float plain)
    {
        live01_ = std::isfinite(plain) ? normOf(value_.control(), plain) : -1.0f;
    }

    void GuiControlItem::tick(float, bool hovered, bool, funkgui::Point pointer)
    {
        hovered_ = hovered;
        hoverButton_ = hovered && kind_ == GuiKind::buttons ? buttonAt(pointer) : -1;
    }

    Rect GuiControlItem::track() const
    {
        if (kind_ != GuiKind::slider)
            return {};
        if (vertical_)
            return { cell_.centreX() - 2.0f, cell_.y + 20.0f, 4.0f, std::max(10.0f, cell_.h - 40.0f) };
        return { cell_.x + 8.0f, cell_.y + 28.0f, std::max(10.0f, cell_.w - 16.0f), 4.0f };
    }

    int GuiControlItem::buttonAt(funkgui::Point p) const
    {
        if (kind_ != GuiKind::buttons || !cell_.contains(p))
            return -1;
        const int n = std::min(value_.count(), kMaxOptions);
        if (n <= 0)
            return -1;
        if (vertical_)
        {
            const int row = static_cast<int>(std::floor((p.y - cell_.y - kGuiLabelH) / 14.0f));
            return row >= 0 && row < n ? row : -1;
        }
        if (p.y < cell_.y + kGuiLabelH)
            return -1;
        const int k = static_cast<int>(std::floor((p.x - cell_.x) / (cell_.w / static_cast<float>(n))));
        return k >= 0 && k < n ? k : -1;
    }

    float GuiControlItem::sliderValueAt(funkgui::Point p) const
    {
        const Rect t = track();
        return vertical_ ? clamp01((t.bottom() - p.y) / std::max(1.0f, t.h)) : clamp01((p.x - t.x) / std::max(1.0f, t.w));
    }

    funkgui::Cursor GuiControlItem::cursorAt(funkgui::Point) const
    {
        if (clicks())
            return funkgui::Cursor::pointingHand;
        if (kind_ == GuiKind::slider && !vertical_)
            return funkgui::Cursor::leftRight;
        return funkgui::Cursor::upDown;
    }

    void GuiControlItem::draw(funkgui::Canvas& c, const funkgui::Theme& th, bool focusRing) const
    {
        if (!shown())
            return;
        const bool tinted = !color_.empty();
        const Col col = colorOf(color_, th, th.accent);
        const Rect& r = cell_;
        const std::string& label = value_.label();
        const std::string text = value_.text();
        const bool hot = hovered_ || dragging_;
        const Col labelInk = hot ? th.ink70 : th.ink52;
        switch (kind_)
        {
            case GuiKind::knob:
                drawKnob(c, th, hot);
                break;
            case GuiKind::slider:
                drawSlider(c, th, hot);
                break;
            case GuiKind::toggle:
            {
                looks::Knob k;
                k.cell = r;
                k.t = value_.index() > 0 ? 1.0f : 0.0f;
                k.label = label.c_str();
                k.value = text.c_str();
                k.atDefault = value_.atDefault();
                k.hover = hot;
                k.color = col;
                k.tinted = tinted;
                looks::toggle(c, th, k, text.c_str());
                break;
            }
            case GuiKind::buttons:
            {
                std::vector<std::string> names;
                const int n = std::min(value_.count(), kMaxOptions);
                for (int i = 0; i < n; ++i)
                    names.push_back(value_.nameOf(i));
                if (vertical_)
                {
                    looks::Knob k;
                    k.cell = r;
                    k.label = label.c_str();
                    k.value = text.c_str();
                    k.hover = hot;
                    k.color = col;
                    k.tinted = tinted;
                    looks::buttons(c, th, k, names, value_.index(), hoverButton_);
                    break;
                }
                c.text(label.c_str(), r.centreX(), r.y, type::kCaption, labelInk, funkgui::Align::centre);
                const float w = r.w / static_cast<float>(std::max(1, n));
                const Col lit = tinted ? col : th.accent;
                for (int i = 0; i < n; ++i)
                {
                    const Rect b{ r.x + w * static_cast<float>(i) + 1.0f, r.y + kGuiLabelH, w - 2.0f, 20.0f };
                    const bool on = i == value_.index();
                    const Col fill = on ? funkgui::premix(th.ground, lit, 0.30f)
                                        : funkgui::mix(th.ground, th.ink16, i == hoverButton_ ? 0.6f : 0.3f);
                    c.rrect(b.x, b.y, b.w, b.h, 2.0f, fill, 1.0f, on ? lit : th.ink32);
                    c.text(names[static_cast<std::size_t>(i)].c_str(), b.centreX(), c.capCentreTop(b.centreY(), type::kMicro),
                           type::kMicro, on ? th.ink100 : (i == hoverButton_ ? th.ink70 : th.ink52), funkgui::Align::centre);
                }
                break;
            }
            case GuiKind::menu:
            {
                c.text(label.c_str(), r.x + 2.0f, r.y, type::kCaption, labelInk);
                const Rect b{ r.x, r.y + kGuiLabelH, r.w, 22.0f };
                c.rrect(b.x, b.y, b.w, b.h, 3.0f, funkgui::mix(th.ground, th.ink16, hot ? 0.6f : 0.35f), 1.0f,
                        hot ? th.accent : (tinted ? col : th.ink32));
                c.text(fitText(c, text, type::kMicro, b.w - 26.0f).c_str(), b.x + 8.0f, c.capCentreTop(b.centreY(), type::kMicro),
                       type::kMicro, th.ink100);
                // A chevron at the right.
                const float cx = b.right() - 11.0f, cyy = b.centreY();
                c.segment(cx - 4.0f, cyy - 2.0f, cx, cyy + 2.0f, 1.5f, th.ink70);
                c.segment(cx, cyy + 2.0f, cx + 4.0f, cyy - 2.0f, 1.5f, th.ink70);
                break;
            }
            case GuiKind::row: case GuiKind::column: case GuiKind::grid: case GuiKind::section: case GuiKind::box:
            case GuiKind::tabs: case GuiKind::page: case GuiKind::spacer: case GuiKind::divider: case GuiKind::label:
            case GuiKind::xy: case GuiKind::envelope: case GuiKind::display: case GuiKind::keyboard: case GuiKind::shader:
                break;
        }
        if (focusRing)
            ring(c, th, r);
    }

    // A knob in its look (GuiStyle): the track and the value's arc over `sweep` degrees (from the middle when bipolar),
    // a cap, a pointer, ticks, a shadow, the live= field's mark, the label and the value where its geometry puts them.
    void GuiControlItem::drawKnob(funkgui::Canvas& c, const funkgui::Theme& th, bool hot) const
    {
        const GuiStyle& st = style_;
        const GuiKnobGeom& g = geom_;
        const Rect& r = cell_;
        const std::string& look = st.look.empty() ? std::string("modern") : st.look;
        const float t = value_.value01();
        const funkgui::Point ctr{ r.x + g.cx, r.y + g.cy };
        const float rad = 0.5f * g.d;
        const float sweep = (st.sweep > 0.0f ? st.sweep : 270.0f) * kDeg;
        const float a0 = 0.5f * kPi + 0.5f * (2.0f * kPi - sweep), a1 = a0 + sweep;
        const float ang = a0 + t * sweep, mid = a0 + 0.5f * sweep;
        const bool tinted = !color_.empty();
        const Col accent = tinted ? colorOf(color_, th, th.accent) : th.accent;
        const bool noArc = st.arc == "none", noTrack = st.track == "none";
        const Col arcCol = !st.arc.empty() && !noArc ? colorOf(st.arc, th, accent) : (tinted ? accent : (hot ? th.accent : th.ink70));
        const Col trackCol = !st.track.empty() && !noTrack ? colorOf(st.track, th, th.ink16) : th.ink16;
        const Col pointerCol = !st.pointerColor.empty() ? colorOf(st.pointerColor, th, th.ink100) : (hot ? th.accent : th.ink100);
        const Col tickCol = !st.ticks.empty() ? colorOf(st.ticks, th, th.ink32) : (hot ? th.ink52 : th.ink32);
        const Col labelCol = !st.label.empty() ? colorOf(st.label, th, th.ink52) : (hot ? th.ink70 : th.ink52);
        const Col valueCol = !st.value.empty() ? colorOf(st.value, th, th.ink100) : valueInk(th, hot, value_.atDefault());
        const float thick = st.thickness > 0.0f ? st.thickness
                          : (look == "minimal" ? std::max(3.0f, 0.13f * g.d) : (g.d >= 48.0f ? 3.0f : 2.0f));
        const Col black = capBlack(th);
        const bool noCap = st.cap == "none";
        const auto capOr = [&](Col fallback) { return !st.cap.empty() && !noCap ? colorOf(st.cap, th, fallback) : fallback; };
        // What every look shares: the shadow, the ticks.
        if (st.shadow == 1 || look == "soft")
            c.rrect(ctr.x - rad, ctr.y - rad + 0.08f * g.d, g.d, g.d, rad, Col{ 0, 0, 0, 120 }, 0.0f, {}, 0.12f * g.d);
        const int ticks = st.tickCount >= 0 ? st.tickCount : (look == "vintage" || look == "chicken" ? 11 : 0);
        for (int i = 0; i < ticks; ++i)
        {
            const float f = ticks > 1 ? static_cast<float>(i) / static_cast<float>(ticks - 1) : 0.5f;
            const bool major = i == 0 || i == ticks - 1 || (ticks % 2 == 1 && i == ticks / 2);
            radial(c, ctr, rad + 3.0f, rad + (major ? 8.0f : 6.0f), a0 + f * sweep, 1.0f, tickCol);
        }
        const float from = st.bipolar ? mid : a0;
        const auto valueArc = [&](float radius, float width) {
            if (noArc)
                return;
            const float lo = std::min(from, ang), hi = std::max(from, ang);
            if (hi - lo > 0.005f)
                c.arc(ctr, radius, lo, hi, width, arcCol);
        };
        const auto pointer = [&](float r0, float r1, float width, Col colr) {
            const int ptr = st.pointer >= 0 ? st.pointer : (look == "dot" ? 1 : (look == "minimal" || look == "ring" || look == "led" ? 3 : 0));
            if (ptr == 3)
                return;
            if (ptr == 1)
            {
                const funkgui::Point p = at(ctr, 0.5f * (r0 + r1), ang);
                c.disc(p.x, p.y, std::max(1.5f, 0.07f * g.d), colr);
            }
            else if (ptr == 2)
                radial(c, ctr, r0 + 0.55f * (r1 - r0), r1, ang, std::max(3.0f, 0.1f * g.d), colr);
            else
                radial(c, ctr, r0, r1, ang, width, colr);
        };
        if (look == "vintage")
        {
            const float skirt = rad + 1.0f, cap = rad * 0.72f;
            c.disc(ctr.x, ctr.y, skirt, funkgui::mix(black, th.ink16, 0.55f), 1.0f, th.ink32);
            radial(c, ctr, cap + 1.0f, skirt - 0.5f, ang, 2.0f, hot ? th.accent : funkgui::mix(th.ink70, th.ink100, 0.5f));
            const Col capCol = capOr(tinted ? funkgui::mix(black, accent, 0.85f) : black);
            c.disc(ctr.x, ctr.y, cap, capCol, 1.0f, funkgui::mix(capCol, Col{ 0xFF, 0xFF, 0xFF }, 0.18f));
            pointer(rad * 0.2f, cap - 1.5f, g.d >= 48.0f ? 3.0f : 2.0f, !st.pointerColor.empty() ? pointerCol : Col{ 0xF2, 0xEF, 0xE8 });
        }
        else if (look == "flat")
        {
            if (!noTrack)
                c.arc(ctr, rad, a0, a1, thick, trackCol);
            valueArc(rad, thick);
            const Col capCol = capOr(funkgui::mix(th.ground, th.ink16, 0.9f));
            if (!noCap)
                c.disc(ctr.x, ctr.y, rad - thick - 2.0f, capCol, 1.0f, th.ink32);
            pointer(0.15f * rad, rad - thick - 3.0f, std::max(2.0f, 0.06f * g.d), pointerCol);
        }
        else if (look == "minimal")
        {
            if (!noTrack)
                c.arc(ctr, rad - 0.5f * thick, a0, a1, thick, trackCol);
            valueArc(rad - 0.5f * thick, thick);
            pointer(rad - thick * 1.5f - 4.0f, rad - thick - 1.0f, 2.0f, pointerCol);
        }
        else if (look == "ring" || look == "led")
        {
            // A ring of lights, lit up to the value (from the middle when bipolar).
            const int n = std::clamp(static_cast<int>(std::round(sweep / kDeg / 13.5f)), 11, 31);
            const float dot = std::max(1.5f, (look == "led" ? 0.055f : 0.045f) * g.d);
            for (int i = 0; i < n; ++i)
            {
                const float a = a0 + sweep * static_cast<float>(i) / static_cast<float>(n - 1);
                const bool lit = !noArc && a >= std::min(from, ang) - 0.01f && a <= std::max(from, ang) + 0.01f;
                const funkgui::Point p = at(ctr, rad - dot, a);
                if (lit && look == "led")            // a glow under a lit light
                    c.disc(p.x, p.y, dot * 2.2f, arcCol.withAlpha(0.25f));
                c.disc(p.x, p.y, dot, lit ? arcCol : (noTrack ? th.ground.withAlpha(0.0f) : trackCol));
            }
            const Col capCol = capOr(funkgui::mix(th.ground, th.ink16, 0.8f));
            if (!noCap)
                c.disc(ctr.x, ctr.y, rad - 3.0f * dot - 3.0f, capCol, 1.0f, th.ink32);
            pointer(0.2f * rad, rad - 3.0f * dot - 4.0f, 2.0f, pointerCol);
        }
        else if (look == "chicken")
        {
            // A chicken-head knob: a round base and a pointer of a bar that tapers to its tip.
            const Col body = capOr(black);
            c.disc(ctr.x, ctr.y, rad * 0.62f, body, 1.0f, funkgui::mix(body, Col{ 0xFF, 0xFF, 0xFF }, 0.18f));
            const funkgui::Point tip = at(ctr, rad, ang), tail = at(ctr, rad * 0.55f, ang + kPi);
            c.segment(tail.x, tail.y, tip.x, tip.y, std::max(5.0f, 0.24f * g.d), body);
            const funkgui::Point tip2 = at(ctr, rad * 0.98f, ang);
            c.segment(ctr.x, ctr.y, tip2.x, tip2.y, std::max(1.5f, 0.05f * g.d), !st.pointerColor.empty() ? pointerCol : Col{ 0xF2, 0xEF, 0xE8 });
        }
        else if (look == "soft")
        {
            // A shaded cap: discs from its rim to a highlight above its middle, the light from the top left.
            const Col base = capOr(funkgui::mix(th.ground, th.ink16, 1.4f));
            for (int k = 0; k < 6; ++k)
            {
                const float f = static_cast<float>(k) / 6.0f;
                const float rr = rad * (1.0f - 0.55f * f);
                const float off = -0.12f * rad * f;
                c.disc(ctr.x + off, ctr.y + off, rr, funkgui::mix(base, Col{ 0xFF, 0xFF, 0xFF }, 0.05f + 0.06f * f));
            }
            if (!noArc)
                valueArc(rad + 3.0f, std::max(2.0f, thick));
            pointer(rad * 0.55f, rad * 0.85f, 2.0f, pointerCol);
        }
        else if (look == "dot")
        {
            if (!noTrack)
                c.arc(ctr, rad, a0, a1, 1.0f, trackCol);
            valueArc(rad, std::max(1.5f, thick - 0.5f));
            const Col capCol = capOr(funkgui::mix(th.ground, th.ink16, 0.7f));
            if (!noCap)
                c.disc(ctr.x, ctr.y, rad * 0.72f, capCol, 1.0f, th.ink32);
            pointer(rad * 0.25f, rad * 0.55f, 2.0f, pointerCol);
        }
        else                                         // modern
        {
            if (!noTrack)
                c.arc(ctr, rad, a0, a1, thick, trackCol);
            valueArc(rad, thick);
            if (g.d >= 48.0f && !noCap)              // a cap under the pointer
                c.disc(ctr.x, ctr.y, rad - 11.0f, capOr(funkgui::mix(th.ground, th.ink16, 0.6f)), 1.0f, th.ink32);
            else if (!st.cap.empty() && !noCap)
                c.disc(ctr.x, ctr.y, rad - thick - 3.0f, capOr(th.ink16), 1.0f, th.ink32);
            pointer(rad - (g.d >= 48.0f ? 9.0f : 6.0f), rad + 2.0f, thick, pointerCol);
        }
        // The live= field: where the program has the parameter now (its modulation), as a mark on the ring and an arc
        // from the knob's value to it.
        if (live01_ >= 0.0f)
        {
            const float la = a0 + std::clamp(live01_, 0.0f, 1.0f) * sweep;
            const float lr = rad + 4.0f;
            const Col liveCol = funkgui::mix(arcCol, Col{ 0xFF, 0xFF, 0xFF }, 0.35f);
            if (std::fabs(la - ang) > 0.01f)
                c.arc(ctr, lr, std::min(la, ang), std::max(la, ang), 1.5f, liveCol.withAlpha(0.6f));
            const funkgui::Point p = at(ctr, lr, la);
            c.disc(p.x, p.y, 2.5f, liveCol);
        }
        const std::string& label = value_.label();
        const bool small = g.d < 26.0f;
        if (g.labelTop)
            c.text(label.c_str(), ctr.x, r.y + g.labelY, type::kCaption, labelCol, funkgui::Align::centre);
        if (g.labelBottom)
            c.text(label.c_str(), ctr.x, r.y + g.labelY, small ? type::kMicro : type::kCaption, labelCol, funkgui::Align::centre);
        if (g.value && (st.valueShow != 1 || hot))
            c.text(value_.text().c_str(), ctr.x, r.y + g.valueY, type::kMicro, valueCol, funkgui::Align::centre);
    }

    // A slider: its slot (track=), the value's fill along it (arc=, from the middle when bipolar), its cap (cap=), its
    // live= field's mark, the label and the value.
    void GuiControlItem::drawSlider(funkgui::Canvas& c, const funkgui::Theme& th, bool hot) const
    {
        const GuiStyle& st = style_;
        const Rect& r = cell_;
        const Rect tr = track();
        const float t = value_.value01();
        const bool tinted = !color_.empty();
        const Col col = colorOf(color_, th, th.accent);
        const Col capCol = !st.cap.empty() && st.cap != "none" ? colorOf(st.cap, th, th.ink70)
                                                               : (tinted ? col : (looks::dark(th) ? th.ink70 : th.ink52));
        const Col slot = !st.track.empty() && st.track != "none" ? colorOf(st.track, th, th.ink16) : funkgui::mix(capBlack(th), th.ink16, 0.5f);
        const Col fill = !st.arc.empty() && st.arc != "none" ? colorOf(st.arc, th, th.accent) : (tinted ? col : th.accent);
        const bool showFill = !st.arc.empty() && st.arc != "none";
        const Col labelCol = !st.label.empty() ? colorOf(st.label, th, th.ink52) : (hot ? th.ink70 : th.ink52);
        const Col valueCol = !st.value.empty() ? colorOf(st.value, th, th.ink100) : valueInk(th, hot, value_.atDefault());
        const bool labelShown = st.labelPos != 2, valueShown = st.valueShow != 2 && (st.valueShow != 1 || hot);
        const float from = st.bipolar ? 0.5f : 0.0f;
        if (st.shadow == 1)
            c.rrect(tr.x - 1.0f, tr.y + 2.0f, tr.w + 2.0f, tr.h, 3.0f, Col{ 0, 0, 0, 110 }, 0.0f, {}, 4.0f);
        if (vertical_)
        {
            if (labelShown)
                c.text(value_.label().c_str(), r.centreX(), r.y, type::kCaption, labelCol, funkgui::Align::centre);
            for (int i = 0; i <= 4; ++i)
            {
                const float y = tr.y + tr.h * static_cast<float>(i) / 4.0f;
                c.hairlineH(tr.centreX() - 13.0f, y, 5.0f, th.ink32);
                c.hairlineH(tr.centreX() + 8.0f, y, 5.0f, th.ink32);
            }
            c.rrect(tr.x, tr.y, tr.w, tr.h, 2.0f, st.track == "none" ? th.ground.withAlpha(0.0f) : slot, 1.0f, th.ink32);
            if (showFill)
            {
                const float y0 = tr.bottom() - tr.h * std::max(from, t), y1 = tr.bottom() - tr.h * std::min(from, t);
                c.rrect(tr.x, y0, tr.w, std::max(0.0f, y1 - y0), 2.0f, fill);
            }
            if (live01_ >= 0.0f)
                c.hairlineH(tr.centreX() - 12.0f, tr.bottom() - tr.h * std::clamp(live01_, 0.0f, 1.0f), 24.0f,
                            funkgui::mix(fill, Col{ 0xFF, 0xFF, 0xFF }, 0.35f));
            const float y = tr.bottom() - tr.h * t;
            c.rrect(tr.centreX() - 10.0f, y - 5.0f, 20.0f, 10.0f, 2.0f, hot ? funkgui::mix(capCol, th.accent, 0.5f) : capCol,
                    1.0f, funkgui::mix(capCol, capBlack(th), 0.4f));
            c.hairlineH(tr.centreX() - 8.0f, y, 16.0f, capBlack(th));
            if (valueShown)
                c.text(value_.text().c_str(), r.centreX(), r.bottom() - 12.0f, type::kMicro, valueCol, funkgui::Align::centre);
        }
        else
        {
            if (labelShown)
                c.text(value_.label().c_str(), r.x + 8.0f, r.y + 2.0f, type::kCaption, labelCol);
            if (valueShown)
                c.text(value_.text().c_str(), r.right() - 8.0f, r.y + 2.0f, type::kMicro, valueCol, funkgui::Align::right);
            for (int i = 0; i <= 4; ++i)
                c.hairlineV(tr.x + tr.w * static_cast<float>(i) / 4.0f, tr.y - 9.0f, 5.0f, th.ink32);
            c.rrect(tr.x, tr.y, tr.w, tr.h, 2.0f, st.track == "none" ? th.ground.withAlpha(0.0f) : slot, 1.0f, th.ink32);
            if (showFill)
            {
                const float x0 = tr.x + tr.w * std::min(from, t), x1 = tr.x + tr.w * std::max(from, t);
                c.rrect(x0, tr.y, std::max(0.0f, x1 - x0), tr.h, 2.0f, fill);
            }
            if (live01_ >= 0.0f)
                c.hairlineV(tr.x + tr.w * std::clamp(live01_, 0.0f, 1.0f), tr.centreY() - 10.0f, 20.0f,
                            funkgui::mix(fill, Col{ 0xFF, 0xFF, 0xFF }, 0.35f));
            const float x = tr.x + tr.w * t;
            c.rrect(x - 5.0f, tr.centreY() - 9.0f, 10.0f, 18.0f, 2.0f, hot ? funkgui::mix(capCol, th.accent, 0.5f) : capCol,
                    1.0f, funkgui::mix(capCol, capBlack(th), 0.4f));
            c.hairlineV(x, tr.centreY() - 7.0f, 14.0f, capBlack(th));
        }
    }

    void GuiControlItem::openMenu(funkgui::GestureController& g, funkgui::HostServices& h)
    {
        funkgui::MenuRequest request;
        const int n = std::min(value_.count(), kMaxMenuOptions);
        for (int k = 0; k < n; ++k)
        {
            funkgui::MenuItem item;
            item.id = k + 1;
            item.label = value_.nameOf(k);
            item.checked = k == value_.index();
            request.items.push_back(std::move(item));
        }
        request.anchor = { cell_.x, cell_.y + kGuiLabelH, cell_.w, 22.0f };
        if (menuTheme_)
            request.theme = menuTheme_();
        funkgui::GestureController* gestures = &g;
        h.showMenu(request, [this, gestures, n](int chosen) {
            if (chosen >= 1 && chosen <= n)
                gestures->tap(value_.port(), value_.normOfIndex(chosen - 1));
        });
    }

    void GuiControlItem::pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController& g, funkgui::HostServices& h)
    {
        if (e.popup || !shown())
            return;
        const funkgui::Point p{ e.x, e.y };
        switch (kind_)
        {
            case GuiKind::toggle:
                g.tap(value_.port(), value_.normOfIndex(value_.index() == 0 ? value_.count() - 1 : 0));
                return;
            case GuiKind::buttons:
                if (const int k = buttonAt(p); k >= 0)
                    g.tap(value_.port(), value_.normOfIndex(k));
                return;
            case GuiKind::menu:
                openMenu(g, h);
                return;
            case GuiKind::slider:
            {
                g.beginDrag(value_.port());
                dragging_ = true;
                // Grabbed on its cap it moves from where it is; elsewhere on the track it jumps there first.
                const Rect tr = track();
                const float at = vertical_ ? tr.bottom() - tr.h * value_.value01() : tr.x + tr.w * value_.value01();
                const float pos = vertical_ ? e.y : e.x;
                dragStart01_ = std::fabs(pos - at) <= 7.0f ? value_.value01() : sliderValueAt(p);
                lastY_ = pos;
                g.dragTo(dragStart01_);
                return;
            }
            case GuiKind::knob:
                g.beginDrag(value_.port());
                dragging_ = true;
                dragStart01_ = value_.value01();
                lastY_ = e.y;
                return;
            case GuiKind::row: case GuiKind::column: case GuiKind::grid: case GuiKind::section: case GuiKind::box:
            case GuiKind::tabs: case GuiKind::page: case GuiKind::spacer: case GuiKind::divider: case GuiKind::label:
            case GuiKind::xy: case GuiKind::envelope: case GuiKind::display: case GuiKind::keyboard: case GuiKind::shader:
                return;
        }
    }

    void GuiControlItem::pointerDrag(const funkgui::PointerEvent& e, funkgui::GestureController& g)
    {
        if (!dragging_)
            return;
        const float fine = e.mods.shift ? 0.1f : 1.0f;
        if (kind_ == GuiKind::knob)
        {
            dragStart01_ = clamp01(dragStart01_ + (lastY_ - e.y) * fine / kDragRange);
            lastY_ = e.y;
        }
        else
        {
            const Rect tr = track();
            const float pos = vertical_ ? e.y : e.x;
            const float len = std::max(1.0f, vertical_ ? tr.h : tr.w);
            dragStart01_ = clamp01(dragStart01_ + (vertical_ ? lastY_ - pos : pos - lastY_) * fine / len);
            lastY_ = pos;
        }
        g.dragTo(dragStart01_);
    }

    void GuiControlItem::pointerUp(const funkgui::PointerEvent&, funkgui::GestureController& g)
    {
        if (!dragging_)
            return;
        dragging_ = false;
        g.endDrag();
    }

    void GuiControlItem::doubleClick(const funkgui::PointerEvent&, funkgui::GestureController& g)
    {
        if (!clicks() && shown())
            g.tap(value_.port(), value_.default01());
    }

    bool GuiControlItem::wheel(const funkgui::WheelEvent& e, funkgui::GestureController& g, double now)
    {
        if (!shown())
            return false;
        float dy = e.dy != 0.0f ? e.dy : -e.dx;
        if (e.reversed)
            dy = -dy;
        if (dy == 0.0f)
            return true;
        const float step = value_.step() * (e.mods.shift ? 0.1f : 1.0f);
        g.wheelTo(value_.port(), clamp01(value_.value01() + (dy > 0.0f ? step : -step)), now);
        return true;
    }

    bool GuiControlItem::key(const funkgui::KeyEvent& e, funkgui::GestureController& g)
    {
        if (!shown())
            return false;
        const float step = value_.step() * (e.mods.shift && value_.count() == 0 ? 10.0f : 1.0f);
        const float v = value_.value01();
        switch (e.key)
        {
            case funkgui::Key::up:
            case funkgui::Key::right:     g.tap(value_.port(), clamp01(v + step)); return true;
            case funkgui::Key::down:
            case funkgui::Key::left:      g.tap(value_.port(), clamp01(v - step)); return true;
            case funkgui::Key::pageUp:    g.tap(value_.port(), clamp01(v + 10.0f * value_.step())); return true;
            case funkgui::Key::pageDown:  g.tap(value_.port(), clamp01(v - 10.0f * value_.step())); return true;
            case funkgui::Key::home:      g.tap(value_.port(), 0.0f); return true;
            case funkgui::Key::end:       g.tap(value_.port(), 1.0f); return true;
            case funkgui::Key::enter:
            case funkgui::Key::space:
                if (!clicks())
                    return false;
                g.tap(value_.port(), value_.normOfIndex((value_.index() + 1) % std::max(1, value_.count())));
                return true;
            case funkgui::Key::character: case funkgui::Key::tab: case funkgui::Key::escape:
            case funkgui::Key::backspace: case funkgui::Key::del:
                return false;
        }
        return false;
    }

    void GuiControlItem::accessibility(std::vector<funkgui::A11yItem>& out) const
    {
        if (!shown())
            return;
        funkgui::A11yItem item;
        item.id = id_;
        item.bounds = cell_;
        item.title = value_.label();
        item.value = value_.text();
        switch (kind_)
        {
            case GuiKind::toggle:
                item.role = funkgui::A11yRole::toggleButton;
                item.checkable = true;
                item.checked = value_.index() > 0;
                break;
            case GuiKind::buttons:
            case GuiKind::menu:
                item.role = funkgui::A11yRole::comboBox;
                break;
            case GuiKind::knob: case GuiKind::slider: case GuiKind::row: case GuiKind::column: case GuiKind::grid:
            case GuiKind::section: case GuiKind::box: case GuiKind::tabs: case GuiKind::page: case GuiKind::spacer:
            case GuiKind::divider: case GuiKind::label: case GuiKind::xy: case GuiKind::envelope: case GuiKind::display: case GuiKind::keyboard: case GuiKind::shader:
                item.role = funkgui::A11yRole::slider;
                item.v = value_.value01();
                item.lo = 0.0;
                item.hi = 1.0;
                item.step = value_.step();
                break;
        }
        out.push_back(std::move(item));
    }

    bool GuiControlItem::a11yAction(uint32_t id, funkgui::A11yAction a, double v, funkgui::GestureController& g)
    {
        if (id != id_ || !shown())
            return false;
        const float now = value_.value01();
        switch (a)
        {
            case funkgui::A11yAction::increment: g.tap(value_.port(), clamp01(now + value_.step())); break;
            case funkgui::A11yAction::decrement: g.tap(value_.port(), clamp01(now - value_.step())); break;
            case funkgui::A11yAction::setValue:  g.tap(value_.port(), clamp01(static_cast<float>(v))); break;
            case funkgui::A11yAction::press:
            case funkgui::A11yAction::toggle:
            case funkgui::A11yAction::showMenu:
                if (clicks())
                    g.tap(value_.port(), value_.normOfIndex((value_.index() + 1) % std::max(1, value_.count())));
                break;
            case funkgui::A11yAction::focus:
                break;
        }
        return true;
    }

    void GuiControlItem::specLine(char* out, std::size_t n) const
    {
        const Control& c = value_.control();
        const char* how = kind_ == GuiKind::menu ? "A CLICK CHOOSES"
                        : kind_ == GuiKind::toggle ? "A CLICK FLIPS IT"
                        : kind_ == GuiKind::buttons ? "A CLICK CHOOSES"
                                                    : "DRAG, WHEEL OR ARROWS   SHIFT FINER   DOUBLE-CLICK: DEFAULT";
        std::snprintf(out, n, "%s   %s   %s   %s", value_.label().c_str(), value_.text().c_str(), upper(c.name).c_str(), how);
    }

    // ---- GuiXyItem ------------------------------------------------------------------------------------------------------
    void GuiXyItem::place(const GuiNode& node, const Rect& cell, const Rect& area, bool shown, const std::string& color)
    {
        cell_ = cell;
        area_ = area;
        shown_ = shown;
        color_ = color;
        label_ = upper(asciiText(node.text));
        if (!shown_)
            dragging_ = false;
    }

    Rect GuiXyItem::pad() const { return { cell_.x, cell_.y + kGuiLabelH, cell_.w, std::max(10.0f, cell_.h - kGuiLabelH) }; }

    void GuiXyItem::moveTo(funkgui::Point p, funkgui::GestureController& g)
    {
        if (vx_ == nullptr || vy_ == nullptr)
            return;
        const Rect r = pad();
        const std::pair<funkgui::ParamPort*, float> writes[] = { { &vx_->port(), clamp01((p.x - r.x) / r.w) },
                                                                  { &vy_->port(), clamp01((r.bottom() - p.y) / r.h) } };
        g.tapMany(writes);
    }

    void GuiXyItem::pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController& g, funkgui::HostServices&)
    {
        if (e.popup || !shown())
            return;
        dragging_ = true;
        moveTo({ e.x, e.y }, g);
    }

    void GuiXyItem::pointerDrag(const funkgui::PointerEvent& e, funkgui::GestureController& g)
    {
        if (dragging_)
            moveTo({ e.x, e.y }, g);
    }

    void GuiXyItem::doubleClick(const funkgui::PointerEvent&, funkgui::GestureController& g)
    {
        if (vx_ == nullptr || vy_ == nullptr)
            return;
        const std::pair<funkgui::ParamPort*, float> writes[] = { { &vx_->port(), vx_->default01() },
                                                                  { &vy_->port(), vy_->default01() } };
        g.tapMany(writes);
    }

    bool GuiXyItem::key(const funkgui::KeyEvent& e, funkgui::GestureController& g)
    {
        if (!shown())
            return false;
        const float step = e.mods.shift ? 0.1f : 0.01f;
        switch (e.key)
        {
            case funkgui::Key::left:  g.tap(vx_->port(), clamp01(vx_->value01() - step)); return true;
            case funkgui::Key::right: g.tap(vx_->port(), clamp01(vx_->value01() + step)); return true;
            case funkgui::Key::up:    g.tap(vy_->port(), clamp01(vy_->value01() + step)); return true;
            case funkgui::Key::down:  g.tap(vy_->port(), clamp01(vy_->value01() - step)); return true;
            case funkgui::Key::character: case funkgui::Key::tab: case funkgui::Key::pageUp: case funkgui::Key::pageDown:
            case funkgui::Key::home: case funkgui::Key::end: case funkgui::Key::escape: case funkgui::Key::enter:
            case funkgui::Key::backspace: case funkgui::Key::del: case funkgui::Key::space:
                return false;
        }
        return false;
    }

    void GuiXyItem::draw(funkgui::Canvas& c, const funkgui::Theme& th, bool focusRing) const
    {
        if (!shown())
            return;
        const Rect r = pad();
        const bool hot = hovered_ || dragging_;
        const Col col = colorOf(color_, th, th.accent);
        const std::string label = label_.empty() ? vx_->label() + " / " + vy_->label() : label_;
        c.text(fitText(c, label, type::kCaption, cell_.w).c_str(), cell_.centreX(), cell_.y, type::kCaption,
               hot ? th.ink70 : th.ink52, funkgui::Align::centre);
        c.rrect(r.x, r.y, r.w, r.h, 3.0f, funkgui::mix(th.ground, th.ink16, 0.35f), 1.0f, hot ? th.ink52 : th.ink32);
        for (int k = 1; k < 4; ++k)
        {
            c.hairlineV(r.x + r.w * static_cast<float>(k) / 4.0f, r.y + 1.0f, r.h - 2.0f, th.ink16);
            c.hairlineH(r.x + 1.0f, r.y + r.h * static_cast<float>(k) / 4.0f, r.w - 2.0f, th.ink16);
        }
        const float px = r.x + r.w * vx_->value01(), py = r.bottom() - r.h * vy_->value01();
        c.hairlineV(px, r.y + 1.0f, r.h - 2.0f, th.ink32);
        c.hairlineH(r.x + 1.0f, py, r.w - 2.0f, th.ink32);
        c.disc(px, py, hot ? 6.0f : 5.0f, col, 1.5f, th.ink100);
        const std::string values = vx_->text() + "  " + vy_->text();
        c.text(fitText(c, values, type::kMicro, r.w - 8.0f).c_str(), r.x + 5.0f, r.bottom() - 15.0f, type::kMicro, th.ink52);
        if (focusRing)
            ring(c, th, r);
    }

    void GuiXyItem::accessibility(std::vector<funkgui::A11yItem>& out) const
    {
        if (!shown())
            return;
        funkgui::A11yItem item;
        item.id = id_;
        item.role = funkgui::A11yRole::slider;
        item.bounds = pad();
        item.title = (label_.empty() ? vx_->label() + " and " + vy_->label() : label_) + " pad";
        item.value = vx_->label() + " " + vx_->text() + ", " + vy_->label() + " " + vy_->text();
        item.v = vx_->value01();
        item.step = 0.01;
        out.push_back(std::move(item));
    }

    bool GuiXyItem::a11yAction(uint32_t id, funkgui::A11yAction a, double v, funkgui::GestureController& g)
    {
        if (id != id_ || !shown())
            return false;
        switch (a)
        {
            case funkgui::A11yAction::increment: g.tap(vx_->port(), clamp01(vx_->value01() + 0.01f)); break;
            case funkgui::A11yAction::decrement: g.tap(vx_->port(), clamp01(vx_->value01() - 0.01f)); break;
            case funkgui::A11yAction::setValue:  g.tap(vx_->port(), clamp01(static_cast<float>(v))); break;
            case funkgui::A11yAction::press: case funkgui::A11yAction::toggle: case funkgui::A11yAction::showMenu:
            case funkgui::A11yAction::focus:
                break;
        }
        return true;
    }

    void GuiXyItem::specLine(char* out, std::size_t n) const
    {
        if (!shown())
        {
            if (n > 0)
                out[0] = '\0';
            return;
        }
        std::snprintf(out, n, "%s %s   %s %s   DRAG MOVES BOTH   ARROWS STEP THEM   DOUBLE-CLICK: DEFAULTS",
                      vx_->label().c_str(), vx_->text().c_str(), vy_->label().c_str(), vy_->text().c_str());
    }

    // ---- GuiEnvelopeItem -------------------------------------------------------------------------------------------------
    void GuiEnvelopeItem::place(const GuiNode& node, const Rect& cell, const Rect& area, bool shown, const std::string& color)
    {
        cell_ = cell;
        area_ = area;
        shown_ = shown;
        color_ = color;
        label_ = upper(asciiText(node.text.empty() ? std::string("ENVELOPE") : node.text));
    }

    bool GuiEnvelopeItem::shown() const
    {
        return shown_ && std::all_of(adsr_.begin(), adsr_.end(), [](const GuiValue* v) { return v != nullptr; });
    }

    std::array<funkgui::Point, 5> GuiEnvelopeItem::corners() const
    {
        std::array<funkgui::Point, 5> p{};
        if (!shown())
            return p;
        const Rect r{ cell_.x + 6.0f, cell_.y + 18.0f, std::max(10.0f, cell_.w - 12.0f), std::max(10.0f, cell_.h - 24.0f) };
        const auto time = [&](int k) { return std::max(0.0f, adsr_[static_cast<std::size_t>(k)]->plain()); };
        const float a = time(0), d = time(1), rel = time(3);
        const Control& sc = adsr_[2]->control();
        const float span = sc.max - sc.min;
        const float s = span > 0.0f ? std::clamp((adsr_[2]->plain() - sc.min) / span, 0.0f, 1.0f) : 0.5f;
        float total = a + d + rel;
        float wa = a, wd = d, wr = rel, hold = 0.25f * total;
        if (!(total > 0.0f))
        {
            wa = wd = wr = hold = 1.0f;
            total = 3.0f;
        }
        const float scale = r.w / (wa + wd + hold + wr);
        const float x0 = r.x, x1 = x0 + wa * scale, x2 = x1 + wd * scale, x3 = x2 + hold * scale, x4 = r.right();
        const float bottom = r.bottom(), top = r.y;
        const float sy = bottom - s * r.h;
        p = { funkgui::Point{ x0, bottom }, funkgui::Point{ x1, top }, funkgui::Point{ x2, sy }, funkgui::Point{ x3, sy },
              funkgui::Point{ x4, bottom } };
        return p;
    }

    void GuiEnvelopeItem::draw(funkgui::Canvas& c, const funkgui::Theme& th, bool) const
    {
        if (!shown())
            return;
        const Col col = colorOf(color_, th, th.accent);
        const Col screen = funkgui::mix(th.ground, th.ink16, 0.30f);
        c.rrect(cell_.x, cell_.y, cell_.w, cell_.h, 3.0f, screen, 1.0f, hovered_ ? th.ink52 : th.ink32);
        c.text(label_.c_str(), cell_.x + 6.0f, cell_.y + 4.0f, type::kMicro, th.ink52);
        const std::array<funkgui::Point, 5> p = corners();
        const Col fill = funkgui::premix(screen, col, 0.22f);
        for (std::size_t k = 0; k + 1 < p.size(); ++k)
            if (p[k + 1].x > p[k].x + 0.5f)
                c.area(p[k].x, p[k + 1].x, p[k].y, p[k + 1].y, p[0].y, p[0].y, fill);
        float xs[5], ys[5];
        for (std::size_t k = 0; k < 5; ++k)
        {
            xs[k] = p[k].x;
            ys[k] = p[k].y;
        }
        c.polyline(xs, ys, 5, 2.0f, col);
        for (std::size_t k = 1; k < 4; ++k)
            c.disc(p[k].x, p[k].y, 2.5f, th.ink100);
    }

    // ---- GuiKeyboardItem --------------------------------------------------------------------------------------------
    std::string noteName(int note)
    {
        static constexpr const char* kNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const int pc = ((note % 12) + 12) % 12;
        const int octave = (note - pc) / 12 - 1;
        return std::string(kNames[pc]) + std::to_string(octave);
    }

    void GuiKeyboardItem::place(const GuiNode& node, const Rect& cell, const Rect& area, bool shown, const std::string& color)
    {
        const int from = node.kind == GuiKind::keyboard ? node.from : from_;
        const int count = node.kind == GuiKind::keyboard ? node.keys : count_;
        if (!shown || from != from_ || count != count_)
            release();
        from_ = from;
        count_ = count;
        cell_ = cell;
        area_ = area;
        shown_ = shown;
        color_ = color;
        wheels_ = node.kind == GuiKind::keyboard && node.wheels;
        keys_.clear();
        const Rect area_k = keysArea();
        const int whites = std::max(1, guiWhiteKeys(from_, count_));
        const float kw = area_k.w / static_cast<float>(whites);
        const auto black = [](int k) {
            const int pc = ((k % 12) + 12) % 12;
            return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
        };
        int w = 0;
        for (int k = from_; k < from_ + count_; ++k)
            if (!black(k))
            {
                keys_.push_back({ { area_k.x + kw * static_cast<float>(w), cell_.y, kw, cell_.h }, k, false });
                ++w;
            }
        // A black key sits over the line between its two white keys, 0.6 of a white key wide and 0.62 as tall.
        w = 0;
        for (int k = from_; k < from_ + count_; ++k)
        {
            if (!black(k))
            {
                ++w;
                continue;
            }
            const float x = area_k.x + kw * static_cast<float>(w);
            const float bw = 0.6f * kw;
            keys_.push_back({ { x - 0.5f * bw, cell_.y, bw, std::round(0.62f * cell_.h) }, k, true });
        }
    }

    Rect GuiKeyboardItem::keysArea() const noexcept
    {
        if (!wheels_)
            return cell_;
        return { cell_.x + kGuiWheelsW, cell_.y, std::max(1.0f, cell_.w - kGuiWheelsW), cell_.h };
    }

    Rect GuiKeyboardItem::pitchWheel() const noexcept
    {
        if (!wheels_)
            return {};
        return { cell_.x + 4.0f, cell_.y + 4.0f, 20.0f, cell_.h - 8.0f };
    }

    Rect GuiKeyboardItem::modWheel() const noexcept
    {
        if (!wheels_)
            return {};
        return { cell_.x + 28.0f, cell_.y + 4.0f, 20.0f, cell_.h - 8.0f };
    }

    void GuiKeyboardItem::moveWheel(float y)
    {
        const Rect r = dragWheel_ == 0 ? pitchWheel() : modWheel();
        const float t = std::clamp((r.bottom() - 6.0f - y) / std::max(1.0f, r.h - 12.0f), 0.0f, 1.0f);   // 0 bottom, 1 top
        if (dragWheel_ == 0)
        {
            const float v = 2.0f * t - 1.0f;
            const float snapped = std::fabs(v) < 0.03f ? 0.0f : v;
            if (snapped != pitch_)
            {
                pitch_ = snapped;
                if (wheel_)
                    wheel_(true, pitch_);
            }
        }
        else if (t != mod_)
        {
            mod_ = t;
            if (wheel_)
                wheel_(false, mod_);
        }
    }

    int GuiKeyboardItem::noteAt(funkgui::Point p, float* velocity) const
    {
        const auto hit = [&](bool blacks) -> int {
            for (const Key& k : keys_)
                if (k.black == blacks && k.r.contains(p))
                {
                    if (velocity != nullptr)        // louder further down the key
                        *velocity = 0.35f + 0.65f * std::clamp((p.y - k.r.y) / std::max(1.0f, k.r.h), 0.0f, 1.0f);
                    return k.note;
                }
            return -1;
        };
        const int b = hit(true);
        return b >= 0 ? b : hit(false);
    }

    funkgui::Point GuiKeyboardItem::centreOfNote(int note) const
    {
        for (const Key& k : keys_)
            if (k.note == note)
                return { k.r.centreX(), k.black ? k.r.y + 0.7f * k.r.h : k.r.y + 0.85f * k.r.h };
        return { cell_.centreX(), cell_.centreY() };
    }

    void GuiKeyboardItem::press(int note, float velocity)
    {
        if (note == held_)
            return;
        release();
        if (note < 0)
            return;
        held_ = note;
        if (play_)
            play_(note, std::max(velocity, 0.05f));
    }

    void GuiKeyboardItem::release()
    {
        if (dragWheel_ == 0 && pitch_ != 0.0f)       // the pitch wheel springs back to its middle
        {
            pitch_ = 0.0f;
            if (wheel_)
                wheel_(true, 0.0f);
        }
        dragWheel_ = -1;
        if (held_ < 0)
            return;
        if (play_)
            play_(held_, 0.0f);
        held_ = -1;
    }

    void GuiKeyboardItem::tick(float, bool hovered, bool, funkgui::Point p)
    {
        hover_ = hovered ? noteAt(p) : -1;
    }

    void GuiKeyboardItem::pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController&, funkgui::HostServices&)
    {
        if (e.popup || !shown_)
            return;
        const funkgui::Point at{ e.x, e.y };
        if (wheels_ && (pitchWheel().contains(at) || modWheel().contains(at)))
        {
            dragWheel_ = pitchWheel().contains(at) ? 0 : 1;
            moveWheel(e.y);
            return;
        }
        float v = 0.8f;
        const int note = noteAt({ e.x, e.y }, &v);
        press(note, v);
    }

    void GuiKeyboardItem::pointerDrag(const funkgui::PointerEvent& e, funkgui::GestureController&)
    {
        if (dragWheel_ >= 0)
        {
            moveWheel(e.y);
            return;
        }
        // A glissando: across the keys, the next note as the pointer reaches it; off them, the note held goes on.
        float v = 0.8f;
        if (const int note = noteAt({ e.x, e.y }, &v); note >= 0)
            press(note, v);
    }

    void GuiKeyboardItem::draw(funkgui::Canvas& c, const funkgui::Theme& th, bool) const
    {
        if (!shown_ || keys_.empty())
            return;
        const Col col = colorOf(color_, th, th.accent);
        const Col ivory{ 0xF1, 0xEF, 0xE8 }, ebony{ 0x1C, 0x1C, 0x20 }, gap{ 0x24, 0x24, 0x28 };
        c.rrect(cell_.x - 1.0f, cell_.y - 1.0f, cell_.w + 2.0f, cell_.h + 2.0f, 4.0f, gap);
        for (const Key& k : keys_)
        {
            if (k.black)
                continue;
            const bool down = k.note == held_, hot = k.note == hover_;
            const Col fill = down ? funkgui::mix(ivory, col, 0.55f) : (hot ? funkgui::mix(ivory, col, 0.12f) : ivory);
            c.rrect(k.r.x + 0.5f, k.r.y, k.r.w - 1.0f, k.r.h, 3.0f, fill);
            if (((k.note % 12) + 12) % 12 == 0 && k.r.w >= 12.0f)
            {
                const std::string name = noteName(k.note);
                c.text(fitText(c, name, type::kMicro, k.r.w - 2.0f).c_str(), k.r.centreX(), k.r.bottom() - 15.0f, type::kMicro,
                       Col{ 0x70, 0x70, 0x74 }, funkgui::Align::centre);
            }
        }
        for (const Key& k : keys_)
        {
            if (!k.black)
                continue;
            const bool down = k.note == held_, hot = k.note == hover_;
            const Col fill = down ? funkgui::mix(ebony, col, 0.7f) : (hot ? funkgui::mix(ebony, col, 0.25f) : ebony);
            c.rrect(k.r.x, k.r.y, k.r.w, k.r.h, 2.5f, fill, 1.0f, gap);
            c.hairlineH(k.r.x + 2.0f, k.r.bottom() - 5.0f, k.r.w - 4.0f, funkgui::mix(fill, Col{ 0xFF, 0xFF, 0xFF }, 0.18f));
        }
        // The top: a felt strip over the keys' ends.
        const Rect keysR = keysArea();
        c.rrect(keysR.x, cell_.y, keysR.w, 3.0f, 0.0f, funkgui::mix(col, ebony, 0.55f));
        if (wheels_)
        {
            // Two wheels in their slots: a dark slot, the wheel's face with ridges, a lit mark where it stands (the
            // pitch wheel's middle drawn).
            for (int w = 0; w < 2; ++w)
            {
                const Rect r = w == 0 ? pitchWheel() : modWheel();
                const float t = w == 0 ? 0.5f * (pitch_ + 1.0f) : mod_;
                const bool hot = dragWheel_ == w;
                c.rrect(r.x, r.y, r.w, r.h, 6.0f, Col{ 0x14, 0x14, 0x17 }, 1.0f, gap);
                const Rect face{ r.x + 3.0f, r.y + 3.0f, r.w - 6.0f, r.h - 6.0f };
                c.rrect(face.x, face.y, face.w, face.h, 4.0f, Col{ 0x2B, 0x2B, 0x30 });
                const float y = face.bottom() - 3.0f - t * (face.h - 6.0f);
                for (int k = -3; k <= 3; ++k)            // ridges, moving with the wheel
                {
                    const float ry = y + static_cast<float>(k) * 5.0f;
                    if (ry > face.y + 2.0f && ry < face.bottom() - 2.0f)
                        c.hairlineH(face.x + 2.0f, ry, face.w - 4.0f, Col{ 0x3A, 0x3A, 0x41 });
                }
                if (w == 0)
                    c.hairlineH(r.x - 2.0f, face.centreY(), 3.0f, Col{ 0x70, 0x70, 0x76 });
                c.rrect(face.x, y - 1.5f, face.w, 3.0f, 1.5f, hot ? funkgui::mix(col, Col{ 0xFF, 0xFF, 0xFF }, 0.25f) : col);
            }
        }
    }

    void GuiKeyboardItem::accessibility(std::vector<funkgui::A11yItem>& out) const
    {
        if (!shown_)
            return;
        funkgui::A11yItem item;
        item.id = id_;
        item.role = funkgui::A11yRole::staticText;
        item.bounds = cell_;
        item.title = "Keyboard";
        item.value = noteName(from_) + " to " + noteName(from_ + count_ - 1) + (held_ >= 0 ? ", playing " + noteName(held_) : "");
        if (wheels_)
        {
            char wheels[64];
            std::snprintf(wheels, sizeof wheels, ", pitch %+.2f, mod %.2f", static_cast<double>(pitch_), static_cast<double>(mod_));
            item.value += wheels;
        }
        item.readOnly = true;
        out.push_back(std::move(item));
    }

    void GuiKeyboardItem::specLine(char* out, std::size_t n) const
    {
        if (!shown_)
        {
            if (n > 0)
                out[0] = '\0';
            return;
        }
        std::snprintf(out, n, "keyboard %s to %s%s%s", noteName(from_).c_str(), noteName(from_ + count_ - 1).c_str(),
                      held_ >= 0 ? "   playing " : "", held_ >= 0 ? noteName(held_).c_str() : "");
    }

    // ---- GuiShaderItem ----------------------------------------------------------------------------------------------
    void GuiShaderItem::place(const GuiNode&, const Rect& cell, const Rect& area, bool shown, uint32_t program,
                              const std::string& file, const std::string& why, bool animated)
    {
        changed_ = changed_ || program != program_ || shown != shown_;
        cell_ = cell;
        area_ = area;
        shown_ = shown;
        program_ = program;
        file_ = file;
        why_ = why;
        animated_ = animated;
    }

    void GuiShaderItem::setInputs(const std::array<float, 8>& v)
    {
        for (std::size_t k = 0; k < v.size(); ++k)
            if (std::fabs(v[k] - inputs_[k]) > 1e-6f || std::isfinite(v[k]) != std::isfinite(inputs_[k]))
                changed_ = true;
        for (std::size_t k = 0; k < v.size(); ++k)
            inputs_[k] = std::isfinite(v[k]) ? v[k] : 0.0f;
    }

    void GuiShaderItem::draw(funkgui::Canvas& c, const funkgui::Theme& th, bool) const
    {
        if (!shown_ || cell_.w <= 0.0f || cell_.h <= 0.0f)
            return;
        if (program_ != 0)
        {
            c.shader(program_, cell_, inputs_.data(), static_cast<int>(inputs_.size()));
            return;
        }
        // The placeholder: a dark plate with the file's name and why it draws nothing.
        c.rrect(cell_.x, cell_.y, cell_.w, cell_.h, 6.0f, funkgui::mix(th.ground, Col{ 0, 0, 0 }, 0.45f), 1.0f, th.ink16);
        const std::string title = "SHADER " + upper(asciiText(file_));
        c.text(fitText(c, title, type::kMicro, cell_.w - 16.0f).c_str(), cell_.x + 8.0f, cell_.y + 8.0f, type::kMicro, th.ink52);
        const std::string why = asciiText(why_.empty() ? std::string("not built yet") : why_);
        // Its reason in as many lines as fit, cut at spaces.
        float y = cell_.y + 24.0f;
        std::size_t at = 0;
        while (at < why.size() && y + 12.0f <= cell_.bottom() - 4.0f)
        {
            std::size_t end = why.size();
            while (end > at && c.textWidth(why.substr(at, end - at).c_str(), type::kMicro) > cell_.w - 16.0f)
            {
                const std::size_t space = why.rfind(' ', end - 1);
                end = space == std::string::npos || space <= at ? at + std::max<std::size_t>(1, (end - at) / 2) : space;
            }
            c.text(why.substr(at, end - at).c_str(), cell_.x + 8.0f, y, type::kMicro, th.ink32);
            at = end;
            while (at < why.size() && why[at] == ' ')
                ++at;
            y += 13.0f;
        }
    }

    void GuiShaderItem::accessibility(std::vector<funkgui::A11yItem>& out) const
    {
        if (!shown_)
            return;
        funkgui::A11yItem item;
        item.id = id_;
        item.role = funkgui::A11yRole::image;
        item.bounds = cell_;
        item.title = "Shader " + file_;
        item.value = program_ != 0 ? std::string("drawn") : (why_.empty() ? std::string("not built yet") : why_);
        item.readOnly = true;
        out.push_back(std::move(item));
    }

    void GuiShaderItem::specLine(char* out, std::size_t n) const
    {
        if (!shown_)
        {
            if (n > 0)
                out[0] = '\0';
            return;
        }
        std::snprintf(out, n, "shader %s   %s", file_.c_str(), program_ != 0 ? "drawn" : (why_.empty() ? "not built" : why_.c_str()));
    }

    void GuiEnvelopeItem::accessibility(std::vector<funkgui::A11yItem>& out) const
    {
        if (!shown())
            return;
        funkgui::A11yItem item;
        item.id = id_;
        item.role = funkgui::A11yRole::staticText;
        item.bounds = cell_;
        item.title = label_;
        item.value = adsr_[0]->label() + " " + adsr_[0]->text() + ", " + adsr_[1]->label() + " " + adsr_[1]->text() + ", "
                   + adsr_[2]->label() + " " + adsr_[2]->text() + ", " + adsr_[3]->label() + " " + adsr_[3]->text();
        item.readOnly = true;
        out.push_back(std::move(item));
    }

    void GuiEnvelopeItem::specLine(char* out, std::size_t n) const
    {
        if (!shown())
        {
            if (n > 0)
                out[0] = '\0';
            return;
        }
        std::snprintf(out, n, "%s   A %s   D %s   S %s   R %s", label_.c_str(), adsr_[0]->text().c_str(), adsr_[1]->text().c_str(),
                      adsr_[2]->text().c_str(), adsr_[3]->text().c_str());
    }

    // ---- GuiTabsItem -----------------------------------------------------------------------------------------------------
    void GuiTabsItem::place(int node, const Rect& rect, std::vector<Rect> tabs, std::vector<std::string> titles, int page,
                            const Rect& area, bool shown, const std::string& color, const GuiStyle& style)
    {
        style_ = style;
        node_ = node;
        rect_ = rect;
        tabs_ = std::move(tabs);
        titles_.clear();
        for (const std::string& t : titles)
            titles_.push_back(upper(asciiText(t)));
        page_ = page;
        area_ = area;
        shown_ = shown;
        color_ = color;
    }

    int GuiTabsItem::tabAt(funkgui::Point p) const
    {
        for (std::size_t k = 0; k < tabs_.size(); ++k)
            if (tabs_[k].contains(p))
                return static_cast<int>(k);
        return -1;
    }

    void GuiTabsItem::tick(float, bool hovered, bool, funkgui::Point p) { hover_ = hovered ? tabAt(p) : -1; }

    void GuiTabsItem::pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController&, funkgui::HostServices&)
    {
        const int k = tabAt({ e.x, e.y });
        if (k >= 0 && choose_)
            choose_(node_, k);
    }

    bool GuiTabsItem::key(const funkgui::KeyEvent& e, funkgui::GestureController&)
    {
        const int n = static_cast<int>(tabs_.size());
        if (n == 0 || !choose_)
            return false;
        if (e.key == funkgui::Key::left || e.key == funkgui::Key::right)
        {
            choose_(node_, (page_ + (e.key == funkgui::Key::right ? 1 : n - 1)) % n);
            return true;
        }
        return false;
    }

    void GuiTabsItem::draw(funkgui::Canvas& c, const funkgui::Theme& th, bool focusRing) const
    {
        if (!shown_)
            return;
        const Col col = colorOf(color_, th, th.accent);
        const bool tinted = !color_.empty();
        const Col edge = !style_.border.empty() && style_.border != "none" ? colorOf(style_.border, th, th.ink32)
                                                                           : (tinted ? funkgui::premix(th.ground, col, 0.65f) : th.ink32);
        const Col titleOn = !style_.title.empty() ? colorOf(style_.title, th, th.ink100) : (tinted ? col : th.ink100);
        const std::string& look = style_.look;
        const Rect chosen = page_ >= 0 && static_cast<std::size_t>(page_) < tabs_.size() ? tabs_[static_cast<std::size_t>(page_)] : Rect{};
        const auto title = [&](std::size_t k) -> const std::string& {
            static const std::string none;
            return k < titles_.size() ? titles_[k] : none;
        };
        if (look == "bar")
        {
            // A header bar, as a synthesizer's pages are chosen: the titles large, the chosen one bright and underlined.
            const float base = rect_.y + kGuiBarH;
            if (!style_.fill.empty() && style_.fill != "none")
                c.rrect(rect_.x, rect_.y, rect_.w, kGuiBarH, style_.radius > 0.0f ? style_.radius : 0.0f, colorOf(style_.fill, th, th.ground));
            c.hairlineH(rect_.x, base, rect_.w, edge);
            for (std::size_t k = 0; k < tabs_.size(); ++k)
            {
                const Rect& t = tabs_[k];
                const bool on = static_cast<int>(k) == page_;
                const Col ink = on ? titleOn : (static_cast<int>(k) == hover_ ? th.ink70 : th.ink52);
                c.text(fitText(c, title(k), kGuiBarText, t.w - 8.0f).c_str(), t.centreX(), c.capCentreTop(t.centreY(), kGuiBarText),
                       kGuiBarText, ink, funkgui::Align::centre);
                if (on)
                    c.rrect(t.x + 10.0f, base - 3.0f, t.w - 20.0f, 3.0f, 1.5f, tinted ? col : th.accent);
            }
        }
        else if (look == "pills")
        {
            for (std::size_t k = 0; k < tabs_.size(); ++k)
            {
                const Rect& t = tabs_[k];
                const bool on = static_cast<int>(k) == page_;
                const float rr = style_.radius >= 0.0f ? style_.radius : 0.5f * t.h;
                const Col lit = tinted ? col : th.accent;
                c.rrect(t.x, t.y, t.w, t.h, rr, on ? funkgui::premix(th.ground, lit, 0.35f)
                                                   : funkgui::mix(th.ground, th.ink16, static_cast<int>(k) == hover_ ? 0.6f : 0.3f),
                        1.0f, on ? lit : edge);
                c.text(fitText(c, title(k), kGuiControlLabel, t.w - 10.0f).c_str(), t.centreX(), c.capCentreTop(t.centreY(), kGuiControlLabel),
                       kGuiControlLabel, on ? titleOn : th.ink52, funkgui::Align::centre);
            }
        }
        else if (look == "side")
        {
            // A list at the left, the chosen page's tab lit, a bar at its edge.
            for (std::size_t k = 0; k < tabs_.size(); ++k)
            {
                const Rect& t = tabs_[k];
                const bool on = static_cast<int>(k) == page_;
                if (on || static_cast<int>(k) == hover_)
                    c.rrect(t.x, t.y, t.w, t.h, style_.radius >= 0.0f ? style_.radius : 3.0f,
                            on ? funkgui::mix(th.ground, th.ink16, 0.6f) : funkgui::mix(th.ground, th.ink16, 0.3f));
                if (on)
                    c.rrect(t.x, t.y + 4.0f, 3.0f, t.h - 8.0f, 1.5f, tinted ? col : th.accent);
                c.text(fitText(c, title(k), kGuiControlLabel, t.w - 20.0f).c_str(), t.x + 12.0f, c.capCentreTop(t.centreY(), kGuiControlLabel),
                       kGuiControlLabel, on ? titleOn : th.ink52);
            }
            if (!tabs_.empty())
                c.hairlineV(tabs_.front().right() + 5.0f, rect_.y, rect_.h, edge);
        }
        else
        {
            // Folder tabs over the page's frame, its top edge the strip's base, open under the chosen tab.
            const float base = rect_.y + kGuiTabsH;
            if (chosen.w > 0.0f)
            {
                c.hairlineH(rect_.x, base, std::max(0.0f, chosen.x - rect_.x), edge);
                c.hairlineH(chosen.right(), base, std::max(0.0f, rect_.right() - chosen.right()), edge);
            }
            else
                c.hairlineH(rect_.x, base, rect_.w, edge);
            c.hairlineV(rect_.x, base, rect_.h - kGuiTabsH, edge);
            c.hairlineV(rect_.right() - 1.0f, base, rect_.h - kGuiTabsH, edge);
            c.hairlineH(rect_.x, rect_.bottom() - 1.0f, rect_.w, edge);
            for (std::size_t k = 0; k < tabs_.size(); ++k)
            {
                const Rect& t = tabs_[k];
                const bool on = static_cast<int>(k) == page_;
                if (on)
                {
                    c.rrect4(t.x, t.y, t.w, t.h + 1.0f, 3.0f, 3.0f, 0.0f, 0.0f, tinted ? funkgui::premix(th.ground, col, 0.14f)
                                                                                        : funkgui::mix(th.ground, th.ink16, 0.35f));
                    c.hairlineH(t.x + 2.0f, t.y, t.w - 4.0f, tinted ? col : th.ink52);
                    c.hairlineV(t.x, t.y + 2.0f, t.h - 2.0f, edge);
                    c.hairlineV(t.right() - 1.0f, t.y + 2.0f, t.h - 2.0f, edge);
                }
                const Col ink = on ? titleOn : (static_cast<int>(k) == hover_ ? th.ink70 : th.ink52);
                c.text(fitText(c, title(k), kGuiControlLabel, t.w - 8.0f).c_str(), t.centreX(), c.capCentreTop(t.centreY(), kGuiControlLabel),
                       kGuiControlLabel, ink, funkgui::Align::centre);
            }
        }
        if (focusRing && chosen.w > 0.0f)
            ring(c, th, chosen);
    }

    void GuiTabsItem::accessibility(std::vector<funkgui::A11yItem>& out) const
    {
        if (!shown_)
            return;
        funkgui::A11yItem group;
        group.id = id_;
        group.role = funkgui::A11yRole::radioGroup;
        group.bounds = { rect_.x, rect_.y, rect_.w, kGuiTabsH };
        group.title = "Pages";
        group.value = page_ >= 0 && static_cast<std::size_t>(page_) < titles_.size() ? titles_[static_cast<std::size_t>(page_)] : "";
        out.push_back(group);
        for (std::size_t k = 0; k < tabs_.size() && k < 15; ++k)
        {
            funkgui::A11yItem tab;
            tab.id = id_ + kTabIdStep * static_cast<uint32_t>(k + 1);
            tab.parent = id_;
            tab.role = funkgui::A11yRole::radioButton;
            tab.bounds = tabs_[k];
            tab.title = k < titles_.size() ? titles_[k] : "";
            tab.checkable = true;
            tab.checked = static_cast<int>(k) == page_;
            out.push_back(std::move(tab));
        }
    }

    bool GuiTabsItem::a11yAction(uint32_t id, funkgui::A11yAction a, double, funkgui::GestureController&)
    {
        if (!shown_ || !choose_)
            return false;
        for (std::size_t k = 0; k < tabs_.size() && k < 15; ++k)
            if (id == id_ + kTabIdStep * static_cast<uint32_t>(k + 1))
            {
                if (a == funkgui::A11yAction::press || a == funkgui::A11yAction::toggle)
                    choose_(node_, static_cast<int>(k));
                return true;
            }
        return id == id_;
    }

    void GuiTabsItem::specLine(char* out, std::size_t n) const
    {
        const std::string page = page_ >= 0 && static_cast<std::size_t>(page_) < titles_.size() ? titles_[static_cast<std::size_t>(page_)] : "";
        std::snprintf(out, n, "PAGES   %s   A CLICK ON A TAB SHOWS ITS PAGE   LEFT AND RIGHT CHANGE IT", page.c_str());
    }

    funkgui::Point GuiTabsItem::centre() const
    {
        if (page_ >= 0 && static_cast<std::size_t>(page_) < tabs_.size())
            return { tabs_[static_cast<std::size_t>(page_)].centreX(), tabs_[static_cast<std::size_t>(page_)].centreY() };
        return { rect_.centreX(), rect_.y + 0.5f * kGuiTabsH };
    }

    // ---- the panel's decorations -----------------------------------------------------------------------------------------
    funkgui::Theme guiTheme(const funkgui::Theme& th, const GuiTree& gui)
    {
        funkgui::Theme t = th;
        if (!gui.background.empty())
        {
            Col bg;
            if (!hexColor(gui.background, bg))
                bg = looks::panelColor(gui.background, th, th.ground);
            t = darkCol(bg) ? funkgui::Theme::graphite() : funkgui::Theme::paper();
            t.ground = bg;
            // The inks of the ladder that sit too near the ground move away from it a little.
            t.ink16 = funkgui::mix(bg, t.ink100, 0.16f);
            t.ink32 = funkgui::mix(bg, t.ink100, 0.32f);
        }
        if (!gui.accent.empty())
        {
            t.accent = colorOf(gui.accent, t, t.accent);
            t.accentDim = funkgui::premix(t.ground, t.accent, 0.45f);
        }
        return t;
    }

    namespace
    {
        // A container's box: its shadow, its fill and its border, as its style says (or the defaults given).
        void decor(funkgui::Canvas& c, const funkgui::Theme& th, const GuiStyle& st, const Rect& r, float radius, Col fill,
                   bool filled, Col border, float borderW, bool shadow)
        {
            const float rr = st.radius >= 0.0f ? st.radius : radius;
            const bool sh = st.shadow >= 0 ? st.shadow == 1 : shadow;
            if (st.fill == "none")
                filled = false;
            else if (!st.fill.empty())
            {
                fill = colorOf(st.fill, th, fill);
                filled = true;
            }
            float bw = st.borderWidth >= 0.0f ? st.borderWidth : borderW;
            if (st.border == "none")
                bw = 0.0f;
            else if (!st.border.empty())
            {
                border = colorOf(st.border, th, border);
                if (bw <= 0.0f)
                    bw = 1.0f;
            }
            if (sh)
                c.rrect(r.x, r.y + 3.0f, r.w, r.h, rr, Col{ 0, 0, 0, 90 }, 0.0f, {}, 10.0f);
            if (filled || bw > 0.0f)
                c.rrect(r.x, r.y, r.w, r.h, rr, filled ? fill : th.ground.withAlpha(0.0f), bw, border);
        }

        // A screw's head, for a plate's corners: a disc and its slot.
        void screw(funkgui::Canvas& c, const funkgui::Theme& th, float x, float y)
        {
            c.disc(x, y, 3.2f, funkgui::mix(th.ink52, th.ink100, 0.3f), 1.0f, funkgui::mix(th.ink32, Col{ 0, 0, 0 }, 0.3f));
            c.segment(x - 2.0f, y - 1.2f, x + 2.0f, y + 1.2f, 1.0f, funkgui::mix(th.ink16, Col{ 0, 0, 0 }, 0.5f));
        }

        funkgui::TextStyle titleStyle(const GuiStyle& st)
        {
            if (st.titleSize < 0)
                return kGuiSectionTitle;
            const funkgui::TextStyle t = guiTextStyle(static_cast<GuiText>(st.titleSize));
            return { t.px, std::max(t.tracking, 2.0f), 0.10f, false };
        }
    }

    void drawGuiPanel(funkgui::Canvas& c, const funkgui::Theme& th, const PanelSpec& spec, const GuiLayout& layout)
    {
        const std::vector<GuiNode>& nodes = spec.gui.nodes;
        if (nodes.empty() || layout.rects.size() != nodes.size())
            return;
        const Rect& area = layout.area;
        if (!spec.gui.background.empty() || !spec.gui.gradient.empty())
        {
            if (!spec.gui.gradient.empty())
            {
                // The ground from background= at the top to gradient= at the bottom, in bands too fine to see.
                const Col top = th.ground, bottom = colorOf(spec.gui.gradient, th, th.ground);
                constexpr int kBands = 48;
                for (int k = 0; k < kBands; ++k)
                {
                    const float y0 = area.y + area.h * static_cast<float>(k) / kBands;
                    const float y1 = area.y + area.h * static_cast<float>(k + 1) / kBands;
                    c.rrect(area.x, std::floor(y0), area.w, std::ceil(y1) - std::floor(y0) + 1.0f, 0.0f,
                            funkgui::mix(top, bottom, (static_cast<float>(k) + 0.5f) / kBands));
                }
            }
            else
                c.rrect(area.x, area.y, area.w, area.h, 0.0f, th.ground);
        }
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            if (!layout.visible[i])
                continue;
            const GuiNode& n = nodes[i];
            const Rect& r = layout.rects[i];
            if (r.right() < area.x || r.x > area.right() || r.bottom() < area.y || r.y > area.bottom())
                continue;
            const GuiStyle& st = n.style;
            const bool tinted = !n.color.empty();
            const Col col = colorOf(n.color, th, th.accent);
            switch (n.kind)
            {
                case GuiKind::section:
                {
                    const std::string& look = st.look.empty() ? std::string("frame") : st.look;
                    const std::string title = upper(asciiText(n.text));
                    const funkgui::TextStyle ts = titleStyle(st);
                    const Col titleCol = !st.title.empty() ? colorOf(st.title, th, th.ink70) : (tinted ? col : th.ink70);
                    const int align = st.titleAlign >= 0 ? st.titleAlign : (look == "frame" ? 1 : 0);
                    if (look == "frame")
                    {
                        // A frame, its top edge a little under the section's top so that the title, cut into it, is inside.
                        const Col edge = tinted ? funkgui::premix(th.ground, col, 0.65f) : th.ink32;
                        const Col fill = tinted ? funkgui::premix(funkgui::mix(th.ground, th.ink16, 0.22f), col, 0.08f)
                                                : funkgui::mix(th.ground, th.ink16, 0.22f);
                        const Rect f{ r.x, r.y + kGuiSectionEdge, r.w, std::max(0.0f, r.h - kGuiSectionEdge) };
                        const bool rounded = st.radius > 0.0f;
                        if (rounded || !st.fill.empty() || !st.border.empty() || st.shadow == 1 || st.borderWidth >= 0.0f)
                            decor(c, th, st, f, 0.0f, fill, true, edge, 1.0f, false);
                        else
                        {
                            c.rrect(f.x, f.y, f.w, f.h, 0.0f, fill);
                            c.hairlineV(f.x, f.y, f.h, edge);
                            c.hairlineV(f.right() - 1.0f, f.y, f.h, edge);
                            c.hairlineH(f.x, f.bottom() - 1.0f, f.w, edge);
                            if (title.empty())
                                c.hairlineH(f.x, f.y, f.w, edge);
                        }
                        if (!title.empty())
                        {
                            const float tw = std::min(c.textWidth(title.c_str(), ts), r.w - 24.0f);
                            const float tx = align == 0 ? r.x + 12.0f + 0.5f * tw : (align == 2 ? r.right() - 12.0f - 0.5f * tw : r.centreX());
                            const float l = tx - 0.5f * tw - 7.0f, rr = tx + 0.5f * tw + 7.0f;
                            if (rounded || !st.border.empty() || !st.fill.empty())
                            {
                                // The rounded frame's edge under the title, covered by a patch of the ground.
                                c.rrect(l, f.y - 1.0f, rr - l, 3.0f, 0.0f, th.ground);
                            }
                            else
                            {
                                c.hairlineH(f.x, f.y, std::max(0.0f, l - f.x), edge);
                                c.hairlineH(rr, f.y, std::max(0.0f, f.right() - rr), edge);
                            }
                            c.text(fitText(c, title, ts, r.w - 24.0f).c_str(), tx, c.capCentreTop(f.y, ts), ts, titleCol, funkgui::Align::centre);
                        }
                    }
                    else
                    {
                        // card: a raised, rounded panel; inset: a recessed one; plate: a metal plate screwed on. The
                        // title inside, at the top.
                        Col fill = funkgui::mix(th.ground, th.ink16, 0.45f);
                        Col border = th.ink16;
                        float radius = 8.0f;
                        bool shadow = true;
                        if (look == "inset")
                        {
                            fill = funkgui::mix(th.ground, Col{ 0, 0, 0 }, looks::dark(th) ? 0.35f : 0.08f);
                            border = funkgui::mix(th.ground, Col{ 0, 0, 0 }, 0.5f);
                            radius = 6.0f;
                            shadow = false;
                        }
                        else if (look == "plate")
                        {
                            fill = funkgui::mix(th.ground, th.ink70, looks::dark(th) ? 0.28f : 0.18f);
                            border = th.ink32;
                            radius = 4.0f;
                        }
                        if (tinted)
                            fill = funkgui::premix(fill, col, 0.12f);
                        decor(c, th, st, r, radius, fill, true, border, 1.0f, shadow);
                        if (look == "inset")                 // its lip: a light line under its bottom edge
                            c.hairlineH(r.x + radius, r.bottom(), r.w - 2.0f * radius, funkgui::mix(th.ground, th.ink100, 0.12f));
                        if (look == "plate")
                        {
                            screw(c, th, r.x + 8.0f, r.y + 8.0f);
                            screw(c, th, r.right() - 8.0f, r.y + 8.0f);
                            screw(c, th, r.x + 8.0f, r.bottom() - 8.0f);
                            screw(c, th, r.right() - 8.0f, r.bottom() - 8.0f);
                        }
                        if (!title.empty())
                        {
                            const float pad = look == "plate" ? 18.0f : 12.0f;
                            const float tx = align == 0 ? r.x + pad : (align == 2 ? r.right() - pad : r.centreX());
                            const funkgui::Align how = align == 0 ? funkgui::Align::left : (align == 2 ? funkgui::Align::right : funkgui::Align::centre);
                            c.text(fitText(c, title, ts, r.w - 2.0f * pad).c_str(), tx, c.capCentreTop(r.y + 13.0f, ts), ts,
                                   !st.title.empty() ? titleCol : (look == "plate" ? funkgui::mix(th.ink100, th.ground, 0.15f) : titleCol), how);
                        }
                    }
                    break;
                }
                case GuiKind::box:
                {
                    const std::string& look = st.look;
                    Col fill = tinted ? funkgui::premix(th.ground, col, 0.16f) : funkgui::mix(th.ground, th.ink16, 0.40f);
                    Col border = tinted ? funkgui::premix(th.ground, col, 0.45f) : th.ink16;
                    float radius = 4.0f;
                    bool shadow = false;
                    if (look == "card")
                    {
                        radius = 8.0f;
                        shadow = true;
                    }
                    else if (look == "inset")
                    {
                        fill = funkgui::mix(th.ground, Col{ 0, 0, 0 }, looks::dark(th) ? 0.35f : 0.08f);
                        border = funkgui::mix(th.ground, Col{ 0, 0, 0 }, 0.5f);
                        radius = 6.0f;
                    }
                    else if (look == "plate")
                    {
                        fill = funkgui::mix(th.ground, th.ink70, looks::dark(th) ? 0.28f : 0.18f);
                        border = th.ink32;
                    }
                    decor(c, th, st, r, radius, fill, true, border, 1.0f, shadow);
                    if (look == "plate")
                    {
                        screw(c, th, r.x + 8.0f, r.y + 8.0f);
                        screw(c, th, r.right() - 8.0f, r.y + 8.0f);
                        screw(c, th, r.x + 8.0f, r.bottom() - 8.0f);
                        screw(c, th, r.right() - 8.0f, r.bottom() - 8.0f);
                    }
                    break;
                }
                case GuiKind::row: case GuiKind::column: case GuiKind::grid: case GuiKind::page:
                    // A plain container draws only what its style asks of it.
                    if (!st.fill.empty() || !st.border.empty() || st.shadow == 1 || st.borderWidth > 0.0f)
                        decor(c, th, st, r, 0.0f, th.ground, false, th.ink32, 0.0f, false);
                    break;
                case GuiKind::label:
                {
                    const funkgui::TextStyle ts = guiLabelStyle(n);
                    const std::string text = upper(asciiText(n.text));
                    int align = n.textAlign;
                    if (align < 0)
                    {
                        const GuiAlign a = n.parent >= 0 ? nodes[static_cast<std::size_t>(n.parent)].align : GuiAlign::start;
                        const bool across = n.parent >= 0 && nodes[static_cast<std::size_t>(n.parent)].vertical;
                        align = across && a == GuiAlign::center ? 1 : (across && a == GuiAlign::end ? 2 : 0);
                    }
                    const float x = align == 1 ? r.centreX() : (align == 2 ? r.right() : r.x);
                    const funkgui::Align how = align == 1 ? funkgui::Align::centre : (align == 2 ? funkgui::Align::right : funkgui::Align::left);
                    const bool big = n.textPx >= 15.0f || n.textSize == GuiText::title || n.textSize == GuiText::large;
                    const Col ink = tinted ? col : (big ? th.ink100 : th.ink70);
                    c.text(fitText(c, text, ts, std::max(r.w, c.textWidth(text.c_str(), ts))).c_str(), x, c.capCentreTop(r.centreY(), ts),
                           ts, ink, how);
                    break;
                }
                case GuiKind::divider:
                {
                    const Col ink = tinted ? col : th.ink16;
                    const float t = st.thickness > 0.0f ? st.thickness : 1.0f;
                    if (r.w <= r.h)
                        c.rrect(std::round(r.centreX() - 0.5f * t), r.y, t, r.h, 0.0f, ink);
                    else
                        c.rrect(r.x, std::round(r.centreY() - 0.5f * t), r.w, t, 0.0f, ink);
                    break;
                }
                case GuiKind::tabs: case GuiKind::spacer: case GuiKind::knob: case GuiKind::slider: case GuiKind::toggle:
                case GuiKind::buttons: case GuiKind::menu: case GuiKind::xy: case GuiKind::envelope: case GuiKind::display:
                case GuiKind::keyboard: case GuiKind::shader:
                    break;
            }
        }
    }

    void drawSpectrum(funkgui::Canvas& c, const funkgui::Theme& th, const Rect& cell, const char* label, const float* db,
                      int n, Col color, bool tinted)
    {
        const Rect s{ cell.x + 1.0f, cell.y + 15.0f, cell.w - 2.0f, std::max(24.0f, cell.h - 28.0f) };   // as a scope's
        c.text(label, cell.centreX(), cell.y, type::kCaption, th.ink52, funkgui::Align::centre);
        const Col screen = Col{ 0x0E, 0x11, 0x12 };
        c.rrect(s.x, s.y, s.w, s.h, 3.0f, screen, 1.0f, th.ink32);
        const Col grid = Col{ 0x26, 0x2C, 0x2E };
        // Decades (100 Hz, 1 kHz, 10 kHz) across 20 Hz to 20 kHz, and every 24 dB down.
        for (const float hz : { 100.0f, 1000.0f, 10000.0f })
            c.hairlineV(s.x + s.w * std::log10(hz / 20.0f) / 3.0f, s.y + 1.0f, s.h - 2.0f, grid);
        for (int k = 1; k < 4; ++k)
            c.hairlineH(s.x + 1.0f, s.y + s.h * static_cast<float>(k) / 3.5f, s.w - 2.0f, grid);
        if (db == nullptr || n < 2)
            return;
        std::vector<float> xs(static_cast<std::size_t>(n)), ys(static_cast<std::size_t>(n));
        for (int k = 0; k < n; ++k)
        {
            const float t = std::clamp((db[k] + 84.0f) / 84.0f, 0.0f, 1.0f);
            xs[static_cast<std::size_t>(k)] = s.x + 1.0f + (s.w - 2.0f) * static_cast<float>(k) / static_cast<float>(n - 1);
            ys[static_cast<std::size_t>(k)] = s.bottom() - 1.0f - (s.h - 3.0f) * t;
        }
        const Col trace = tinted ? color : Col{ 0x7F, 0xD4, 0xE8 };
        c.areaStrip(xs.data(), n, ys.data(), nullptr, s.bottom() - 1.0f, funkgui::premix(screen, trace, 0.25f));
        c.polyline(xs.data(), ys.data(), n, 1.5f, trace);
    }
}
