// Source/editor/GuiLayout.h: where a .gui file's elements go (fminus/GuiSpec.h), as CSS's flexbox would put them.
//
// Every element has a natural size: a control's or a display's cell (below), a label's text, a container's children
// with their gaps and its padding (a section's frame, a box's, the tabs' strip around them). Then, from the root down,
// each container shares its room out: along its direction, the room left over goes to the children that grow (in
// proportion to grow=), else where justify= puts them; across it, align= places them (stretch, the default, makes a
// container, a label, a divider, a scope, a spectrum and an envelope as wide or tall as their container's inside,
// unless they have a width= or height= of their own; a control keeps its cell, centred). A container's width= and
// height= make it at least that big. A tabs' pages share one size, the largest's, and only the chosen one is laid out.
//
// The root fills the area it is given and is never smaller than its natural size: when that is larger than the area,
// the panel scrolls (scrollX, scrollY, up to maxScroll*).
//
// The cells (px; the label above, as DialLayout.h's):
//   knob            small 54 x 52, medium 60 x 80, large 84 x 108 (vintage the same)
//   slider          vertical 44 x 96 (height= sets it), horizontal 150 x 44 (width=)
//   switch          44 x 80
//   buttons         a column: 16 + 14 a value; a row: 16 + 20, a cell a value
//   menu            its longest name + 26 (width= sets it) x 40
//   xy              120 square (size=), and 16 for its label
//   envelope        150 x 80          scope 150 x 80          spectrum 220 x 80
//   meter 34 x 80 (a field's 22)   vu 112 x 80   led 40 x 80   number 76 x 80
// and each as wide as its label needs.
#pragma once

#include "dsp/Controls.h"
#include "fminus/PanelSpec.h"

#include <funkgui/core/Geometry.h>
#include <funkgui/text/TextStyle.h>

#include <functional>
#include <string>
#include <vector>

namespace testingplayground::ui
{
    struct GuiExtent
    {
        float w = 0.0f, h = 0.0f;
    };

    // The width of `text` set in `style` (the Panel's fonts).
    using GuiMeasure = std::function<float(const std::string& text, const funkgui::TextStyle& style)>;

    struct GuiLayout
    {
        std::vector<funkgui::Rect> rects;     // every node's place (a control's or a display's: its cell), by node
        std::vector<bool> visible;            // on the pages shown, by node
        funkgui::Rect area;                   // where the panel is seen (the clip)
        float contentW = 0.0f, contentH = 0.0f;   // the root's size
        float maxScrollX = 0.0f, maxScrollY = 0.0f;
        std::vector<int> pages;               // each tabs node's chosen page (a child's index), by node
    };

    // The styles the layout measures with and the Panel draws with.
    funkgui::TextStyle guiTextStyle(GuiText size);
    inline constexpr funkgui::TextStyle kGuiControlLabel{ 10.0f, 2.0f, 0.03f, false };    // funkgui::type::kCaption
    inline constexpr funkgui::TextStyle kGuiSmall{ 10.0f, 1.4f, 0.03f, false };           // kMicro
    inline constexpr funkgui::TextStyle kGuiSectionTitle{ 10.0f, 2.4f, 0.10f, false };    // as a #pragma section's

    inline constexpr float kGuiTabsH = 24.0f;              // the tabs' strip
    inline constexpr float kGuiSectionTop = 20.0f, kGuiSectionSide = 10.0f, kGuiSectionBottom = 10.0f;
    inline constexpr float kGuiSectionEdge = 6.0f;         // a section's frame's top edge, under its top (its title on it)
    inline constexpr float kGuiLabelH = 16.0f;             // a control's label, over its cell

    // The root's natural size.
    GuiExtent naturalGuiSize(const PanelSpec& spec, const Controls& controls, const GuiMeasure& measure);

    // Every node of spec.gui in `area`, scrolled by (scrollX, scrollY) (clamped to what there is to scroll), with
    // `pages` (by node; a missing or wrong entry: the first page).
    GuiLayout layoutGui(const PanelSpec& spec, const Controls& controls, const GuiMeasure& measure,
                        const std::vector<int>& pages, funkgui::Rect area, float scrollX, float scrollY);

    // A tabs node's tabs: their rects in its strip (its rect's top kGuiTabsH).
    std::vector<funkgui::Rect> guiTabRects(const PanelSpec& spec, int node, const funkgui::Rect& rect,
                                           const GuiMeasure& measure);
}
