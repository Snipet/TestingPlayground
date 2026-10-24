// PROBE name=gui.dump timeout=240
//
// gui.dump: the editor's Panel, headless (funkgui::HeadlessHost: no window, no GPU), over a real Processor. It writes
// settled frames, rasterised on the CPU by FunkGui's SoftRaster at 2x, to <build>/probe-out/:
//   gui.dump.png         GRAPHITE (theme 0): the default program, its dials
//   gui.dump.paper.png   PAPER (theme 1)
//   gui.dump.error.png   a run with a syntax error: the status, the problem's line, the mark in the gutter
//   gui.dump.edited.png  after the EXAMPLES menu: another example, running, its dials
//   gui.dump.files.png   two files, the second shown, after a run whose error is in it: its tab's mark, the problem
// and checks:
//   FRAME     the frame settles, every glyph is in the font, the editor shows the processor's draft (the first
//             example), and the dials are the program's parameters: one for each, the rest not shown
//   TYPE      typed keys go to the editor, which has the caret when the panel opens; the processor's draft follows
//             the text; the status says the text is edited
//   RUN       Cmd-Return runs the text: the processor plays it, the status says it runs
//   ERROR     a program with a syntax error on its line 3: the status counts one error, the problems list shows it
//             with its line, a mark is drawn in the gutter, and the program that played plays on; a click on the
//             problem puts the caret on that line
//   EXAMPLES  the EXAMPLES button opens a menu of every example; choosing the second replaces the text with it and
//             runs it, and Cmd-Z gives the text back (one undo step). The panel shows the new program running, with
//             its dials, as soon as the compile finishes, before the processor's timer has taken it
//   DIAL      a drag up on the first dial raises its parameter by a quarter for 60 px; a double-click returns it to
//             the program's default
//   FILES     the + adds a second tab, file2.fm, shown, its name edited in place: typed keys and Return rename it
//             (filters.fm), a name taken already is refused (the edit goes on) and Escape cancels; text typed goes to
//             that file; the program includes it and runs: both files play. An error in the included file: the
//             problem names it and its line, its tab has a mark and the gutter a mark only while it is shown; a click
//             on the problem shows that file with the caret on the line. A click on the first tab shows the program
//             again. The processor's draft has both files and the tab shown. The x of a file with text asks first (a
//             menu; dismissed, the file stays), and closing it shows the tab before; an empty file closes at once
//   KEYS      Escape leaves the editor, after which Tab moves the focus ring (and no key reaches the text)
#include "Probe.h"

#include "editor/Panel.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"

#include "Examples.h"

#include <funkgui/canvas/Prim.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/widgets/TextEditor.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace tp = testingplayground;

namespace
{
    int countTag(const funkgui::PrimList& frame, funkgui::Tag tag)
    {
        int n = 0;
        for (const funkgui::Prim& p : frame.prims)
            n += p.tag == tag ? 1 : 0;
        return n;
    }

    bool writePng(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const char* name, const std::string& key)
    {
        const std::string path = probe::outPath(name);
        host.draw();                                            // writePng rasterises the last frame drawn
        const bool ok = host.writePng(path.c_str(), 2);
        P.eq(key + ".png_written", ok ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
        return ok;
    }

    // Lets a run finish: the compiler, the processor's poll, the panel's next frames.
    void settleRun(tp::Processor& p, funkgui::HeadlessHost& host)
    {
        p.settle();
        host.tick(2);
        host.settle();
    }
}

PROBE(gui, dump)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    tp::Processor processor;
    tp::EditorPort& port = processor.editorPort();
    int params = 0;
    for (const tp::Control& c : port.controls())
        params += c.used ? 1 : 0;

    // FRAME, in both themes.
    for (const int theme : { 0, 1 })
    {
        const std::string tag = theme == 0 ? "graphite" : "paper";
        funkgui::UiPreferences::get().setTheme(theme);
        tp::ui::Panel panel(processor.ports(), port);
        funkgui::HeadlessHost host(panel, theme, 2.0f);
        host.setZoom({ 100, 125, 150, 200 }, 100);
        P.le(tag + ".settle_frames", host.settle(), 599.0);
        const funkgui::PrimList& frame = host.draw();
        P.eq(tag + ".missing_glyphs", static_cast<int64_t>(frame.missingGlyphs), 0);
        const tp::Draft draft = port.draft();
        P.eq(tag + ".editor_shows_draft",
             panel.editor().text() == draft.files[static_cast<std::size_t>(draft.active)].text ? 1 : 0, 1);
        P.eq(tag + ".tabs_are_files", panel.fileCount(), static_cast<int64_t>(draft.files.size()));
        int dials = 0;
        for (int c = 0; c < tp::kMaxControls; ++c)
            dials += panel.centreOfDial(c).has_value() ? 1 : 0;
        P.eq(tag + ".dials_are_params", dials, params);
        writePng(P, host, theme == 0 ? "gui.dump.png" : "gui.dump.paper.png", tag);
    }

    funkgui::UiPreferences::get().setTheme(0);
    tp::ui::Panel panel(processor.ports(), port);
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.setZoom({ 100, 125, 150, 200 }, 100);
    host.settle();
    funkgui::TextEditor& editor = panel.editor();

    // TYPE: a comment at the end, typed.
    P.eq("type.editor_has_caret", editor.focused() ? 1 : 0, 1);
    editor.setCaret(editor.endPos());
    host.keys("return,/,/,space,h,i");
    host.settle();
    const std::string typed = editor.text();
    P.eq("type.text_ends", typed.size() >= 6 && typed.substr(typed.size() - 6) == "\n// hi" ? 1 : 0, 1);
    P.eq("type.draft_follows", port.draft().files[0].text == typed ? 1 : 0, 1);
    P.eq("type.status_edited", panel.statusText().rfind("EDITED", 0) == 0 ? 1 : 0, 1);
    std::printf("status: %s\n", panel.statusText().c_str());

    // RUN
    host.keys("cmd+return");
    settleRun(processor, host);
    P.eq("run.plays_text", port.running() == tp::singleFile(typed) ? 1 : 0, 1);
    P.eq("run.status_running", panel.statusText().rfind("RUNNING", 0) == 0 ? 1 : 0, 1);
    std::printf("status: %s\n", panel.statusText().c_str());

    // ERROR: line 3 has a syntax error.
    editor.selectAll();
    editor.insert("// a broken program\n\nmodule {\n");
    host.keys("cmd+return");
    settleRun(processor, host);
    std::printf("status: %s\n", panel.statusText().c_str());
    for (const tp::ui::Problem& p : panel.problems())
        std::printf("problem %d:%d %s %s\n", p.line, p.column, p.error ? "error" : "note", p.text.c_str());
    P.eq("error.status", panel.statusText().rfind("1 ERROR", 0) == 0 ? 1 : 0, 1);
    P.eq("error.listed", panel.problems().empty() ? 0 : 1, 1);
    P.eq("error.line", panel.problems().empty() ? 0 : panel.problems()[0].line, 3);
    P.eq("error.previous_plays", port.running() == tp::singleFile(typed) ? 1 : 0, 1);
    P.eq("error.file", panel.problems().empty() ? 0 : (panel.problems()[0].file == "program.fm" ? 1 : 0), 1);
    {
        const funkgui::PrimList& frame = host.draw();
        P.ge("error.gutter_mark", countTag(frame, tp::ui::kMarkTag), 1.0);
        P.ge("error.problem_drawn", countTag(frame, tp::ui::kProblemTag), 1.0);
        P.eq("error.missing_glyphs", static_cast<int64_t>(frame.missingGlyphs), 0);
        writePng(P, host, "gui.dump.error.png", "error");
    }
    editor.setCaret({ 0, 0 });
    const funkgui::Point problem = panel.centreOfProblem(0);
    host.click(problem.x, problem.y);
    host.settle();
    P.eq("error.click_jumps", editor.caret().line, 2);

    // EXAMPLES
    const std::string before = editor.text();
    const funkgui::Point examples = panel.centreOfExamples();
    host.click(examples.x, examples.y);
    const funkgui::MenuRequest* menu = host.pendingMenu();
    P.eq("examples.menu", menu != nullptr ? 1 : 0, 1);
    if (menu != nullptr)
    {
        P.eq("examples.items", static_cast<int64_t>(menu->items.size()), static_cast<int64_t>(std::size(tp::kExamples)));
        P.eq("examples.chosen", host.chooseMenuItem(2) ? 1 : 0, 1);
        while (port.compiling())                                // the compile, without the processor's poll
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        host.tick(2);
        P.eq("examples.running_before_poll", panel.statusText().rfind("RUNNING", 0) == 0 ? 1 : 0, 1);
        const tp::CompileStatus compiled = port.status();
        int wanted = 0, shown = 0;
        for (int c = 0; c < tp::kMaxControls; ++c)
        {
            wanted += compiled.controls[static_cast<std::size_t>(c)].used ? 1 : 0;
            shown += panel.centreOfDial(c).has_value() ? 1 : 0;
        }
        P.eq("examples.dials_before_poll", shown, wanted);
        settleRun(processor, host);
        P.eq("examples.replaces_text", editor.text() == tp::kExamples[1].source ? 1 : 0, 1);
        P.eq("examples.runs_it", port.running() == tp::singleFile(tp::kExamples[1].source) ? 1 : 0, 1);
        P.eq("examples.status_running", panel.statusText().rfind("RUNNING", 0) == 0 ? 1 : 0, 1);
        writePng(P, host, "gui.dump.edited.png", "examples");
        P.eq("examples.undo_one_step", editor.undo() && editor.text() == before ? 1 : 0, 1);
        editor.redo();
        host.settle();
    }

    // DIAL: the first.
    {
        const auto at = panel.centreOfDial(0);
        P.eq("dial.found", at.has_value() ? 1 : 0, 1);
        if (at)
        {
            funkgui::ParamPort& p = *processor.ports()[0];
            p.beginGesture();
            p.setValue01(0.25f);
            p.endGesture();
            host.settle();
            host.drag(at->x, at->y, at->x, at->y - 60.0f);
            host.settle();
            P.near("dial.drag_up_60px", static_cast<double>(p.value01() - 0.25f), 0.25, 0.02);
            host.doubleClick(at->x, at->y);
            host.settle();
            const tp::Control c = processor.control(0);
            P.near("dial.double_click_default", static_cast<double>(p.value01()), static_cast<double>(tp::normOf(c, c.def)),
                   1e-6);
        }
    }

    // FILES
    {
        const std::string program = editor.text();
        const funkgui::Point plus = panel.centreOfNewTab();
        host.click(plus.x, plus.y);
        host.settle();
        P.eq("files.plus_adds", panel.fileCount(), 2);
        P.eq("files.new_shown", panel.activeFile(), 1);
        P.eq("files.new_named", panel.fileName(1) == "file2.fm" ? 1 : 0, 1);
        P.eq("files.new_renaming", panel.renamingFile() ? 1 : 0, 1);
        host.keys("p,r,o,g,r,a,m,.,f,m,return");                  // taken: refused, the edit goes on
        P.eq("files.taken_refused", panel.renamingFile() && panel.fileName(1) == "file2.fm" ? 1 : 0, 1);
        host.keys("escape");
        P.eq("files.escape_cancels", !panel.renamingFile() && panel.fileName(1) == "file2.fm" ? 1 : 0, 1);
        const funkgui::Point tab1 = panel.centreOfTab(1);
        host.doubleClick(tab1.x, tab1.y);
        P.eq("files.double_click_renames", panel.renamingFile() ? 1 : 0, 1);
        host.keys("f,i,l,t,e,r,s,.,f,m,return");
        host.settle();
        P.eq("files.renamed", panel.fileName(1) == "filters.fm" && !panel.renamingFile() ? 1 : 0, 1);

        // Text into the second file; the program includes it.
        panel.editor().insert("float quarter(float x)\n{\n    return x * 0.25f;\n}\n");
        host.tick(1);                                             // no input event: a frame's refresh sends it
        P.eq("files.typed_into_second", port.draft().files.size() == 2 && port.draft().files[0].text == program &&
                                            port.draft().files[1].text.find("quarter") != std::string::npos
                                            ? 1 : 0,
             1);
        const funkgui::Point tab0 = panel.centreOfTab(0);
        host.click(tab0.x, tab0.y);
        host.settle();
        P.eq("files.first_tab_shows_program", panel.activeFile() == 0 && panel.editor().text() == program ? 1 : 0, 1);
        panel.editor().setCaret({ 0, 0 });
        panel.editor().insert("#include \"filters.fm\"\n");
        host.keys("cmd+return");
        settleRun(processor, host);
        P.eq("files.run_plays_both", port.running().size() == 2 && port.status().ok ? 1 : 0, 1);
        P.eq("files.status_running", panel.statusText().rfind("RUNNING", 0) == 0 ? 1 : 0, 1);

        // An error in the included file.
        host.click(tab1.x, tab1.y);
        host.settle();
        panel.editor().selectAll();
        panel.editor().insert("float quarter(float x)\n{\n    return y;\n}\n");
        host.click(tab0.x, tab0.y);                               // the program shown while it runs
        host.keys("cmd+return");
        settleRun(processor, host);
        for (const tp::ui::Problem& p : panel.problems())
            std::printf("problem %s:%d:%d %s\n", p.file.c_str(), p.line, p.column, p.text.c_str());
        P.eq("files.error_listed", panel.problems().empty() ? 0 : 1, 1);
        P.eq("files.error_names_file", !panel.problems().empty() && panel.problems()[0].file == "filters.fm" &&
                                           panel.problems()[0].line == 3
                                           ? 1 : 0,
             1);
        {
            const funkgui::PrimList& frame = host.draw();
            P.eq("files.no_gutter_mark_in_program", countTag(frame, tp::ui::kMarkTag), 0);
            P.ge("files.tabs_drawn", countTag(frame, tp::ui::kTabTag), 2.0);
        }
        const funkgui::Point inFile = panel.centreOfProblem(0);
        host.click(inFile.x, inFile.y);
        host.settle();
        P.eq("files.problem_shows_file", panel.activeFile(), 1);
        P.eq("files.problem_caret_line", panel.editor().caret().line, 2);
        {
            const funkgui::PrimList& frame = host.draw();
            P.ge("files.gutter_mark_in_file", countTag(frame, tp::ui::kMarkTag), 1.0);
            P.eq("files.missing_glyphs", static_cast<int64_t>(frame.missingGlyphs), 0);
            writePng(P, host, "gui.dump.files.png", "files");
        }
        P.eq("files.draft_has_tab", port.draft().active, 1);

        // Closing: a file with text asks first.
        const funkgui::Point x1 = panel.centreOfCloseTab(1);
        host.click(x1.x, x1.y);
        P.eq("files.close_asks", host.pendingMenu() != nullptr ? 1 : 0, 1);
        P.eq("files.dismissed_stays", host.cancelMenu() && panel.fileCount() == 2 ? 1 : 0, 1);
        host.click(x1.x, x1.y);
        P.eq("files.close_chosen", host.chooseMenuItem(1) && panel.fileCount() == 1 ? 1 : 0, 1);
        host.settle();
        P.eq("files.closed_shows_before", panel.activeFile(), 0);
        P.eq("files.draft_one_file", static_cast<int64_t>(port.draft().files.size()), 1);
        // An empty file closes at once.
        host.click(plus.x, plus.y);
        host.keys("escape");
        host.settle();
        const funkgui::Point x2 = panel.centreOfCloseTab(1);
        host.click(x2.x, x2.y);
        host.settle();
        P.eq("files.empty_closes_at_once", host.pendingMenu() == nullptr && panel.fileCount() == 1 ? 1 : 0, 1);
        // The program as it was before this section, running.
        panel.editor().selectAll();
        panel.editor().insert(program);
        host.keys("cmd+return");
        settleRun(processor, host);
    }

    // KEYS
    {
        const funkgui::Point inside{ editor.bounds().x + 200.0f, editor.bounds().y + 40.0f };
        host.click(inside.x, inside.y);
        P.eq("keys.click_gives_caret", editor.focused() ? 1 : 0, 1);
        host.keys("escape");
        P.eq("keys.escape_leaves", editor.focused() ? 0 : 1, 1);
        const std::string text = editor.text();
        host.keys("tab,x");
        P.eq("keys.text_untouched", editor.text() == text ? 1 : 0, 1);
    }
    return P.finish();
}
