// Source/editor/Panel.h: the plugin's interface, a funkgui::Panel. It is GPU-free: funkgui::EditorHost runs it live
// in the plugin (Source/editor/gpu/CreateEditorGpu.cpp) and funkgui::HeadlessHost runs it in the probes (gui.dump),
// drawing the same frames.
//
// It builds itself from the parameter table (Source/dsp/Params.h), in table order:
//   header    the product name, its kind and version, and the THEME cells (GRAPHITE, PAPER). The panel widens for a
//             long name (up to a limit) so the whole header fits; past it the kind and version are left out, and a
//             name that still does not fit ends in an ellipsis: the header text never runs into the THEME cells
//   envelope  ENVELOPE: a picture of the ADSR (EnvelopeView.h), when the table has the four envelope rows
//   controls  each row's control, left to right, wrapping at the panel's width: Control::dial a funkgui::Dial
//             (60 x 80), Control::slider a funkgui::RuleSlider (180 px rule), Control::choice a row of its own with
//             one cell per choice (a funkgui::SegmentedSelector over funkgui::ParamCells); Control::hidden nothing. A
//             row of two or more dials and sliders that fills more than half the width spreads out to the full width
//   footer    the spec line of the control under the hand (or focused, or last touched), and the ZOOM cells
// so a new table row appears with no editor change. For a custom view (a graph, a meter), add an Item in Panel.cpp
// and place it in the constructor, or draw it as the envelope is drawn; README.md walks through it.
//
// Input: pointer, wheel, double-click (the default) and keys (Tab moves the focus ring over the controls; arrows,
// Page and Home/End adjust; Esc hides the ring) reach the item under the pointer or in focus. Every parameter write
// goes through one funkgui::GestureController (begin/set/end gestures for the host). A right-click (or ctrl-click)
// on a control opens the host's parameter menu.
//
// Message thread only. The ports (one per table row, table order) belong to the processor and outlive the Panel.
#pragma once

#include "editor/EnvelopeView.h"

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/panel/Input.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/params/GestureController.h>
#include <funkgui/params/ParamPort.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace funkgui
{
    class Canvas;
    class CellModel;
    class HostServices;
    struct Theme;
}

namespace testingplaygroundsynth::ui
{
    class Item;                                          // one control on the panel (Panel.cpp)

    // The panel's own primitive tags, for probes that find what they check by tag (FunkGui's tags end below
    // funkgui::tags::firstProduct; a view of your own takes the next free one: EnvelopeView.h's kEnvelopeTag is
    // firstProduct + 1).
    inline constexpr funkgui::Tag kHeaderTextTag = funkgui::tags::firstProduct;   // the name, its kind and version

    class Panel final : public funkgui::Panel
    {
    public:
        // `name` is the product's (Product.h) unless given: probes try the header with other names.
        explicit Panel(std::span<funkgui::ParamPort* const> ports, std::string_view name = {});
        ~Panel() override;

        Panel(const Panel&) = delete;
        Panel& operator=(const Panel&) = delete;

        // For probes: the centre of the control bound to parameter `param` (nullopt for a hidden one), and the number
        // of controls (parameters with a control, plus the THEME and ZOOM cells).
        std::optional<funkgui::Point> centreOf(std::size_t param) const;
        std::size_t itemCount() const noexcept { return items_.size(); }
        // For probes: the envelope view (nullptr when the table lacks an envelope row) and the parameter whose
        // envelope segment is highlighted (-1 for none).
        const EnvelopeView* envelope() const noexcept { return envelope_ ? &*envelope_ : nullptr; }
        int highlightedParam() const;

        // For probes: where the header's parts end, in logical px from the panel's left edge, as draw() places them.
        struct HeaderLayout
        {
            float titleRight = 0.0f;                     // the name's right edge
            float subtitleRight = 0.0f;                  // the kind and version's (0 when left out)
            float themeLeft = 0.0f;                      // the THEME cells' left edge
            bool  subtitleShown = false;
            bool  titleCut = false;                      // the name ends in an ellipsis
            bool  widest = false;                        // the panel is as wide as it grows for the header
        };
        const HeaderLayout& header() const noexcept { return header_; }

        // funkgui::Panel
        void  attach(funkgui::HostServices&) override;
        int   width() const override;
        int   height() const override;
        void  tick(float dt) override;
        void  idle(double nowSec) override;
        void  draw(funkgui::Canvas&, const funkgui::Theme&) override;
        bool  wantsFullRate() const override;
        void  pointerMove(const funkgui::PointerEvent&) override;
        void  pointerExit() override;
        void  pointerDown(const funkgui::PointerEvent&) override;
        void  pointerDrag(const funkgui::PointerEvent&) override;
        void  pointerUp(const funkgui::PointerEvent&) override;
        void  doubleClick(const funkgui::PointerEvent&) override;
        bool  wheel(const funkgui::WheelEvent&) override;
        bool  key(const funkgui::KeyEvent&) override;
        funkgui::Cursor cursor() const override;
        void  accessibility(std::vector<funkgui::A11yItem>&) const override;
        uint32_t a11yRevision() const override;
        void  a11yAction(uint32_t id, funkgui::A11yAction, double value = 0) override;
        void  closeGestures() override;

    private:
        int  itemAt(funkgui::Point) const;               // the item under p, -1 for none
        bool moveFocus(int dir);

        std::vector<std::unique_ptr<Item>> items_;       // controls in table order, then THEME, then ZOOM
        std::vector<int> itemOfParam_;                   // table index -> item index, -1 for a hidden parameter
        std::optional<EnvelopeView> envelope_;
        float width_ = 0.0f, height_ = 0.0f;
        std::string title_;                              // the product name, upper case (cut to fit: header_)
        std::string subtitle_;                           // "INSTRUMENT  0.1.0"
        HeaderLayout header_;

        funkgui::CellModel* zoom_ = nullptr;             // the ZOOM cells' model (the last item owns it)
        funkgui::HostServices* host_ = nullptr;
        std::optional<funkgui::GestureController> gestures_;
        funkgui::Point pointer_{};
        bool hasPointer_ = false;
        bool ring_ = false;                              // the focus ring is shown (keyboard use)
        bool dirty_ = true;                              // an input event since the last tick
        int  pressed_ = -1;                              // the item holding the pointer between down and up
        int  focus_ = -1;
        int  touched_ = -1;                              // the last item used, for the spec line
    };
}
