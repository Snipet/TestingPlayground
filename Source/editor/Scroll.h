// Source/editor/Scroll.h: one axis of scrolling as FunkGui's TextEditor scrolls (src/widgets/TextEditor.cpp), which
// every list and scrolling view of the editor takes, so that each scrolls as the code does:
//
//   the wheel   offset -= delta * (smooth ? 512 : 3 * row / (50 / 256)): a trackpad's point one px, a notched wheel's
//               notch three of the surface's rows. The delta goes in as JUCE gave it (e.dy, or e.dx): deltaY already
//               follows the user's natural scrolling (macOS's scrollingDeltaY; JUCE's Viewport reads no isReversed),
//               so WheelEvent::reversed is never read here, nor by any caller (it says which way the hand moved: a
//               value control's business, a dial's or a slider's, never a list's)
//   the offset  a float in px, 0 .. max(), clamped, never rounded here (a view that places items at whole px rounds it
//               itself: GuiView) and never animated
//   the bar     at the area's right (bottom, horizontal): 4 px wide, 2 px in, a thumb at least 12 px, ink32 (ink52
//               while dragged), hit through a rect 2 px larger. A press on the thumb drags it; on the track, the thumb
//               jumps under the pointer and drags from there
//
// The owner sets content, view and row at each layout (or before each use, from its places), and mixes key() into its
// drawKey(). The constants are FunkGui's TextEditor's, copied until FunkGui exposes them; gui.scroll holds them to it.
#pragma once

#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>

#include <cstdint>

namespace playground::ui
{
    struct Scroll
    {
        static constexpr float kWheelPxPerUnit = 512.0f;        // smooth: JUCE's precise device gives 0.5/256 a point
        static constexpr float kWheelNotch = 50.0f / 256.0f;    // one notch of a notched wheel
        static constexpr float kRowsPerNotch = 3.0f;
        static constexpr float kBarW = 4.0f, kBarInset = 2.0f, kBarMin = 12.0f, kBarHit = 2.0f;

        float offset = 0.0f;                     // 0 .. max(), never rounded here
        float content = 0.0f, view = 0.0f;       // set by the owner at each layout
        float row = 24.0f;                       // the height three of which a notch moves
        bool horizontal = false;                 // the bar along the area's bottom

        // Px a unit of the wheel's delta moves, for a row of `row` px.
        static constexpr float pxPerUnit(bool smooth, float row) noexcept
        {
            return smooth ? kWheelPxPerUnit : kRowsPerNotch * row / kWheelNotch;
        }

        float max() const noexcept { return content > view ? content - view : 0.0f; }
        void clamp() noexcept;
        bool shown() const noexcept { return content > view && view > 0.0f; }   // something overflows: a bar
        // The wheel's delta as it came (e.dy, or e.dx): false when nothing overflows (the event is not used); an edge
        // bump is true (the parent never scrolls under it), as the TextEditor's.
        bool wheel(float delta, bool smooth) noexcept;
        void reveal(float from, float to) noexcept;     // the least move that shows from .. to (the keys)
        bool moveTo(float to) noexcept;                 // clamped: whether it moved

        funkgui::Rect track(const funkgui::Rect& area) const noexcept;
        funkgui::Rect thumb(const funkgui::Rect& area) const noexcept;
        bool hits(funkgui::Point p, const funkgui::Rect& area) const noexcept;   // the bar, 2 px larger
        bool press(funkgui::Point p, const funkgui::Rect& area) noexcept;        // on the bar: its drag starts
        void drag(funkgui::Point p, const funkgui::Rect& area) noexcept;
        void release() noexcept { grab_ = -1.0f; }
        bool dragging() const noexcept { return grab_ >= 0.0f; }
        void draw(funkgui::Canvas& c, const funkgui::Rect& area, const funkgui::Theme& th) const;
        uint64_t key() const noexcept;                  // for the owner's drawKey()

    private:
        float along(funkgui::Point p) const noexcept { return horizontal ? p.x : p.y; }
        float grab_ = -1.0f;                            // the pointer's distance into the thumb while dragged
    };
}
