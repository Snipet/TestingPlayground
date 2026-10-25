// Source/editor/DialLayout.h: where the dials and displays go, under the problems list (the Panel's layout).
//
//   plain rows   a program with no section, no display and only knobs: the dials, in order, in as few rows as the
//                column's width holds, shared out evenly between the rows (a row of sixteen on a wide panel, two of
//                eight at the default size)
//   sections     what the program's #pragma panel lines ask for (fminus/PanelSpec.h), as an analog synth's panel is
//                printed: each section a frame with its title cut into the top edge, its items inside (dials of every
//                style, and displays), the sections left to right in rows (a row breaks where the program says, or a
//                run too wide for the window splits where its widest row is narrowest), each row as tall as its
//                tallest item, and the shorter rows widened toward the widest so that the sections stand as one block.
//                An item's slot is as wide as its cell or its label needs. When the sections at full size need more
//                height than the panel can give, every knob is drawn small.
//
// The cells (an item's place; its label sits at its top):
//   knob, vintage   60 x 80 (small: 54 x 52, its label under the knob, sitting with its knob on the full ones' line)
//   fader, switch   44 x 80
//   buttons         its options' width and a light, 16 px and 14 a value tall (80 at least)
//   meter           LED bars: 34 x 80 for the audio (left and right), 22 x 80 for a field
//   vu              112 x 80        led  40 x 80        number  76 x 80        scope  150 x 80
#pragma once

#include "dsp/Controls.h"
#include "fminus/PanelSpec.h"

#include <funkgui/core/Geometry.h>

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace testingplaygroundsynth::ui
{
    struct ItemPlace
    {
        float x = 0.0f, top = 0.0f, w = 0.0f, h = 0.0f;   // the item's cell
        bool small = false;                               // a knob drawn small (funkgui::DialSize::compact)
    };

    struct SectionFrame
    {
        funkgui::Rect r;                      // the frame (its top edge carries the title)
        std::string title;                    // as the program wrote it; "" for none
        std::string color;                    // its colour's name (PanelSpec), "" for none
    };

    struct DialsLayout
    {
        std::array<ItemPlace, kMaxControls> dials{};      // every slot's (an unused one's is not shown)
        std::array<ItemPlace, kMaxDisplays> displays{};   // every display's
        std::vector<SectionFrame> sections;   // none: the plain rows
        float top = 0.0f;                     // the area's top; its bottom is the `bottom` asked for
        int rows = 1;                         // rows of dials (plain) or of sections
        int cols = 1;                         // plain: the dials in a full row
        bool allSmall = false;                // the sections took every knob small to fit the height
    };

    struct DialMeasure
    {
        std::function<float(int slot, bool small)> label;     // a dial's label's width at that size
        std::function<float(const std::string&)> title;       // a section's title's width
        std::function<float(const std::string&)> caption;     // a display's label's width (kCaption)
        std::function<float(const std::string&)> micro;       // an option's (kMicro), on buttons
    };

    // The dials of `controls` (its used slots) and the displays of `spec` in a column `width` wide from `x`, ending at
    // `bottom`; `maxHeight` is what the sections may take before their knobs go small.
    DialsLayout layoutDials(const PanelSpec& spec, const Controls& controls, const DialMeasure& measure, float x,
                            float width, float bottom, float maxHeight);

    // The geometry's numbers, for the Panel's drawing and the probes.
    inline constexpr float kDialW = 60.0f, kDialH = 80.0f;                // funkgui::DialGeom, primary
    inline constexpr float kSmallDialW = 54.0f, kSmallDialH = 52.0f;      // compact
    inline constexpr float kFaderW = 44.0f, kSwitchW = 44.0f;
    inline constexpr float kButtonRowH = 14.0f, kButtonsTop = 16.0f;      // a value's row, and the first row's top
    inline constexpr float kMeterW = 34.0f, kMeterMonoW = 22.0f, kVuW = 112.0f, kLedW = 40.0f, kNumberW = 76.0f,
                           kScopeW = 150.0f;
    inline constexpr float kSectionTitleTop = 5.0f;                      // the frame's top edge, below the row's top
    inline constexpr float kSectionDialsTop = 16.0f;                     // the items, below the row's top
}
