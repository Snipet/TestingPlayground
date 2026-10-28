// Source/editor/GuiLayout.cpp: a .gui file's elements placed (GuiLayout.h). Two walks: natural sizes from the root down
// (each node measured with the width its container offers, which a wrapping row needs) and up (its children's), then
// places from the root down.
#include "editor/GuiLayout.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>

namespace playground::ui
{
    namespace
    {
        constexpr float kNoLimit = std::numeric_limits<float>::max();

        std::string upperText(const std::string& s)
        {
            std::string out;
            for (const char c : s)
                out += static_cast<char>(static_cast<unsigned char>(c) < 0x80 ? std::toupper(static_cast<unsigned char>(c)) : '?');
            return out;
        }

        bool isContainer(GuiKind k)
        {
            return k == GuiKind::row || k == GuiKind::column || k == GuiKind::grid || k == GuiKind::section
                || k == GuiKind::box || k == GuiKind::tabs || k == GuiKind::page;
        }

        class Layouter
        {
        public:
            Layouter(const PanelSpec& spec, const Controls& controls, const GuiMeasure& measure)
                : spec_(spec), nodes_(spec.gui.nodes), controls_(controls), measure_(measure),
                  natural_(nodes_.size()), done_(nodes_.size(), false) {}

            // A node's natural size, margin included, offered `maxW` across (kNoLimit for none).
            GuiExtent natural(int i, float maxW)
            {
                if (i < 0 || static_cast<std::size_t>(i) >= nodes_.size())
                    return {};
                if (done_[static_cast<std::size_t>(i)])
                    return natural_[static_cast<std::size_t>(i)];
                done_[static_cast<std::size_t>(i)] = true;        // a cycle (none is made) would stop here
                const GuiNode& n = nodes_[static_cast<std::size_t>(i)];
                const float inner = maxW == kNoLimit ? kNoLimit : std::max(0.0f, maxW - 2.0f * n.margin);
                GuiExtent e = measure(i, inner);
                if (n.kind != GuiKind::spacer)
                {
                    e.w = std::max(e.w, n.width);
                    e.h = std::max(e.h, n.height);
                }
                e.w = bound(e.w, n.minW, n.maxW);
                e.h = bound(e.h, n.minH, n.maxH);
                e.w += 2.0f * n.margin;
                e.h += 2.0f * n.margin;
                natural_[static_cast<std::size_t>(i)] = e;
                return e;
            }

            // A node at its slot `r` (margin included).
            void place(int i, funkgui::Rect r, bool visible, GuiLayout& out)
            {
                const GuiNode& n = nodes_[static_cast<std::size_t>(i)];
                r = { r.x + n.margin, r.y + n.margin, std::max(0.0f, r.w - 2.0f * n.margin), std::max(0.0f, r.h - 2.0f * n.margin) };
                if (n.maxW > 0.0f && r.w > n.maxW)
                    r.w = n.maxW;
                if (n.maxH > 0.0f && r.h > n.maxH)
                    r.h = n.maxH;
                out.rects[static_cast<std::size_t>(i)] = r;
                out.visible[static_cast<std::size_t>(i)] = visible;
                if (!isContainer(n.kind))
                    return;
                const funkgui::Rect in = inside(i, r);
                if (n.kind == GuiKind::tabs)
                {
                    const int chosen = std::clamp(out.pages[static_cast<std::size_t>(i)], 0,
                                                  std::max(0, static_cast<int>(n.children.size()) - 1));
                    out.pages[static_cast<std::size_t>(i)] = chosen;
                    for (std::size_t k = 0; k < n.children.size(); ++k)
                        place(n.children[k], in, visible && static_cast<int>(k) == chosen, out);
                    return;
                }
                if (n.kind == GuiKind::grid)
                {
                    placeGrid(i, in, visible, out);
                    return;
                }
                if (n.wrap && !n.vertical)
                {
                    placeWrapped(i, in, visible, out);
                    return;
                }
                placeLine(n, n.children, in, visible, out);
            }

            // A tabs node's tabs, for its look, in its rect.
            std::vector<funkgui::Rect> tabRects(int i, const funkgui::Rect& r) const
            {
                const GuiNode& n = nodes_[static_cast<std::size_t>(i)];
                const std::string& look = n.style.look;
                std::vector<funkgui::Rect> out;
                if (look == "side")
                {
                    const float w = sideWidth(i);
                    float y = r.y;
                    for (std::size_t k = 0; k < n.children.size(); ++k)
                    {
                        out.push_back({ r.x, y, w, kGuiSideTabH });
                        y += kGuiSideTabH + 2.0f;
                    }
                    return out;
                }
                const bool bar = look == "bar", pills = look == "pills";
                const funkgui::TextStyle st = bar ? kGuiBarText : kGuiControlLabel;
                float x = r.x + (bar || pills ? 0.0f : 8.0f);
                for (const int c : n.children)
                {
                    const float tw = measure_(upperText(nodes_[static_cast<std::size_t>(c)].text), st);
                    const float w = bar ? tw + 32.0f : (pills ? std::max(48.0f, tw + 28.0f) : std::max(56.0f, tw + 24.0f));
                    out.push_back({ x, r.y, w, bar ? kGuiBarH : (pills ? kGuiPillsH : kGuiTabsH) });
                    x += w + (pills ? 6.0f : 2.0f);
                }
                return out;
            }

        private:
            static float bound(float v, float lo, float hi)
            {
                if (lo > 0.0f)
                    v = std::max(v, lo);
                if (hi > 0.0f)
                    v = std::min(v, hi);
                return v;
            }

            std::string labelOf(int slot) const
            {
                const std::string& l = spec_.labels[static_cast<std::size_t>(slot)];
                return upperText(l.empty() ? std::string(controls_[static_cast<std::size_t>(slot)].name) : l);
            }

            std::vector<std::string> optionsOf(int slot) const
            {
                const Control& c = controls_[static_cast<std::size_t>(slot)];
                const int count = c.type == ControlType::toggle ? 2
                                : c.type == ControlType::integer ? static_cast<int>(std::lround(c.max) - std::lround(c.min)) + 1
                                                                 : 0;
                std::vector<std::string> out;
                const std::vector<std::string>& named = spec_.options[static_cast<std::size_t>(slot)];
                for (int k = 0; k < std::min(count, 64); ++k)
                {
                    if (static_cast<std::size_t>(k) < named.size() && !named[static_cast<std::size_t>(k)].empty())
                        out.push_back(upperText(named[static_cast<std::size_t>(k)]));
                    else if (c.type == ControlType::toggle)
                        out.push_back(k == 0 ? "OFF" : "ON");
                    else
                        out.push_back(std::to_string(std::lround(c.min) + k));
                }
                return out;
            }

            float labelW(int slot) const { return slot >= 0 ? measure_(labelOf(slot), kGuiControlLabel) + 6.0f : 0.0f; }

            float sideWidth(int i) const
            {
                float w = 90.0f;
                for (const int c : nodes_[static_cast<std::size_t>(i)].children)
                    w = std::max(w, measure_(upperText(nodes_[static_cast<std::size_t>(c)].text), kGuiControlLabel) + 36.0f);
                return w;
            }

            // The room a container's frame, title, strip or padding takes: left, top, right, bottom.
            std::array<float, 4> insets(int i) const
            {
                const GuiNode& n = nodes_[static_cast<std::size_t>(i)];
                float l = n.pad, t = n.pad, rt = n.pad, b = n.pad;
                if (n.kind == GuiKind::section)
                {
                    const bool card = n.style.look == "card" || n.style.look == "inset" || n.style.look == "plate";
                    l += kGuiSectionSide;
                    rt += kGuiSectionSide;
                    t += card ? kGuiSectionTop + 4.0f : kGuiSectionTop;
                    b += kGuiSectionBottom;
                }
                else if (n.kind == GuiKind::tabs)
                {
                    const std::string& look = n.style.look;
                    if (look == "bar")
                        t += kGuiBarH + 10.0f;
                    else if (look == "pills")
                        t += kGuiPillsH + 10.0f;
                    else if (look == "side")
                        l += sideWidth(i) + 12.0f;
                    else
                    {
                        l += 8.0f;
                        rt += 8.0f;
                        t += kGuiTabsH + 8.0f;
                        b += 8.0f;
                    }
                }
                return { l, t, rt, b };
            }

            // A node's own size, before width=, height=, its bounds and its margin, `maxW` across at most.
            GuiExtent measure(int i, float maxW)
            {
                const GuiNode& n = nodes_[static_cast<std::size_t>(i)];
                switch (n.kind)
                {
                    case GuiKind::knob:
                    {
                        const GuiKnobGeom g = guiKnobGeom(n, labelW(n.slot));
                        return { g.w, g.h };
                    }
                    case GuiKind::slider:
                        return n.vertical ? GuiExtent{ std::max(44.0f, labelW(n.slot)), 96.0f }
                                          : GuiExtent{ std::max(150.0f, labelW(n.slot) + 40.0f), 44.0f };
                    case GuiKind::toggle:
                    {
                        float w = std::max(44.0f, labelW(n.slot));
                        for (const std::string& o : optionsOf(n.slot))
                            w = std::max(w, measure_(o, kGuiSmall) + 8.0f);
                        return { w, 80.0f };
                    }
                    case GuiKind::buttons:
                    {
                        const std::vector<std::string> opts = optionsOf(n.slot);
                        if (n.vertical)
                        {
                            float w = labelW(n.slot);
                            for (const std::string& o : opts)
                                w = std::max(w, measure_(o, kGuiSmall) + 26.0f);
                            return { std::max(w, 44.0f), std::max(80.0f, kGuiLabelH + 14.0f * static_cast<float>(opts.size()) + 4.0f) };
                        }
                        float widest = 28.0f;           // the cells are equal, each as wide as the widest name needs
                        for (const std::string& o : opts)
                            widest = std::max(widest, measure_(o, kGuiSmall) + 14.0f);
                        return { std::max(widest * static_cast<float>(opts.size()) + 2.0f, labelW(n.slot)), kGuiLabelH + 22.0f };
                    }
                    case GuiKind::menu:
                    {
                        float w = labelW(n.slot);
                        for (const std::string& o : optionsOf(n.slot))
                            w = std::max(w, measure_(o, kGuiSmall) + 30.0f);
                        return { std::max(w, 76.0f), kGuiLabelH + 24.0f };
                    }
                    case GuiKind::xy:
                    {
                        const float s = n.width > 0.0f ? n.width : 120.0f;
                        return { s, s + kGuiLabelH };
                    }
                    case GuiKind::envelope:
                        return { 150.0f, 80.0f };
                    case GuiKind::keyboard:
                        return { std::max(160.0f, 16.0f * static_cast<float>(guiWhiteKeys(n.from, n.keys)))
                                     + (n.wheels ? kGuiWheelsW : 0.0f), 72.0f };
                    case GuiKind::shader:
                        return { 200.0f, 120.0f };
                    case GuiKind::display:
                    {
                        if (n.display < 0 || static_cast<std::size_t>(n.display) >= spec_.displays.size())
                            return {};
                        const PanelDisplay& d = spec_.displays[static_cast<std::size_t>(n.display)];
                        const float lw = measure_(upperText(d.label.empty() ? d.name : d.label), kGuiControlLabel) + 6.0f;
                        switch (d.kind)
                        {
                            case DisplayKind::meter:    return { std::max(d.audio() ? 34.0f : 22.0f, lw), 80.0f };
                            case DisplayKind::vu:       return { std::max(112.0f, lw), 80.0f };
                            case DisplayKind::led:      return { std::max(40.0f, lw), 80.0f };
                            case DisplayKind::number:   return { std::max(76.0f, lw), 80.0f };
                            case DisplayKind::scope:    return { std::max(150.0f, lw), 80.0f };
                            case DisplayKind::spectrum: return { std::max(220.0f, lw), 80.0f };
                        }
                        return {};
                    }
                    case GuiKind::label:
                    {
                        const funkgui::TextStyle st = guiLabelStyle(n);
                        return { measure_(upperText(n.text), st) + 2.0f, std::ceil(st.px * 1.5f) };
                    }
                    case GuiKind::spacer:
                        return { n.width, n.height };
                    case GuiKind::divider:
                    {
                        const bool across = n.parent >= 0 && nodes_[static_cast<std::size_t>(n.parent)].vertical;
                        const float t = std::max(9.0f, n.style.thickness + 8.0f);
                        return across ? GuiExtent{ 0.0f, t } : GuiExtent{ t, 0.0f };
                    }
                    case GuiKind::grid:
                    {
                        const std::array<float, 4> ins = insets(i);
                        const int cols = std::max(1, n.cols);
                        const float cellMax = maxW == kNoLimit ? kNoLimit
                                            : std::max(0.0f, (maxW - ins[0] - ins[2] - n.gap * static_cast<float>(cols - 1)) / static_cast<float>(cols));
                        std::vector<float> colW(static_cast<std::size_t>(cols), 0.0f), rowH;
                        for (std::size_t k = 0; k < n.children.size(); ++k)
                        {
                            const GuiExtent c = natural(n.children[k], cellMax);
                            const std::size_t col = k % static_cast<std::size_t>(cols), row = k / static_cast<std::size_t>(cols);
                            if (row >= rowH.size())
                                rowH.push_back(0.0f);
                            colW[col] = std::max(colW[col], c.w);
                            rowH[row] = std::max(rowH[row], c.h);
                        }
                        float w = 0.0f, h = 0.0f;
                        for (const float x : colW)
                            w += x;
                        for (const float y : rowH)
                            h += y;
                        w += n.gap * static_cast<float>(std::max(0, std::min(cols, static_cast<int>(n.children.size())) - 1));
                        h += n.gap * static_cast<float>(std::max<std::size_t>(rowH.size(), 1) - 1);
                        return { w + ins[0] + ins[2], h + ins[1] + ins[3] };
                    }
                    case GuiKind::tabs:
                    {
                        const std::array<float, 4> ins = insets(i);
                        const float inner = maxW == kNoLimit ? kNoLimit : std::max(0.0f, maxW - ins[0] - ins[2]);
                        GuiExtent e{ 0.0f, 0.0f };
                        for (const int c : n.children)
                        {
                            const GuiExtent p = natural(c, inner);
                            e.w = std::max(e.w, p.w);
                            e.h = std::max(e.h, p.h);
                        }
                        float strip = 0.0f, list = 0.0f;
                        for (const funkgui::Rect& t : tabRects(i, {}))
                        {
                            strip = std::max(strip, t.right());
                            list = std::max(list, t.bottom());
                        }
                        if (n.style.look == "side")
                            return { e.w + ins[0] + ins[2], std::max(e.h, list) + ins[1] + ins[3] };
                        return { std::max(e.w + ins[0] + ins[2], strip), e.h + ins[1] + ins[3] };
                    }
                    case GuiKind::row:
                    case GuiKind::column:
                    case GuiKind::section:
                    case GuiKind::box:
                    case GuiKind::page:
                    {
                        const std::array<float, 4> ins = insets(i);
                        const float inner = maxW == kNoLimit ? kNoLimit : std::max(0.0f, maxW - ins[0] - ins[2]);
                        GuiExtent e;
                        if (n.wrap && !n.vertical)
                        {
                            // Lines as wide as the room offered: each child on the line it fits, a new line when not.
                            float lineW = 0.0f, lineH = 0.0f, w = 0.0f, h = 0.0f;
                            bool first = true, any = false;
                            for (const int c : n.children)
                            {
                                const GuiExtent ce = natural(c, inner);
                                if (any && lineW + n.gap + ce.w > inner)
                                {
                                    w = std::max(w, lineW);
                                    h += (first ? 0.0f : n.gap) + lineH;
                                    first = false;
                                    lineW = lineH = 0.0f;
                                    any = false;
                                }
                                lineW += (any ? n.gap : 0.0f) + ce.w;
                                lineH = std::max(lineH, ce.h);
                                any = true;
                            }
                            if (any)
                            {
                                w = std::max(w, lineW);
                                h += (first ? 0.0f : n.gap) + lineH;
                            }
                            e = { w, h };
                        }
                        else
                        {
                            float main = 0.0f, cross = 0.0f;
                            for (const int c : n.children)
                            {
                                const GuiExtent ce = natural(c, inner);
                                main += n.vertical ? ce.h : ce.w;
                                cross = std::max(cross, n.vertical ? ce.w : ce.h);
                            }
                            main += n.gap * static_cast<float>(std::max<std::size_t>(n.children.size(), 1) - 1);
                            e = n.vertical ? GuiExtent{ cross, main } : GuiExtent{ main, cross };
                        }
                        e.w += ins[0] + ins[2];
                        e.h += ins[1] + ins[3];
                        if (n.kind == GuiKind::section && !n.text.empty())
                            e.w = std::max(e.w, measure_(upperText(n.text), kGuiSectionTitle) + 36.0f);
                        return e;
                    }
                }
                return {};
            }

            funkgui::Rect inside(int i, funkgui::Rect r) const
            {
                const std::array<float, 4> ins = insets(i);
                return { r.x + ins[0], r.y + ins[1], std::max(0.0f, r.w - ins[0] - ins[2]), std::max(0.0f, r.h - ins[1] - ins[3]) };
            }

            // Whether a node takes its container's size across it (align=stretch), rather than its own.
            bool stretches(const GuiNode& c, bool acrossIsWidth) const
            {
                const float own = acrossIsWidth ? c.width : c.height;
                if (own > 0.0f && c.kind != GuiKind::spacer)
                    return false;
                if (isContainer(c.kind) || c.kind == GuiKind::label || c.kind == GuiKind::divider || c.kind == GuiKind::spacer
                    || c.kind == GuiKind::envelope || c.kind == GuiKind::shader || c.kind == GuiKind::keyboard)
                    return true;
                if (c.kind == GuiKind::display && c.display >= 0 && static_cast<std::size_t>(c.display) < spec_.displays.size())
                {
                    const DisplayKind k = spec_.displays[static_cast<std::size_t>(c.display)].kind;
                    return k == DisplayKind::scope || k == DisplayKind::spectrum;
                }
                return false;
            }

            // A child's slot (margin included) in its share (sx, sy, sw, sh) of a line: its own size, or the share where
            // it fills it, placed by its align (its self=, else its container's).
            funkgui::Rect inSlot(const GuiNode& parent, bool vertical, int child, float sx, float sy, float sw, float sh) const
            {
                const GuiNode& c = nodes_[static_cast<std::size_t>(child)];
                const GuiExtent e = natural_[static_cast<std::size_t>(child)];
                float w = e.w, h = e.h;
                if (vertical ? stretches(c, false) : stretches(c, true))    // along: the share it was given
                    (vertical ? h : w) = vertical ? sh : sw;
                const GuiAlign a = c.self >= 0 ? static_cast<GuiAlign>(c.self) : parent.align;
                const bool acrossIsWidth = vertical;
                if (a == GuiAlign::stretch && stretches(c, acrossIsWidth))
                    (acrossIsWidth ? w : h) = acrossIsWidth ? sw : sh;
                if (c.maxW > 0.0f)
                    w = std::min(w, c.maxW + 2.0f * c.margin);
                if (c.maxH > 0.0f)
                    h = std::min(h, c.maxH + 2.0f * c.margin);
                w = std::min(w, std::max(sw, e.w));
                h = std::min(h, std::max(sh, e.h));
                float x = sx, y = sy;
                const auto across = [&](float slot, float size) {
                    switch (a)
                    {
                        case GuiAlign::start:   return 0.0f;
                        case GuiAlign::end:     return slot - size;
                        case GuiAlign::center:
                        case GuiAlign::stretch: return std::floor(0.5f * (slot - size));
                    }
                    return 0.0f;
                };
                if (acrossIsWidth)
                {
                    x += across(sw, w);
                    y += std::floor(0.5f * (sh - h));         // a control in a share that grew: centred along
                }
                else
                {
                    y += across(sh, h);
                    x += std::floor(0.5f * (sw - w));
                }
                return { std::round(x), std::round(y), std::round(w), std::round(h) };
            }

            // One line of children along n's direction in `in` (a row's, a column's, or a wrapping row's line).
            void placeLine(const GuiNode& n, const std::vector<int>& children, const funkgui::Rect& in, bool visible, GuiLayout& out)
            {
                const std::size_t count = children.size();
                if (count == 0)
                    return;
                const bool vertical = n.vertical;
                float used = 0.0f, grow = 0.0f;
                for (const int c : children)
                {
                    const GuiExtent e = natural_[static_cast<std::size_t>(c)];
                    used += vertical ? e.h : e.w;
                    grow += nodes_[static_cast<std::size_t>(c)].grow;
                }
                float gap = n.gap;
                used += gap * static_cast<float>(count - 1);
                const float room = (vertical ? in.h : in.w) - used;
                float at = vertical ? in.y : in.x;
                if (room > 0.0f && grow <= 0.0f)
                {
                    switch (n.justify)
                    {
                        case GuiJustify::start: break;
                        case GuiJustify::center: at += std::floor(0.5f * room); break;
                        case GuiJustify::end: at += room; break;
                        case GuiJustify::between:
                            if (count > 1)
                                gap += room / static_cast<float>(count - 1);
                            else
                                at += std::floor(0.5f * room);
                            break;
                        case GuiJustify::around:
                            gap += room / static_cast<float>(count);
                            at += 0.5f * room / static_cast<float>(count);
                            break;
                    }
                }
                for (const int c : children)
                {
                    const GuiExtent e = natural_[static_cast<std::size_t>(c)];
                    float size = vertical ? e.h : e.w;
                    if (room > 0.0f && grow > 0.0f)
                        size += room * nodes_[static_cast<std::size_t>(c)].grow / grow;
                    const funkgui::Rect slot = vertical ? funkgui::Rect{ in.x, at, in.w, size } : funkgui::Rect{ at, in.y, size, in.h };
                    place(c, inSlot(n, vertical, c, slot.x, slot.y, slot.w, slot.h), visible, out);
                    at += size + gap;
                }
            }

            // A wrapping row: its children in lines as wide as `in`, each line a row as tall as its tallest child.
            void placeWrapped(int i, const funkgui::Rect& in, bool visible, GuiLayout& out)
            {
                const GuiNode& n = nodes_[static_cast<std::size_t>(i)];
                std::vector<std::vector<int>> lines(1);
                float lineW = 0.0f;
                for (const int c : n.children)
                {
                    const float w = natural_[static_cast<std::size_t>(c)].w;
                    if (!lines.back().empty() && lineW + n.gap + w > in.w)
                    {
                        lines.emplace_back();
                        lineW = 0.0f;
                    }
                    lineW += (lines.back().empty() ? 0.0f : n.gap) + w;
                    lines.back().push_back(c);
                }
                float y = in.y;
                for (const std::vector<int>& line : lines)
                {
                    float h = 0.0f;
                    for (const int c : line)
                        h = std::max(h, natural_[static_cast<std::size_t>(c)].h);
                    placeLine(n, line, { in.x, y, in.w, h }, visible, out);
                    y += h + n.gap;
                }
            }

            void placeGrid(int i, const funkgui::Rect& in, bool visible, GuiLayout& out)
            {
                const GuiNode& n = nodes_[static_cast<std::size_t>(i)];
                const int cols = std::max(1, n.cols);
                std::vector<float> colW(static_cast<std::size_t>(cols), 0.0f), rowH;
                for (std::size_t k = 0; k < n.children.size(); ++k)
                {
                    const GuiExtent e = natural_[static_cast<std::size_t>(n.children[k])];
                    const std::size_t col = k % static_cast<std::size_t>(cols), row = k / static_cast<std::size_t>(cols);
                    if (row >= rowH.size())
                        rowH.push_back(0.0f);
                    colW[col] = std::max(colW[col], e.w);
                    rowH[row] = std::max(rowH[row], e.h);
                }
                // Room left across the grid goes to its columns evenly, when it stretches.
                float total = n.gap * static_cast<float>(cols - 1);
                for (const float w : colW)
                    total += w;
                if (in.w > total && n.align == GuiAlign::stretch)
                    for (float& w : colW)
                        w += (in.w - total) / static_cast<float>(cols);
                float y = in.y;
                for (std::size_t row = 0; row < rowH.size(); ++row)
                {
                    float x = in.x;
                    for (std::size_t col = 0; col < colW.size(); ++col)
                    {
                        const std::size_t k = row * static_cast<std::size_t>(cols) + col;
                        if (k >= n.children.size())
                            break;
                        place(n.children[k], inSlot(n, true, n.children[k], x, y, colW[col], rowH[row]), visible, out);
                        x += colW[col] + n.gap;
                    }
                    y += rowH[row] + n.gap;
                }
            }

            const PanelSpec& spec_;
            const std::vector<GuiNode>& nodes_;
            const Controls& controls_;
            const GuiMeasure& measure_;
            std::vector<GuiExtent> natural_;
            std::vector<bool> done_;
        };
    }

    funkgui::TextStyle guiTextStyle(GuiText size)
    {
        switch (size)
        {
            case GuiText::small:  return { 10.0f, 1.4f, 0.03f, false };
            case GuiText::normal: return { 11.0f, 1.6f, 0.03f, false };
            case GuiText::large:  return { 15.0f, 2.4f, 0.05f, false };
            case GuiText::title:  return { 24.0f, 5.0f, 0.08f, false };
        }
        return { 11.0f, 1.6f, 0.03f, false };
    }

    funkgui::TextStyle guiLabelStyle(const GuiNode& n)
    {
        if (n.textPx > 0.0f)
            return { n.textPx, std::max(1.0f, 0.18f * n.textPx), n.textPx >= 18.0f ? 0.07f : 0.04f, false };
        return guiTextStyle(n.textSize);
    }

    GuiKnobGeom guiKnobGeom(const GuiNode& n, float labelWidth)
    {
        GuiKnobGeom g;
        const bool small = n.style.px == 0.0f && n.size == GuiSize::small;
        g.d = n.style.px > 0.0f ? n.style.px : (small ? 22.0f : (n.size == GuiSize::large ? 56.0f : 30.0f));
        const int pos = n.style.labelPos >= 0 ? n.style.labelPos : (small ? 1 : 0);
        g.labelTop = pos == 0;
        g.labelBottom = pos == 1;
        g.value = n.style.valueShow >= 0 ? n.style.valueShow != 2 : !small;
        const float faceTop = g.labelTop ? 18.0f : 6.0f;
        g.cy = faceTop + 0.5f * g.d + 1.0f;
        float y = faceTop + g.d + (g.value ? 12.0f : 10.0f);
        if (g.value)
        {
            g.valueY = y;
            y += 14.0f;
        }
        if (g.labelBottom)
        {
            g.labelY = y;
            y += 14.0f;
        }
        g.h = std::round(y + (small ? 0.0f : 6.0f));
        g.w = std::round(std::max(g.d + 30.0f, labelWidth));
        g.cx = 0.5f * g.w;
        return g;
    }

    int guiWhiteKeys(int from, int keys)
    {
        int whites = 0;
        for (int k = from; k < from + keys; ++k)
        {
            const int pc = ((k % 12) + 12) % 12;
            whites += pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10 ? 0 : 1;
        }
        return whites;
    }

    GuiExtent naturalGuiSize(const PanelSpec& spec, const Controls& controls, const GuiMeasure& measure, float width)
    {
        if (spec.gui.empty())
            return {};
        Layouter l(spec, controls, measure);
        return l.natural(0, width > 0.0f ? width : kNoLimit);
    }

    GuiLayout layoutGui(const PanelSpec& spec, const Controls& controls, const GuiMeasure& measure,
                        const std::vector<int>& pages, funkgui::Rect area, float scrollX, float scrollY)
    {
        GuiLayout out;
        out.area = area;
        const std::size_t n = spec.gui.nodes.size();
        out.rects.assign(n, {});
        out.visible.assign(n, false);
        out.pages.assign(n, 0);
        for (std::size_t i = 0; i < n && i < pages.size(); ++i)
            out.pages[i] = pages[i];
        if (n == 0)
            return out;
        Layouter l(spec, controls, measure);
        const GuiExtent root = l.natural(0, area.w);
        out.contentW = std::max(root.w, area.w);
        out.contentH = std::max(root.h, area.h);
        out.maxScrollX = std::max(0.0f, root.w - area.w);
        out.maxScrollY = std::max(0.0f, root.h - area.h);
        const float sx = std::clamp(scrollX, 0.0f, out.maxScrollX), sy = std::clamp(scrollY, 0.0f, out.maxScrollY);
        l.place(0, { area.x - sx, area.y - sy, out.contentW, out.contentH }, true, out);
        return out;
    }

    std::vector<funkgui::Rect> guiTabRects(const PanelSpec& spec, int node, const funkgui::Rect& rect,
                                           const GuiMeasure& measure)
    {
        if (node < 0 || static_cast<std::size_t>(node) >= spec.gui.nodes.size())
            return {};
        Controls none{};
        Layouter l(spec, none, measure);
        return l.tabRects(node, rect);
    }
}
