// Source/editor/Scroll.h: one axis of scrolling as FunkGui's TextEditor scrolls (funkgui/widgets/Scroll.h, the rule and
// the bar's geometry the TextEditor itself uses), which every list and scrolling view of the editor takes, so that each
// scrolls as the code does:
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
// drawKey(). The wheel's and the bar's constants are FunkGui's (funkgui::scroll, since FunkGui F1: none is copied here);
// gui.scroll holds this to the TextEditor's behaviour.
#pragma once

#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/widgets/Scroll.h>

#include <cstdint>

namespace playground::ui
{
    struct Scroll
    {
        static constexpr float kBarHit = 2.0f;                  // the bar's hit rect, this much larger (ours)

        float offset = 0.0f;                     // 0 .. max(), never rounded here
        float content = 0.0f, view = 0.0f;       // set by the owner at each layout
        float row = 24.0f;                       // the height three of which a notch moves
        bool horizontal = false;                 // the bar along the area's bottom

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
