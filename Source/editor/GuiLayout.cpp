// Source/editor/GuiLayout.cpp: a .gui file's elements placed (GuiLayout.h). Two walks: natural sizes from the leaves
// up, then places from the root down.
#include "editor/GuiLayout.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace testingplaygroundsynth::ui
{
    namespace
    {
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

            GuiExtent natural(int i)
            {
                if (i < 0 || static_cast<std::size_t>(i) >= nodes_.size())
                    return {};
                if (done_[static_cast<std::size_t>(i)])
                    return natural_[static_cast<std::size_t>(i)];
                done_[static_cast<std::size_t>(i)] = true;        // a cycle (none is made) would stop here
                GuiExtent e = measure(i);
                const GuiNode& n = nodes_[static_cast<std::size_t>(i)];
                if (n.kind != GuiKind::spacer)
                {
                    e.w = std::max(e.w, n.width);
                    e.h = std::max(e.h, n.height);
                }
                natural_[static_cast<std::size_t>(i)] = e;
                return e;
            }

            void place(int i, funkgui::Rect r, bool visible, GuiLayout& out)
            {
                const GuiNode& n = nodes_[static_cast<std::size_t>(i)];
                out.rects[static_cast<std::size_t>(i)] = r;
                out.visible[static_cast<std::size_t>(i)] = visible;
                if (!isContainer(n.kind))
                    return;
                funkgui::Rect in = inside(i, r);
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
                placeFlex(i, in, visible, out);
            }

            // A tabs node's tabs, in its strip.
            std::vector<funkgui::Rect> tabRects(int i, const funkgui::Rect& r)
            {
                std::vector<funkgui::Rect> out;
                float x = r.x;
                for (const int c : nodes_[static_cast<std::size_t>(i)].children)
                {
                    const float w = std::max(56.0f, measure_(upperText(nodes_[static_cast<std::size_t>(c)].text), kGuiControlLabel) + 24.0f);
                    out.push_back({ x, r.y, w, kGuiTabsH });
                    x += w + 2.0f;
                }
                return out;
            }

        private:
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

            // A node's own size, before width= and height=.
            GuiExtent measure(int i)
            {
                const GuiNode& n = nodes_[static_cast<std::size_t>(i)];
                switch (n.kind)
                {
                    case GuiKind::knob:
                    {
                        GuiExtent e = n.size == GuiSize::small ? GuiExtent{ 54.0f, 52.0f }
                                    : n.size == GuiSize::large ? GuiExtent{ 84.0f, 108.0f }
                                                               : GuiExtent{ 60.0f, 80.0f };
                        e.w = std::max(e.w, n.size == GuiSize::small ? measure_(labelOf(n.slot), kGuiSmall) + 6.0f : labelW(n.slot));
                        return e;
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
                        const funkgui::TextStyle st = guiTextStyle(n.textSize);
                        return { measure_(upperText(n.text), st) + 2.0f, std::ceil(st.px * 1.5f) };
                    }
                    case GuiKind::spacer:
                        return { n.width, n.height };
                    case GuiKind::divider:
                    {
                        const bool across = n.parent >= 0 && nodes_[static_cast<std::size_t>(n.parent)].vertical;
                        return across ? GuiExtent{ 0.0f, 9.0f } : GuiExtent{ 9.0f, 0.0f };
                    }
                    case GuiKind::grid:
                    {
                        const int cols = std::max(1, n.cols);
                        std::vector<float> colW(static_cast<std::size_t>(cols), 0.0f), rowH;
                        for (std::size_t k = 0; k < n.children.size(); ++k)
                        {
                            const GuiExtent c = natural(n.children[k]);
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
                        return { w + 2.0f * n.pad, h + 2.0f * n.pad };
                    }
                    case GuiKind::tabs:
                    {
                        GuiExtent e{ 0.0f, 0.0f };
                        for (const int c : n.children)
                        {
                            const GuiExtent p = natural(c);
                            e.w = std::max(e.w, p.w);
                            e.h = std::max(e.h, p.h);
                        }
                        float strip = 0.0f;
                        for (const funkgui::Rect& t : tabRects(i, {}))
                            strip = t.right();
                        return { std::max(e.w + 2.0f * (n.pad + 8.0f), strip), e.h + kGuiTabsH + 2.0f * (n.pad + 8.0f) };
                    }
                    case GuiKind::row:
                    case GuiKind::column:
                    case GuiKind::section:
                    case GuiKind::box:
                    case GuiKind::page:
                    {
                        float main = 0.0f, cross = 0.0f;
                        for (const int c : n.children)
                        {
                            const GuiExtent e = natural(c);
                            main += n.vertical ? e.h : e.w;
                            cross = std::max(cross, n.vertical ? e.w : e.h);
                        }
                        main += n.gap * static_cast<float>(std::max<std::size_t>(n.children.size(), 1) - 1);
                        GuiExtent e = n.vertical ? GuiExtent{ cross, main } : GuiExtent{ main, cross };
                        e.w += 2.0f * n.pad;
                        e.h += 2.0f * n.pad;
                        if (n.kind == GuiKind::section)
                        {
                            e.w += 2.0f * kGuiSectionSide;
                            e.h += kGuiSectionTop + kGuiSectionBottom;
                            if (!n.text.empty())
                                e.w = std::max(e.w, measure_(upperText(n.text), kGuiSectionTitle) + 36.0f);
                        }
                        return e;
                    }
                }
                return {};
            }

            // A container's inside: its rect less its padding (and a section's frame, the tabs' strip and frame).
            funkgui::Rect inside(int i, funkgui::Rect r) const
            {
                const GuiNode& n = nodes_[static_cast<std::size_t>(i)];
                float l = n.pad, t = n.pad, rt = n.pad, b = n.pad;
                if (n.kind == GuiKind::section)
                {
                    l += kGuiSectionSide;
                    rt += kGuiSectionSide;
                    t += kGuiSectionTop;
                    b += kGuiSectionBottom;
                }
                else if (n.kind == GuiKind::tabs)
                {
                    l += 8.0f;
                    rt += 8.0f;
                    t += kGuiTabsH + 8.0f;
                    b += 8.0f;
                }
                return { r.x + l, r.y + t, std::max(0.0f, r.w - l - rt), std::max(0.0f, r.h - t - b) };
            }

            // Whether a node takes its container's size across it (align=stretch), rather than its own.
            bool stretches(const GuiNode& c, bool acrossIsWidth) const
            {
                const float own = acrossIsWidth ? c.width : c.height;
                if (own > 0.0f && c.kind != GuiKind::spacer)
                    return false;
                if (isContainer(c.kind) || c.kind == GuiKind::label || c.kind == GuiKind::divider || c.kind == GuiKind::spacer
                    || c.kind == GuiKind::envelope)
                    return true;
                if (c.kind == GuiKind::display && c.display >= 0 && static_cast<std::size_t>(c.display) < spec_.displays.size())
                {
                    const DisplayKind k = spec_.displays[static_cast<std::size_t>(c.display)].kind;
                    return k == DisplayKind::scope || k == DisplayKind::spectrum;
                }
                return false;
            }

            // Whether a node grows to fill its slot along a container's direction, given room there (a container, and
            // what stretches); a control keeps its cell, centred in the slot.
            bool fills(const GuiNode& c, bool alongIsWidth) const { return stretches(c, alongIsWidth); }

            // A child's rect in its slot (sx, sy, sw, sh): its own size, or the slot's where it fills it, placed by align.
            funkgui::Rect inSlot(const GuiNode& parent, int child, float sx, float sy, float sw, float sh) const
            {
                const GuiNode& c = nodes_[static_cast<std::size_t>(child)];
                const GuiExtent e = natural_[static_cast<std::size_t>(child)];
                const bool vertical = parent.vertical && parent.kind != GuiKind::grid;
                float w = e.w, h = e.h;
                // Along: the slot's (it was sized for the child) when the child fills it.
                if (vertical ? fills(c, false) : fills(c, true))
                    (vertical ? h : w) = vertical ? sh : sw;
                // Across: align.
                const GuiAlign a = parent.align;
                const bool acrossIsWidth = vertical;
                if (a == GuiAlign::stretch && stretches(c, acrossIsWidth))
                    (acrossIsWidth ? w : h) = acrossIsWidth ? sw : sh;
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
                    y += std::floor(0.5f * (sh - h));         // a control in a slot that grew: centred along
                }
                else
                {
                    y += across(sh, h);
                    x += std::floor(0.5f * (sw - w));
                }
                return { std::round(x), std::round(y), std::round(w), std::round(h) };
            }

            void placeFlex(int i, const funkgui::Rect& in, bool visible, GuiLayout& out)
            {
                const GuiNode& n = nodes_[static_cast<std::size_t>(i)];
                const std::size_t count = n.children.size();
                if (count == 0)
                    return;
                const bool vertical = n.vertical;
                float used = 0.0f, grow = 0.0f;
                for (const int c : n.children)
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
                for (const int c : n.children)
                {
                    const GuiExtent e = natural_[static_cast<std::size_t>(c)];
                    float size = vertical ? e.h : e.w;
                    if (room > 0.0f && grow > 0.0f)
                        size += room * nodes_[static_cast<std::size_t>(c)].grow / grow;
                    const funkgui::Rect slot = vertical ? funkgui::Rect{ in.x, at, in.w, size } : funkgui::Rect{ at, in.y, size, in.h };
                    place(c, inSlot(n, c, slot.x, slot.y, slot.w, slot.h), visible, out);
                    at += size + gap;
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
                        GuiNode cell = n;                     // a cell centres a control, stretches the rest
                        cell.vertical = true;
                        place(n.children[k], inSlot(cell, n.children[k], x, y, colW[col], rowH[row]), visible, out);
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

    GuiExtent naturalGuiSize(const PanelSpec& spec, const Controls& controls, const GuiMeasure& measure)
    {
        if (spec.gui.empty())
            return {};
        Layouter l(spec, controls, measure);
        return l.natural(0);
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
        for (std::size_t i = 0; i < n; ++i)
            l.natural(static_cast<int>(i));
        const GuiExtent root = l.natural(0);
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
        return l.tabRects(node, { rect.x + 8.0f, rect.y, rect.w - 16.0f, kGuiTabsH });
    }
}
