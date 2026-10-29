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
//                       scroll kept (clamped), then every item placed (place())
//   clear()             no interface: every item hidden
//   setControls()       each slot's dial (its Control, label and options' names) for the controls
//   registerShaders()   the spec's shaders' programs given to FunkGui's registry (once built: Shaders.h), or why not
//   tick()              a frame: the controls' live= marks and the shaders' inputs, from the meters
//
// Message thread only, as the items.
#pragma once

#include "editor/GuiItems.h"
#include "editor/GuiLayout.h"
#include "editor/Items.h"

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
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace playground::ui
{
    class GuiView
    {
    public:
        using Items = std::vector<std::unique_ptr<Item>>;

        static constexpr int kPool = 8;                    // tabs, pads and envelopes a panel draws
        static constexpr int kKeyboards = 2;
        static constexpr int kShaders = 4;                 // kMaxGuiShaders
        static constexpr float kScrollGutter = 8.0f;       // the scroll bar's room, at the area's right

        struct Hooks
        {
            std::function<funkgui::Theme()> menuTheme;               // a menu control's popup
            std::function<void(int node, int page)> choosePage;      // a tab clicked
            std::function<void(int note, float velocity)> playNote;  // a keyboard's key (velocity 0: let go)
            std::function<void(bool pitch, float value)> playWheel;  // its wheels
        };

        // Its items, appended to `items` in this order (the Tab order): kPool tabs, a control a port (kMaxControls at
        // most), kPool pads, kPool envelopes, kKeyboards keyboards, kShaders shaders, each with the id `idFor` gives as
        // it is made. `displays`: the owner's display items (kMaxDisplays, DisplayItem), placed while the interface
        // shows them.
        GuiView(Items& items, std::span<funkgui::ParamPort* const> ports, std::vector<int> displays,
                const std::function<uint32_t()>& idFor, Hooks hooks);

        GuiView(const GuiView&) = delete;
        GuiView& operator=(const GuiView&) = delete;

        void setControls(const Controls& controls, const PanelSpec& spec);
        void layout(const PanelSpec& spec, const Controls& controls, funkgui::Rect area);
        void clear(const PanelSpec& spec);
        void place(const PanelSpec& spec);
        // `files`: the program's (a shader's file is one of them, read for whether its code moves); `notes`: the
        // compile's, which say why a shader was not built.
        void registerShaders(const PanelSpec& spec, const Sources& files, const std::string& notes);
        // `shown`: the meters as the displays take them; `live`: they are this interface's program's (else the controls
        // show no live= mark).
        void tick(const PanelSpec& spec, const Meters& shown, bool live);

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
        // Scrolled by (dx, dy) px, clamped: false when it did not move.
        bool scrollBy(float dx, float dy);

        // The items, by what they are.
        int controlItem(int slot) const;                   // slot's control's index in the owner's list, -1 for none
        bool isControl(int item) const;
        const std::vector<int>& pads() const noexcept { return pads_; }
        const std::vector<int>& envelopes() const noexcept { return envelopes_; }
        const std::vector<int>& tabs() const noexcept { return tabs_; }
        const std::vector<int>& keyboards() const noexcept { return keyboards_; }
        const std::vector<int>& shaders() const noexcept { return shaders_; }
        const std::vector<int>& displays() const noexcept { return displays_; }
        void releaseKeys();                                // a note held is let go (the editor closes)

    private:
        GuiControlItem& control(int slot);

        Items& items_;
        std::vector<int> displays_, controls_, tabs_, pads_, envelopes_, keyboards_, shaders_;
        std::vector<int> shaderNode_;                      // by shaders_: the node it shows (-1: none)
        // The shaders' programs, by PanelSpec::shaders: the id registered with FunkGui (0: none), why not, and whether
        // its code reads the time (it then moves every frame).
        std::vector<uint32_t> shaderIds_;
        std::vector<std::string> shaderWhy_;
        std::vector<bool> shaderMoves_;
        std::vector<bool> mine_;                           // by item: drawn in the area, with its theme
        GuiLayout gui_;
        std::vector<int> pages_;                           // by node
        float scrollX_ = 0.0f, scrollY_ = 0.0f;
    };

    // A frame of the displays `items` (DisplayItem) at `displays`: `shown` and, for an audio scope or spectrum, the last
    // DisplayItem::kScopeSamples samples `scope` fills (input or output, oldest first), dt seconds. `in` and `out` are
    // the caller's buffers, kept between frames.
    void updateDisplays(GuiView::Items& items, const std::vector<int>& displays, const Meters& shown,
                        const std::function<void(bool input, float* out, int n)>& scope, std::vector<float>& in,
                        std::vector<float>& out, float dt);
}
