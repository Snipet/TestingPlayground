// Source/editor/ShapeView.h: the LFO shape editor, a FunkGui view over EditorPort's shape (message thread only).
//
// The port of the legacy GraphEditor (git show legacy-visage:Source/gui/GraphEditor.cpp, 1,486 lines on Visage) cut
// down to what it was for: drawing one cycle of an LFO as points joined by curves. Its point canvas is here, with the
// legacy gestures and maths (LfoShape.h); the rest is not ported:
//   kept      a point drag (Alt snaps x and y to the eighths of the grid; Cmd keeps the point between its neighbours);
//             a drag on a segment's midpoint handle bends it (the legacy curve, solved through the handle); a
//             double-click adds a point (and keeps dragging it while the button is held) or removes the one under the
//             hand; Delete or Backspace removes the selected point; the grid of eighths; the curve's fill
//   changed   a right-click opens a menu (Delete point, Reset shape) instead of deleting at once: in FunkGui a popup
//             click never writes; the two edge points move together, because the cycle wraps there and a step would
//             jump the delay lines; arrow keys move the selected point (1/64 of the range, Shift 1/256)
//   dropped   the File and Edit menus (New, Save, Save As, Copy, Cut, Paste), the .lfo files, the strip of several
//             graphs, the LFO / ONESHOT switch and the side panel (LATENCY, RATE, AMPLITUDE, PHASE): the shape lives in
//             the plugin state now, there is one LFO, and its RATE and DEPTH are host parameters (dials beside it)
//
// The plot draws one LFO cycle left to right, +1 (the longest delay) at the top and -1 at the bottom; the zero line is
// the unmodulated delay. The curve uses the theme's signal colour, the job this product gives it.
#pragma once

#include "dsp/LfoShape.h"

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/Input.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace funkgui
{
    class Canvas;
    class HostServices;
}

namespace testingplayground
{
    class EditorPort;
}

namespace testingplayground::ui
{
    class ShapeView
    {
    public:
        // `bounds`: the view, caption included; the plot is inside it (plot()).
        ShapeView(EditorPort& port, funkgui::Rect bounds, uint32_t a11yId);

        void tick(float dt, bool hovered, bool focused);
        void draw(funkgui::Canvas&, const funkgui::Theme&, bool focusRing) const;
        bool contains(funkgui::Point) const;
        funkgui::Cursor cursorAt(funkgui::Point) const;
        void pointerMove(const funkgui::PointerEvent&);
        void pointerExit();
        void pointerDown(const funkgui::PointerEvent&, funkgui::HostServices&, const funkgui::Theme& menuTheme);
        void pointerDrag(const funkgui::PointerEvent&);
        void pointerUp(const funkgui::PointerEvent&);
        void doubleClick(const funkgui::PointerEvent&);
        bool key(const funkgui::KeyEvent&);
        void accessibility(std::vector<funkgui::A11yItem>&) const;
        bool a11yAction(uint32_t id, funkgui::A11yAction, double value);
        void specLine(char* out, std::size_t n) const;
        bool settled() const noexcept { return true; }   // nothing eases: every change shows on the next frame
        uint32_t a11yId() const noexcept { return a11yId_; }

        // ---- geometry, for the Panel's layout and the probes -----------------------------------------------------
        const funkgui::Rect& bounds() const noexcept { return bounds_; }
        funkgui::Rect plot() const noexcept;
        funkgui::Point pointAt(int i) const;               // where point i is drawn
        funkgui::Point midpointAt(int i) const;            // the bend handle of the segment from point i
        const LfoShape& shown() const noexcept { return shape_; }   // the shape as last read from the port
        int selected() const noexcept { return selected_; }

        static constexpr int kGrid = 8;                    // the legacy's default subdivisions (snap with Alt)
        static constexpr float kHitRadius = 10.0f;         // the legacy's handle hit radius, px
        static constexpr float kLabelW = 22.0f;            // the +1 / 0 / -1 labels left of the plot
        static constexpr float kCaptionH = 18.0f;

    private:
        enum class Drag : uint8_t { none, point, curve };

        void refresh();                                    // re-reads the port's shape when its revision moved
        void commit();                                     // writes shape_ to the port (and re-reads the result)
        void finish();                                     // an edit is complete: the port tells the host
        int  pointNear(funkgui::Point, float radius) const;
        int  midpointNear(funkgui::Point, float radius) const;
        void hoverAt(funkgui::Point);
        void deletePoint(int i);
        void insertPoint(funkgui::Point);
        void movePoint(int i, float x, float y, bool keepBetween);
        float toX(float px) const noexcept;                // view px -> shape x in [0, 1]
        float toY(float py) const noexcept;

        EditorPort& port_;
        funkgui::Rect bounds_;
        uint32_t a11yId_;

        LfoShape shape_{};
        uint64_t revision_ = 0;
        int hoverPoint_ = -1, hoverMid_ = -1, selected_ = -1;
        Drag drag_ = Drag::none;
        int dragIndex_ = -1;
        bool edited_ = false;                              // the current gesture changed the shape
        bool hovered_ = false, focused_ = false;
    };
}
