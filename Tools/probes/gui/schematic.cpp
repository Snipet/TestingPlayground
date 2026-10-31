// PROBE name=gui.schematic timeout=300
//
// gui.schematic: a .cir tab's Schematic view (editor/SchematicView.h, SchematicDraw.h), headless over a real Processor,
// driven by the pointer and the keys as a user would. It writes, to <build>/probe-out/<key>/:
//   gui.schematic.png          diodetone.cir fitted on opening, GRAPHITE
//   gui.schematic.screamer.png the Tube Screamer's netlist (screamer.cir), laid out from its text alone
//   gui.schematic.paper.png    PAPER, a part and a wire selected, an error, a warning and a note marked
//   gui.schematic.small.png    the least window (1000 x 600), 32 dials folded into the bar under the drawing
//   gui.schematic.switch.png   the least window, twelve tabs and the IR tab: the pills icon-only, no tab under its least
//   gui.schematic.wire.png     a wire being drawn, from a pin, mid-drag
//   gui.schematic.move.png     two parts being moved, their wires rubber-banded, mid-drag
//   gui.schematic.errors.png   an error, a warning and a note on parts (badges, an outline, a ring), an unconnected pin,
//                              pins that touch (a dot) and a hand edit's overlap (both outlined)
//   gui.schematic.empty.png    an empty netlist: the hint and Start a circuit
//   gui.schematic.zoom25.png   25 %: symbols as their boxes, no labels, no grid
//   gui.schematic.zoom400.png  400 %
//   gui.schematic.dials.png    a program driving screamer.cir's pots with dials: a knob beside each pot (fitted)
//   gui.schematic.play.png     a netlist no program #included, played: program.fm now plays it, Undo in the info strip
//   gui.schematic.own.png      a program that calls screamer.cir's set() itself: no knobs, the info strip saying why
//   gui.schematic.probes.png   Play this circuit's program for diodetone.cir playing (an effect's 1 kHz sine, a synth's
//                              A4): net a's readout card and two pinned lanes (c, b), each a steady triggered trace
// and checks:
//   VIEW      the pills (Netlist, Schematic) at the tab bar's right end while a .cir tab shows, labelled at the
//             default size; a click shows the drawing: opening fits it, never under 60 %; the status line's words; the
//             parts and wires drawn inside the canvas only; nothing missing from the font; Cmd-Shift-D; the find bar
//             closed by the drawing (switched to, or its tab shown)
//   GESTURES  each gives the exact text as one undo step: placing every kind (by click), a wire from pin to pin, a
//             wire into a mid-wire T, a wire dragged from a wire point, the Ground tool, a move, a refused overlap (no
//             change), rotate, mirror, delete (its stubs pruned), a deleted wire splitting a net, Start a circuit; the
//             info strip's notices (a placement says nothing, a merge is said until the text moves); a netlist of parts
//             the view cannot draw (no Start, the Not drawn banner before the count)
//   UNDO      Cmd-Z and Cmd-Shift-Z in either view undo and redo the other's steps; so do the toolbar's Undo and Redo
//   OVERTAKEN a drag whose base a merge (Claude's write) moves is cancelled and changes nothing; that write moving a
//             part's line flashes the part; a gesture made after Claude's write and before a frame merged it is undone
//             (the write wins, the strip says to do it again); text typed in one file and Claude's write of another in
//             one frame, before the editor gave its files: both kept, the typing given to the processor
//   POINTER   a row per cell of the pointer's table: idle on a body (at 30 % too, where its pins would cover it), a
//             pin, a wire, nothing; Alt (a click on a wire pins
//             its net to a probe, again unpins it; a drag pans); carrying a part (a
//             popup click turns the ghost, a popup drag pans and carries on, placing ends only on Escape, Select or
//             another tool); the Ground tool; the Label tool's drag nothing; a badge
//   KEYS      edge auto-pan's units in a fixed number of ticks; the PANEL view ending a part carried, the hidden
//             drawing waiting; Escape's order and leaving the canvas; Tab in line order; Cmd-Shift-D in either view,
//             and nothing while the IR tab shows; the parts' keys away from the canvas place at once
//   ROOM      the canvas at least 240 px tall at 1000 x 600 with 32 dials (folded) and 9 (shown), at 1360 x 760 with
//             the explorer open and 32 shown; the fold's bar shows and hides them (every one shown, the editor taking
//             what is left down to its 80 px floor); a wire selected and not: the canvas the same; the IR tab narrowed
//             beside the pills opens on a click anywhere (no close box)
//   MARKS     the badges', the outline's and the ring's colours are the tab editor's error, warning and note inks (both
//             themes); a layout-only gesture leaves the status RUNNING, asks for no check and keeps a planted error's
//             badge and squiggle; a problems row's click (a netlist shown in its Schematic view) selects and centres
//             the part; the info strip's count steps through them; the structural marks (kMarks): an unconnected pin's
//             open circle, none on a ground pin alone on 0, one dot where pins of two nets touch, both bodies of a hand
//             edit's overlap outlined
//   MENUS     the part's, the wire's, the canvas's and More's menus (pendingMenu), each item working (chooseMenuItem)
//   A11Y      the group, an item a part and a net, their ids the same across a pan and a zoom, the revision too, every
//             item visible, at 400 % too with parts off the canvas (bounds on it); the focus action selects and centres
//   DIALS     a program holding screamer.cir's module, never calling its set(): a knob beside Pdrive, Ptone and Plevel
//             on the slots of their dials; a drag up on one turns its dial (the program card's) and leaves the text
//             alone, the info strip saying its value; a double-click its default, closing the drag its second press
//             began; the body still moves the part; a switch's chip steps its position; a program that calls the
//             module's set() itself gets no knobs, and the info strip says why
//   PLAY      a netlist no program #includes: the banner's Play this circuit writes program.fm (CirTemplates.h's host
//             program, the library tabs it needs), runs it, the view staying on the netlist, and offers Undo, which
//             gives program.fm's text back in one step; a probe's reason the strip draws (a node typed since RUN)
//             hides the Undo with the note it takes back, and a wire that resolves leaves both
//   NOTES     the drawing's notes in the info strip (a *@ part line it cannot read); a part's value spelled out there
//             (2.2k = 2,200 ohms, 10n = 10 nanofarads); a netlist's law inputs in the status words (otalp.cir's iabc)
//   PERSIST   nothing kept by opening; a Panel opened again keeps each file's view, zoom and pan (the session's views);
//             a view switched but never zoomed nor panned keeps no zoom, and fits again when opened again; a text-only
//             netlist's session pins, after Claude added a part by text: a view_panel picture leaves them as they were,
//             and an editor opened again draws every part where the user's did
//   PROBES    the live probes (SchematicView.h's "Live probes", Engine.h's "Probes") on Play this circuit's program
//             for diodetone.cir: a wire selected while what plays lacks the netlist says "not playing" (drawn in the
//             strip, spoken) and moves neither the canvas nor the frame rate; after RUN a node typed since says it is
//             not in the program that plays, screamer.cir's t1 why a probe cannot read it, ground (Alt-click on a
//             grounded pin) that it is 0 V, a netlist no program #includes that it is not played, one the program
//             #includes without a field of its module that it has none, and the synth with no note that no voice
//             sounds; playing, slot 0 follows the selected wire and Alt-click pins two nets (their
//             numbered flags), the card and the lanes taking at most 40 % of the canvas, their numbers the meters'
//             within 1 %, each trace triggered on a rising crossing of its mean (the same window two frames apart) and
//             ranged in a power of two volts, the readouts accessibility items (their values the numbers); with the
//             meters standing still the view settles; the synth's "no voice sounding" is spoken once, not after each
//             note let go; the wire's menu pins and unpins, and the effect's chip chooses amp[1] (and back); a
//             view_panel window picture sends no set and a second Panel opened and closed leaves the set as it was, and
//             a Panel that owns none (a picture's) reads the slots it shares with the user's set live; the
//             Netlist view clears the set and the Schematic view arms the pins again (the session keeps them), a RUN
//             arms them on the new program; in host blocks of 2048 an effect's scope shows each block's newest 1,024,
//             triggered inside them (never two blocks joined), a synth's whole windows; and the Panel's going clears them
//   EDIT      each kind's field (all 13 kinds and both terminals) writing the exact tokens as one undo step, its rows,
//             the taper FMinus plays (a line's taper= before its .pot's word), a row past 64 characters left alone;
//             E12 steps; refusals; 1M's warning; the Label tool; Show as labels (its tags overlapping nothing new);
//             copy, paste, cut, duplicate (where the copy meets nothing), text no netlist's and prose refused; each new
//             menu item
#include "Probe.h"

#include "editor/CodeAssist.h"
#include "editor/Panel.h"
#include "editor/SchematicDraw.h"
#include "editor/SchematicView.h"
#include "editor/Theme.h"
#include "fminus/CirDoc.h"
#include "fminus/CirTemplates.h"
#include "fminus/LibraryIncludes.h"
#include "plugin/ChatAgent.h"
#include "plugin/ClaudeApi.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"

#include "Examples.h"
#include "Library.h"
#include "Product.h"

#include <funkgui/canvas/Prim.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/widgets/TextEditor.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace pg = playground;
namespace cir = playground::cir;
namespace sch = playground::ui::sch;
using SV = pg::ui::SchematicView;

namespace
{
    std::string libraryText(const char* name)
    {
        const pg::LibraryFile* f = pg::findLibraryFile(name);
        return f != nullptr ? std::string(f->source) : std::string();
    }

    void writePng(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const char* name, const std::string& key)
    {
        const std::string path = probe::outPath(name);
        host.draw();
        P.eq(key + ".png_written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
    }

    int countTag(const funkgui::PrimList& frame, funkgui::Tag tag)
    {
        int n = 0;
        for (const funkgui::Prim& p : frame.prims)
            n += p.tag == tag ? 1 : 0;
        return n;
    }

    uint32_t packed(funkgui::Col c) { return uint32_t{ c.r } | (uint32_t{ c.g } << 8) | (uint32_t{ c.b } << 16) | (uint32_t{ c.a } << 24); }

    // A program of the product's kind with `dials` parameters, #including the netlist.
    std::string dialsProgram(const std::string& cir, int dials)
    {
        std::string params;
        for (int k = 0; k < dials && k < pg::kMaxControls; ++k)
            params += "    param float p" + std::to_string(k) + " [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n";
        const std::string inc = "#include \"" + cir + "\"\n";
        if constexpr (pg::product::kIsInstrument)
            return inc + "module Voice\n{\n" + params + "    float note;\n    void start(float n, float v) { note = n; }\n"
                   "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n";
        else
            return inc + "module Effect\n{\n" + params + "    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
    }

    // Ticks (and lets the compiler's thread work) until `done`, a second and a half at most.
    bool waitFor(funkgui::HeadlessHost& host, const std::function<bool()>& done)
    {
        for (int k = 0; k < 300; ++k)
        {
            host.tick(1);
            if (done())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    // A planted netlist: the input u on in, R1 in-a, C1 a-0, R2 a-out, the output vout on out.
    const std::string kPlanted = "* g.cir: a planted netlist\n"
                                 ".input  u in\n"
                                 ".output vout out\n"
                                 "R1 in a 1k\n"
                                 "C1 a 0 10n\n"
                                 "R2 a out 2.2k\n"
                                 "R3 out 0 10k\n"
                                 ".end\n";
    // ... and one to wire: R2's first pin alone on b.
    const std::string kLoose = "* g.cir: a planted netlist\n"
                               ".input  u in\n"
                               ".output vout out\n"
                               "R1 in a 1k\n"
                               "C1 a 0 10n\n"
                               "R2 b out 1k\n"
                               ".end\n";
    // A net of four pins (m), drawn by hand (fminus.schematic's ladder).
    const std::string kLadder = "* g.cir: a planted netlist, drawn\n"
                                ".input u in\n"
                                ".output y o\n"
                                "R1 in m 1k\n"
                                "C1 m 0 10n\n"
                                "C2 m 0 22n\n"
                                "R2 m o 1k\n"
                                "*@ schematic 1 (the playground's drawing; SPICE reads *@ lines as comments)\n"
                                "*@ input u 0,0\n"
                                "*@ output y 27,0\n"
                                "*@ part C1 10,5 r90\n"
                                "*@ part C2 14,5 r90\n"
                                "*@ part R1 6,0\n"
                                "*@ part R2 21,0\n"
                                "*@ net in 0,0 4,0\n"
                                "*@ net m 8,0 19,0 | 10,0 10,3 | 14,0 14,3\n"
                                "*@ net o 23,0 27,0\n"
                                ".end\n";

    // The structural marks, drawn by hand: R3.1 alone on b; C1.2 the only pin on ground; R4 and R5 placed so that R4.2
    // (on d) and R5.2 (on c) touch at one point, their bodies overlapping (c and d then unroutable: name tags). R1, C1
    // and R2 on lines 4 to 6, as in kPlanted (the planted messages' lines).
    const std::string kMarks = "* g.cir: a planted netlist, marked\n"
                               ".input  u in\n"
                               ".output vout out\n"
                               "R1 in a 1k\n"
                               "C1 a 0 10n\n"
                               "R2 a out 2.2k\n"
                               "R3 b out 10k\n"
                               "R4 c d 1k\n"
                               "R5 d c 1k\n"
                               "*@ schematic 1 (the playground's drawing; SPICE reads *@ lines as comments)\n"
                               "*@ input u 0,0\n"
                               "*@ output vout 31,0\n"
                               "*@ part C1 10,5 r90\n"
                               "*@ part R1 6,0\n"
                               "*@ part R2 17,0\n"
                               "*@ part R3 25,0 r180\n"
                               "*@ part R4 20,8\n"
                               "*@ part R5 22,10 r270\n"
                               "*@ net a 8,0 15,0 | 10,0 10,3\n"
                               "*@ net in 0,0 4,0\n"
                               "*@ net out 19,0 23,0 | 22,0 22,-2 30,-2 30,0 31,0\n"
                               ".end\n";

    // A part of every kind the schematic draws, and the terminals (not one circuit: what the fields edit).
    const std::string kKinds = "* g.cir: a part of every kind\n"
                               ".input  u in\n"
                               ".output vout out\n"
                               ".pot tone 0.5 lin\n"
                               ".switch mode 2 0\n"
                               ".model 1N34A D(IS=2.6e-6 N=1.6 RS=7)\n"
                               "R1 in a 1k\n"
                               "R2 a 0 10k\n"
                               "C1 a 0 10n\n"
                               "L1 a b 10m\n"
                               "D1 b 0 1N914\n"
                               "P1 b w out 100k {tone}\n"
                               "P2 c x 50k {1-tone} taper=audio\n"
                               "XU1 c w d opamp\n"
                               "E1 e 0 c 0 10\n"
                               "G1 f 0 c 0 1m\n"
                               "G2 0 g OTA c 0 {100u * tone}\n"
                               "V1 vb 0 DC 9\n"
                               "I1 h 0 DC 1m\n"
                               "S1 h 0 mode 1\n"
                               ".end\n";

    cir::Op setOp(const std::string& part, const std::string& key, const std::string& value)
    {
        cir::Op op;
        op.type = cir::Op::Type::set;
        op.name = part;
        op.key = key;
        op.value = value;
        return op;
    }

    cir::Op renameOp(const std::string& from, const std::string& to)
    {
        cir::Op op;
        op.type = cir::Op::Type::rename;
        op.name = from;
        op.to = to;
        return op;
    }

    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    // Each character of `text` typed (a comma cannot be: paste it).
    void typed(funkgui::HeadlessHost& host, const std::string& text)
    {
        for (const char ch : text)
            host.keys(std::string(1, ch).c_str());
    }

    // A processor, a Panel on it and a headless host; the processor's draft set first.
    struct Rig
    {
        pg::Processor processor;
        std::unique_ptr<pg::ui::Panel> panel;
        std::unique_ptr<funkgui::HeadlessHost> host;

        Rig(const pg::Draft& d, int theme = 0, int w = 0, int h = 0)
        {
            processor.editorPort().setDraft(d);
            open(theme, w, h);
        }
        ~Rig() { close(); }
        Rig(const Rig&) = delete;
        Rig& operator=(const Rig&) = delete;

        void open(int theme = 0, int w = 0, int h = 0)
        {
            panel = std::make_unique<pg::ui::Panel>(processor.ports(), processor.editorPort());
            host = std::make_unique<funkgui::HeadlessHost>(*panel, theme, 2.0f);
            host->setZoom({ 100, 125, 150, 200 }, 100);
            if (w > 0)
                host->resize(w, h);
            host->settle();
        }
        void close()
        {
            host.reset();
            panel.reset();
        }
        SV& view() { return panel->schematic(); }
        funkgui::TextEditor& ed() { return panel->editor(); }
        void run()
        {
            panel->runNow();
            processor.settle();
            host->tick(2);
            host->settle();
        }
    };

    pg::Draft draftWith(const std::string& program, const std::vector<std::pair<std::string, std::string>>& files, int shown)
    {
        pg::Draft d;
        d.files.push_back({ "program.fm", program });
        for (const auto& [n, t] : files)
            d.files.push_back({ n, t });
        d.active = shown;
        return d;
    }

    // The drawing at 100 %, centred on the canvas.
    void frame(SV& v, funkgui::HeadlessHost& host)
    {
        const funkgui::Rect c = v.canvasRect();
        v.zoomTo(1.0f, { c.centreX(), c.centreY() });
        const cir::Box b = v.drawing().bounds();
        const funkgui::Point a = v.screenOf({ (b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2 });
        v.panBy(c.centreX() - a.x, c.centreY() - a.y);
        host.tick(1);
    }

    funkgui::Point pinAt(SV& v, const char* part, const char* pin) { return v.centreOfPin(part, pin).value_or(funkgui::Point{}); }
    funkgui::Point partAt(SV& v, const char* part) { return v.centreOfPart(part).value_or(funkgui::Point{}); }

    const cir::Part* partOf(const cir::Drawing& d, const char* name)
    {
        const int i = d.part(name);
        return i >= 0 ? &d.parts[static_cast<std::size_t>(i)] : nullptr;
    }

    // A point of net `net`'s route that is no pin and has two edges (a wire's middle), else nullopt.
    std::optional<cir::Pt> midWire(const cir::Drawing& d, const char* net)
    {
        const int n = d.net(net);
        if (n < 0)
            return std::nullopt;
        const cir::Net& w = d.nets[static_cast<std::size_t>(n)];
        std::map<cir::Pt, int> degree;
        for (const cir::UnitEdge& e : w.edges)
        {
            ++degree[e.a];
            ++degree[e.a + (e.h ? cir::Pt{ 1, 0 } : cir::Pt{ 0, 1 })];
        }
        std::set<cir::Pt> pins;
        for (const cir::Part& p : d.parts)
            for (const cir::PinAt& pin : p.pins)
                pins.insert(pin.at);
        for (const auto& [p, k] : degree)
            if (k == 2 && pins.count(p) == 0)
                return p;
        return std::nullopt;
    }

    cir::Applied applied(const std::string& text, const std::vector<cir::Op>& ops)
    {
        return cir::apply(text, cir::reconcile(text), std::span<const cir::Op>(ops.data(), ops.size()));
    }

    // The path a drag draws when it leaves `from` horizontally first: from, the corner, to.
    std::vector<cir::Pt> lPath(cir::Pt from, cir::Pt to)
    {
        const cir::Pt corner{ to.x, from.y };
        std::vector<cir::Pt> path{ from };
        if (corner != from && corner != to)
            path.push_back(corner);
        path.push_back(to);
        return path;
    }

    // A drag from a to b, horizontally first, then down to b.
    void lDrag(funkgui::HeadlessHost& host, funkgui::Point a, funkgui::Point b, funkgui::Mods m = {})
    {
        host.press(a.x, a.y, m);
        host.moveTo(b.x, a.y, 6);
        host.moveTo(b.x, b.y, 6);
        host.release();
        host.tick(1);
    }

    funkgui::Mods popupMods()
    {
        funkgui::Mods m;
        m.ctrl = true;
        return m;
    }

    std::vector<std::string> menuLabels(const funkgui::HeadlessHost& host)
    {
        std::vector<std::string> out;
        if (const funkgui::MenuRequest* r = host.pendingMenu())
            for (const funkgui::MenuItem& i : r->items)
                if (!i.separator)
                    out.push_back(i.label);
        return out;
    }

    bool hasLabel(const std::vector<std::string>& labels, const std::string& prefix)
    {
        return std::any_of(labels.begin(), labels.end(), [&](const std::string& l) { return l.rfind(prefix, 0) == 0; });
    }

    // The first label of the pending menu starting with `prefix`, chosen.
    bool choose(funkgui::HeadlessHost& host, const std::string& prefix)
    {
        for (const std::string& l : menuLabels(host))
            if (l.rfind(prefix, 0) == 0)
                return host.chooseMenuItem(l);
        host.cancelMenu();
        return false;
    }
}

PROBE(gui, schematic)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);
    const std::string planted = cir::written(kPlanted);
    const std::string loose = cir::written(kLoose);

    // ---- VIEW, and the library's netlists as PNGs -------------------------------------------------------------------
    {
        Rig r(draftWith("#include \"diodetone.cir\"\n" + std::string(pg::kExamples[0].source),
                        { { "diodetone.cir", libraryText("diodetone.cir") }, { "screamer.cir", libraryText("screamer.cir") } }, 1));
        pg::ui::Panel& panel = *r.panel;
        funkgui::HeadlessHost& host = *r.host;
        P.eq("view.pills_shown", panel.viewPillsShown() ? 1 : 0, 1);
        P.eq("view.pills_labelled", panel.viewPillsLabelled() ? 1 : 0, 1);
        P.eq("view.netlist_first", panel.schematicShown() ? 0 : 1, 1);
        P.eq("view.opening_keeps_nothing", static_cast<int64_t>(r.processor.editorPort().fileViews().size()), 0);
        {
            const funkgui::Point tab0 = panel.centreOfTab(0);
            host.click(tab0.x, tab0.y);
            host.tick(1);
            P.eq("view.no_pills_on_fm", panel.viewPillsShown() ? 0 : 1, 1);
            const funkgui::Point tab1 = panel.centreOfTab(1);
            host.click(tab1.x, tab1.y);
            host.tick(1);
        }
        const funkgui::Rect pills = panel.viewPillRect(false);
        P.eq("view.pills_right_end", pills.x > panel.fileTabRect(panel.fileCount() - 1).right() ? 1 : 0, 1);
        const funkgui::Point pill = panel.centreOfViewPill(true);
        host.click(pill.x, pill.y);
        host.settle();
        P.eq("view.schematic_shown", panel.schematicShown() ? 1 : 0, 1);
        SV& v = r.view();
        P.ge("view.open_zoom", v.zoom(), 0.6);
        std::printf("opened at %.0f%%, canvas %.0f x %.0f\n", static_cast<double>(v.zoom() * 100.0f), static_cast<double>(v.canvasRect().w),
                    static_cast<double>(v.canvasRect().h));
        const funkgui::PrimList& f = host.draw();
        P.ge("view.parts_drawn", countTag(f, sch::kPartTag), 30.0);
        P.ge("view.wires_drawn", countTag(f, sch::kWireTag), 4.0);
        P.eq("view.missing_glyphs", static_cast<int64_t>(f.missingGlyphs), 0);
        bool inside = true;
        const funkgui::Rect canvas = v.canvasRect();
        for (const funkgui::Prim& p : f.prims)
            if (p.tag == sch::kPartTag || p.tag == sch::kWireTag || p.tag == sch::kBadgeTag)
                inside = inside && p.x0 >= canvas.x - 0.5f && p.y0 >= canvas.y - 0.5f && p.x1 <= canvas.right() + 0.5f
                      && p.y1 <= canvas.bottom() + 0.5f;
        P.eq("view.inside_canvas", inside ? 1 : 0, 1);
        bool visible = true;                      // every part's body on the canvas when it opens
        for (const cir::Part& p : v.drawing().parts)
            visible = visible && canvas.contains(partAt(v, p.name.c_str()));
        P.eq("view.all_parts_in_view", visible ? 1 : 0, 1);
        std::printf("status words: %s\n", v.statusWords().c_str());
        P.eq("view.status_words", v.statusWords().rfind("Schematic   9 parts   5 nets", 0) == 0 ? 1 : 0, 1);
        P.eq("view.played", v.stripRight().find("Not played") == std::string::npos ? 1 : 0, 1);
        writePng(P, host, "gui.schematic.png", "view");
        // the Tube Screamer: no program #includes it
        const funkgui::Point tab2 = panel.centreOfTab(2);
        host.click(tab2.x, tab2.y);
        host.settle();
        P.eq("view.netlist_again", panel.schematicShown() ? 0 : 1, 1);
        host.keys("cmd+f");                       // the find bar, open over the text: the drawing closes it
        P.eq("view.find_open", panel.codeAssist().findOpen() ? 1 : 0, 1);
        host.keys("cmd+shift+d");
        host.settle();
        P.eq("view.cmd_shift_d", panel.schematicShown() ? 1 : 0, 1);
        P.eq("view.find_closed", panel.codeAssist().findOpen() ? 0 : 1, 1);
        P.eq("view.not_played", v.stripRight() == "Not played: no program #includes screamer.cir" ? 1 : 0, 1);
        P.ge("view.screamer_zoom", v.zoom(), 0.6);
        {
            // the drawing's budget: 3,000 primitives a frame for 40 parts (the screamer has 23; the grid, the toolbar
            // and the info strip counted in)
            const funkgui::PrimList& sf = host.draw();
            const int drawn = countTag(sf, sch::kSchematicTag) + countTag(sf, sch::kPartTag) + countTag(sf, sch::kWireTag)
                            + countTag(sf, sch::kBadgeTag);
            std::printf("screamer: %d primitives the drawing's, %zu in the frame\n", drawn, sf.prims.size());
            P.le("view.frame_budget", drawn, 3000.0);
        }
        writePng(P, host, "gui.schematic.screamer.png", "screamer");
        host.keys("cmd+shift+d");
        host.tick(1);
        P.eq("view.cmd_shift_d_back", panel.schematicShown() ? 0 : 1, 1);
        // zoomed out to 25 % and in to 400 %
        host.keys("cmd+shift+d");
        host.settle();
        const funkgui::Rect c = v.canvasRect();
        v.zoomTo(0.25f, { c.centreX(), c.centreY() });
        host.tick(1);
        const funkgui::PrimList& z = host.draw();
        P.near("zoom.floor", v.zoom(), 0.25, 1e-6);
        P.eq("zoom.25_boxes", countTag(z, sch::kPartTag) < 80 ? 1 : 0, 1);   // bodies as boxes, no labels
        writePng(P, host, "gui.schematic.zoom25.png", "zoom25");
        v.zoomTo(9.0f, { c.centreX(), c.centreY() });
        host.tick(1);
        P.near("zoom.ceiling", v.zoom(), 4.0, 1e-6);
        writePng(P, host, "gui.schematic.zoom400.png", "zoom400");
        // the find bar open over screamer.cir's text, then the tab of a netlist shown in its Schematic view: closed
        host.keys("cmd+shift+d");
        host.tick(1);
        host.keys("cmd+f");
        const funkgui::Point tab1 = panel.centreOfTab(1);
        host.click(tab1.x, tab1.y);
        host.settle();
        P.eq("view.find_closed_by_tab", panel.schematicShown() && !panel.codeAssist().findOpen() ? 1 : 0, 1);
    }

    // ---- PERSIST: a view switched but never zoomed nor panned keeps no zoom: opened again, it fits again ------------
    {
        Rig r(draftWith(std::string(pg::kExamples[0].source), { { "diodetone.cir", libraryText("diodetone.cir") } }, 1));
        const funkgui::Point pill = r.panel->centreOfViewPill(true);
        r.host->click(pill.x, pill.y);
        r.host->settle();
        const float zoom = r.view().zoom();
        const funkgui::Point centre = r.view().centre();
        const std::vector<pg::FileView> views = r.processor.editorPort().fileViews();
        P.eq("persist.switch_no_zoom", views.size() == 1 && views[0].schematic && views[0].zoom == 0.0f ? 1 : 0, 1);
        r.close();
        r.open();
        P.eq("persist.reopened_fits", r.panel->schematicShown() && r.view().zoom() == zoom && r.view().centre().x == centre.x
                                          && r.view().centre().y == centre.y ? 1 : 0, 1);
    }
    // ---- PERSIST, session pins: a text-only netlist's places (its parts with no line) after Claude added a part by text
    // survive a view_panel picture (whose Panel publishes none) and an editor opened again (which takes the port's)
    {
        const std::string text = cir::stripLayout(libraryText("diodetone.cir"));
        Rig r(draftWith(std::string(pg::kExamples[0].source), { { "t.cir", text } }, 1));
        const funkgui::Point pill = r.panel->centreOfViewPill(true);
        r.host->click(pill.x, pill.y);
        r.host->settle();
        std::string more = text;
        more.insert(more.find(".end"), "C9 a 0 10n\n");
        const pg::ChatAgent::McpResult wrote = r.processor.chatAgent().mcpCall(
            "write_file", pg::claude::object({ { "name", "t.cir" }, { "content", juce::String(more) } }));
        r.host->tick(2);
        const auto placesOf = [](const cir::Drawing& d) {
            std::string s;
            for (const cir::Part& p : d.parts)
                s += p.name + "@" + std::to_string(p.origin.x) + "," + std::to_string(p.origin.y) + cir::orientName(p.orient) + " ";
            return s;
        };
        const std::string seen = placesOf(r.view().drawing()), pins = r.processor.editorPort().schematicPins("t.cir");
        const std::string afresh = placesOf(cir::reconcile(more));
        std::printf("session pins: the user's view %s\n  laid out afresh %s\n", seen.c_str(), afresh.c_str());
        const pg::PanelPicture pic = pg::picturePanel(r.processor.ports(), r.processor.editorPort(), 0, 0, true, "");
        P.eq("persist.picture_keeps_pins", !wrote.isError && has(r.ed().text(), "C9 a 0 10n") && !pins.empty() && !pic.png.empty()
                                               && r.processor.editorPort().schematicPins("t.cir") == pins ? 1 : 0, 1);
        r.close();
        r.open();
        P.eq("persist.reopened_pins", r.panel->schematicShown() && placesOf(r.view().drawing()) == seen
                                          && r.processor.editorPort().schematicPins("t.cir") == pins ? 1 : 0, 1);
    }

    // ---- NOTES: the drawing's notes in the info strip (a *@ line it cannot read), the law inputs in the status words, a
    // part's value spelled out in the strip
    {
        std::string bad = cir::written(libraryText("diodetone.cir"));   // its drawing written, then a line spoilt
        const std::string from = "*@ part R1 6,0";
        const bool found = bad.find(from) != std::string::npos;
        if (found)
            bad.replace(bad.find(from), from.size(), "*@ part R1 6,x");
        Rig r(draftWith(dialsProgram("d.cir", 1), { { "d.cir", bad }, { "otalp.cir", libraryText("otalp.cir") } }, 1));
        r.panel->setSchematic(true);
        r.host->settle();
        SV& v = r.view();
        std::printf("notes: the strip \"%s\"\n", v.stripRight().c_str());
        P.eq("notes.strip", found && v.stripRight().rfind("Note: ", 0) == 0 && has(v.stripRight(), "'6,x' is not a position") ? 1 : 0, 1);
        v.selectPart("R1", false);
        r.host->tick(1);
        v.selectPart("C1", false);
        r.host->tick(1);
        const std::string c1 = v.stripLeft();
        v.selectPart("R1", false);
        r.host->tick(1);
        std::printf("values spelled: \"%s\", \"%s\"\n", v.stripLeft().c_str(), c1.c_str());
        P.eq("notes.value_spelled", has(v.stripLeft(), "2.2k = 2,200 ohms") && has(c1, "10n = 10 nanofarads") ? 1 : 0, 1);
        const funkgui::Point tab = r.panel->centreOfTab(2);
        r.host->click(tab.x, tab.y);
        r.panel->setSchematic(true);
        r.host->settle();
        std::printf("otalp.cir: %s\n", v.statusWords().c_str());
        P.eq("notes.law_inputs", has(v.statusWords(), "   law input iabc   ") ? 1 : 0, 1);
    }

    // ---- GESTURES -------------------------------------------------------------------------------------------------------
    {
        Rig r(draftWith(dialsProgram("g.cir", 2), { { "g.cir", planted }, { "pedal.cir", "" } }, 1));
        pg::ui::Panel& panel = *r.panel;
        funkgui::HeadlessHost& host = *r.host;
        panel.setSchematic(true);
        host.settle();
        SV& v = r.view();
        funkgui::TextEditor& ed = r.ed();
        const auto reset = [&](const std::string& text) {
            ed.setText(text);
            host.tick(1);
            frame(v, host);
            v.setTool(SV::Tool::select);
        };
        const auto oneStep = [&](const std::string& key, const std::string& before, const std::string& want) {
            funkgui::TextEditor& shown = panel.editor();   // the tab shown's
            const std::string after = shown.text();
            const bool same = after == want;
            if (!same)
                std::printf("%s: got\n%s\nwanted\n%s\n", key.c_str(), after.c_str(), want.c_str());
            P.eq(key, same ? 1 : 0, 1);
            const bool undone = shown.canUndo() && shown.undo() && shown.text() == before && !shown.canUndo();
            const bool redone = shown.redo() && shown.text() == after;
            host.tick(1);
            P.eq(key + ".one_step", undone && redone ? 1 : 0, 1);
        };

        // placing every kind, by a click with the part carried
        {
            const cir::Kind kinds[] = { cir::Kind::R, cir::Kind::C, cir::Kind::L, cir::Kind::D, cir::Kind::Pot,
                                        cir::Kind::Rheostat, cir::Kind::OpAmp, cir::Kind::Vcvs, cir::Kind::Vccs, cir::Kind::Ota,
                                        cir::Kind::V, cir::Kind::I, cir::Kind::S };
            for (const cir::Kind k : kinds)
            {
                reset(planted);
                const cir::Box b = v.drawing().bounds();
                const cir::Pt at{ (b.x0 + b.x1) / 2, b.y1 + 6 };
                v.setTool(SV::Tool::place, k);
                const funkgui::Point s = v.screenOf(at);
                host.move(s.x, s.y);
                host.click(s.x, s.y);
                host.tick(1);
                cir::Op op;
                op.type = cir::Op::Type::add;
                op.lines = cir::placementLines(cir::read(planted), k);
                op.at = at;
                op.orient = cir::Orient::r0;
                std::string word = cir::symbol(k).word;
                for (char& ch : word)
                    ch = std::isalnum(static_cast<unsigned char>(ch)) != 0 ? static_cast<char>(std::tolower(static_cast<unsigned char>(ch))) : '_';
                const std::string key = "gestures.place_" + word;
                if (k == cir::Kind::R)                // what the user just did is not said
                    P.eq("gestures.place_says_nothing", v.notice().empty() ? 1 : 0, 1);
                oneStep(key, planted, applied(planted, { op }).text);
                P.eq(key + ".carries_on", v.tool() == SV::Tool::place && v.carried() == k ? 1 : 0, 1);
            }
            // the input and the output: one each (a netlist that has them refuses another)
            reset("* g.cir: a planted netlist\nR1 a b 1k\n.end\n");
            const std::string bare = ed.text();
            frame(v, host);
            const cir::Box b = v.drawing().bounds();
            v.setTool(SV::Tool::place, cir::Kind::Input);
            const funkgui::Point s = v.screenOf({ b.x0 - 10, b.y0 });
            host.move(s.x, s.y);
            host.click(s.x, s.y);
            host.tick(1);
            cir::Op in;
            in.type = cir::Op::Type::add;
            in.lines = { ".input u n1" };
            in.at = cir::Pt{ b.x0 - 10, b.y0 };
            oneStep("gestures.place_input", bare, applied(bare, { in }).text);
            const std::string withInput = ed.text();
            host.click(s.x + 40.0f, s.y);
            host.tick(1);
            P.eq("gestures.place_input_once", ed.text() == withInput && v.notice().find("FMinus takes exactly one") != std::string::npos ? 1 : 0, 1);
            ed.setText(withInput);                    // its own undo history: one step
            host.tick(1);
            v.setTool(SV::Tool::place, cir::Kind::Output);
            const funkgui::Point o = v.screenOf({ b.x1 + 10, b.y0 });
            host.move(o.x, o.y);
            host.click(o.x, o.y);
            host.tick(1);
            cir::Op out;
            out.type = cir::Op::Type::add;
            out.lines = { ".output vout n2" };
            out.at = cir::Pt{ b.x1 + 10, b.y0 };
            oneStep("gestures.place_output", withInput, applied(withInput, { out }).text);
        }

        // wires: pin to pin; into a wire's middle (a T); from a wire's middle to a pin
        {
            reset(loose);
            const cir::Drawing d = v.drawing();
            const cir::Pt from = partOf(d, "R1")->pins[1].at, to = partOf(d, "R2")->pins[0].at;
            lDrag(host, pinAt(v, "R1", "2"), pinAt(v, "R2", "1"));
            cir::Op op;
            op.type = cir::Op::Type::connect;
            op.a = { "R1", "2", "" };
            op.b = { "R2", "1", "" };
            op.path = lPath(from, to);
            const std::string merged = v.notice();
            std::printf("merged: %s\n", merged.c_str());
            oneStep("gestures.wire_pin_to_pin", loose, applied(loose, { op }).text);
            // two nets merged: said, and gone once the text moved (the undo and redo of oneStep)
            P.eq("gestures.merge_said", merged.rfind("connected R1.2 and R2.1: ", 0) == 0 && v.notice().empty() ? 1 : 0, 1);
            P.eq("gestures.wire_pin_to_pin_net", cir::read(ed.text()).elements[2].nodes[0].text == "a" ? 1 : 0, 1);

            reset(loose);
            const std::optional<cir::Pt> mid = midWire(v.drawing(), "a");
            P.eq("gestures.mid_wire_found", mid ? 1 : 0, 1);
            if (mid)
            {
                const cir::Pt r2 = partOf(v.drawing(), "R2")->pins[0].at;
                lDrag(host, pinAt(v, "R2", "1"), v.screenOf(*mid));
                cir::Op t;
                t.type = cir::Op::Type::connect;
                t.a = { "R2", "1", "" };
                t.b = { "", "", "a" };
                t.path = lPath(r2, *mid);
                oneStep("gestures.wire_into_t", loose, applied(loose, { t }).text);
                // a junction where the T meets the wire
                const cir::Drawing after = v.drawing();
                const cir::Net& a = after.nets[static_cast<std::size_t>(after.net("a"))];
                P.eq("gestures.t_junction", std::find(a.junctions.begin(), a.junctions.end(), *mid) != a.junctions.end() ? 1 : 0, 1);

                reset(loose);
                lDrag(host, v.screenOf(*mid), pinAt(v, "R2", "1"));
                cir::Op w;
                w.type = cir::Op::Type::connect;
                w.a = { "", "", "a" };
                w.b = { "R2", "1", "" };
                w.path = lPath(*mid, r2);
                oneStep("gestures.wire_from_wire", loose, applied(loose, { w }).text);
            }
            // a wire released on nothing draws nothing
            reset(loose);
            const funkgui::Point p = pinAt(v, "R2", "1");
            host.press(p.x, p.y);
            host.moveTo(p.x, p.y + 60.0f, 6);
            host.release();
            host.tick(1);
            P.eq("gestures.wire_to_nothing", ed.text() == loose && !ed.canUndo() ? 1 : 0, 1);
        }

        // the Ground tool: R2's first pin goes to ground
        {
            reset(loose);
            host.keys("g");
            P.eq("gestures.ground_tool", v.tool() == SV::Tool::ground ? 1 : 0, 1);
            const funkgui::Point p = pinAt(v, "R2", "1");
            host.click(p.x, p.y);
            host.tick(1);
            cir::Op op;
            op.type = cir::Op::Type::connect;
            op.a = { "R2", "1", "" };
            op.b = { "", "", "0" };
            oneStep("gestures.ground", loose, applied(loose, { op }).text);
            P.eq("gestures.ground_line", ed.text().find("R2 0 out 1k") != std::string::npos ? 1 : 0, 1);
            v.setTool(SV::Tool::select);
        }

        // a move; one onto another part refused; rotate; mirror; delete
        {
            reset(planted);
            const cir::Part r2 = *partOf(v.drawing(), "R2");
            const funkgui::Point c = partAt(v, "R2");
            const float s = sch::kUnitPx * v.zoom();
            host.press(c.x, c.y);
            host.moveTo(c.x + 4.0f * s, c.y - 6.0f * s, 8);
            host.release();
            host.tick(1);
            cir::Op op;
            op.type = cir::Op::Type::place;
            op.name = "R2";
            op.at = r2.origin + cir::Pt{ 4, -6 };
            op.orient = r2.orient;
            oneStep("gestures.move", planted, applied(planted, { op }).text);
            P.eq("gestures.move_layout_only", cir::sameNetlist(ed.text(), planted) ? 1 : 0, 1);

            reset(planted);
            const funkgui::Point a = partAt(v, "R2"), b = partAt(v, "C1");
            host.press(a.x, a.y);
            host.moveTo(b.x, b.y, 8);
            P.eq("gestures.refused_cursor", host.cursor() == funkgui::Cursor::notAllowed ? 1 : 0, 1);
            host.release();
            host.tick(1);
            P.eq("gestures.refused_overlap", ed.text() == planted && !ed.canUndo() && v.notice().find("overlap") != std::string::npos ? 1 : 0, 1);
            std::printf("refused: %s\n", v.notice().c_str());

            reset(planted);
            const funkgui::Point r2c = partAt(v, "R2");
            host.click(r2c.x, r2c.y);
            host.keys("cmd+r");
            host.tick(1);
            cir::Op rot;
            rot.type = cir::Op::Type::place;
            rot.name = "R2";
            rot.at = r2.origin;
            rot.orient = cir::rotatedCw(r2.orient);
            oneStep("gestures.rotate", planted, applied(planted, { rot }).text);

            reset(planted);
            host.click(r2c.x, r2c.y);
            host.keys("cmd+e");
            host.tick(1);
            cir::Op mir;
            mir.type = cir::Op::Type::place;
            mir.name = "R2";
            mir.at = r2.origin;
            mir.orient = cir::mirrored(r2.orient);
            oneStep("gestures.mirror", planted, applied(planted, { mir }).text);

            reset(planted);
            const funkgui::Point c1 = partAt(v, "C1");
            host.click(c1.x, c1.y);
            host.keys("delete");
            host.tick(1);
            cir::Op del;
            del.type = cir::Op::Type::remove;
            del.names = { "C1" };
            oneStep("gestures.delete", planted, applied(planted, { del }).text);
            // its stub pruned: net a's route touches no point where C1's pin was
            const cir::Drawing d = v.drawing();
            const cir::Net& na = d.nets[static_cast<std::size_t>(d.net("a"))];
            const cir::Pt gone = partOf(cir::reconcile(planted), "C1")->pins[0].at;
            bool stub = false;
            for (const cir::UnitEdge& e : na.edges)
                stub = stub || e.a == gone || e.a + (e.h ? cir::Pt{ 1, 0 } : cir::Pt{ 0, 1 }) == gone;
            P.eq("gestures.delete_stub_pruned", stub ? 0 : 1, 1);
            P.eq("gestures.delete_no_element", ed.text().find("C1 a 0 10n") == std::string::npos ? 1 : 0, 1);
        }

        // a wire run deleted splits its net: the ladder's m between C1's and C2's drops
        {
            reset(kLadder);
            const funkgui::Point w = v.screenOf({ 12, 0 });
            host.click(w.x, w.y);
            P.eq("gestures.run_selected", v.selectedRuns(), 1);
            host.keys("backspace");
            host.tick(1);
            const std::string t = ed.text();
            const auto lineOf = [&](const char* start) {
                const std::size_t a = t.find(start);
                return a == std::string::npos ? std::string("(none)") : t.substr(a, t.find('\n', a) - a);
            };
            P.eq("gestures.cut_split", cir::stripLayout(t).find("C2 n1 0 22n\nR2 n1 o 1k\n") != std::string::npos ? 1 : 0, 1);
            P.eq("gestures.cut_split_routes", lineOf("*@ net m ") == "*@ net m 8,0 10,0 10,3" && lineOf("*@ net n1 ") == "*@ net n1 14,3 14,0 19,0" ? 1 : 0, 1);
            std::printf("cut: %s / %s\n", lineOf("*@ net m ").c_str(), lineOf("*@ net n1 ").c_str());
            const bool undone = ed.undo() && ed.text() == kLadder && !ed.canUndo();
            P.eq("gestures.cut_split.one_step", undone ? 1 : 0, 1);
        }

        // Start a circuit, in an empty netlist
        {
            const funkgui::Point tab2 = panel.centreOfTab(2);
            host.click(tab2.x, tab2.y);
            host.tick(1);
            panel.setSchematic(true);
            host.settle();
            P.eq("empty.start_offered", v.centreOfStart() ? 1 : 0, 1);
            P.eq("empty.hint", v.stripLeft() == "An empty netlist" ? 1 : 0, 1);
            writePng(P, host, "gui.schematic.empty.png", "empty");
            const funkgui::Point s = v.centreOfStart().value_or(funkgui::Point{});
            host.click(s.x, s.y);
            host.settle();
            oneStep("gestures.start",
                    "", "* pedal.cir: a circuit drawn in the schematic\n"
                        ".input u in\n"
                        ".output vout out\n"
                        "*@ schematic 1 (the playground's drawing; SPICE reads *@ lines as comments)\n"
                        "*@ input u 0,0\n"
                        "*@ output vout 24,0\n"
                        ".end\n");
        }
        // a netlist whose only element the view cannot draw: no Start offered (a click on it would do nothing); the
        // Not drawn banner, and after it the count once FMinus's message lands (it refuses every such element)
        {
            funkgui::TextEditor& pe = panel.editor();
            pe.setText("* pedal.cir: a transistor\nQ1 c b e 2N3904\n.end\n");
            host.tick(1);
            P.eq("empty.undrawn_no_start", !v.centreOfStart() && v.stripLeft() != "An empty netlist" ? 1 : 0, 1);
            P.eq("empty.undrawn_banner", v.stripRight().rfind("Not drawn: Q1 (a BJT: Phase 2)", 0) == 0 ? 1 : 0, 1);
            const std::vector<pg::ui::Diagnostic> q{ { "pedal.cir", 2, 1, pg::ui::Diagnostic::Severity::error, "planted: the transistor Q1" } };
            panel.codeAssist().setCheckDiagnostics(q, { { "pedal.cir", pe.text() } });
            host.tick(1);
            std::printf("not drawn: %s\n", v.stripRight().c_str());
            P.eq("empty.undrawn_banner_and_count", v.stripRight() == "Not drawn: Q1 (a BJT: Phase 2)   1 PROBLEM" ? 1 : 0, 1);
            P.eq("empty.start_refused_says", !v.startCircuit() && v.notice() == "Start a circuit is for an empty netlist" ? 1 : 0, 1);
        }
    }

    // ---- UNDO, OVERTAKEN ---------------------------------------------------------------------------------------------------
    {
        Rig r(draftWith(dialsProgram("g.cir", 2), { { "g.cir", planted } }, 1));
        pg::ui::Panel& panel = *r.panel;
        funkgui::HeadlessHost& host = *r.host;
        panel.setSchematic(true);
        host.settle();
        SV& v = r.view();
        funkgui::TextEditor& ed = r.ed();
        frame(v, host);
        const float s = sch::kUnitPx * v.zoom();
        // A: a move in the Schematic view
        const funkgui::Point c = partAt(v, "R2");
        host.press(c.x, c.y);
        host.moveTo(c.x, c.y - 5.0f * s, 6);
        host.release();
        host.tick(1);
        const std::string afterA = ed.text();
        P.eq("undo.a_moved", afterA != planted ? 1 : 0, 1);
        // B: typing in the Netlist view
        host.keys("cmd+shift+d");
        host.tick(1);
        P.eq("undo.netlist", panel.schematicShown() ? 0 : 1, 1);
        ed.setCaret({ 0, 0 });
        host.keys("x");
        host.tick(1);
        const std::string afterB = ed.text();
        P.eq("undo.b_typed", afterB == "x" + afterA ? 1 : 0, 1);
        host.keys("cmd+shift+d");
        host.tick(1);
        host.keys("cmd+z");
        host.tick(1);
        P.eq("undo.schematic_undoes_typing", ed.text() == afterA ? 1 : 0, 1);
        host.keys("cmd+z");
        host.tick(1);
        P.eq("undo.schematic_undoes_move", ed.text() == planted ? 1 : 0, 1);
        host.keys("cmd+shift+d");
        host.tick(1);
        host.keys("cmd+shift+z");
        host.tick(1);
        P.eq("undo.netlist_redoes_move", ed.text() == afterA ? 1 : 0, 1);
        host.keys("cmd+shift+d");
        host.tick(1);
        const funkgui::Point redo = v.centreOfButton(SV::Button::redo), undo = v.centreOfButton(SV::Button::undo);
        host.click(redo.x, redo.y);
        host.tick(1);
        P.eq("undo.toolbar_redo", ed.text() == afterB ? 1 : 0, 1);
        host.click(undo.x, undo.y);
        host.tick(1);
        P.eq("undo.toolbar_undo", ed.text() == afterA ? 1 : 0, 1);
        P.eq("undo.selection_survives", v.selection() == std::vector<std::string>{ "R2" } ? 1 : 0, 1);

        // OVERTAKEN: a move under way, Claude's write merged meanwhile: cancelled, nothing applied
        const std::string base = ed.text();
        const funkgui::Point c2 = partAt(v, "R2");
        host.press(c2.x, c2.y);
        host.moveTo(c2.x + 3.0f * s, c2.y, 6);
        P.eq("overtaken.moving", v.gestureOn() ? 1 : 0, 1);
        // Claude moves R1's part line (write_file)
        const cir::Part r1 = *partOf(cir::reconcile(base), "R1");
        const std::string was = "*@ part R1 " + std::to_string(r1.origin.x) + "," + std::to_string(r1.origin.y);
        std::string moved = base;
        moved.replace(moved.find(was), was.size(), "*@ part R1 " + std::to_string(r1.origin.x) + "," + std::to_string(r1.origin.y - 6));
        // Claude's write_file (Claude Desktop's call, as the plugin's chat makes it), merged into the tab
        const pg::ChatAgent::McpResult wrote = r.processor.chatAgent().mcpCall(
            "write_file", pg::claude::object({ { "name", "g.cir" }, { "content", juce::String(moved) } }));
        P.eq("overtaken.write_file", wrote.isError ? 0 : 1, 1);
        host.tick(1);
        P.eq("overtaken.cancelled", v.gestureOn() ? 0 : 1, 1);
        host.release();
        host.tick(1);
        P.eq("overtaken.nothing_applied", ed.text() == moved ? 1 : 0, 1);
        P.eq("overtaken.said", v.notice().find("changed") != std::string::npos ? 1 : 0, 1);
        P.ge("overtaken.flash", v.flashOf("R1"), 0.5);
        P.eq("overtaken.flash_only_moved", v.flashOf("C1") == 0.0f && v.flashOf("R2") == 0.0f ? 1 : 0, 1);
        host.settle();
        P.eq("overtaken.flash_fades", v.flashOf("R1") == 0.0f ? 1 : 0, 1);
        // a gesture made after Claude's write, before a frame merged it: undone (the processor's draft is Claude's),
        // the write merged, and the strip saying to do it again
        {
            const std::string base2 = ed.text();
            const cir::Part q = *partOf(cir::reconcile(base2), "R1");
            const std::string at = "*@ part R1 " + std::to_string(q.origin.x) + "," + std::to_string(q.origin.y);
            std::string moved2 = base2;
            moved2.replace(moved2.find(at), at.size(), "*@ part R1 " + std::to_string(q.origin.x) + "," + std::to_string(q.origin.y - 3));
            const pg::ChatAgent::McpResult w2 = r.processor.chatAgent().mcpCall(
                "write_file", pg::claude::object({ { "name", "g.cir" }, { "content", juce::String(moved2) } }));
            v.selectPart("R2", false);
            const bool nudged = v.nudge(0, 1);
            const bool untouched = ed.text() == base2;
            host.tick(2);
            std::printf("overtaken after the write: %s\n", v.notice().c_str());
            P.eq("overtaken.gesture_after_write", !w2.isError && !nudged && untouched && ed.text() == moved2
                                                      && r.processor.editorPort().draft().files[1].text == moved2
                                                      && has(v.notice(), "Claude's edit came first") ? 1 : 0, 1);
        }
        // TYPING over a write: text typed in g.cir and Claude's write of program.fm in one frame, before the editor gave
        // its files: both stay (the merge keeps the typing where the write left the file, then gives it to the port)
        {
            ed.setCaret({ 0, 0 });
            ed.insert("* typed before the frame\n");
            const std::string typed = ed.text();
            const std::string program = r.processor.editorPort().draft().files[0].text + "// Claude's write\n";
            const pg::ChatAgent::McpResult w3 = r.processor.chatAgent().mcpCall(
                "write_file", pg::claude::object({ { "name", "program.fm" }, { "content", juce::String(program) } }));
            host.tick(1);
            const pg::Draft after = r.processor.editorPort().draft();
            P.eq("overtaken.typing_kept_over_write", !w3.isError && ed.text() == typed && after.files.size() == 2 && after.files[1].text == typed
                                                         && has(after.files[0].text, "// Claude's write\n") ? 1 : 0, 1);
        }
    }

    // ---- POINTER, KEYS --------------------------------------------------------------------------------------------------
    {
        Rig r(draftWith(dialsProgram("g.cir", 2), { { "g.cir", planted } }, 1));
        pg::ui::Panel& panel = *r.panel;
        funkgui::HeadlessHost& host = *r.host;
        panel.setSchematic(true);
        host.settle();
        SV& v = r.view();
        funkgui::TextEditor& ed = r.ed();
        frame(v, host);
        const float s = sch::kUnitPx * v.zoom();
        const funkgui::Rect canvas = v.canvasRect();
        const funkgui::Point empty{ canvas.x + 30.0f, canvas.y + 30.0f };
        const std::optional<cir::Pt> mid = midWire(v.drawing(), "a");
        const funkgui::Point wire = mid ? v.screenOf(*mid) : funkgui::Point{};
        const auto centreMoved = [&](funkgui::Point before) {
            return std::fabs(v.centre().x - before.x) > 0.5f || std::fabs(v.centre().y - before.y) > 0.5f;
        };
        const auto popupDrag = [&](funkgui::Point a) {
            const funkgui::Point before = v.centre();
            const std::string text = ed.text();
            host.press(a.x, a.y, popupMods());
            host.moveTo(a.x + 40.0f, a.y + 20.0f, 6);
            host.release();
            host.tick(1);
            return centreMoved(before) && ed.text() == text && host.pendingMenu() == nullptr;
        };
        // idle, a body
        const funkgui::Point r2 = partAt(v, "R2"), c1 = partAt(v, "C1");
        host.click(r2.x, r2.y);
        P.eq("pointer.body_click_selects", v.selection() == std::vector<std::string>{ "R2" } ? 1 : 0, 1);
        funkgui::Mods shift;
        shift.shift = true;
        host.click(c1.x, c1.y, shift);
        P.eq("pointer.body_shift_adds", v.selection().size() == 2 ? 1 : 0, 1);
        host.click(c1.x, c1.y, shift);
        P.eq("pointer.body_shift_toggles", v.selection() == std::vector<std::string>{ "R2" } ? 1 : 0, 1);
        host.click(c1.x, c1.y);
        host.press(r2.x, r2.y);
        host.moveTo(r2.x, r2.y - 4.0f * s, 6);
        P.eq("pointer.body_drag_moves_it_first", v.selection() == std::vector<std::string>{ "R2" } && v.gestureOn() ? 1 : 0, 1);
        host.release();
        host.tick(1);
        ed.undo();
        host.tick(1);
        host.click(r2.x, r2.y, popupMods());
        P.eq("pointer.body_popup_menu", hasLabel(menuLabels(host), "Rotate") && hasLabel(menuLabels(host), "Show in netlist") ? 1 : 0, 1);
        host.cancelMenu();
        P.eq("pointer.body_popup_drag_pans", popupDrag(r2) ? 1 : 0, 1);
        // at 30 %, where R2's pins (2 units from its centre, 6 px) would cover its body: a drag on its middle moves it
        {
            const funkgui::Rect c = v.canvasRect();
            v.zoomTo(0.3f, { c.centreX(), c.centreY() });
            host.tick(1);
            const funkgui::Point small = partAt(v, "R2");
            const std::string was = ed.text();
            host.press(small.x, small.y);
            host.moveTo(small.x, small.y - 4.0f * sch::kUnitPx * v.zoom(), 6);
            host.release();
            host.tick(1);
            const std::string after = ed.text();
            P.eq("pointer.body_drag_low_zoom_moves", after != was && cir::sameNetlist(after, was) ? 1 : 0, 1);
            if (after != was)
                ed.undo();
            host.tick(1);
        }
        frame(v, host);
        // idle, a pin
        const funkgui::Point pin = pinAt(v, "R2", "2");
        host.click(empty.x, empty.y);
        host.click(pin.x, pin.y);
        P.eq("pointer.pin_click_selects_part", v.selection() == std::vector<std::string>{ "R2" } ? 1 : 0, 1);
        host.press(pin.x, pin.y);
        host.moveTo(pin.x + 3.0f * s, pin.y + 3.0f * s, 6);
        P.eq("pointer.pin_drag_wires", v.gestureOn() && host.cursor() == funkgui::Cursor::crosshair ? 1 : 0, 1);
        host.release();
        host.tick(1);
        host.click(pin.x, pin.y, popupMods());
        P.eq("pointer.pin_popup_menu", hasLabel(menuLabels(host), "Rotate") ? 1 : 0, 1);
        host.cancelMenu();
        P.eq("pointer.pin_popup_drag_pans", popupDrag(pin) ? 1 : 0, 1);
        frame(v, host);
        // idle, a wire
        host.click(wire.x, wire.y);
        P.eq("pointer.wire_click_selects_run", v.selectedRuns() == 1 && v.selection().empty() ? 1 : 0, 1);
        host.click(wire.x, wire.y, shift);
        P.eq("pointer.wire_shift_toggles", v.selectedRuns(), 0);
        host.click(wire.x, wire.y, popupMods());
        P.eq("pointer.wire_popup_menu", hasLabel(menuLabels(host), "Delete wire") && hasLabel(menuLabels(host), "Select net") ? 1 : 0, 1);
        host.cancelMenu();
        P.eq("pointer.wire_popup_drag_pans", popupDrag(wire) ? 1 : 0, 1);
        frame(v, host);
        // idle, nothing
        host.click(r2.x, r2.y);
        host.click(empty.x, empty.y);
        P.eq("pointer.empty_click_clears", v.selection().empty() ? 1 : 0, 1);
        {
            const cir::Part* p = partOf(v.drawing(), "R2");
            const funkgui::Point a = v.screenOf({ p->body.x0 - 1, p->body.y0 - 1 }), b = v.screenOf({ p->body.x1 + 1, p->body.y1 + 1 });
            host.press(a.x, a.y);
            host.moveTo(b.x, b.y, 6);
            host.release();
            P.eq("pointer.marquee", v.selection() == std::vector<std::string>{ "R2" } ? 1 : 0, 1);
            const cir::Part* q = partOf(v.drawing(), "C1");
            const funkgui::Point qa = v.screenOf({ q->body.x0 - 1, q->body.y0 - 1 }), qb = v.screenOf({ q->body.x1 + 1, q->body.y1 + 1 });
            host.press(qa.x, qa.y, shift);
            host.moveTo(qb.x, qb.y, 6);
            host.release();
            P.eq("pointer.marquee_shift_adds", v.selection().size() == 2 ? 1 : 0, 1);
        }
        host.click(empty.x, empty.y, popupMods());
        P.eq("pointer.empty_popup_menu", hasLabel(menuLabels(host), "Select all") && hasLabel(menuLabels(host), "Tidy") ? 1 : 0, 1);
        host.cancelMenu();
        P.eq("pointer.empty_popup_drag_pans", popupDrag(empty) ? 1 : 0, 1);
        frame(v, host);
        // Alt
        {
            funkgui::Mods alt;
            alt.alt = true;
            const std::string text = ed.text();
            host.click(wire.x, wire.y, alt);
            const std::vector<std::string> pinned = v.pinnedProbes();
            host.tick(40);                                        // (no double-click)
            host.click(wire.x, wire.y, alt);                      // again: unpinned
            P.eq("pointer.alt_click_wire_changes_nothing", ed.text() == text ? 1 : 0, 1);
            P.eq("pointer.alt_click_wire_pins", pinned.size() == 1 && v.pinnedProbes().empty() ? 1 : 0, 1);
            const funkgui::Point before = v.centre();
            host.press(r2.x, r2.y, alt);
            host.moveTo(r2.x + 30.0f, r2.y, 6);
            host.release();
            host.tick(1);
            P.eq("pointer.alt_drag_pans", centreMoved(before) && ed.text() == text ? 1 : 0, 1);
            frame(v, host);
        }
        // carrying a part
        {
            const std::string text = ed.text();
            const cir::Box b = v.drawing().bounds();
            const funkgui::Point p = v.screenOf({ b.x0 + 2, b.y1 + 6 });
            host.move(p.x, p.y);
            host.keys("r");
            P.eq("pointer.key_carries", v.tool() == SV::Tool::place && v.carried() == cir::Kind::R ? 1 : 0, 1);
            host.click(p.x, p.y, popupMods());
            P.eq("pointer.carry_popup_click_turns", v.carriedOrient() == cir::Orient::r90 && ed.text() == text && host.pendingMenu() == nullptr ? 1 : 0, 1);
            // Cmd-R, Cmd-Shift-R and Cmd-E turn the part carried, not the selection
            host.keys("cmd+r");
            const bool cw = v.carriedOrient() == cir::Orient::r180;
            host.keys("cmd+shift+r");
            const bool ccw = v.carriedOrient() == cir::Orient::r90;
            host.keys("cmd+e");
            P.eq("keys.carried_turns", cw && ccw && v.carriedOrient() == cir::mirrored(cir::Orient::r90) && ed.text() == text ? 1 : 0, 1);
            host.keys("cmd+e");
            const funkgui::Point before = v.centre();
            host.press(p.x, p.y, popupMods());
            host.moveTo(p.x + 40.0f, p.y, 6);
            host.release();
            host.tick(1);
            P.eq("pointer.carry_popup_drag_pans", centreMoved(before) && v.tool() == SV::Tool::place && ed.text() == text ? 1 : 0, 1);
            host.move(p.x, p.y);
            host.click(p.x, p.y);
            host.tick(1);
            P.eq("pointer.carry_click_places", ed.text() != text && v.tool() == SV::Tool::place ? 1 : 0, 1);
            const std::string once = ed.text();
            std::set<std::string> had;
            for (const cir::Part& x : v.drawing().parts)
                had.insert(x.name);
            const funkgui::Point q = { p.x + 10.0f * s, p.y };
            host.press(q.x - 6.0f * s, q.y);
            host.moveTo(q.x, q.y, 6);
            host.release();
            host.tick(1);
            const cir::Drawing d = v.drawing();
            const cir::Part* placed = nullptr;
            for (const cir::Part& x : d.parts)
                if (had.count(x.name) == 0)
                    placed = &x;
            P.eq("pointer.carry_drag_places_where_released", ed.text() != once && placed != nullptr && std::fabs(v.screenOf(placed->origin).x - q.x) < 0.01f ? 1 : 0, 1);
            const funkgui::Point sel = v.centreOfButton(SV::Button::select);
            host.click(sel.x, sel.y);
            P.eq("pointer.select_ends_carrying", v.tool() == SV::Tool::select ? 1 : 0, 1);
            host.move(p.x, p.y);
            host.keys("c");
            host.keys("g");
            P.eq("pointer.another_tool_ends_carrying", v.tool() == SV::Tool::ground ? 1 : 0, 1);
            host.keys("d");
            host.keys("escape");
            P.eq("pointer.escape_ends_carrying", v.tool() == SV::Tool::select ? 1 : 0, 1);
            ed.undo();
            ed.undo();
            host.tick(1);
        }
        // the Ground tool: a popup click ends it, a popup drag pans and keeps it
        {
            host.move(empty.x, empty.y);
            host.keys("g");
            P.eq("pointer.ground_popup_drag_pans", popupDrag(empty) && v.tool() == SV::Tool::ground ? 1 : 0, 1);
            host.click(empty.x, empty.y, popupMods());
            P.eq("pointer.ground_popup_click_ends", v.tool() == SV::Tool::select && host.pendingMenu() == nullptr ? 1 : 0, 1);
            frame(v, host);
        }
        // the Label tool: a drag from a pin, or on a body, does nothing (no wire, no move, no marquee)
        {
            const funkgui::Point from = pinAt(v, "R2", "2"), body = partAt(v, "R2");
            const std::string text = ed.text();
            host.move(empty.x, empty.y);
            host.keys("n");
            host.press(from.x, from.y);
            host.moveTo(from.x + 3.0f * s, from.y + 2.0f * s, 6);
            const bool wiring = v.gestureOn();
            host.release();
            host.press(body.x, body.y);
            host.moveTo(body.x, body.y - 4.0f * s, 6);
            const bool moving = v.gestureOn();
            host.release();
            host.tick(1);
            P.eq("pointer.label_drag_nothing", v.tool() == SV::Tool::label && !wiring && !moving && ed.text() == text && !v.editing() ? 1 : 0, 1);
            host.keys("escape");
            host.keys("escape");
            host.tick(1);
            frame(v, host);
        }

        // KEYS: edge auto-pan, a marquee held 8 px inside the right edge: 300 px/s for half a second
        {
            host.press(empty.x, empty.y);
            host.moveTo(canvas.right() - 8.0f, empty.y + 20.0f, 8);
            const funkgui::Point before = v.centre();
            host.tick(30);
            const float dx = v.centre().x - before.x;
            host.release();
            P.near("keys.edge_autopan_units", dx, 150.0 / static_cast<double>(s), 1e-3);
            P.near("keys.edge_autopan_level", v.centre().y - before.y, 0.0, 1e-6);
            frame(v, host);
        }
        // the PANEL view: a part carried at the canvas's edge is let go, and the hidden drawing waits (no auto-pan,
        // nothing written to the session)
        {
            host.move(canvas.right() - 8.0f, empty.y);
            host.keys("r");
            const bool carrying = v.tool() == SV::Tool::place;
            const funkgui::Point before = v.centre();
            const std::vector<pg::FileView> views = r.processor.editorPort().fileViews();
            panel.setPanelView(true);
            host.tick(1);
            P.eq("keys.panel_view_ends_tool", carrying && v.tool() == SV::Tool::select ? 1 : 0, 1);
            const funkgui::Rect hidden = v.canvasRect();
            host.move(hidden.x + 4.0f, hidden.centreY());
            host.tick(30);
            P.eq("keys.panel_view_waits", !centreMoved(before) && r.processor.editorPort().fileViews() == views ? 1 : 0, 1);
            panel.setPanelView(false);
            host.settle();
            frame(v, host);
        }
        // Escape: a gesture, then the tool, then the selection, then the canvas
        {
            const funkgui::Point c = partAt(v, "R2");
            host.click(c.x, c.y);
            host.move(c.x, c.y);
            host.keys("c");
            host.press(c.x, c.y);
            host.moveTo(c.x + 3.0f * s, c.y, 4);
            host.keys("escape");
            P.eq("keys.escape_cancels_gesture", !v.gestureOn() && v.tool() == SV::Tool::place ? 1 : 0, 1);
            host.release();
            host.keys("escape");
            P.eq("keys.escape_ends_tool", v.tool() == SV::Tool::select && v.selection().size() == 1 ? 1 : 0, 1);
            host.keys("escape");
            P.eq("keys.escape_clears", v.selection().empty() ? 1 : 0, 1);
            host.keys("escape");
            host.keys("r");
            P.eq("keys.escape_leaves_canvas", v.tool() == SV::Tool::select ? 1 : 0, 1);
            host.click(empty.x, empty.y);
        }
        // Tab: the parts in line order (the elements, then the input, the output)
        {
            // kPlanted: R1, C1, R2, R3 on lines 4 to 7, then the input (line 2) and the output (line 3)
            const std::vector<std::string> want{ "R1", "C1", "R2", "R3", "u", "vout" };
            std::vector<std::string> order;
            for (std::size_t k = 0; k < want.size(); ++k)
            {
                host.keys("tab");
                order.push_back(v.selection().empty() ? std::string() : v.selection().front());
            }
            P.eq("keys.tab_line_order", order == want ? 1 : 0, 1);
            host.keys("shift+tab");
            P.eq("keys.shift_tab", v.selection() == std::vector<std::string>{ want[want.size() - 2] } ? 1 : 0, 1);
            P.ge("keys.tab_announced", static_cast<double>(host.log.announcements.size()), static_cast<double>(want.size()));
        }
        // a part's key with the pointer away: placed at once, right of the selection
        {
            const std::string text = ed.text();
            host.move(canvas.x - 200.0f, canvas.y);   // over the explorer
            host.keys("tab");
            host.keys("l");
            host.tick(1);
            P.eq("keys.part_key_away_places", ed.text() != text && cir::read(ed.text()).element("L1") >= 0 && v.tool() == SV::Tool::select ? 1 : 0, 1);
            ed.undo();
            host.tick(1);
        }
        // Cmd-Shift-D: nothing while the IR tab shows
        {
            panel.openIrView(0);
            host.tick(1);
            host.keys("cmd+shift+d");
            host.tick(1);
            P.eq("keys.cmd_shift_d_ir", panel.irShown() && panel.viewPillsShown() == false ? 1 : 0, 1);
            panel.closeIrView();
            host.tick(1);
            P.eq("keys.cmd_shift_d_ir_kept_view", panel.schematicShown() ? 1 : 0, 1);
        }
        // the More menu: A, and its button
        {
            const funkgui::Point more = v.centreOfButton(SV::Button::more);
            host.click(more.x, more.y);
            const std::vector<std::string> labels = menuLabels(host);
            P.eq("menus.more_items", labels.size() == 8 && hasLabel(labels, "OTA") && hasLabel(labels, "Input (the netlist has one") ? 1 : 0, 1);
            P.eq("menus.more_input_disabled", host.chooseMenuItem("Input (the netlist has one: FMinus takes exactly one)") ? 0 : 1, 1);
            P.eq("menus.more_chooses", choose(host, "OTA") && v.tool() == SV::Tool::place && v.carried() == cir::Kind::Ota ? 1 : 0, 1);
            host.keys("escape");
            host.move(empty.x, empty.y);
            host.keys("a");
            P.eq("menus.more_key", host.pendingMenu() != nullptr ? 1 : 0, 1);
            P.eq("menus.more_vccs", choose(host, "VCCS") && v.carried() == cir::Kind::Vccs ? 1 : 0, 1);
            host.keys("escape");
        }
        // the part's menu, each item
        const std::string beforeMenus = ed.text();
        {
            const auto partMenu = [&](const char* part, const std::string& item) {
                const funkgui::Point c = partAt(v, part);
                host.click(c.x, c.y, popupMods());
                const bool ok = choose(host, item);
                host.tick(1);
                return ok;
            };
            const cir::Orient o = partOf(v.drawing(), "R2")->orient;
            P.eq("menus.part_rotate", partMenu("R2", "Rotate   ") && partOf(v.drawing(), "R2")->orient == cir::rotatedCw(o) ? 1 : 0, 1);
            P.eq("menus.part_rotate_back", partMenu("R2", "Rotate back") && partOf(v.drawing(), "R2")->orient == o ? 1 : 0, 1);
            P.eq("menus.part_mirror", partMenu("R2", "Mirror") && partOf(v.drawing(), "R2")->orient == cir::mirrored(o) ? 1 : 0, 1);
            P.eq("menus.part_show", partMenu("R2", "Show in netlist") && !panel.schematicShown() && ed.caret().line == 5
                                        && ed.line(5).substr(static_cast<std::size_t>(ed.caret().col)).rfind("2.2k", 0) == 0 ? 1 : 0, 1);
            panel.setSchematic(true);
            host.tick(1);
            P.eq("menus.part_delete", partMenu("R2", "Delete") && partOf(v.drawing(), "R2") == nullptr ? 1 : 0, 1);
            ed.setText(beforeMenus);
            host.tick(1);
            frame(v, host);
        }
        // the wire's menu
        if (const std::optional<cir::Pt> at = midWire(v.drawing(), "a"))
        {
            const funkgui::Point w = v.screenOf(*at);
            host.click(w.x, w.y, popupMods());
            P.eq("menus.wire_select_net", choose(host, "Select net") && v.selectedRuns() >= 1 ? 1 : 0, 1);
            const std::string text = ed.text();
            host.click(w.x, w.y, popupMods());
            P.eq("menus.wire_delete", choose(host, "Delete wire") && ed.text() != text && !cir::sameNetlist(ed.text(), text) ? 1 : 0, 1);
            ed.undo();
            host.tick(1);
            // a part selected beside the net's runs: Delete wire takes the wires alone
            {
                host.click(w.x, w.y, popupMods());
                const bool net = choose(host, "Select net");
                funkgui::Mods add;
                add.shift = true;
                const funkgui::Point rp = partAt(v, "R2");
                host.click(rp.x, rp.y, add);
                const bool both = v.selection() == std::vector<std::string>{ "R2" } && v.selectedRuns() >= 1;
                host.click(w.x, w.y, popupMods());
                const bool chose = choose(host, "Delete wire");
                P.eq("menus.wire_delete_only_wires", net && both && chose && partOf(v.drawing(), "R2") != nullptr && ed.text() != text
                                                         && has(ed.text(), "\nR2 ") ? 1 : 0, 1);
                ed.undo();
                host.tick(1);
            }
            host.click(w.x, w.y, popupMods());
            P.eq("menus.wire_show", choose(host, "Show in netlist") && !panel.schematicShown() && ed.line(ed.caret().line).substr(static_cast<std::size_t>(ed.caret().col)).rfind("a", 0) == 0 ? 1 : 0, 1);
            panel.setSchematic(true);
            host.tick(1);
        }
        // the canvas's menu
        {
            host.click(empty.x, empty.y, popupMods());
            P.eq("menus.canvas_select_all", choose(host, "Select all") && v.selection().size() == v.drawing().parts.size() ? 1 : 0, 1);
            host.click(empty.x, empty.y, popupMods());
            const float before = v.zoom();
            choose(host, "Fit");
            host.settle();
            P.eq("menus.canvas_fit", v.zoom() != before ? 1 : 0, 1);
            frame(v, host);
            const funkgui::Point c = partAt(v, "R2");
            host.click(c.x, c.y);
            host.keys("right");                       // a nudge, which Tidy takes back
            host.tick(1);
            const std::string text = ed.text();
            host.click(empty.x, empty.y, popupMods());
            cir::Op tidy;
            tidy.type = cir::Op::Type::layout;
            P.eq("menus.canvas_tidy", choose(host, "Tidy") && ed.text() == applied(text, { tidy }).text ? 1 : 0, 1);
            host.settle();
            frame(v, host);
            const std::string tidied = ed.text();
            host.click(empty.x, empty.y, popupMods());
            P.eq("menus.canvas_undo", choose(host, "Undo") && ed.text() == text ? 1 : 0, 1);
            host.click(empty.x, empty.y, popupMods());
            P.eq("menus.canvas_redo", choose(host, "Redo") && ed.text() == tidied ? 1 : 0, 1);
            host.click(empty.x, empty.y, popupMods());
            P.eq("menus.canvas_netlist", choose(host, "Netlist view") && !panel.schematicShown() ? 1 : 0, 1);
            panel.setSchematic(true);
            host.settle();
        }
    }

    // ---- ROOM -----------------------------------------------------------------------------------------------------------
    {
        Rig r(draftWith(dialsProgram("g.cir", 32), { { "g.cir", planted } }, 1), 0, 1000, 600);
        pg::ui::Panel& panel = *r.panel;
        funkgui::HeadlessHost& host = *r.host;
        r.run();
        std::printf("run: %s\n", panel.statusText().c_str());
        for (const pg::ui::Problem& pr : panel.problems())
            std::printf("problem: %s:%d: %s\n", pr.file.c_str(), pr.line, pr.text.c_str());
        panel.setSchematic(true);
        host.settle();
        SV& v = r.view();
        std::printf("1000 x 600, 32 dials: canvas %.0f x %.0f, folded %d\n", static_cast<double>(v.canvasRect().w),
                    static_cast<double>(v.canvasRect().h), panel.dialsFolded() ? 1 : 0);
        P.eq("room.small_32_folded", panel.dialsFolded() && panel.dialsBarShown() && !panel.centreOfDial(0) ? 1 : 0, 1);
        P.ge("room.small_32_canvas", v.canvasRect().h, 240.0);
        P.near("room.small_32_editor", panel.editorArea().h, 420.0 - 32.0, 0.5);
        writePng(P, host, "gui.schematic.small.png", "small");
        const funkgui::Point bar = panel.centreOfDialsBar();
        host.click(bar.x, bar.y);
        host.tick(1);
        std::printf("32 dials shown at 1000 x 600: the editor %.0f px tall\n", static_cast<double>(panel.editorArea().h));
        P.eq("room.bar_shows", !panel.dialsFolded() && panel.dialsBarShown() && panel.centreOfDial(31) ? 1 : 0, 1);
        P.eq("room.bar_shows_every_dial", panel.centreOfDial(0) && panel.centreOfDial(8) && panel.centreOfDial(31) ? 1 : 0, 1);
        P.ge("room.shown_editor_floor", panel.editorArea().h, 80.0);
        host.click(bar.x, bar.y);
        host.tick(1);
        P.eq("room.bar_hides", panel.dialsFolded() ? 1 : 0, 1);
        // a wire selected, and not: the canvas the same
        frame(v, host);
        const funkgui::Rect before = v.canvasRect();
        if (const std::optional<cir::Pt> mid = midWire(v.drawing(), "a"))
        {
            const funkgui::Point w = v.screenOf(*mid);
            host.click(w.x, w.y);
            host.tick(1);
            const funkgui::Rect with = v.canvasRect();
            host.click(v.canvasRect().x + 20.0f, v.canvasRect().y + 20.0f);
            host.tick(1);
            const funkgui::Rect after = v.canvasRect();
            P.eq("room.wire_selection_keeps_canvas", with.y == before.y && with.h == before.h && after.h == before.h ? 1 : 0, 1);
        }
        // the Netlist view: the problems list and the dials as before
        panel.setSchematic(false);
        host.tick(1);
        P.eq("room.netlist_unfolds", panel.dialsFolded() || panel.dialsBarShown() ? 0 : 1, 1);
    }
    {
        Rig r(draftWith(dialsProgram("g.cir", 9), { { "g.cir", planted } }, 1), 0, 1000, 600);
        r.run();
        r.panel->setSchematic(true);
        r.host->settle();
        std::printf("1000 x 600, 9 dials: canvas %.0f\n", static_cast<double>(r.view().canvasRect().h));
        P.eq("room.small_9_shown", !r.panel->dialsFolded() && !r.panel->dialsBarShown() && r.panel->centreOfDial(8) ? 1 : 0, 1);
        P.ge("room.small_9_canvas", r.view().canvasRect().h, 240.0);
    }
    {
        Rig r(draftWith(dialsProgram("g.cir", 32), { { "g.cir", planted } }, 1));
        r.run();
        r.panel->setSchematic(true);
        r.host->settle();
        std::printf("1360 x 760, explorer open, 32 dials: canvas %.0f x %.0f\n", static_cast<double>(r.view().canvasRect().w),
                    static_cast<double>(r.view().canvasRect().h));
        P.eq("room.default_explorer", r.panel->explorerShown() ? 1 : 0, 1);
        P.eq("room.default_32_shown", !r.panel->dialsFolded() && !r.panel->dialsBarShown() && r.panel->centreOfDial(31) ? 1 : 0, 1);
        P.ge("room.default_32_canvas", r.view().canvasRect().h, 240.0);
    }
    // twelve tabs and the IR tab at the least size: the pills icon-only, no tab under its least
    {
        std::vector<std::pair<std::string, std::string>> files = { { "g.cir", planted } };
        for (int k = 1; static_cast<int>(files.size()) + 1 < static_cast<int>(pg::kMaxFiles); ++k)
            files.push_back({ "f" + std::to_string(k) + ".fm", "// a file\n" });
        Rig r(draftWith(dialsProgram("g.cir", 4), files, 1), 0, 1000, 600);
        pg::ui::Panel& panel = *r.panel;
        funkgui::HeadlessHost& host = *r.host;
        panel.openIrView(0);
        host.tick(1);
        const funkgui::Point tab = panel.centreOfTab(1);
        host.click(tab.x, tab.y);
        panel.setSchematic(true);
        host.settle();
        P.eq("switch.twelve", panel.fileCount(), static_cast<int64_t>(pg::kMaxFiles));
        P.eq("switch.ir_open", panel.irOpen() && !panel.irShown() ? 1 : 0, 1);
        P.eq("switch.icon_only", panel.viewPillsShown() && !panel.viewPillsLabelled() ? 1 : 0, 1);
        float least = 1.0e9f;
        for (int i = 0; i < panel.fileCount(); ++i)
            least = std::min(least, panel.fileTabRect(i).w);
        std::printf("the least window: the narrowest tab %.1f, the last ends at %.1f, the pills %.1f to %.1f, the IR tab %.1f wide from %.1f\n",
                    static_cast<double>(least), static_cast<double>(panel.fileTabRect(panel.fileCount() - 1).right()),
                    static_cast<double>(panel.viewPillRect(false).x), static_cast<double>(panel.viewPillRect(true).right()),
                    static_cast<double>(panel.irTabRect().w), static_cast<double>(panel.irTabRect().x));
        P.ge("switch.no_tab_under_least", least, 36.0 - 1e-3);
        P.ge("switch.ir_tab_least", panel.irTabRect().w, 36.0 - 1e-3);
        P.eq("switch.apart", panel.fileTabRect(panel.fileCount() - 1).right() < panel.viewPillRect(false).x
                                 && panel.viewPillRect(true).right() <= panel.irTabRect().x ? 1 : 0, 1);
        P.near("switch.pill_width", panel.viewPillRect(true).w, 28.0, 1e-3);
        // dials folded (the synth's Init, its sections taller than the room): no section's frame between the
        // editor and the fold's bar
        if (panel.dialsFolded())
        {
            const funkgui::PrimList& f = host.draw();
            // the band between them (the bar's own box starts 12 px over its button's centre, its edge half a px more)
            const float top = panel.editorArea().bottom() + 1.0f, bottom = panel.centreOfDialsBar().y - 14.0f;
            int frames = 0;
            for (const funkgui::Prim& p : f.prims)
                if (p.tag == pg::ui::kSectionTag && p.y1 > top && p.y0 < bottom)
                {
                    ++frames;
                    std::printf("a frame between the editor and the bar: %.1f,%.1f to %.1f,%.1f (band %.1f to %.1f)\n", static_cast<double>(p.x0),
                                static_cast<double>(p.y0), static_cast<double>(p.x1), static_cast<double>(p.y1), static_cast<double>(top),
                                static_cast<double>(bottom));
                }
            P.eq("switch.folded_no_frames", frames, 0);
        }
        writePng(P, host, "gui.schematic.switch.png", "switch");
        // the pills to a screen reader: a radio group of two
        int radios = 0;
        for (const funkgui::A11yItem& it : host.accessibility())
            radios += it.role == funkgui::A11yRole::radioButton && (it.title == "Netlist" || it.title == "Schematic") ? 1 : 0;
        P.eq("switch.a11y_radios", radios, 2);
        // the narrow IR tab has no close box: a click where its cross would be, under the pointer, opens it
        {
            const funkgui::Rect ir = panel.irTabRect();
            host.moveTo(ir.x + ir.w - 14.0f, ir.centreY(), 2);
            host.click(ir.x + ir.w - 14.0f, ir.centreY());
            host.tick(1);
            P.eq("switch.narrow_ir_click_opens", panel.irOpen() && panel.irShown() ? 1 : 0, 1);
        }
    }

    // ---- MARKS, A11Y, PERSIST (and the PNGs of a wire and a move mid-drag) ---------------------------------------------
    for (int theme = 0; theme < 2; ++theme)
    {
        funkgui::UiPreferences::get().setTheme(theme);
        const std::string T = theme == 0 ? "marks." : "marks.paper.";
        Rig r(draftWith(dialsProgram("g.cir", 2), { { "g.cir", planted } }, 1), theme);
        pg::ui::Panel& panel = *r.panel;
        funkgui::HeadlessHost& host = *r.host;
        pg::ui::CodeAssist& code = panel.codeAssist();
        r.run();
        waitFor(host, [&] { return !code.checking() && code.settled(); });
        host.tick(60);
        waitFor(host, [&] { return !code.checking() && code.settled(); });
        panel.setSchematic(true);
        host.settle();
        SV& v = r.view();
        funkgui::TextEditor& ed = r.ed();
        frame(v, host);
        // an error on R1, a warning on R2, a note on C1, as the check would land them
        std::vector<pg::ui::Diagnostic> msgs(3);
        msgs[0] = { "g.cir", 4, 1, pg::ui::Diagnostic::Severity::error, "planted: an error on R1" };
        msgs[1] = { "g.cir", 6, 1, pg::ui::Diagnostic::Severity::warning, "planted: a warning on R2" };
        msgs[2] = { "g.cir", 5, 1, pg::ui::Diagnostic::Severity::info, "planted: a note on C1" };
        code.setCheckDiagnostics(msgs, { { "g.cir", ed.text() } });
        host.tick(1);
        P.eq(T + "count", v.problemCount(), 3);
        P.eq(T + "strip", v.stripRight() == "3 PROBLEMS" ? 1 : 0, 1);
        host.draw();                                  // the editor takes the theme's inks as it draws
        const funkgui::Theme th = pg::ui::productTheme(funkgui::Theme::byIndex(theme));
        const funkgui::TextEditorOptions& o = ed.options();
        const uint32_t err = packed(o.inks.error.resolve(th)), warn = packed(o.inks.warning.resolve(th)), note = packed(o.inks.info.resolve(th));
        const funkgui::PrimList& f = host.draw();
        int errorBadges = 0, outlines = 0, warningBadges = 0, rings = 0, noteFills = 0;
        for (const funkgui::Prim& p : f.prims)
        {
            if (p.tag != sch::kBadgeTag)
                continue;
            const bool hollow = (p.c0 >> 24) == 0;
            const float w = p.x1 - p.x0;
            errorBadges += !hollow && p.c0 == err && w < 30.0f ? 1 : 0;
            outlines += hollow && p.c1 == err && w > 30.0f ? 1 : 0;
            warningBadges += !hollow && p.c0 == warn ? 1 : 0;
            rings += hollow && p.c1 == note ? 1 : 0;
            noteFills += !hollow && p.c0 == note ? 1 : 0;
        }
        std::printf("%sinks: error %08x, warning %08x, note %08x; badges %d %d, outlines %d, rings %d\n", T.c_str(), err, warn, note,
                    errorBadges, warningBadges, outlines, rings);
        P.ge(T + "error_badge_ink", errorBadges, 1.0);
        P.eq(T + "error_outline_ink", outlines, 1);
        P.ge(T + "warning_badge_ink", warningBadges, 1.0);
        P.eq(T + "note_ring_ink", rings, 1);
        P.eq(T + "note_hollow", noteFills, 0);
        P.eq(T + "badges", v.centreOfBadge("R1") && v.centreOfBadge("R2") && v.centreOfBadge("C1") && !v.centreOfBadge("vout") ? 1 : 0, 1);
        if (theme == 1)
        {
            // PAPER, a part and a wire selected
            const funkgui::Point r2 = partAt(v, "R2");
            host.click(r2.x, r2.y);
            if (const std::optional<cir::Pt> mid = midWire(v.drawing(), "a"))
            {
                funkgui::Mods shift;
                shift.shift = true;
                const funkgui::Point w = v.screenOf(*mid);
                host.click(w.x, w.y, shift);
            }
            host.move(r2.x + 200.0f, r2.y + 200.0f);
            writePng(P, host, "gui.schematic.paper.png", "paper");
            continue;
        }
        // a message whose column falls on a node token marks that pin too: R2's "out" (line 6, column 6), a ring
        {
            const std::vector<pg::ui::Diagnostic> onPin{ { "g.cir", 6, 6, pg::ui::Diagnostic::Severity::error, "planted: on R2.2" } };
            code.setCheckDiagnostics(onPin, { { "g.cir", ed.text() } });
            host.tick(1);
            const funkgui::PrimList& g = host.draw();
            const funkgui::Point pin = pinAt(v, "R2", "2");
            int pinRings = 0;
            for (const funkgui::Prim& p : g.prims)
                pinRings += p.tag == sch::kBadgeTag && (p.c0 >> 24) == 0 && p.c1 == err && p.x1 - p.x0 < 30.0f
                              && std::fabs(0.5f * (p.x0 + p.x1) - pin.x) < 1.0f && std::fabs(0.5f * (p.y0 + p.y1) - pin.y) < 1.0f
                             ? 1 : 0;
            P.eq("marks.pin_ring", pinRings, 1);
            code.setCheckDiagnostics(msgs, { { "g.cir", ed.text() } });
            host.tick(1);
            P.eq("marks.restored", v.problemCount(), 3);
        }
        // the badge: a click selects its part and says its message
        {
            const funkgui::Point b = v.centreOfBadge("R2").value_or(funkgui::Point{});
            host.click(b.x, b.y);
            P.eq("marks.badge_click", v.selection() == std::vector<std::string>{ "R2" } && v.notice() == "planted: a warning on R2" ? 1 : 0, 1);
        }
        // the info strip's count steps through them: R1's line, C1's, R2's; Shift back
        {
            const funkgui::Point c = v.centreOfProblems();
            std::vector<std::string> seen;
            for (int k = 0; k < 3; ++k)
            {
                host.click(c.x, c.y);
                seen.push_back(v.selection().empty() ? std::string() : v.selection().front());
            }
            P.eq("marks.strip_steps", seen == std::vector<std::string>{ "R1", "C1", "R2" } ? 1 : 0, 1);
            funkgui::Mods shift;
            shift.shift = true;
            host.click(c.x, c.y, shift);
            P.eq("marks.strip_steps_back", v.selection() == std::vector<std::string>{ "C1" } ? 1 : 0, 1);
            const cir::Part* c1 = partOf(v.drawing(), "C1");
            P.near("marks.strip_centres", v.centre().x, 0.5 * (c1->body.x0 + c1->body.x1), 1e-3);
        }
        // a layout-only gesture: still RUNNING, no check asked, the planted error's badge and squiggle kept
        {
            P.eq("marks.running_before", panel.statusText().rfind("RUNNING", 0) == 0 ? 1 : 0, 1);
            const float s = sch::kUnitPx * v.zoom();
            const funkgui::Point c = partAt(v, "R2");
            // two parts moved, mid-drag (the PNG), then let go with Escape
            {
                const funkgui::Point c3 = partAt(v, "R3");
                funkgui::Mods shift;
                shift.shift = true;
                host.click(c.x, c.y);
                host.click(c3.x, c3.y, shift);
                host.press(c3.x, c3.y);
                host.moveTo(c3.x + 3.0f * s, c3.y + 2.0f * s, 6);
                P.eq("marks.moving_two", v.selection().size() == 2 && v.gestureOn() ? 1 : 0, 1);
                writePng(P, host, "gui.schematic.move.png", "move");
                host.keys("escape");
                host.release();
                host.tick(1);
                P.eq("marks.escape_drops_move", cir::sameNetlist(ed.text(), r.processor.editorPort().running()[1].text) && !ed.canUndo() ? 1 : 0, 1);
            }
            host.press(c.x, c.y);
            host.moveTo(c.x + 2.0f * s, c.y - 5.0f * s, 6);
            host.release();
            host.tick(1);
            P.eq("marks.layout_only", ed.text() != r.processor.editorPort().running()[1].text && cir::sameNetlist(ed.text(), r.processor.editorPort().running()[1].text) ? 1 : 0, 1);
            host.tick(90);
            std::printf("after the move: %s\n", panel.statusText().c_str());
            P.eq("marks.still_running", panel.statusText().rfind("RUNNING", 0) == 0 ? 1 : 0, 1);
            P.eq("marks.no_check", code.checking() ? 0 : 1, 1);
            int squiggles = 0;
            for (const funkgui::Decoration& d : ed.decorations())
                squiggles += d.kind == funkgui::DecorationKind::error && d.from.line == 3 ? 1 : 0;
            P.eq("marks.squiggle_kept", squiggles, 1);
            P.eq("marks.badge_kept", v.centreOfBadge("R1") ? 1 : 0, 1);
            ed.undo();
            host.tick(1);
        }
        // a wire mid-drag, for the PNG
        {
            frame(v, host);
            host.click(v.canvasRect().x + 20.0f, v.canvasRect().y + 20.0f);   // nothing selected
            const funkgui::Point p = pinAt(v, "R2", "2");
            const float s = sch::kUnitPx * v.zoom();
            host.press(p.x, p.y);
            host.moveTo(p.x + 6.0f * s, p.y, 6);
            host.moveTo(p.x + 6.0f * s, p.y + 5.0f * s, 6);
            writePng(P, host, "gui.schematic.wire.png", "wire");
            host.keys("escape");
            host.release();
            host.tick(1);
        }
        // A11Y: a group, a part an item and a net an item, ids kept across a pan and a zoom, every item visible
        {
            const std::vector<funkgui::A11yItem> items = host.accessibility();
            const auto group = std::find_if(items.begin(), items.end(), [](const funkgui::A11yItem& it) { return it.id == SV::kA11yBase; });
            P.eq("a11y.group", group != items.end() && group->title == "Schematic, g.cir" ? 1 : 0, 1);
            std::vector<uint32_t> parts, nets;
            bool visible = true;
            std::string r2Title;
            uint32_t r2Id = 0;
            for (const funkgui::A11yItem& it : items)
            {
                if (it.parent == SV::kA11yBase + 1)
                {
                    parts.push_back(it.id);
                    if (it.title.rfind("R2,", 0) == 0)
                    {
                        r2Title = it.title;
                        r2Id = it.id;
                    }
                }
                if (it.parent == SV::kA11yBase + 2)
                    nets.push_back(it.id);
                if ((it.id & 0xFF000000u) == SV::kA11yBase)
                    visible = visible && it.visible && it.bounds.w > 0.0f && it.bounds.h > 0.0f;
            }
            int withPins = 0;
            for (const cir::Net& n : v.drawing().nets)
                withPins += n.pins.empty() ? 0 : 1;
            std::printf("a11y: %s\n", r2Title.c_str());
            P.eq("a11y.parts", static_cast<int64_t>(parts.size()), static_cast<int64_t>(v.drawing().parts.size()));
            P.eq("a11y.nets", static_cast<int64_t>(nets.size()), static_cast<int64_t>(withPins));
            P.eq("a11y.part_words", r2Title == "R2, resistor 2.2k, from a to out" ? 1 : 0, 1);
            P.eq("a11y.visible", visible ? 1 : 0, 1);
            const uint32_t rev = panel.a11yRevision();
            v.panBy(37.0f, -21.0f);
            v.zoomTo(1.7f, { v.canvasRect().centreX(), v.canvasRect().centreY() });
            host.tick(1);
            std::vector<uint32_t> again;
            for (const funkgui::A11yItem& it : host.accessibility())
                if (it.parent == SV::kA11yBase + 1 || it.parent == SV::kA11yBase + 2)
                    again.push_back(it.id);
            std::vector<uint32_t> all = parts;
            all.insert(all.end(), nets.begin(), nets.end());
            P.eq("a11y.ids_kept", again == all && panel.a11yRevision() == rev ? 1 : 0, 1);
            // at 400 %, parts off the canvas: every item still visible, its bounds on the canvas
            {
                const funkgui::Rect cr = v.canvasRect();
                v.zoomTo(4.0f, { cr.centreX(), cr.centreY() });
                host.tick(1);
                bool off = false;
                for (const cir::Part& p : v.drawing().parts)
                    off = off || !cr.contains(partAt(v, p.name.c_str()));
                bool on = true;
                int n = 0;
                for (const funkgui::A11yItem& it : host.accessibility())
                    if (it.parent == SV::kA11yBase + 1 || it.parent == SV::kA11yBase + 2)
                    {
                        ++n;
                        on = on && it.visible && it.bounds.w > 0.0f && it.bounds.h > 0.0f && it.bounds.x >= cr.x - 0.01f
                          && it.bounds.y >= cr.y - 0.01f && it.bounds.right() <= cr.right() + 0.01f && it.bounds.bottom() <= cr.bottom() + 0.01f;
                    }
                P.eq("a11y.on_canvas_at_400", off && on && n == static_cast<int>(all.size()) ? 1 : 0, 1);
            }
            panel.a11yAction(r2Id, funkgui::A11yAction::focus);
            host.tick(1);
            const cir::Part* p = partOf(v.drawing(), "R2");
            P.eq("a11y.focus_selects", v.selection() == std::vector<std::string>{ "R2" } ? 1 : 0, 1);
            P.near("a11y.focus_centres", v.centre().x, 0.5 * (p->body.x0 + p->body.x1), 1e-3);
        }
        // PERSIST: opening kept nothing; the switch, a zoom and a pan are kept, and a Panel opened again has them
        {
            const std::vector<pg::FileView> views = r.processor.editorPort().fileViews();
            P.eq("persist.switch_kept", views.size() == 1 && views[0].file == "g.cir" && views[0].schematic ? 1 : 0, 1);
            host.keys("cmd+=");
            const funkgui::Point e{ v.canvasRect().x + 30.0f, v.canvasRect().y + 30.0f };
            host.press(e.x, e.y, popupMods());
            host.moveTo(e.x + 50.0f, e.y + 30.0f, 4);
            host.release();
            host.tick(1);
            const float zoom = v.zoom();
            const funkgui::Point centre = v.centre();
            const std::vector<pg::FileView> kept = r.processor.editorPort().fileViews();
            P.eq("persist.zoom_pan_kept", kept.size() == 1 && kept[0].zoom == zoom && kept[0].x == centre.x && kept[0].y == centre.y ? 1 : 0, 1);
            r.close();
            r.open(theme);
            P.eq("persist.reopened_view", r.panel->schematicShown() ? 1 : 0, 1);
            P.eq("persist.reopened_zoom_pan", r.view().zoom() == zoom && r.view().centre().x == centre.x && r.view().centre().y == centre.y ? 1 : 0, 1);
        }
        // a problems row's click: a netlist shown in its Schematic view selects and centres the part
        {
            pg::ui::Panel& p2 = *r.panel;
            funkgui::HeadlessHost& h2 = *r.host;
            funkgui::TextEditor& e2 = p2.editor();
            e2.setText(cir::written(std::string(kPlanted).replace(kPlanted.find("2.2k"), 4, "1M")));
            h2.tick(1);
            const funkgui::Point tab0 = p2.centreOfTab(0);
            h2.click(tab0.x, tab0.y);
            r.run();
            int row = -1;
            for (std::size_t k = 0; k < p2.problems().size() && k < 4; ++k)
                if (p2.problems()[k].file == "g.cir" && p2.problems()[k].line == 6)
                    row = static_cast<int>(k);
            for (const pg::ui::Problem& pr : p2.problems())
                std::printf("problem: %s:%d: %s\n", pr.file.c_str(), pr.line, pr.text.c_str());
            P.ge("marks.problem_row", row, 0.0);
            if (row >= 0)
            {
                const funkgui::Point at = p2.centreOfProblem(row);
                h2.click(at.x, at.y);
                h2.settle();
                SV& v2 = r.view();
                const cir::Part* r2 = partOf(v2.drawing(), "R2");
                P.eq("marks.problem_row_schematic", p2.schematicShown() && v2.selection() == std::vector<std::string>{ "R2" } ? 1 : 0, 1);
                P.near("marks.problem_row_centres", v2.centre().y, 0.5 * (r2->body.y0 + r2->body.y1), 1e-3);
            }
        }
    }
    funkgui::UiPreferences::get().setTheme(0);

    // ---- DIALS: the circuit dials on the drawing (dials.png) --------------------------------------------------------
    {
        // A program that holds screamer.cir's module and never calls its set(): its pots are dials of their own.
        const std::string program = pg::product::kIsInstrument
            ? "#include \"screamer.cir\"\nmodule Voice\n{\n    Screamer amp;\n    bool held;\n\n    void init(float rate)\n    {\n"
              "        amp.init(rate);\n    }\n\n    void start(float note, float velocity)\n    {\n        held = true;\n    }\n\n"
              "    void stop()\n    {\n        held = false;\n    }\n\n"
              "    bool process(float* restrict left, float* restrict right, int n)\n    {\n        for (int i = 0; i < n; ++i)\n"
              "        {\n            const float y = amp.tick(0);\n            left[i] += y;\n            right[i] += y;\n        }\n"
              "        return held;\n    }\n};\n"
            : "#include \"screamer.cir\"\nmodule Effect\n{\n    Screamer amp[2];\n\n    void init(float rate)\n    {\n"
              "        amp[0].init(rate);\n        amp[1].init(rate);\n    }\n\n"
              "    void process(float* restrict left, float* restrict right, int n)\n    {\n        for (int i = 0; i < n; ++i)\n"
              "        {\n            left[i] = amp[0].tick(left[i]);\n            right[i] = amp[1].tick(right[i]);\n        }\n    }\n};\n";
        Rig r(draftWith(program, { { "screamer.cir", libraryText("screamer.cir") } }, 1));
        r.run();
        pg::ui::Panel& panel = *r.panel;
        funkgui::HeadlessHost& host = *r.host;
        pg::EditorPort& port = r.processor.editorPort();
        std::string names;
        for (const pg::Control& c : port.controls())
            if (c.used)
                names += (names.empty() ? "" : " ") + std::string(c.name);
        std::printf("dials: %s%s; %s\n", port.status().errors.c_str(), port.status().notes.c_str(), names.c_str());
        P.eq("dials.run", port.status().ok && names == "drive tone level" ? 1 : 0, 1);
        const funkgui::Point pill = panel.centreOfViewPill(true);
        host.click(pill.x, pill.y);
        host.settle();
        SV& v = r.view();
        host.settle();                            // opening fits the drawing (never under 60 %, where knobs show)
        std::printf("dials: %d knobs at %.0f%%; Pdrive's slot %d, Ptone's %d, Plevel's %d\n", v.knobCount(),
                    static_cast<double>(v.zoom() * 100.0f), v.knobSlot("Pdrive"), v.knobSlot("Ptone"), v.knobSlot("Plevel"));
        P.eq("dials.knobs", v.knobCount() == 3 && v.knobSlot("Pdrive") == 0 && v.knobSlot("Ptone") == 1 && v.knobSlot("Plevel") == 2 ? 1 : 0, 1);
        {
            // each knob and its pot on the canvas
            bool inside = true;
            const funkgui::Point none{ -1.0e4f, -1.0e4f };
            for (const char* part : { "Pdrive", "Ptone", "Plevel" })
                inside = inside && v.canvasRect().contains(v.centreOfKnob(part).value_or(none))
                      && v.canvasRect().contains(v.centreOfPart(part).value_or(none));
            P.eq("dials.knobs_on_canvas", inside ? 1 : 0, 1);
        }
        writePng(P, host, "gui.schematic.dials.png", "dials");
        frame(v, host);                           // 100 %, the drawing centred: room in the canvas's lower left
        host.settle();
        // dragging Pdrive's knob 60 px up turns the dial of its slot (the program card's): +0.4 of its range
        funkgui::ParamPort& drive = *r.processor.ports()[0];
        const float before = drive.value01();
        const funkgui::Point k = v.centreOfKnob("Pdrive").value_or(funkgui::Point{});
        host.move(k.x, k.y);
        host.tick(1);
        const std::string hover = v.stripLeft();
        host.press(k.x, k.y);
        host.moveTo(k.x, k.y - 60.0f, 6);
        const std::string turning = v.stripLeft();
        host.release();
        host.tick(2);
        std::printf("knob: %.3f -> %.3f (hover \"%s\", turning \"%s\")\n", static_cast<double>(before), static_cast<double>(drive.value01()),
                    hover.c_str(), turning.c_str());
        P.near("dials.knob_turns_dial", drive.value01(), before + 0.4f, 0.01);
        P.eq("dials.hover_shows_value", hover.rfind("drive 50 %", 0) == 0 && turning.rfind("drive ", 0) == 0 ? 1 : 0, 1);
        P.eq("dials.text_untouched", port.draft().files[1].text == libraryText("screamer.cir") ? 1 : 0, 1);
        host.doubleClick(k.x, k.y);
        host.tick(2);
        P.near("dials.double_click_default", drive.value01(), pg::normOf(port.controls()[0], port.controls()[0].def), 1e-6);
        // its second press began a drag, which the double-click ends: the host's unbounded drag (beginDrag's) is off
        P.eq("dials.double_click_gesture_closed", host.log.unbounded ? 0 : 1, 1);
        // the body still moves the part (into the canvas's empty lower left)
        const funkgui::Point body = partAt(v, "Pdrive");
        const cir::Pt was = partOf(v.drawing(), "Pdrive") != nullptr ? partOf(v.drawing(), "Pdrive")->origin : cir::Pt{};
        lDrag(host, body, { v.canvasRect().x + 50.0f, v.canvasRect().bottom() - 40.0f });
        host.settle();
        const cir::Part* moved = partOf(v.drawing(), "Pdrive");
        std::printf("Pdrive dragged by its body: %d,%d -> %d,%d\n", was.x, was.y, moved != nullptr ? moved->origin.x : 0,
                    moved != nullptr ? moved->origin.y : 0);
        P.eq("dials.body_still_moves", moved != nullptr && !(moved->origin == was)
                                           && drive.value01() == pg::normOf(port.controls()[0], port.controls()[0].def) ? 1 : 0, 1);
    }
    {
        // A switch's chip: a click steps its position (ClipSwitch, FMinus's switch.cir, open by default).
        const std::string netlist = ".title ClipSwitch\n.input  u    in   scale=1\n.output vout a\n.switch clip 2 0\n"
                                    ".oversample 4 iir blocks=auto\n.adaa on\n.solver omega(1)\n"
                                    ".target solver=-60dB alias=-60dB level=4.5V freq=3k\n"
                                    "R1     in a  2.2k\nC1     a  0  10n\nS1     a  d  clip 1\nD1     d  0  1N914\nD2     0  d  1N914\n.end\n";
        const std::string program = pg::product::kIsInstrument
            ? "#include \"sw.cir\"\nmodule Voice\n{\n    ClipSwitch clip;\n\n    void init(float rate)\n    {\n        clip.init(rate);\n    }\n\n"
              "    void start(float note, float velocity)\n    {\n    }\n\n"
              "    bool process(float* restrict left, float* restrict right, int n)\n    {\n        return false;\n    }\n};\n"
            : "#include \"sw.cir\"\nmodule Effect\n{\n    ClipSwitch clip;\n\n    void init(float rate)\n    {\n        clip.init(rate);\n    }\n\n"
              "    void process(float* restrict left, float* restrict right, int n)\n    {\n        for (int i = 0; i < n; ++i)\n"
              "            left[i] = clip.tick(left[i]);\n    }\n};\n";
        Rig r(draftWith(program, { { "sw.cir", netlist } }, 1));
        r.run();
        pg::ui::Panel& panel = *r.panel;
        funkgui::HeadlessHost& host = *r.host;
        const funkgui::Point pill = panel.centreOfViewPill(true);
        host.click(pill.x, pill.y);
        host.settle();
        SV& v = r.view();
        frame(v, host);
        funkgui::ParamPort& sw = *r.processor.ports()[0];
        const pg::Control c = r.processor.editorPort().controls()[0];
        const std::optional<funkgui::Point> chip = v.centreOfKnob("S1");
        const float at0 = pg::plainOf(c, sw.value01());
        if (chip)
            host.click(chip->x, chip->y);
        host.tick(2);
        const float at1 = pg::plainOf(c, sw.value01());
        if (chip)
            host.click(chip->x, chip->y);
        host.tick(2);
        const float at2 = pg::plainOf(c, sw.value01());
        std::printf("a switch's chip: %s, %g -> %g -> %g\n", chip ? "drawn" : "MISSING", static_cast<double>(at0), static_cast<double>(at1),
                    static_cast<double>(at2));
        P.eq("dials.chip_steps", chip && v.knobSlot("S1") == 0 && std::string(c.name) == "clip" && at0 == 0.0f && at1 == 1.0f && at2 == 0.0f
                                     ? 1 : 0, 1);
    }
    {
        // A program that calls the module's set() itself: no knobs, and the info strip says why (own.png).
        const std::string program = pg::product::kIsInstrument
            ? "#include \"screamer.cir\"\nmodule Voice\n{\n    Screamer amp;\n    bool held;\n\n    void init(float rate)\n    {\n"
              "        amp.init(rate);\n        amp.set(0.5f, 0.5f, 0.7f);\n    }\n\n"
              "    void start(float note, float velocity)\n    {\n        held = true;\n    }\n\n"
              "    void stop()\n    {\n        held = false;\n    }\n\n"
              "    bool process(float* restrict left, float* restrict right, int n)\n    {\n        for (int i = 0; i < n; ++i)\n"
              "        {\n            const float y = amp.tick(0);\n            left[i] += y;\n            right[i] += y;\n        }\n"
              "        return held;\n    }\n};\n"
            : "#include \"screamer.cir\"\nmodule Effect\n{\n    Screamer amp[2];\n\n    void init(float rate)\n    {\n"
              "        amp[0].init(rate);\n        amp[1].init(rate);\n        amp[0].set(0.5f, 0.5f, 0.7f);\n"
              "        amp[1].set(0.5f, 0.5f, 0.7f);\n    }\n\n"
              "    void process(float* restrict left, float* restrict right, int n)\n    {\n        for (int i = 0; i < n; ++i)\n"
              "        {\n            left[i] = amp[0].tick(left[i]);\n            right[i] = amp[1].tick(right[i]);\n        }\n    }\n};\n";
        Rig r(draftWith(program, { { "screamer.cir", libraryText("screamer.cir") } }, 1));
        r.run();
        pg::ui::Panel& panel = *r.panel;
        funkgui::HeadlessHost& host = *r.host;
        const pg::CompileStatus st = r.processor.editorPort().status();
        const funkgui::Point pill = panel.centreOfViewPill(true);
        host.click(pill.x, pill.y);
        host.settle();
        SV& v = r.view();
        const std::string why = "program.fm:10: Screamer's pots have no dials: the program calls its set()";
        std::printf("own: %s%s; %d knobs; the strip \"%s\"\n", st.errors.c_str(), st.notes.c_str(), v.knobCount(), v.stripRight().c_str());
        P.eq("dials.own_says_why", st.ok && st.notes.empty() && v.knobCount() == 0 && v.stripRight() == why ? 1 : 0, 1);
        writePng(P, host, "gui.schematic.own.png", "own");
    }

    // ---- PLAY: Play this circuit (play.png) -------------------------------------------------------------------------
    {
        const std::string init = pg::kExamples[0].source;
        Rig r(draftWith(init, { { "diodetone.cir", libraryText("diodetone.cir") } }, 1));
        r.run();
        pg::ui::Panel& panel = *r.panel;
        funkgui::HeadlessHost& host = *r.host;
        pg::EditorPort& port = r.processor.editorPort();
        const funkgui::Point pill = panel.centreOfViewPill(true);
        host.click(pill.x, pill.y);
        host.settle();
        SV& v = r.view();
        P.eq("play.offered", v.stripButton() == "Play this circuit" && v.stripRight() == "Not played: no program #includes diodetone.cir"
                                 && v.centreOfStripButton().has_value() ? 1 : 0, 1);
        const funkgui::Point b = v.centreOfStripButton().value_or(funkgui::Point{});
        host.click(b.x, b.y);
        r.processor.settle();
        waitFor(host, [&] { return port.status().ok && port.running().size() == port.draft().files.size(); });
        host.settle();
        const pg::cir::HostProgram want = pg::cir::hostProgram("diodetone.cir", libraryText("diodetone.cir"), pg::product::kIsInstrument);
        const pg::Draft d = port.draft();
        std::string names;
        for (const pg::Control& c : port.controls())
            if (c.used)
                names += (names.empty() ? "" : " ") + std::string(c.name);
        std::printf("played: %zu files, %s%s; dials %s; strip: %s [%s]\n", d.files.size(), port.status().errors.c_str(), port.status().notes.c_str(),
                    names.c_str(), v.stripRight().c_str(), v.stripButton().c_str());
        P.eq("play.program_written", !d.files.empty() && d.files[0].text == want.text ? 1 : 0, 1);
        P.eq("play.library_tabs", static_cast<int64_t>(d.files.size()), pg::product::kIsInstrument ? 4 : 2);
        P.eq("play.runs", port.status().ok && port.status().notes.empty() && names == "tone" ? 1 : 0, 1);
        P.eq("play.view_stays", panel.schematicShown() && v.file() == "diodetone.cir" ? 1 : 0, 1);
        P.eq("play.undo_offered", v.stripButton() == "Undo" && v.stripRight() == "program.fm now plays diodetone.cir" ? 1 : 0, 1);
        writePng(P, host, "gui.schematic.play.png", "play");
        // a probe's reason the strip draws (a node typed since RUN: no card) takes the strip, and the Undo goes with
        // the note it takes back; a wire that resolves (the card's) leaves the note and its Undo
        {
            funkgui::TextEditor& ed = r.ed();
            const std::string was = ed.text();
            const std::size_t end = was.find("\n.end");
            ed.selectAll();
            ed.insert(was.substr(0, end + 1) + "R9     b  n9  1k\nR10    n9 0   1k\n" + was.substr(end + 1));
            host.tick(2);
            const std::optional<cir::Pt> n9 = midWire(v.drawing(), "n9");
            const funkgui::Point at9 = n9 ? v.screenOf(*n9) : funkgui::Point{ -1.0e4f, -1.0e4f };
            host.click(at9.x, at9.y);
            host.tick(2);
            const std::string strip = v.stripRight(), button = v.stripButton();
            ed.undo();
            host.tick(2);
            const std::optional<cir::Pt> a = midWire(v.drawing(), "a");
            const funkgui::Point atA = a ? v.screenOf(*a) : funkgui::Point{ -1.0e4f, -1.0e4f };
            host.click(atA.x, atA.y);
            host.tick(2);
            std::printf("play: n9 selected, strip \"%s\" [%s]; a, strip \"%s\" [%s]\n", strip.c_str(), button.c_str(),
                        v.stripRight().c_str(), v.stripButton().c_str());
            P.eq("play.reason_hides_undo", strip == "Probe: n9 is not in the program that plays (edited since RUN)" && button.empty() ? 1 : 0, 1);
            P.eq("play.resolved_keeps_undo", ed.text() == was && v.probeNet(0) == "a" && v.stripButton() == "Undo"
                                                 && v.stripRight() == "program.fm now plays diodetone.cir" ? 1 : 0, 1);
        }
        const funkgui::Point u = v.centreOfStripButton().value_or(funkgui::Point{});
        host.click(u.x, u.y);
        host.tick(2);
        P.eq("play.undo_one_step", port.draft().files[0].text == init && v.stripButton() == "Play this circuit" ? 1 : 0, 1);
    }

    // ---- PROBES: the live probes on Play this circuit's program for diodetone.cir (probes.png) ------------------------
    {
        constexpr bool kInstrument = pg::product::kIsInstrument;
        constexpr double kRate = 48000.0;
        constexpr int kBlock = 256;
        const std::string dt = libraryText("diodetone.cir");
        pg::Draft d = draftWith(pg::cir::hostProgram("diodetone.cir", dt, kInstrument).text, { { "diodetone.cir", dt } }, 1);
        d.files = pg::withLibraryIncludes(d.files);
        Rig r(d);
        pg::ui::Panel& panel = *r.panel;
        funkgui::HeadlessHost& host = *r.host;
        pg::EditorPort& port = r.processor.editorPort();
        const funkgui::Point pill = panel.centreOfViewPill(true);
        host.click(pill.x, pill.y);
        host.settle();
        SV& v = r.view();
        const auto wireOf = [&](const char* net) {
            const std::optional<cir::Pt> at = midWire(v.drawing(), net);
            return at ? v.screenOf(*at) : funkgui::Point{ -1.0e4f, -1.0e4f };
        };
        const auto spoken = [&](const std::string& text) {
            return std::find(host.log.announcements.begin(), host.log.announcements.end(), text) != host.log.announcements.end();
        };
        funkgui::Mods alt;
        alt.alt = true;
        // what plays (the processor's first program) lacks the netlist the files play: "not playing", and a wire selected
        // moves neither the canvas nor the frame rate
        const funkgui::Rect canvas0 = v.canvasRect();
        const bool rate0 = panel.wantsFullRate();
        const funkgui::Point a = wireOf("a");
        host.click(a.x, a.y);
        host.tick(2);
        std::printf("probes: before RUN, slot 0 %s: \"%s\"; strip \"%s\"\n", v.probeNet(0).c_str(), v.probeReason(0).c_str(), v.stripRight().c_str());
        P.eq("probes.reason_not_playing", v.probeNet(0) == "a" && v.probeReason(0) == "not playing: Cmd-Return runs it"
                                              && v.stripRight() == "Probe: not playing: Cmd-Return runs it" ? 1 : 0, 1);
        P.eq("probes.reason_not_playing_spoken", spoken("Probe: not playing: Cmd-Return runs it") ? 1 : 0, 1);
        const funkgui::Rect canvas1 = v.canvasRect();
        P.eq("probes.select_keeps_canvas", canvas1.x == canvas0.x && canvas1.y == canvas0.y && canvas1.w == canvas0.w && canvas1.h == canvas0.h ? 1 : 0, 1);
        P.eq("probes.select_keeps_rate", !rate0 && !panel.wantsFullRate() && v.settled() && !v.probesLive() && v.probeCard().h == 0.0f ? 1 : 0, 1);
        P.eq("probes.nothing_sent", v.probeSet() == pg::ProbeSet{} && panel.probeOwner() == 0 ? 1 : 0, 1);
        r.run();
        P.eq("probes.run", port.status().ok && !port.status().probes.empty() && port.status().probes[0].file == "diodetone.cir" ? 1 : 0, 1);
        // a node typed since RUN: not in the program that plays
        {
            funkgui::TextEditor& ed = r.ed();
            const std::string was = ed.text();
            const std::size_t end = was.find("\n.end");
            ed.selectAll();
            ed.insert(was.substr(0, end + 1) + "R9     b  n9  1k\nR10    n9 0   1k\n" + was.substr(end + 1));
            host.tick(2);
            const funkgui::Point n9 = wireOf("n9");
            host.click(n9.x, n9.y);
            host.tick(2);
            std::printf("probes: edited, slot 0 %s: \"%s\"\n", v.probeNet(0).c_str(), v.probeReason(0).c_str());
            P.eq("probes.reason_edited", v.probeNet(0) == "n9" && v.probeReason(0) == "n9 is not in the program that plays (edited since RUN)"
                                             && v.stripRight() == "Probe: n9 is not in the program that plays (edited since RUN)"
                                             && spoken("Probe: n9 is not in the program that plays (edited since RUN)") ? 1 : 0, 1);
            ed.undo();
            host.tick(2);
            P.eq("probes.edit_undone", ed.text() == was ? 1 : 0, 1);
        }
        // the audio: an effect's 1 kHz sine at 0.5 V, a synth's A4 (after a while with no note)
        r.processor.prepareToPlay(kRate, kBlock);
        juce::AudioBuffer<float> buffer(2, kBlock);
        double phase = 0.0;
        const auto push = [&](int blocks, bool noteOn) {
            for (int b = 0; b < blocks; ++b)
            {
                juce::MidiBuffer midi;
                if (noteOn && b == 0)
                    midi.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(110)), 0);
                for (int i = 0; i < kBlock; ++i)
                {
                    const float x = kInstrument ? 0.0f : static_cast<float>(0.5 * std::sin(phase));
                    phase += 2.0 * 3.14159265358979 * 1000.0 / kRate;
                    buffer.setSample(0, i, x);
                    buffer.setSample(1, i, x);
                }
                r.processor.processBlock(buffer, midi);
            }
        };
        const auto play = [&](int frames) {
            for (int k = 0; k < frames; ++k)
            {
                push(1, false);
                host.tick(1);
            }
        };
        const funkgui::Point a2 = wireOf("a");          // (the drawing as it is now)
        host.click(a2.x, a2.y);                  // slot 0: net a again
        play(4);
        if constexpr (kInstrument)
        {
            std::printf("probes: no note, slot 0: \"%s\"\n", v.probeReason(0).c_str());
            P.eq("probes.reason_no_voice", v.probeReason(0) == "no voice sounding" && v.stripRight() == "Probe: no voice sounding"
                                               && spoken("Probe: no voice sounding") && !v.probesLive() ? 1 : 0, 1);
            // a note, let go: "no voice sounding" again once it ends, drawn but not spoken again
            push(1, true);
            play(4);
            const bool sounded = v.probeReason(0).empty() && v.probesLive();
            {
                juce::MidiBuffer midi;
                midi.addEvent(juce::MidiMessage::noteOff(1, 69), 0);
                buffer.clear();
                r.processor.processBlock(buffer, midi);
            }
            for (int k = 0; k < 600 && v.probeReason(0) != "no voice sounding"; ++k)
                play(1);
            const auto times = std::count(host.log.announcements.begin(), host.log.announcements.end(), std::string("Probe: no voice sounding"));
            std::printf("probes: a note let go, slot 0 \"%s\", \"no voice sounding\" spoken %d times\n", v.probeReason(0).c_str(),
                        static_cast<int>(times));
            P.eq("probes.no_voice_spoken_once", sounded && v.probeReason(0) == "no voice sounding" && times == 1 ? 1 : 0, 1);
            push(1, true);
        }
        // two nets pinned (Alt-click on their wires): lanes 1 and 2, numbered flags
        const funkgui::Point c = wireOf("c"), b = wireOf("b");
        host.click(c.x, c.y, alt);
        host.click(b.x, b.y, alt);
        play(150);
        pg::Meters m;
        port.meters(m);
        const pg::ProbeSet armed = v.probeSet();
        std::printf("probes: pinned %zu, set {%llu, %d, %d, %d %d %d %d}, meters' set %s, instance %d\n", v.pinnedProbes().size(),
                    static_cast<unsigned long long>(armed.program), armed.circuit, armed.instance, armed.nodes[0], armed.nodes[1],
                    armed.nodes[2], armed.nodes[3], m.probeSet == armed ? "the same" : "another", static_cast<int>(m.probeInstance));
        P.eq("probes.pinned", v.pinnedProbes() == std::vector<std::string>{ "c", "b" } && v.probeNet(1) == "c" && v.probeNet(2) == "b" ? 1 : 0, 1);
        P.eq("probes.armed", armed.armed() && armed.program == port.status().program && armed.nodes[0] >= 0 && armed.nodes[1] >= 0
                                 && armed.nodes[2] >= 0 && m.probeSet == armed && panel.probeOwner() > 0 ? 1 : 0, 1);
        P.eq("probes.live", v.probesLive() && !v.settled() && panel.wantsFullRate() ? 1 : 0, 1);
        // the numbers the card and the lanes show are the meters' within 1 %
        bool numbers = true;
        for (int slot = 0; slot < 3; ++slot)
        {
            const std::string words = v.probeWords(slot);
            float peak = 0.0f, rms = 0.0f, mean = 0.0f;
            const std::size_t at = words.find("PEAK ");
            const bool read = at != std::string::npos && std::sscanf(words.c_str() + at, "PEAK %f V   RMS %f V   MEAN %f V", &peak, &rms, &mean) == 3;
            const pg::ProbeMeter& pm = m.probes[slot];
            const bool near = read && pm.valid && std::fabs(peak - pm.peak) <= 0.01f * pm.peak && std::fabs(rms - pm.rms) <= 0.01f * pm.rms
                           && std::fabs(mean - pm.mean) <= 0.01f * pm.rms;
            std::printf("probes: slot %d \"%s\" (meters %.4f %.4f %+.5f)\n", slot, words.c_str(), static_cast<double>(pm.peak),
                        static_cast<double>(pm.rms), static_cast<double>(pm.mean));
            numbers = numbers && near && pm.rms > 0.01f;
        }
        P.eq("probes.numbers_are_the_meters", numbers ? 1 : 0, 1);
        // each trace triggered on a rising crossing of its mean, the same window two frames apart, its range a power of two
        bool triggered = true, steady = true, ranged = true;
        std::vector<std::vector<float>> first;
        for (int slot = 0; slot < 3; ++slot)
            first.push_back(v.probeTrace(slot));
        play(6);
        port.meters(m);
        for (int slot = 0; slot < 3; ++slot)
        {
            const std::vector<float> w = v.probeTrace(slot);
            const float mean = m.probes[slot].mean, range = v.probeRange(slot);
            triggered = triggered && w.size() == static_cast<std::size_t>(SV::kTraceSamples) && w.front() >= mean
                     && w[1] >= w[0] - 0.01f * range;
            double diff = 0.0;
            for (std::size_t i = 0; i < w.size() && i < first[static_cast<std::size_t>(slot)].size(); ++i)
                diff += std::fabs(static_cast<double>(w[i] - first[static_cast<std::size_t>(slot)][i]));
            diff /= static_cast<double>(std::max<std::size_t>(w.size(), 1));
            steady = steady && first[static_cast<std::size_t>(slot)].size() == w.size() && diff <= 0.05 * static_cast<double>(range);
            const float l2 = std::log2(range);
            ranged = ranged && range >= 0.125f && range <= 16.0f && l2 == std::round(l2);
            std::printf("probes: slot %d trace from %.4f, range %.3f V, frames apart %.5f V\n", slot, w.empty() ? 0.0 : static_cast<double>(w.front()),
                        static_cast<double>(range), diff);
        }
        P.eq("probes.triggered", triggered ? 1 : 0, 1);
        P.eq("probes.steady", steady ? 1 : 0, 1);
        P.eq("probes.ranged", ranged ? 1 : 0, 1);
        // the card and the lanes over the canvas's bottom, at most 40 % of it; the flags and readouts drawn as live
        {
            const funkgui::Rect cv = v.canvasRect(), card = v.probeCard(), l1 = v.probeLane(0), l2 = v.probeLane(1);
            std::printf("probes: canvas %.0f x %.0f, card %.0f px at %.0f, lanes at %.0f and %.0f\n", static_cast<double>(cv.w),
                        static_cast<double>(cv.h), static_cast<double>(card.h), static_cast<double>(card.y), static_cast<double>(l1.y),
                        static_cast<double>(l2.y));
            P.eq("probes.card", card.h == SV::kCardH && card.bottom() <= cv.bottom() && card.x >= cv.x && card.right() <= cv.right() ? 1 : 0, 1);
            P.eq("probes.lanes", l1.h > 0.0f && l2.h > 0.0f && l2.y > l1.y && l2.bottom() <= card.y ? 1 : 0, 1);
            P.eq("probes.share", cv.bottom() - std::min(l1.y, card.y) <= SV::kProbeShare * cv.h + 12.0f ? 1 : 0, 1);
            const funkgui::PrimList& frame = host.draw();
            int live = 0, tagged = 0;
            for (const funkgui::Prim& q : frame.prims)
                if (q.tag == sch::kProbeTag)
                {
                    ++tagged;
                    live += (static_cast<uint32_t>(q.d2[3] + 0.5f) & funkgui::pflag::live) != 0 ? 1 : 0;
                }
            P.eq("probes.drawn_live", tagged > 0 && live > 0 ? 1 : 0, 1);
            int items = 0;
            for (const funkgui::A11yItem& it : host.accessibility())
                if (it.title == "Probe a" || it.title == "Probe 1, c" || it.title == "Probe 2, b")
                    items += it.value.rfind("PEAK ", 0) == 0 && it.value.find(" V   RMS ") != std::string::npos ? 1 : 0;
            P.eq("probes.a11y_readouts", items, 3);
            writePng(P, host, "gui.schematic.probes.png", "probes");
        }
        // the meters standing still (an audio thread that stopped calling): the readouts stay, and the view settles
        {
            const bool moving = !v.settled();
            host.tick(20);
            P.eq("probes.still_settles", moving && v.probesLive() && v.settled() && !v.probeWords(0).empty() ? 1 : 0, 1);
            play(2);
            P.eq("probes.moving_again", v.probesLive() && !v.settled() ? 1 : 0, 1);
        }
        // ground: Alt-click on a grounded pin (C1's second) says it is 0 V, and pins nothing
        {
            const funkgui::Point g = pinAt(v, "C1", "2");
            host.click(g.x, g.y, alt);
            host.tick(1);
            P.eq("probes.reason_ground", v.notice() == "Probe: ground is 0 V" && spoken("Probe: ground is 0 V")
                                             && v.pinnedProbes() == std::vector<std::string>{ "c", "b" } ? 1 : 0, 1);
        }
        // the wire's menu: Probe this net, Pin probe, then Unpin probe
        {
            const funkgui::Point aw = wireOf("a");
            host.click(aw.x, aw.y, popupMods());
            const std::vector<std::string> labels = menuLabels(host);
            const bool offered = hasLabel(labels, "Probe this net") && hasLabel(labels, "Pin probe");
            choose(host, "Pin probe");
            host.tick(1);
            const bool pinnedA = v.pinnedProbes() == std::vector<std::string>{ "c", "b", "a" };
            host.click(aw.x, aw.y, popupMods());
            const bool unpin = hasLabel(menuLabels(host), "Unpin probe");
            choose(host, "Unpin probe");
            host.tick(1);
            P.eq("probes.menu_pins", offered && pinnedA && unpin && v.pinnedProbes() == std::vector<std::string>{ "c", "b" } ? 1 : 0, 1);
        }
        // the chip (an effect's amp[2]): its menu chooses the element the probes watch
        if constexpr (!kInstrument)
        {
            const std::optional<funkgui::Point> chip = v.centreOfChip();
            if (chip)
                host.click(chip->x, chip->y);
            const bool listed = menuLabels(host) == std::vector<std::string>{ "amp[0]", "amp[1]" };
            choose(host, "amp[1]");
            play(4);
            port.meters(m);
            const bool second = v.probeElement() == "amp[1]" && v.probeSet().instance == 1 && m.probeSet == v.probeSet() && v.probesLive();
            if (chip)
                host.click(chip->x, chip->y);
            choose(host, "amp[0]");
            play(4);
            port.meters(m);
            P.eq("probes.chip", chip && listed && second && v.probeSet().instance == 0 && m.probeSet == v.probeSet() ? 1 : 0, 1);
        }
        // a view_panel window picture sends no set; a second Panel opened and closed leaves the set as it was
        {
            const uint64_t gen = r.processor.engine().probeGeneration();
            const pg::PanelPicture pic = pg::picturePanel(r.processor.ports(), port, 0, 0, true, "");
            play(3);
            port.meters(m);
            P.eq("probes.picture_keeps_set", !pic.png.empty() && r.processor.engine().probeGeneration() == gen && m.probeSet == armed ? 1 : 0, 1);
            {
                pg::ui::Panel second(r.processor.ports(), port);
                funkgui::HeadlessHost h2(second, 0, 2.0f);
                h2.tick(3);
                push(2, false);
                h2.tick(3);
            }
            play(3);
            port.meters(m);
            P.eq("probes.second_panel_leaves_set", m.probeSet == armed && v.probeSet() == armed && v.probesLive() ? 1 : 0, 1);
            // a Panel that owns no probe (a picture's) reads the slots it shares with the user's set: its lanes are
            // live though it has no wire selected (the user's slot 0)
            {
                const uint64_t before = r.processor.engine().probeGeneration();
                pg::ui::Panel third(r.processor.ports(), port);
                third.setOwnsProbes(false);
                funkgui::HeadlessHost h3(third, 0, 2.0f);
                h3.tick(3);
                push(2, false);
                h3.tick(3);
                SV& tv = third.schematic();
                std::printf("probes: a picture's Panel, slots \"%s\" \"%s\" \"%s\", live %d, lane 1 \"%s\"\n", tv.probeNet(0).c_str(),
                            tv.probeNet(1).c_str(), tv.probeNet(2).c_str(), tv.probesLive() ? 1 : 0, tv.probeWords(1).c_str());
                P.eq("probes.unowned_reads_shared", third.schematicShown() && tv.probeNet(0).empty() && tv.probeNet(1) == "c" && tv.probesLive()
                                                        && !tv.probeWords(1).empty() && !tv.probeWords(2).empty() && !tv.probeTrace(1).empty()
                                                        && r.processor.engine().probeGeneration() == before ? 1 : 0, 1);
            }
        }
        // the Netlist view clears the set; the Schematic view arms the pins again (the session keeps them)
        {
            const funkgui::Point net = panel.centreOfViewPill(false);
            host.click(net.x, net.y);
            play(3);
            port.meters(m);
            const bool cleared = m.probeSet == pg::ProbeSet{} && m.probeProgram == 0;
            bool kept = false;
            for (const pg::FileView& f : port.fileViews())
                kept = kept || (f.file == "diodetone.cir" && f.probes == std::vector<std::string>{ "c", "b" });
            host.click(pill.x, pill.y);
            play(4);
            port.meters(m);
            P.eq("probes.cleared_on_hide", cleared ? 1 : 0, 1);
            P.eq("probes.pins_kept", kept ? 1 : 0, 1);
            P.eq("probes.rearmed_on_show", v.probeNet(0).empty() && v.probeNet(1) == "c" && m.probeSet == v.probeSet() && v.probeSet().armed()
                                               && v.probesLive() ? 1 : 0, 1);
        }
        // a RUN: the same names armed in the new program
        {
            const uint64_t was = port.status().program;
            panel.runNow();
            r.processor.settle();
            host.tick(2);
            play(4);
            port.meters(m);
            std::printf("probes: RUN %llu -> %llu, the set's %llu\n", static_cast<unsigned long long>(was),
                        static_cast<unsigned long long>(port.status().program), static_cast<unsigned long long>(m.probeSet.program));
            P.eq("probes.rearmed_after_run", port.status().program != was && m.probeSet.program == port.status().program
                                                 && m.probeProgram == port.status().program && v.probesLive() ? 1 : 0, 1);
        }
        // host blocks of 2048 (net a selected): an effect's rings hold each block's newest 1,024 (the call is longer
        // than the module's ring), never two blocks joined, its scope triggered inside them; a synth's voice parts are
        // whole, so its windows are
        {
            constexpr int kBig = 2048;
            const funkgui::Point aw = wireOf("a");
            host.click(aw.x, aw.y);
            host.tick(1);
            r.processor.prepareToPlay(kRate, kBig);
            juce::AudioBuffer<float> big(2, kBig);
            for (int blk = 0; blk < 4; ++blk)
            {
                juce::MidiBuffer midi;
                if (kInstrument && blk == 0)
                    midi.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(110)), 0);
                for (int i = 0; i < kBig; ++i)
                {
                    const float x = kInstrument ? 0.0f : static_cast<float>(0.5 * std::sin(phase));
                    phase += 2.0 * 3.14159265358979 * 1000.0 / kRate;
                    big.setSample(0, i, x);
                    big.setSample(1, i, x);
                }
                r.processor.processBlock(big, midi);
                host.tick(1);
            }
            port.meters(m);
            std::vector<float> ring(static_cast<std::size_t>(kBig));
            const int valid = port.probe(0, ring.data(), kBig);
            const std::vector<float> w = v.probeTrace(0);
            const float mean = m.probes[0].mean;
            const std::size_t at = ring.size() - std::min(w.size(), ring.size());
            const bool tail = !w.empty() && std::equal(w.begin(), w.end(), ring.begin() + static_cast<std::ptrdiff_t>(at));
            const bool rising = !w.empty() && at > 0 && ring[at - 1] < mean && mean <= w.front();
            std::printf("probes: blocks of %d, net %s, %d valid, a trace of %zu from %.4f (mean %+.4f)\n", kBig, v.probeNet(0).c_str(), valid,
                        w.size(), w.empty() ? 0.0 : static_cast<double>(w.front()), static_cast<double>(mean));
            if constexpr (kInstrument)
                P.eq("probes.large_blocks", v.probesLive() && valid == kBig && w.size() == static_cast<std::size_t>(SV::kTraceSamples) ? 1 : 0, 1);
            else
                P.eq("probes.large_blocks", v.probesLive() && valid == SV::kTraceSamples && w.size() >= static_cast<std::size_t>(SV::kTraceSamples / 2)
                                                && w.size() <= static_cast<std::size_t>(SV::kTraceSamples) && tail && rising ? 1 : 0, 1);
        }
        // the Panel goes: its set with it
        r.close();
        push(2, false);
        port.meters(m);
        P.eq("probes.cleared_with_panel", m.probeSet == pg::ProbeSet{} && m.probeProgram == 0 ? 1 : 0, 1);
    }
    // screamer.cir's t1 (in its tone block, computed as a whole) cannot be read; a netlist no program #includes is not played
    {
        const std::string ts = libraryText("screamer.cir");
        pg::Draft d = draftWith(pg::cir::hostProgram("screamer.cir", ts, pg::product::kIsInstrument).text, { { "screamer.cir", ts } }, 1);
        d.files = pg::withLibraryIncludes(d.files);
        Rig r(d);
        r.run();
        const funkgui::Point pill = r.panel->centreOfViewPill(true);
        r.host->click(pill.x, pill.y);
        r.host->settle();
        SV& v = r.view();
        const std::optional<cir::Pt> t1 = midWire(v.drawing(), "t1");
        const funkgui::Point at = t1 ? v.screenOf(*t1) : funkgui::Point{ -1.0e4f, -1.0e4f };
        r.host->click(at.x, at.y);
        r.host->tick(2);
        const std::string why = "t1: inside a filter computed as a whole at 1x (block 3)";
        std::printf("probes: screamer's t1: \"%s\"\n", v.probeReason(0).c_str());
        P.eq("probes.reason_unreadable", v.probeNet(0) == "t1" && v.probeReason(0) == why && v.stripRight() == "Probe: " + why
                                             && std::find(r.host->log.announcements.begin(), r.host->log.announcements.end(), "Probe: " + why)
                                                    != r.host->log.announcements.end() ? 1 : 0, 1);
    }
    {
        Rig r(draftWith(pg::kExamples[0].source, { { "diodetone.cir", libraryText("diodetone.cir") } }, 1));
        r.run();
        const funkgui::Point pill = r.panel->centreOfViewPill(true);
        r.host->click(pill.x, pill.y);
        r.host->settle();
        SV& v = r.view();
        const std::optional<cir::Pt> a = midWire(v.drawing(), "a");
        const funkgui::Point at = a ? v.screenOf(*a) : funkgui::Point{ -1.0e4f, -1.0e4f };
        r.host->click(at.x, at.y);
        r.host->tick(2);
        P.eq("probes.reason_not_played", v.probeReason(0) == "not played: no program #includes diodetone.cir"
                                             && v.stripRight() == "Not played: no program #includes diodetone.cir"
                                             && std::find(r.host->log.announcements.begin(), r.host->log.announcements.end(),
                                                          "Probe: not played: no program #includes diodetone.cir") != r.host->log.announcements.end()
                                             ? 1 : 0, 1);
    }

    // a program that #includes the netlist and holds none of its module: says so
    {
        Rig r(draftWith(dialsProgram("diodetone.cir", 1), { { "diodetone.cir", libraryText("diodetone.cir") } }, 1));
        r.run();
        const funkgui::Point pill = r.panel->centreOfViewPill(true);
        r.host->click(pill.x, pill.y);
        r.host->settle();
        SV& v = r.view();
        const std::optional<cir::Pt> a = midWire(v.drawing(), "a");
        const funkgui::Point at = a ? v.screenOf(*a) : funkgui::Point{ -1.0e4f, -1.0e4f };
        r.host->click(at.x, at.y);
        r.host->tick(2);
        const std::string want = std::string(pg::product::kIsInstrument ? "Voice" : "Effect") + " has no field of diodetone.cir's module";
        std::printf("probes: no field: \"%s\"\n", v.probeReason(0).c_str());
        P.eq("probes.reason_no_field", v.probeReason(0) == want && v.stripRight() == "Probe: " + want ? 1 : 0, 1);
    }

    // ---- EDIT: the fields, the Label tool, labels and wires, the clipboard, the menus' new items ----------------------
    {
        const std::string kinds = cir::written(kKinds);
        Rig r(draftWith(std::string(pg::kExamples[0].source), { { "g.cir", kinds } }, 1));
        pg::ui::Panel& panel = *r.panel;
        funkgui::HeadlessHost& host = *r.host;
        panel.setSchematic(true);
        host.settle();
        SV& v = r.view();
        funkgui::TextEditor& ed = r.ed();
        const funkgui::Rect canvas = v.canvasRect();
        // the view at 100 %, a world point at the canvas's centre
        const auto look = [&](float x, float y) {
            v.zoomTo(1.0f, { canvas.centreX(), canvas.centreY() });
            const funkgui::Point a = v.view().at(x, y);
            v.panBy(canvas.centreX() - a.x, canvas.centreY() - a.y);
            host.tick(1);
        };
        const auto on = [&](const char* part) {
            const cir::Part* q = partOf(v.drawing(), part);
            if (q != nullptr)
                look(0.5f * static_cast<float>(q->body.x0 + q->body.x1), 0.5f * static_cast<float>(q->body.y0 + q->body.y1));
            return partAt(v, part);
        };
        const auto gridOf = [&](funkgui::Point p) {
            const funkgui::Point w = v.view().world(p);
            return cir::Pt{ static_cast<int>(std::lround(w.x)), static_cast<int>(std::lround(w.y)) };
        };
        // an empty place: left of the drawing's box, at its middle height
        const cir::Box all = v.drawing().bounds();
        const auto emptyPoint = [&] {
            look(static_cast<float>(all.x0 - 14), static_cast<float>((all.y0 + all.y1) / 2));
            return funkgui::Point{ canvas.centreX(), canvas.centreY() };
        };
        const auto rowsTyped = [&](const std::vector<std::pair<std::string, std::string>>& rows) {
            const std::vector<std::string> keys = v.fieldKeys();
            for (std::size_t k = 0; k < keys.size(); ++k)
            {
                for (const auto& [key, text] : rows)
                    if (key == keys[k])
                    {
                        host.setClipboard(text);
                        host.keys("cmd+a");
                        host.keys("cmd+v");
                    }
                if (k + 1 < keys.size())
                    host.keys("tab");
            }
        };
        P.eq("edit.kinds_drawn", static_cast<int64_t>(v.drawing().parts.size()), 16);   // 14 elements (13 kinds), 2 terminals

        // each kind's fields: the exact tokens, one undo step
        struct Case
        {
            const char* part;
            std::vector<std::pair<std::string, std::string>> rows;
            std::vector<cir::Op> ops;
            std::vector<std::string> lines;
        };
        const std::vector<Case> cases = {
            { "R1", { { "name", "R7" }, { "value", "2.2k" } }, { setOp("R1", "", "2.2k"), renameOp("R1", "R7") }, { "R7 in a 2.2k" } },
            { "C1", { { "value", "22n" } }, { setOp("C1", "", "22n") }, { "C1 a 0 22n" } },
            { "L1", { { "value", "47m" } }, { setOp("L1", "", "47m") }, { "L1 a b 47m" } },
            { "D1", { { "model", "1N34A" } }, { setOp("D1", "model", "1N34A") }, { "D1 b 0 1N34A" } },
            { "P1", { { "name", "P9" }, { "value", "250k" }, { "pot", "drive" }, { "taper", "audio" } },
              { setOp("P1", "", "250k"), setOp("P1", "pot", "drive"), setOp("P1", "taper", "audio"), renameOp("P1", "P9") },
              { "P9 b w out 250k {drive}", ".pot drive 0.5 audio", "G2 0 g OTA c 0 {100u * drive}", "P2 c x 50k {1-drive} taper=audio" } },
            // a rheostat whose {expr} is no pot alone: its own taper= is the one written (the .pot's word is P1's)
            { "P2", { { "value", "47k" }, { "taper", "rlog" } }, { setOp("P2", "", "47k"), setOp("P2", "taper", "rlog") },
              { "P2 c x 47k {1-tone} taper=rlog", ".pot tone 0.5 lin" } },
            { "XU1", { { "name", "XU2" } }, { renameOp("XU1", "XU2") }, { "XU2 c w d opamp" } },
            { "E1", { { "gain", "20" } }, { setOp("E1", "gain", "20") }, { "E1 e 0 c 0 20" } },
            { "G1", { { "gain", "2m" } }, { setOp("G1", "gain", "2m") }, { "G1 f 0 c 0 2m" } },
            { "G2", { { "gain", "{200u * tone}" } }, { setOp("G2", "gain", "{200u * tone}") }, { "G2 0 g OTA c 0 {200u * tone}" } },
            { "V1", { { "value", "12" } }, { setOp("V1", "", "12") }, { "V1 vb 0 DC 12" } },
            { "I1", { { "value", "2m" } }, { setOp("I1", "", "2m") }, { "I1 h 0 DC 2m" } },
            { "S1", { { "switch", "sel" }, { "positions", "0,1" } }, { setOp("S1", "switch", "sel"), setOp("S1", "positions", "0,1") },
              { "S1 h 0 sel 0,1", ".switch sel 2 0" } },
            { "u", { { "scale", "2" } }, { setOp("u", "scale", "2") }, { ".input  u in scale=2" } },
            { "vout", { { "name", "y" } }, { renameOp("vout", "y") }, { ".output y out" } },
        };
        int exact = 0, oneStep = 0;
        for (const Case& c : cases)
        {
            const std::string before = ed.text();
            const funkgui::Point at = on(c.part);
            host.doubleClick(at.x, at.y);
            host.tick(1);
            const bool opened = v.editing();
            rowsTyped(c.rows);
            host.keys("return");
            host.tick(1);
            const std::string after = ed.text();
            bool lines = true;
            for (const std::string& l : c.lines)
                lines = lines && after.find(l + "\n") != std::string::npos;
            const bool same = opened && !v.editing() && after == applied(before, c.ops).text && lines;
            exact += same ? 1 : 0;
            ed.undo();
            host.tick(1);
            oneStep += ed.text() == before ? 1 : 0;
            if (!same)
                std::printf("fields: %s did not edit as expected (opened %d)\n", c.part, opened ? 1 : 0);
        }
        P.eq("edit.fields_exact", exact, static_cast<int64_t>(cases.size()));
        P.eq("edit.fields_one_step", oneStep, static_cast<int64_t>(cases.size()));
        // the rows each kind has
        {
            const funkgui::Point at = on("P1");
            host.doubleClick(at.x, at.y);
            host.tick(1);
            P.eq("edit.pot_rows", v.fieldKeys() == std::vector<std::string>{ "name", "value", "pot", "taper" } ? 1 : 0, 1);
            const std::string p1Taper = v.fieldText("taper");
            host.keys("escape");
            host.tick(1);
            // the taper FMinus plays: P1's .pot word ({tone} alone), P2's own taper= ({1-tone} is no pot alone)
            const funkgui::Point p2 = on("P2");
            host.doubleClick(p2.x, p2.y);
            host.tick(1);
            P.eq("edit.taper_shown", p1Taper == "lin" && v.fieldKeys() == std::vector<std::string>{ "name", "value", "pot", "taper" }
                                         && v.fieldText("taper") == "audio" ? 1 : 0, 1);
            host.keys("escape");
            host.tick(1);
            const funkgui::Point x = on("XU1");
            host.doubleClick(x.x, x.y);
            host.tick(1);
            P.eq("edit.opamp_rows", v.fieldKeys() == std::vector<std::string>{ "name" } ? 1 : 0, 1);
            host.keys("escape");
            host.tick(1);
            P.eq("edit.escape_closes", !v.editing() && ed.text() == kinds ? 1 : 0, 1);
        }
        // a row longer than 64 characters (an OTA's long expression) left alone: the name alone is written
        {
            const std::string longGain = "{100u * tone + 0 * tone + 0 * tone + 0 * tone + 0 * tone + 0 * tone + 0 * tone}";
            std::string t = kinds;
            t.replace(t.find("{100u * tone}"), 13, longGain);
            ed.setText(t);
            host.tick(1);
            const funkgui::Point at = on("G2");
            host.doubleClick(at.x, at.y);
            host.tick(1);
            const bool whole = v.fieldText("gain") == longGain;
            rowsTyped({ { "name", "G3" } });
            host.keys("return");
            host.tick(1);
            P.eq("edit.long_row_kept", whole && longGain.size() > 64 && !v.editing() && ed.text() == applied(t, { renameOp("G2", "G3") }).text
                                           && has(ed.text(), "\nG3 0 g OTA c 0 " + longGain + "\n") ? 1 : 0, 1);
            ed.setText(kinds);
            host.tick(1);
        }
        // Return with one part selected, and a click on its value label, open it (the caret on the value)
        {
            const funkgui::Point at = on("C1");
            host.click(at.x, at.y);
            host.keys("return");
            host.tick(1);
            P.eq("edit.return_opens", v.editing() && v.fieldRow() == "name" ? 1 : 0, 1);
            host.keys("escape");
            host.draw();
            const std::optional<funkgui::Point> value = v.centreOfValue("C1");
            if (value)
                host.click(value->x, value->y);
            host.tick(1);
            P.eq("edit.value_click_opens", value && v.editing() && v.fieldRow() == "value" && v.fieldText("value") == "10n" ? 1 : 0, 1);
            host.keys("escape");
            host.tick(1);
            // a double-click on it opens it too (its second press leaves the field the first opened)
            host.draw();
            if (const std::optional<funkgui::Point> again = v.centreOfValue("C1"))
                host.doubleClick(again->x, again->y);
            host.tick(1);
            P.eq("edit.value_double_click_opens", v.editing() && v.fieldRow() == "value" && v.fieldText("value") == "10n" ? 1 : 0, 1);
            host.keys("escape");
            host.tick(1);
        }
        // a field open when the view goes (the Netlist view) is written, as a press outside it writes it
        {
            host.draw();
            const std::optional<funkgui::Point> value = v.centreOfValue("C1");
            if (value)
                host.click(value->x, value->y);
            host.tick(1);
            host.keys("cmd+a");
            for (const char ch : std::string("22n"))
                host.keys(std::string(1, ch).c_str());
            host.keys("cmd+shift+d");
            host.tick(1);
            const bool written = has(ed.text(), "\nC1 ") && has(ed.text(), " 22n\n") && !v.editing();
            host.keys("cmd+z");
            host.tick(1);
            P.eq("edit.view_change_writes_field", value && written && ed.text() == kinds ? 1 : 0, 1);
            host.keys("cmd+shift+d");
            host.tick(1);
        }
        // Up and Down: E12, keeping the suffix's style; Shift a decade
        {
            const funkgui::Point at = on("R1");
            host.doubleClick(at.x, at.y);
            host.keys("tab");
            host.setClipboard("820");
            host.keys("cmd+a");
            host.keys("cmd+v");
            host.keys("up");
            const std::string up = v.fieldText("value");
            host.keys("shift+up");
            const std::string decade = v.fieldText("value");
            host.keys("down");
            const std::string down = v.fieldText("value");
            host.keys("shift+down");
            const std::string back = v.fieldText("value");
            std::printf("E12: 820 up %s, a decade %s, down %s, a decade down %s\n", up.c_str(), decade.c_str(), down.c_str(), back.c_str());
            P.eq("edit.e12_up", up == "1k" ? 1 : 0, 1);
            P.eq("edit.e12_decade", decade == "10k" && down == "8.2k" && back == "820" ? 1 : 0, 1);
            host.keys("escape");
            host.tick(1);
            P.eq("edit.e12_wrote_nothing", ed.text() == kinds ? 1 : 0, 1);
        }
        // a name or a value FMinus cannot take: refused in the field, which stays open
        {
            const funkgui::Point at = on("R1");
            host.doubleClick(at.x, at.y);
            host.setClipboard("Q5");
            host.keys("cmd+a");
            host.keys("cmd+v");
            host.keys("return");
            host.tick(1);
            const std::string letter = v.fieldWhy();
            host.setClipboard("R2");
            host.keys("cmd+a");
            host.keys("cmd+v");
            host.keys("return");
            host.tick(1);
            const std::string taken = v.fieldWhy();
            host.setClipboard("R1");
            host.keys("cmd+a");
            host.keys("cmd+v");
            host.keys("tab");
            host.setClipboard("abc");
            host.keys("cmd+a");
            host.keys("cmd+v");
            host.keys("return");
            host.tick(1);
            const std::string value = v.fieldWhy();
            std::printf("refused: %s | %s | %s\n", letter.c_str(), taken.c_str(), value.c_str());
            P.eq("edit.refused_letter", v.editing() && has(letter, "letter") ? 1 : 0, 1);
            P.eq("edit.refused_taken", has(taken, "R2 exists") ? 1 : 0, 1);
            P.eq("edit.refused_value", v.editing() && has(value, "not a SPICE number") && ed.text() == kinds ? 1 : 0, 1);
            host.keys("escape");
            host.tick(1);
        }
        // 1M on an R: FMinus's warning under the field; Return keeps 1M, Alt-Return writes 1meg (edit.png)
        {
            const funkgui::Point at = on("R1");
            host.doubleClick(at.x, at.y);
            host.keys("tab");
            host.keys("cmd+a");
            typed(host, "1M");
            host.tick(1);
            const std::string warning = v.fieldWarning();
            std::printf("1M: %s\n", warning.c_str());
            P.eq("edit.milli_warning", warning == "'1M' is 1 milliohm in SPICE; write 1meg for a megohm" ? 1 : 0, 1);
            writePng(P, host, "gui.schematic.edit.png", "edit");
            host.keys("return");
            host.tick(1);
            P.eq("edit.milli_return_keeps", has(ed.text(), "\nR1 in a 1M\n") ? 1 : 0, 1);
            ed.undo();
            host.tick(1);
            const funkgui::Point again = on("R1");
            host.doubleClick(again.x, again.y);
            host.keys("tab");
            host.keys("cmd+a");
            typed(host, "1M");
            host.keys("alt+return");
            host.tick(1);
            P.eq("edit.milli_alt_meg", has(ed.text(), "\nR1 in a 1meg\n") ? 1 : 0, 1);
            ed.undo();
            host.tick(1);
            P.eq("edit.milli_undone", ed.text() == kinds ? 1 : 0, 1);
        }
        // the Label tool: a net renamed in every element and directive line; a name another net has merges them
        {
            const auto labelled = [&](const char* part, const char* pin, const std::string& to) {
                const funkgui::Point a = on(part);
                (void)a;
                const funkgui::Point p = pinAt(v, part, pin);
                host.click(p.x, p.y);
                host.tick(1);
                const bool opened = v.editing() && v.fieldKeys() == std::vector<std::string>{ "net" };
                host.setClipboard(to);
                host.keys("cmd+a");
                host.keys("cmd+v");
                host.keys("return");
                host.tick(1);
                return opened;
            };
            const funkgui::Point a = on("R1");
            host.move(a.x, a.y);
            host.keys("n");
            P.eq("edit.label_tool", v.tool() == SV::Tool::label ? 1 : 0, 1);
            const bool opened = labelled("R1", "1", "src");
            const std::string renamed = ed.text();
            P.eq("edit.label_renames", opened && renamed == applied(kinds, { renameOp("in", "src") }).text
                                           && has(renamed, ".input  u src\n") && has(renamed, "\nR1 src a 1k\n") ? 1 : 0, 1);
            ed.undo();
            host.tick(1);
            labelled("L1", "2", "a");
            const std::string merged = ed.text();
            std::printf("label merge: %s\n", v.notice().c_str());
            P.eq("edit.label_merges", merged == applied(kinds, { renameOp("b", "a") }).text && has(merged, "\nL1 a a 10m\n")
                                          && has(v.notice(), "the nets merged") ? 1 : 0, 1);
            ed.undo();
            host.tick(1);
            // on a wire: its name tag too (label= on the net's line)
            if (const std::optional<cir::Pt> mid = midWire(v.drawing(), "a"))
            {
                look(static_cast<float>(mid->x), static_cast<float>(mid->y));
                const funkgui::Point w = v.screenOf(*mid);
                host.click(w.x, w.y);
                host.tick(1);
                host.setClipboard("mid");
                host.keys("cmd+a");
                host.keys("cmd+v");
                host.keys("return");
                host.tick(1);
                cir::Op tag;
                tag.type = cir::Op::Type::label;
                tag.name = "a";
                tag.points = { *mid };
                const std::string tagged = ed.text();
                P.eq("edit.label_tag", tagged == applied(kinds, { tag, renameOp("a", "mid") }).text
                                           && has(tagged, "*@ net mid label=" + std::to_string(mid->x) + "," + std::to_string(mid->y)) ? 1 : 0, 1);
            }
            else
                P.eq("edit.label_tag", 0, 1);
            host.keys("escape");
            host.tick(1);
            P.eq("edit.label_tool_ends", v.tool() == SV::Tool::select ? 1 : 0, 1);
        }
        // Show as labels writes flag; Show as wires (a pin's menu: a tag has no wire) takes it out (labels.png)
        {
            const std::string before = ed.text();
            if (const std::optional<cir::Pt> mid = midWire(v.drawing(), "mid"))
            {
                look(static_cast<float>(mid->x), static_cast<float>(mid->y));
                const funkgui::Point w = v.screenOf(*mid);
                host.click(w.x, w.y, popupMods());
                const cir::Drawing wired = cir::reconcile(ed.text());
                P.eq("edit.show_as_labels", choose(host, "Show as labels") && has(ed.text(), "\n*@ net mid flag") ? 1 : 0, 1);
                // its tags touch nothing they did not (two of one net may: one name), so nothing is outlined
                const cir::Drawing tagged = cir::reconcile(ed.text());
                int fresh = 0;
                for (const auto& o : tagged.overlaps)
                    fresh += std::find(wired.overlaps.begin(), wired.overlaps.end(), o) == wired.overlaps.end() ? 1 : 0;
                P.eq("edit.labels_no_overlap", fresh, 0);
            }
            else
                P.eq("edit.show_as_labels", 0, 1);
            host.settle();
            writePng(P, host, "gui.schematic.labels.png", "labels");
            const funkgui::Point a = on("R2");
            (void)a;
            const funkgui::Point pin = pinAt(v, "R2", "1");
            host.click(pin.x, pin.y, popupMods());
            P.eq("edit.show_as_wires", choose(host, "Show mid as wires") && !has(ed.text(), "*@ net mid flag")
                                           && has(ed.text(), "\n*@ net mid ") ? 1 : 0, 1);
            ed.undo();
            ed.undo();
            host.tick(1);
            P.eq("edit.labels_undone", ed.text() == before ? 1 : 0, 1);
            ed.undo();
            host.tick(1);
            P.eq("edit.tag_undone", ed.text() == kinds ? 1 : 0, 1);
        }
        // copy, then paste: the exact text, parts renamed, fresh nodes (ground kept), places offset to the pointer
        {
            const funkgui::Point r1 = on("R1");
            host.click(r1.x, r1.y);
            const funkgui::Point c1 = partAt(v, "C1");
            host.click(c1.x, c1.y, funkgui::Mods{ true, false, false, false });
            host.keys("cmd+c");
            const std::string clip = host.clipboardText();
            const std::string want = cir::copied(kinds, cir::reconcile(kinds), { "R1", "C1" });
            std::printf("copied:\n%s", clip.c_str());
            P.eq("edit.copy_text", !clip.empty() && clip == want && has(clip, "R1 in a 1k\n") && has(clip, "C1 a 0 10n\n")
                                       && has(clip, "*@ part R1 ") && has(clip, "*@ part C1 ") && !has(clip, "R2 ") ? 1 : 0, 1);
            const funkgui::Point empty = emptyPoint();
            host.move(empty.x, empty.y);
            const cir::Pt to = gridOf(empty);
            host.keys("cmd+v");
            host.tick(1);
            cir::Op paste;
            paste.type = cir::Op::Type::paste;
            paste.at = to;
            for (std::size_t at = 0; at < clip.size();)
            {
                const std::size_t end = std::min(clip.find('\n', at), clip.size());
                paste.lines.push_back(clip.substr(at, end - at));
                at = end + 1;
            }
            paste.lines.push_back("");
            const std::string pasted = ed.text();
            P.eq("edit.paste_exact", pasted == applied(kinds, { paste }).text ? 1 : 0, 1);
            // the pasted parts (selected): new names, fresh nodes but ground, at the pointer
            const std::vector<std::string> sel = v.selection();
            const cir::Netlist nl = cir::read(pasted);
            const cir::Drawing d = cir::reconcile(pasted);
            bool fresh = sel.size() == 2, grounded = false, placed = true;
            cir::Box box;
            for (const std::string& n : sel)
                if (const cir::Part* q = partOf(d, n.c_str()))
                    box = box.joined(q->body);
            for (const std::string& n : sel)
            {
                const int e = nl.element(n);
                fresh = fresh && e >= 0 && n != "R1" && n != "C1";
                if (e < 0)
                    continue;
                for (const cir::Token& t : nl.elements[static_cast<std::size_t>(e)].nodes)
                {
                    fresh = fresh && t.text != "in" && t.text != "a";
                    grounded = grounded || t.text == "0";
                }
            }
            placed = !box.empty() && box.x0 == to.x && box.y0 == to.y;
            std::printf("pasted %zu: %s %s; their box %d,%d\n", sel.size(), sel.empty() ? "" : sel[0].c_str(),
                        sel.size() > 1 ? sel[1].c_str() : "", box.x0, box.y0);
            P.eq("edit.paste_fresh", fresh && grounded ? 1 : 0, 1);
            P.eq("edit.paste_at_pointer", placed ? 1 : 0, 1);
            // cut: copied and gone; pasted back
            const std::string cutName = sel.empty() ? std::string() : sel.front();
            v.selectPart(cutName, true);
            host.tick(1);
            host.keys("cmd+x");
            host.tick(1);
            const bool gone = cir::read(ed.text()).element(cutName) < 0 && has(host.clipboardText(), cutName + " ");
            host.move(empty.x, empty.y);
            host.keys("cmd+v");
            host.tick(1);
            P.eq("edit.cut_paste", gone && cir::read(ed.text()).element(cutName) >= 0 ? 1 : 0, 1);
            // duplicate: a copy where it meets nothing, just below L1 (else beside it): no overlap, L1's turn
            const std::string dupBefore = ed.text();
            const funkgui::Point l1 = on("L1");
            host.click(l1.x, l1.y);
            host.keys("cmd+d");
            host.tick(1);
            cir::Pt origin;
            const cir::Drawing beforeDup = cir::reconcile(dupBefore);
            const std::string lone = cir::copied(dupBefore, beforeDup, { "L1" }, &origin);
            const cir::Drawing afterDup = cir::reconcile(ed.text());
            const cir::Part* l1Was = partOf(beforeDup, "L1");
            const cir::Part* l2 = partOf(afterDup, "L2");
            const cir::Pt off = l2 != nullptr && l1Was != nullptr ? l2->origin - l1Was->origin : cir::Pt{};
            int dupOverlaps = 0;
            for (const auto& o : afterDup.overlaps)
                dupOverlaps += std::find(beforeDup.overlaps.begin(), beforeDup.overlaps.end(), o) == beforeDup.overlaps.end() ? 1 : 0;
            std::printf("duplicate: L2 at %d,%d off L1, %d new overlaps\n", off.x, off.y, dupOverlaps);
            P.eq("edit.duplicate_clear", l2 != nullptr && l1Was != nullptr && (off.x != 0 || off.y != 0) && l2->orient == l1Was->orient
                                             && !l2->body.meets(l1Was->body) && dupOverlaps == 0 ? 1 : 0, 1);
            cir::Op dup;
            dup.type = cir::Op::Type::paste;
            dup.at = origin + off;
            for (std::size_t at = 0; at < lone.size();)
            {
                const std::size_t end = std::min(lone.find('\n', at), lone.size());
                dup.lines.push_back(lone.substr(at, end - at));
                at = end + 1;
            }
            dup.lines.push_back("");
            P.eq("edit.duplicate", ed.text() == applied(dupBefore, { dup }).text && cir::read(ed.text()).element("L2") >= 0 ? 1 : 0, 1);
            // what is no netlist is refused
            const std::string refusedBefore = ed.text();
            host.setClipboard("hello world\nthis is not a netlist\n");
            host.move(empty.x, empty.y);
            host.keys("cmd+v");
            host.tick(1);
            std::printf("paste refused: %s\n", v.notice().c_str());
            P.eq("edit.paste_refused", ed.text() == refusedBefore && has(v.notice(), "Paste takes netlist lines") ? 1 : 0, 1);
            // prose of a part's first letter too: an inductor 'Lorem' of 'sit', a resistor 'Remember' of 'milk'
            int prose = 0;
            for (const char* t : { "Lorem ipsum dolor sit amet", "Remember to buy milk" })
            {
                host.setClipboard(t);
                host.keys("cmd+v");
                host.tick(1);
                prose += ed.text() == refusedBefore && has(v.notice(), "Paste takes netlist lines") ? 1 : 0;
            }
            std::printf("prose refused: %s\n", v.notice().c_str());
            P.eq("edit.paste_prose_refused", prose, 2);
            ed.setText(kinds);
            host.tick(1);
        }
        // the menus' new items
        {
            const auto partMenu = [&](const char* part, const std::string& item) {
                const funkgui::Point c = on(part);
                host.click(c.x, c.y, popupMods());
                const bool ok = choose(host, item);
                host.tick(1);
                return ok;
            };
            P.eq("menus.part_edit", partMenu("C1", "Edit") && v.editing() && v.fieldText("value") == "10n" ? 1 : 0, 1);
            host.keys("escape");
            host.setClipboard("");
            P.eq("menus.part_copy", partMenu("C1", "Copy") && host.clipboardText() == cir::copied(kinds, cir::reconcile(kinds), { "C1" }) ? 1 : 0, 1);
            P.eq("menus.part_duplicate", partMenu("C1", "Duplicate") && cir::read(ed.text()).element("C2") >= 0 ? 1 : 0, 1);
            ed.undo();
            host.tick(1);
            P.eq("menus.part_cut", partMenu("C1", "Cut") && cir::read(ed.text()).element("C1") < 0 ? 1 : 0, 1);
            ed.undo();
            host.tick(1);
            if (const std::optional<cir::Pt> mid = midWire(v.drawing(), "a"))
            {
                look(static_cast<float>(mid->x), static_cast<float>(mid->y));
                const funkgui::Point w = v.screenOf(*mid);
                host.click(w.x, w.y, popupMods());
                const std::vector<std::string> labels = menuLabels(host);
                P.eq("menus.wire_items", hasLabel(labels, "Rename net") && hasLabel(labels, "Show as labels") && hasLabel(labels, "Copy")
                                             && hasLabel(labels, "Cut") ? 1 : 0, 1);
                P.eq("menus.wire_rename", choose(host, "Rename net") && v.editing() && v.fieldText("net") == "a" ? 1 : 0, 1);
                host.keys("escape");
                // with parts selected, the wire's Copy and Cut take them
                host.keys("cmd+a");
                host.click(w.x, w.y, popupMods());
                host.setClipboard("");
                P.eq("menus.wire_copy", choose(host, "Copy") && has(host.clipboardText(), "R1 in a 1k") ? 1 : 0, 1);
                host.click(w.x, w.y, popupMods());
                P.eq("menus.wire_cut", choose(host, "Cut") && cir::read(ed.text()).elements.empty() ? 1 : 0, 1);
                ed.undo();
                host.tick(1);
            }
            else
                P.eq("menus.wire_items", 0, 1);
            P.eq("menus.edit_restored", ed.text() == kinds ? 1 : 0, 1);
            host.setClipboard(cir::copied(kinds, cir::reconcile(kinds), { "D1" }));
            const funkgui::Point empty = emptyPoint();
            host.click(empty.x, empty.y, popupMods());
            P.eq("menus.canvas_paste", choose(host, "Paste") && cir::read(ed.text()).element("D2") >= 0 ? 1 : 0, 1);
        }
    }

    // ---- MARKS, the structural ones (and errors.png): kMarks, with an error, a warning and a note planted as above ------
    {
        Rig r(draftWith(dialsProgram("g.cir", 2), { { "g.cir", kMarks } }, 1));
        pg::ui::Panel& panel = *r.panel;
        funkgui::HeadlessHost& host = *r.host;
        pg::ui::CodeAssist& code = panel.codeAssist();
        r.run();
        waitFor(host, [&] { return !code.checking() && code.settled(); });
        host.tick(60);
        waitFor(host, [&] { return !code.checking() && code.settled(); });
        panel.setSchematic(true);
        host.settle();
        SV& v = r.view();
        funkgui::TextEditor& ed = r.ed();
        frame(v, host);
        std::vector<pg::ui::Diagnostic> msgs(3);
        msgs[0] = { "g.cir", 4, 1, pg::ui::Diagnostic::Severity::error, "planted: an error on R1" };
        msgs[1] = { "g.cir", 6, 1, pg::ui::Diagnostic::Severity::warning, "planted: a warning on R2" };
        msgs[2] = { "g.cir", 5, 1, pg::ui::Diagnostic::Severity::info, "planted: a note on C1" };
        code.setCheckDiagnostics(msgs, { { "g.cir", ed.text() } });
        host.tick(1);
        P.eq("marks.overlap_noted", v.drawing().overlaps.empty() ? 0 : 1, 1);
        host.draw();
        const funkgui::Theme th = pg::ui::productTheme(funkgui::Theme::byIndex(0));
        const uint32_t err = packed(ed.options().inks.error.resolve(th));
        const funkgui::PrimList& f = host.draw();
        const funkgui::Point lone = pinAt(v, "R3", "1"), ground = pinAt(v, "C1", "2"), touch = pinAt(v, "R4", "2");
        const funkgui::Point r4 = partAt(v, "R4"), r5 = partAt(v, "R5");
        const auto centredOn = [](const funkgui::Prim& p, funkgui::Point q) {
            return std::fabs(0.5f * (p.x0 + p.x1) - q.x) < 1.0f && std::fabs(0.5f * (p.y0 + p.y1) - q.y) < 1.0f;
        };
        const auto around = [](const funkgui::Prim& p, funkgui::Point q) { return p.x0 < q.x && q.x < p.x1 && p.y0 < q.y && q.y < p.y1; };
        int circles = 0, onGround = 0, dots = 0, outlinedR4 = 0, outlinedR5 = 0;
        for (const funkgui::Prim& p : f.prims)
        {
            if (p.tag != sch::kBadgeTag)
                continue;
            const bool hollow = (p.c0 >> 24) == 0;
            circles += centredOn(p, lone) && hollow && p.c1 == err ? 1 : 0;
            onGround += centredOn(p, ground) ? 1 : 0;
            dots += centredOn(p, touch) && !hollow && p.c0 == err ? 1 : 0;
            const bool outline = hollow && p.c1 == err && std::max(p.x1 - p.x0, p.y1 - p.y0) > 30.0f;   // a body's, not a pin's
            outlinedR4 += outline && around(p, r4) ? 1 : 0;
            outlinedR5 += outline && around(p, r5) ? 1 : 0;
        }
        std::printf("structural marks: circles %d, on the ground pin %d, dots %d, outlines %d %d\n", circles, onGround, dots,
                    outlinedR4, outlinedR5);
        P.eq("marks.unconnected_circle", circles, 1);
        P.eq("marks.lone_ground_unmarked", onGround, 0);
        P.eq("marks.touch_dot", dots, 1);
        P.eq("marks.overlap_outlined", outlinedR4 == 1 && outlinedR5 == 1 ? 1 : 0, 1);
        writePng(P, host, "gui.schematic.errors.png", "errors");
    }

    return P.finish();
}
