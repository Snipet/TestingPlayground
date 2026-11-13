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
//   GuiShaderItem    a shader of the program's own, drawn by the GPU; the pointer over it is among its inputs
//                    (pointer.x ...), and its pointer properties write parameters as an area's do
//   GuiPointerItem   an area, or what a container, a label or a display with pointer properties takes (x=, y=,
//                    down=, wheel=, hover=: PanelPointer, PointerSink): it draws nothing (but a focus ring) and is
//                    hit after every other item, so a control inside a container still takes its own presses
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
        uint64_t drawKey() const override                // its value (a host's automation too), its live= mark, its menu,
        {                                                // its MIDI mark
            return DrawKey{}.mix(value_.value01()).mix(live01_).mix(hovered_).mix(hoverButton_).mix(dragging_)
                .mix(menuOpenNow()).mix(midiMapped_).mix(midiLearning_).mix(midiSpec_).h;
        }
        // MIDI learn (the plugin's editor, editor/Panel.cpp; none in the GUI sandbox): what a right-click, a ctrl-click
        // and a screen reader's menu action open at a point, alike, on a knob, a slider, a switch or buttons (none:
        // nothing, as before; never on a dropdown, whose menu action opens its own list as its press does), its mark
        // (looks::midiMark: a MIDI control moves it, it learns one) and what its footer line says after its own.
        void setMenu(std::function<void(funkgui::Point)> menu) { dialMenu_ = std::move(menu); }
        void setMidi(bool mapped, bool learning, std::string spec)
        {
            midiMapped_ = mapped;
            midiLearning_ = learning;
            midiSpec_ = std::move(spec);
        }
        bool midiMarked() const noexcept { return midiMapped_; }
        bool midiLearning() const noexcept { return midiLearning_; }
        // Where its mark goes: a knob's at its cell's right edge level with its face's top (under a label on top), clear
        // of the face; another kind's at its cell's top right.
        funkgui::Point midiMarkAt() const noexcept;
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
        bool dialMenuHere() const noexcept { return dialMenu_ && kind_ != GuiKind::menu && shown(); }   // setMenu's
        void drawControl(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const;
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
        std::function<void(funkgui::Point)> dialMenu_;
        bool midiMapped_ = false, midiLearning_ = false;
        std::string midiSpec_;
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

    // What the pointer does over an element (PanelPointer, fminus/GuiSpec.h): the parameters it writes and the pointer's
    // own state, which a shader reads as its pointer.* inputs. One press is one gesture of x, y and down together: each
    // port's begin at the press, its writes as it moves, its end at the release (FunkGui's GestureController holds one
    // drag at a time, so the sink brackets its ports itself, the writes of an event in one host batch, as tapMany's).
    // x and y absolute: the place across the rect (left 0, right 1) and up it (bottom 0, top 1), as host values, so a
    // knob's taper maps them; relative: by the travel, kDragRange px the range, Shift ten times finer (a knob's); spring: their
    // defaults at the release. down: 1 (its highest) while pressed, 0 at the release. A double-click puts x and y back
    // to their defaults; the wheel nudges wheel= as a knob's; a right-click opens the first parameter's dial menu. The
    // keys (a focusable element: x, y or down): the arrows move x and y by 1 % (Shift 10 %), Home puts both back, Space
    // and Return press down for kKeyHold seconds, a key's repeat keeping it pressed (FunkGui sends no key's release).
    // hover=, a panel dial, is written straight (a panel dial has no gesture) as the pointer comes and goes.
    class PointerSink
    {
    public:
        static constexpr float kDragRange = 200.0f;      // px for the whole range, relative
        static constexpr float kKeyHold = 0.3f;          // seconds a key's press holds down

        // Its properties and the values of their slots (x, y, down, wheel, hover: the controls' values, which change
        // with the program; null for none); `menu` opens a slot's dial menu at a point (none: no menu). A gesture open
        // over other ports ends first.
        void bind(const PanelPointer& p, std::array<GuiValue*, 5> values, std::function<void(int, funkgui::Point)> menu);
        void setRect(const funkgui::Rect& r) noexcept { rect_ = r; }
        const PanelPointer& pointer() const noexcept { return p_; }
        bool any() const noexcept { return p_.any(); }
        bool focusable() const noexcept { return p_.presses(); }
        bool wheels() const noexcept { return v_[kWheel] != nullptr; }

        void move(funkgui::Point at) noexcept;           // the pointer over it (pointer.x and pointer.y)
        void press(const funkgui::PointerEvent& e, funkgui::GestureController& g);
        void drag(const funkgui::PointerEvent& e, funkgui::GestureController& g);
        void release();                                  // the gesture ends (a release, the element hidden, the editor going)
        void doubleClick(funkgui::GestureController& g);
        bool wheel(const funkgui::WheelEvent& e, funkgui::GestureController& g, double now);
        bool key(const funkgui::KeyEvent& e, funkgui::GestureController& g);
        // A frame: `hovered` (the pointer over it, and no card over the panel) writes hover=; a key's press runs out.
        void tick(float dt, bool hovered);

        // The pointer's inputs for a shader: x, y (0..1 in its rect, the last place), down, hover.
        std::array<float, 4> inputs() const noexcept;
        bool pressed() const noexcept { return pressed_ || keyHeld_ > 0.0f; }
        bool hovered() const noexcept { return hover_; }
        bool busy() const noexcept { return keyHeld_ > 0.0f; }   // a key's press running out: frames wanted
        funkgui::Cursor cursor() const noexcept;
        uint64_t key() const noexcept;                   // what a ring drawn round it changes with

        // A screen reader's: a slider for x and for y, a button for down, titled by their parameters (ids x, y, down).
        void accessibility(std::vector<funkgui::A11yItem>& out, const std::array<uint32_t, 3>& ids) const;
        bool a11yAction(const std::array<uint32_t, 3>& ids, uint32_t id, funkgui::A11yAction a, double v,
                        funkgui::GestureController& g);
        // The footer's line: what it writes, and how.
        void specLine(char* out, std::size_t n, const char* what) const;

    private:
        enum : std::size_t { kX, kY, kDown, kWheel, kHover };
        funkgui::ParamPort* port(std::size_t k) const noexcept { return v_[k] != nullptr ? &v_[k]->port() : nullptr; }
        void begin(funkgui::GestureController& g);       // the ports of x, y and down: their gestures begin
        void write(std::size_t k, float host01);         // inside the gesture, when it changes
        void end();                                      // down low, springs, the gestures end
        void writeHover(bool on);

        PanelPointer p_;
        std::array<GuiValue*, 5> v_{};
        std::function<void(int, funkgui::Point)> menu_;
        funkgui::Rect rect_;
        std::vector<funkgui::ParamPort*> open_;          // ports whose gesture is open
        funkgui::HostServices* host_ = nullptr;          // the press's (its batches), while it is open
        bool pressed_ = false, hover_ = false;
        funkgui::ParamPort* hoverOn_ = nullptr;          // the hover= port written high
        float px_ = 0.5f, py_ = 0.5f;                    // the pointer's place (pointer.x, pointer.y)
        float x01_ = 0.0f, y01_ = 0.0f;                  // relative: where x and y have got to
        funkgui::Point last_{};
        float keyHeld_ = 0.0f;                           // seconds a key's press of down has left
    };

    // An area, or a container's, a label's or a display's pointer properties: an item that draws nothing but its focus
    // ring and writes what the pointer does over its rect (PointerSink). The Panel hits it after every other item.
    class GuiPointerItem final : public Item
    {
    public:
        explicit GuiPointerItem(std::array<uint32_t, 3> ids) : ids_(ids) {}
        PointerSink& sink() noexcept { return sink_; }
        const PointerSink& sink() const noexcept { return sink_; }
        // Its element's kind (the footer's word), its rect, the panel's area, whether it is shown.
        void place(GuiKind kind, const funkgui::Rect& rect, const funkgui::Rect& area, bool shown);
        const funkgui::Rect& rect() const noexcept { return rect_; }
        GuiKind kind() const noexcept { return kind_; }
        void tick(float dt, bool hovered, bool focused, funkgui::Point) override;
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override;
        uint64_t drawKey() const override { return DrawKey{}.mix(sink_.key()).mix(rect_).mix(focused_).h; }
        bool contains(funkgui::Point p) const override { return shown() && area_.contains(p) && rect_.contains(p); }
        funkgui::Cursor cursorAt(funkgui::Point) const override { return sink_.cursor(); }
        void pointerMove(const funkgui::PointerEvent& e) override { sink_.move({ e.x, e.y }); }
        void pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController& g, funkgui::HostServices&) override;
        void pointerDrag(const funkgui::PointerEvent& e, funkgui::GestureController& g) override { sink_.drag(e, g); }
        void pointerUp(const funkgui::PointerEvent&, funkgui::GestureController&) override { sink_.release(); }
        void doubleClick(const funkgui::PointerEvent&, funkgui::GestureController& g) override { sink_.doubleClick(g); }
        bool wheel(const funkgui::WheelEvent& e, funkgui::GestureController& g, double now) override { return sink_.wheel(e, g, now); }
        bool key(const funkgui::KeyEvent& e, funkgui::GestureController& g) override { return sink_.key(e, g); }
        void accessibility(std::vector<funkgui::A11yItem>& out) const override;
        bool a11yAction(uint32_t id, funkgui::A11yAction a, double v, funkgui::GestureController& g) override;
        void specLine(char* out, std::size_t n) const override;
        bool settled() const override { return !sink_.busy(); }
        funkgui::Point centre() const override { return { rect_.centreX(), rect_.centreY() }; }
        bool shown() const override { return shown_ && sink_.any(); }
        bool focusable() const override { return sink_.focusable(); }

    private:
        std::array<uint32_t, 3> ids_;
        PointerSink sink_;
        GuiKind kind_ = GuiKind::area;
        funkgui::Rect rect_, area_;
        bool shown_ = false, focused_ = false;
    };

    // A shader element (fminus/Shaders.h): its program, drawn over its cell by FunkGui (Canvas::shader) with its
    // inputs as u_params, once the Panel has registered it (funkgui/gpu/ShaderLibrary.h); until then, or when it was
    // refused, a placeholder that says why. It moves every frame while it animates (its code reads u_time:
    // shaderAnimates), else while an input moves (Stirred: what place() and setInputs() see change, kept until the next
    // tick reads it).
    class GuiShaderItem final : public Item
    {
    public:
        // `ids`: its own (an image to a screen reader), then its pointer's x, y and down.
        explicit GuiShaderItem(std::array<uint32_t, 4> ids) : id_(ids[0]), pointerIds_{ ids[1], ids[2], ids[3] } {}

        // Its element, its cell, the panel's area, whether it is shown; its program's id (0: none) and file, and why
        // it draws nothing ("" when it draws), and whether it animates.
        void place(const GuiNode& node, const funkgui::Rect& cell, const funkgui::Rect& area, bool shown, uint32_t program,
                   const std::string& file, const std::string& why, bool animated);
        void setInputs(const std::array<float, kMaxShaderInputs>& v);
        const std::array<float, kMaxShaderInputs>& inputs() const noexcept { return inputs_; }
        uint32_t program() const noexcept { return program_; }
        const std::string& why() const noexcept { return why_; }
        const funkgui::Rect& cell() const noexcept { return cell_; }
        // Its pointer properties (PointerSink), and whether its inputs read the pointer (pointer.x ...): with either
        // it takes the pointer; with neither a press goes to the container around it.
        PointerSink& sink() noexcept { return sink_; }
        const PointerSink& sink() const noexcept { return sink_; }
        void setReadsPointer(bool reads) noexcept { readsPointer_ = reads; }
        bool takesPointer() const noexcept { return readsPointer_ || sink_.any(); }

        // A frame: its inputs moved (Stirred: what place() and setInputs() saw change, kept until the next tick reads
        // it), the pointer over it.
        void tick(float dt, bool hovered, bool focused, funkgui::Point) override
        {
            stir_.tick(dt, changed_);
            changed_ = false;
            sink_.tick(dt, hovered && shown_);
            focused_ = focused;
        }
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override;
        uint64_t drawKey() const override                // its inputs (u_params, the pointer's among them) and what it draws
        {
            DrawKey k;
            for (const float v : inputs_)
                k.mix(v);
            return k.mix(program_).mix(why_).mix(cell_).mix(focused_).h;
        }
        bool contains(funkgui::Point p) const override { return shown_ && takesPointer() && area_.contains(p) && cell_.contains(p); }
        funkgui::Cursor cursorAt(funkgui::Point) const override { return sink_.cursor(); }
        void pointerMove(const funkgui::PointerEvent& e) override { sink_.move({ e.x, e.y }); }
        void pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController& g, funkgui::HostServices&) override;
        void pointerDrag(const funkgui::PointerEvent& e, funkgui::GestureController& g) override { sink_.drag(e, g); }
        void pointerUp(const funkgui::PointerEvent&, funkgui::GestureController&) override { sink_.release(); }
        void doubleClick(const funkgui::PointerEvent&, funkgui::GestureController& g) override { sink_.doubleClick(g); }
        bool wheel(const funkgui::WheelEvent& e, funkgui::GestureController& g, double now) override { return sink_.wheel(e, g, now); }
        bool key(const funkgui::KeyEvent& e, funkgui::GestureController& g) override { return sink_.key(e, g); }
        void accessibility(std::vector<funkgui::A11yItem>& out) const override;
        bool a11yAction(uint32_t id, funkgui::A11yAction a, double v, funkgui::GestureController& g) override;
        void specLine(char* out, std::size_t n) const override;
        // Moving while it animates, while an input moves, and while a key's press of its down= runs out.
        bool settled() const override
        {
            return !shown_ || (!(animated_ && program_ != 0) && !stir_.moving() && !sink_.busy());
        }
        funkgui::Point centre() const override { return { cell_.centreX(), cell_.centreY() }; }
        bool shown() const override { return shown_; }
        bool focusable() const override { return shown_ && sink_.focusable(); }

    private:
        uint32_t id_;
        std::array<uint32_t, 3> pointerIds_;
        PointerSink sink_;
        bool readsPointer_ = false, focused_ = false;
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
