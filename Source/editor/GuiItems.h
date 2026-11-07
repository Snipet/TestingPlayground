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
//   GuiKeyboardItem  an instrument's keys: a click plays a note, a drag slides across them
//   GuiShaderItem    a shader of the program's own, drawn by the GPU
//   GuiTabsItem      the tabs over a tabs element's pages (a strip, a bar, pills or a side list): a click shows that
//                    page
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

namespace playground::ui
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
        // `menuOpen`: whether a menu opened from this rect is open (the Panel's menuOpenAt; none in the GUI sandbox): a
        // menu control's box draws open while its menu is (Dropdown.h). `menuHost`: the host a key (Return, Space) or a
        // screen reader's press opens a menu control's menu from, as a click does (none in the GUI sandbox: they step
        // its value there).
        GuiControlItem(funkgui::ParamPort& port, uint32_t id, std::function<funkgui::Theme()> menuTheme,
                       std::function<bool(const funkgui::Rect&)> menuOpen = {},
                       std::function<funkgui::HostServices*()> menuHost = {})
            : value_(port), id_(id), menuTheme_(std::move(menuTheme)), menuOpen_(std::move(menuOpen)),
              menuHost_(std::move(menuHost)) {}

        GuiValue& value() noexcept { return value_; }
        // Its element (its kind, size, direction and style), its cell, the panel's area, whether it is on the panel,
        // and its colour (its own, else the panel's accent, else "").
        void place(const GuiNode& node, const funkgui::Rect& cell, const funkgui::Rect& area, bool shown,
                   const std::string& color);
        GuiKind kind() const noexcept { return kind_; }
        const funkgui::Rect& cell() const noexcept { return cell_; }
        // Its live= field (a hidden display's index, -1 for none) and that field's value now, in the parameter's units.
        int liveDisplay() const noexcept { return liveDisplay_; }
        void setLive(float plain);
        float live01() const noexcept { return live01_; }           // -1 for none
        const GuiStyle& style() const noexcept { return style_; }
        const GuiKnobGeom& knobGeom() const noexcept { return geom_; }

        void tick(float dt, bool hovered, bool focused, funkgui::Point pointer) override;
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override;
        uint64_t drawKey() const override                // its value (a host's automation too), its live= mark, its menu
        {
            return DrawKey{}.mix(value_.value01()).mix(live01_).mix(hovered_).mix(hoverButton_).mix(dragging_)
                .mix(menuOpenNow()).h;
        }
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
        // Moving while dragged, and while its value or its live= mark moves with no hand on it (Stirred).
        bool settled() const override { return !dragging_ && (!shown() || !stir_.moving()); }
        funkgui::Point centre() const override { return { cell_.centreX(), cell_.centreY() }; }
        bool shown() const override { return shown_ && value_.control().used; }

        // For probes and the Panel: the button a point is over (buttons), else -1; the slider's track; a menu's box
        // (its trigger, under its label: the menu's anchor).
        int buttonAt(funkgui::Point p) const;
        funkgui::Rect track() const;
        funkgui::Rect menuBox() const noexcept;

    private:
        bool clicks() const noexcept { return kind_ == GuiKind::toggle || kind_ == GuiKind::buttons || kind_ == GuiKind::menu; }
        void drawKnob(funkgui::Canvas& c, const funkgui::Theme& th, bool hot) const;
        void drawSlider(funkgui::Canvas& c, const funkgui::Theme& th, bool hot) const;
        void openMenu(funkgui::GestureController& g, funkgui::HostServices& h);
        bool openMenuFromHost(funkgui::GestureController& g);   // a menu control's, from menuHost_: false when none
        bool menuOpenNow() const { return kind_ == GuiKind::menu && menuOpen_ && menuOpen_(menuBox()); }
        float sliderValueAt(funkgui::Point p) const;

        GuiValue value_;
        uint32_t id_;
        std::function<funkgui::Theme()> menuTheme_;
        std::function<bool(const funkgui::Rect&)> menuOpen_;
        std::function<funkgui::HostServices*()> menuHost_;
        GuiKind kind_ = GuiKind::knob;
        GuiSize size_ = GuiSize::medium;
        bool vertical_ = true, vintage_ = false;
        GuiStyle style_;
        GuiKnobGeom geom_;
        int liveDisplay_ = -1;
        float live01_ = -1.0f;
        std::string color_;
        funkgui::Rect cell_, area_;
        bool shown_ = false;
        bool hovered_ = false, dragging_ = false;
        int hoverButton_ = -1;
        float dragStart01_ = 0.0f;                   // the value the drag has reached
        float lastY_ = 0.0f;
        Stirred stir_;
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

        void tick(float dt, bool hovered, bool, funkgui::Point) override
        {
            hovered_ = hovered;
            stir_.tick(dt, { vx_ != nullptr ? vx_->value01() : -1.0f, vy_ != nullptr ? vy_->value01() : -1.0f });
        }
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override;
        uint64_t drawKey() const override
        {
            return DrawKey{}
                .mix(vx_ != nullptr ? vx_->value01() : -1.0f)
                .mix(vy_ != nullptr ? vy_->value01() : -1.0f)
                .mix(hovered_)
                .mix(dragging_)
                .h;
        }
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
        bool settled() const override { return !dragging_ && (!shown() || !stir_.moving()); }   // as a control's
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
        Stirred stir_;
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

        void tick(float dt, bool hovered, bool, funkgui::Point) override
        {
            hovered_ = hovered;
            const auto at = [this](std::size_t k) { return adsr_[k] != nullptr ? adsr_[k]->value01() : -1.0f; };
            stir_.tick(dt, { at(0), at(1), at(2), at(3) });
        }
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override;
        uint64_t drawKey() const override                // the four parameters' values
        {
            DrawKey k;
            for (const GuiValue* v : adsr_)
                k.mix(v != nullptr ? v->value01() : -1.0f);
            return k.mix(hovered_).h;
        }
        bool contains(funkgui::Point p) const override { return shown() && area_.contains(p) && cell_.contains(p); }
        void pointerDown(const funkgui::PointerEvent&, funkgui::GestureController&, funkgui::HostServices&) override {}
        void accessibility(std::vector<funkgui::A11yItem>& out) const override;
        bool a11yAction(uint32_t, funkgui::A11yAction, double, funkgui::GestureController&) override { return false; }
        void specLine(char* out, std::size_t n) const override;
        bool settled() const override { return !shown() || !stir_.moving(); }   // its parameters moving (Stirred)
        funkgui::Point centre() const override { return { cell_.centreX(), cell_.centreY() }; }
        bool shown() const override;
        bool focusable() const override { return false; }

    private:
        uint32_t id_;
        std::array<GuiValue*, 4> adsr_{};
        std::string label_, color_;
        funkgui::Rect cell_, area_;
        bool shown_ = false, hovered_ = false;
        Stirred stir_;
    };

    // A keyboard (an instrument's): its keys, from= for keys=, across its cell. A click plays a note (louder lower on
    // the key), a drag slides from key to key, letting go releases it; `play` takes (note, velocity 0..1), 0 a release.
    // With wheels, a pitch wheel and a mod wheel at its left: a drag moves one (the pitch wheel springs back to its
    // middle when let go); `wheel` takes (pitch, value: -1..1 for the pitch wheel, 0..1 for the mod wheel).
    class GuiKeyboardItem final : public Item
    {
    public:
        GuiKeyboardItem(uint32_t id, std::function<void(int note, float velocity)> play,
                        std::function<void(bool pitch, float value)> wheel)
            : id_(id), play_(std::move(play)), wheel_(std::move(wheel)) {}

        void place(const GuiNode& node, const funkgui::Rect& cell, const funkgui::Rect& area, bool shown,
                   const std::string& color);
        int noteAt(funkgui::Point p, float* velocity = nullptr) const;     // -1 off its keys
        funkgui::Point centreOfNote(int note) const;     // a key's middle, for the probes (its cell's centre if none)
        int held() const noexcept { return held_; }      // the note held, -1 for none
        void release();                                  // lets go of it (and of the pitch wheel)
        bool wheels() const noexcept { return wheels_; }
        funkgui::Rect pitchWheel() const noexcept;       // the wheels' slots (empty without wheels)
        funkgui::Rect modWheel() const noexcept;
        float pitch() const noexcept { return pitch_; }
        float mod() const noexcept { return mod_; }

        void tick(float, bool hovered, bool, funkgui::Point p) override;
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override;
        uint64_t drawKey() const override { return DrawKey{}.mix(held_).mix(hover_).mix(pitch_).mix(mod_).mix(dragWheel_).h; }
        bool contains(funkgui::Point p) const override { return shown_ && area_.contains(p) && cell_.contains(p); }
        funkgui::Cursor cursorAt(funkgui::Point) const override { return funkgui::Cursor::pointingHand; }
        void pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController&, funkgui::HostServices&) override;
        void pointerDrag(const funkgui::PointerEvent& e, funkgui::GestureController&) override;
        void pointerUp(const funkgui::PointerEvent&, funkgui::GestureController&) override { release(); }
        void accessibility(std::vector<funkgui::A11yItem>& out) const override;
        bool a11yAction(uint32_t, funkgui::A11yAction, double, funkgui::GestureController&) override { return false; }
        void specLine(char* out, std::size_t n) const override;
        bool settled() const override { return held_ < 0 && dragWheel_ < 0; }
        funkgui::Point centre() const override { return { cell_.centreX(), cell_.centreY() }; }
        bool shown() const override { return shown_; }
        bool focusable() const override { return false; }

    private:
        struct Key
        {
            funkgui::Rect r;
            int note = 0;
            bool black = false;
        };
        void press(int note, float velocity);
        void moveWheel(float y);                         // the wheel held, to the pointer's height
        funkgui::Rect keysArea() const noexcept;         // the cell less the wheels

        uint32_t id_;
        std::function<void(int, float)> play_;
        std::function<void(bool, float)> wheel_;
        bool wheels_ = false;
        float pitch_ = 0.0f, mod_ = 0.0f;
        int dragWheel_ = -1;                             // 0 the pitch wheel, 1 the mod wheel, -1 none
        std::vector<Key> keys_;                          // the white keys, then the black (over them)
        int from_ = 48, count_ = 25;
        std::string color_;
        funkgui::Rect cell_, area_;
        bool shown_ = false;
        int held_ = -1, hover_ = -1;
    };

    // A note's name: 60 is C4.
    std::string noteName(int note);

    // A shader element (fminus/Shaders.h): its program, drawn over its cell by FunkGui (Canvas::shader) with its
    // inputs as u_params, once the Panel has registered it (funkgui/gpu/ShaderLibrary.h); until then, or when it was
    // refused, a placeholder that says why. It moves every frame while it animates (its code reads u_time:
    // shaderAnimates), else while an input moves (Stirred: what place() and setInputs() see change, kept until the next
    // tick reads it).
    class GuiShaderItem final : public Item
    {
    public:
        explicit GuiShaderItem(uint32_t id) : id_(id) {}

        // Its element, its cell, the panel's area, whether it is shown; its program's id (0: none) and file, and why
        // it draws nothing ("" when it draws), and whether it animates.
        void place(const GuiNode& node, const funkgui::Rect& cell, const funkgui::Rect& area, bool shown, uint32_t program,
                   const std::string& file, const std::string& why, bool animated);
        void setInputs(const std::array<float, kMaxShaderInputs>& v);
        const std::array<float, kMaxShaderInputs>& inputs() const noexcept { return inputs_; }
        uint32_t program() const noexcept { return program_; }
        const std::string& why() const noexcept { return why_; }
        const funkgui::Rect& cell() const noexcept { return cell_; }

        void tick(float dt, bool, bool, funkgui::Point) override
        {
            stir_.tick(dt, changed_);
            changed_ = false;
        }
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override;
        uint64_t drawKey() const override                // its inputs (u_params) and what it draws
        {
            DrawKey k;
            for (const float v : inputs_)
                k.mix(v);
            return k.mix(program_).mix(why_).mix(cell_).h;
        }
        bool contains(funkgui::Point p) const override { return shown_ && area_.contains(p) && cell_.contains(p); }
        void pointerDown(const funkgui::PointerEvent&, funkgui::GestureController&, funkgui::HostServices&) override {}
        void accessibility(std::vector<funkgui::A11yItem>& out) const override;
        bool a11yAction(uint32_t, funkgui::A11yAction, double, funkgui::GestureController&) override { return false; }
        void specLine(char* out, std::size_t n) const override;
        bool settled() const override { return !shown_ || (!(animated_ && program_ != 0) && !stir_.moving()); }
        funkgui::Point centre() const override { return { cell_.centreX(), cell_.centreY() }; }
        bool shown() const override { return shown_; }
        bool focusable() const override { return false; }

    private:
        uint32_t id_;
        uint32_t program_ = 0;
        std::string file_, why_;
        std::array<float, kMaxShaderInputs> inputs_{};
        funkgui::Rect cell_, area_;
        bool shown_ = false, animated_ = false;
        bool changed_ = false;                           // since the last tick: an input, the program, shown
        Stirred stir_;
    };

    class GuiTabsItem final : public Item
    {
    public:
        GuiTabsItem(uint32_t id, std::function<void(int node, int page)> choose) : id_(id), choose_(std::move(choose)) {}

        // Its tabs node, its rect, its tabs' rects and titles, the page shown, the panel's area, and its colour.
        void place(int node, const funkgui::Rect& rect, std::vector<funkgui::Rect> tabs, std::vector<std::string> titles,
                   int page, const funkgui::Rect& area, bool shown, const std::string& color, const GuiStyle& style);
        int node() const noexcept { return node_; }
        const std::vector<funkgui::Rect>& tabs() const noexcept { return tabs_; }

        void tick(float, bool hovered, bool, funkgui::Point p) override;
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override;
        uint64_t drawKey() const override { return DrawKey{}.mix(page_).mix(hover_).mix(rect_).h; }
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
        GuiStyle style_;                         // its look (tabs, bar, pills, side) and colours
        bool shown_ = false;
    };

    // The panel's ground (the .gui's background), the frames of its sections and boxes, its labels and dividers, those
    // on the pages shown and in its area.
    void drawGuiPanel(funkgui::Canvas& c, const funkgui::Theme& th, const PanelSpec& spec, const GuiLayout& layout);

    // The theme the panel's items are drawn with: the Panel's, or, with a background=, one whose inks suit it (light on
    // a dark ground, dark on a light one) on that ground; with an accent=, that accent.
    funkgui::Theme guiTheme(const funkgui::Theme& th, const GuiTree& gui);

    // An analyser's curve (DisplayItem: a spectrum), `db` across its cell from 20 Hz to 20 kHz, -84 to 0 dB: n points,
    // kSpectrumMostPoints at most (its points are on the stack; DisplayItem asserts that its kSpectrumPoints fit).
    constexpr int kSpectrumMostPoints = 256;
    void drawSpectrum(funkgui::Canvas& c, const funkgui::Theme& th, const funkgui::Rect& cell, const char* label,
                      const float* db, int n, funkgui::Col color, bool tinted);
}
