// Source/editor/Panel.h: the plugin's interface, a funkgui::Panel. It is GPU-free: funkgui::EditorHost runs it live
// in the plugin (Source/editor/gpu/CreateEditorGpu.cpp) and funkgui::HeadlessHost runs it in the probes (gui.dump),
// drawing the same frames.
//
// It builds itself from the parameter table (Source/dsp/Params.h), in table order:
//   header    the product name, its kind and version, and the THEME cells (GRAPHITE, PAPER)
//   controls  each row's control, left to right, wrapping at the panel's width: Control::dial a funkgui::Dial
//             (60 x 80), Control::slider a funkgui::RuleSlider (180 px rule), Control::choice a row of its own with
//             one cell per choice (a funkgui::SegmentedSelector over funkgui::ParamCells); Control::hidden nothing
//   footer    the spec line of the control under the hand (or focused, or last touched), and the ZOOM cells
// so a new table row appears with no editor change. For a custom view (a graph, a meter), add an Item in Panel.cpp
// and place it in the constructor; README.md walks through it.
//
// Input: pointer, wheel, double-click (the default) and keys (Tab moves the focus ring over the controls; arrows,
// Page and Home/End adjust; Esc hides the ring) reach the item under the pointer or in focus. Every parameter write
// goes through one funkgui::GestureController (begin/set/end gestures for the host). A right-click (or ctrl-click)
// on a control opens the host's parameter menu.
//
// Message thread only. The ports (one per table row, table order) belong to the processor and outlive the Panel.
#pragma once

#include <funkgui/a11y/A11yItem.h>
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

    class Panel final : public funkgui::Panel
    {
    public:
        explicit Panel(std::span<funkgui::ParamPort* const> ports);
        ~Panel() override;

        Panel(const Panel&) = delete;
        Panel& operator=(const Panel&) = delete;

        // For probes: the centre of the control bound to parameter `param` (nullopt for a hidden one), and the number
        // of controls (parameters with a control, plus the THEME and ZOOM cells).
        std::optional<funkgui::Point> centreOf(std::size_t param) const;
        std::size_t itemCount() const noexcept { return items_.size(); }

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
        float width_ = 0.0f, height_ = 0.0f;
        std::string title_;                              // the product name, upper case
        std::string subtitle_;                           // "EFFECT  0.1.0"

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
