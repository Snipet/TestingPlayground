// Source/editor/GuiItems.cpp: a .gui file's controls, pads, envelopes, tabs and decorations (GuiItems.h).
#include "editor/GuiItems.h"

#include "editor/ChatItems.h"
#include "editor/Looks.h"
#include "fminus/GuiSpec.h"

#include <funkgui/core/TypeScale.h>

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
        cell_ = cell;
        area_ = area;
        shown_ = shown;
        color_ = color;
        if (!shown_)
            dragging_ = false;
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
        const float t = value_.value01();
        const std::string& label = value_.label();
        const std::string text = value_.text();
        const bool hot = hovered_ || dragging_;
        const Col labelInk = hot ? th.ink70 : th.ink52;
        switch (kind_)
        {
            case GuiKind::knob:
            {
                const bool small = size_ == GuiSize::small, large = size_ == GuiSize::large;
                const float rad = small ? 11.0f : (large ? 24.0f : 15.0f);
                const float cy = r.y + (small ? 20.0f : (large ? 50.0f : 34.0f));
                const funkgui::Point centre{ r.centreX(), cy };
                const float a = looks::knobAngle(t);
                if (!small)
                    c.text(label.c_str(), r.centreX(), r.y, type::kCaption, labelInk, funkgui::Align::centre);
                if (vintage_)
                {
                    const float skirt = rad + 1.0f, cap = rad * 0.72f;
                    const Col print = hot ? th.ink52 : th.ink32;
                    for (int i = 0; i <= 10; ++i)
                    {
                        const bool major = i == 0 || i == 5 || i == 10;
                        radial(c, centre, skirt + 2.0f, skirt + (major ? 6.0f : 4.0f), looks::knobAngle(static_cast<float>(i) / 10.0f),
                               1.0f, print);
                    }
                    const Col black = capBlack(th);
                    c.disc(centre.x, cy, skirt, funkgui::mix(black, th.ink16, 0.55f), 1.0f, th.ink32);
                    radial(c, centre, cap + 1.0f, skirt - 0.5f, a, 2.0f, hot ? th.accent : funkgui::mix(th.ink70, th.ink100, 0.5f));
                    const Col capCol = tinted ? funkgui::mix(black, col, 0.85f) : black;
                    c.disc(centre.x, cy, cap, capCol, 1.0f, funkgui::mix(capCol, Col{ 0xFF, 0xFF, 0xFF }, 0.18f));
                    radial(c, centre, rad * 0.2f, cap - 1.5f, a, large ? 3.0f : 2.0f, Col{ 0xF2, 0xEF, 0xE8 });
                }
                else
                {
                    const float a0 = looks::knobAngle(0.0f), a1 = looks::knobAngle(1.0f);
                    const float w = large ? 3.0f : 2.0f;
                    c.arc(centre, rad, a0, a1, w, th.ink16);
                    if (a > a0 + 0.01f)
                        c.arc(centre, rad, a0, a, w, tinted ? col : (hot ? th.accent : th.ink70));
                    radial(c, centre, rad - (large ? 9.0f : 6.0f), rad + 2.0f, a, w, hot ? th.accent : th.ink100);
                    if (large)                       // a cap under the pointer
                        c.disc(centre.x, cy, rad - 11.0f, funkgui::mix(th.ground, th.ink16, 0.6f), 1.0f, th.ink32);
                }
                if (small)
                    c.text(label.c_str(), r.centreX(), r.y + 38.0f, type::kMicro, labelInk, funkgui::Align::centre);
                else
                    c.text(text.c_str(), r.centreX(), cy + rad + (large ? 14.0f : 11.0f), type::kMicro,
                           valueInk(th, hot, value_.atDefault()), funkgui::Align::centre);
                break;
            }
            case GuiKind::slider:
            {
                const Rect tr = track();
                const Col capCol = tinted ? col : (looks::dark(th) ? th.ink70 : th.ink52);
                if (vertical_)
                {
                    c.text(label.c_str(), r.centreX(), r.y, type::kCaption, labelInk, funkgui::Align::centre);
                    for (int i = 0; i <= 4; ++i)
                    {
                        const float y = tr.y + tr.h * static_cast<float>(i) / 4.0f;
                        c.hairlineH(tr.centreX() - 13.0f, y, 5.0f, th.ink32);
                        c.hairlineH(tr.centreX() + 8.0f, y, 5.0f, th.ink32);
                    }
                    c.rrect(tr.x, tr.y, tr.w, tr.h, 2.0f, funkgui::mix(capBlack(th), th.ink16, 0.5f), 1.0f, th.ink32);
                    const float y = tr.bottom() - tr.h * t;
                    c.rrect(tr.centreX() - 10.0f, y - 5.0f, 20.0f, 10.0f, 2.0f, hot ? funkgui::mix(capCol, th.accent, 0.5f) : capCol,
                            1.0f, funkgui::mix(capCol, capBlack(th), 0.4f));
                    c.hairlineH(tr.centreX() - 8.0f, y, 16.0f, capBlack(th));
                    c.text(text.c_str(), r.centreX(), r.bottom() - 12.0f, type::kMicro, valueInk(th, hot, value_.atDefault()),
                           funkgui::Align::centre);
                }
                else
                {
                    c.text(label.c_str(), r.x + 8.0f, r.y + 2.0f, type::kCaption, labelInk);
                    c.text(text.c_str(), r.right() - 8.0f, r.y + 2.0f, type::kMicro, valueInk(th, hot, value_.atDefault()),
                           funkgui::Align::right);
                    for (int i = 0; i <= 4; ++i)
                        c.hairlineV(tr.x + tr.w * static_cast<float>(i) / 4.0f, tr.y - 9.0f, 5.0f, th.ink32);
                    c.rrect(tr.x, tr.y, tr.w, tr.h, 2.0f, funkgui::mix(capBlack(th), th.ink16, 0.5f), 1.0f, th.ink32);
                    const float x = tr.x + tr.w * t;
                    c.rrect(x - 5.0f, tr.centreY() - 9.0f, 10.0f, 18.0f, 2.0f, hot ? funkgui::mix(capCol, th.accent, 0.5f) : capCol,
                            1.0f, funkgui::mix(capCol, capBlack(th), 0.4f));
                    c.hairlineV(x, tr.centreY() - 7.0f, 14.0f, capBlack(th));
                }
                break;
            }
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
            case GuiKind::xy: case GuiKind::envelope: case GuiKind::display:
                break;
        }
        if (focusRing)
            ring(c, th, r);
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
            case GuiKind::xy: case GuiKind::envelope: case GuiKind::display:
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
            case GuiKind::divider: case GuiKind::label: case GuiKind::xy: case GuiKind::envelope: case GuiKind::display:
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
                            const Rect& area, bool shown, const std::string& color)
    {
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
        const Col edge = tinted ? funkgui::premix(th.ground, col, 0.65f) : th.ink32;
        const float base = rect_.y + kGuiTabsH;
        // The page's frame, its top edge the strip's base, open under the chosen tab.
        const Rect chosen = page_ >= 0 && static_cast<std::size_t>(page_) < tabs_.size() ? tabs_[static_cast<std::size_t>(page_)] : Rect{};
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
            const Col ink = on ? (tinted ? col : th.ink100) : (static_cast<int>(k) == hover_ ? th.ink70 : th.ink52);
            const std::string& title = k < titles_.size() ? titles_[k] : std::string();
            c.text(fitText(c, title, kGuiControlLabel, t.w - 8.0f).c_str(), t.centreX(), c.capCentreTop(t.centreY(), kGuiControlLabel),
                   kGuiControlLabel, ink, funkgui::Align::centre);
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

    void drawGuiPanel(funkgui::Canvas& c, const funkgui::Theme& th, const PanelSpec& spec, const GuiLayout& layout)
    {
        const std::vector<GuiNode>& nodes = spec.gui.nodes;
        if (nodes.empty() || layout.rects.size() != nodes.size())
            return;
        if (!spec.gui.background.empty())
            c.rrect(layout.area.x, layout.area.y, layout.area.w, layout.area.h, 0.0f, th.ground);
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            if (!layout.visible[i])
                continue;
            const GuiNode& n = nodes[i];
            const Rect& r = layout.rects[i];
            if (r.right() < layout.area.x || r.x > layout.area.right() || r.bottom() < layout.area.y || r.y > layout.area.bottom())
                continue;
            const bool tinted = !n.color.empty();
            const Col col = colorOf(n.color, th, th.accent);
            switch (n.kind)
            {
                case GuiKind::section:
                {
                    const Col edge = tinted ? funkgui::premix(th.ground, col, 0.65f) : th.ink32;
                    const Col fill = tinted ? funkgui::premix(funkgui::mix(th.ground, th.ink16, 0.22f), col, 0.08f)
                                            : funkgui::mix(th.ground, th.ink16, 0.22f);
                    // The frame's top edge sits a little under the section's top, so that its title, centred on
                    // it, is inside the section (and never cut by the panel's area).
                    const float top = r.y + kGuiSectionEdge, h = std::max(0.0f, r.h - kGuiSectionEdge);
                    c.rrect(r.x, top, r.w, h, 0.0f, fill);
                    const std::string title = upper(asciiText(n.text));
                    if (title.empty())
                        c.hairlineH(r.x, top, r.w, edge);
                    else
                    {
                        const float tw = std::min(c.textWidth(title.c_str(), kGuiSectionTitle), r.w - 16.0f);
                        const float l = r.centreX() - 0.5f * tw - 7.0f, rr = r.centreX() + 0.5f * tw + 7.0f;
                        c.hairlineH(r.x, top, std::max(0.0f, l - r.x), edge);
                        c.hairlineH(rr, top, std::max(0.0f, r.right() - rr), edge);
                        c.text(fitText(c, title, kGuiSectionTitle, r.w - 16.0f).c_str(), r.centreX(),
                               c.capCentreTop(top, kGuiSectionTitle), kGuiSectionTitle, tinted ? col : th.ink70, funkgui::Align::centre);
                    }
                    c.hairlineV(r.x, top, h, edge);
                    c.hairlineV(r.right() - 1.0f, top, h, edge);
                    c.hairlineH(r.x, r.bottom() - 1.0f, r.w, edge);
                    break;
                }
                case GuiKind::box:
                    c.rrect(r.x, r.y, r.w, r.h, 4.0f,
                            tinted ? funkgui::premix(th.ground, col, 0.16f) : funkgui::mix(th.ground, th.ink16, 0.40f), 1.0f,
                            tinted ? funkgui::premix(th.ground, col, 0.45f) : th.ink16);
                    break;
                case GuiKind::label:
                {
                    const funkgui::TextStyle st = guiTextStyle(n.textSize);
                    const std::string text = upper(asciiText(n.text));
                    const GuiAlign a = n.parent >= 0 ? nodes[static_cast<std::size_t>(n.parent)].align : GuiAlign::start;
                    const bool across = n.parent >= 0 && nodes[static_cast<std::size_t>(n.parent)].vertical;
                    float x = r.x;
                    funkgui::Align how = funkgui::Align::left;
                    if (across && a == GuiAlign::center)
                    {
                        x = r.centreX();
                        how = funkgui::Align::centre;
                    }
                    else if (across && a == GuiAlign::end)
                    {
                        x = r.right();
                        how = funkgui::Align::right;
                    }
                    const Col ink = tinted ? col : (n.textSize == GuiText::title || n.textSize == GuiText::large ? th.ink100 : th.ink70);
                    c.text(fitText(c, text, st, std::max(r.w, c.textWidth(text.c_str(), st))).c_str(), x, c.capCentreTop(r.centreY(), st),
                           st, ink, how);
                    break;
                }
                case GuiKind::divider:
                {
                    const Col ink = tinted ? col : th.ink16;
                    if (r.w <= r.h)
                        c.hairlineV(r.centreX(), r.y, r.h, ink);
                    else
                        c.hairlineH(r.x, r.centreY(), r.w, ink);
                    break;
                }
                case GuiKind::row: case GuiKind::column: case GuiKind::grid: case GuiKind::tabs: case GuiKind::page:
                case GuiKind::spacer: case GuiKind::knob: case GuiKind::slider: case GuiKind::toggle: case GuiKind::buttons:
                case GuiKind::menu: case GuiKind::xy: case GuiKind::envelope: case GuiKind::display:
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
