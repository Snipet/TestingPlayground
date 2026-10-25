// Source/editor/DialLayout.h: where the dials go, under the problems list (the Panel's layout).
//
//   no section   the dials of the program that plays, in order, in as few rows as the column's width holds, shared out
//                evenly between the rows (a row of sixteen on a wide panel, two of eight at the default size)
//   sections     what the program's #pragma panel lines ask for (fminus/PanelSpec.h), as an analog synth's panel is
//                printed: each section a frame with its title cut into the top edge, its dials inside, the sections
//                left to right in rows (a row breaks where the program says, or where the next section does not
//                fit), each row as tall as its tallest, and the shorter rows widened toward the widest so that the
//                sections stand as one block. A dial's slot is as wide as its label needs. When the sections at full
//                size need more height than the panel can give, every dial is drawn small.
//
// A small dial (funkgui::DialSize::compact, 54 x 52: its knob and its label under it) sits with its knob on the line of
// the full ones' (60 x 80: the label, the knob, the value) in a row that has both.
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
    struct DialPlace
    {
        float x = 0.0f, top = 0.0f;           // the dial's cell, as funkgui::DialGeom takes it
        bool small = false;
    };

    struct SectionFrame
    {
        funkgui::Rect r;                      // the frame (its top edge carries the title)
        std::string title;                    // as the program wrote it; "" for none
    };

    struct DialsLayout
    {
        std::array<DialPlace, kMaxControls> dials{};   // every slot's (an unused one's is not shown)
        std::vector<SectionFrame> sections;   // none: the plain rows
        float top = 0.0f;                     // the area's top; its bottom is the `bottom` asked for
        int rows = 1;                         // rows of dials (plain) or of sections
        int cols = 1;                         // plain: the dials in a full row
        bool allSmall = false;                // the sections took every dial small to fit the height
    };

    struct DialMeasure
    {
        std::function<float(int slot, bool small)> label;   // a dial's label's width at that size
        std::function<float(const std::string&)> title;      // a section's title's width
    };

    // The dials of `controls` (its used slots) in a column `width` wide from `x`, ending at `bottom`; `maxHeight` is what
    // the sections may take before they go small.
    DialsLayout layoutDials(const PanelSpec& spec, const Controls& controls, const DialMeasure& measure, float x,
                            float width, float bottom, float maxHeight);

    // The geometry's numbers, for the Panel's drawing and the probes.
    inline constexpr float kDialW = 60.0f, kDialH = 80.0f;                // funkgui::DialGeom, primary
    inline constexpr float kSmallDialW = 54.0f, kSmallDialH = 52.0f;      // compact
    inline constexpr float kSectionTitleTop = 5.0f;                      // the frame's top edge, below the row's top
    inline constexpr float kSectionDialsTop = 16.0f;                     // the dials, below the row's top
}
