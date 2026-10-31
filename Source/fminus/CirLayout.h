// Source/fminus/CirLayout.h: a .cir netlist's drawing, its geometry (JUCE-, FunkGui- and FMinus-free, as Lang is): the
// grid, the eight orientations, the part library's symbols, and the layout engine CirDoc's reconcile runs.
//
// The grid. One unit is one pin pitch (10 logical px at 100 % in the editor); x grows to the right, y down. A part's
// point is its symbol's origin, a terminal's its pin.
//
// Orientation. LTspice's eight SYMBOL codes, applied to a point (x, y) of the symbol's own frame: mN first mirrors
// x -> -x; then the part turns N degrees clockwise on screen: r90 maps (x, y) to (-y, x), r180 to (-x, -y), r270 to
// (y, -x). So r90 puts a two-pin part's first pin on top, and m180 is the up-down flip.
//
// The symbols (the part library, ANSI style). Pins sit on the body box's edge, in grid units at r0;
// a pin is named as the tools name it (R4.2, D1.k, Ptone.w, XU1.inn), and `Part.k` means netlist token k: the pins are
// listed in token order, FMinus's (its Netlist.cpp), not the drawing's top to bottom:
//   R, C, L, S        1 (-2,0) W, 2 (2,0) E                       body (-2,-1)..(2,1)
//   D                 a (-2,0) W, k (2,0) E                       as R
//   P (a pot)         e1 (-2,0) W, w (0,-2) N, e2 (2,0) E         (-2,-2)..(2,1)
//   P (a rheostat)    a (-2,0) W, b (2,0) E                       as R
//   X (an op amp)     inp (-3,1) W, inn (-3,-1) W, out (3,0) E     (-3,-2)..(3,2)
//   E, G (linear)     p (1,-2) N, n (1,2) S, cp (-3,-1) W, cn (-3,1) W
//   G ... OTA         p (0,2) S, n (3,0) E (its current goes into n-: the output), cp (-3,1) W, cn (-3,-1) W
//   V, I              p (-2,0) W, n (2,0) E                       as R
//   the input         its pin (0,0) E                             (-3,-1)..(0,1)
//   the output        its pin (0,0) W                             (0,-1)..(3,1)
// Ground and rails are flags at their pins, never wires: drawn 2 units out along the pin's outward direction.
//
// The engine (CirLayout.cpp): the full layout of a netlist with no position (classify, columns by breadth-first search
// from the input, three barycentre sweeps, orientation by score, shunts and feedback rows as satellites, coordinates),
// incremental placement of parts without a position (stable: it moves nothing placed), and repair of every wired net
// (its stored route pruned of what is no longer valid, then extended by A* on the grid: a step 1, a bend 4, a crossing
// 8). Integer arithmetic only, every sort with a total key, no hash map iterated, budgets counted (A* pops, the grid's
// points), never timed: the same text gives the same drawing on every machine.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace playground::cir
{
    struct Drawing;
    struct Netlist;

    // ---- the grid -----------------------------------------------------------------------------------------------------
    struct Pt
    {
        int x = 0;
        int y = 0;

        friend constexpr bool operator==(Pt a, Pt b) noexcept { return a.x == b.x && a.y == b.y; }
        friend constexpr bool operator!=(Pt a, Pt b) noexcept { return !(a == b); }
        // (x, y) order: the canonical form's
        friend constexpr bool operator<(Pt a, Pt b) noexcept { return a.x < b.x || (a.x == b.x && a.y < b.y); }
        friend constexpr Pt operator+(Pt a, Pt b) noexcept { return { a.x + b.x, a.y + b.y }; }
        friend constexpr Pt operator-(Pt a, Pt b) noexcept { return { a.x - b.x, a.y - b.y }; }
    };

    inline constexpr int kCoordLimit = 9999;      // a stored coordinate is -9999..9999
    // A part's (or a terminal's) point is within -9979..9979: its body and pins reach 3 units further, the router's
    // wires at most 16 beyond the drawing, so that everything a write says reads back. A part line past it is placed
    // again (with a note), placement keeps within it, and an op that would go past it is refused.
    inline constexpr int kPartLimit = kCoordLimit - 20;
    constexpr bool withinReach(Pt p) noexcept
    {
        return p.x >= -kPartLimit && p.x <= kPartLimit && p.y >= -kPartLimit && p.y <= kPartLimit;
    }

    enum class Dir : uint8_t { E, S, W, N };
    constexpr Pt unit(Dir d) noexcept
    {
        return d == Dir::E ? Pt{ 1, 0 } : d == Dir::S ? Pt{ 0, 1 } : d == Dir::W ? Pt{ -1, 0 } : Pt{ 0, -1 };
    }
    constexpr Dir opposite(Dir d) noexcept { return static_cast<Dir>((static_cast<int>(d) + 2) % 4); }
    constexpr bool horizontal(Dir d) noexcept { return d == Dir::E || d == Dir::W; }

    // A closed box of grid points, [x0, x1] x [y0, y1].
    struct Box
    {
        int x0 = 0, y0 = 0, x1 = -1, y1 = -1;

        constexpr bool empty() const noexcept { return x1 < x0 || y1 < y0; }
        constexpr bool contains(Pt p) const noexcept { return p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1; }
        constexpr bool meets(const Box& o) const noexcept
        {
            return !empty() && !o.empty() && x0 <= o.x1 && o.x0 <= x1 && y0 <= o.y1 && o.y0 <= y1;
        }
        constexpr Box grown(int k) const noexcept { return { x0 - k, y0 - k, x1 + k, y1 + k }; }
        constexpr Box moved(Pt d) const noexcept { return { x0 + d.x, y0 + d.y, x1 + d.x, y1 + d.y }; }
        Box joined(const Box& o) const noexcept;  // the smallest box holding both (an empty one adds nothing)
        static Box of(Pt a, Pt b) noexcept;       // the box with these corners, in any order
        friend constexpr bool operator==(const Box& a, const Box& b) noexcept
        {
            return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
        }
    };

    // ---- orientation --------------------------------------------------------------------------------------------------
    enum class Orient : uint8_t { r0, r90, r180, r270, m0, m90, m180, m270 };
    inline constexpr std::array<Orient, 8> kOrients{ Orient::r0, Orient::r90, Orient::r180, Orient::r270,
                                                     Orient::m0, Orient::m90, Orient::m180, Orient::m270 };
    const char* orientName(Orient o) noexcept;    // "r0", "r90", ... "m270"
    std::optional<Orient> orientOf(std::string_view word) noexcept;   // any case; "r0" is read, never written
    Pt turned(Orient o, Pt p) noexcept;           // a symbol-frame point in the part's frame
    Dir turned(Orient o, Dir d) noexcept;
    Box turned(Orient o, const Box& b) noexcept;
    // `o` turned 90 degrees clockwise (counter-clockwise: back), or mirrored left to right, as Cmd-R, Cmd-Shift-R and
    // Cmd-E turn a part on screen.
    Orient rotatedCw(Orient o) noexcept;
    Orient rotatedCcw(Orient o) noexcept;
    Orient mirrored(Orient o) noexcept;

    // ---- the part library -----------------------------------------------------------------------------------------------
    enum class Kind : uint8_t { R, C, L, D, Pot, Rheostat, OpAmp, Vcvs, Vccs, Ota, V, I, S, Input, Output, Unknown };
    inline constexpr int kPlaceableKinds = 13;    // R .. S: what the toolbar places (Input, Output: terminals)

    struct PinDef
    {
        const char* name = "";                    // as the tools name it: "1", "a", "inn", "e1"
        int token = 0;                            // the netlist token it is (the element's name is token 0)
        Pt at{};                                  // at r0, from the origin
        Dir out = Dir::E;                         // its outward direction at r0
    };

    struct Symbol
    {
        Kind kind = Kind::Unknown;
        const char* word = "";                    // "resistor", "op amp", ... (the a11y text, the status line)
        const char* key = "";                     // its toolbar key: "R", "Shift-P", "O"
        std::array<PinDef, 4> pins{};             // in token order
        int pinCount = 0;
        Box body{};                               // at r0, from the origin
    };

    const Symbol& symbol(Kind k) noexcept;        // Unknown: no pins, no body
    // The pin of `k` named `name` (any case) or written as its token number ("2": token 2), else -1.
    int pinIndex(Kind k, std::string_view name) noexcept;
    // The kind's letter in the netlist ('R', ..., 'X' for an op amp, 'G' for a VCCS and an OTA); 0 for a terminal.
    char letterOf(Kind k) noexcept;

    // ---- the engine (CirLayout.cpp; CirDoc's reconcile and apply call it) -------------------------------------------------
    struct Part;
    // `p` with its origin at `origin`, turned `o`: its body and its pins in the world; placed.
    void put(Part& p, Pt origin, Orient o) noexcept;
    // What a flag (ground, a rail, a name tag) at a pin covers: 1 to 2 units out along `out`, 1 either side.
    Box flagBox(Pt pin, Dir out) noexcept;

    // A* pops are counted, never timed: at most kPopsPerConnection a connection (in the bounds + 8, then again in the
    // bounds + 16), kPopsPerLayout a reconcile; past them a net is drawn as labels, with a note.
    inline constexpr long kPopsPerConnection = 50000;
    inline constexpr long kPopsPerLayout = 2000000;
    // The router's grid is the drawing's box (bodies and stored wires) and 18 units around it, at most kRouteCells
    // points (a 200-part drawing spans about 50,000), so that its memory and each search's setup stay bounded as the
    // pops bound the searches. Past that, the parts' box alone is the grid (a stored wire outside it is dropped and its
    // net routed again); a drawing whose parts alone span more (one moved thousands of units away) routes nothing:
    // every wired net is drawn as name tags, with one note, and an op that would make a drawing so is refused.
    inline constexpr long kRouteCells = 1L << 18;

    struct Budget
    {
        long pops = 0;                            // spent in this reconcile
        long most = 0;                            // the most one connection took
        long cells = 0;                           // the router's grid, in points (the last pass's)
        bool wide = false;                        // the parts span more than kRouteCells: nothing routed
    };

    // Every part of the drawing laid out from nothing (spec 7.2-7.7), every route cleared.
    void layoutAll(Drawing& d, const Netlist& nl);
    // Every part without a position placed, in natural order, without moving anything placed (spec 7.9); an island
    // (parts no placed pin or route reaches) gets the full layout of its own, 6 units right of the rest, top-aligned.
    void placeRest(Drawing& d, const Netlist& nl);
    // Every wired net's route repaired (spec 7.8): what stays valid kept, the rest pruned, the pins connected by A*.
    void repairAll(Drawing& d, Budget& budget);
}
