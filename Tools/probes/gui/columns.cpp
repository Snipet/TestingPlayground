// PROBE name=gui.columns timeout=180
//
// gui.columns: the explorer's and the chat's widths (editor/Columns.h), headless over a real Processor, to
// <build>/probe-out/:
//   gui.columns.png            the default size, both edges dragged: the explorer wider, the chat wider
//   gui.columns.hover.png      the chat's edge under the pointer, its hairline lit (and .hover.paper.png in PAPER)
//   gui.columns.paper.png      in PAPER, the explorer's edge focused from the keyboard (its ring)
//   gui.columns.wide.png       1800 x 1000 with wider widths set (and .wide.paper.png in PAPER)
//   gui.columns.narrow.png     1300 x 760: the explorer given way to its least, the chat after it
//   gui.columns.small.png      the least size: the explorer folded away, the chat as wide as the room
//   gui.columns.panel.png      the PANEL view: no explorer, the chat's width kept
//   gui.columns.schematic.png  a netlist's Schematic view between the two
// and checks:
//   FIT        Columns::fit's arithmetic: with no width set, the shares as before (a third, a sixth, their ranges); a
//              width set kept within its range; at every window width from the least up, with any widths asked, the
//              program's card keeps kProgramMinW and neither card is under its least; the explorer gives way first,
//              then the chat, then folds; the rail; each edge's reach
//   DEFAULT    a new instance, nothing set: the shares; both edges shown, the left-right cursor over each; each a slider
//              in px to a screen reader
//   DRAG       the explorer's edge dragged right widens it by the drag (the program's card narrower by as much, the chat
//              where it was), the chat's dragged left widens the chat; nothing kept until the release, then the session
//              and this Mac's default have the width; the caret stays in the editor
//   CLAMP      a drag past an edge's most stops there (the explorer's at the chat's width, the program's card keeping
//              kProgramMinW; the chat's at the explorer's least), past its least at the least
//   RESET      a double-click on an edge: the share again, the session's and this Mac's width 0
//   KEYS       Shift-Tab to the chat's edge: Left widens it by 8, Shift-Left by 32, Right narrows it, Home its least, End
//              its most, Delete resets it; the explorer's edge: Right widens it; the session has each step at once, this
//              Mac's default once the steps pause, in one write
//   A11Y       a screen reader's increment, decrement and value
//   KEPT       the state carries the widths to another processor, whose Panel shows them over this Mac's default; a new
//              instance with none takes this Mac's default, as the host's reload left it when it attached the Panel; a
//              second Panel on the processor follows the first's; a fresh start (EXAMPLES) keeps them
//   GIVE       narrowing the window: the explorer gives way first, to its least, then the chat, then the explorer folds;
//              a key or a screen reader's step that moves nothing there keeps the widths asked; an edge that hides gives
//              up the focus and sets nothing; widened again, both come back to their widths
//   OVER       the code editor's completion list over the chat's edge is the list's: its cursor, a press choosing a row
//              and moving no edge
//   VIEWS      the PANEL view: the explorer and its edge away, the chat's width kept; the chat folded: no chat edge (one
//              focused gives up the focus), the explorer's reach the room the rail leaves; a netlist's Schematic view
//              between the two
#include "Probe.h"

#include "editor/CodeAssist.h"
#include "editor/Columns.h"
#include "editor/Panel.h"
#include "editor/SchematicView.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"

#include "Examples.h"
#include "Library.h"
#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/widgets/TextEditor.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace pg = playground;
namespace col = playground::ui::columns;

namespace
{
    void writePng(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const char* name)
    {
        const std::string path = probe::outPath(name);
        host.draw();
        P.eq(std::string(name) + ".written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
    }

    // A drag from `from` by dx, in steps, released.
    void drag(funkgui::HeadlessHost& host, funkgui::Point from, float dx)
    {
        host.press(from.x, from.y);
        host.moveTo(from.x + dx, from.y, 8);
        host.release();
        host.settle();
    }

    int pref(const char* key) { return funkgui::UiPreferences::get().getInt(key, -1, -1, 100000); }
    uint32_t prefsRevision() { return funkgui::UiPreferences::get().revision(); }

    // The steps' pause and a little more, on the host's clock: a step's width reaches this Mac's default.
    void pause(funkgui::HeadlessHost& host) { host.tick(static_cast<int>(std::ceil(col::kDefaultPause * 60.0)) + 2); }

    // The keyboard's way to an edge: the editor's caret, Escape (nothing focused), then Shift-Tab to the last stop, the
    // chat's edge, and once more to the explorer's.
    bool focusEdge(pg::ui::Panel& panel, funkgui::HeadlessHost& host, bool chat)
    {
        const funkgui::Rect a = panel.editor().textArea();
        host.click(a.x + 30.0f, a.y + 4.0f);
        host.keys("escape");
        host.keys(chat ? "shift+tab" : "shift+tab, shift+tab");
        return panel.edgeFocused(chat);
    }

    // The program's card's width, as Panel lays it out: the window less the margins, the gaps and the two cards.
    float programOf(float window, const col::Fit& f, bool chat)
    {
        return window - 2.0f * col::kMargin - (chat ? col::kChatGap : col::kChatRailGap) - f.chat
             - (f.explorer > 0.0f ? f.explorer + col::kExplorerGap : 0.0f);
    }

    const funkgui::A11yItem* slider(const std::vector<funkgui::A11yItem>& items, const std::string& title)
    {
        for (const funkgui::A11yItem& it : items)
            if (it.title == title && it.role == funkgui::A11yRole::slider)
                return &it;
        return nullptr;
    }

    std::string libraryText(const char* name)
    {
        const pg::LibraryFile* f = pg::findLibraryFile(name);
        return f != nullptr ? std::string(f->source) : std::string();
    }
}

PROBE(gui, columns)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);

    // ---- FIT -----------------------------------------------------------------------------------------------------------
    {
        col::Asked a;
        a.window = static_cast<float>(pg::ui::kDefaultWidth);
        const col::Fit f = col::fit(a);
        // The shares, as the editor laid its columns out before widths could be set (1360: 0.16 and 0.32 of it).
        P.eq("fit.share_explorer", static_cast<int64_t>(f.explorer), 218);
        P.eq("fit.share_chat", static_cast<int64_t>(f.chat), 435);
        a.window = 2600.0f;
        const col::Fit wide = col::fit(a);
        P.eq("fit.share_most", wide.explorer == col::kExplorerAutoMaxW && wide.chat == col::kChatAutoMaxW ? 1 : 0, 1);
        a.explorerWant = 9999.0f;
        a.chatWant = 1.0f;
        const col::Fit set = col::fit(a);
        P.eq("fit.set_in_range", set.explorer == col::kExplorerMaxW && set.chat == col::kChatMinW ? 1 : 0, 1);
        P.eq("fit.kept_width", col::keptWidth(0.4f, true) == 0 && col::keptWidth(100.0f, false) == static_cast<int>(col::kExplorerMinW)
                                   && col::keptWidth(5000.0f, true) == static_cast<int>(col::kChatMaxW)
                                   && col::keptWidth(301.4f, false) == 301 && col::keptWidth(std::nanf(""), true) == 0
                                   ? 1 : 0, 1);
        // Every window width from the least up, every combination of the folds and some widths asked: the program's card
        // keeps its least, the cards theirs, a card never wider than asked, and each edge's reach holds its width.
        int bad = 0, cases = 0;
        const float wants[] = { 0.0f, 180.0f, 300.0f, 440.0f, 760.0f };
        for (int w = pg::ui::kMinWidth; w <= 2600; w += 7)
            for (const float ew : wants)
                for (const float cw : wants)
                    for (int flags = 0; flags < 4; ++flags)
                    {
                        col::Asked q;
                        q.window = static_cast<float>(w);
                        q.explorer = (flags & 1) != 0;
                        q.chat = (flags & 2) != 0;
                        q.explorerWant = ew;
                        q.chatWant = cw;
                        const col::Fit g = col::fit(q);
                        ++cases;
                        const bool ok = programOf(q.window, g, q.chat) >= col::kProgramMinW - 0.01f
                                     && (g.explorer == 0.0f || (q.explorer && g.explorer >= col::kExplorerMinW
                                                                && g.explorer <= col::kExplorerMaxW && g.explorer <= g.explorerMost))
                                     && (q.chat ? g.chat >= col::kChatMinW && g.chat <= col::kChatMaxW && g.chat <= g.chatMost
                                                : g.chat == col::kChatRailW)
                                     && (ew < 1.0f || g.explorer <= std::max(ew, col::kExplorerMinW))
                                     && (cw < 1.0f || !q.chat || g.chat <= std::max(cw, col::kChatMinW));
                        bad += ok ? 0 : 1;
                    }
        P.eq("fit.every_width_holds", bad, 0);
        P.ge("fit.cases", cases, 4000);
        // Narrowing: the explorer gives way first, then the chat, then the explorer folds (and the chat has the room).
        col::Asked n;
        n.explorerWant = 300.0f;
        n.chatWant = 520.0f;
        n.window = 1800.0f;
        const col::Fit roomy = col::fit(n);
        P.eq("fit.both_as_asked", roomy.explorer == 300.0f && roomy.chat == 520.0f ? 1 : 0, 1);
        n.window = 1400.0f;   // room for 736: the explorer gives 84
        const col::Fit first = col::fit(n);
        P.eq("fit.explorer_gives_first", first.explorer == 216.0f && first.chat == 520.0f ? 1 : 0, 1);
        n.window = 1300.0f;   // room for 636: the explorer at its least, the chat 456
        const col::Fit second = col::fit(n);
        P.eq("fit.chat_gives_next", second.explorer == col::kExplorerMinW && second.chat == 456.0f ? 1 : 0, 1);
        n.window = 1100.0f;   // room for 436 beside each other: too little; the explorer folds, the chat 448
        const col::Fit folded = col::fit(n);
        P.eq("fit.explorer_folds", folded.explorer == 0.0f && folded.chat == 448.0f ? 1 : 0, 1);
        // The reach: the explorer's edge stops at the chat's width; the chat's takes the explorer to its least.
        P.eq("fit.explorer_most", static_cast<int64_t>(first.explorerMost), 216);
        P.eq("fit.chat_most", static_cast<int64_t>(first.chatMost), 556);
        n.chat = false;
        const col::Fit rail = col::fit(n);   // 1100 with the chat folded: the explorer beside the rail
        P.eq("fit.rail", rail.chat == col::kChatRailW && rail.explorer == 300.0f && rail.explorerMost == 392.0f ? 1 : 0, 1);
    }

    pg::Processor processor;
    pg::EditorPort& port = processor.editorPort();
    processor.settle();
    pg::ui::Panel panel(processor.ports(), port);
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.setZoom({ 100, 125, 150, 200 }, 100);
    host.settle();
    const float W = static_cast<float>(pg::ui::kDefaultWidth);

    // ---- DEFAULT -------------------------------------------------------------------------------------------------------
    P.eq("default.none_set", port.explorerWidth() == 0 && port.chatWidth() == 0 && panel.explorerWant() == 0.0f
                                 && panel.chatWant() == 0.0f ? 1 : 0, 1);
    P.eq("default.shares", panel.explorerCard().w == 218.0f && panel.chatCard().w == 435.0f ? 1 : 0, 1);
    P.eq("default.edges_shown", panel.explorerEdgeShown() && panel.chatEdgeShown() ? 1 : 0, 1);
    {
        const funkgui::Point e = panel.centreOfExplorerEdge(), c = panel.centreOfChatEdge();
        P.near("default.explorer_edge_x", static_cast<double>(e.x), static_cast<double>(panel.explorerCard().right()), 0.01);
        P.near("default.chat_edge_x", static_cast<double>(c.x), static_cast<double>(panel.chatCard().x), 0.01);
        host.move(e.x + 4.0f, e.y);
        const bool explorerCursor = host.cursor() == funkgui::Cursor::leftRight;
        host.move(c.x - 4.0f, c.y);
        const bool chatCursor = host.cursor() == funkgui::Cursor::leftRight;
        host.move(c.x - 40.0f, c.y);
        P.eq("default.left_right_cursor", explorerCursor && chatCursor && host.cursor() != funkgui::Cursor::leftRight ? 1 : 0, 1);
        const std::vector<funkgui::A11yItem> items = host.accessibility();
        const funkgui::A11yItem* es = slider(items, "Explorer width");
        const funkgui::A11yItem* cs = slider(items, "Chat width");
        P.eq("default.a11y_sliders", es != nullptr && cs != nullptr ? 1 : 0, 1);
        P.eq("default.a11y_values", es != nullptr && cs != nullptr && es->value == "218 px" && cs->value == "435 px"
                                        && es->lo == col::kExplorerMinW && cs->lo == col::kChatMinW && es->hi >= es->v
                                        && cs->hi >= cs->v ? 1 : 0, 1);
    }

    // ---- DRAG ----------------------------------------------------------------------------------------------------------
    {
        const std::string before = panel.editor().text();
        const float program0 = panel.programCard().w, chatX0 = panel.chatCard().x;
        const funkgui::Point e = panel.centreOfExplorerEdge();
        host.press(e.x, e.y);
        host.moveTo(e.x + 40.0f, e.y, 6);
        host.tick(1);
        P.eq("drag.follows", static_cast<int64_t>(panel.explorerCard().w), 258);
        P.eq("drag.nothing_kept_yet", port.explorerWidth() == 0 && pref(col::kExplorerWidthKey) <= 0 ? 1 : 0, 1);
        P.eq("drag.cursor", host.cursor() == funkgui::Cursor::leftRight ? 1 : 0, 1);
        host.release();
        host.settle();
        P.eq("drag.explorer_wider", static_cast<int64_t>(panel.explorerCard().w), 258);
        P.near("drag.program_narrower", static_cast<double>(panel.programCard().w), static_cast<double>(program0 - 40.0f), 0.01);
        P.near("drag.chat_unmoved", static_cast<double>(panel.chatCard().x), static_cast<double>(chatX0), 0.01);
        P.eq("drag.kept_session", port.explorerWidth(), 258);
        P.eq("drag.kept_default", pref(col::kExplorerWidthKey), 258);
        // The chat's edge to the left: the chat wider by as much; the explorer gives way (the chat keeps its width
        // first), the program's card at its least.
        const funkgui::Point c = panel.centreOfChatEdge();
        drag(host, c, -60.0f);
        P.eq("drag.chat_wider", static_cast<int64_t>(panel.chatCard().w), 495);
        P.eq("drag.chat_kept", port.chatWidth() == 495 && pref(col::kChatWidthKey) == 495 ? 1 : 0, 1);
        P.ge("drag.program_least", static_cast<double>(panel.programCard().w), static_cast<double>(col::kProgramMinW) - 0.01);
        P.eq("drag.explorer_gave_way", static_cast<int64_t>(panel.explorerCard().w),
             static_cast<int64_t>(W - 2.0f * col::kMargin - col::kChatGap - col::kExplorerGap - col::kProgramMinW - 495.0f));
        P.eq("drag.explorer_still_asks", static_cast<int64_t>(panel.explorerWant()), 258);
        // The edge took no focus: the editor keeps the caret, and an arrow moves it there, not an edge.
        const float ew = panel.explorerCard().w, cw = panel.chatCard().w;
        host.keys("right");
        P.eq("drag.caret_stays", panel.editor().focused() && panel.editor().text() == before && panel.explorerCard().w == ew
                                     && panel.chatCard().w == cw ? 1 : 0, 1);
        host.move(panel.programCard().centreX(), panel.programCard().y + 4.0f);   // the edges at rest
        host.settle();
        writePng(P, host, "gui.columns.png");
    }

    // ---- CLAMP ---------------------------------------------------------------------------------------------------------
    {
        // The chat's edge far to the left: it stops where the explorer is at its least.
        drag(host, panel.centreOfChatEdge(), -900.0f);
        const float most = W - 2.0f * col::kMargin - col::kChatGap - col::kExplorerGap - col::kProgramMinW - col::kExplorerMinW;
        P.eq("clamp.chat_most", static_cast<int64_t>(panel.chatCard().w), static_cast<int64_t>(most));
        P.eq("clamp.explorer_least", static_cast<int64_t>(panel.explorerCard().w), static_cast<int64_t>(col::kExplorerMinW));
        P.near("clamp.program_least", static_cast<double>(panel.programCard().w), static_cast<double>(col::kProgramMinW), 0.01);
        // ... and far to the right: its least.
        drag(host, panel.centreOfChatEdge(), 900.0f);
        P.eq("clamp.chat_least", static_cast<int64_t>(panel.chatCard().w), static_cast<int64_t>(col::kChatMinW));
        P.eq("clamp.chat_least_kept", port.chatWidth(), static_cast<int64_t>(col::kChatMinW));
        // The explorer, back at its asked 258 now the chat leaves room, far to the right: it stops at the chat's width.
        P.eq("clamp.explorer_back", static_cast<int64_t>(panel.explorerCard().w), 258);
        drag(host, panel.centreOfExplorerEdge(), 900.0f);
        const float reach = W - 2.0f * col::kMargin - col::kChatGap - col::kExplorerGap - col::kProgramMinW - col::kChatMinW;
        P.eq("clamp.explorer_most", static_cast<int64_t>(panel.explorerCard().w),
             static_cast<int64_t>(std::min(reach, col::kExplorerMaxW)));
        P.eq("clamp.chat_kept_its_width", static_cast<int64_t>(panel.chatCard().w), static_cast<int64_t>(col::kChatMinW));
        drag(host, panel.centreOfExplorerEdge(), -900.0f);
        P.eq("clamp.explorer_least_drag", static_cast<int64_t>(panel.explorerCard().w), static_cast<int64_t>(col::kExplorerMinW));
    }

    // ---- RESET ---------------------------------------------------------------------------------------------------------
    {
        const funkgui::Point e = panel.centreOfExplorerEdge();
        host.doubleClick(e.x, e.y);
        host.settle();
        P.eq("reset.explorer_share", static_cast<int64_t>(panel.explorerCard().w), 218);
        P.eq("reset.explorer_none", port.explorerWidth() == 0 && pref(col::kExplorerWidthKey) == 0 ? 1 : 0, 1);
        const funkgui::Point c = panel.centreOfChatEdge();
        host.doubleClick(c.x, c.y);
        host.settle();
        P.eq("reset.chat_share", static_cast<int64_t>(panel.chatCard().w), 435);
        P.eq("reset.chat_none", port.chatWidth() == 0 && pref(col::kChatWidthKey) == 0 && panel.chatWant() == 0.0f ? 1 : 0, 1);
    }

    // ---- KEYS ----------------------------------------------------------------------------------------------------------
    {
        host.keys("escape, escape");                     // out of the editor: nothing focused
        host.keys("shift+tab");                          // the last stop: the chat's edge
        P.eq("keys.chat_edge_focused", panel.edgeFocused(true) ? 1 : 0, 1);
        const uint32_t rev0 = prefsRevision();
        host.keys("left");
        P.eq("keys.left_widens_chat", static_cast<int64_t>(panel.chatCard().w), 443);
        P.eq("keys.session_at_once", port.chatWidth(), 443);
        host.keys("shift+left");
        P.eq("keys.shift_32", static_cast<int64_t>(panel.chatCard().w), 475);
        host.keys("right");
        P.eq("keys.right_narrows", static_cast<int64_t>(panel.chatCard().w), 467);
        // This Mac's default waits for the steps to pause, then takes the last of them in one write (a held arrow
        // would otherwise write the preferences' file with every repeat).
        P.eq("keys.default_waits", pref(col::kChatWidthKey) == 0 && prefsRevision() == rev0 && port.chatWidth() == 467 ? 1 : 0, 1);
        pause(host);
        P.eq("keys.default_after_pause", pref(col::kChatWidthKey), 467);
        P.eq("keys.default_one_write", static_cast<int64_t>(prefsRevision() - rev0), 1);
        host.keys("home");
        P.eq("keys.home_least", static_cast<int64_t>(panel.chatCard().w), static_cast<int64_t>(col::kChatMinW));
        host.keys("end");
        P.eq("keys.end_most", static_cast<int64_t>(panel.chatCard().w), static_cast<int64_t>(panel.columnFit().chatMost));
        host.keys("delete");
        P.eq("keys.delete_resets", panel.chatCard().w == 435.0f && port.chatWidth() == 0 ? 1 : 0, 1);
        host.keys("shift+tab");                          // the explorer's edge
        host.keys("right");
        P.eq("keys.right_widens_explorer", static_cast<int64_t>(panel.explorerCard().w), 226);
        host.settle();
        // PAPER, the explorer's edge focused from the keyboard: its ring.
        funkgui::UiPreferences::get().setTheme(1);
        {
            pg::ui::Panel paper(processor.ports(), port);
            funkgui::HeadlessHost paperHost(paper, 1, 2.0f);
            paperHost.settle();
            paperHost.keys("escape, escape");
            paperHost.keys("shift+tab");
            paperHost.keys("shift+tab");
            paperHost.settle();
            P.eq("keys.paper_same_widths", paper.explorerCard().w == panel.explorerCard().w
                                               && paper.chatCard().w == panel.chatCard().w ? 1 : 0, 1);
            writePng(P, paperHost, "gui.columns.paper.png");
            // ... and the chat's edge under the pointer, its hairline lit on PAPER's card edge (the ring put away).
            paperHost.keys("escape");
            const funkgui::Point c = paper.centreOfChatEdge();
            paperHost.move(c.x - 3.0f, c.y);
            paperHost.tick(1);
            P.eq("keys.paper_hover_cursor", paperHost.cursor() == funkgui::Cursor::leftRight ? 1 : 0, 1);
            writePng(P, paperHost, "gui.columns.hover.paper.png");
        }
        funkgui::UiPreferences::get().setTheme(0);
        host.keys("escape");
    }

    // ---- A11Y ----------------------------------------------------------------------------------------------------------
    {
        const std::vector<funkgui::A11yItem> items = host.accessibility();
        const funkgui::A11yItem* cs = slider(items, "Chat width");
        const uint32_t id = cs != nullptr ? cs->id : 0;
        panel.a11yAction(id, funkgui::A11yAction::increment, 0.0);
        P.eq("a11y.increment", static_cast<int64_t>(panel.chatCard().w), 443);
        panel.a11yAction(id, funkgui::A11yAction::decrement, 0.0);
        panel.a11yAction(id, funkgui::A11yAction::decrement, 0.0);
        P.eq("a11y.decrement", static_cast<int64_t>(panel.chatCard().w), 427);
        panel.a11yAction(id, funkgui::A11yAction::setValue, 480.0);
        P.eq("a11y.set_value", static_cast<int64_t>(panel.chatCard().w), 480);
        const std::vector<funkgui::A11yItem> after = host.accessibility();
        const funkgui::A11yItem* now = slider(after, "Chat width");
        P.eq("a11y.value_follows", now != nullptr && now->value == "480 px" && port.chatWidth() == 480 ? 1 : 0, 1);
        // The explorer gave way for it: it asks 226 still, and shows what the chat leaves.
        P.eq("a11y.explorer_gave_way", panel.explorerWant() == 226.0f && panel.explorerCard().w == 216.0f ? 1 : 0, 1);
        // A step against the reach moves nothing, and keeps nothing: the explorer still asks 226.
        const funkgui::A11yItem* es = slider(after, "Explorer width");
        if (es != nullptr)
            panel.a11yAction(es->id, funkgui::A11yAction::increment, 0.0);
        P.eq("a11y.reach_keeps_asked", es != nullptr && panel.explorerWant() == 226.0f && port.explorerWidth() == 226
                                           && panel.explorerCard().w == 216.0f ? 1 : 0, 1);
        pause(host);
        P.eq("a11y.default_after_pause", pref(col::kChatWidthKey) == 480 && pref(col::kExplorerWidthKey) == 226 ? 1 : 0, 1);
        host.settle();
        const funkgui::Point c = panel.centreOfChatEdge();
        host.move(c.x - 3.0f, c.y);
        host.settle();
        writePng(P, host, "gui.columns.hover.png");
        host.move(c.x - 200.0f, c.y);
    }

    // ---- KEPT ----------------------------------------------------------------------------------------------------------
    {
        // The session now: the explorer 226, the chat 480.
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        funkgui::UiPreferences::get().setInt(col::kExplorerWidthKey, 300);   // another instance's since
        funkgui::UiPreferences::get().setInt(col::kChatWidthKey, 400);
        pg::Processor other;
        other.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        P.eq("kept.in_state", other.editorPort().explorerWidth() == 226 && other.editorPort().chatWidth() == 480 ? 1 : 0, 1);
        {
            pg::ui::Panel reopened(other.ports(), other.editorPort());
            funkgui::HeadlessHost h2(reopened, 0, 2.0f);
            h2.resize(1800, 1000);                       // room for both as asked
            h2.settle();
            P.eq("kept.session_over_default", reopened.explorerCard().w == 226.0f && reopened.chatCard().w == 480.0f ? 1 : 0, 1);
        }
        // A new instance has set none: this Mac's default.
        pg::Processor fresh;
        {
            pg::ui::Panel newPanel(fresh.ports(), fresh.editorPort());
            funkgui::HeadlessHost h3(newPanel, 0, 2.0f);
            h3.resize(1800, 1000);
            h3.settle();
            P.eq("kept.new_takes_default", newPanel.explorerCard().w == 300.0f && newPanel.chatCard().w == 400.0f
                                               && fresh.editorPort().explorerWidth() == 0 ? 1 : 0, 1);
        }
        // The default as the host found it when it attached the Panel: EditorHost reloads the preferences between making
        // the Panel and attaching it, so a width another process set meanwhile counts.
        {
            pg::Processor late;
            pg::ui::Panel latePanel(late.ports(), late.editorPort());
            funkgui::UiPreferences::get().setInt(col::kChatWidthKey, 410);
            funkgui::HeadlessHost h5(latePanel, 0, 2.0f);
            h5.resize(1800, 1000);
            h5.settle();
            P.eq("kept.default_at_attach", latePanel.chatWant() == 410.0f && latePanel.chatCard().w == 410.0f
                                               && latePanel.explorerCard().w == 300.0f && late.editorPort().chatWidth() == 0
                                               ? 1 : 0, 1);
        }
        // A second Panel on the processor follows a width the first sets.
        {
            pg::ui::Panel second(processor.ports(), port);
            funkgui::HeadlessHost h4(second, 0, 2.0f);
            h4.settle();
            P.eq("kept.second_same", second.chatCard().w == 480.0f ? 1 : 0, 1);
            panel.a11yAction(slider(host.accessibility(), "Chat width")->id, funkgui::A11yAction::increment, 0.0);
            h4.tick(1);
            P.eq("kept.second_follows", static_cast<int64_t>(second.chatCard().w), 488);
        }
        // A fresh start (EXAMPLES) keeps the editor's look, its widths too.
        panel.loadExample(1);
        processor.settle();
        host.settle();
        const pg::Draft now = port.draft();
        P.eq("kept.fresh_start", !now.files.empty() && now.files[0].text == std::string(pg::kExamples[1].source)
                                     && panel.explorerWant() == 226.0f && port.explorerWidth() == 226
                                     && panel.chatCard().w == 488.0f && port.chatWidth() == 488 ? 1 : 0, 1);
    }

    // ---- GIVE ----------------------------------------------------------------------------------------------------------
    {
        host.resize(1800, 1000);
        host.settle();
        panel.a11yAction(slider(host.accessibility(), "Explorer width")->id, funkgui::A11yAction::setValue, 320.0);
        panel.a11yAction(slider(host.accessibility(), "Chat width")->id, funkgui::A11yAction::setValue, 600.0);
        host.settle();
        P.eq("give.wide", panel.explorerCard().w == 320.0f && panel.chatCard().w == 600.0f ? 1 : 0, 1);
        writePng(P, host, "gui.columns.wide.png");
        funkgui::UiPreferences::get().setTheme(1);
        {
            pg::ui::Panel paper(processor.ports(), port);
            funkgui::HeadlessHost paperHost(paper, 1, 2.0f);
            paperHost.resize(1800, 1000);
            paperHost.settle();
            P.eq("give.wide_paper", paper.explorerCard().w == 320.0f && paper.chatCard().w == 600.0f ? 1 : 0, 1);
            writePng(P, paperHost, "gui.columns.wide.paper.png");
        }
        funkgui::UiPreferences::get().setTheme(0);
        pause(host);                                     // the steps' widths in this Mac's default
        host.resize(1300, pg::ui::kDefaultHeight);
        host.settle();
        // room for 636 beside each other: the explorer at its least, the chat 456
        P.eq("give.explorer_first", static_cast<int64_t>(panel.explorerCard().w), static_cast<int64_t>(col::kExplorerMinW));
        P.eq("give.chat_next", static_cast<int64_t>(panel.chatCard().w), 456);
        P.near("give.program_least", static_cast<double>(panel.programCard().w), static_cast<double>(col::kProgramMinW), 0.01);
        P.eq("give.asked_kept", port.explorerWidth() == 320 && port.chatWidth() == 600 ? 1 : 0, 1);
        writePng(P, host, "gui.columns.narrow.png");
        // Both at their reach: a step that would widen either moves nothing, and so keeps nothing (the chat's Left and
        // End, a screen reader's increment; the explorer's Right and End).
        const bool chatFocused = focusEdge(panel, host, true);
        host.keys("left, end");
        panel.a11yAction(slider(host.accessibility(), "Chat width")->id, funkgui::A11yAction::increment, 0.0);
        host.keys("shift+tab");                          // the explorer's edge
        const bool explorerFocused = panel.edgeFocused(false);
        host.keys("right, end");
        pause(host);
        P.eq("give.edges_focused", chatFocused && explorerFocused ? 1 : 0, 1);
        P.eq("give.reach_keeps_asked", panel.chatWant() == 600.0f && panel.explorerWant() == 320.0f && port.chatWidth() == 600
                                           && port.explorerWidth() == 320 && pref(col::kChatWidthKey) == 600
                                           && pref(col::kExplorerWidthKey) == 320 && panel.chatCard().w == 456.0f
                                           && panel.explorerCard().w == col::kExplorerMinW ? 1 : 0, 1);
        host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight);
        host.settle();
        P.eq("give.explorer_folds", !panel.explorerShown() && panel.explorerOpen() && !panel.explorerEdgeShown() ? 1 : 0, 1);
        // Its edge had the focus: it gives it up, and no key sets the explorer's width while it is away.
        P.eq("give.hidden_edge_unfocused", panel.edgeFocused(false) ? 0 : 1, 1);
        host.keys("left, right, end, delete");
        pause(host);
        P.eq("give.hidden_edge_sets_nothing", panel.explorerWant() == 320.0f && port.explorerWidth() == 320
                                                  && pref(col::kExplorerWidthKey) == 320 ? 1 : 0, 1);
        P.eq("give.chat_room", static_cast<int64_t>(panel.chatCard().w),
             static_cast<int64_t>(pg::ui::kMinWidth - 2.0f * col::kMargin - col::kChatGap - col::kProgramMinW));
        P.ge("give.program_least_small", static_cast<double>(panel.programCard().w), static_cast<double>(col::kProgramMinW) - 0.01);
        writePng(P, host, "gui.columns.small.png");
        host.resize(1800, 1000);
        host.settle();
        P.eq("give.back", panel.explorerCard().w == 320.0f && panel.chatCard().w == 600.0f ? 1 : 0, 1);
    }

    // ---- OVER ----------------------------------------------------------------------------------------------------------
    {
        // The completion list, which the window bounds and not the program's card, over the chat's edge: a long line's
        // end, then Ctrl-Space.
        pg::ui::Panel over(processor.ports(), port);
        funkgui::HeadlessHost ho(over, 0, 2.0f);
        ho.resize(1800, 1000);
        ho.settle();
        funkgui::TextEditor& ed = over.editor();
        const funkgui::Rect a = ed.textArea();
        ho.click(a.x + 30.0f, a.y + 4.0f);               // the editor has the keys
        const int last = ed.lineCount() - 1;
        ed.setCaret({ last, static_cast<int>(ed.line(last).size()) });
        std::string spaces = "return";
        for (int k = 0; k < 72; ++k)
            spaces += ", space";
        ho.keys(spaces.c_str());
        ho.keys("ctrl+space");
        ho.tick(2);
        const bool open = over.codeAssist().completionOpen();
        float x0 = 1.0e9f, y0 = 1.0e9f, x1 = -1.0e9f;
        for (const funkgui::Prim& p : ho.draw().prims)
            if (p.tag == pg::ui::kCodePopupTag)
            {
                x0 = std::min(x0, p.x0);
                y0 = std::min(y0, p.y0);
                x1 = std::max(x1, p.x1);
            }
        const float edgeX = over.chatCard().x;
        const funkgui::Point at{ edgeX - 2.0f, y0 + 20.0f };   // the band's middle, the list's first row
        P.eq("over.list_over_band", open && x0 + 16.0f < at.x && at.x < x1 - 16.0f ? 1 : 0, 1);
        ho.move(at.x, at.y);
        ho.tick(1);
        P.eq("over.list_cursor", ho.cursor() == funkgui::Cursor::pointingHand ? 1 : 0, 1);
        const std::string before = ed.text();
        ho.press(at.x, at.y);
        ho.moveTo(at.x - 40.0f, at.y, 4);
        ho.release();
        ho.settle();
        P.eq("over.press_chooses", !over.codeAssist().completionOpen() && ed.text() != before ? 1 : 0, 1);
        P.eq("over.no_edge_moved", over.chatCard().w == 600.0f && port.chatWidth() == 600 && over.chatWant() == 600.0f ? 1 : 0, 1);
    }

    // ---- VIEWS ---------------------------------------------------------------------------------------------------------
    {
        panel.setPanelView(true);
        host.settle();
        P.eq("views.panel_no_explorer", !panel.explorerShown() && !panel.explorerEdgeShown() && panel.chatEdgeShown() ? 1 : 0, 1);
        P.eq("views.panel_chat_kept", static_cast<int64_t>(panel.chatCard().w), 600);
        writePng(P, host, "gui.columns.panel.png");
        panel.setPanelView(false);
        host.settle();
        const bool focused = focusEdge(panel, host, true);
        panel.setChatOpen(false);
        host.settle();
        P.eq("views.rail_no_edge", !panel.chatEdgeShown() && panel.explorerEdgeShown() ? 1 : 0, 1);
        // Its edge had the focus: it gives it up, and no key sets the chat's width while it is folded.
        P.eq("views.fold_unfocuses", focused && !panel.edgeFocused(true) ? 1 : 0, 1);
        host.keys("left, delete");
        P.eq("views.folded_sets_nothing", panel.chatWant() == 600.0f && port.chatWidth() == 600 ? 1 : 0, 1);
        P.eq("views.rail_reach", static_cast<int64_t>(panel.columnFit().explorerMost), static_cast<int64_t>(col::kExplorerMaxW));
        panel.setChatOpen(true);
        host.settle();
        P.eq("views.chat_back", static_cast<int64_t>(panel.chatCard().w), 600);
        // A netlist's Schematic view, between the explorer and the chat at their widths.
        pg::Draft d;
        d.files = { { "program.fm", std::string(pg::kExamples[0].source) }, { "diodetone.cir", libraryText("diodetone.cir") } };
        d.active = 1;
        port.setDraft(d);
        pg::ui::Panel sch(processor.ports(), port);
        funkgui::HeadlessHost hs(sch, 0, 2.0f);
        hs.resize(1800, 1000);
        hs.settle();
        sch.setSchematic(true);
        hs.settle();
        const funkgui::Rect area = sch.editorArea();
        P.eq("views.schematic_shown", sch.schematicShown() ? 1 : 0, 1);
        P.eq("views.schematic_between", area.x >= sch.explorerCard().right() && area.right() <= sch.chatCard().x
                                            && sch.explorerCard().w == 320.0f && sch.chatCard().w == 600.0f ? 1 : 0, 1);
        writePng(P, hs, "gui.columns.schematic.png");
    }
    return P.finish();
}
