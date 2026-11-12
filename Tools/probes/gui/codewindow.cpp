// PROBE name=gui.codewindow timeout=240
//
// gui.codewindow: the code editor and the explorer in a window of their own (editor/Panel.h, "The code window";
// EditorPort::codeWindow), headless over a real Processor: the editor's Panel and the code window's Panel
// (Panel::Role::code) each in a HeadlessHost, the window's owner played by the probe as the plugin's editor plays it
// (editor/gpu/CodeWindow.h: the code window's Panel made when the session has the editor out, after the click that
// asked, and let go when it has it in; the JUCE window itself is not drawn here). To <build>/probe-out/:
//   gui.codewindow.png             the editor's window, the pointer over the top bar's window button (the footer's line)
//   gui.codewindow.main.png        the editor's window while the editor is out: Init's dials full size, Run alone in the
//                                  program's card's header (and .main.paper.png in PAPER)
//   gui.codewindow.code.png        the code window: the top bar, the explorer, the tabs and the editor down to the
//                                  problems, no chat and no panel (and .code.paper.png in PAPER)
//   gui.codewindow.code.small.png  the code window at its least: the explorer folded away
//   gui.codewindow.code.wide.png   the code window at 1600 x 1000
//   gui.codewindow.gui.png         a program with a .gui file, its interface full size in the editor's window
//   gui.codewindow.ir.png          the code window with the IR tab the editor had open
// and checks:
//   FIT        Columns::fit without a chat: no chat, no rail and no gap; the explorer's share; the program's card keeping
//              kProgramMinW, the explorer folding away past that
//   DEFAULT    no window button on a Panel without a window's owner (headless, a picture's); with one, the button at the
//              top bar left of the Code / Panel switch, the hand over it, a toggle to a screen reader, unchecked
//   OUT        a click asks the owner and sets the session (out); the owner's Panel then has the editor itself: the same
//              TextEditor and CodeAssist, its tabs, the tab shown, the text, the caret, the find bar and the caret's
//              focus; the editor's window shows the panel full size (the dials from the column's top), the explorer
//              and the editor away, Run alone in its card's header, the button checked, the session's view unchanged
//   CODE       the code window: no chat card, the explorer, no dials, the editor down to the problems, no Panel switch
//              in its status line, Cmd-J nothing; the button checked; its least size; a wide one
//   UNDO       Cmd-Z there undoes what was typed before the editor went out, Cmd-Shift-Z redoes it
//   SYNC       what is typed there reaches the processor's draft; Run in the editor's window and Cmd-Return there run
//              the code window's files, and its status says what the code window's does (running, edited); Claude's
//              write (a draft) reaches the code window as an undoable edit
//   SWITCH     the editor's window's Code half and the code window's Panel half ask the owner to bring the other to the
//              front, and change no view; the editor's window's Panel half changes nothing while the editor is out
//   BACK       the code window's button (or its close: the session in) brings the editor back: the same TextEditor, the
//              text typed out there and the undo steps made there (Claude's write merged there, the typing), the
//              explorer and the editor in place, the actions back; the session in
//   IR         the IR tab opened in the code window (none in the editor's window's copy) comes back with the editor
//   KEPT       the state carries the window's choice, size and place to another processor; a state from before it has
//              the editor in; sizes out of range are none; an editor that goes while the editor is out leaves the
//              session out, and the next one's owner takes the editor out again at its first look, the code window at
//              the size the session kept; while no editor has its code out (the session out, no window yet, or none
//              to have), the window picture shows the editor in place
//   PICTURE    view_panel's window picture while the editor is out: the panel full size, and its words say the code is
//              in a window of its own; the panel's picture as before; neither changes the session
//   HIDDEN     the editor's window's hidden copy of the editor while it is out: PROJECTS and EXPORT closed there give it
//              no focus, keys typed there (a letter, Cmd-Z) reach neither it nor the processor's draft, which stays the
//              code window's text, and Cmd-S there saves that text as the project's new version
//   GUI        a program with a .gui file: its interface across the editor's window's column while the editor is out
//   PANEL      the PANEL view chosen before the editor goes out is kept when it comes back, the caret then nowhere (the
//              editor is hidden): a key typed changes no file
#include "Probe.h"

#include "editor/CodeAssist.h"
#include "editor/Columns.h"
#include "editor/Panel.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"
#include "plugin/ProjectStore.h"

#include "Library.h"
#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/widgets/TextEditor.h>

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;
    namespace col = pg::ui::columns;

    void writePng(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const char* name)
    {
        const std::string path = probe::outPath(name);
        host.draw();
        P.eq(std::string(name) + ".written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
    }

    void click(funkgui::HeadlessHost& host, funkgui::Point p)
    {
        host.click(p.x, p.y);
        host.settle();
    }

    // Types `text` (no commas) as the keys it takes.
    void typeText(funkgui::HeadlessHost& host, const std::string& text)
    {
        std::string spec;
        for (const char ch : text)
            spec += (spec.empty() ? "" : ",") + (ch == ' ' ? std::string("space") : std::string(1, ch));
        host.keys(spec.c_str());
    }

    const funkgui::A11yItem* windowButton(const std::vector<funkgui::A11yItem>& items)
    {
        for (const funkgui::A11yItem& it : items)
            if (it.title == "Code editor in its own window")
                return &it;
        return nullptr;
    }

    int dialsShown(const pg::ui::Panel& panel)
    {
        int n = 0;
        for (int c = 0; c < pg::kMaxControls; ++c)
            n += panel.centreOfDial(c).has_value() ? 1 : 0;
        return n;
    }

    // What the plugin's editor does (editor/gpu/CodeWindow.h), headless: the code window's Panel in a host of its own,
    // made when the session has the editor out and let go when it has it in (follow(), which the editor runs after the
    // event that asked; here, the probe runs it).
    struct Owner
    {
        Owner(pg::Processor& p, pg::ui::Panel& m, int t = 0) : processor(p), main(m), theme(t) {}

        pg::Processor& processor;
        pg::ui::Panel& main;
        int theme = 0;
        std::unique_ptr<pg::ui::Panel> code;
        std::unique_ptr<funkgui::HeadlessHost> host;     // after code: it goes first
        int asked = 0, raisedCode = 0, raisedMain = 0;

        void hook()
        {
            pg::ui::Panel::WindowHooks h;
            h.out = [this](bool) { ++asked; };
            h.raise = [this] { ++raisedCode; };
            main.setWindowHooks(std::move(h));
        }

        void follow()
        {
            pg::EditorPort& port = processor.editorPort();
            const pg::CodeWindow cw = port.codeWindow();
            if (cw.out && code == nullptr)
            {
                code = std::make_unique<pg::ui::Panel>(processor.ports(), port, pg::ui::Panel::Role::code);
                if (cw.width > 0 && cw.height > 0)
                    code->resize(cw.width, cw.height);
                pg::ui::Panel::WindowHooks h;
                h.out = [this](bool) { ++asked; };
                h.raise = [this] { ++raisedMain; };
                code->setWindowHooks(std::move(h));
                main.lendEditor(*code);
                host = std::make_unique<funkgui::HeadlessHost>(*code, theme, 2.0f);
                host->setZoom({ 100, 125, 150, 200 }, 100);
                host->settle();
            }
            else if (!cw.out && code != nullptr)
            {
                main.takeEditorBack();
                host.reset();
                code.reset();
            }
        }
    };

    std::string guiProgram()
    {
        const std::string params = "    param float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]];\n"
                                   "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                   "    param float mix [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n";
        if constexpr (kInstrument)
            return "// Code Window: a voice with an interface\nmodule Voice\n{\n" + params
                   + "    float ph;\n    float step;\n"
                     "    void start(float n, float v) { step = 440.0f * expf(0.0577622650f * (n - 69)) / 48000; }\n"
                     "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                     "        for (int i = 0; i < n; ++i)\n        {\n"
                     "            const float s = 0.25f * level[i] * sinf(6.28318531f * ph);\n"
                     "            ph += step;\n            if (ph >= 1) ph -= 1;\n            left[i] += s;\n            right[i] += s;\n"
                     "        }\n        return true;\n    }\n};\n";
        else
            return "// Code Window: an effect with an interface\nmodule Effect\n{\n" + params
                   + "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                     "        for (int i = 0; i < n; ++i)\n        {\n            left[i] *= level[i];\n            right[i] *= level[i];\n"
                     "        }\n    }\n};\n";
    }

    const char* const kGui = R"GUI(// The probe's interface.
panel accent=orange {
    row {
        label "CODE WINDOW" size=title
    }
    row gap=10 {
        section "TONE" {
            knob cutoff size=large taper=log unit="Hz"
            knob level
        }
        section "OUT" {
            slider mix
            meter out
        }
    }
}
)GUI";
}

PROBE(gui, codewindow)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);
    // The projects (HIDDEN saves one) in a folder of the probe's own, never the user's.
    const juce::File projects = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                    .getChildFile(juce::String(pg::product::kEnvPrefix) + "gui-codewindow-"
                                                  + juce::String(juce::Time::currentTimeMillis()));
    projects.deleteRecursively();
    pg::projectstore::setDirectoryForTests(projects);
    struct Gone
    {
        juce::File dir;
        ~Gone()
        {
            pg::projectstore::setDirectoryForTests({});
            dir.deleteRecursively();
        }
    } const gone{ projects };

    // ---- FIT -----------------------------------------------------------------------------------------------------------
    {
        col::Asked a;
        a.window = 1080.0f;
        a.noChat = true;
        const col::Fit f = col::fit(a);
        P.eq("fit.no_chat", f.chat == 0.0f ? 1 : 0, 1);
        P.near("fit.explorer_share", static_cast<double>(f.explorer), 180.0, 0.5);   // a sixth of 1080, at its least
        a.window = 1600.0f;
        P.near("fit.explorer_wide", static_cast<double>(col::fit(a).explorer), 250.0, 0.5);
        a.window = 2.0f * col::kMargin + col::kProgramMinW + col::kExplorerGap + col::kExplorerMinW;
        P.near("fit.explorer_least", static_cast<double>(col::fit(a).explorer), static_cast<double>(col::kExplorerMinW), 0.5);
        a.window -= 1.0f;
        P.eq("fit.explorer_folds", col::fit(a).explorer == 0.0f ? 1 : 0, 1);
        a.window = static_cast<float>(pg::ui::kCodeMinWidth);
        P.eq("fit.least_no_explorer", col::fit(a).explorer == 0.0f && col::fit(a).chat == 0.0f ? 1 : 0, 1);
    }

    pg::Processor processor;
    pg::EditorPort& port = processor.editorPort();
    processor.settle();
    pg::ui::Panel panel(processor.ports(), port);
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.setZoom({ 100, 125, 150, 200 }, 100);
    host.settle();
    const int initDials = dialsShown(panel);
    P.ge("default.init_has_dials", initDials, 2);

    // ---- DEFAULT -------------------------------------------------------------------------------------------------------
    {
        P.eq("default.no_owner_no_button", !panel.windowButtonShown() && windowButton(host.accessibility()) == nullptr ? 1 : 0, 1);
        pg::ui::Panel picture(processor.ports(), port, true);
        funkgui::HeadlessHost pictureHost(picture, 0, 2.0f);
        pictureHost.settle();
        P.eq("default.picture_no_button", !picture.windowButtonShown() ? 1 : 0, 1);
        P.eq("default.session_in", !port.codeWindow().out ? 1 : 0, 1);
    }
    Owner owner{ processor, panel };
    owner.hook();
    host.settle();
    {
        P.eq("default.button", panel.windowButtonShown() ? 1 : 0, 1);
        const funkgui::Point b = panel.centreOfWindowButton();
        const funkgui::Point code = panel.centreOfView(false);
        P.eq("default.left_of_switch", b.x < code.x - 40.0f && b.x > code.x - 120.0f && std::fabs(b.y - code.y) < 1.0f ? 1 : 0, 1);
        const funkgui::A11yItem* it = windowButton(host.accessibility());
        P.eq("default.a11y", it != nullptr && it->role == funkgui::A11yRole::toggleButton && it->checkable && !it->checked ? 1 : 0, 1);
        host.move(b.x, b.y);
        host.settle();
        P.eq("default.hand", host.cursor() == funkgui::Cursor::pointingHand ? 1 : 0, 1);
        writePng(P, host, "gui.codewindow.png");
    }

    // ---- OUT -----------------------------------------------------------------------------------------------------------
    // Before: a word typed at the end of the program's first line (a comment), and the find bar open.
    funkgui::TextEditor* const editor = &panel.editor();
    pg::ui::CodeAssist* const assist = &panel.codeAssist();
    const std::string typed = " out";
    {
        const funkgui::Rect a = panel.editor().textArea();
        host.click(a.x + 30.0f, a.y + 4.0f);
        host.settle();
        panel.editor().setCaret({ 0, static_cast<int>(panel.editor().line(0).size()) });
        typeText(host, typed);
        host.settle();
        P.eq("out.typed", panel.editor().line(0).ends_with(typed) ? 1 : 0, 1);
    }
    const std::string before = panel.editor().text();
    const funkgui::TextPos caret = panel.editor().caret();
    const int tabs = panel.fileCount(), active = panel.activeFile();
    {
        host.keys("cmd+f");
        host.settle();
        P.eq("out.find_open", panel.codeAssist().findOpen() ? 1 : 0, 1);
        host.keys("escape");                             // the find bar closes; the editor keeps the caret
        host.settle();
        host.keys("cmd+f");
        host.settle();
        // Back to the editor (the find bar stays open): a click in its text puts the caret where it was.
        const funkgui::Rect a = panel.editor().textArea();
        host.click(a.x + 30.0f, a.y + 30.0f);
        host.settle();
        panel.editor().setCaret(caret);
        P.eq("out.caret_in_editor", panel.editor().focused() ? 1 : 0, 1);
    }
    const int askedBefore = owner.asked;
    click(host, panel.centreOfWindowButton());
    P.eq("out.asked", owner.asked, askedBefore + 1);
    P.eq("out.session_out", port.codeWindow().out ? 1 : 0, 1);
    P.eq("out.not_yet", !panel.editorOut() && !port.codeWindowOpen() ? 1 : 0, 1);   // the owner follows after the click
    owner.follow();
    host.settle();
    P.eq("out.code_window", owner.code != nullptr && panel.editorOut() && panel.peer() == owner.code.get() ? 1 : 0, 1);
    P.eq("out.port_open", port.codeWindowOpen() ? 1 : 0, 1);
    if (owner.code == nullptr)
        return P.finish();
    pg::ui::Panel& code = *owner.code;
    funkgui::HeadlessHost& codeHost = *owner.host;
    {
        P.eq("out.same_editor", &code.editor() == editor && &code.codeAssist() == assist ? 1 : 0, 1);
        P.eq("out.main_copy", &panel.editor() != editor ? 1 : 0, 1);
        P.eq("out.tabs", code.fileCount() == tabs && code.activeFile() == active ? 1 : 0, 1);
        P.eq("out.text", code.editor().text() == before && panel.editor().text() == before ? 1 : 0, 1);
        P.eq("out.caret", code.editor().caret() == caret ? 1 : 0, 1);
        P.eq("out.focus", code.editor().focused() ? 1 : 0, 1);
        P.eq("out.find_bar", code.codeAssist().findOpen() ? 1 : 0, 1);
        // The editor's window: the panel full size (the dials from the column's top), the explorer and the editor
        // away, Run alone in the program's card's header; the session's view unchanged.
        P.eq("out.main_full", panel.editorOut() && !panel.explorerShown() && dialsShown(panel) == initDials ? 1 : 0, 1);
        P.le("out.dials_from_top", static_cast<double>(panel.dialsLayout().top), 130.0);
        P.eq("out.view_kept", !panel.panelView() && !port.panelView() ? 1 : 0, 1);
        const std::vector<funkgui::A11yItem> items = host.accessibility();
        bool examples = false, run = false;
        for (const funkgui::A11yItem& it : items)
        {
            examples = examples || it.title == "Examples" || it.title == "Library" || it.title == "A random program";
            run = run || it.title == "Run the program";
        }
        P.eq("out.run_alone", run && !examples ? 1 : 0, 1);
        const funkgui::A11yItem* it = windowButton(items);
        P.eq("out.button_checked", it != nullptr && it->checked ? 1 : 0, 1);
        host.move(panel.programCard().centreX(), panel.programCard().y + 4.0f);
        host.settle();
        writePng(P, host, "gui.codewindow.main.png");
    }

    // ---- CODE ----------------------------------------------------------------------------------------------------------
    {
        P.eq("code.role", code.role() == pg::ui::Panel::Role::code ? 1 : 0, 1);
        P.eq("code.size", code.width() == pg::ui::kCodeDefaultWidth && code.height() == pg::ui::kCodeDefaultHeight ? 1 : 0, 1);
        P.eq("code.least", code.minWidth() == pg::ui::kCodeMinWidth && code.minHeight() == pg::ui::kCodeMinHeight ? 1 : 0, 1);
        P.eq("code.no_chat", code.chatCard().w <= 0.0f ? 1 : 0, 1);
        P.eq("code.explorer", code.explorerShown() ? 1 : 0, 1);
        P.eq("code.no_dials", dialsShown(code) == 0 && code.codePanelHidden() ? 1 : 0, 1);
        P.near("code.program_to_right", static_cast<double>(code.programCard().right()),
               static_cast<double>(code.width()) - static_cast<double>(col::kMargin), 0.5);
        const funkgui::Rect card = code.programCard();
        P.near("code.editor_to_problems", static_cast<double>(code.editorArea().bottom()),
               static_cast<double>(card.bottom() - 10.0f - 16.0f - 60.0f - 8.0f), 0.5);
        P.eq("code.no_panel_switch", code.codeAssist().panelSwitchRect().w <= 0.0f ? 1 : 0, 1);
        codeHost.keys("cmd+j");
        codeHost.settle();
        P.eq("code.cmd_j_nothing", dialsShown(code) == 0 && port.codePanel(false) ? 1 : 0, 1);
        const std::vector<funkgui::A11yItem> items = codeHost.accessibility();
        const funkgui::A11yItem* it = windowButton(items);
        P.eq("code.button_checked", code.windowButtonShown() && it != nullptr && it->checked ? 1 : 0, 1);
        bool chat = false;
        for (const funkgui::A11yItem& i : items)
            chat = chat || i.title == "Send or stop" || i.title == "Fold or open the chat";
        P.eq("code.no_chat_items", !chat ? 1 : 0, 1);
        codeHost.move(card.centreX(), card.y + 4.0f);
        codeHost.settle();
        writePng(P, codeHost, "gui.codewindow.code.png");
        codeHost.resize(pg::ui::kCodeMinWidth, pg::ui::kCodeMinHeight);
        codeHost.settle();
        P.eq("code.small_no_explorer", !code.explorerShown() ? 1 : 0, 1);
        P.ge("code.small_program", static_cast<double>(code.programCard().w), static_cast<double>(col::kProgramMinW));
        writePng(P, codeHost, "gui.codewindow.code.small.png");
        codeHost.resize(1600, 1000);
        codeHost.settle();
        P.eq("code.wide_explorer", code.explorerShown() ? 1 : 0, 1);
        writePng(P, codeHost, "gui.codewindow.code.wide.png");
        codeHost.resize(pg::ui::kCodeDefaultWidth, pg::ui::kCodeDefaultHeight);
        codeHost.settle();
    }

    // ---- UNDO ----------------------------------------------------------------------------------------------------------
    {
        codeHost.keys("cmd+z");
        codeHost.settle();
        P.eq("undo.before_out", !code.editor().line(0).ends_with(typed) ? 1 : 0, 1);
        codeHost.keys("cmd+shift+z");
        codeHost.settle();
        P.eq("undo.redo", code.editor().text() == before ? 1 : 0, 1);
    }

    // ---- SYNC ----------------------------------------------------------------------------------------------------------
    const std::string typed2 = " window";
    {
        code.editor().setCaret({ 0, static_cast<int>(code.editor().line(0).size()) });
        typeText(codeHost, typed2);
        codeHost.settle();
        P.eq("sync.typed", code.editor().line(0).ends_with(typed + typed2) ? 1 : 0, 1);
        const pg::Draft d = port.draft();
        P.eq("sync.draft", !d.files.empty() && d.files[0].text == code.editor().text() ? 1 : 0, 1);
        // Run in the editor's window: the code window's files.
        click(host, panel.centreOfRun());
        processor.settle();
        host.tick(2);
        codeHost.tick(2);
        host.tick(2);
        const pg::CompileStatus st = port.status();
        P.eq("sync.run_from_main", st.ok && !st.sources.empty() && st.sources[0].text == code.editor().text() ? 1 : 0, 1);
        if (!st.ok)
            std::printf("errors:\n%s\n", st.errors.c_str());
        std::printf("status: %s | %s\n", panel.statusText().c_str(), code.statusText().c_str());
        P.eq("sync.main_running", panel.statusText().rfind("RUNNING", 0) == 0 && code.statusText().rfind("RUNNING", 0) == 0
                                      ? 1 : 0, 1);
        // Claude's write (a draft): an undoable edit in the code window, and the editor's window says it is edited.
        pg::Draft w = port.draft();
        const std::string written = w.files[0].text + "// written\n";
        w.files[0].text = written;
        port.setDraft(w);                                // as a session restores it: the draft's revision moves
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        codeHost.tick(2);
        codeHost.settle();
        host.tick(2);
        P.eq("sync.write_merged", code.editor().text() == written && &code.editor() == editor ? 1 : 0, 1);
        P.eq("sync.main_edited", panel.statusText().rfind("EDITED", 0) == 0 ? 1 : 0, 1);
        codeHost.keys("cmd+z");
        codeHost.settle();
        P.eq("sync.write_undoable", code.editor().text() != written ? 1 : 0, 1);
        codeHost.keys("cmd+shift+z");
        codeHost.settle();
        // Cmd-Return in the editor's window runs it.
        const uint64_t compiles = port.statusRevision();
        host.keys("cmd+return");
        processor.settle();
        host.tick(2);
        P.eq("sync.cmd_return_main", port.statusRevision() > compiles && !port.status().sources.empty()
                                         && port.status().sources[0].text == written ? 1 : 0, 1);
    }

    // ---- SWITCH --------------------------------------------------------------------------------------------------------
    {
        const int c0 = owner.raisedCode, m0 = owner.raisedMain;
        click(host, panel.centreOfView(false));          // the editor's window's Code half
        P.eq("switch.code_raises", owner.raisedCode, c0 + 1);
        P.eq("switch.main_stays", panel.editorOut() && !panel.panelView() ? 1 : 0, 1);
        click(host, panel.centreOfView(true));           // its Panel half: nothing while the editor is out
        P.eq("switch.panel_nothing", !panel.panelView() && !port.panelView() && owner.raisedCode == c0 + 1 ? 1 : 0, 1);
        click(codeHost, code.centreOfView(true));        // the code window's Panel half
        P.eq("switch.panel_raises", owner.raisedMain, m0 + 1);
        P.eq("switch.code_stays", !code.panelView() && !port.panelView() ? 1 : 0, 1);
    }

    // ---- IR (while out) ------------------------------------------------------------------------------------------------
    {
        code.openIrView(0);
        processor.settle();
        codeHost.tick(2);
        codeHost.settle();
        P.eq("ir.opened_out", code.irOpen() && code.irShown() && !panel.irOpen() ? 1 : 0, 1);
        writePng(P, codeHost, "gui.codewindow.ir.png");
        click(codeHost, code.centreOfTab(0));            // the program shown again, the IR tab still open
        P.eq("ir.program_shown", code.irOpen() && !code.irShown() && &code.editor() == editor ? 1 : 0, 1);
    }

    // ---- BACK ----------------------------------------------------------------------------------------------------------
    {
        const std::string text = code.editor().text();
        const int asked0 = owner.asked;
        click(codeHost, code.centreOfWindowButton());
        P.eq("back.asked", owner.asked, asked0 + 1);
        P.eq("back.session_in", !port.codeWindow().out ? 1 : 0, 1);
        owner.follow();
        host.settle();
        P.eq("back.gone", owner.code == nullptr && !panel.editorOut() && panel.peer() == nullptr ? 1 : 0, 1);
        P.eq("back.port_closed", !port.codeWindowOpen() ? 1 : 0, 1);
        P.eq("back.same_editor", &panel.editor() == editor && &panel.codeAssist() == assist ? 1 : 0, 1);
        P.eq("back.text", panel.editor().text() == text ? 1 : 0, 1);
        P.eq("back.ir", panel.irOpen() && !panel.irShown() ? 1 : 0, 1);
        panel.closeIrView();
        host.settle();
        P.eq("back.in_place", panel.explorerShown() && panel.editorArea().w > 100.0f && dialsShown(panel) == initDials
                                  && panel.dialsLayout().top > 300.0f ? 1 : 0, 1);
        const std::vector<funkgui::A11yItem> items = host.accessibility();
        bool examples = false;
        for (const funkgui::A11yItem& it : items)
            examples = examples || it.title == "Examples";
        P.eq("back.actions", examples ? 1 : 0, 1);
        const funkgui::A11yItem* it = windowButton(items);
        P.eq("back.button_unchecked", it != nullptr && !it->checked ? 1 : 0, 1);
        // The undo steps made out there came back with it: Claude's write merged there goes first, then the words
        // typed (before and out there: the editor makes adjacent typing one step).
        const funkgui::Rect a = panel.editor().textArea();
        host.click(a.x + 30.0f, a.y + 30.0f);
        host.settle();
        P.eq("back.focus", panel.editor().focused() ? 1 : 0, 1);
        host.keys("cmd+z");
        host.settle();
        P.eq("back.undo_write", panel.editor().text().find("// written") == std::string::npos
                                    && panel.editor().line(0).ends_with(typed + typed2) ? 1 : 0, 1);
        host.keys("cmd+z");
        host.settle();
        P.eq("back.undo_typing", !panel.editor().line(0).ends_with(typed) && !panel.editor().line(0).ends_with(typed2) ? 1 : 0, 1);
        host.keys("cmd+shift+z");
        host.keys("cmd+shift+z");
        host.settle();
        // Out and back through the window's close (the session in, then the owner follows).
        click(host, panel.centreOfWindowButton());
        owner.follow();
        P.eq("back.out_again", owner.code != nullptr && &owner.code->editor() == editor ? 1 : 0, 1);
        if (owner.code != nullptr)
        {
            const funkgui::Rect t = owner.code->editor().textArea();
            owner.host->click(t.x + 30.0f, t.y + 30.0f);
            owner.host->settle();
        }
        pg::CodeWindow cw = port.codeWindow();
        cw.out = false;                                  // what the window's close button does
        port.setCodeWindow(cw);
        owner.follow();
        host.settle();
        P.eq("back.closed", owner.code == nullptr && !panel.editorOut() && &panel.editor() == editor ? 1 : 0, 1);
        P.eq("back.caret_follows", panel.editor().focused() ? 1 : 0, 1);
    }

    // ---- KEPT ----------------------------------------------------------------------------------------------------------
    {
        pg::CodeWindow cw;
        cw.out = true;
        cw.width = 900;
        cw.height = 640;
        cw.placed = true;
        cw.x = -1200;                                    // a display at the left
        cw.y = 140;
        port.setCodeWindow(cw);
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        pg::Processor second;
        second.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        P.eq("kept.in_session", second.editorPort().codeWindow() == cw ? 1 : 0, 1);
        pg::Processor fresh;
        juce::MemoryBlock old;
        fresh.getStateInformation(old);
        second.setStateInformation(old.getData(), static_cast<int>(old.getSize()));
        P.eq("kept.old_state_in", second.editorPort().codeWindow() == pg::CodeWindow{} ? 1 : 0, 1);
        pg::CodeWindow bad = cw;
        bad.width = 99999;
        second.editorPort().setCodeWindow(bad);
        P.eq("kept.bad_size_none", second.editorPort().codeWindow().width == 0 && second.editorPort().codeWindow().height == 0
                                       && second.editorPort().codeWindow().placed ? 1 : 0, 1);
        // An editor whose session has it out: its owner takes the editor out at its first look, at the size kept.
        {
            pg::ui::Panel other(processor.ports(), port);
            funkgui::HeadlessHost otherHost(other, 0, 2.0f);
            otherHost.settle();
            Owner o{ processor, other };
            o.hook();
            o.follow();
            otherHost.settle();
            P.eq("kept.opens", o.code != nullptr && other.editorOut() ? 1 : 0, 1);
            P.eq("kept.size", o.code != nullptr && o.code->width() == 900 && o.code->height() == 640 ? 1 : 0, 1);
            // The editor goes while it is out (a host closes its window): the code window first; the session stays out.
            o.host.reset();
            o.code.reset();
            P.eq("kept.peer_forgotten", other.peer() == nullptr ? 1 : 0, 1);
        }
        P.eq("kept.stays_out", port.codeWindow().out ? 1 : 0, 1);
        P.eq("kept.port_closed", !port.codeWindowOpen() ? 1 : 0, 1);   // the editor that had it out has gone
        // The session out, but no editor has its code out (not yet: an editor follows once it is on screen; or never,
        // without a window's owner): the window picture shows the editor in place.
        {
            const pg::PanelPicture inPlace = pg::picturePanel(processor.ports(), port, 0, 0, true, "");
            P.eq("kept.picture_in_place", !inPlace.png.empty() && inPlace.about.find("a window of its own") == std::string::npos
                                              ? 1 : 0, 1);
        }
        owner.follow();                                  // this editor's owner takes it out again
        host.settle();
        P.eq("kept.out_again", owner.code != nullptr && panel.editorOut() && &owner.code->editor() == editor ? 1 : 0, 1);
    }

    // ---- PICTURE -------------------------------------------------------------------------------------------------------
    {
        const pg::PanelPicture window = pg::picturePanel(processor.ports(), port, 0, 0, true, "");
        std::printf("picture: %d x %d, %s\n", window.width, window.height, window.about.c_str());
        P.eq("picture.window", !window.png.empty() && window.about.find("a window of its own") != std::string::npos ? 1 : 0, 1);
        const pg::PanelPicture shown = pg::picturePanel(processor.ports(), port, 0, 0, false, "");
        P.eq("picture.panel", !shown.png.empty() && shown.about.find("a window of its own") == std::string::npos ? 1 : 0, 1);
        P.eq("picture.session_kept", port.codeWindow().out && !port.panelView() && panel.editorOut() ? 1 : 0, 1);
    }

    // ---- HIDDEN --------------------------------------------------------------------------------------------------------
    // The editor's window keeps a hidden copy of the editor while the code window has it (its focus none: the lend took
    // the caret from it). A card closed there gives the copy no focus, and no key typed there reaches the copy or the
    // processor's draft; Cmd-S there saves the code window's text.
    if (owner.code != nullptr)
    {
        pg::ui::Panel& out = *owner.code;
        funkgui::HeadlessHost& outHost = *owner.host;
        const funkgui::Rect t = out.editor().textArea();
        outHost.click(t.x + 30.0f, t.y + 30.0f);
        outHost.settle();
        out.editor().setCaret({ 0, static_cast<int>(out.editor().line(0).size()) });
        typeText(outHost, " hidden");
        outHost.settle();
        const std::string text = out.editor().text();
        P.eq("hidden.synced", !port.draft().files.empty() && port.draft().files[0].text == text ? 1 : 0, 1);
        const std::string copy = panel.editor().text();
        P.eq("hidden.copy_apart", &panel.editor() != &out.editor() && !panel.editor().focused() ? 1 : 0, 1);
        // PROJECTS from the editor's window (Cmd-S, no project open), closed: the focus goes back to none.
        host.keys("cmd+s");
        host.settle();
        P.eq("hidden.projects_open", panel.projectBrowser().isOpen() ? 1 : 0, 1);
        host.keys("escape");
        host.settle();
        P.eq("hidden.projects_closed", !panel.projectBrowser().isOpen() && !panel.editor().focused() ? 1 : 0, 1);
        host.keys("x,cmd+z,y");
        host.tick(2);
        outHost.tick(2);
        host.settle();
        P.eq("hidden.copy_untouched", panel.editor().text() == copy ? 1 : 0, 1);
        P.eq("hidden.draft_kept", !port.draft().files.empty() && port.draft().files[0].text == text ? 1 : 0, 1);
        P.eq("hidden.code_untouched", out.editor().text() == text ? 1 : 0, 1);
        // A project open, more typed in the code window, then EXPORT from the editor's window, closed, and keys there:
        // Cmd-S there saves the code window's text as the project's next version.
        pg::SavedVersion saved;
        std::string why;
        P.eq("hidden.project", port.saveVersion("Code Window", saved, why) && saved.number == 1 ? 1 : 0, 1);
        typeText(outHost, " saved");
        outHost.settle();
        const std::string text2 = out.editor().text();
        panel.openExport("Code Window");
        host.settle();
        P.eq("hidden.export_open", panel.exportCard().isOpen() ? 1 : 0, 1);
        host.keys("escape");
        host.settle();
        P.eq("hidden.export_closed", !panel.exportCard().isOpen() && !panel.editor().focused() ? 1 : 0, 1);
        host.keys("z");
        host.tick(2);
        host.keys("cmd+s");
        processor.settle();                              // the save, off the message thread, lands
        host.tick(2);
        host.settle();
        const std::optional<pg::projectstore::Project> p = pg::projectstore::load("Code Window");
        P.eq("hidden.saved_v2", p.has_value() && p->latest().number == 2 ? 1 : 0, 1);
        P.eq("hidden.saved_code", p.has_value() && !p->latest().files.empty() && p->latest().files[0].text == text2 ? 1 : 0, 1);
        P.eq("hidden.draft_code", !port.draft().files.empty() && port.draft().files[0].text == text2 ? 1 : 0, 1);
        P.eq("hidden.copy_still", panel.editor().text() == copy ? 1 : 0, 1);
    }

    // ---- GUI -----------------------------------------------------------------------------------------------------------
    {
        // HIDDEN's Cmd-S left its notice (SAVED: ...) in both windows' status lines for kNoticeFor of each host's
        // clock, in place of the compile's status. How many frames the Run below takes to settle depends on how long
        // the compile keeps the compiler busy against how fast this thread ticks (a slow machine's settle ends sooner),
        // so the notice runs out first, and gui.running reads the Run's own status.
        const int noticeFrames = static_cast<int>(std::ceil(pg::ui::kNoticeFor * 60.0)) + 2;
        host.tick(noticeFrames);
        owner.host->tick(noticeFrames);
        pg::Draft d{ { { pg::kProgramName, guiProgram() }, { "panel.gui", kGui } }, 1 };
        port.setDraft(d);
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        owner.host->tick(2);
        owner.host->settle();
        P.eq("gui.files_out_there", owner.code->fileCount() == 2 && owner.code->activeFile() == 1 ? 1 : 0, 1);
        click(host, panel.centreOfRun());
        processor.settle();
        host.tick(2);
        owner.host->tick(2);                             // the code window's status, which the editor's window shows
        host.tick(2);
        host.settle();
        P.eq("gui.compiles", port.status().ok && panel.hasGui() ? 1 : 0, 1);
        std::printf("gui: status %s | the code window's %s\n", panel.statusText().c_str(),
                    owner.code != nullptr ? owner.code->statusText().c_str() : "(none)");
        P.eq("gui.running", panel.statusText().rfind("RUNNING", 0) == 0 ? 1 : 0, 1);
        if (!port.status().ok)
            std::printf("errors:\n%s\n", port.status().errors.c_str());
        P.le("gui.full_column", static_cast<double>(panel.guiLayout().area.y), 100.0);
        P.eq("gui.none_out_there", owner.code->guiLayout().area.w <= 0.0f ? 1 : 0, 1);
        writePng(P, host, "gui.codewindow.gui.png");
    }

    // ---- PANEL ---------------------------------------------------------------------------------------------------------
    {
        pg::CodeWindow cw = port.codeWindow();
        cw.out = false;
        port.setCodeWindow(cw);
        owner.follow();
        host.settle();
        click(host, panel.centreOfView(true));           // the PANEL view, chosen
        P.eq("panel.chosen", panel.panelView() && port.panelView() ? 1 : 0, 1);
        click(host, panel.centreOfWindowButton());
        owner.follow();
        host.settle();
        P.eq("panel.out", panel.editorOut() && owner.code != nullptr && owner.code->explorerShown() ? 1 : 0, 1);
        click(owner.code ? *owner.host : host, owner.code ? owner.code->centreOfWindowButton() : panel.centreOfWindowButton());
        owner.follow();
        host.settle();
        P.eq("panel.kept", !panel.editorOut() && panel.panelView() && port.panelView() ? 1 : 0, 1);
        // Out again, the caret in the code window, and back by its close: the PANEL view hides the editor, so the caret
        // goes to nothing, and a key typed changes no file.
        click(host, panel.centreOfWindowButton());
        owner.follow();
        host.settle();
        if (owner.code != nullptr)
        {
            const funkgui::Rect t = owner.code->editor().textArea();
            owner.host->click(t.x + 30.0f, t.y + 30.0f);
            owner.host->settle();
            P.eq("panel.caret_out", owner.code->editor().focused() ? 1 : 0, 1);
        }
        cw = port.codeWindow();
        cw.out = false;                                  // what the window's close button does
        port.setCodeWindow(cw);
        owner.follow();
        host.settle();
        const pg::Draft draftBefore = port.draft();
        const std::string shown = panel.editor().text();
        P.eq("panel.no_caret", !panel.editorOut() && panel.panelView() && !panel.editor().focused() ? 1 : 0, 1);
        host.keys("q");
        host.tick(2);
        host.settle();
        P.eq("panel.key_nothing", panel.editor().text() == shown && port.draft() == draftBefore ? 1 : 0, 1);
        click(host, panel.centreOfView(false));
    }

    // ---- PAPER ---------------------------------------------------------------------------------------------------------
    {
        funkgui::UiPreferences::get().setTheme(1);
        pg::Processor paper;
        paper.settle();
        pg::ui::Panel mainPanel(paper.ports(), paper.editorPort());
        funkgui::HeadlessHost mainHost(mainPanel, 1, 2.0f);
        mainHost.setZoom({ 100, 125, 150, 200 }, 100);
        Owner o{ paper, mainPanel, 1 };
        o.hook();
        mainHost.settle();
        click(mainHost, mainPanel.centreOfWindowButton());
        o.follow();
        mainHost.settle();
        P.eq("paper.out", o.code != nullptr && mainPanel.editorOut() ? 1 : 0, 1);
        mainHost.move(mainPanel.programCard().centreX(), mainPanel.programCard().y + 4.0f);
        mainHost.settle();
        writePng(P, mainHost, "gui.codewindow.main.paper.png");
        if (o.code != nullptr)
        {
            o.host->move(o.code->programCard().centreX(), o.code->programCard().y + 4.0f);
            o.host->settle();
            writePng(P, *o.host, "gui.codewindow.code.paper.png");
        }
        o.host.reset();
        o.code.reset();
        funkgui::UiPreferences::get().setTheme(0);
    }

    return P.finish();
}
