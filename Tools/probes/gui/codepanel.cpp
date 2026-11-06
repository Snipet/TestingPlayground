// PROBE name=gui.codepanel timeout=180
//
// gui.codepanel: the panel under the code (the code editor's status line's Panel switch and Cmd-J: editor/Panel.h),
// headless over a real Processor, to <build>/probe-out/:
//   gui.codepanel.png            the default size, Init's dials hidden: the editor has the column (.paper.png in PAPER)
//   gui.codepanel.hover.png      the pointer over the switch, the dials shown: its footer line
//   gui.codepanel.narrow.png     the program's card at its least: the switch its icon alone, the dials hidden
//   gui.codepanel.small.png      the least size, the dials hidden
//   gui.codepanel.wide.png       1800 x 1000, the dials hidden (.wide.paper.png in PAPER)
//   gui.codepanel.gui.png        a program with a .gui file, its code shown with the interface hidden under it
//   gui.codepanel.interface.png  ... its panel.gui shown, the interface under it (that choice's own)
//   gui.codepanel.schematic.png  a netlist's Schematic view with the panel hidden: no dials, no fold's bar
// and checks:
//   FILES      the interface's files (.gui, a shader's .sc) and the code's (.fm, .cir, a name that only ends so)
//   DEFAULT    a new instance: shown under the code and under the interface's files; the switch at the status line's
//              right end, its word beside its icon, the hand over it; a toggle button to a screen reader, checked
//   CLICK      a click on the switch hides the dials: none shown, the editor down to the problems list, the session's
//              choice for the code hidden and the interface's still shown; the footer says so; a second click brings
//              them back where they were
//   KEYS       Cmd-J from the editor (its caret and text kept) hides and shows; with nothing focused too; a screen
//              reader's press
//   SIZES      the program's card at its least: the switch's icon alone at the line's right end, the line's other
//              parts at their widest clear of the problems' count and Checking... (a click toggles); the least window;
//              a wide one: its word again
//   GUI        a program with a .gui file: the code's choice and the interface files' apart (each tab follows its own,
//              a .sc tab the interface's, the IR tab the code's); the interface hidden takes no room and no control
//              shows, shown again it is where it was
//   VIEW       the PANEL view shows the panel whatever the choices, and Cmd-J there changes nothing; CODE hides it again
//   PICTURE    view_panel's picture (the PANEL view) the same with the panel hidden under the code; the window's too is
//              drawn; neither changes a choice
//   KEPT       the state carries both choices to another processor and its Panel (laid out for the tab the session
//              shows when it attaches); a state from before them shows both;
//              a second Panel on the processor follows the first's toggle
//   SCHEMATIC  a netlist's Schematic view: hidden, no dials and no fold's bar, the drawing taller; shown, as before
#include "Probe.h"

#include "editor/ChatStyle.h"
#include "editor/CodeAssist.h"
#include "editor/GuiItems.h"
#include "editor/Items.h"
#include "editor/Panel.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"

#include "Library.h"
#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>
#include <funkgui/widgets/TextEditor.h>

#include <juce_core/juce_core.h>

#include <cstdio>
#include <string>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;
    enum Slot : int { cutoff, level, mix };

    void writePng(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const char* name)
    {
        const std::string path = probe::outPath(name);
        host.draw();
        P.eq(std::string(name) + ".written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
    }

    // The editor's files as a session restores them (the editor shows them: draftRevision bumps); load() runs them too.
    void restore(pg::Processor& p, const pg::Sources& files, int active)
    {
        p.editorPort().setDraft({ files, active });
        juce::MemoryBlock state;
        p.getStateInformation(state);
        p.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    }

    void load(pg::Processor& p, const pg::Sources& files, int active = 0)
    {
        restore(p, files, active);
        p.editorPort().run(files);
    }

    void settleRun(pg::Processor& p, funkgui::HeadlessHost& host)
    {
        p.settle();
        host.tick(2);
        host.settle();
    }

    int dialsShown(const pg::ui::Panel& panel)
    {
        int n = 0;
        for (int c = 0; c < pg::kMaxControls; ++c)
            n += panel.centreOfDial(c).has_value() ? 1 : 0;
        return n;
    }

    int guiControlsShown(const pg::ui::Panel& panel)
    {
        int n = 0;
        for (int c = 0; c < pg::kMaxControls; ++c)
            n += panel.guiControl(c).shown() ? 1 : 0;
        return n;
    }

    const funkgui::A11yItem* switchItem(const std::vector<funkgui::A11yItem>& items)
    {
        for (const funkgui::A11yItem& it : items)
            if (it.title == "Panel under the code")
                return &it;
        return nullptr;
    }

    bool same(const funkgui::Rect& a, const funkgui::Rect& b)
    {
        return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
    }

    void click(funkgui::HeadlessHost& host, funkgui::Point p)
    {
        host.click(p.x, p.y);
        host.settle();
    }

    std::string guiProgram()
    {
        const std::string params = "    param float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]];\n"
                                   "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                   "    param float mix [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n";
        if constexpr (kInstrument)
            return "// Code Panel: a voice with an interface\nmodule Voice\n{\n" + params
                   + "    float ph;\n    float step;\n"
                     "    void start(float n, float v) { step = 440.0f * expf(0.0577622650f * (n - 69)) / 48000; }\n"
                     "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                     "        for (int i = 0; i < n; ++i)\n        {\n"
                     "            const float s = 0.25f * level[i] * sinf(6.28318531f * ph);\n"
                     "            ph += step;\n            if (ph >= 1) ph -= 1;\n            left[i] += s;\n            right[i] += s;\n"
                     "        }\n        return true;\n    }\n};\n";
        else
            return "// Code Panel: an effect with an interface\nmodule Effect\n{\n" + params
                   + "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                     "        for (int i = 0; i < n; ++i)\n        {\n            left[i] *= level[i];\n            right[i] *= level[i];\n"
                     "        }\n    }\n};\n";
    }

    const char* const kGui = R"GUI(// The probe's interface.
panel accent=orange {
    row {
        label "CODE PANEL" size=title
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

PROBE(gui, codepanel)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);

    // ---- FILES ---------------------------------------------------------------------------------------------------------
    P.eq("files.interface", pg::ui::isInterfaceFile("panel.gui") && pg::ui::isInterfaceFile("glow.sc")
                                && pg::ui::isInterfaceFile("my panel.gui") ? 1 : 0, 1);
    P.eq("files.code", !pg::ui::isInterfaceFile("program.fm") && !pg::ui::isInterfaceFile("amp.cir")
                           && !pg::ui::isInterfaceFile("gui.fm") && !pg::ui::isInterfaceFile("panel.gui.fm")
                           && !pg::ui::isInterfaceFile(".gui") && !pg::ui::isInterfaceFile(".sc")
                           && !pg::ui::isInterfaceFile("sc") ? 1 : 0, 1);

    pg::Processor processor;
    pg::EditorPort& port = processor.editorPort();
    processor.settle();
    pg::ui::Panel panel(processor.ports(), port);
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.setZoom({ 100, 125, 150, 200 }, 100);
    host.settle();
    const int initDials = dialsShown(panel);         // Init's
    P.ge("default.init_has_dials", initDials, 2);

    // ---- DEFAULT -------------------------------------------------------------------------------------------------------
    const funkgui::Rect ed0 = panel.editorArea();
    funkgui::Point sw = panel.centreOfPanelSwitch();
    {
        P.eq("default.both_shown", port.codePanel(false) && port.codePanel(true) && panel.codePanelShown()
                                       && !panel.codePanelHidden() ? 1 : 0, 1);
        // The switch: at the right end of the status line under the editor, its word beside its icon.
        const funkgui::Rect r = panel.codeAssist().panelSwitchRect();
        P.eq("default.in_status_line", r.bottom() <= ed0.bottom() + 0.5f && r.y >= ed0.bottom() - 22.5f && r.w > 0.0f ? 1 : 0, 1);
        P.eq("default.labelled", panel.panelSwitchLabelled() ? 1 : 0, 1);
        P.ge("default.at_right_end", static_cast<double>(r.right()), static_cast<double>(ed0.right() - 9.0f));
        host.move(sw.x, sw.y);
        P.eq("default.hand", host.cursor() == funkgui::Cursor::pointingHand ? 1 : 0, 1);
        const std::vector<funkgui::A11yItem> items = host.accessibility();
        const funkgui::A11yItem* it = switchItem(items);
        P.eq("default.a11y", it != nullptr && it->role == funkgui::A11yRole::toggleButton && it->checkable && it->checked
                                 ? 1 : 0, 1);
        char line[256] = {};
        panel.codeAssist().specLine(line, sizeof line, sw);
        std::printf("footer: %s\n", line);
        P.eq("default.footer", std::string(line).rfind("PANEL   SHOWN UNDER THE EDITOR   A CLICK (OR CMD-J) HIDES IT", 0) == 0 ? 1 : 0, 1);
        host.settle();
        writePng(P, host, "gui.codepanel.hover.png");
    }

    // ---- CLICK ---------------------------------------------------------------------------------------------------------
    {
        const std::string text = panel.editor().text();
        click(host, sw);
        P.eq("click.hidden", panel.codePanelHidden() && !panel.codePanelShown() ? 1 : 0, 1);
        P.eq("click.no_dials", dialsShown(panel), 0);
        P.eq("click.session_code_hidden", !port.codePanel(false) ? 1 : 0, 1);
        P.eq("click.session_interface_kept", port.codePanel(true) ? 1 : 0, 1);
        // The editor down to the problems list, which stays at the column's bottom (its four rows, 16 px under them;
        // the card's inset 10, the editor 8 over them).
        const funkgui::Rect card = panel.programCard();
        P.near("click.editor_to_problems", static_cast<double>(panel.editorArea().bottom()),
               static_cast<double>(card.bottom() - 10.0f - 16.0f - 60.0f - 8.0f), 0.5);
        P.ge("click.editor_taller", static_cast<double>(panel.editorArea().h), static_cast<double>(ed0.h + 60.0f));
        P.eq("click.text_kept", panel.editor().text() == text ? 1 : 0, 1);
        const funkgui::A11yItem* it = switchItem(host.accessibility());
        P.eq("click.a11y_unchecked", it != nullptr && !it->checked ? 1 : 0, 1);
        sw = panel.centreOfPanelSwitch();
        P.near("click.switch_follows", static_cast<double>(sw.y), static_cast<double>(panel.editorArea().bottom() - 11.0f), 0.5);
        char line[256] = {};
        panel.codeAssist().specLine(line, sizeof line, sw);
        P.eq("click.footer", std::string(line).rfind("PANEL   HIDDEN   A CLICK (OR CMD-J) SHOWS IT", 0) == 0 ? 1 : 0, 1);
        host.move(card.centreX(), card.y + 4.0f);
        host.settle();
        writePng(P, host, "gui.codepanel.png");
        {
            funkgui::UiPreferences::get().setTheme(1);
            pg::ui::Panel paper(processor.ports(), port);
            funkgui::HeadlessHost paperHost(paper, 1, 2.0f);
            paperHost.settle();
            P.eq("click.paper_hidden", paper.codePanelHidden() && dialsShown(paper) == 0 ? 1 : 0, 1);
            writePng(P, paperHost, "gui.codepanel.paper.png");
            funkgui::UiPreferences::get().setTheme(0);
        }
        host.tick(2);
        click(host, sw);
        P.eq("click.shown_again", !panel.codePanelHidden() && port.codePanel(false) && dialsShown(panel) == initDials ? 1 : 0, 1);
        P.near("click.editor_back", static_cast<double>(panel.editorArea().h), static_cast<double>(ed0.h), 0.01);
        sw = panel.centreOfPanelSwitch();
    }

    // ---- KEYS ----------------------------------------------------------------------------------------------------------
    {
        const funkgui::Rect a = panel.editor().textArea();
        host.click(a.x + 30.0f, a.y + 4.0f);
        host.settle();
        const std::string text = panel.editor().text();
        host.keys("cmd+j");
        host.settle();
        P.eq("keys.cmd_j_hides", panel.codePanelHidden() && !port.codePanel(false) && dialsShown(panel) == 0 ? 1 : 0, 1);
        P.eq("keys.caret_kept", panel.editor().focused() && panel.editor().text() == text ? 1 : 0, 1);
        host.keys("cmd+j");
        host.settle();
        P.eq("keys.cmd_j_shows", !panel.codePanelHidden() && port.codePanel(false) && dialsShown(panel) == initDials ? 1 : 0, 1);
        host.keys("escape");
        host.keys("cmd+j");
        host.settle();
        P.eq("keys.unfocused", panel.codePanelHidden() ? 1 : 0, 1);
        // A screen reader's press on the switch.
        const funkgui::A11yItem* it = switchItem(host.accessibility());
        P.eq("keys.a11y_found", it != nullptr ? 1 : 0, 1);
        if (it != nullptr)
        {
            panel.a11yAction(it->id, funkgui::A11yAction::press);
            host.settle();
        }
        P.eq("keys.a11y_press", !panel.codePanelHidden() && port.codePanel(false) ? 1 : 0, 1);
    }

    // ---- SIZES ---------------------------------------------------------------------------------------------------------
    {
        // The program's card at its least (the chat as wide as it may be): the switch's icon alone.
        port.setChatWidth(900);
        host.tick(2);
        host.settle();
        std::printf("program's card %.0f px, status line %.0f px\n", static_cast<double>(panel.programCard().w),
                    static_cast<double>(panel.codeAssist().statusRect().w));
        P.near("sizes.program_least", static_cast<double>(panel.programCard().w), 600.0, 0.5);
        const funkgui::Rect r = panel.codeAssist().panelSwitchRect();
        P.eq("sizes.narrow_icon_only", !panel.panelSwitchLabelled() ? 1 : 0, 1);
        P.le("sizes.narrow_width", static_cast<double>(r.w), 30.0);
        P.ge("sizes.narrow_at_right_end", static_cast<double>(r.right()), static_cast<double>(panel.editorArea().right() - 9.0f));
        // The line's parts from its right end, at their widest (a selection's count, the IR's name, a long place), leave
        // the problems' count and Checking... their 160 px at its left.
        {
            const funkgui::TextStyle& st = pg::ui::chat::kSmall;          // the status line's
            const auto w = [&](const char* t) {
                return funkgui::text::width(funkgui::FontService::get().atlas(), t, st) + 16.0f;
            };
            const float right = r.w + w("Format on Run") + 14.0f + w("LLVM IR") + w("Spaces: 4")
                              + w("Ln 9999, Col 99") + funkgui::text::width(funkgui::FontService::get().atlas(), " (999 selected)", st);
            const funkgui::Rect line = panel.codeAssist().statusRect();
            std::printf("status line %.0f px: its right parts %.0f px\n", static_cast<double>(line.w), static_cast<double>(right));
            P.ge("sizes.narrow_problems_room", static_cast<double>(line.w - 16.0f - right), 160.0);
        }
        click(host, panel.centreOfPanelSwitch());
        P.eq("sizes.narrow_click", panel.codePanelHidden() && dialsShown(panel) == 0 ? 1 : 0, 1);
        host.move(panel.programCard().centreX(), panel.programCard().y + 4.0f);
        host.settle();
        writePng(P, host, "gui.codepanel.narrow.png");
        port.setChatWidth(0);
        host.tick(2);
        // The least window, hidden.
        host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight);
        host.settle();
        P.eq("sizes.least_hidden", panel.codePanelHidden() && dialsShown(panel) == 0 ? 1 : 0, 1);
        writePng(P, host, "gui.codepanel.small.png");
        host.resize(1800, 1000);
        host.settle();
        P.eq("sizes.wide_labelled", panel.panelSwitchLabelled() && panel.codePanelHidden() && dialsShown(panel) == 0 ? 1 : 0, 1);
        writePng(P, host, "gui.codepanel.wide.png");
        {
            funkgui::UiPreferences::get().setTheme(1);
            pg::ui::Panel paper(processor.ports(), port);
            funkgui::HeadlessHost paperHost(paper, 1, 2.0f);
            paperHost.resize(1800, 1000);
            paperHost.settle();
            writePng(P, paperHost, "gui.codepanel.wide.paper.png");
            funkgui::UiPreferences::get().setTheme(0);
        }
        host.tick(2);
        click(host, panel.centreOfPanelSwitch());
        host.resize(pg::ui::kDefaultWidth, pg::ui::kDefaultHeight);
        host.settle();
        P.eq("sizes.shown_again", !panel.codePanelHidden() && dialsShown(panel) == initDials ? 1 : 0, 1);
    }

    // ---- GUI -----------------------------------------------------------------------------------------------------------
    {
        load(processor, { { pg::kProgramName, guiProgram() }, { "panel.gui", kGui } });
        settleRun(processor, host);
        P.eq("gui.compiles", port.status().ok && panel.hasGui() ? 1 : 0, 1);
        if (!port.status().ok)
            std::printf("errors:\n%s\n", port.status().errors.c_str());
        P.eq("gui.program_shown", panel.activeFile() == 0 && !panel.codePanelHidden() ? 1 : 0, 1);
        const funkgui::Rect area = panel.guiLayout().area;
        const funkgui::Rect ed = panel.editorArea();
        P.eq("gui.interface_under", area.w > 0.0f && area.y > ed.bottom() && guiControlsShown(panel) == 3 ? 1 : 0, 1);
        // The program's code: the interface hidden, no room and no control of it.
        host.keys("cmd+j");
        host.settle();
        P.eq("gui.code_hidden", panel.codePanelHidden() && !port.codePanel(false) && port.codePanel(true) ? 1 : 0, 1);
        P.eq("gui.no_room", panel.guiLayout().area.w == 0.0f && guiControlsShown(panel) == 0 ? 1 : 0, 1);
        P.ge("gui.editor_taller", static_cast<double>(panel.editorArea().h), static_cast<double>(ed.h + area.h));
        writePng(P, host, "gui.codepanel.gui.png");
        // Its panel.gui: the interface files' own choice, shown.
        click(host, panel.centreOfTab(1));
        P.eq("gui.interface_tab", panel.activeFile() == 1 && panel.codePanelShown() && !panel.codePanelHidden() ? 1 : 0, 1);
        P.eq("gui.interface_back", same(panel.guiLayout().area, area) && guiControlsShown(panel) == 3 ? 1 : 0, 1);
        P.near("gui.editor_as_before", static_cast<double>(panel.editorArea().h), static_cast<double>(ed.h), 0.01);
        writePng(P, host, "gui.codepanel.interface.png");
        host.keys("cmd+j");
        host.settle();
        P.eq("gui.interface_hidden", panel.codePanelHidden() && !port.codePanel(true) ? 1 : 0, 1);
        // The program's code shown again: its choice alone changes.
        click(host, panel.centreOfTab(0));
        host.keys("cmd+j");
        host.settle();
        P.eq("gui.code_shown", !panel.codePanelHidden() && port.codePanel(false) && !port.codePanel(true) ? 1 : 0, 1);
        click(host, panel.centreOfTab(1));
        P.eq("gui.interface_still_hidden", panel.codePanelHidden() ? 1 : 0, 1);
        // A shader's file (.sc) follows the interface's choice, the IR tab the code's.
        restore(processor, { { pg::kProgramName, guiProgram() }, { "panel.gui", kGui }, { "glow.sc", "// a shader\n" } }, 1);
        host.tick(2);
        host.settle();
        P.eq("gui.sc_tab", panel.fileCount() == 3 && panel.fileName(2) == "glow.sc" ? 1 : 0, 1);
        click(host, panel.centreOfTab(2));
        P.eq("gui.sc_follows_interface", panel.activeFile() == 2 && panel.codePanelHidden() ? 1 : 0, 1);
        panel.openIrView(0);
        host.settle();
        P.eq("gui.ir_follows_code", panel.irShown() && !panel.codePanelHidden() ? 1 : 0, 1);
        panel.closeIrView();
        host.settle();
        click(host, panel.centreOfTab(0));
        P.eq("gui.back_to_code", panel.activeFile() == 0 && !panel.codePanelHidden() ? 1 : 0, 1);
    }

    // ---- VIEW and PICTURE ----------------------------------------------------------------------------------------------
    {
        // view_panel's picture with the panel shown under the code, then with both choices hidden.
        const pg::PanelPicture shown = pg::picturePanel(processor.ports(), port, 0, 0, false, "");
        host.keys("cmd+j");
        host.settle();
        P.eq("view.both_hidden", !port.codePanel(false) && !port.codePanel(true) && panel.codePanelHidden() ? 1 : 0, 1);
        const pg::PanelPicture hidden = pg::picturePanel(processor.ports(), port, 0, 0, false, "");
        std::printf("picture: %d x %d, %s\n", hidden.width, hidden.height, hidden.about.c_str());
        P.eq("picture.drawn", !hidden.png.empty() && hidden.about.find("from panel.gui") != std::string::npos ? 1 : 0, 1);
        P.eq("picture.unaffected", shown.width == hidden.width && shown.height == hidden.height && shown.about == hidden.about
                                       ? 1 : 0, 1);
        const pg::PanelPicture window = pg::picturePanel(processor.ports(), port, 0, 0, true, "");
        P.eq("picture.window_drawn", !window.png.empty() ? 1 : 0, 1);
        P.eq("picture.choices_kept", !port.codePanel(false) && !port.codePanel(true) && !port.panelView() ? 1 : 0, 1);
        host.tick(2);
        P.eq("picture.panel_kept", panel.codePanelHidden() ? 1 : 0, 1);
        // The PANEL view shows the panel whatever the choices; Cmd-J there changes nothing.
        click(host, panel.centreOfView(true));
        P.eq("view.panel_shows", panel.panelView() && !panel.codePanelHidden() && guiControlsShown(panel) == 3
                                     && panel.guiLayout().area.y <= 100.0f ? 1 : 0, 1);
        host.keys("cmd+j");
        host.settle();
        P.eq("view.cmd_j_nothing", panel.panelView() && !port.codePanel(false) && !port.codePanel(true) ? 1 : 0, 1);
        click(host, panel.centreOfView(false));
        P.eq("view.code_hides_again", !panel.panelView() && panel.codePanelHidden() && guiControlsShown(panel) == 0 ? 1 : 0, 1);
    }

    // ---- KEPT ----------------------------------------------------------------------------------------------------------
    {
        click(host, panel.centreOfTab(1));
        host.keys("cmd+j");                              // the interface's files: shown; the code's stays hidden
        host.settle();
        P.eq("kept.set", !port.codePanel(false) && port.codePanel(true) ? 1 : 0, 1);
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        pg::Processor second;
        second.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        P.eq("kept.in_session", !second.editorPort().codePanel(false) && second.editorPort().codePanel(true) ? 1 : 0, 1);
        {
            second.settle();
            pg::ui::Panel other(second.ports(), second.editorPort());
            funkgui::HeadlessHost otherHost(other, 0, 2.0f);
            // Laid out at attach for the tab the session shows (panel.gui: the interface's choice), before any frame.
            P.eq("kept.at_attach", other.activeFile() == 1 && !other.codePanelHidden() ? 1 : 0, 1);
            otherHost.settle();
            P.eq("kept.panel_follows", other.activeFile() == 1 && !other.codePanelHidden() ? 1 : 0, 1);
            click(otherHost, other.centreOfTab(0));
            P.eq("kept.code_hidden", other.activeFile() == 0 && other.codePanelHidden() ? 1 : 0, 1);
        }
        // A state from before the choices (none in it): both shown.
        pg::Processor fresh;
        juce::MemoryBlock before;
        fresh.getStateInformation(before);
        second.setStateInformation(before.getData(), static_cast<int>(before.getSize()));
        P.eq("kept.old_state_shows", second.editorPort().codePanel(false) && second.editorPort().codePanel(true) ? 1 : 0, 1);
        // A second Panel on this processor follows the first's toggle.
        pg::ui::Panel two(processor.ports(), port);
        funkgui::HeadlessHost twoHost(two, 0, 2.0f);
        twoHost.settle();
        click(twoHost, two.centreOfTab(0));
        P.eq("kept.second_panel", two.codePanelHidden() ? 1 : 0, 1);
        click(host, panel.centreOfTab(0));
        host.keys("cmd+j");
        host.settle();
        twoHost.tick(2);
        P.eq("kept.second_follows", !panel.codePanelHidden() && !two.codePanelHidden() && port.codePanel(false) ? 1 : 0, 1);
    }

    // ---- SCHEMATIC -----------------------------------------------------------------------------------------------------
    {
        const pg::LibraryFile* tone = pg::findLibraryFile("diodetone.cir");
        P.eq("schematic.netlist", tone != nullptr ? 1 : 0, 1);
        if (tone != nullptr)
        {
            load(processor, { { pg::kProgramName, guiProgram() }, { "tone.cir", tone->source } }, 1);
            settleRun(processor, host);
            if (panel.activeFile() != 1)
                click(host, panel.centreOfTab(1));
            panel.setSchematic(true);
            host.settle();
            P.eq("schematic.shown", panel.schematicShown() && !panel.codePanelHidden() && dialsShown(panel) == 3 ? 1 : 0, 1);
            const funkgui::Rect ed = panel.editorArea();
            host.keys("cmd+j");
            host.settle();
            P.eq("schematic.hidden", panel.schematicShown() && panel.codePanelHidden() && dialsShown(panel) == 0
                                         && !panel.dialsBarShown() && !port.codePanel(false) ? 1 : 0, 1);
            P.near("schematic.drawing_to_bottom", static_cast<double>(panel.editorArea().bottom()),
                   static_cast<double>(panel.programCard().bottom() - 10.0f), 0.5);
            P.ge("schematic.taller", static_cast<double>(panel.editorArea().h), static_cast<double>(ed.h + 40.0f));
            writePng(P, host, "gui.codepanel.schematic.png");
            host.keys("cmd+j");
            host.settle();
            P.eq("schematic.back", !panel.codePanelHidden() && dialsShown(panel) == 3 && same(panel.editorArea(), ed) ? 1 : 0, 1);
        }
    }

    return P.finish();
}
