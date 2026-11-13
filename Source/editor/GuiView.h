// Source/editor/GuiView.h: a .gui file's interface on a panel (fminus/GuiSpec.h): the items it is made of (GuiItems.h:
// a control a parameter, pools of tabs, pads, envelopes, keyboards and shaders; and the displays, DisplayItem.h, which
// its owner makes) laid out in an area (GuiLayout.h), placed, fed and drawn. The Panel shows one in place of the dials
// (Panel.cpp: under the editor, or across the column in the PANEL view); the GUI sandbox (Source/sandbox/) shows one
// with no plugin: the same items, made in the same order and placed, fed and drawn by this same code, so its picture is
// the PANEL view's.
//
// The items live in the owner's list (its Tab order, hit-testing and accessibility ids, which this leaves as they
// were): the constructor appends them there and remembers where. The owner draws its list in order, each item that
// owns() says is the interface's clipped to the area in theme()'s colours, between drawGround() and drawScrollBars().
//
//   layout()            the interface `spec` in `area` (its root never smaller than its natural size: it scrolls past
//                       the area, which then keeps kScrollGutter at its right for the bar), the tabs' pages and the
//                       scroll kept (clamped), then every item placed (place()). It lays out once, at no scroll (the
//                       content's places, and each tabs node's tabs); what is shown is that moved by the scroll
//   scrollBy()          the scroll moved: the items placed again, each moved by the scroll (whole px), and nothing
//                       laid out again (a trackpad sends a hundred events a second). The owner scrolls it as the code
//                       scrolls (Scroll.h: wheelPx(), a notch kRow * 3 px, the delta as it came); its bars' thumbs drag
//                       (pressBar(), dragBar(), releaseBar())
//   clear()             no interface, or one hidden (the editor's panel under the code): every item hidden, its pages
//                       and scroll kept for the next layout()
//   setControls()       each slot's dial (its Control, label and options' names) for the controls
//   registerShaders()   the spec's shaders' programs given to FunkGui's registry (once built: Shaders.h), or why not
//   tick()              a frame: the controls' live= marks and the shaders' inputs, from the meters (and the
//                       pointer over each shader: pointer.x ...)
//   showPointer()       a picture's pointer (view_panel's and the sandbox's values: pointer.hover ...): every shader's
//                       pointer inputs drawn at those values, in place of the pointer's
//
// The pointer (fminus/GuiSpec.h; GuiItems.h's PointerSink): a pool of kPointers GuiPointerItems, appended last so the
// owner hits them after every other item (a control in a container with pointer properties takes its own presses),
// the innermost element's first; each area's, container's, label's and display's with pointer properties, its rect the
// element's; a shader's are its own item's. A display under them passes presses by (DisplayItem::setPassThrough).
//
// Message thread only, as the items.
#pragma once

#include "editor/GuiItems.h"
#include "editor/GuiLayout.h"
#include "editor/Items.h"
#include "editor/Scroll.h"

#include "dsp/Controls.h"
#include "dsp/Meters.h"
#include "dsp/Sources.h"
#include "fminus/PanelSpec.h"

#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/params/ParamPort.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace playground::ui
{
    struct DisplayFeed;                                    // DisplayItem.h

    class GuiView
    {
    public:
        using Items = std::vector<std::unique_ptr<Item>>;

        static constexpr int kPool = 8;                    // tabs, pads and envelopes a panel draws
        static constexpr int kKeyboards = 2;
        static constexpr int kShaders = 4;                 // kMaxGuiShaders
        static constexpr int kPointers = kMaxPointers;     // areas and other elements with pointer properties
        static constexpr float kScrollGutter = 8.0f;       // the scroll bar's room, at the area's right
        static constexpr float kRow = 24.0f;               // the rows three of which a wheel's notch scrolls
        static constexpr float wheelPx(bool smooth) noexcept { return funkgui::scroll::wheelPx(1.0f, smooth, kRow); }

        struct Hooks
        {
            std::function<funkgui::Theme()> menuTheme;               // a menu control's popup
            std::function<bool(const funkgui::Rect&)> menuOpen;      // ... open from that box (the Panel's menuOpenAt)
            std::function<funkgui::HostServices*()> menuHost;        // ... opened by a key or a screen reader from it
            std::function<void(int node, int page)> choosePage;      // a tab clicked
            std::function<void(int note, float velocity)> playNote;  // a keyboard's key (velocity 0: let go)
            std::function<void(bool pitch, float value)> playWheel;  // its wheels
            std::function<void(int slot, funkgui::Point at)> dialMenu;  // a control's right-click (GuiControlItem::setMenu)
        };

        // Its items, appended to `items` in this order (the Tab order): kPool tabs, a control a port (kMaxControls at
        // most), kPool pads, kPool envelopes, kKeyboards keyboards, kShaders shaders (four ids each: its own, its
        // pointer's x, y and down), kPointers pointer items (three ids each), each with the ids `idFor` gives as it is
        // made. `displays`: the owner's display items (kMaxDisplays, DisplayItem), placed while the interface
        // shows them.
        GuiView(Items& items, std::span<funkgui::ParamPort* const> ports, std::vector<int> displays,
                const std::function<uint32_t()>& idFor, Hooks hooks);

        GuiView(const GuiView&) = delete;
        GuiView& operator=(const GuiView&) = delete;

        void setControls(const Controls& controls, const PanelSpec& spec);
        // MIDI learn's on slot's control (GuiControlItem::setMidi).
        void setMidi(int slot, bool mapped, bool learning, std::string spec);
        void layout(const PanelSpec& spec, const Controls& controls, funkgui::Rect area);
        void clear(const PanelSpec& spec);
        void place(const PanelSpec& spec);
        // `files`: the program's (a shader's file is one of them, read for whether its code moves); `notes`: the
        // compile's, which say why a shader was not built.
        void registerShaders(const PanelSpec& spec, const Sources& files, const std::string& notes);
        // `shown`: the meters as the displays take them; `live`: they are this interface's program's (else the controls
        // show no live= mark); `liveTaps`: taps whose live= marks show even so (bit t: tap t; a picture's state).
        void tick(const PanelSpec& spec, const Meters& shown, bool live, uint32_t liveTaps = 0);
        // A picture's pointer: x, y, down, hover (pointer.x ... in a shader's inputs), each NaN for the pointer's own.
        void showPointer(const std::array<float, 4>& pointer) noexcept { shownPointer_ = pointer; }

        // Drawing: the ground, the frames, the labels and the dividers (drawGuiPanel, in the area); the theme the items
        // are drawn in (guiTheme); the scroll bars, in the gutter and under the area (in `th`, the panel's).
        funkgui::Theme theme(const funkgui::Theme& th, const PanelSpec& spec) const;
        void drawGround(funkgui::Canvas& c, const funkgui::Theme& gth, const PanelSpec& spec, funkgui::Tag tag) const;
        void drawScrollBars(funkgui::Canvas& c, const funkgui::Theme& th) const;
        bool owns(std::size_t item) const noexcept { return item < mine_.size() && mine_[item]; }

        // The layout, and what it keeps: each tabs node's page (by node), the scroll.
        const GuiLayout& places() const noexcept { return gui_; }
        const std::vector<int>& pages() const noexcept { return pages_; }
        void setPages(std::vector<int> pages) { pages_ = std::move(pages); }
        // A tabs node's page chosen: false when it was already (nothing to lay out again).
        bool choosePage(const PanelSpec& spec, int node, int page);
        float scrollX() const noexcept { return scrollX_; }
        float scrollY() const noexcept { return scrollY_; }
        void resetScroll() noexcept { scrollX_ = scrollY_ = 0.0f; }
        // Scrolled by (dx, dy) px, clamped: false when it did not move. The items of `spec` (the one laid out) move by
        // the scroll, rounded to whole px, as the layout placed them at no scroll.
        bool scrollBy(const PanelSpec& spec, float dx, float dy);
        // The bars (Scroll.h): a press on one starts its thumb's drag (a press on its track jumps first), true when it
        // was on one; a drag moves the scroll as scrollBy does, true when it moved.
        bool pressBar(const PanelSpec& spec, funkgui::Point p);
        bool dragBar(const PanelSpec& spec, funkgui::Point p);
        void releaseBar() noexcept;
        bool barDragging() const noexcept { return barX_.dragging() || barY_.dragging(); }
        Scroll scroller(bool horizontal) const noexcept;   // the scroll on that axis, as Scroll.h has it
        funkgui::Rect barArea(bool horizontal) const noexcept;

        // The items, by what they are.
        int controlItem(int slot) const;                   // slot's control's index in the owner's list, -1 for none
        // A control, or an item whose pointer properties take the wheel (wheel=): the wheel over it is its own, and
        // does not scroll the interface.
        bool isControl(int item) const;
        const std::vector<int>& pads() const noexcept { return pads_; }
        const std::vector<int>& envelopes() const noexcept { return envelopes_; }
        const std::vector<int>& tabs() const noexcept { return tabs_; }
        const std::vector<int>& keyboards() const noexcept { return keyboards_; }
        const std::vector<int>& shaders() const noexcept { return shaders_; }
        const std::vector<int>& pointers() const noexcept { return pointers_; }   // GuiPointerItem
        const std::vector<int>& displays() const noexcept { return displays_; }
        void releaseKeys();                                // a note held is let go, a press of the pointer's ended
                                                           // (the editor closes)

    private:
        GuiControlItem& control(int slot);
        std::array<GuiValue*, 5> pointerValues(const PanelPointer& p);   // x, y, down, wheel, hover's values
        void scrolled();                                   // gui_ from content_, moved by the scroll

        Items& items_;
        std::vector<int> displays_, controls_, tabs_, pads_, envelopes_, keyboards_, shaders_, pointers_;
        std::function<void(int, funkgui::Point)> dialMenu_;   // Hooks::dialMenu, a pointer's right-click
        static constexpr float kNoPointer = std::numeric_limits<float>::quiet_NaN();
        std::array<float, 4> shownPointer_{ kNoPointer, kNoPointer, kNoPointer, kNoPointer };
        std::vector<int> shaderNode_;                      // by shaders_: the node it shows (-1: none)
        // The shaders' programs, by PanelSpec::shaders: the id registered with FunkGui (0: none), why not, and whether
        // its code reads the time (it then moves every frame).
        std::vector<uint32_t> shaderIds_;
        std::vector<std::string> shaderWhy_;
        std::vector<bool> shaderMoves_;
        std::vector<bool> mine_;                           // by item: drawn in the area, with its theme
        // The layout at no scroll, each tabs node's tabs in it (by node), and what is shown: that moved by the scroll,
        // shownX_ and shownY_ (the scroll rounded to whole px).
        GuiLayout content_;
        std::vector<std::vector<funkgui::Rect>> contentTabs_;
        GuiLayout gui_;
        std::vector<std::vector<funkgui::Rect>> shownTabs_;
        float shownX_ = 0.0f, shownY_ = 0.0f;
        std::vector<int> pages_;                           // by node
        float scrollX_ = 0.0f, scrollY_ = 0.0f;
        Scroll barX_, barY_;                               // their thumbs' drags (the offsets: scrollX_, scrollY_)
    };

    // A frame of the displays `items` (DisplayItem) at `displays`: `shown` and, for an audio scope or spectrum, the last
    // DisplayItem::kScopeSamples samples `scope` fills (input or output, oldest first, read once for every display of
    // that source), and a spectrum's source's Spectrum, dt seconds. `feed` is the caller's, kept between frames.
    void updateDisplays(GuiView::Items& items, const std::vector<int>& displays, const Meters& shown,
                        const std::function<void(bool input, float* out, int n)>& scope, DisplayFeed& feed, float dt);
}
