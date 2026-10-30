// Source/fminus/CirDoc.h: a .cir netlist as the schematic editor sees it (JUCE-, FunkGui- and FMinus-free, as Lang is,
// so every layer can use it: the editor, the chat's tools, the probes). The .cir file is the only source of truth: the
// netlist's lines are the circuit, and the drawing lives in `*@` lines, which FMinus, ngspice and LTspice all read as
// comments (FMinus's scan skips a line whose first non-blank character is `*`, joins `+` continuations across comment
// lines and stops at `.end`; its emitted module never copies a comment; the self-test's key drops comment lines). A
// change to `*@` lines alone is invisible to the program, to its messages and to what Claude and listen are told plays:
// sameProgram() below is the comparison every such site makes.
//
//   read()          the netlist's structure, tolerantly: its elements with their tokens as FMinus's Netlist.cpp reads
//                   them (split at blanks, `{...}` one token, `key = value` joined; a `;` ends a line), each drawn
//                   part's node tokens with their token indices (P lines by their first `{`, G lines with token 3
//                   OTA), the terminals (.input, .output; law inputs have no node), the pots, switches and models, and
//                   each node (folded; `gnd` is `0`) with its spelling and its pins in line order. Q, J, M, F, H, K, T,
//                   B, W and X lines that are not `opamp` are read (their nodes too) and never drawn.
//   readLayout()    the `*@` block, wherever its lines are (after `.end` too): the grammar below, read tolerantly (a
//                   malformed line is noted and ignored for what it cannot say; a second line for one part or net:
//                   the first wins).
//   reconcile()     the drawing of a text (pure): its lines bound by folded name, orphans adopted (a renamed part, net
//                   or terminal: one orphan line and one unplaced part of its kind letter), session pins, the rest
//                   placed (the full layout when nothing is, else incremental placement), every wired net repaired.
//   writeBlock()    the canonical block; withBlock() puts it in a text (every `*@` line removed, the block just
//                   before the first `.end` line, else at the end; never line 1; the file ending in one newline).
//   apply()         edits (§6.3 of the spec: add, place, connect, disconnect, cut, set, rename, delete, label, style,
//                   paste, layout) as the smallest patches of the text plus the regenerated block, all or none.
//   parseOp()       edit_circuit's grammar, a command a line.
//   stripLayout(), sameNetlist(), lineMap(), sameProgram()   the text helpers of "layout lines are invisible".
//
// The block (keywords and names compare case-folded; tokens split at blanks; a point is "x,y", -9999..9999):
//   *@ schematic 1 (the playground's drawing; SPICE reads *@ lines as comments)     the header; a version above 1
//                                                    makes the drawing read-only (never rewritten)
//   *@ input NAME X,Y              the signal .input's terminal, its pin at X,Y
//   *@ output NAME X,Y             the .output's
//   *@ part NAME X,Y [ORIENT] [key=value ...]        a part's symbol origin; r90 r180 r270 m0 m90 m180 m270 (r0 is
//                                                    read, never written); key=value pairs reserved, kept verbatim
//   *@ net NODE [flag] [label=X,Y ...] [ROUTE]       how a net is drawn: `flag`, name tags at its pins; else ROUTE,
//                                                    polylines `X,Y X,Y ...` (consecutive points share x or y)
//                                                    separated by `|`; label= points are name tags on the route
//   *@ WORD ...                    any other keyword ("probe" is reserved): kept verbatim
// Ground (0, gnd) and rails (a net a V element ties to ground) are always flags and get no line. The canonical form:
// the header, input, output, part lines in natural order of their folded names (R2 < R10; ties by bytes), net lines
// likewise, unknown lines in file order; names spelled as the netlist first spells them; one blank between tokens; r0
// left out; label= points sorted by (x, y) before the route; a route reduced to a unique form (unit edges merged into
// maximal straight segments, chained through clean corners, each chain from its smaller end, the chains sorted); wired
// nets with fewer than two pins, and lines that name nothing, left out.
//
// Pure functions of their arguments, on the calling thread (the message thread's view, the agent's worker for
// edit_circuit); nothing here is for the audio thread.
#pragma once

#include "dsp/Sources.h"
#include "fminus/CirLayout.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace playground::cir
{
    // ---- the netlist ------------------------------------------------------------------------------------------------
    struct Pos
    {
        int line = 0;                             // 0-based, as the editor counts
        int col = 0;                              // a byte offset into the line

        friend constexpr bool operator==(Pos a, Pos b) noexcept { return a.line == b.line && a.col == b.col; }
        friend constexpr bool operator<(Pos a, Pos b) noexcept { return a.line < b.line || (a.line == b.line && a.col < b.col); }
    };

    // One edit of the text: [from, to) becomes `text` (the editor makes a funkgui::TextEdit of it).
    struct Edit
    {
        Pos from{};
        Pos to{};
        std::string text;

        friend bool operator==(const Edit&, const Edit&) = default;
    };

    // A token as FMinus reads it: "R4", "{1 - tone}", "taper=audio" (key = value written apart is one token).
    struct Token
    {
        std::string text;
        Pos at{};                                 // its first byte
        int len = 0;                              // its bytes on that line (to the end of a joined key = value)
    };

    struct Element
    {
        std::string name;                         // token 0, as written
        Kind kind = Kind::Unknown;
        char letter = 0;                          // its first letter, folded ('r', 'x', ...)
        int firstLine = 0, lastLine = 0;          // its line and its last + continuation
        std::vector<Token> tokens;                // every token of its logical line
        std::vector<Token> nodes;                 // its node tokens, in token order (a part not drawn: its nodes too)
        std::vector<int> tokenOf;                 // each node's token index (the spec's table: FMinus's order)
        std::vector<std::string> pinNames;        // each node's pin name ("1", "inn"); "" for a part not drawn
        int valueToken = -1;                      // its value's token (R C L: 3; P: before the {; V, I: the last; E, G:
                                                  // the gain; D: its model; S: its positions), -1 for none
        std::string why;                          // not drawn: why ("a BJT: Phase 2"); empty when drawn

        bool drawn() const noexcept { return why.empty(); }
    };

    struct Terminal
    {
        std::string name;                         // .input NAME / .output NAME, as written
        bool input = true;
        Token nameToken;                          // token 1
        Token node;                               // token 2 (the signal input's and the output's)
        int line = 0;
        std::vector<Token> tokens;
    };

    struct PotLine
    {
        std::string name;                         // .pot NAME, as written
        std::string defaultText;                  // its default as written ("" when absent)
        std::string taper;                        // its taper word as written ("" when absent)
        int line = 0;
        std::vector<Token> tokens;
    };

    struct SwitchLine
    {
        std::string name;
        int positions = 0;
        int defaultPosition = 0;
        int line = 0;
        std::vector<Token> tokens;
    };

    // A pin on a node: an element's (its index in Netlist::elements and its node index), or a terminal's.
    struct NodePin
    {
        int element = -1;                         // -1: a terminal
        int pin = 0;                              // the element's node index; 0 for a terminal
        bool output = false;                      // a terminal: the output (else the signal input)
        int line = 0;
    };

    struct Node
    {
        std::string key;                          // folded; "gnd" is "0"
        std::string spelling;                     // as first written
        std::vector<NodePin> pins;                // in line order
        bool ground = false;
    };

    struct Netlist
    {
        std::vector<std::string> lines;           // the text's lines (split at LF; a CR before it left out)
        std::vector<Element> elements;            // in line order
        std::optional<Terminal> input;            // the signal .input (the first)
        std::optional<Terminal> output;
        std::vector<Terminal> laws;               // the law inputs (.input NAME - kind=...): no node, no symbol
        std::vector<PotLine> pots;
        std::vector<SwitchLine> switches;
        std::vector<std::pair<std::string, int>> models;   // .model NAME, its line
        std::vector<std::pair<std::string, int>> directives; // every directive's word (folded) and line, in order
        std::vector<Node> nodes;                  // in first-mention order
        int endLine = -1;                         // the first .end's line; -1: none
        int lastElementLine = -1;                 // the last element's last line; -1: none

        int node(std::string_view name) const noexcept;      // a node's index by name (any case; gnd is 0), or -1
        int element(std::string_view name) const noexcept;   // an element's index by name (any case), or -1
    };

    Netlist read(std::string_view text);

    // ---- the block ------------------------------------------------------------------------------------------------
    struct PartLine
    {
        std::string name;
        Pt at{};
        Orient orient = Orient::r0;
        std::vector<std::string> extra;           // its key=value tokens, verbatim
        int line = -1;
    };

    struct TerminalLine
    {
        std::string name;
        Pt at{};
        int line = -1;
    };

    using Polyline = std::vector<Pt>;

    struct NetLine
    {
        std::string node;
        bool flag = false;
        std::vector<Pt> labels;
        std::vector<Polyline> route;
        int line = -1;
    };

    struct Layout
    {
        int version = 0;                          // the header's; 0: no header (read as 1)
        std::vector<PartLine> parts;              // in file order (a second line for a name kept here, not used)
        std::vector<TerminalLine> inputs, outputs;
        std::vector<NetLine> nets;
        std::vector<std::string> unknown;         // other keywords' lines, "*@ probe ...", tokens joined by one blank
        std::vector<std::string> notes;           // what could not be read ("pedal.cir" is the caller's to add)
        std::vector<int> lines;                   // every *@ line's number (0-based)
    };

    Layout readLayout(std::string_view text);
    using SessionPins = Layout;                   // the places the view drew for parts that have no part line

    // ---- the drawing ------------------------------------------------------------------------------------------------
    enum class Source : uint8_t { stored, session, placed };   // a position from a part line, the session, the layout

    struct PinAt
    {
        std::string name;                         // its pin name ("2", "inn"; "" for a terminal's)
        int token = 0;                            // its netlist token
        Pt at{};                                  // in the world
        Dir out = Dir::E;
        int net = -1;                             // Drawing::nets
    };

    struct Part
    {
        std::string name;                         // as written: an element's name, or a terminal's
        Kind kind = Kind::Unknown;                // Input, Output for a terminal
        int element = -1;                         // Netlist::elements; -1 for a terminal
        bool placed = false;                      // has a position (every part has one once reconciled)
        Source source = Source::placed;
        Pt origin{};
        Orient orient = Orient::r0;
        Box body{};                               // in the world
        std::vector<PinAt> pins;                  // in token order
        std::vector<std::string> extra;           // its part line's key=value tokens
    };

    enum class Style : uint8_t { wired, flag, ground, rail };

    // A unit edge of a route: from `a` to a + (1, 0) (horizontal) or a + (0, 1).
    struct UnitEdge
    {
        Pt a{};
        bool h = true;

        friend constexpr bool operator==(UnitEdge x, UnitEdge y) noexcept { return x.a == y.a && x.h == y.h; }
        friend constexpr bool operator<(UnitEdge x, UnitEdge y) noexcept
        {
            return x.a < y.a || (x.a == y.a && x.h < y.h);
        }
    };

    struct PinRef
    {
        int part = -1;                            // Drawing::parts
        int pin = 0;                              // its pins
        friend constexpr bool operator==(PinRef a, PinRef b) noexcept { return a.part == b.part && a.pin == b.pin; }
    };

    struct Net
    {
        std::string name;                         // as first written
        std::string key;                          // folded ("0" for ground)
        Style style = Style::wired;               // as drawn: wired, flag (name tags), ground or rail flags
        bool positive = true;                     // a rail: positive (its flag above), else negative
        bool stored = false;                      // a net line named it
        bool unroutable = false;                  // no route within the budget: drawn as name tags, written `flag`
        int totalPins = 0;                        // its pins in the netlist (parts not drawn too)
        std::vector<PinRef> pins;                 // its drawn pins, in line order
        std::vector<UnitEdge> edges;              // its route, sorted
        std::vector<Pt> labels;                   // label= points
        std::vector<Pt> junctions;                // points with three or more unit edges
    };

    struct Flag
    {
        PinRef pin;
        Pt at{};                                  // the pin
        Dir out = Dir::S;
        int net = -1;
        Box box{};                                // what it covers: 1 to 2 units out, 1 either side
    };

    struct Drawing
    {
        std::vector<Part> parts;                  // the drawn elements in line order, then the input, the output
        std::vector<Net> nets;                    // Netlist::nodes' order
        std::vector<Flag> flags;
        std::vector<std::string> unknown;         // the block's other lines, kept
        std::vector<std::string> notes;           // the view's notes (never a compile's): orphans, unroutable nets
        std::vector<std::string> notDrawn;        // "Q1 (a BJT: Phase 2)"
        std::vector<std::pair<std::string, std::string>> overlaps;   // bodies, terminals or flags that overlap
        bool readOnly = false;                    // a block of a newer version: drawn, never rewritten
        int version = 1;
        Budget budget;                            // the A* pops this reconcile spent

        int part(std::string_view name) const noexcept;   // a part by name (any case): elements first, then terminals
        int net(std::string_view name) const noexcept;    // a net by node name (any case; gnd is 0)
        Box bounds() const noexcept;              // every body, flag and route point
    };

    Drawing reconcile(std::string_view text, const SessionPins& pins = {});
    // The same from a read netlist and layout (apply's working state: every position is a part line).
    Drawing reconcile(const Netlist& nl, const Layout& layout, const SessionPins& pins = {});

    // The drawing as a block's lines (every part placed, every route): what writeBlock writes, as a Layout.
    Layout layoutOf(const Netlist& nl, const Drawing& d);
    std::string writeBlock(const Netlist& nl, const Drawing& d);   // §4.4: each line ends in LF
    // `text` with every *@ line removed and `block` placed (§4.4.6).
    std::string withBlock(std::string_view text, std::string_view block);
    // `text` with its drawing written: what the first gesture on a text-only netlist writes (the drawing reconciled).
    std::string written(std::string_view text, const SessionPins& pins = {});

    // ---- edits ------------------------------------------------------------------------------------------------------
    struct End
    {
        std::string part;                         // Part.pin: the part ("" for a node)
        std::string pin;                          // its pin's name or token number
        std::string node;                         // a node's name (0 or gnd: ground)
    };

    struct Op
    {
        enum class Type : uint8_t
        {
            add,         // lines: an element line (with the .pot or .switch line it needs before it) or a directive
            place,       // name at [orient]
            connect,     // a b [path]: merges two nets; the drawn path joins the route
            disconnect,  // a (a Part.pin): that pin alone on a fresh node
            cut,         // node, path: those wires of the net taken out; it splits where it falls apart
            set,         // name value, or name key=value
            rename,      // name to: a part (its name token and part line) or a node (every token of it)
            remove,      // names: parts and terminals
            label,       // node, points: its name tags
            style,       // node, flag: name tags at its pins, or wires
            paste,       // text at: netlist lines and *@ lines, names made fresh
            layout,      // names: placed again; none: the full layout of everything (Tidy)
        };
        Type type = Type::place;
        std::vector<std::string> lines;           // add: its lines; paste: the text's lines
        std::string name;                         // place, set, rename, disconnect (a Part.pin), cut, label, style
        std::vector<std::string> names;           // remove, layout
        std::optional<Pt> at;                     // add, place, paste
        std::optional<Orient> orient;             // add, place
        End a, b;                                 // connect
        std::vector<Pt> path;                     // connect, cut: a polyline of the wires drawn or taken out
        std::string key;                          // set: "" for the value, else model, taper, pot, gain, switch,
                                                  // positions, scale
        std::string value;                        // set
        std::string to;                           // rename
        std::vector<Pt> points;                   // label
        bool flag = false;                        // style
        std::string text;                         // as written (parseOp's line), for messages
    };

    struct Applied
    {
        std::vector<Edit> edits;                  // on the text given, sorted, none overlapping or touching
        std::string text;                         // the text after them
        std::string error;                        // "op 2 (connect R9.1 n3): there is no part R9"; edits empty
        std::vector<std::string> touched;         // the parts and nets an op changed
        std::vector<std::string> said;            // a line an op ("added R5 at 12,4 r90 on n3, 0")
    };

    // `ops` applied to `text` (drawn as `d`: reconcile(text, pins)) in order, all or none: the netlist's lines patched
    // token by token, then the canonical block written in. A drawing of a newer version refuses every op.
    Applied apply(std::string_view text, const Drawing& d, std::span<const Op> ops, const SessionPins& pins = {});

    // edit_circuit's grammar, one command a line:
    //   add <netlist line> [at X,Y [orient]]      an element line (R C L D P X E G V I S) or .pot .switch .model
    //                                             .input .output
    //   place <Part|Terminal> X,Y [orient]
    //   connect <end> <end>                       end: Part.pin (Part.k: token k) or a node's name (0, gnd: ground)
    //   disconnect <Part.pin>
    //   set <Part> <value | {expr}>  |  set <Part> <key>=<value>   model taper pot gain switch positions scale
    //   rename <Part|node> <new name>
    //   delete <Part> {<Part>}
    //   layout [<Part> {<Part>}]                  none: everything laid out again; else those placed again
    std::optional<Op> parseOp(std::string_view line, std::string& why);

    // The lines a part of `kind` placed from the toolbar writes, in order (a pot's or switch's directive, then its
    // element), with the next free name and fresh nodes (§5: "R3 n1 n2 10k", ".pot pot2 0.5 lin" and
    // "P2 n4 n5 n6 100k {pot2}"); its name in `name`.
    std::vector<std::string> placementLines(const Netlist& nl, Kind kind, std::string* name = nullptr);
    std::string freshNode(const Netlist& nl);     // "n<k>": the smallest k no node has (any case)

    // ---- the text helpers -------------------------------------------------------------------------------------------
    bool isLayoutLine(std::string_view line) noexcept;   // its first non-blank characters are "*@"
    // `text` without its *@ lines, ending in a line break when anything is left (FMinus reads both the same).
    std::string stripLayout(std::string_view text);
    bool sameNetlist(std::string_view a, std::string_view b);
    // When sameNetlist(from, to): for each line of `from`, the line of `to` with the same ordinal among the lines that
    // are not *@ lines (-1 for a *@ line, or past `to`'s last); else empty.
    std::vector<int> lineMap(std::string_view from, std::string_view to);
    // The same files in the same order, a .cir compared by sameNetlist and every other file byte for byte, each text
    // as the editor keeps it (sanitised: below).
    bool sameProgram(const Sources& a, const Sources& b);
    // The text as funkgui::TextEditor::sanitise keeps it (CR LF and CR become LF; tabs and LF kept; any other
    // character that is not printable ASCII, a UTF-8 sequence counted once, becomes '?'), without FunkGui.
    std::string sanitised(std::string_view text);

    // Names: folded (ASCII lower case), and natural order (digit runs as numbers, then bytes).
    std::string folded(std::string_view s);
    bool naturalLess(std::string_view a, std::string_view b) noexcept;
}
