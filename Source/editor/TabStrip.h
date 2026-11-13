// Source/editor/TabStrip.h: the files' tabs as a strip that scrolls sideways (Panel.cpp's FilesItem draws them and
// owns what they do): where each tab is, what is in view, and the controls the strip adds when its tabs do not fit.
//
//   the tabs      each at its own width (its name's, FilesItem's naturalWidth: kTabMinW to kTabMaxW), side by side from
//                 the bar's left, never narrowed; the strip's view clips them
//   overflowing   when they are wider than the bar leaves them (the + box, the IR tab, a netlist's pills keep their
//                 places at the right and never scroll):
//                   - the strip scrolls sideways (ui::Scroll, horizontal): the wheel over it, either axis, as JUCE gave
//                     it (WheelEvent::reversed is never read: Scroll.h)
//                   - the tab that becomes active is brought into view (reveal)
//                   - a thin bar under the strip (in the gap between the tabs and the editor), while the pointer is
//                     over the strip or the bar is dragged; a press on it drags as Scroll's does
//                   - an edge that hides tabs fades, and a chevron there steps one tab
//                   - a files dropdown (the chevron alone) at the strip's right lists every file
//
// Pure geometry and state: no drawing, no Panel. Message thread only.
#pragma once

#include "editor/Scroll.h"

#include <funkgui/core/Geometry.h>
#include <funkgui/panel/Input.h>

#include <cstdint>
#include <vector>

namespace playground::ui
{
    class TabStrip
    {
    public:
        static constexpr float kMenuW = 28.0f;           // the files dropdown's trigger (its chevron centred)
        static constexpr float kChevronW = 20.0f;        // an edge's step chevron, over the tabs it hides
        static constexpr float kFadeW = 28.0f;           // the fade at an edge that hides tabs
        static constexpr float kNotchRow = 32.0f;        // Scroll's row: a wheel's notch moves three of them

        // At each layout (cheap: FilesItem calls it before any use): the bar, the tabs' widths in order, the room the
        // fixed parts at the bar's right take (the IR tab, the pills, with their gaps), the + box's width (0: none),
        // the gap between the parts, and how far down the strip's area reaches (the editor's top: the bar's track).
        void layout(const funkgui::Rect& bar, const std::vector<float>& widths, float fixedRight, float newTabW, float gap,
                    float areaBottom);

        bool overflows() const noexcept { return overflow_; }
        const funkgui::Rect& view() const noexcept { return view_; }   // where tabs show (clipped to it)
        funkgui::Rect area() const noexcept;                            // the view and the gap under it (the bar's)
        funkgui::Rect tab(int i) const;                                 // scrolled; may lie outside the view
        funkgui::Rect shown(int i) const;                               // tab(i) within the view; empty when out
        const funkgui::Rect& newTab() const noexcept { return newTab_; }  // empty with no + box
        const funkgui::Rect& menu() const noexcept { return menu_; }     // empty unless overflowing
        funkgui::Rect chevron(bool right) const;                         // empty while that edge hides nothing
        int count() const noexcept { return static_cast<int>(x_.size()); }

        // The tab under p (in the view, not under a chevron), else -1.
        int tabAt(funkgui::Point p) const;
        // The strip moved: the wheel over its area (either axis), a chevron's step, a tab shown.
        bool wheel(const funkgui::WheelEvent& e);
        void reveal(int i);
        bool step(bool right);                           // the chevron's: the next tab hidden that way, wholly in view
        // The bar: shown while overflowing and `hover` (the pointer over the area) or dragged.
        bool barShown(bool hover) const noexcept { return overflow_ && (hover || scroll_.dragging()); }
        funkgui::Rect barTrack() const { return scroll_.track(barArea()); }
        funkgui::Rect barThumb() const { return scroll_.thumb(barArea()); }
        bool barHits(funkgui::Point p) const { return overflow_ && scroll_.hits(p, barArea()); }
        bool press(funkgui::Point p) { return overflow_ && scroll_.press(p, barArea()); }
        void drag(funkgui::Point p) { scroll_.drag(p, barArea()); }
        void release() noexcept { scroll_.release(); }
        bool dragging() const noexcept { return scroll_.dragging(); }
        float offset() const noexcept { return scroll_.offset; }
        uint64_t key() const noexcept;                   // for FilesItem's drawKey

    private:
        funkgui::Rect barArea() const noexcept { return { view_.x, view_.y, view_.w, areaBottom_ - view_.y }; }

        Scroll scroll_{};
        std::vector<float> x_, w_;                       // each tab's left in the content, and its width
        funkgui::Rect view_{}, newTab_{}, menu_{};
        float areaBottom_ = 0.0f;
        bool overflow_ = false;
    };
}
