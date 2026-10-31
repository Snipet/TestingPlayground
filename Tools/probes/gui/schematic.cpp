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
//   gui.schematic.errors.png   an error, a warning and a note on parts (badges, an outline, a ring), unconnected pins
//   gui.schematic.empty.png    an empty netlist: the hint and Start a circuit
//   gui.schematic.zoom25.png   25 %: symbols as their boxes, no labels, no grid
//   gui.schematic.zoom400.png  400 %
// and checks:
//   VIEW      the pills (Netlist, Schematic) at the tab bar's right end while a .cir tab shows, labelled at the
//             default size; a click shows the drawing: opening fits it, never under 60 %; the status line's words; the
//             parts and wires drawn inside the canvas only; nothing missing from the font; Cmd-Shift-D
//   GESTURES  each gives the exact text as one undo step: placing every kind (by click), a wire from pin to pin, a
//             wire into a mid-wire T, a wire dragged from a wire point, the Ground tool, a move, a refused overlap (no
//             change), rotate, mirror, delete (its stubs pruned), a deleted wire splitting a net, Start a circuit
//   UNDO      Cmd-Z and Cmd-Shift-Z in either view undo and redo the other's steps; so do the toolbar's Undo and Redo
//   OVERTAKEN a drag whose base a merge (Claude's write) moves is cancelled and changes nothing; that write moving a
//             part's line flashes the part
//   POINTER   a row per cell of the pointer's table: idle on a body, a pin, a wire, nothing; Alt; carrying a part (a
//             popup click turns the ghost, a popup drag pans and carries on, placing ends only on Escape, Select or
//             another tool); the Ground tool; a badge
//   KEYS      edge auto-pan's units in a fixed number of ticks; Escape's order and leaving the canvas; Tab in line
//             order; Cmd-Shift-D in either view, and nothing while the IR tab shows; the parts' keys away from the
//             canvas place at once
//   ROOM      the canvas at least 240 px tall at 1000 x 600 with 32 dials (folded) and 9 (shown), at 1360 x 760 with
//             the explorer open and 32 shown; the fold's bar shows and hides them; a wire selected and not: the canvas
//             the same
//   MARKS     the badges', the outline's and the ring's colours are the tab editor's error, warning and note inks (both
//             themes); a layout-only gesture leaves the status RUNNING, asks for no check and keeps a planted error's
//             badge and squiggle; a problems row's click (a netlist shown in its Schematic view) selects and centres
//             the part; the info strip's count steps through them
//   MENUS     the part's, the wire's, the canvas's and More's menus (pendingMenu), each item working (chooseMenuItem)
//   A11Y      the group, an item a part and a net, their ids the same across a pan and a zoom, the revision too, every
//             item visible; the focus action selects and centres
//   PERSIST   nothing kept by opening; a Panel opened again keeps each file's view, zoom and pan (the session's views);
//             a view switched but never zoomed nor panned keeps no zoom, and fits again when opened again
#include "Probe.h"

#include "editor/CodeAssist.h"
#include "editor/Panel.h"
#include "editor/SchematicDraw.h"
#include "editor/SchematicView.h"
#include "editor/Theme.h"
#include "fminus/CirDoc.h"
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
            oneStep("gestures.wire_pin_to_pin", loose, applied(loose, { op }).text);
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
            P.eq("pointer.alt_click_wire_changes_nothing", ed.text() == text ? 1 : 0, 1);
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
            std::vector<std::string> order, want;
            for (const cir::Part& p : v.drawing().parts)
                want.push_back(p.name);
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
        P.eq("room.bar_shows", !panel.dialsFolded() && panel.dialsBarShown() && panel.centreOfDial(31) ? 1 : 0, 1);
        P.ge("room.shown_editor_keeps", panel.editorArea().h, 192.0);
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
        writePng(P, host, "gui.schematic.errors.png", "errors");
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

    return P.finish();
}
