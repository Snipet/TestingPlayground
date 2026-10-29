// PROBE name=gui.ir timeout=300
//
// gui.ir: the IR view (editor/IrView.h; dsp/Compiler.h, "The IR"), headless over a real Processor, driven as a user
// would. It writes, to <build>/probe-out/:
//   gui.ir.png          GRAPHITE: the program's optimised IR in its tab, the explorer's LLVM IR rows, the status line
//   gui.ir.lowered.png  the lowered view, the find bar over it
//   gui.ir.frozen.png   the frozen copy's, while it plays
//   gui.ir.paper.png    PAPER: the optimised view
//   gui.ir.tabs.png     the least window, twelve files and a long program name: the IR tab beside the narrowed tabs
// and checks:
//   NOBODY    with no IR tab open nothing is kept: a RUN keeps no text (the compiler wants none, holds none), and
//             Patch::compile keeps none unless asked (asked, both texts, with the program's process function)
//   HIGHLIGHT LLVM IR's numbers as numbers: decimal, a float's bits as LLVM 23 prints them (f0x3D6C9820), -inf,
//             +qnan, +nan(0x...); a %name with f0x in it a name
//   SAME      the IR alone (Patch::ir, Patch::frozenIr: what the compiler's thread makes for a program compiled
//             without it) is the text the program's own compile kept, both texts, and the frozen copy's freeze kept
//   OPEN      the explorer's LLVM IR section: its caption and a row a view (optimised, lowered; frozen only while a
//             copy plays); a click on the optimised row opens the tab, shown, apart from the files' at the bar's right
//             (none of the tabs, the draft's files the same; to a screen reader the tab selected, not the file under
//             it), named <program>.ll; the processor now wants the IR and
//             the compiler's thread makes it for the program that plays, with no RUN: the text holds the define of
//             playground_process, the appended call Patch compiles
//   RUN       a RUN of a changed program (a function of its own added) changes the text: the IR's revision moves, the
//             new function's define in the lowered view, the caret's line kept
//   READONLY  the view is read only: typed characters, Space, Return, Backspace, Delete, a paste, Cmd-Z, Cmd-/,
//             Alt-Down, Shift-Cmd-K, Cmd-] and Shift-Alt-F change nothing (its text and revision stay) and are taken,
//             never the host's; the status says READ ONLY, whole in the card's header; the files' texts are untouched
//   LANGUAGE  no completion (a typed name, Ctrl-Space), no signature, no hover card, no squiggle; the status line says
//             LLVM IR and read only
//   FIND      Cmd-F and `define`: its matches counted and marked; Return goes on; Cmd-Alt-F opens no replace field;
//             Escape closes the bar, no match left marked
//   GOTO      Ctrl-G and a line's number, Return: the caret on that line, the bar closed; a line past the end is
//             refused (the bar stays); Escape closes it; in a file's tab too
//   FAILED    a RUN that does not compile: the view says so, and keeps the IR of the program that plays
//   CUT       a text past Compiler::kIrMaxBytes is cut at a line's end, its whole size and lines kept; the view says
//             so at its top and its end; the editor takes the cut text on the message thread in well under a second
//   NOTFILE   the IR is none of the program's files: not in the draft, the session's state, a project's saved files
//             or its source export; an #include of its name finds no file
//   SWITCH    a file's tab shows the file (the IR tab stays open; to a screen reader the file's tab selected); the IR
//             tab's click shows it again, its caret kept; the find bar's replace row, open in the file's tab, closed in
//             the IR's
//   FROZEN    the lowered view shown with a selection, blocks played with the dials still: the frozen copy plays and
//             its IR is kept, the lowered view's text, selection and scroll untouched (nothing of it changed); its row
//             shows, its view holds the copy's IR (its define of playground_process) and says it plays; a dial moved:
//             the view says the copy stopped, a comment line more, the selection on the same text of LLVM's
//   CLOSE     the tab's close box: the file under it shown again, the processor wants nothing, the compiler holds
//             nothing; a RUN then keeps nothing; a Panel that goes with its IR tab open leaves nothing wanted
//   TABS      the least window, twelve files, a long program name, the lowered view: the files' tabs end before the IR
//             tab, which keeps at least 64 px inside the card; a click on the last file's tab and on the IR tab each
//             does its own; the read-only notice whole in the card's header
#include "Probe.h"

#include "dsp/Compiler.h"
#include "editor/CodeAssist.h"
#include "editor/Explorer.h"
#include "editor/IrView.h"
#include "editor/Panel.h"
#include "fminus/Patch.h"
#include "plugin/EditorPort.h"
#include "plugin/ProjectStore.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/widgets/TextEditor.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace pg = playground;

namespace
{
    using S = pg::ui::ExplorerItem::Section;

    void writePng(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const char* name, const std::string& key)
    {
        const std::string path = probe::outPath(name);
        host.draw();
        P.eq(key + ".png_written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
    }

    // Ticks (and lets the compiler's threads work) until `done`, ten seconds at most.
    bool waitFor(funkgui::HeadlessHost& host, const std::function<bool()>& done)
    {
        for (int k = 0; k < 2000; ++k)
        {
            host.tick(1);
            if (done())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    void settleRun(pg::Processor& p, funkgui::HeadlessHost& host)
    {
        p.settle();
        host.tick(2);
        host.settle();
    }

    void clickAt(funkgui::HeadlessHost& host, const std::optional<funkgui::Point>& p)
    {
        if (p)
            host.click(p->x, p->y);
        host.settle();
    }

    // The editor has the keys (a click in its text), the caret where it was.
    void focusEditor(pg::ui::Panel& panel, funkgui::HeadlessHost& host)
    {
        funkgui::TextEditor& ed = panel.editor();
        const funkgui::TextPos caret = ed.caret();
        const funkgui::Rect a = ed.textArea();
        host.click(a.x + 30.0f, a.y + 4.0f);
        panel.editor().setCaret(caret);
    }

    // The explorer's row in `s` labelled `label`, or -1.
    int rowOf(const pg::ui::ExplorerItem& ex, S s, const std::string& label)
    {
        for (int i = 0; i < ex.rows(s); ++i)
            if (const pg::ui::ExplorerItem::Row* r = ex.row(s, i); r != nullptr && !r->heading && r->label == label)
                return i;
        return -1;
    }

    // A line of `text` that defines `function` (an @name followed by its parenthesis).
    bool defines(const std::string& text, const std::string& function)
    {
        const std::string name = "@" + function + "(";
        for (std::size_t at = text.find(name); at != std::string::npos; at = text.find(name, at + 1))
        {
            const std::size_t start = text.rfind('\n', at);
            const std::size_t line = start == std::string::npos ? 0 : start + 1;
            if (text.compare(line, 7, "define ") == 0)
                return true;
        }
        return false;
    }

    bool kept(const pg::IrStatus& ir) { return ir.lowered || ir.optimised || ir.frozen; }

    // The Files group's radio buttons checked, by title (a screen reader's "selected").
    std::vector<std::string> checkedTabs(funkgui::HeadlessHost& host)
    {
        std::vector<std::string> out;
        for (const funkgui::A11yItem& it : host.accessibility())
            if (it.role == funkgui::A11yRole::radioButton && it.checkable && it.checked)
                out.push_back(it.title);
        return out;
    }

    // The style IrHighlighter gives `word` in `line` (one span from its start to its end), or -1.
    int styleOf(const std::string& line, const std::string& word)
    {
        const std::size_t at = line.find(word);
        if (at == std::string::npos)
            return -1;
        std::vector<funkgui::text::Span> spans;
        pg::ui::irview::IrHighlighter().line(line, 0, spans);
        for (const funkgui::text::Span& sp : spans)
            if (sp.start == static_cast<int>(at) && sp.end == static_cast<int>(at + word.size()))
                return sp.style;
        return -1;
    }

    // The program's text typed into its tab, as a user would: its tab shown, the text replaced, and the IR tab shown
    // again when it was.
    void setProgram(pg::ui::Panel& panel, funkgui::HeadlessHost& host, const std::string& text)
    {
        const bool ir = panel.irShown();
        const funkgui::Point tab0 = panel.centreOfTab(0);
        host.click(tab0.x, tab0.y);
        host.settle();
        panel.editor().selectAll();
        panel.editor().insert(text);
        host.tick(2);
        if (ir)
        {
            const funkgui::Point t = panel.centreOfIrTab();
            host.click(t.x, t.y);
            host.settle();
        }
    }

    // A program of the product's kind: a filter with two dials, and `extra` before its module (a function of its own).
    std::string program(const std::string& extra = {})
    {
        if constexpr (pg::product::kIsInstrument)
            return "// IR probe: a saw through a one-pole low-pass.\n" + extra +
                   "module Voice\n"
                   "{\n"
                   "    param float cutoff [[fminus::range(0, 1)]] [[fminus::default(0.3)]];\n"
                   "    param float gain [[fminus::range(0, 2)]] [[fminus::default(1)]];\n"
                   "    float rate;\n"
                   "    float phase;\n"
                   "    float step;\n"
                   "    float level;\n"
                   "    float y;\n"
                   "\n"
                   "    void init(float r)\n"
                   "    {\n"
                   "        rate = r;\n"
                   "    }\n"
                   "\n"
                   "    void start(float note, float velocity)\n"
                   "    {\n"
                   "        step = 440 * expf(0.0577622650f * (note - 69)) / rate;\n"
                   "        level = velocity;\n"
                   "    }\n"
                   "\n"
                   "    bool process(float* restrict left, float* restrict right, int n)\n"
                   "    {\n"
                   "        for (int i = 0; i < n; ++i)\n"
                   "        {\n"
                   "            phase += step;\n"
                   "            if (phase >= 1)\n"
                   "                phase -= 1;\n"
                   "            y += cutoff[i] * ((2 * phase - 1) - y);\n"
                   "            const float out = y * gain[i] * level * 0.25f;\n"
                   "            left[i] += out;\n"
                   "            right[i] += out;\n"
                   "        }\n"
                   "        return true;\n"
                   "    }\n"
                   "};\n";
        else
            return "// IR probe: a one-pole low-pass.\n" + extra +
                   "module Effect\n"
                   "{\n"
                   "    param float cutoff [[fminus::range(0, 1)]] [[fminus::default(0.3)]];\n"
                   "    param float gain [[fminus::range(0, 2)]] [[fminus::default(1)]];\n"
                   "    float y;\n"
                   "\n"
                   "    void process(float* restrict left, float* restrict right, int n)\n"
                   "    {\n"
                   "        for (int i = 0; i < n; ++i)\n"
                   "        {\n"
                   "            y += cutoff[i] * (left[i] - y);\n"
                   "            left[i] = y * gain[i];\n"
                   "            right[i] = y * gain[i];\n"
                   "        }\n"
                   "    }\n"
                   "};\n";
    }

    // Keys that would edit a text, each sent as the panel gets it; true when every one was taken.
    bool sendEdits(pg::ui::Panel& panel, funkgui::HeadlessHost& host)
    {
        bool taken = true;
        const auto press = [&](funkgui::Key k, char32_t ch, funkgui::Mods m = {}) {
            funkgui::KeyEvent e;
            e.key = k;
            e.ch = ch;
            e.mods = m;
            taken = panel.key(e) && taken;
        };
        funkgui::Mods cmd;
        cmd.cmd = true;
        funkgui::Mods alt;
        alt.alt = true;
        funkgui::Mods shiftCmd = cmd;
        shiftCmd.shift = true;
        funkgui::Mods shiftAlt = alt;
        shiftAlt.shift = true;
        press(funkgui::Key::character, U'x');
        press(funkgui::Key::character, U'{');
        press(funkgui::Key::space, U' ');
        press(funkgui::Key::enter, 0);
        press(funkgui::Key::backspace, 0);
        press(funkgui::Key::del, 0);
        press(funkgui::Key::character, U'v', cmd);   // paste
        press(funkgui::Key::character, U'x', cmd);   // cut
        press(funkgui::Key::character, U'z', cmd);   // undo
        press(funkgui::Key::character, U'/', cmd);   // comment
        press(funkgui::Key::down, 0, alt);           // move the line
        press(funkgui::Key::character, U'k', shiftCmd);   // delete the line
        press(funkgui::Key::character, U']', cmd);   // indent
        press(funkgui::Key::character, U'F', shiftAlt);   // Format
        host.tick(1);
        return taken;
    }
}

PROBE(gui, ir)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);
    const juce::File folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                  .getChildFile(juce::String(pg::product::kEnvPrefix) + "gui-ir-" + juce::String(juce::Time::currentTimeMillis()));
    folder.deleteRecursively();
    pg::projectstore::setDirectoryForTests(folder.getChildFile("projects"));

    pg::Processor processor;
    processor.compiler().setFreezing(false);             // the frozen copy only where FROZEN asks for it
    pg::EditorPort& port = processor.editorPort();
    pg::Draft d;
    d.files = { { "program.fm", program() } };
    port.setDraft(d);

    // ---- NOBODY ------------------------------------------------------------------------------------------------------
    {
        port.run(d.files);
        processor.settle();
        const pg::IrStatus ir = processor.compiler().ir();
        P.eq("nobody.compiled", port.status().ok ? 1 : 0, 1);
        P.eq("nobody.not_wanted", ir.wanted ? 0 : 1, 1);
        P.eq("nobody.nothing_kept", kept(ir) ? 0 : 1, 1);
        const pg::Patch::Result plain = pg::Patch::compile(d.files, 48000.0f);
        P.eq("nobody.patch_keeps_none", plain.patch && plain.ir.lowered.empty() && plain.ir.optimised.empty() ? 1 : 0, 1);
        const pg::Patch::Result asked = pg::Patch::compile(d.files, 48000.0f, true);
        P.eq("nobody.patch_asked_keeps", asked.patch && defines(asked.ir.lowered, "playground_process")
                                             && defines(asked.ir.optimised, "playground_process") ? 1 : 0, 1);
        std::printf("IR of the probe's program: lowered %zu bytes, optimised %zu bytes\n", asked.ir.lowered.size(),
                    asked.ir.optimised.size());

        // ---- SAME ------------------------------------------------------------------------------------------------------
        if (asked.patch)
        {
            const pg::Patch::Recipe recipe = asked.patch->recipe();
            pg::Patch::Ir alone;
            std::string why;
            P.eq("same.ir_alone_compiles", pg::Patch::ir(recipe, alone, why) ? 1 : 0, 1);
            P.eq("same.lowered", alone.lowered == asked.ir.lowered ? 1 : 0, 1);
            P.eq("same.optimised", alone.optimised == asked.ir.optimised ? 1 : 0, 1);
            std::array<float, pg::kMaxControls> values{};
            values[0] = 0.4f;
            values[1] = 1.5f;
            std::string fromFreeze, fromIr;
            const std::unique_ptr<pg::Patch::Frozen> f = pg::Patch::freeze(recipe, values, why, false, &fromFreeze);
            P.eq("same.frozen_compiles", f != nullptr && pg::Patch::frozenIr(recipe, values, fromIr, why) ? 1 : 0, 1);
            P.eq("same.frozen", !fromIr.empty() && fromIr == fromFreeze && defines(fromIr, "playground_process") ? 1 : 0, 1);
            P.eq("same.frozen_differs", fromIr != asked.ir.optimised ? 1 : 0, 1);
            P.eq("same.freeze_without_ir", pg::Patch::freeze(recipe, values, why) != nullptr ? 1 : 0, 1);
        }

        // ---- HIGHLIGHT -------------------------------------------------------------------------------------------------
        {
            P.eq("highlight.decimal", styleOf("  %0 = fadd float %note, -6.900000e+01", "-6.900000e+01"), pg::ui::csNumber);
            P.eq("highlight.hex_float", styleOf("  %1 = fmul float %0, f0x3D6C9820", "f0x3D6C9820"), pg::ui::csNumber);
            P.eq("highlight.hex_double", styleOf("  %2 = fadd double %1, f0x3FB999999999999A", "f0x3FB999999999999A"),
                 pg::ui::csNumber);
            P.eq("highlight.inf", styleOf("  %3 = fcmp oeq float %2, -inf", "-inf"), pg::ui::csNumber);
            P.eq("highlight.qnan", styleOf("  %4 = select i1 %3, float +qnan, float %2", "+qnan"), pg::ui::csNumber);
            P.eq("highlight.nan_payload", styleOf("  %5 = fadd float %4, +nan(0x200000)", "+nan(0x200000)"), pg::ui::csNumber);
            P.eq("highlight.instruction", styleOf("  %1 = fmul float %0, f0x3D6C9820", "fmul"), pg::ui::csKeyword);
            P.eq("highlight.local", styleOf("  %1 = fmul float %0, f0x3D6C9820", "%1"), pg::ui::csMember);
            P.eq("highlight.word_not_hex", styleOf("  %fx = load float, ptr %f0xg", "%f0xg"), pg::ui::csMember);
        }

        // ---- CUT -------------------------------------------------------------------------------------------------------
        std::string big;
        while (big.size() < 3 * pg::Compiler::kIrMaxBytes && !asked.ir.optimised.empty())
            big += asked.ir.optimised;
        const std::shared_ptr<const pg::IrText> t = pg::Compiler::irText(big);
        P.eq("cut.made", t != nullptr ? 1 : 0, 1);
        if (t)
        {
            P.le("cut.bounded", static_cast<double>(t->text.size()), static_cast<double>(pg::Compiler::kIrMaxBytes));
            P.ge("cut.most_kept", static_cast<double>(t->text.size()), 0.99 * static_cast<double>(pg::Compiler::kIrMaxBytes));
            P.eq("cut.at_line_end", !t->text.empty() && t->text.back() == '\n' ? 1 : 0, 1);
            P.eq("cut.whole_known", t->bytes == big.size() && t->cut()
                                        && t->lines == static_cast<std::size_t>(std::count(big.begin(), big.end(), '\n'))
                                        ? 1 : 0, 1);
            P.eq("cut.small_whole", !pg::Compiler::irText(asked.ir.optimised)->cut() ? 1 : 0, 1);
            pg::IrStatus cut;
            cut.program = 1;
            cut.optimised = t;
            pg::CompileStatus st;
            st.revision = 1;
            st.request = 1;
            st.ok = true;
            pg::ui::irview::Source from;
            from.ir = &cut;
            from.status = &st;
            const std::string text = pg::ui::irview::text(pg::ui::irview::View::optimised, "program.fm", from);
            P.eq("cut.said_at_top", text.find("; Cut: the IR is ") < 600 ? 1 : 0, 1);
            P.eq("cut.said_at_end", text.size() > 12 && text.compare(text.size() - 12, 12, " more lines\n") == 0 ? 1 : 0, 1);
            funkgui::TextEditor ed;
            pg::ui::irview::configure(ed, funkgui::Theme::graphite());
            ed.setBounds({ 0.0f, 0.0f, 800.0f, 500.0f });
            const auto t0 = std::chrono::steady_clock::now();
            ed.setText(text);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::printf("a cut text of %zu bytes, %d lines, set in the view in %.1f ms\n", text.size(), ed.lineCount(), ms);
            P.le("cut.set_ms", ms, 1000.0);
        }
    }

    {
        pg::ui::Panel panel(processor.ports(), port);
        funkgui::HeadlessHost host(panel, 0, 2.0f);
        host.setZoom({ 100, 125, 150, 200 }, 100);
        host.settle();
        pg::ui::ExplorerItem& ex = panel.explorer();
        pg::ui::CodeAssist& code = panel.codeAssist();
        const std::string optimisedName = pg::ui::irview::name(pg::ui::irview::View::optimised, "program.fm");
        const std::string loweredName = pg::ui::irview::name(pg::ui::irview::View::lowered, "program.fm");
        const std::string frozenName = pg::ui::irview::name(pg::ui::irview::View::frozen, "program.fm");

        // ---- OPEN ------------------------------------------------------------------------------------------------
        {
            P.eq("open.names", optimisedName == "program.ll" && loweredName == "program.lowered.ll"
                                   && frozenName == "program.frozen.ll" ? 1 : 0, 1);
            const pg::ui::ExplorerItem::Row* caption = ex.row(S::ir, 0);
            P.eq("open.caption", caption != nullptr && caption->heading && caption->label.find("READ ONLY") != std::string::npos
                                     ? 1 : 0, 1);
            P.eq("open.rows", ex.rows(S::ir), 3);         // the caption, optimised, lowered: no frozen copy plays
            P.eq("open.optimised_row", rowOf(ex, S::ir, optimisedName), 1);
            P.eq("open.lowered_row", rowOf(ex, S::ir, loweredName), 2);
            P.eq("open.no_frozen_row", rowOf(ex, S::ir, frozenName), -1);
            P.eq("open.not_wanted_yet", processor.compiler().ir().wanted ? 0 : 1, 1);
            const int files = panel.fileCount();
            clickAt(host, ex.centreOfRow(S::ir, 1));
            P.eq("open.tab_open", panel.irOpen() && panel.irShown() ? 1 : 0, 1);
            P.eq("open.view", panel.irView(), static_cast<int64_t>(pg::ui::irview::View::optimised));
            P.eq("open.tab_name", panel.irName() == optimisedName ? 1 : 0, 1);
            P.eq("open.not_a_tab", panel.fileCount(), files);
            P.eq("open.draft_same", port.draft().files == d.files ? 1 : 0, 1);
            P.eq("open.row_marked", ex.row(S::ir, 1) != nullptr && ex.row(S::ir, 1)->active
                                        && !ex.row(S::files, 0)->active ? 1 : 0, 1);
            const funkgui::Point tab = panel.centreOfIrTab();
            P.ge("open.tab_at_right", static_cast<double>(tab.x), static_cast<double>(panel.centreOfNewTab().x) + 40.0);
            // To a screen reader the IR tab is the one selected among the tabs, not the file under it.
            const std::vector<std::string> checked = checkedTabs(host);
            P.eq("open.a11y_ir_selected", checked.size() == 1 && checked[0] == optimisedName ? 1 : 0, 1);
            P.eq("open.wanted", processor.compiler().ir().wanted ? 1 : 0, 1);
            const bool made = waitFor(host, [&] {
                return defines(panel.editor().text(), "playground_process");
            });
            P.eq("open.made_without_run", made ? 1 : 0, 1);
            const std::string text = panel.editor().text();
            P.eq("open.says_what", text.rfind("; program.ll: ", 0) == 0 ? 1 : 0, 1);
            P.eq("open.target", text.find("target triple = ") != std::string::npos ? 1 : 0, 1);
            P.eq("open.read_only", panel.editor().options().readOnly ? 1 : 0, 1);
            P.eq("open.highlighter", dynamic_cast<const pg::ui::irview::IrHighlighter*>(panel.editor().highlighter()) != nullptr
                                         ? 1 : 0, 1);
            const pg::IrStatus ir = processor.compiler().ir();
            P.eq("open.both_kept", ir.lowered && ir.optimised && ir.program == port.status().request ? 1 : 0, 1);
            P.eq("open.not_cut", ir.optimised && !ir.optimised->cut() ? 1 : 0, 1);
            std::printf("the optimised view: %d lines\n", panel.editor().lineCount());
            host.move(10.0f, 10.0f);
            host.settle();
            writePng(P, host, "gui.ir.png", "open");
        }

        // ---- READONLY ------------------------------------------------------------------------------------------------
        {
            focusEditor(panel, host);
            funkgui::TextEditor& ed = panel.editor();
            const std::string text = ed.text();
            const uint32_t revision = ed.revision();
            host.setClipboard("pasted text\n");
            ed.setCaret({ 12, 2 });
            P.eq("readonly.focused", ed.focused() ? 1 : 0, 1);
            P.eq("readonly.taken", sendEdits(panel, host) ? 1 : 0, 1);
            P.eq("readonly.text_same", panel.editor().text() == text ? 1 : 0, 1);
            P.eq("readonly.revision_same", static_cast<int64_t>(panel.editor().revision()), static_cast<int64_t>(revision));
            P.eq("readonly.says_so", panel.statusText().find("READ ONLY") != std::string::npos ? 1 : 0, 1);
            P.eq("readonly.said_whole", panel.statusWhole() ? 1 : 0, 1);
            P.eq("readonly.files_same", port.draft().files == d.files ? 1 : 0, 1);
            panel.editor().selectAll();
            P.eq("readonly.insert_refused", panel.editor().insert("x") ? 0 : 1, 1);
            panel.editor().setCaret({ 0, 0 });
        }

        // ---- LANGUAGE ------------------------------------------------------------------------------------------------
        {
            funkgui::TextEditor& ed = panel.editor();
            ed.setCaret({ 20, 0 });
            host.keys("ctrl+space");
            P.eq("language.no_completion", code.completionOpen() ? 0 : 1, 1);
            P.eq("language.no_signature", code.signatureOpen() ? 0 : 1, 1);
            // The pointer resting on a word of the text: no card.
            const funkgui::Point at = ed.pointOf({ 22, 4 });
            host.move(at.x + 3.0f, at.y + 4.0f);
            host.tick(60);
            P.eq("language.no_hover", code.hoverOpen() ? 0 : 1, 1);
            int squiggles = 0;
            for (const funkgui::Decoration& dec : ed.decorations())
                squiggles += dec.kind == funkgui::DecorationKind::error || dec.kind == funkgui::DecorationKind::warning ? 1 : 0;
            P.eq("language.no_squiggles", squiggles, 0);
            char line[256] = {};
            const funkgui::Rect sr = code.statusRect();
            const bool spoke = code.specLine(line, sizeof line, { sr.x + 20.0f, sr.centreY() });
            P.eq("language.status_llvm_ir_read_only", spoke && std::string(line).rfind("LLVM IR   READ ONLY", 0) == 0 ? 1 : 0, 1);
            host.move(10.0f, 10.0f);
            host.settle();
        }

        // ---- RUN -----------------------------------------------------------------------------------------------------
        {
            const std::string before = panel.editor().text();
            const uint64_t revision = processor.compiler().irRevision();
            panel.editor().setCaret({ 30, 0 });
            setProgram(panel, host, program("float irprobe_twice(float x)\n{\n    return x + x;\n}\n\n"));
            P.eq("run.ir_still_shown", panel.irShown() ? 1 : 0, 1);
            panel.runNow();
            settleRun(processor, host);
            const bool changedText = waitFor(host, [&] { return panel.editor().text() != before; });
            P.eq("run.compiled", port.status().ok ? 1 : 0, 1);
            P.eq("run.text_changed", changedText ? 1 : 0, 1);
            P.ge("run.revision_moved", static_cast<double>(processor.compiler().irRevision()), static_cast<double>(revision) + 1.0);
            P.eq("run.caret_kept", panel.editor().caret().line, 30);
            P.eq("run.optimised_process", defines(panel.editor().text(), "playground_process") ? 1 : 0, 1);
            P.eq("run.kept_by_the_run", processor.compiler().ir().program == port.status().request ? 1 : 0, 1);
            clickAt(host, ex.centreOfRow(S::ir, rowOf(ex, S::ir, loweredName)));
            P.eq("run.lowered_view", panel.irView() == static_cast<int>(pg::ui::irview::View::lowered)
                                         && panel.irName() == loweredName ? 1 : 0, 1);
            const bool lowered = waitFor(host, [&] { return defines(panel.editor().text(), "irprobe_twice"); });
            P.eq("run.lowered_has_new_function", lowered ? 1 : 0, 1);
            P.eq("run.lowered_has_process", defines(panel.editor().text(), "playground_process") ? 1 : 0, 1);
            P.eq("run.lowered_row_marked", ex.row(S::ir, rowOf(ex, S::ir, loweredName))->active ? 1 : 0, 1);
            P.eq("run.lowered_differs", panel.editor().text() != before ? 1 : 0, 1);
        }

        // ---- FIND ----------------------------------------------------------------------------------------------------
        {
            focusEditor(panel, host);
            panel.editor().setCaret({ 0, 0 });
            host.keys("cmd+f");
            for (const char ch : std::string("define"))
            {
                funkgui::KeyEvent e;
                e.key = funkgui::Key::character;
                e.ch = static_cast<char32_t>(ch);
                panel.key(e);
            }
            host.tick(2);
            P.eq("find.open", code.findOpen() ? 1 : 0, 1);
            P.ge("find.matches", code.findCount(), 2.0);
            P.eq("find.current", code.findCurrent(), 0);
            host.keys("return");
            P.eq("find.next", code.findCurrent(), 1);
            int marked = 0;
            for (const funkgui::Decoration& dec : panel.editor().decorations())
                marked += dec.kind == funkgui::DecorationKind::findMatch || dec.kind == funkgui::DecorationKind::findCurrent ? 1 : 0;
            P.eq("find.marked", marked, static_cast<int64_t>(code.findCount()));
            const std::string text = panel.editor().text();
            host.keys("cmd+alt+f");
            P.eq("find.no_replace", panel.editor().text() == text ? 1 : 0, 1);
            host.move(10.0f, 10.0f);
            host.tick(2);
            writePng(P, host, "gui.ir.lowered.png", "lowered");
            host.keys("escape");
            P.eq("find.closed", code.findOpen() ? 0 : 1, 1);
            marked = 0;
            for (const funkgui::Decoration& dec : panel.editor().decorations())
                marked += dec.kind == funkgui::DecorationKind::findMatch || dec.kind == funkgui::DecorationKind::findCurrent ? 1 : 0;
            P.eq("find.unmarked", marked, 0);
        }

        // ---- GOTO ----------------------------------------------------------------------------------------------------
        {
            focusEditor(panel, host);
            host.keys("ctrl+g");
            P.eq("goto.open", code.goToOpen() ? 1 : 0, 1);
            host.keys("4,2,return");
            P.eq("goto.line", panel.editor().caret().line, 41);
            P.eq("goto.closed", code.goToOpen() ? 0 : 1, 1);
            P.eq("goto.editor_has_keys", panel.editor().focused() ? 1 : 0, 1);
            host.keys("ctrl+g,9,9,9,9,9,9,return");
            P.eq("goto.past_end_refused", code.goToOpen() && panel.editor().caret().line == 41 ? 1 : 0, 1);
            host.keys("escape");
            P.eq("goto.escape_closes", code.goToOpen() ? 0 : 1, 1);
            P.eq("goto.text_same", panel.editor().options().readOnly ? 1 : 0, 1);
        }

        // ---- SWITCH --------------------------------------------------------------------------------------------------
        {
            const funkgui::TextPos caret = panel.editor().caret();
            const funkgui::Point tab0 = panel.centreOfTab(0);
            host.click(tab0.x, tab0.y);
            host.settle();
            P.eq("switch.file_shown", !panel.irShown() && panel.irOpen() && panel.activeFile() == 0
                                          && panel.editor().text() == port.draft().files[0].text ? 1 : 0, 1);
            P.eq("switch.file_writable", panel.editor().options().readOnly ? 0 : 1, 1);
            focusEditor(panel, host);
            host.keys("ctrl+g,3,return");
            P.eq("switch.goto_in_file", panel.editor().caret().line, 2);
            P.eq("switch.file_row_marked", ex.row(S::files, 0)->active && !ex.row(S::ir, rowOf(ex, S::ir, loweredName))->active
                                               ? 1 : 0, 1);
            const std::vector<std::string> checked = checkedTabs(host);
            P.eq("switch.a11y_file_selected", checked.size() == 1 && checked[0] == "program.fm" ? 1 : 0, 1);
            // The find bar's replace row, open in the file's tab, is not in the IR's: nothing to replace there.
            host.keys("cmd+alt+f");
            P.eq("switch.replace_in_file", code.findOpen() && code.replaceOpen() ? 1 : 0, 1);
            const funkgui::Point irTab = panel.centreOfIrTab();
            host.click(irTab.x, irTab.y);
            host.settle();
            P.eq("switch.ir_again", panel.irShown() && panel.irView() == static_cast<int>(pg::ui::irview::View::lowered) ? 1 : 0, 1);
            P.eq("switch.no_replace_in_ir", code.findOpen() && !code.replaceOpen() ? 1 : 0, 1);
            host.keys("escape");
            P.eq("switch.find_closed", code.findOpen() ? 0 : 1, 1);
            P.eq("switch.caret_kept", panel.editor().caret() == caret ? 1 : 0, 1);
        }

        // ---- FAILED --------------------------------------------------------------------------------------------------
        {
            const pg::IrStatus before = processor.compiler().ir();
            setProgram(panel, host, port.draft().files[0].text + "this does not compile\n");
            panel.runNow();
            settleRun(processor, host);
            host.tick(4);
            P.eq("failed.did_not_compile", port.status().ok ? 0 : 1, 1);
            const std::string text = panel.editor().text();
            P.eq("failed.says_so", text.find("The last RUN did not compile") != std::string::npos ? 1 : 0, 1);
            P.eq("failed.keeps_playing_ir", defines(text, "irprobe_twice") && processor.compiler().ir().program == before.program
                                                ? 1 : 0, 1);
            // An #include of the view's name finds no file: the view is none of them.
            setProgram(panel, host, "#include \"" + optimisedName + "\"\n" + program());
            panel.runNow();
            settleRun(processor, host);
            P.eq("notfile.include_not_found", !port.status().ok
                                                  && port.status().errors.find("'" + optimisedName + "' file not found")
                                                         != std::string::npos ? 1 : 0, 1);
            // Back to the program, compiled.
            setProgram(panel, host, program("float irprobe_twice(float x)\n{\n    return x + x;\n}\n\n"));
            panel.runNow();
            settleRun(processor, host);
            P.eq("failed.compiles_again", port.status().ok ? 1 : 0, 1);
            waitFor(host, [&] { return panel.editor().text().find("did not compile") == std::string::npos; });
            P.eq("failed.no_longer_said", panel.editor().text().find("did not compile") == std::string::npos ? 1 : 0, 1);
        }

        // ---- NOTFILE -------------------------------------------------------------------------------------------------
        {
            const pg::Draft now = port.draft();
            P.eq("notfile.draft", now.files.size() == 1 && now.files[0].name == "program.fm" ? 1 : 0, 1);
            juce::MemoryBlock state;
            processor.getStateInformation(state);
            const std::string session = state.toString().toStdString();
            P.eq("notfile.session", session.find("source_filename") == std::string::npos
                                        && session.find(".ll") == std::string::npos ? 1 : 0, 1);
            std::string why;
            pg::SavedVersion saved;
            P.eq("notfile.project_saved", port.saveVersion("IR Probe", saved, why) ? 1 : 0, 1);
            bool projectClean = false;
            for (const juce::File& f : folder.getChildFile("projects").findChildFiles(juce::File::findFiles, true, "*"))
            {
                const std::string t = f.loadFileAsString().toStdString();
                projectClean = t.find("program.fm") != std::string::npos && t.find("source_filename") == std::string::npos
                            && t.find(optimisedName) == std::string::npos;
                if (projectClean)
                    break;
            }
            P.eq("notfile.project_files", projectClean ? 1 : 0, 1);
            const juce::File zipFile = folder.getChildFile("IR Probe.zip");
            pg::Exported ex2;
            P.eq("notfile.exported", port.exportProject("IR Probe", false, true, zipFile.getFullPathName().toStdString(), ex2, why)
                                         ? 1 : 0, 1);
            juce::ZipFile zip(zipFile);
            bool exportClean = zip.getNumEntries() > 0;
            for (int i = 0; i < zip.getNumEntries(); ++i)
            {
                const juce::ZipFile::ZipEntry* entry = zip.getEntry(i);
                const std::unique_ptr<juce::InputStream> in(zip.createStreamForEntry(i));
                const std::string t = in != nullptr ? in->readEntireStreamAsString().toStdString() : std::string();
                exportClean = exportClean && !entry->filename.endsWith(".ll") && t.find("source_filename") == std::string::npos;
            }
            P.eq("notfile.export", exportClean ? 1 : 0, 1);
        }

        // ---- FROZEN --------------------------------------------------------------------------------------------------
        {
            constexpr bool kInstrument = pg::product::kIsInstrument;
            constexpr double kRate = 48000.0;
            constexpr int kBlock = 256;
            juce::AudioBuffer<float> buffer(2, kBlock);
            double phase = 0.0;
            const auto play = [&](int k) {
                juce::MidiBuffer midi;
                if (kInstrument && k == 0)
                    midi.addEvent(juce::MidiMessage::noteOn(1, 57, static_cast<juce::uint8>(100)), 0);
                for (int i = 0; i < kBlock; ++i)
                {
                    const float v = kInstrument ? 0.0f : static_cast<float>(0.25 * std::sin(phase));
                    phase += 2.0 * 3.14159265358979 * 440.0 / kRate;
                    buffer.setSample(0, i, v);
                    buffer.setSample(1, i, v);
                }
                processor.processBlock(buffer, midi);
            };
            // The program's view shown, a selection in it: the frozen copy coming (its IR kept, the IR's revision
            // moving) leaves its text, its selection and its scroll alone, as nothing of it changed.
            const bool loweredShown = panel.irShown() && panel.irView() == static_cast<int>(pg::ui::irview::View::lowered);
            P.eq("frozen.lowered_shown", loweredShown ? 1 : 0, 1);
            panel.editor().select({ 14, 2 }, { 16, 5 });
            const std::string selected = panel.editor().selectedText();
            const uint32_t textRevision = panel.editor().revision();
            const uint64_t irRevision = processor.compiler().irRevision();
            processor.compiler().setFreezing(true, 0.25);
            processor.prepareToPlay(kRate, kBlock);
            for (int k = 0; k < 400 && !processor.engine().frozen(); ++k)
            {
                play(k);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            P.eq("frozen.plays", processor.engine().frozen() ? 1 : 0, 1);
            const bool held = waitFor(host, [&] { return processor.compiler().ir().frozen != nullptr; });
            host.tick(2);
            host.settle();
            P.eq("frozen.ir_kept", held && processor.compiler().irRevision() > irRevision ? 1 : 0, 1);
            P.eq("frozen.program_view_untouched", static_cast<int64_t>(panel.editor().revision()), static_cast<int64_t>(textRevision));
            P.eq("frozen.selection_kept", !selected.empty() && panel.editor().selectedText() == selected
                                              && panel.editor().selectionStart() == funkgui::TextPos{ 14, 2 } ? 1 : 0, 1);
            const int row = rowOf(ex, S::ir, frozenName);
            P.eq("frozen.row_shown", row, 3);
            clickAt(host, ex.centreOfRow(S::ir, row));
            P.eq("frozen.view", panel.irView() == static_cast<int>(pg::ui::irview::View::frozen) && panel.irName() == frozenName
                                    ? 1 : 0, 1);
            const bool made = waitFor(host, [&] { return defines(panel.editor().text(), "playground_process"); });
            P.eq("frozen.text", made ? 1 : 0, 1);
            const std::string text = panel.editor().text();
            P.eq("frozen.says_plays", text.find("It plays now.") != std::string::npos ? 1 : 0, 1);
            P.eq("frozen.says_dials", text.find("Its dials, as constants: cutoff ") != std::string::npos ? 1 : 0, 1);
            const pg::IrStatus ir = processor.compiler().ir();
            P.eq("frozen.held", ir.frozen && ir.frozenProgram == ir.program ? 1 : 0, 1);
            host.move(10.0f, 10.0f);
            host.settle();
            writePng(P, host, "gui.ir.frozen.png", "frozen");
            // A dial moves: the copy stops playing and the view says so in a comment line more, its text made again;
            // the selection stays on the same text of LLVM's, moved down with it.
            {
                funkgui::TextEditor& fe = panel.editor();
                const int body = [&] {
                    for (int l = 0; l < fe.lineCount(); ++l)
                        if (fe.line(l).rfind("; ModuleID", 0) == 0)
                            return l;
                    return -1;
                }();
                fe.select({ body + 6, 0 }, { body + 7, 4 });
                const std::string before = fe.selectedText();
                const uint32_t revision = fe.revision();
                processor.compiler().setFreezing(false);
                processor.getParameters()[0]->setValueNotifyingHost(0.7f);
                play(1);
                const bool stopped = waitFor(host, [&] { return panel.editor().revision() != revision; });
                P.eq("frozen.stopped", stopped && !processor.engine().frozen() ? 1 : 0, 1);
                P.eq("frozen.says_stopped", panel.editor().text().find("It does not play now") != std::string::npos ? 1 : 0, 1);
                P.eq("frozen.selection_follows", body > 0 && !before.empty() && panel.editor().selectedText() == before ? 1 : 0, 1);
            }
            processor.releaseResources();
        }

        // ---- PAPER ---------------------------------------------------------------------------------------------------
        {
            clickAt(host, ex.centreOfRow(S::ir, rowOf(ex, S::ir, optimisedName)));
            funkgui::UiPreferences::get().setTheme(1);
            pg::ui::Panel paper(processor.ports(), port);
            funkgui::HeadlessHost paperHost(paper, 1, 2.0f);
            paperHost.setZoom({ 100, 125, 150, 200 }, 100);
            paperHost.settle();
            P.eq("paper.no_tab_of_its_own", paper.irOpen() ? 0 : 1, 1);   // the IR tab is no part of the session
            paper.openIrView(static_cast<int>(pg::ui::irview::View::optimised));
            const bool made = waitFor(paperHost, [&] { return defines(paper.editor().text(), "playground_process"); });
            P.eq("paper.text", made ? 1 : 0, 1);
            paperHost.draw();
            const funkgui::Col comment = paper.editor().palette()[pg::ui::csComment].colour;
            const funkgui::Col graphite = pg::ui::codePalette(funkgui::Theme::graphite())[pg::ui::csComment].colour;
            P.eq("paper.palette", comment.r != graphite.r || comment.g != graphite.g ? 1 : 0, 1);
            paperHost.move(10.0f, 10.0f);
            paperHost.settle();
            writePng(P, paperHost, "gui.ir.paper.png", "paper");
            funkgui::UiPreferences::get().setTheme(0);
        }
        // Two Panels looked; the PAPER one went with its tab open: this one still looks.
        P.eq("close.other_panel_gone", processor.compiler().ir().wanted ? 1 : 0, 1);

        // ---- CLOSE ---------------------------------------------------------------------------------------------------
        {
            const funkgui::Point x = panel.centreOfIrClose();
            host.click(x.x, x.y);
            host.settle();
            P.eq("close.tab_gone", panel.irOpen() ? 0 : 1, 1);
            P.eq("close.file_shown", panel.activeFile() == 0 && panel.editor().text() == port.draft().files[0].text ? 1 : 0, 1);
            const pg::IrStatus ir = processor.compiler().ir();
            P.eq("close.not_wanted", ir.wanted ? 0 : 1, 1);
            P.eq("close.nothing_held", kept(ir) ? 0 : 1, 1);
            P.eq("close.rows_left", ex.rows(S::ir), port.frozen() ? 4 : 3);   // the frozen copy's row while it plays
            panel.runNow();
            settleRun(processor, host);
            P.eq("close.run_keeps_nothing", port.status().ok && !kept(processor.compiler().ir()) ? 1 : 0, 1);
            // Open again, and the Panel goes with it open: nothing is wanted after.
            clickAt(host, ex.centreOfRow(S::ir, 1));
            P.eq("close.reopened", panel.irShown() && processor.compiler().ir().wanted ? 1 : 0, 1);
        }
    }
    P.eq("close.panel_gone", processor.compiler().ir().wanted ? 0 : 1, 1);
    P.eq("close.panel_gone_nothing_held", kept(processor.compiler().ir()) ? 0 : 1, 1);

    // ---- TABS --------------------------------------------------------------------------------------------------------
    // The least window, twelve files and a long program name: the IR tab takes no more than the files' tabs leave at
    // their narrowest, so none runs under it, and the read-only notice fits the card's header.
    {
        pg::Draft many;
        many.files.push_back({ "a_long_named_program_file.fm", program() });
        for (int k = 1; k < static_cast<int>(pg::kMaxFiles); ++k)
            many.files.push_back({ "file" + std::to_string(k) + ".fm", "// file " + std::to_string(k) + "\n" });
        port.setDraft(many);
        pg::ui::Panel panel(processor.ports(), port);
        funkgui::HeadlessHost host(panel, 0, 2.0f);
        host.setZoom({ 100, 125, 150, 200 }, 100);
        P.eq("tabs.least_size", host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight) ? 1 : 0, 1);
        host.settle();
        P.eq("tabs.twelve", panel.fileCount(), static_cast<int64_t>(pg::kMaxFiles));
        panel.openIrView(static_cast<int>(pg::ui::irview::View::lowered));
        host.settle();
        const funkgui::Rect irTab = panel.irTabRect();
        float filesRight = 0.0f;
        for (int i = 0; i < panel.fileCount(); ++i)
            filesRight = std::max(filesRight, panel.fileTabRect(i).right());
        std::printf("the least window: the files' tabs end at %.1f, the IR tab spans %.1f to %.1f\n", filesRight, irTab.x,
                    irTab.right());
        P.eq("tabs.ir_open", panel.irShown() && panel.irName() == "a_long_named_program_file.lowered.ll" ? 1 : 0, 1);
        P.le("tabs.files_clear_of_ir", static_cast<double>(filesRight), static_cast<double>(irTab.x) - 4.0 + 0.01);
        P.ge("tabs.ir_room", static_cast<double>(irTab.w), 64.0);
        P.le("tabs.ir_inside", static_cast<double>(irTab.right()), static_cast<double>(panel.programCard().right()));
        // A file's tab still takes its click (the one at the IR tab's left), and the IR tab its own.
        const funkgui::Point last = panel.centreOfTab(panel.fileCount() - 1);
        host.click(last.x, last.y);
        host.settle();
        P.eq("tabs.last_file_click", !panel.irShown() && panel.activeFile() == panel.fileCount() - 1 ? 1 : 0, 1);
        const funkgui::Point ir = panel.centreOfIrTab();
        host.click(ir.x, ir.y);
        host.settle();
        P.eq("tabs.ir_click", panel.irShown() ? 1 : 0, 1);
        // An edit key in the view: the notice whole in the card's header.
        focusEditor(panel, host);
        funkgui::KeyEvent e;
        e.key = funkgui::Key::character;
        e.ch = U'x';
        panel.key(e);
        host.tick(1);
        P.eq("tabs.notice", panel.statusText().find("READ ONLY") != std::string::npos ? 1 : 0, 1);
        P.eq("tabs.notice_whole", panel.statusWhole() ? 1 : 0, 1);
        waitFor(host, [&] { return defines(panel.editor().text(), "playground_process"); });
        host.move(10.0f, 10.0f);
        host.settle();
        writePng(P, host, "gui.ir.tabs.png", "tabs");
        port.setDraft(d);
    }
    P.eq("tabs.panel_gone", processor.compiler().ir().wanted ? 0 : 1, 1);

    pg::projectstore::setDirectoryForTests({});
    folder.deleteRecursively();
    return P.finish();
}
