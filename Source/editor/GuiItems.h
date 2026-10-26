// Source/editor/GuiItems.h: what a .gui file's interface is made of on the Panel (fminus/GuiSpec.h, GuiLayout.h).
//
//   GuiControlItem   a parameter's knob (small, medium, large; modern or vintage), slider (vertical or horizontal),
//                    switch, buttons (a column or a row) or menu. A drag moves a knob (up raises it, 200 px the whole
//                    range, Shift ten times finer) and a slider (along its track, from where it is grabbed); the wheel
//                    and the arrow keys step it (a hundredth, Shift a tenth; an int's or a bool's a value), Home and End
//                    go to its ends, a double-click returns to the parameter's default; a switch flips and a button
//                    chooses on a click (Return and Space step them), a menu opens on a click. One gesture each.
//   GuiXyItem        a pad that moves two parameters at once: x to the right, y up
//   GuiEnvelopeItem  the ADSR shape four parameters make, drawn (a readout)
//   GuiTabsItem      the tabs over a tabs element's pages: a click shows that page
// and drawGuiPanel, which draws what is no item: the panel's ground, the sections' and boxes' frames, the labels and
// the dividers. Every item is drawn inside the panel's area (the Panel clips it) and takes input only there.
#pragma once

#include "editor/GuiLayout.h"
#include "editor/Items.h"

#include "dsp/Controls.h"
#include "fminus/PanelSpec.h"

#include <funkgui/params/ParamPort.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace testingplaygroundsynth::ui
{
    // A parameter as a .gui control shows it: its Control, its label and its values' names.
    class GuiValue
    {
    public:
        explicit GuiValue(funkgui::ParamPort& port) : port_(port) {}

        void set(const Control& c, const std::string& label, const std::vector<std::string>& options);
        const Control& control() const noexcept { return control_; }
        const std::string& label() const noexcept { return label_; }
        funkgui::ParamPort& port() noexcept { return port_; }

        float value01() const;                   // the host's value, 0..1 (the last finite one)
        float plain() const { return plainOf(control_, value01()); }
        float default01() const { return normOf(control_, control_.def); }
        bool atDefault() const { return std::fabs(plain() - control_.def) < 1e-6f; }
        int count() const;                       // an int's or a bool's values (0 for a float)
        int index() const;                       // which one it is at
        std::string nameOf(int k) const;
        float normOfIndex(int k) const { return normOf(control_, control_.min + static_cast<float>(k)); }
        std::string text() const;                // the value as the panel shows it
        float step() const;                      // a wheel notch's or an arrow key's change, 0..1

    private:
        funkgui::ParamPort& port_;
        Control control_{};
        std::string label_;
        std::vector<std::string> options_;
        mutable float last01_ = 0.5f;
    };

    class GuiControlItem final : public Item
    {
    public:
        GuiControlItem(funkgui::ParamPort& port, uint32_t id, std::function<funkgui::Theme()> menuTheme)
            : value_(port), id_(id), menuTheme_(std::move(menuTheme)) {}

        GuiValue& value() noexcept { return value_; }
        // Its element (its kind, size, direction and style), its cell, the panel's area, whether it is on the panel,
        // and its colour (its own, else the panel's accent, else "").
        void place(const GuiNode& node, const funkgui::Rect& cell, const funkgui::Rect& area, bool shown,
                   const std::string& color);
        GuiKind kind() const noexcept { return kind_; }
        const funkgui::Rect& cell() const noexcept { return cell_; }

        void tick(float dt, bool hovered, bool focused, funkgui::Point pointer) override;
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override;
        bool contains(funkgui::Point p) const override { return shown_ && area_.contains(p) && cell_.contains(p); }
        funkgui::Cursor cursorAt(funkgui::Point p) const override;
        void pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController& g, funkgui::HostServices& h) override;
        void pointerDrag(const funkgui::PointerEvent& e, funkgui::GestureController& g) override;
        void pointerUp(const funkgui::PointerEvent& e, funkgui::GestureController& g) override;
        void doubleClick(const funkgui::PointerEvent& e, funkgui::GestureController& g) override;
        bool wheel(const funkgui::WheelEvent& e, funkgui::GestureController& g, double now) override;
        bool key(const funkgui::KeyEvent& e, funkgui::GestureController& g) override;
        void accessibility(std::vector<funkgui::A11yItem>& out) const override;
        bool a11yAction(uint32_t id, funkgui::A11yAction a, double v, funkgui::GestureController& g) override;
        void specLine(char* out, std::size_t n) const override;
        bool settled() const override { return !dragging_; }
        funkgui::Point centre() const override { return { cell_.centreX(), cell_.centreY() }; }
        bool shown() const override { return shown_ && value_.control().used; }

        // For probes and the Panel: the button a point is over (buttons), else -1; the slider's track.
        int buttonAt(funkgui::Point p) const;
        funkgui::Rect track() const;

    private:
        bool clicks() const noexcept { return kind_ == GuiKind::toggle || kind_ == GuiKind::buttons || kind_ == GuiKind::menu; }
        void openMenu(funkgui::GestureController& g, funkgui::HostServices& h);
        float sliderValueAt(funkgui::Point p) const;

        GuiValue value_;
        uint32_t id_;
        std::function<funkgui::Theme()> menuTheme_;
        GuiKind kind_ = GuiKind::knob;
        GuiSize size_ = GuiSize::medium;
        bool vertical_ = true, vintage_ = false;
        std::string color_;
        funkgui::Rect cell_, area_;
        bool shown_ = false;
        bool hovered_ = false, dragging_ = false;
        int hoverButton_ = -1;
        float dragStart01_ = 0.0f;                   // the value the drag has reached
        float lastY_ = 0.0f;
    };

    class GuiXyItem final : public Item
    {
    public:
        explicit GuiXyItem(uint32_t id) : id_(id) {}

        // Its parameters (the controls' values of its x and y slots: they change with the program), null for none.
        void bind(GuiValue* x, GuiValue* y)
        {
            vx_ = x;
            vy_ = y;
        }
        void place(const GuiNode& node, const funkgui::Rect& cell, const funkgui::Rect& area, bool shown,
                   const std::string& color);
        funkgui::Rect pad() const;               // its square, under its label

        void tick(float, bool hovered, bool, funkgui::Point) override { hovered_ = hovered; }
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override;
        bool contains(funkgui::Point p) const override { return shown() && area_.contains(p) && pad().contains(p); }
        funkgui::Cursor cursorAt(funkgui::Point) const override { return funkgui::Cursor::crosshair; }
        void pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController& g, funkgui::HostServices&) override;
        void pointerDrag(const funkgui::PointerEvent& e, funkgui::GestureController& g) override;
        void pointerUp(const funkgui::PointerEvent&, funkgui::GestureController&) override { dragging_ = false; }
        void doubleClick(const funkgui::PointerEvent&, funkgui::GestureController& g) override;
        bool key(const funkgui::KeyEvent& e, funkgui::GestureController& g) override;
        void accessibility(std::vector<funkgui::A11yItem>& out) const override;
        bool a11yAction(uint32_t id, funkgui::A11yAction a, double v, funkgui::GestureController& g) override;
        void specLine(char* out, std::size_t n) const override;
        bool settled() const override { return !dragging_; }
        funkgui::Point centre() const override { return { cell_.centreX(), cell_.centreY() }; }
        bool shown() const override { return shown_ && vx_ != nullptr && vy_ != nullptr; }

    private:
        void moveTo(funkgui::Point p, funkgui::GestureController& g);

        uint32_t id_;
        GuiValue* vx_ = nullptr;
        GuiValue* vy_ = nullptr;
        std::string label_, color_;
        funkgui::Rect cell_, area_;
        bool shown_ = false, hovered_ = false, dragging_ = false;
    };

    class GuiEnvelopeItem final : public Item
    {
    public:
        explicit GuiEnvelopeItem(uint32_t id) : id_(id) {}

        void bind(std::array<GuiValue*, 4> adsr) { adsr_ = adsr; }
        void place(const GuiNode& node, const funkgui::Rect& cell, const funkgui::Rect& area, bool shown,
                   const std::string& color);
        // The shape's corners (x, y) across its cell, for the probes: start, peak, sustain's start, its end, the end.
        std::array<funkgui::Point, 5> corners() const;

        void tick(float, bool hovered, bool, funkgui::Point) override { hovered_ = hovered; }
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override;
        bool contains(funkgui::Point p) const override { return shown() && area_.contains(p) && cell_.contains(p); }
        void pointerDown(const funkgui::PointerEvent&, funkgui::GestureController&, funkgui::HostServices&) override {}
        void accessibility(std::vector<funkgui::A11yItem>& out) const override;
        bool a11yAction(uint32_t, funkgui::A11yAction, double, funkgui::GestureController&) override { return false; }
        void specLine(char* out, std::size_t n) const override;
        funkgui::Point centre() const override { return { cell_.centreX(), cell_.centreY() }; }
        bool shown() const override;
        bool focusable() const override { return false; }

    private:
        uint32_t id_;
        std::array<GuiValue*, 4> adsr_{};
        std::string label_, color_;
        funkgui::Rect cell_, area_;
        bool shown_ = false, hovered_ = false;
    };

    class GuiTabsItem final : public Item
    {
    public:
        GuiTabsItem(uint32_t id, std::function<void(int node, int page)> choose) : id_(id), choose_(std::move(choose)) {}

        // Its tabs node, its rect, its tabs' rects and titles, the page shown, the panel's area, and its colour.
        void place(int node, const funkgui::Rect& rect, std::vector<funkgui::Rect> tabs, std::vector<std::string> titles,
                   int page, const funkgui::Rect& area, bool shown, const std::string& color);
        int node() const noexcept { return node_; }
        const std::vector<funkgui::Rect>& tabs() const noexcept { return tabs_; }

        void tick(float, bool hovered, bool, funkgui::Point p) override;
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override;
        bool contains(funkgui::Point p) const override { return shown_ && area_.contains(p) && tabAt(p) >= 0; }
        funkgui::Cursor cursorAt(funkgui::Point) const override { return funkgui::Cursor::pointingHand; }
        void pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController&, funkgui::HostServices&) override;
        bool key(const funkgui::KeyEvent& e, funkgui::GestureController&) override;
        void accessibility(std::vector<funkgui::A11yItem>& out) const override;
        bool a11yAction(uint32_t id, funkgui::A11yAction a, double v, funkgui::GestureController&) override;
        void specLine(char* out, std::size_t n) const override;
        funkgui::Point centre() const override;
        bool shown() const override { return shown_; }

    private:
        int tabAt(funkgui::Point p) const;

        uint32_t id_;
        std::function<void(int, int)> choose_;
        int node_ = -1, page_ = 0, hover_ = -1;
        funkgui::Rect rect_, area_;
        std::vector<funkgui::Rect> tabs_;
        std::vector<std::string> titles_;
        std::string color_;
        bool shown_ = false;
    };

    // The panel's ground (the .gui's background), the frames of its sections and boxes, its labels and dividers, those
    // on the pages shown and in its area.
    void drawGuiPanel(funkgui::Canvas& c, const funkgui::Theme& th, const PanelSpec& spec, const GuiLayout& layout);

    // The theme the panel's items are drawn with: the Panel's, or, with a background=, one whose inks suit it (light on
    // a dark ground, dark on a light one) on that ground; with an accent=, that accent.
    funkgui::Theme guiTheme(const funkgui::Theme& th, const GuiTree& gui);

    // An analyser's curve (DisplayItem: a spectrum), `db` across its cell from 20 Hz to 20 kHz, -84 to 0 dB.
    void drawSpectrum(funkgui::Canvas& c, const funkgui::Theme& th, const funkgui::Rect& cell, const char* label,
                      const float* db, int n, funkgui::Col color, bool tinted);
}
