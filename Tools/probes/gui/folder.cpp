// PROBE name=gui.folder timeout=180
//
// gui.folder: the explorer's FOLDER section (editor/Explorer.h; plugin/FolderSync.h), headless over a real Processor
// whose projects, and so its folders, are in a temporary folder of the probe's own, the folder's editor played by the
// probe's writes there, to <build>/probe-out/:
//   gui.folder.off.png      off: the row that turns it on, under FILES
//   gui.folder.png          on, after a save in the folder landed (the status says so), with a conflict and a note
//   gui.folder.paper.png    the same in PAPER
//   gui.folder.wide.png     a wide window
//   gui.folder.small.png    the least size (the chat folded, so the explorer shows)
// and checks:
//   OFF      the section after FILES: one row, Edit in a folder; no header button
//   ON       its click turns the processor's folder on, named for the instance; a caption naming it (its path in the
//            status line under the pointer), then the rows Open in VS Code, Show in Finder and Run on save; the
//            header's cross; the status names it
//   LAUNCH   VS Code's row and the Finder's go to the processor's launcher (VS Code, the Finder)
//   RUN      Run on save's click ticks it (the processor's choice), a second unticks it
//   SAVE     a save in the folder lands in the editor's tab, the status says so ("FROM THE FOLDER: PROGRAM.FM"), and
//            one Cmd-Z gives the text before it back (one undo step); typing in the editor reaches the folder
//   CONFLICT the program changed in the editor and in the folder before they met: a row with the red dot, the status
//            says so; its click asks (a menu of two); keeping the folder's, the tab shows it and the editor's text is
//            in .conflicts/; a library file an #include names that the program lacks, a read-only copy
//   A11Y     the folder's rows are buttons (Open in VS Code), a note is text
//   STOP     the header's cross turns it off, the folder left as it was; the row to turn it on is back
#include "Probe.h"

#include "editor/Explorer.h"
#include "editor/Panel.h"
#include "plugin/EditorPort.h"
#include "plugin/ProjectStore.h"
#include "plugin/Processor.h"

#include "Library.h"
#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/widgets/TextEditor.h>

#include <juce_core/juce_core.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace pg = playground;
namespace fs = std::filesystem;
using S = pg::ui::ExplorerItem::Section;

namespace
{
    std::string program(const std::string& body)
    {
        if constexpr (pg::product::kIsInstrument)
            return "// Folder: a program\nmodule Voice\n{\n    " + body
                 + "\n    void start(float note, float v) {}\n"
                   "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n";
        else
            return "// Folder: a program\nmodule Effect\n{\n    " + body
                 + "\n    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
    }

    std::string readFile(const fs::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    void writeFile(const fs::path& p, const std::string& text)
    {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << text;
    }
    bool keptAside(const fs::path& dir, const std::string& text)
    {
        std::error_code ec;
        for (fs::directory_iterator it(dir / pg::FolderSync::kConflicts, ec), end; !ec && it != end; it.increment(ec))
            if (readFile(it->path()) == text)
                return true;
        return false;
    }

    void writePng(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const char* name)
    {
        const std::string path = probe::outPath(name);
        host.draw();
        P.eq(std::string(name) + ".written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
    }

    void clickAt(funkgui::HeadlessHost& host, const std::optional<funkgui::Point>& p)
    {
        if (p)
            host.click(p->x, p->y);
        host.tick(2);
        host.settle();
    }

    int find(const pg::ui::ExplorerItem& ex, S s, const std::string& label)
    {
        for (int i = 0; i < ex.rows(s); ++i)
            if (const auto* r = ex.row(s, i); r != nullptr && r->label == label)
                return i;
        return -1;
    }

#if defined(_WIN32)
    constexpr const char* kShow = "Show in Explorer";
#else
    constexpr const char* kShow = "Show in Finder";
#endif

    // The processor's timer, as the plugin runs it: a poll every 25 ms of real time, the Panel's frames between,
    // until `done` or `seconds`.
    bool pump(pg::Processor& p, funkgui::HeadlessHost& host, double seconds, const std::function<bool()>& done)
    {
        const auto from = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count() < seconds)
        {
            p.poll();
            host.tick(1);
            if (done && done())
            {
                host.settle();
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        host.settle();
        return done ? done() : true;
    }
}

PROBE(gui, folder)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);
    const juce::File temp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getChildFile(juce::String(pg::product::kEnvPrefix) + "gui-folder-" + juce::String(juce::Time::currentTimeMillis()));
    temp.deleteRecursively();
    pg::projectstore::setDirectoryForTests(temp);

    pg::Processor processor;
    pg::EditorPort& port = processor.editorPort();
    std::vector<pg::FolderApp> launched;
    processor.setFolderLauncher([&launched](pg::FolderApp app, const std::string&, std::string&) {
        launched.push_back(app);
        return true;
    });
    pg::Draft d;
    const std::string first = program("param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];");
    d.files = { { "program.fm", first } };
    port.setDraft(d);
    port.run(d.files);
    processor.settle();

    pg::ui::Panel panel(processor.ports(), port);
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.settle();
    processor.settle();
    host.tick(4);
    host.settle();
    pg::ui::ExplorerItem& ex = panel.explorer();

    // ---- OFF -----------------------------------------------------------------------------------------------------------
    P.eq("off.one_row", ex.rows(S::folder), 1);
    P.eq("off.row", ex.row(S::folder, 0) != nullptr && ex.row(S::folder, 0)->label == "Edit in a folder" ? 1 : 0, 1);
    P.eq("off.no_button", ex.centreOfAction(S::folder).has_value() ? 0 : 1, 1);
    {
        const auto files = ex.centreOfHeader(S::files), folder = ex.centreOfHeader(S::folder), ir = ex.centreOfHeader(S::ir);
        P.eq("off.after_files", files && folder && ir && files->y < folder->y && folder->y < ir->y ? 1 : 0, 1);
    }
    writePng(P, host, "gui.folder.off.png");

    // ---- ON ------------------------------------------------------------------------------------------------------------
    clickAt(host, ex.centreOfRow(S::folder, 0));
    const pg::FolderState on = port.folder();
    P.eq("on.port", on.on ? 1 : 0, 1);
    const fs::path dir = fs::path(temp.getChildFile("Folders").getFullPathName().toStdString()) / on.name;
    P.eq("on.named", !on.name.empty() && fs::is_directory(dir) ? 1 : 0, 1);
    P.eq("on.rows", find(ex, S::folder, "Open in VS Code") == 1 && find(ex, S::folder, kShow) == 2
                        && find(ex, S::folder, "Run on save") == 3 ? 1 : 0, 1);
    P.eq("on.name_caption", ex.row(S::folder, 0) != nullptr && ex.row(S::folder, 0)->heading
                                && ex.row(S::folder, 0)->label == pg::ui::upper(on.name) ? 1 : 0, 1);
    P.eq("on.stop_button", ex.centreOfAction(S::folder).has_value() ? 1 : 0, 1);
    P.eq("on.status", panel.statusText().find("FOLDER") != std::string::npos ? 1 : 0, 1);
    pump(processor, host, 0.6, [&] { return readFile(dir / "program.fm") == first; });
    P.eq("on.files_out", readFile(dir / "program.fm") == first ? 1 : 0, 1);

    // ---- LAUNCH --------------------------------------------------------------------------------------------------------
    clickAt(host, ex.centreOfRow(S::folder, find(ex, S::folder, "Open in VS Code")));
    clickAt(host, ex.centreOfRow(S::folder, find(ex, S::folder, kShow)));
    P.eq("launch.vscode_finder", launched.size() == 2u && launched[0] == pg::FolderApp::vscode && launched[1] == pg::FolderApp::files
                                     ? 1 : 0, 1);

    // ---- RUN -----------------------------------------------------------------------------------------------------------
    clickAt(host, ex.centreOfRow(S::folder, find(ex, S::folder, "Run on save")));
    P.eq("run.ticked", port.folder().runOnSave && ex.row(S::folder, find(ex, S::folder, "Run on save"))->marked ? 1 : 0, 1);
    clickAt(host, ex.centreOfRow(S::folder, find(ex, S::folder, "Run on save")));
    P.eq("run.unticked", !port.folder().runOnSave && !ex.row(S::folder, find(ex, S::folder, "Run on save"))->marked ? 1 : 0, 1);

    // ---- SAVE ----------------------------------------------------------------------------------------------------------
    const std::string saved = program("param float level [[fminus::range(0, 1)]] [[fminus::default(0.25)]];");
    writeFile(dir / "program.fm", saved);
    const bool landed = pump(processor, host, 2.0, [&] { return panel.editor().text() == saved; });
    P.eq("save.lands_in_tab", landed ? 1 : 0, 1);
    P.eq("save.status", panel.statusText().find("FROM THE FOLDER: PROGRAM.FM") != std::string::npos ? 1 : 0, 1);
    host.keys("cmd+z");
    host.settle();
    P.eq("save.one_undo_step", panel.editor().text() == first ? 1 : 0, 1);
    host.keys("cmd+shift+z");
    host.settle();
    P.eq("save.redo", panel.editor().text() == saved ? 1 : 0, 1);
    // Typing reaches the folder.
    panel.editor().setCaret(panel.editor().endPos());
    host.keys("/,/,space,t");
    host.settle();
    pump(processor, host, 2.0, [&] { return readFile(dir / "program.fm") == saved + "// t"; });
    P.eq("save.typing_out", readFile(dir / "program.fm") == saved + "// t" ? 1 : 0, 1);

    // ---- CONFLICT ------------------------------------------------------------------------------------------------------
    {
        // The editor's typing given to the processor at once, and the folder's save, before the sync meets either.
        const std::string mine = saved + "// mine\n", theirs = saved + "// theirs\n";
        panel.editor().setText(mine);
        (void)panel.pushFiles();
        writeFile(dir / "program.fm", theirs);
        const bool listed = pump(processor, host, 2.0, [&] { return !port.folder().conflicts.empty(); });
        P.eq("conflict.listed", listed ? 1 : 0, 1);
        const int row = find(ex, S::folder, "program.fm");
        P.eq("conflict.row", row >= 0 && ex.row(S::folder, row)->error ? 1 : 0, 1);
        P.eq("conflict.status", panel.statusText().find("PROGRAM.FM: CHANGED HERE AND IN THE FOLDER") != std::string::npos ? 1 : 0, 1);
        P.eq("conflict.held", panel.editor().text() == mine && readFile(dir / "program.fm") == theirs ? 1 : 0, 1);
        // A file there whose name the plugin cannot take: left there, with a note.
        writeFile(dir / "my sketch.fm", "// a space in its name\n");
        const bool note = pump(processor, host, 2.0, [&] {
            for (const std::string& n : port.folder().notes)
                if (n.find("not a valid name") != std::string::npos)
                    return true;
            return false;
        });
        P.eq("conflict.note", note && ex.row(S::folder, ex.rows(S::folder) - 1)->heading ? 1 : 0, 1);
        {
            // The caption may be cut: under the pointer the status line says it whole.
            if (const auto at = ex.centreOfRow(S::folder, ex.rows(S::folder) - 1))
                host.move(at->x, at->y);
            host.tick(2);
            char line[256] = {};
            ex.specLine(line, sizeof line);
            P.eq("conflict.note_whole", std::string(line) == "FOLDER   MY SKETCH.FM: NOT A VALID NAME" ? 1 : 0, 1);
            if (const auto away = ex.centreOfRow(S::folder, find(ex, S::folder, "Run on save")))
                host.move(away->x, away->y);
            host.tick(1);
        }
        writePng(P, host, "gui.folder.png");
        {
            funkgui::UiPreferences::get().setTheme(1);
            pg::ui::Panel paper(processor.ports(), port);
            funkgui::HeadlessHost paperHost(paper, 1, 2.0f);
            paperHost.settle();
            paperHost.tick(2);
            paperHost.settle();
            writePng(P, paperHost, "gui.folder.paper.png");
            funkgui::UiPreferences::get().setTheme(0);
        }
        {
            const bool wide = host.resize(1900, 1000);
            host.settle();
            P.eq("wide.resized", wide ? 1 : 0, 1);
            writePng(P, host, "gui.folder.wide.png");
        }
        {
            const bool least = host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight);
            panel.setChatOpen(false);                    // the least width has room for the explorer beside the chat's rail
            host.settle();
            P.eq("small.shown", least && panel.explorerShown() && find(ex, S::folder, "program.fm") >= 0 ? 1 : 0, 1);
            writePng(P, host, "gui.folder.small.png");
            panel.setChatOpen(true);
            host.resize(pg::ui::kDefaultWidth, pg::ui::kDefaultHeight);
            host.settle();
        }
        clickAt(host, ex.centreOfRow(S::folder, find(ex, S::folder, "program.fm")));
        const funkgui::MenuRequest* menu = host.pendingMenu();
        P.eq("conflict.asks", menu != nullptr && menu->items.size() == 2u ? 1 : 0, 1);
        P.eq("conflict.keep_folder", menu != nullptr && host.chooseMenuItem(2) ? 1 : 0, 1);
        host.tick(2);
        host.settle();
        P.eq("conflict.tab_shows_folder", panel.editor().text() == theirs ? 1 : 0, 1);
        P.eq("conflict.mine_kept_aside", keptAside(dir, mine) ? 1 : 0, 1);
        P.eq("conflict.row_gone", find(ex, S::folder, "program.fm") < 0 ? 1 : 0, 1);
    }

    // ---- A11Y ----------------------------------------------------------------------------------------------------------
    {
        bool vscode = false;
        for (const funkgui::A11yItem& it : host.accessibility())
            vscode = vscode || (it.role == funkgui::A11yRole::button && it.title == "Open in VS Code");
        P.eq("a11y.button", vscode ? 1 : 0, 1);
    }

    // ---- STOP ----------------------------------------------------------------------------------------------------------
    {
        const std::string before = readFile(dir / "program.fm");
        clickAt(host, ex.centreOfAction(S::folder));
        P.eq("stop.off", port.folder().on ? 0 : 1, 1);
        P.eq("stop.folder_stays", readFile(dir / "program.fm") == before && !before.empty() ? 1 : 0, 1);
        P.eq("stop.row_back", ex.rows(S::folder) == 1 && ex.row(S::folder, 0)->label == "Edit in a folder" ? 1 : 0, 1);
    }

    pg::projectstore::setDirectoryForTests({});
    temp.deleteRecursively();
    return P.finish();
}
