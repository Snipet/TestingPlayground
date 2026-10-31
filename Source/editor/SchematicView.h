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
//   toolbar      Select, Ground (G), Label (N); R C L D P X S, each its symbol; More (A: E, Shift-G, O, V, I, Shift-P,
//                Input, Output); at the right Undo, Redo (the tab's own), Tidy, -, the zoom (a click fits), +
//   canvas       the drawing (SchematicDraw.h) on a grid of one pin pitch, 10 px at 100 % (25 % to 400 %): parts, wires,
//                flags, names and values, the problems' badges, the structural marks, the ghost of a part carried,
//                the wire being drawn, the parts being moved with their wires rubber-banded, the marquee, the field
//   info strip   a gesture's notice (4 s: a refusal, a change of nets), else the dial under the pointer, else the
//                selection, else a hint; at its right the problems' count (a click steps to the next, Shift-click back),
//                a banner before it (Not drawn: ..., else Not played: ... with its button, Play this circuit, else
//                "program.fm now plays pedal.cir" with its button, Undo, while program.fm's text is what Play wrote,
//                else why its pots have no knobs: "program.fm:38: Screamer's pots have no dials: the program calls its
//                set()")
//
// The circuit dials (the spec's §10.5): when the program that plays drives this file's pots and switches with dials
// (CompileStatus::circuitDials, Patch.h's "Circuits"), a 16 px knob beside each pot symbol and a position chip beside
// each switch, where it covers the fewest bodies, flags, pins and wires (below or above a horizontal body, left or right
// of a vertical one, then the corners: SchematicDraw.h's placeKnobs); the labels then keep clear of it. A drag
// on a knob turns its dial (up raises it) and a double-click sets its default, a click on a chip steps its switch, each
// through the Panel's GestureController on the dial's own port, as the program card's dial writes; the pointer over
// one shows the dial's value in the info strip. Dragging the symbol's body still moves the part. Hidden with the
// labels (under 60 %).
//
// The pointer (§3.5): a press decides nothing until it moves 3 px (a drag) or comes up (a click). Idle, on a body: a
// click selects (Shift toggles), a drag moves the selection; on a pin: a click selects its part, a drag draws a wire;
// on a wire: a click selects its run, a drag draws a wire from that grid point (a T); on nothing: a click clears, a
// drag is a marquee (Shift adds). Carrying a part: a click places it and carries on, a drag places it where released, a
// popup click turns the ghost. The Ground tool: a click grounds that pin's or wire's net; the Label tool: a click opens
// that net's name field (on a wire, its tag goes there). A popup click opens the part's (Edit, Rotate, Rotate back,
// Mirror, Copy, Cut, Duplicate, Delete, Show in netlist; on a pin of a net drawn as tags, Show it as wires), the wire's
// (Rename net, Show as labels, Delete wire, Select net, Copy, Cut, Show in netlist) or the canvas's menu (Select all,
// Paste, Fit, Tidy, Netlist view, Undo, Redo); a popup drag, or a drag with Alt, pans in every mode and cancels nothing.
// A double click on a part (and Return with one selected, and a click on its value) edits it in place (the field,
// below); on a badge it opens the Netlist view at its message's line; on a wire it selects the whole net. A press
// outside an open field writes what can be written and closes it. A left drag or a carried part within 16 px of the
// canvas's edge pans the view in tick(), 600 px/s at the edge, so headless settles are exact.
//
// Keys while the canvas has them (FilesItem takes every key in this view; Escape leaves it): the parts' keys (R C L D
// P X S E V I, Shift-G, O, Shift-P: carried under the pointer, or placed at the view's centre, or 4 units right of the
// selection, when the pointer is away), A the More menu, G the Ground tool, N the Label tool, Escape (a gesture, then
// the tool, then the selection, then the canvas), Return (the field), Cmd-R, Cmd-Shift-R, Cmd-E, Delete and Backspace,
// Cmd-Z, Cmd-Shift-Z, Cmd-Y, Cmd-A, Cmd-C, Cmd-X, Cmd-V, Cmd-D (the clipboard: netlist text, fminus/CirDoc.h's copied
// and apply's paste op; Duplicate pastes just below the selection, else beside it, where the copy meets nothing; a
// paste onto parts is refused, as a drop is), Tab and Shift-Tab (the parts in line order), the
// arrows (a unit; Shift 4), F and Home (fit), Cmd-0, Cmd-=, Cmd--. While a field is open it takes every key.
//
// Live probes (the spec's §9.3; Engine.h's "Probes"). While the program that plays holds this netlist's module (the
// status's probe table, CompileStatus::probes), the selected wire run's net is probe slot 0 and up to three pinned
// nets slots 1 to 3 (Alt-click on a wire, the wire's menu, the card's Pin; a fourth pin replaces the oldest; each
// pinned net a numbered flag on its wire; the pins are the session's, EditorPort::fileViews). Each net is resolved by
// name in the playing compile's nodes for this file, in the element the instance chip chose (amp[0] by default), and
// the Panel's own set goes to the processor (ProbeHooks::arm) each time it changes, so a RUN or a refine (a new
// program id) arms the same names again. The readout card (56 px: the net, its peak, RMS and mean, a triggered scope
// of 1,024 samples, a rising crossing of the mean, auto-ranged in powers of two volts from 0.125 to 16, the chip and
// Pin) and the pinned nets' lanes (36 px each, scrolling) are drawn over the canvas's bottom, at most 40 % of it, so
// they never move the canvas; hovering a lane lights its wire. A net that does not resolve says why in the info strip
// and to the screen reader (the files play the netlist but what plays does not: not playing; what plays reads it but
// holds no field of its module: so; a node the program that plays lacks: edited since RUN; ground, 0 V; a node a
// probe cannot read, its why; no voice sounding; no file #includes it: not played), and so does the readout's
// accessibility item. The set is cleared (an empty one) when the view goes (another tab, the
// Netlist view, the PANEL view) and with the Panel; with nothing armed nothing is live and the view settles as before.
//
// Session pins: the places the view drew for parts and terminals that have no line of their own (SchematicFile::pins,
// reconciled with each revision so they stay put) go to the Panel as *@ part, *@ input and *@ output lines whenever
// they change (setPinsHook: EditorPort::setSchematicPins), so Claude's edit_circuit and view_panel's picture draw them
// where the user sees them.
//
// Accessibility: one group "Schematic, <file>" (the item's Tab stop), a list of parts ("R4, resistor 4.7k, from n1 to
// z1") and a list of nets ("net n1: R4.1, C4.1, XU1.inn"), ids kA11yBase plus a 24-bit hash of "p:"+name or
// "n:"+name (collisions probed in natural order), stable across pans and zooms; a11yRevision() bumps only when parts or
// nets come or go. Message thread only.
#pragma once

#include "dsp/Controls.h"
#include "dsp/Meters.h"
#include "editor/CodeAssist.h"
#include "editor/SchematicDraw.h"
#include "fminus/CirDoc.h"
#include "fminus/Circuits.h"
#include "fminus/Lang.h"

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/Input.h>
#include <funkgui/text/LineEdit.h>
#include <funkgui/widgets/TextEditor.h>

#include <array>
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
        cir::SessionPins pins;                       // the places drawn for parts and terminals without a line
        std::string published;                       // ... as *@ lines, as the Panel last published them
        bool merged = false;                         // a draft merged into the tab: what it moved flashes
        std::vector<std::string> probes;             // the nets pinned to probe slots 1 to 3, in order (the session
                                                     // keeps them: EditorPort::FileView::probes)
        std::string probeAt;                         // the element the probes watch ("amp[1]"; "": the first)
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

        enum class Tool : uint8_t { select, ground, label, place };
        // The toolbar's buttons, in order (the parts' are R C L D P X S).
        enum class Button : uint8_t
        {
            select, ground, label, r, c, l, d, p, x, s, more, undo, redo, tidy, zoomOut, zoom, zoomIn, count
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
        // The probes (above): the program that plays's probe table and its id (0: none plays), the meters as the Panel
        // read them this frame, a slot's ring (EditorPort::probe), and this Panel's set (EditorPort::setProbes).
        struct ProbeHooks
        {
            std::function<const std::vector<circuits::Probed>&()> table;
            std::function<uint64_t()> program;
            std::function<const Meters&()> meters;
            std::function<int(int slot, float* out, int n)> samples;
            std::function<void(const ProbeSet&)> arm;
            std::function<bool(const std::string& file)> runs;   // the program that plays #includes this netlist
        };
        void setProbeHooks(ProbeHooks hooks) { probe_ = std::move(hooks); }
        // The Panel's GestureController, through which a knob writes (FilesItem hands it in with each pointer event).
        void setGestures(funkgui::GestureController* g) noexcept { gestures_ = g; }

        void attach(funkgui::HostServices* host) { host_ = host; }
        // The session pins (EditorPort::setSchematicPins): called with the file and its *@ part, *@ input and *@ output
        // lines whenever a reconcile changes the places drawn for parts and terminals that have no line of their own
        // ("" when there are none).
        void setPinsHook(std::function<void(const std::string& file, const std::string& lines)> pins) { pins_ = std::move(pins); }
        // The files to the processor at once, right after a gesture writes the tab (the Panel's: EditorPort::setDraftIf),
        // so that Claude's conditional write sees the gesture. False when a write of Claude's came in that the Panel has
        // not merged yet: the gesture is undone (one step) and the strip says to do it again, so neither is overwritten.
        void setSyncHook(std::function<bool()> sync) { sync_ = std::move(sync); }
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

        // ---- the field (editing in place, the spec's §3.11; the Label tool's name) ------------------------------------
        // A part's names and values, a row each (R C L: name, value; D: name, model; a pot: name, value, pot, taper; X:
        // name; E, G: name, gain; an OTA: name, its gain expression; V, I: name, value; S: name, switch, positions; the
        // input and the output: name, scale), or a net's name (the Label tool), on the drawing beside it, drawn and keyed
        // as a tab's name is. Tab and Shift-Tab move between rows; Up and Down step a value through E12 (Shift: a
        // decade); Return writes every row changed as one gesture (refused, the field stays open and says why); Escape
        // cancels; Cmd-V pastes. A value of an R or a pot written in milliohms shows FMinus's warning under it: Return
        // keeps it as typed, Alt-Return writes megohms. A pot's name renames its .pot line and every {} that names it.
        bool editing() const noexcept { return field_.on; }
        std::vector<std::string> fieldKeys() const;      // its rows' keys: "name", "value", "model", "pot", "taper",
                                                         // "gain", "switch", "positions", "scale"; a net's: "net"
        std::string fieldText(const std::string& key) const;   // a row's text now
        std::string fieldRow() const;                    // the row with the caret's key
        std::string fieldWhy() const { return field_.why; }    // a refusal ("" for none)
        std::string fieldWarning() const;                // FMinus's milliohm warning for the row shown ("" for none)
        funkgui::Rect fieldRect() const;                 // the field's card on screen (empty when none)
        bool editPart(const std::string& name, const std::string& row = {});   // a part's field (row: the caret's)
        bool editNetAt(funkgui::Point p);                // the Label tool's field for the net under a point
        bool commitField(bool megohms = false);          // Return (Alt-Return): false when refused (it stays open)
        void closeField();                               // Escape
        std::optional<funkgui::Point> centreOfValue(const std::string& part) const;   // its value label (a click edits it)

        // ---- the clipboard (§3.14): parts as .cir text (fminus/CirDoc.h's copied; a paste op takes it back) ----------
        bool copySelection();                            // Cmd-C
        bool cutSelection();                             // Cmd-X
        bool paste();                                    // Cmd-V: at the pointer over the canvas, else the view's centre
        bool duplicate();                                // Cmd-D: a copy just below the selection (else beside it)
                                                         // where it meets nothing
        // Show as labels / Show as wires (§3.8): a signal net drawn as name tags at its pins, or wired again.
        bool showAsLabels(const std::string& net, bool labels);

        // ---- the probes (above) ------------------------------------------------------------------------------------
        static constexpr float kCardH = 56.0f;       // the readout card
        static constexpr float kLaneH = 36.0f;       // a pinned net's lane
        static constexpr float kProbeShare = 0.4f;   // card and lanes take at most this much of the canvas
        static constexpr int kTraceSamples = 1024;   // a scope's window
        // Pin a net to a slot (1 to 3; a fourth replaces the oldest), unpin it, or either (Alt-click): false when there
        // is no such net (ground cannot be pinned: it is 0 V). A user's gesture: the session keeps the pins.
        bool pinProbe(const std::string& net);
        bool unpinProbe(const std::string& net);
        bool togglePin(const std::string& net);
        const std::vector<std::string>& pinnedProbes() const;
        // The nets the slots watch now ("" for none), the set sent last (EditorPort::setProbes), why a slot's net does
        // not resolve ("" when it does, or holds none), the readout's words ("PROBE a   PEAK 0.402 V   RMS 0.294 V
        // MEAN +0.000 V"; "" while it reads nothing), and whether anything is live (the meters this set's).
        std::string probeNet(int slot) const;
        const ProbeSet& probeSet() const noexcept { return sent_; }
        std::string probeReason(int slot) const;
        std::string probeWords(int slot) const;
        bool probesLive() const;
        // The elements the chip offers ("amp[0]", "amp[1]") and the one chosen; choosing one (the chip's menu).
        std::vector<std::string> probeElements() const;
        std::string probeElement() const;
        void setProbeElement(const std::string& path);
        // Where they are drawn now (empty when not shown): the card, a lane (0: slot 1's), its pin button, the chip.
        funkgui::Rect probeCard() const;
        funkgui::Rect probeLane(int lane) const;
        std::optional<funkgui::Point> centreOfPin() const;
        std::optional<funkgui::Point> centreOfChip() const;
        // A slot's scope as drawn: its window (the newest kTraceSamples from the last rising crossing of its mean that
        // leaves one whole, else the newest; fewer while fewer are of the set that plays) and its range in volts (a power
        // of two from 0.125 to 16 that holds the window about its mean; 0 while it reads nothing).
        std::vector<float> probeTrace(int slot) const;
        float probeRange(int slot) const;

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
        using Mark = sch::Mark;                          // a message on the file, mapped to what it marks
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
        // One gesture; false (and a notice, or the refusal in `why`) when refused; the parts and nets it changed.
        bool commit(const std::vector<cir::Op>& ops, std::string* why = nullptr, std::vector<std::string>* touched = nullptr);
        bool pasteText(const std::string& text, cir::Pt at);    // a paste op of `text` at `at`, the pasted parts selected
        void publishPins() const;                        // the session pins, to the Panel when they changed
        bool synced();                                   // a gesture's files to the processor (else undone): sync_
        bool fieldKey(const funkgui::KeyEvent& e);       // a key while the field is open
        void stepField(int dir, bool decade);            // Up, Down in a value row: E12
        void drawField(funkgui::Canvas& c, const funkgui::Theme& th, const sch::View& v, const sch::Inks& k) const;
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

        // The probes (above): each slot's net and what it resolved to this frame, the samples read for its scope, the
        // set sent, the lanes' scroll and the one under the pointer, the reason last spoken, the readouts' items' count.
        struct ProbeSlot
        {
            std::string net;                             // the net's name ("" for none)
            int node = -1;                               // its index in the table's nodes, -1 when it does not resolve
            std::string reason;                          // why not ("" when it does, or holds none)
            int valid = 0;                               // samples of this set in `trace`
            std::vector<float> trace;                    // the last 2 x kTraceSamples of its ring
        };
        void updateProbes();                             // tick(): the slots resolved, the set sent, the rings read
        void clearProbes();                              // leave(): an empty set
        int probeTable() const;                          // the table's field for this file and the element chosen (-1)
        int probeElementIndex(int field) const;
        float lanesTop() const;                          // the lanes' area's top (the card's top when none)
        int laneAt(funkgui::Point p) const;              // the lane under a point, -1 for none
        std::optional<funkgui::Point> flagAt(const std::string& net) const;   // where a pinned net's number sits (world)
        void drawProbes(funkgui::Canvas& c, const funkgui::Theme& th, const sch::View& v, const sch::Inks& k) const;
        void openChipMenu();
        ProbeHooks probe_;
        std::array<ProbeSlot, 4> slots_{};
        ProbeSet sent_{};
        bool sentAny_ = false;
        float laneScroll_ = 0.0f;
        std::string spoken_;
        uint32_t probeA11y_ = 0;
        int probeItems_ = 0;

        Hooks hooks_;
        DialHooks dials_;
        std::function<void(const std::string&, const std::string&)> pins_;
        std::function<bool()> sync_;                     // setSyncHook
        // The field (above): its rows, the one with the caret, what it edits, the text's revision it opened on.
        struct Row
        {
            std::string key, label, was;
            funkgui::text::LineEdit edit;
        };
        struct Field
        {
            bool on = false;
            std::string part;                            // the part (an element's or a terminal's name), else ""
            std::string net;                             // the Label tool's net, else ""
            std::optional<cir::Pt> tag;                  // ... and where its tag goes (a wire's point clicked)
            cir::Kind kind = cir::Kind::Unknown;
            std::vector<Row> rows;
            int row = 0;
            std::string why;                             // a refusal, until a key
            uint32_t rev = 0;
            funkgui::Point world{};                      // where it sits (world): under the part's body, or the point
        } field_;
        mutable std::vector<funkgui::Rect> valueRects_;  // each part's value label as the last frame drew it
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
        bool overtaken_ = false;                         // a gesture undone by synced(), to be said after the merge
        uint32_t overtakenRev_ = 0;                      // ... the revision it left
        mutable std::map<std::string, float> flash_;     // part name -> seconds left
    };
}
