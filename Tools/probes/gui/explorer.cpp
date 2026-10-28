// PROBE name=gui.explorer timeout=180
//
// gui.explorer: the explorer at the editor's left (editor/Explorer.h), headless over a real Processor, to
// <build>/probe-out/:
//   gui.explorer.png         the default size: a program of two files (the second's errors marked), a library file
//                            added, two projects, the open one's two presets
//   gui.explorer.folded.png  folded away, the program's card wider, the button that opens it in its header
//   gui.explorer.paper.png   in PAPER
//   gui.explorer.small.png   the least size with the chat folded, scrolled to the end
// and checks:
//   OPEN      open at first: its card at the left, the program's card after it and the chat's at the right, none
//             overlapping; the sections FILES, PRESETS, PROJECTS, LIBRARY and EXAMPLES; a row a tab, the shown one
//             marked; PRESETS, with no project open, says to save one
//   FILES     a click on a file's row shows its tab; a double click renames it (its tab's name being edited); the
//             header's + makes a new file (its name being edited); a row's cross closes a file with no text; a file
//             the last run's errors name has its mark, and loses it once the program compiles
//   LIBRARY   every library file, each category under its caption; a click adds one as a tab and the program
//             #includes it; its row is then ticked; a second click shows it, adding no tab
//   EXAMPLES  a click puts the example in the program (one edit: Cmd-Z takes it back) and runs it
//   PROJECTS  the projects saved, each with its newest version, the one open marked; a click opens one (its files in
//             the editor); with the editor's files edited it asks first (a menu), and opens on the menu's choice only;
//             the header's + opens PROJECTS to save
//   PRESETS   the presets of the project open, the one applied marked; a click applies one (its dial at once, no
//             compile); the header's + opens PROJECTS with a preset's name to save
//   FOLD      a section's header folds it (its rows gone) and opens it again; the chevron folds the explorer away (its
//             rows gone, the program's card wider by the explorer and its gap, the program's card's header holding the
//             button that opens it); the processor keeps it folded, and its state carries that to another processor;
//             the button opens it
//   PANEL     the PANEL view has no explorer and no button for it (the program's card from the left margin), the
//             session still keeping it open; the CODE view shows it again
//   A11Y      the explorer speaks as a list, its rows as buttons and its headers as toggles; a row pressed through
//             accessibility does what its click does
//   NARROW    at the least size with the chat open there is no room for it: it is away (still open in the session),
//             the program's card keeps kProgramMinW; its button folds the chat and shows it; the chat opened again,
//             away again; at the default size it shows
//   SCROLL    at the least size (the chat folded) the list is taller than its card: the wheel scrolls it
#include "Probe.h"

#include "editor/Explorer.h"
#include "editor/Panel.h"
#include "plugin/EditorPort.h"
#include "plugin/ProjectStore.h"
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
#include <cstdio>
#include <string>
#include <vector>

namespace tp = testingplayground;
using S = tp::ui::ExplorerItem::Section;

namespace
{
    // A program that #includes lib.fm, which may be broken.
    std::string program()
    {
        if constexpr (tp::product::kIsInstrument)
            return "// Two Files: a program and its helper\n#include \"lib.fm\"\nmodule Voice\n{\n"
                   "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                   "    void start(float note, float v) {}\n"
                   "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n";
        else
            return "// Two Files: a program and its helper\n#include \"lib.fm\"\nmodule Effect\n{\n"
                   "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                   "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        for (int i = 0; i < n; ++i)\n        {\n            left[i] *= level[i];\n"
                   "            right[i] *= level[i];\n        }\n    }\n};\n";
    }

    const char* kGoodLib = "float twice(float x) { return x * 2.0f; }\n";
    const char* kBadLib = "float twice(float x) { return x * ; }\n";

    void writePng(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const char* name)
    {
        const std::string path = probe::outPath(name);
        host.draw();
        P.eq(std::string(name) + ".written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
    }

    void settleRun(tp::Processor& p, funkgui::HeadlessHost& host)
    {
        p.settle();
        host.tick(4);
        host.settle();
    }

    void clickAt(funkgui::HeadlessHost& host, const std::optional<funkgui::Point>& p)
    {
        if (p)
            host.click(p->x, p->y);
        host.tick(2);
        host.settle();
    }

    // The program's first dial, level, in its own units.
    float level(tp::Processor& p) { return tp::plainOf(p.control(0), p.getParameters()[0]->getValue()); }
    void setLevel(tp::Processor& p, float plain) { p.getParameters()[0]->setValueNotifyingHost(tp::normOf(p.control(0), plain)); }

    int find(const tp::ui::ExplorerItem& ex, S s, const std::string& label)
    {
        for (int i = 0; i < ex.rows(s); ++i)
            if (const auto* r = ex.row(s, i); r != nullptr && r->label == label)
                return i;
        return -1;
    }

    bool overlap(const funkgui::Rect& a, const funkgui::Rect& b)
    {
        return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
    }
}

PROBE(gui, explorer)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);
    const juce::File folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                  .getChildFile("gui-explorer-" + juce::String(juce::Time::currentTimeMillis()));
    folder.deleteRecursively();
    tp::projectstore::setDirectoryForTests(folder);

    tp::Processor processor;
    tp::EditorPort& port = processor.editorPort();
    tp::Draft d;
    d.files = { { "program.fm", program() }, { "lib.fm", kBadLib } };
    port.setDraft(d);
    port.run(d.files);
    processor.settle();

    tp::ui::Panel panel(processor.ports(), port);
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.setZoom({ 100, 125, 150, 200 }, 100);
    host.settle();
    settleRun(processor, host);
    tp::ui::ExplorerItem& ex = panel.explorer();

    // ---- OPEN ----------------------------------------------------------------------------------------------------------
    P.eq("open.at_first", panel.explorerOpen() ? 1 : 0, 1);
    const funkgui::Rect card = panel.explorerCard(), program = panel.programCard(), chat = panel.chatCard();
    P.eq("open.left_of_program", card.right() <= program.x && program.right() <= chat.x ? 1 : 0, 1);
    P.eq("open.no_overlap", !overlap(card, program) && !overlap(program, chat) ? 1 : 0, 1);
    P.eq("open.files_rows", ex.rows(S::files), 2);
    P.eq("open.presets_say", ex.rows(S::presets) == 0 && ex.centreOfAction(S::presets) == std::nullopt ? 1 : 0, 1);
    P.eq("open.sections", ex.centreOfHeader(S::files) && ex.centreOfHeader(S::presets) && ex.centreOfHeader(S::projects)
                              && ex.centreOfHeader(S::library) && ex.rows(S::examples) > 0   // EXAMPLES under the library, out of view
                              ? 1 : 0, 1);
    P.eq("open.shown_marked", ex.row(S::files, panel.activeFile()) != nullptr && ex.row(S::files, panel.activeFile())->active
                                  ? 1 : 0, 1);
    P.eq("open.error_marked", ex.row(S::files, 1) != nullptr && ex.row(S::files, 1)->error && !ex.row(S::files, 0)->error
                                  ? 1 : 0, 1);

    // ---- FILES ---------------------------------------------------------------------------------------------------------
    clickAt(host, ex.centreOfRow(S::files, 1));
    P.eq("files.click_shows", panel.activeFile(), 1);
    P.eq("files.row_follows", ex.row(S::files, 1) != nullptr && ex.row(S::files, 1)->active ? 1 : 0, 1);
    // The library file fixed: the program compiles, and the mark goes.
    panel.editor().selectAll();
    panel.editor().insert(kGoodLib);
    panel.runNow();
    settleRun(processor, host);
    P.eq("files.error_gone", ex.row(S::files, 1) != nullptr && !ex.row(S::files, 1)->error ? 1 : 0, 1);
    if (const auto r = ex.centreOfRow(S::files, 0))
        host.doubleClick(r->x, r->y);
    host.settle();
    P.eq("files.double_click_renames", panel.renamingFile() && panel.activeFile() == 0 ? 1 : 0, 1);
    host.keys("escape");
    host.settle();
    const int before = panel.fileCount();
    clickAt(host, ex.centreOfAction(S::files));
    P.eq("files.plus_adds", panel.fileCount(), before + 1);
    P.eq("files.plus_renames", panel.renamingFile() ? 1 : 0, 1);
    P.eq("files.plus_row", ex.rows(S::files), before + 1);
    host.keys("escape");
    host.settle();
    // The new file is empty: its cross (shown under the pointer) closes it without asking.
    if (const auto r = ex.centreOfRow(S::files, before))
        host.move(r->x, r->y);
    host.settle();
    clickAt(host, ex.centreOfClose(S::files, before));
    P.eq("files.cross_closes", panel.fileCount(), before);
    P.eq("files.no_cross_on_program", ex.centreOfClose(S::files, 0).has_value() ? 0 : 1, 1);

    // ---- LIBRARY -------------------------------------------------------------------------------------------------------
    int libraryRows = 0, headings = 0;
    for (int i = 0; i < ex.rows(S::library); ++i)
        (ex.row(S::library, i)->heading ? headings : libraryRows) += 1;
    P.eq("library.every_file", libraryRows, static_cast<int64_t>(std::size(tp::kLibrary)));
    P.ge("library.captions", headings, 3);
    const std::string libName = tp::kLibrary[0].name;
    const int libRow = find(ex, S::library, libName);
    P.eq("library.unticked", libRow >= 0 && !ex.row(S::library, libRow)->marked ? 1 : 0, 1);
    ex.scrollTo(0.0f);
    host.settle();
    const int filesBefore = panel.fileCount();
    clickAt(host, ex.centreOfRow(S::library, libRow));
    P.eq("library.adds_tab", panel.fileCount(), filesBefore + 1);
    P.eq("library.included", port.draft().files[0].text.find("#include \"" + libName + "\"") != std::string::npos ? 1 : 0, 1);
    P.eq("library.ticked", ex.row(S::library, libRow) != nullptr && ex.row(S::library, libRow)->marked ? 1 : 0, 1);
    clickAt(host, ex.centreOfRow(S::files, 0));
    clickAt(host, ex.centreOfRow(S::library, libRow));
    P.eq("library.again_shows", panel.fileCount() == filesBefore + 1 && panel.fileName(panel.activeFile()) == libName ? 1 : 0, 1);

    // ---- PROJECTS (two saved) and PRESETS (the open one's, two) ---------------------------------------------------------
    {
        std::string why;
        tp::SavedVersion saved;
        port.setDraft(panel.fileCount() > 0 ? port.draft() : d);
        P.eq("projects.saved", port.saveVersion("Alpha Pad", saved, why) && port.saveVersion("Beta Lead", saved, why) ? 1 : 0, 1);
        setLevel(processor, 0.2f);
        P.eq("presets.saved", port.savePreset("Soft", why) ? 1 : 0, 1);
        setLevel(processor, 0.9f);
        P.eq("presets.saved_two", port.savePreset("Wide", why) ? 1 : 0, 1);
    }
    host.tick(200);                                      // past the explorer's three seconds
    host.settle();
    P.eq("projects.rows", ex.rows(S::projects), 2);
    {
        const int beta = find(ex, S::projects, "Beta Lead");
        const tp::ui::ExplorerItem::Row* r = beta >= 0 ? ex.row(S::projects, beta) : nullptr;
        P.eq("projects.open_marked", r != nullptr && r->active && r->detail == "v1" ? 1 : 0, 1);
    }
    P.eq("presets.rows", ex.rows(S::presets), 2);
    {
        const int wide = find(ex, S::presets, "Wide");
        P.eq("presets.applied_marked", wide >= 0 && ex.row(S::presets, wide)->active ? 1 : 0, 1);
    }
    writePng(P, host, "gui.explorer.png");
    {
        // A preset's click: its dial at once.
        clickAt(host, ex.centreOfRow(S::presets, find(ex, S::presets, "Soft")));
        host.settle();
        P.near("presets.applies", static_cast<double>(level(processor)), 0.2, 1e-4);
        P.eq("presets.applied", port.project().preset == "Soft" ? 1 : 0, 1);
        // Edited since: a project's click asks first; the menu's choice opens it.
        tp::ui::ExplorerItem::Row first = *ex.row(S::projects, find(ex, S::projects, "Alpha Pad"));
        panel.editor().setCaret(panel.editor().endPos());
        host.keys("return,/,/,space,e");
        host.settle();
        const int at = find(ex, S::projects, first.label);
        if (!ex.centreOfRow(S::projects, at))
            ex.scrollTo(ex.contentHeight());
        host.settle();
        clickAt(host, ex.centreOfRow(S::projects, at));
        const bool asked = host.pendingMenu() != nullptr;
        P.eq("projects.asks_when_edited", asked ? 1 : 0, 1);
        if (asked)
            host.chooseMenuItem(1);
        settleRun(processor, host);
        P.eq("projects.opened", port.project().name == first.label && port.project().version == 1 ? 1 : 0, 1);
        P.eq("projects.files_back", panel.editor().text().find("// e") == std::string::npos ? 1 : 0, 1);
        host.tick(10);
        host.settle();
        P.eq("presets.follow_the_project", ex.rows(S::presets) == 0 ? 1 : 0, 1);   // Alpha Pad has none
        clickAt(host, ex.centreOfAction(S::presets));
        P.eq("presets.plus_opens", panel.projectBrowser().isOpen() && panel.projectBrowser().presetMode() ? 1 : 0, 1);
        host.keys("escape");
        host.settle();
        clickAt(host, ex.centreOfAction(S::projects));
        P.eq("projects.plus_opens", panel.projectBrowser().isOpen() && !panel.projectBrowser().presetMode() ? 1 : 0, 1);
        host.keys("escape");
        host.settle();
    }

    // ---- EXAMPLES ------------------------------------------------------------------------------------------------------
    ex.scrollTo(0.0f);
    host.settle();
    ex.setSectionOpen(S::library, false);                // room for the examples in view
    host.settle();
    clickAt(host, ex.centreOfRow(S::examples, 1));
    settleRun(processor, host);
    P.eq("examples.loads", port.draft().files[0].text == tp::kExamples[1].source ? 1 : 0, 1);
    P.eq("examples.runs", port.running().front().text == tp::kExamples[1].source ? 1 : 0, 1);

    // ---- FOLD ----------------------------------------------------------------------------------------------------------
    ex.setSectionOpen(S::library, true);
    host.settle();
    clickAt(host, ex.centreOfHeader(S::library));
    P.eq("fold.section_closes", ex.sectionOpen(S::library) || ex.centreOfRow(S::library, 1).has_value() ? 0 : 1, 1);
    clickAt(host, ex.centreOfHeader(S::library));
    P.eq("fold.section_opens", ex.sectionOpen(S::library) ? 1 : 0, 1);
    const float wideBefore = panel.programCard().w, explorerW = panel.explorerCard().w;
    const funkgui::Point chevron = panel.centreOfExplorerToggle();
    host.click(chevron.x, chevron.y);
    host.settle();
    P.eq("fold.folded", panel.explorerOpen() ? 0 : 1, 1);
    P.eq("fold.rows_gone", ex.shown() ? 0 : 1, 1);
    P.near("fold.program_wider", panel.programCard().w - wideBefore, explorerW + 12.0f, 0.5);
    P.eq("fold.card_gone", panel.explorerCard().w == 0.0f ? 1 : 0, 1);
    {
        const funkgui::Point b = panel.centreOfExplorerToggle();
        const funkgui::Rect pc = panel.programCard();
        P.eq("fold.button_in_header", pc.contains(b) && b.y < pc.y + 40.0f && b.x < pc.x + 40.0f ? 1 : 0, 1);
    }
    P.eq("fold.port_keeps", port.explorerOpen() ? 0 : 1, 1);
    writePng(P, host, "gui.explorer.folded.png");
    {
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        tp::Processor other;
        other.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        P.eq("fold.state_carries", other.editorPort().explorerOpen() ? 0 : 1, 1);
    }
    const funkgui::Point rail = panel.centreOfExplorerToggle();
    host.click(rail.x, rail.y);
    host.settle();
    P.eq("fold.button_opens", panel.explorerOpen() && ex.shown() ? 1 : 0, 1);

    // ---- PANEL ---------------------------------------------------------------------------------------------------------
    {
        const funkgui::Point to = panel.centreOfView(true);
        host.click(to.x, to.y);
        host.settle();
        P.eq("panel.view", panel.panelView() ? 1 : 0, 1);
        P.eq("panel.away", !panel.explorerShown() && !ex.shown() ? 1 : 0, 1);
        P.le("panel.card_from_margin", panel.programCard().x, 24.0f);
        int buttons = 0;
        for (const funkgui::A11yItem& it : host.accessibility())
            buttons += it.title == "Fold or open the explorer" ? 1 : 0;
        P.eq("panel.no_button", buttons, 0);
        P.eq("panel.still_open", port.explorerOpen() ? 1 : 0, 1);
        const funkgui::Point back = panel.centreOfView(false);
        host.click(back.x, back.y);
        host.settle();
        P.eq("panel.code_shows", !panel.panelView() && panel.explorerShown() && ex.shown() ? 1 : 0, 1);
    }

    // ---- A11Y ----------------------------------------------------------------------------------------------------------
    {
        const std::vector<funkgui::A11yItem> items = host.accessibility();
        int lists = 0, buttons = 0, toggles = 0;
        uint32_t fileButton = 0;
        for (const funkgui::A11yItem& it : items)
        {
            if (it.title == "Explorer" && it.role == funkgui::a11yrole::list)
                ++lists;
            if (it.title == "FILES" && it.role == funkgui::A11yRole::toggleButton)
                ++toggles;
            if (it.role == funkgui::A11yRole::button && it.title == "lib.fm")
                fileButton = it.id;
            buttons += it.role == funkgui::A11yRole::button && it.help.find("Shows this file") != std::string::npos ? 1 : 0;
        }
        P.eq("a11y.list", lists, 1);
        P.eq("a11y.header_toggle", toggles, 1);
        P.ge("a11y.file_buttons", buttons, 2);
        clickAt(host, ex.centreOfRow(S::files, 0));
        if (fileButton != 0)
            panel.a11yAction(fileButton, funkgui::A11yAction::press, 0.0);
        host.settle();
        P.eq("a11y.press_shows", fileButton != 0 && panel.fileName(panel.activeFile()) == "lib.fm" ? 1 : 0, 1);
    }

    // ---- PAPER ---------------------------------------------------------------------------------------------------------
    {
        funkgui::UiPreferences::get().setTheme(1);
        tp::ui::Panel paper(processor.ports(), port);
        funkgui::HeadlessHost paperHost(paper, 1, 2.0f);
        paperHost.settle();
        writePng(P, paperHost, "gui.explorer.paper.png");
        funkgui::UiPreferences::get().setTheme(0);
    }

    // ---- NARROW --------------------------------------------------------------------------------------------------------
    {
        P.eq("narrow.least_size", host.resize(tp::ui::kMinWidth, tp::ui::kMinHeight) ? 1 : 0, 1);
        host.settle();
        P.eq("narrow.chat_open", panel.chatOpen() ? 1 : 0, 1);
        P.eq("narrow.still_open", panel.explorerOpen() ? 1 : 0, 1);
        P.eq("narrow.away", !panel.explorerShown() && !ex.shown() && panel.explorerCard().w == 0.0f ? 1 : 0, 1);
        P.ge("narrow.program_room", panel.programCard().w, 600.0f);
        const funkgui::Point r = panel.centreOfExplorerToggle();
        host.click(r.x, r.y);
        host.settle();
        P.eq("narrow.button_folds_chat", !panel.chatOpen() && panel.explorerShown() && ex.shown() ? 1 : 0, 1);
        P.ge("narrow.program_room_after", panel.programCard().w, 600.0f);
        panel.setChatOpen(true);
        host.settle();
        P.eq("narrow.chat_back_away", panel.explorerOpen() && !panel.explorerShown() ? 1 : 0, 1);
        const bool back = host.resize(tp::ui::kDefaultWidth, tp::ui::kDefaultHeight);
        host.settle();
        P.eq("narrow.default_shows", back && panel.explorerShown() ? 1 : 0, 1);
    }

    // ---- SCROLL --------------------------------------------------------------------------------------------------------
    {
        P.eq("scroll.least_size", host.resize(tp::ui::kMinWidth, tp::ui::kMinHeight) ? 1 : 0, 1);
        panel.setChatOpen(false);                    // the least width has room for the explorer beside the chat's rail
        host.settle();
        P.eq("scroll.shown", panel.explorerShown() ? 1 : 0, 1);
        P.ge("scroll.taller", ex.contentHeight(), ex.bounds().h + 1.0f);
        const float s0 = ex.scroll();
        host.wheel(ex.bounds().centreX(), ex.bounds().centreY(), -3.0f);
        host.settle();
        P.ge("scroll.wheel", ex.scroll() - s0, 20.0f);
        ex.scrollTo(ex.contentHeight());
        host.settle();
        writePng(P, host, "gui.explorer.small.png");
    }

    tp::projectstore::setDirectoryForTests({});
    folder.deleteRecursively();
    return P.finish();
}
