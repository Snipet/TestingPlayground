// Source/editor/SchematicDraw.h: a .cir netlist's drawing (fminus/CirDoc.h's Drawing) on the canvas, ANSI style: the
// symbols of the part library (fminus/CirLayout.h's table: pins on the body box's edge, in grid units at r0, turned by
// LTspice's eight orientations), wires as merged straight runs, junction dots where three or more unit edges meet,
// ground's three bars and a rail's bar at a pin (never a wire), name tags, names and values (always upright), the
// problems' marks in the tab editor's own inks with shapes of their own (an error: the part's outline and a filled
// badge; a warning: a filled badge; a note: a hollow ring), and the structural marks (an unconnected pin, posts that
// touch without connecting, a hand edit's overlap). SchematicView.h lays out and drives it; Picture.cpp (s4) draws it
// with no Panel.
//
// The transform. FunkGui has no transform stack: a View {scale, origin} maps a world point (grid units, x right, y
// down) to the Panel's px, and every point, stroke width and TextStyle::px goes through it. A 90-degree turn is done
// here, on the symbol's own points; text is never turned.
//
// Level of detail: what lies outside the clip is culled; names and values hide below 6 px of text (under 60 % zoom),
// but in picture mode (7 px at least, always drawn), and grow no larger than 20 px (200 %); a symbol under 12 px
// becomes its body box; the grid's dots only at 8 px apart or more (every unit, or every 2, 5 or 10 when the canvas
// would hold too many: the frame's budget).
//
// Colours are theme tokens (and the tab editor's DecorationInks for the marks): symbols ink100, wires, junctions and
// values ink70, names ink52, the grid ink16, the selection an accentDim halo while focused (ink16 otherwise) under
// ink100 strokes, the part or pin under the pointer the accent, Claude's flash an ink32 halo. Translucent strokes are
// premixed over the ground: Canvas::polyline needs opaque inks.
#pragma once

#include "fminus/CirDoc.h"

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/text/TextStyle.h>

#include <cstdint>
#include <string>
#include <vector>

namespace playground::ui
{
    struct Diagnostic;                                   // editor/CodeAssist.h
}

namespace playground::ui::sch
{
    // The schematic's primitive tags (after CodeAssist's +20 and +21): the canvas (its ground, grid, toolbar, info
    // strip, hint and ghost), the parts (symbols, names, values, flags), the wires (and junctions), the problems'
    // badges and marks, and the probes (s6).
    inline constexpr funkgui::Tag kSchematicTag = funkgui::tags::firstProduct + 22;
    inline constexpr funkgui::Tag kPartTag = funkgui::tags::firstProduct + 23;
    inline constexpr funkgui::Tag kWireTag = funkgui::tags::firstProduct + 24;
    inline constexpr funkgui::Tag kBadgeTag = funkgui::tags::firstProduct + 25;
    inline constexpr funkgui::Tag kProbeTag = funkgui::tags::firstProduct + 26;

    inline constexpr float kUnitPx = 10.0f;              // px a grid unit at 100 %: one pin pitch
    inline constexpr float kLabelPx = 10.0f;             // names and values at 100 %
    inline constexpr float kLabelHidePx = 6.0f;          // below this the labels hide
    inline constexpr float kLabelMaxPx = 20.0f;          // ... and they grow no larger (200 %)
    inline constexpr float kPictureLabelPx = 7.0f;       // a picture's labels, at least
    inline constexpr float kSymbolLodPx = 12.0f;         // a symbol smaller on screen is its body box
    inline constexpr float kGridMinPx = 8.0f;            // grid dots at least this far apart
    inline constexpr int kGridMaxDots = 1200;            // ... and at most this many

    // World (grid units) to the Panel's px: p * scale + origin.
    struct View
    {
        float scale = kUnitPx;                           // px a grid unit
        float ox = 0.0f, oy = 0.0f;                      // where world (0, 0) is

        funkgui::Point at(float x, float y) const noexcept { return { x * scale + ox, y * scale + oy }; }
        funkgui::Point at(cir::Pt p) const noexcept { return at(static_cast<float>(p.x), static_cast<float>(p.y)); }
        funkgui::Point world(funkgui::Point p) const noexcept { return { (p.x - ox) / scale, (p.y - oy) / scale }; }
        float stroke() const noexcept;                   // a symbol's and a wire's width at this scale
        funkgui::Rect rect(const cir::Box& b) const noexcept;   // a box of grid points, on screen
    };

    // The inks of a frame (theme tokens; the marks the tab editor's).
    struct Inks
    {
        funkgui::Col ground, symbol, wire, value, name, grid, accent, halo, flash, error, warning, note, faint;
    };
    // `marks`: the tab editor's DecorationInks resolved (error, warning, note), as both views draw them.
    Inks inksFor(const funkgui::Theme& th, funkgui::Col error, funkgui::Col warning, funkgui::Col note, bool focused);

    // A part's marks: the problems on it (their worst severity and count), on its pins, its own (structural).
    struct PartMarks
    {
        int errors = 0, warnings = 0, notes = 0;
        bool outline = false;                            // an error's outline, or a hand edit's overlap
        // the pins a message's column falls on (a node token): bit k for pin k, by severity (error, warning, note)
        uint8_t pinMask[3] = { 0, 0, 0 };
    };

    // How a frame draws a part beyond its symbol.
    struct PartLook
    {
        bool selected = false;
        bool hover = false;                              // the accent
        float flash = 0.0f;                              // Claude's: 1 (just merged) to 0
        PartMarks marks;
        int hoverPin = -1;                               // a pin under the pointer (accent)
    };

    // A symbol's strokes, its origin at world (x, y) (a ghost's may be between grid points), turned `o`; `small`: the
    // body box only (the level of detail, a toolbar icon of no symbol).
    void symbol(funkgui::Canvas& c, const View& v, cir::Kind kind, cir::Orient o, float x, float y, float width,
                funkgui::Col ink);
    // The body box, outlined.
    void bodyBox(funkgui::Canvas& c, const View& v, cir::Kind kind, cir::Orient o, float x, float y, float width,
                 funkgui::Col ink);
    // Whether a symbol of this kind draws as its body box at this scale.
    bool lodBox(const View& v, cir::Kind kind) noexcept;
    // A kind's symbol fitted in a box of `size` px centred on (cx, cy): the toolbar's icons.
    void icon(funkgui::Canvas& c, cir::Kind kind, float cx, float cy, float size, funkgui::Col ink);

    // A net's route: its unit edges merged into straight runs; its junctions as dots.
    void wires(funkgui::Canvas& c, const View& v, const cir::Net& net, float width, funkgui::Col ink);
    void junctions(funkgui::Canvas& c, const View& v, const cir::Net& net, funkgui::Col ink);
    // The straight runs of a list of unit edges (sorted, as a Net holds them): each a pair of end points.
    std::vector<std::pair<cir::Pt, cir::Pt>> runs(const std::vector<cir::UnitEdge>& edges);

    // A flag at a pin: ground's three bars, a rail's bar and its name, a name tag (a net drawn as tags).
    void flag(funkgui::Canvas& c, const View& v, const cir::Flag& f, cir::Style style, bool positive,
              const std::string& name, float width, funkgui::Col ink, funkgui::Col text, float labelPx);
    // A name tag on a route (label= points): the name over the point.
    void tag(funkgui::Canvas& c, const View& v, cir::Pt at, const std::string& name, funkgui::Col ink, float labelPx);

    // A terminal's flag (the input, the output) and its name inside, upright.
    void terminal(funkgui::Canvas& c, const View& v, const cir::Part& p, float width, funkgui::Col ink, funkgui::Col text,
                  float labelPx);

    // The labels' size at this scale (0: hidden), in picture mode or not.
    float labelPx(const View& v, bool picture) noexcept;
    // Where a part's name and value go: above a horizontal part, right of a vertical one, the value under the name;
    // where that covers another body, a flag, a label placed before, a pin, a junction or a wire, or sits nearer another
    // part than its own, the first of the other spots that does so least (below, left; name and value on one line;
    // half a unit along; beside the body off the line its leads run on).
    struct LabelSpot
    {
        float x = 0.0f, top = 0.0f;                      // the first line's anchor and top
        bool oneLine = false;                            // "R6 51k" on one line
        funkgui::Align align = funkgui::Align::centre;
        bool shown = false;
        bool backdrop = true;                            // false: no spot clear of wires, pins and junctions (they show
                                                         // through: a label never breaks a connection)
    };
    // Each part's (Drawing::parts' order; a terminal's is not shown: its name is in its flag), `values` theirs;
    // `keepOut`: more to keep clear of, as a body is (the circuit dials' knobs).
    std::vector<LabelSpot> placeLabels(const funkgui::Canvas& c, const View& v, const cir::Drawing& d,
                                       const std::vector<std::string>& values, float px,
                                       const std::vector<funkgui::Rect>& keepOut = {});
    // A part's name and value at its spot, upright, on a backdrop of the canvas's ground (none where the spot crosses a
    // wire, a pin or a junction); (dx, dy): a move's offset, px.
    void labels(funkgui::Canvas& c, const LabelSpot& s, const std::string& name, const std::string& value,
                funkgui::Col nameInk, funkgui::Col valueInk, funkgui::Col ground, float px, float dx = 0.0f, float dy = 0.0f);
    // Where a part's badge sits (its body's top-right corner, on screen), and its radius.
    funkgui::Point badgeAt(const View& v, const cir::Part& p) noexcept;
    inline constexpr float kBadgeR = 6.0f;
    // A part's problems: an error's outline and filled badge, a warning's filled badge, a note's hollow ring; a pin a
    // message names, a ring round it in the worst severity's ink.
    void marks(funkgui::Canvas& c, const View& v, const cir::Part& p, const PartMarks& m, const Inks& ink);

    // The grid's dots over `clip`.
    void grid(funkgui::Canvas& c, const View& v, const funkgui::Rect& clip, funkgui::Col ink);

    // A circuit dial on the drawing (SchematicView.h; the size on screen whatever the zoom): a pot's knob of radius `r`
    // at its value t (0 to 1, the dial's normalised value), over a disc of the canvas's ground, its track faint and its
    // turned part and pointer in `ink`; a switch's position chip, `text` its position, outlined in `ink`.
    void knob(funkgui::Canvas& c, funkgui::Point at, float r, float t, funkgui::Col ink, const Inks& k);
    // Where each of them sits (`at`, on screen): beside its part's body, the first of a few spots (below and above a
    // horizontal body, left and right of a vertical one, then its corners, then its other sides) that meets the least
    // of the other bodies, the flags, the knobs placed before, the pins, the junctions and the wires, in that order of
    // weight. `hw`, `hh`: its half width and height. Canvas-free, so a hit test finds it where the frame drew it.
    struct KnobSpot
    {
        int part = -1;                                   // Drawing::parts
        float hw = 8.0f, hh = 8.0f;
        funkgui::Point at{};
    };
    void placeKnobs(const View& v, const cir::Drawing& d, std::vector<KnobSpot>& knobs);
    void chip(funkgui::Canvas& c, funkgui::Point at, float w, float h, const std::string& text, funkgui::Col ink, const Inks& k);

    // The text a part shows as its value: the netlist's own spelling (R4's "4.7k", a pot's "100k {tone}", a switch's
    // "sw1: 1", an OTA's gain expression); "" for an op amp or a terminal.
    std::string valueText(const cir::Netlist& nl, const cir::Part& p);

    // Where labels() draws a part's value at its spot, on screen (empty when it draws none): a click there edits it.
    funkgui::Rect valueRect(const LabelSpot& s, const std::string& name, const std::string& value, float px);

    // ---- the problems on the drawing (the view's marks, a picture's) -------------------------------------------------
    // A message on the file, mapped to what it marks (the spec's table): an element's logical line marks its part (and
    // the pin whose node token the column falls on), a .pot line every part whose {expr} names it, a .model line every
    // diode of it, an .input or .output line its terminal; any other line marks nothing (the count only).
    struct Mark
    {
        int line = 0, col = 0;                           // 0-based
        int severity = 0;                                // 0 error, 1 warning, 2 note
        std::string text;
        std::vector<int> parts;                          // Drawing::parts it marks
        std::vector<std::pair<int, int>> pins;           // the pins its column falls on: (part, pin)
    };
    // `problems` (the file's messages, their lines 1-based, as the tab's decorations place them), in line order.
    std::vector<Mark> mapMarks(const cir::Netlist& nl, const cir::Drawing& d, const std::vector<Diagnostic>& problems);
    // Each part's marks (Drawing::parts' order) from them; a hand edit's overlap outlines both bodies.
    std::vector<PartMarks> partMarksOf(const cir::Drawing& d, const std::vector<Mark>& marks);

    // A drawing as it stands, with nothing of the pointer's, the selection's or a gesture's: the grid over `clip`, the
    // wires and junctions, the name tags and flags, the symbols and terminals, every name and value (at `px`; 0: none),
    // and the problems' marks (`marks`: Drawing::parts' order, or empty). view_panel's schematic picture. The parts
    // labelled: how many names were drawn.
    int still(funkgui::Canvas& c, const View& v, const cir::Netlist& nl, const cir::Drawing& d, const std::vector<PartMarks>& marks,
              const Inks& k, const funkgui::Rect& clip, float px);
}
