// Source/editor/SchematicView.h: the Schematic view of a .cir tab (the spec's §3): a drawing of the netlist that edits
// it. The .cir file is the only source of truth: the tab's TextEditor is the model, and the drawing is
// fminus/CirDoc.h's reconcile() of its text (a pure function, run lazily while the view shows, once per revision). A
// gesture becomes cir::Ops, cir::apply() makes them text patches on the text the user saw (the netlist's tokens, the
// canonical *@ block before .end), and they go into the editor as one TextEditor::applyEdits: one undo step, which the
// Netlist view undoes too, and the other way round. A gesture whose base revision moves under it (Claude's write
// merged, a host loading a session) is cancelled and changes nothing.
//
// FilesItem (Panel.cpp) owns it and hands it draw, input, wheel and keys while the tab shown is a .cir in Schematic view
// (a view of a file, never a tab: it adds no file). It reaches the Panel only through its hooks and the processor not
// at all; the session's file views (EditorPort::fileViews) are the Panel's to keep.
//
//   toolbar      Select, Ground (G); R C L D P X S, each its symbol; More (A: E, Shift-G, O, V, I, Shift-P, Input,
//                Output); at the right Undo, Redo (the tab's own), Tidy, -, the zoom (a click fits), +
//   canvas       the drawing (SchematicDraw.h) on a grid of one pin pitch, 10 px at 100 % (25 % to 400 %): parts, wires,
//                flags, names and values, the problems' badges, the structural marks, the ghost of a part carried,
//                the wire being drawn, the parts being moved with their wires rubber-banded, the marquee
//   info strip   a gesture's notice (4 s: a refusal, a change of nets), else the dial under the pointer, else the
//                selection, else a hint; at its right the problems' count (a click steps to the next, Shift-click back),
//                a banner before it (Not drawn: ..., else Not played: ... with its button, Play this circuit, else
//                "program.fm now plays pedal.cir" with its button, Undo, while program.fm's text is what Play wrote)
//
// The circuit dials (the spec's §10.5): when the program that plays drives this file's pots and switches with dials
// (CompileStatus::circuitDials, Patch.h's "Circuits"), a 16 px knob beside each pot symbol (below a horizontal one,
// left of a vertical one; on the other side when its labels are there) and a position chip beside each switch. A drag
// on a knob turns its dial (up raises it) and a double-click sets its default, a click on a chip steps its switch, each
// through the Panel's GestureController on the dial's own port, as the program card's dial writes; the pointer over
// one shows the dial's value in the info strip. Dragging the symbol's body still moves the part. Hidden with the
// labels (under 60 %).
//
// The pointer (§3.5): a press decides nothing until it moves 3 px (a drag) or comes up (a click). Idle, on a body: a
// click selects (Shift toggles), a drag moves the selection; on a pin: a click selects its part, a drag draws a wire;
// on a wire: a click selects its run, a drag draws a wire from that grid point (a T); on nothing: a click clears, a
// drag is a marquee (Shift adds). Carrying a part: a click places it and carries on, a drag places it where released, a
// popup click turns the ghost. The Ground tool: a click grounds that pin's or wire's net. A popup click opens the part's,
// the wire's or the canvas's menu; a popup drag, or a drag with Alt, pans in every mode and cancels nothing. A double
// click on a part (and Return with one selected) opens the Netlist view at its value (s4 edits it in place); on a
// badge at its message's line; on a wire it selects the whole net. A left drag or a carried part within 16 px of the
// canvas's edge pans the view in tick(), 600 px/s at the edge, so headless settles are exact.
//
// Keys while the canvas has them (FilesItem takes every key in this view; Escape leaves it): the parts' keys (R C L D
// P X S E V I, Shift-G, O, Shift-P: carried under the pointer, or placed at the view's centre, or 4 units right of the
// selection, when the pointer is away), A the More menu, G the Ground tool, Escape (a gesture, then the tool, then the
// selection, then the canvas), Cmd-R, Cmd-Shift-R, Cmd-E, Delete and Backspace, Cmd-Z, Cmd-Shift-Z, Cmd-Y, Cmd-A,
// Tab and Shift-Tab (the parts in line order), the arrows (a unit; Shift 4), F and Home (fit), Cmd-0, Cmd-=, Cmd--.
//
// Accessibility: one group "Schematic, <file>" (the item's Tab stop), a list of parts ("R4, resistor 4.7k, from n1 to
// z1") and a list of nets ("net n1: R4.1, C4.1, XU1.inn"), ids kA11yBase plus a 24-bit hash of "p:"+name or
// "n:"+name (collisions probed in natural order), stable across pans and zooms; a11yRevision() bumps only when parts or
// nets come or go. Message thread only.
#pragma once

#include "dsp/Controls.h"
#include "editor/CodeAssist.h"
#include "editor/SchematicDraw.h"
#include "fminus/CirDoc.h"
#include "fminus/Lang.h"

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/Input.h>
#include <funkgui/widgets/TextEditor.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace funkgui
{
    class GestureController;
    class HostServices;
    class ParamPort;
}

namespace playground::ui
{
    // A .cir file's view (FilesItem keeps one a file, by name): which view shows, the zoom and the centre (the session
    // keeps these: EditorPort::fileViews), and in memory the selection, the places drawn for parts that have no part
    // line (session pins) and a merge to flash.
    struct SchematicFile
    {
        bool schematic = false;                      // the Schematic view (else the Netlist view)
        bool stored = false;                         // the user chose its view, zoom or pan: the session keeps it
        bool framed = false;                         // zoom and centre are set (stored, or fitted when it opened)
        float zoom = 1.0f;                           // 1: 100 % (10 px a grid unit)
        float cx = 0.0f, cy = 0.0f;                  // the world point at the canvas's centre, grid units
        std::vector<std::string> selected;           // parts, by name: the selection survives undo
        cir::SessionPins pins;                       // the places drawn for parts without a part line
        bool merged = false;                         // a draft merged into the tab: what it moved flashes
    };

    class SchematicView
    {
    public:
        static constexpr uint32_t kA11yBase = 0x40000000u;
        static constexpr std::size_t kA11yItems = 400;   // the accessibility tree's budget: parts first, then nets
        static constexpr float kToolbarH = 28.0f;    // the toolbar, over the canvas
        static constexpr float kStripH = 22.0f;      // the info strip, under it (always reserved)
        static constexpr float kMinZoom = 0.25f, kMaxZoom = 4.0f;
        static constexpr float kOpenMinZoom = 0.6f;  // opening fits the drawing, never below this
        static constexpr float kEdgePx = 16.0f;      // edge auto-pan: its band
        static constexpr float kEdgeSpeed = 600.0f;  // ... and its speed at the edge, px/s
        static constexpr float kNoticeSeconds = 4.0f;
        static constexpr float kFlashSeconds = 0.6f;
        static constexpr float kKnobR = 8.0f;          // a circuit dial's knob on the drawing: 16 px across
        static constexpr float kChipW = 20.0f, kChipH = 14.0f;   // ... a switch's position chip

        enum class Tool : uint8_t { select, ground, place };
        // The toolbar's buttons, in order (the parts' are R C L D P X S).
        enum class Button : uint8_t
        {
            select, ground, r, c, l, d, p, x, s, more, undo, redo, tidy, zoomOut, zoom, zoomIn, count
        };

        struct Hooks
        {
            std::function<void(int line, int col)> showNetlist;    // the Netlist view, the caret there (0-based)
            std::function<std::vector<Diagnostic>()> problems;     // the tab's messages, where its decorations are now
            std::function<bool()> played;                          // a file of the program #includes this one
            std::function<void()> persist;                         // the user changed the view, its zoom or its pan
        };

        explicit SchematicView(Hooks hooks);

        // The circuit dials and Play this circuit (above): the program that plays's circuit fields, a dial's port and its
        // control (the program that plays's), Play (its why when it does not play: "" when it did), what the strip says
        // while program.fm's text is Play's ("" otherwise), and its Undo.
        struct DialHooks
        {
            std::function<const std::vector<lang::CircuitBinding>&()> bindings;
            std::function<funkgui::ParamPort*(int slot)> port;
            std::function<Control(int slot)> control;
            std::function<std::string()> play;
            std::function<std::string()> playedNote;
            std::function<void()> undoPlay;
        };
        void setDialHooks(DialHooks hooks) { dials_ = std::move(hooks); }
        // The Panel's GestureController, through which a knob writes (FilesItem hands it in with each pointer event).
        void setGestures(funkgui::GestureController* g) noexcept { gestures_ = g; }

        void attach(funkgui::HostServices* host) { host_ = host; }
        // The file shown in Schematic view: its tab's editor, its name and its state (FilesItem's; null: none shown).
        void show(funkgui::TextEditor* ed, const std::string& file, SchematicFile* state);
        bool showing() const noexcept { return ed_ != nullptr && st_ != nullptr; }
        const std::string& file() const noexcept { return file_; }
        void setArea(const funkgui::Rect& editorRect);   // the tab editor's rect: toolbar, canvas, info strip
        void leave();                                    // the view goes: a gesture cancels, the tool ends

        // ---- as an Item (FilesItem's) -------------------------------------------------------------------------------
        void tick(float dt, funkgui::Point pointer, bool focused);
        bool settled() const;
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool focused) const;
        funkgui::Cursor cursorAt(funkgui::Point p) const;
        void pointerMove(funkgui::Point p);              // the pointer over the item (a key places under it)
        void pointerExit();                              // ... or away
        void pointerDown(const funkgui::PointerEvent& e);
        void pointerDrag(const funkgui::PointerEvent& e);
        void pointerUp(const funkgui::PointerEvent& e);
        void doubleClick(const funkgui::PointerEvent& e);
        bool wheel(const funkgui::WheelEvent& e, double nowSec);
        bool key(const funkgui::KeyEvent& e);            // false: Escape with nothing left to end (leave the canvas)
        void accessibility(std::vector<funkgui::A11yItem>& out) const;
        bool a11yAction(uint32_t id, funkgui::A11yAction a);
        bool specLine(char* out, std::size_t n, funkgui::Point pointer) const;
        void focusChanged(bool focused);
        uint32_t a11yRevision() const;                   // bumps when parts or nets come or go
        std::string statusWords() const;                 // CodeAssist's status line: "Schematic   12 parts   ..."

        // ---- the drawing and the view (the Panel's, the probes') ---------------------------------------------------------
        const cir::Drawing& drawing() const;
        const cir::Netlist& netlist() const;
        funkgui::Rect canvasRect() const noexcept { return canvas_; }
        funkgui::Rect toolbarRect() const noexcept { return toolbar_; }
        funkgui::Rect stripRect() const noexcept { return strip_; }
        sch::View view() const;                          // world to px now
        float zoom() const noexcept { return st_ != nullptr ? st_->zoom : 1.0f; }
        funkgui::Point centre() const noexcept { return st_ != nullptr ? funkgui::Point{ st_->cx, st_->cy } : funkgui::Point{}; }
        funkgui::Point screenOf(cir::Pt p) const;
        std::optional<funkgui::Point> centreOfPart(const std::string& name) const;   // its body's centre
        std::optional<funkgui::Point> centreOfPin(const std::string& part, const std::string& pin) const;
        std::optional<funkgui::Point> centreOfBadge(const std::string& part) const;
        funkgui::Point centreOfButton(Button b) const;
        funkgui::Rect buttonRect(Button b) const;
        funkgui::Point centreOfProblems() const;         // the info strip's count (its click)
        std::optional<funkgui::Point> centreOfStart() const;   // an empty netlist's [Start a circuit]
        std::optional<funkgui::Point> centreOfKnob(const std::string& part) const;   // a pot's knob, a switch's chip
        int knobSlot(const std::string& part) const;     // the dial a part's knob or chip turns (-1: none)
        int knobCount() const;                           // the knobs and chips drawn now
        std::string stripButton() const;                 // the strip's button: "Play this circuit", "Undo", or ""
        std::optional<funkgui::Point> centreOfStripButton() const;
        std::vector<std::string> selection() const;      // the parts selected, by name
        int selectedRuns() const noexcept { return static_cast<int>(runs_.size()); }
        std::vector<cir::UnitEdge> selectedEdges() const;
        Tool tool() const noexcept { return tool_; }
        cir::Kind carried() const noexcept { return kind_; }
        cir::Orient carriedOrient() const noexcept { return orient_; }
        std::string notice() const;                      // the info strip's notice (none once the text moved since)
        std::string stripLeft() const;                   // what the info strip says at its left
        std::string stripRight() const;                  // ... and at its right: the banner, then the problems' count
        int problemCount() const;                        // the messages on this file
        bool gestureOn() const noexcept;                 // a drag in progress (moving, wiring, a marquee)
        float flashOf(const std::string& part) const;    // Claude's flash: 1 just merged, 0 none

        // ---- actions (the menus', the keys', the Panel's) ------------------------------------------------------------
        void setTool(Tool t, cir::Kind kind = cir::Kind::Unknown);
        void selectPart(const std::string& name, bool centre);
        bool selectAtLine(int line, bool centre);        // the part on a netlist line (0-based); the problems' jump
        void fit(bool opening);                          // F: everything, down to 25 %; opening: never below 60 %
        void zoomTo(float zoom, funkgui::Point aboutPx); // about a point of the canvas
        void panBy(float dxPx, float dyPx);              // the content moves by these px
        bool startCircuit();                             // an empty file's seed, one gesture
        bool rotate(bool clockwise);
        bool mirror();
        bool deleteSelection();
        bool nudge(int dx, int dy);
        bool tidy();
        void undo();
        void redo();
        void selectAll();
        void stepPart(int dir);                          // Tab, Shift-Tab
        void stepProblem(int dir);                       // the info strip's count
        bool placeAt(cir::Kind kind, cir::Pt at, cir::Orient o);
        void openMenu(funkgui::Point at);                // the popup click's menu, for what is under `at`

    private:
        // What is under a point.
        struct Hit
        {
            enum class Kind : uint8_t { none, badge, pin, part, wire, start };
            Kind kind = Kind::none;
            int part = -1, pin = -1, net = -1;
            cir::Pt at{};                                // a wire's grid point; a pin's place
            cir::UnitEdge edge{};                        // the wire's edge under it
        };
        // A wire run: the maximal chain of a net's unit edges between pins and junctions.
        struct Run
        {
            std::string net;                             // its key (folded)
            std::vector<cir::UnitEdge> edges;            // sorted
        };
        struct Mark                                      // a message on the file, mapped to what it marks
        {
            int line = 0, col = 0;                       // 0-based
            int severity = 0;                            // 0 error, 1 warning, 2 note
            std::string text;
            std::vector<int> parts;                      // the parts it marks (none: the count only)
            std::vector<std::pair<int, int>> pins;       // the pins its column falls on: (part, pin)
        };
        enum class Drag : uint8_t { none, press, move, wire, marquee, pan, carry, knob, control };
        // A circuit dial on the drawing (above): its part, its slot, a switch's chip (and its positions), its centre.
        struct Knob
        {
            int part = -1;
            int slot = -1;
            bool chip = false;
            int positions = 2;
            funkgui::Point at{};
        };

        void ensure() const;                             // the drawing of the editor's text now (and its marks)
        bool emptyNetlist() const;                       // no element (drawn or not) and no terminal: Start is offered
        std::string stripCount() const;                  // "2 PROBLEMS" (none: empty)
        funkgui::Rect countRect() const;                 // the count's, at the strip's right end (its click)
        std::string stripBanner() const;                 // "Not drawn: Q1 (a BJT: Phase 2)", else "Not played: ..."
        funkgui::Rect stripButtonRect() const;           // the banner's button, at the left of the count
        std::vector<Knob> knobs() const;                 // the circuit dials drawn now (screen points)
        int knobAt(funkgui::Point p) const;              // knobs()'s index under a point, -1 for none
        std::string knobWords(const Knob& k) const;      // "drive 62 %   drag to turn it, double-click: its default"
        void layoutBars();
        void computeMarks() const;                       // the tab's messages on the drawing (each tick: a check lands)
        Hit hitAt(funkgui::Point p) const;
        Run runAt(int net, cir::UnitEdge e) const;
        std::vector<Run> runsOf(int net) const;
        bool commit(const std::vector<cir::Op>& ops);    // one gesture; false (and a notice) when refused
        bool commitText(const std::string& text);        // a text made by several applies, as one gesture
        void say(const std::string& text);               // the info strip's notice, for 4 s (or until the text moves)
        void quiet();                                    // ... gone
        void cancelGesture(const char* why);
        void beginDrag(funkgui::Point p);                // the press became a drag
        void finishDrag(funkgui::Point p);
        void click(const funkgui::PointerEvent& e);      // the press came up under 3 px
        void followPointer(funkgui::Point p);            // a drag's state for the pointer (and the view) now
        cir::Pt gridAt(funkgui::Point p) const;          // the grid point nearest a canvas point
        void setSelection(std::vector<std::string> parts, std::vector<Run> runs);
        void persist();
        void announceSelection();
        std::string partWords(int part) const;           // "R4, resistor 4.7k, from n1 to z1"
        std::string netWords(int net) const;             // "net n1: R4.1, C4.1, XU1.inn"
        uint32_t idOf(const std::string& key) const;
        void openNetlistAtPart(int part);
        bool moving(int part) const;
        cir::End endOf(const Hit& h) const;
        std::vector<cir::Pt> wirePath() const;

        Hooks hooks_;
        DialHooks dials_;
        funkgui::GestureController* gestures_ = nullptr;
        float knobFrom_ = 0.0f, knobY_ = 0.0f;           // a knob's drag: the value it started at, the pointer's y
        int knobPart_ = -1;                              // ... and its part
        funkgui::HostServices* host_ = nullptr;
        funkgui::TextEditor* ed_ = nullptr;
        std::string file_;
        SchematicFile* st_ = nullptr;
        funkgui::Rect area_{}, toolbar_{}, canvas_{}, strip_{};
        std::vector<funkgui::Rect> buttons_;

        // the model: the editor's text at a revision, read and reconciled (lazily)
        mutable bool valid_ = false;
        mutable uint32_t rev_ = 0;
        mutable std::string text_;
        mutable cir::Netlist nl_;
        mutable cir::Drawing d_;
        mutable uint32_t structure_ = 0;
        mutable std::string structureKey_;
        mutable std::map<std::string, uint32_t> ids_;   // a11y ids by "p:"/"n:" name
        mutable std::map<std::string, std::string> shapes_;   // part name -> origin/orient/value (the flash's)

        mutable std::vector<Mark> marks_;
        mutable std::vector<sch::PartMarks> partMarks_;  // per part of d_
        // The structural marks of d_ (made with it, once a revision): a pin alone on its net (an open circle), a pin
        // that touches another net's pin or wire end without connecting (a dot).
        struct Loose
        {
            int part = -1;
            cir::Pt at{};
            bool alone = true;
        };
        mutable std::vector<Loose> loose_;
        void computeLoose() const;
        int stepAt_ = -1;                                // the problem the info strip stepped to

        // the view's motion: a fit eases there
        bool easing_ = false;
        float zoomTo_ = 1.0f, cxTo_ = 0.0f, cyTo_ = 0.0f;
        bool fitPending_ = false;
        double zoomedAt_ = -10.0;                        // the wheel's last zoom (the guard)

        // the pointer, the tool, the gesture
        funkgui::Point pointer_{ -1.0e4f, -1.0e4f };
        bool focused_ = false;
        Tool tool_ = Tool::select;
        cir::Kind kind_ = cir::Kind::Unknown;
        cir::Orient orient_ = cir::Orient::r0;
        Drag drag_ = Drag::none;
        struct Press
        {
            funkgui::Point at{};
            funkgui::Mods mods{};
            bool popup = false;
            Hit hit{};
            uint32_t rev = 0;
            funkgui::Point world{};
        } press_;
        cir::Pt moveBy_{};                               // moving: the selection's offset
        bool moveRefused_ = false;                       // ... it would overlap
        cir::Pt wireFrom_{}, wireTo_{};
        bool wireH_ = true, wireDecided_ = false;
        cir::End wireStart_{};
        funkgui::Point marqueeA_{}, marqueeB_{};         // world
        funkgui::Point panLast_{};
        bool panned_ = false;
        bool autoPanned_ = false;                        // edge auto-pan moved the view (kept once it stops)

        mutable std::vector<Run> runs_;                  // the wires selected (kept while their nets keep them)

        std::string notice_;
        float noticeLeft_ = 0.0f;
        uint32_t noticeRev_ = 0;                         // the editor's revision it was said at
        mutable std::map<std::string, float> flash_;     // part name -> seconds left
    };
}
