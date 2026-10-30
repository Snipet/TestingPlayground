// PROBE name=gui.code timeout=300
//
// gui.code: the program's editor as a code editor (editor/CodeAssist.h), headless over a real Processor, driven by keys
// and the pointer as a user would. It writes, to <build>/probe-out/:
//   gui.code.png             GRAPHITE: a program and the library's svf.fm, the caret on a field (its uses marked, the
//                            bracket pair boxed), indent guides, the minimap, the status line
//   gui.code.completion.png  the completion list after a typed prefix, the chosen item's signature and documentation
//   gui.code.signature.png   signature help in a method's call
//   gui.code.hover.png       the hover card of a method (its declaration and the library's usage line)
//   gui.code.problems.png    a check while typing: the squiggle, the mark, the count in the status line, the card
//   gui.code.find.png        the find and replace bar, its matches marked
//   gui.code.paper.png       PAPER: the completion list over the program
//   gui.code.cir.png         a netlist's tab (screamer.cir) highlighted, a check's squiggle on a broken line
// and checks:
//   LOOK      the program's tabs have the code editor's looks (indent guides, the bracket pair, auto-closing pairs,
//             brace indent, the minimap, the mark column) and draw them; the chat's message box has none of them;
//             after RUN no problem; the caret resting on a field marks its three places
//   COMPLETE  a typed prefix opens the list (`fi`: filter first), Tab accepts it; `.` opens the members, Return
//             accepts a method with the caret in its parentheses and its signature shown; `)` steps over the closer;
//             Ctrl-Space opens the list, Escape closes it and the caret stays in the text; a library module (Lfo)
//             accepted adds its tab and its #include, Cmd-Z takes the #include and the word back in one step
//   SIGNATURE `(` after a builtin opens its signature, `,` moves to the next parameter, Escape closes it
//   HOVER     the pointer still over a method's name for half a second: its card; moved off: gone
//   CHECK     an undeclared name typed: within a second, without RUN, a squiggle on it, an error mark on its line, the
//             status line's count, the program that played playing on; the hover card says the message; undone, the
//             next check clears it
//   FORMAT    Shift-Alt-F formats the text shown (the program's house style back from a scrambled copy), one undo
//             step; with Format on Run, RUN formats the files first and plays the formatted text
//   FIND      Cmd-F, a word typed: its matches counted and marked, the first chosen; Return and Cmd-G go on,
//             Shift-Cmd-G back; Cmd-Alt-F and a replacement: Alt-Return replaces them all (one undo step); Escape
//             closes the bar
//   LINES     Cmd-/ comments a line and uncomments it; Alt-Down and Alt-Up move it; Shift-Alt-Down copies it;
//             Shift-Cmd-K deletes it
//   DEFINE    Cmd-click on a method of the library's file shows that tab, the caret on its declaration; Cmd-B on a
//             field goes to its declaration
//   ORDER     an error with a note before it (FMinus writes the error first): each squiggle's card says its own message
//   RUN ONLY  a note only RUN gives stays after RUN (no check of what it compiled replaces it); an edit's check takes
//             it away, and undone (the files RUN compiled) RUN's messages come back
//   FIND TABS Escape after the bar was open over two tabs leaves no match marked in either
//   CIRCUIT   a netlist's tab (the library's screamer.cir, added by LIBRARY with its #include): its own highlighter
//             (lang::lexCirLine) in the code's palette, SPICE in the status line, no completion (Ctrl-Space) and no
//             Format (Shift-Alt-F changes nothing); a broken line typed into it (a transistor, Phase 2's): the check
//             generates the netlist and squiggles that line, the program that plays playing on
#include "Probe.h"

#include "editor/CodeAssist.h"
#include "editor/Panel.h"
#include "fminus/Lang.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"

#include "Library.h"
#include "Product.h"

#include <funkgui/canvas/Prim.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/widgets/TextEditor.h>

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

namespace pg = playground;

namespace
{
    int countTag(const funkgui::PrimList& frame, funkgui::Tag tag)
    {
        int n = 0;
        for (const funkgui::Prim& p : frame.prims)
            n += p.tag == tag ? 1 : 0;
        return n;
    }

    void writePng(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const char* name, const std::string& key)
    {
        const std::string path = probe::outPath(name);
        host.draw();
        P.eq(key + ".png_written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
    }

    // `text` typed, a key a character (a comma too, which HeadlessHost::keys cannot spell); '\n' is Return.
    void type(pg::ui::Panel& panel, funkgui::HeadlessHost& host, const std::string& text)
    {
        for (const char ch : text)
        {
            funkgui::KeyEvent e;
            if (ch == ' ')
            {
                e.key = funkgui::Key::space;
                e.ch = U' ';
            }
            else if (ch == '\n')
                e.key = funkgui::Key::enter;
            else
            {
                e.key = funkgui::Key::character;
                e.ch = static_cast<char32_t>(ch);
            }
            panel.key(e);
        }
        host.tick(1);
    }

    // The place of the first `needle` in the editor's text, plus `extra` columns.
    funkgui::TextPos posIn(const funkgui::TextEditor& ed, std::string_view needle, int extra = 0)
    {
        for (int l = 0; l < ed.lineCount(); ++l)
            if (const std::size_t at = ed.line(l).find(needle); at != std::string::npos)
                return { l, static_cast<int>(at) + extra };
        return { -1, -1 };
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

    // The editor has the keys (a click in its text), the caret where it was.
    void focusEditor(pg::ui::Panel& panel, funkgui::HeadlessHost& host)
    {
        funkgui::TextEditor& ed = panel.editor();
        const funkgui::TextPos caret = ed.caret();
        const funkgui::Rect a = ed.textArea();
        host.click(a.x + 30.0f, a.y + 4.0f);
        panel.editor().setCaret(caret);
    }

    // Until no check is due or under way, and the editor's timers have run.
    void settleChecks(pg::ui::Panel& panel, funkgui::HeadlessHost& host)
    {
        pg::ui::CodeAssist& code = panel.codeAssist();
        waitFor(host, [&] { return !code.checking() && code.settled(); });
        host.settle();
    }

    std::string program()
    {
        if constexpr (pg::product::kIsInstrument)
            return "// Code probe: a filtered saw.\n"
                   "module Voice\n"
                   "{\n"
                   "    param float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]];\n"
                   "    param float gain [[fminus::range(0, 2)]] [[fminus::default(1)]];\n"
                   "    Svf filter;\n"
                   "    float rate;\n"
                   "    float phase;\n"
                   "    float step;\n"
                   "    float level;\n"
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
                   "            filter.set(cutoff[i], 0.3f, rate);\n"
                   "            const float y = filter.tick(2 * phase - 1) * gain[i] * level;\n"
                   "            left[i] += y;\n"
                   "            right[i] += y;\n"
                   "        }\n"
                   "        return true;\n"
                   "    }\n"
                   "};\n";
        else
            return "// Code probe: a filter.\n"
                   "module Effect\n"
                   "{\n"
                   "    param float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]];\n"
                   "    param float gain [[fminus::range(0, 2)]] [[fminus::default(1)]];\n"
                   "    Svf filter;\n"
                   "    float rate;\n"
                   "\n"
                   "    void init(float r)\n"
                   "    {\n"
                   "        rate = r;\n"
                   "    }\n"
                   "\n"
                   "    void process(float* restrict left, float* restrict right, int n)\n"
                   "    {\n"
                   "        for (int i = 0; i < n; ++i)\n"
                   "        {\n"
                   "            filter.set(cutoff[i], 0.3f, rate);\n"
                   "            const float y = filter.tick(left[i]) * gain[i];\n"
                   "            left[i] = y;\n"
                   "            right[i] = y;\n"
                   "        }\n"
                   "    }\n"
                   "};\n";
    }

    // The program's last statement in the loop, after which the probe types.
    const char* lastLine() { return pg::product::kIsInstrument ? "right[i] += y;" : "right[i] = y;"; }

    void settleRun(pg::Processor& p, funkgui::HeadlessHost& host)
    {
        p.settle();
        host.tick(2);
        host.settle();
    }
}

PROBE(gui, code)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);
    pg::Processor processor;
    pg::EditorPort& port = processor.editorPort();
    std::string withSvf;
    {
        pg::ui::Panel panel(processor.ports(), port);
        funkgui::HeadlessHost host(panel, 0, 2.0f);
        host.setZoom({ 100, 125, 150, 200 }, 100);
        host.settle();
        pg::ui::CodeAssist& code = panel.codeAssist();

        // The program, with svf.fm from the library (a tab, #included), run.
        panel.editor().selectAll();
        panel.editor().insert(program());
        P.eq("setup.library_tab", panel.addLibraryFile("svf.fm") ? 1 : 0, 1);
        const funkgui::Point tab0 = panel.centreOfTab(0);
        host.click(tab0.x, tab0.y);
        host.keys("cmd+return");
        settleRun(processor, host);
        withSvf = panel.editor().text();
        P.eq("setup.runs", port.status().ok ? 1 : 0, 1);
        settleChecks(panel, host);

        // ---- LOOK -------------------------------------------------------------------------------------------------
        {
            funkgui::TextEditor& ed = panel.editor();
            const funkgui::TextEditorOptions& o = ed.options();
            P.eq("look.options", o.indentGuides && o.matchBrackets && o.autoClosePairs && o.braceIndent && o.minimap && o.markColumn ? 1 : 0, 1);
            P.eq("look.highlighter", dynamic_cast<const pg::ui::CodeHighlighter*>(ed.highlighter()) != nullptr ? 1 : 0, 1);
            P.eq("look.palette", static_cast<int64_t>(ed.palette().size()), static_cast<int64_t>(pg::ui::csCount));
            const funkgui::TextEditorOptions& chat = panel.chatInput().options();
            P.eq("look.chat_untouched", !chat.minimap && !chat.markColumn && !chat.indentGuides && !chat.autoClosePairs ? 1 : 0, 1);
            P.eq("look.no_problems", code.errorCount() + code.warningCount(), 0);
            ed.setFocused(true);
            ed.setCaret(posIn(ed, "filter.set", 3));
            host.tick(30);
            settleChecks(panel, host);
            int marked = 0;
            for (const funkgui::Decoration& d : ed.decorations())
                marked += d.kind == funkgui::DecorationKind::highlight ? 1 : 0;
            P.eq("look.occurrences", marked, 3);
            const funkgui::PrimList& frame = host.draw();
            P.ge("look.guides_drawn", countTag(frame, funkgui::tags::editGuide), 1.0);
            P.ge("look.minimap_drawn", countTag(frame, funkgui::tags::editMap), 1.0);
            P.ge("look.marks_drawn", countTag(frame, funkgui::tags::editMark), 2.0);   // the ones in view
            P.ge("look.status_drawn", countTag(frame, pg::ui::kCodeTag), 1.0);
            P.eq("look.missing_glyphs", static_cast<int64_t>(frame.missingGlyphs), 0);
            writePng(P, host, "gui.code.png", "look");
        }

        // ---- COMPLETE -----------------------------------------------------------------------------------------------
        {
            funkgui::TextEditor& ed = panel.editor();
            const funkgui::TextPos end = posIn(ed, lastLine(), static_cast<int>(std::string(lastLine()).size()));
            ed.setCaret(end);
            type(panel, host, "\nfi");
            P.eq("complete.opens", code.completionOpen() ? 1 : 0, 1);
            std::printf("completion: %zu items, first %s\n", code.completion().items.size(),
                        code.completion().items.empty() ? "-" : code.completion().items[0].label.c_str());
            P.eq("complete.first", !code.completion().items.empty() && code.completion().items[0].label == "filter" ? 1 : 0, 1);
            host.tick(2);
            {
                const funkgui::PrimList& frame = host.draw();
                bool inside = true;
                for (const funkgui::Prim& p : frame.prims)
                    if (p.tag == pg::ui::kCodePopupTag)
                        inside = inside && p.x0 >= -8.0f && p.y0 >= -8.0f && p.x1 <= static_cast<float>(panel.width()) + 8.0f
                              && p.y1 <= static_cast<float>(panel.height()) + 8.0f;
                P.ge("complete.drawn", countTag(frame, pg::ui::kCodePopupTag), 10.0);
                P.eq("complete.inside_panel", inside ? 1 : 0, 1);
                writePng(P, host, "gui.code.completion.png", "complete");
            }
            host.keys("tab");
            const int ln = ed.caret().line;
            P.eq("complete.tab_accepts", ed.line(ln) == "            filter" && !code.completionOpen() ? 1 : 0, 1);
            type(panel, host, ".");
            P.eq("complete.members", code.completionOpen() && code.completion().member ? 1 : 0, 1);
            type(panel, host, "ti");
            P.eq("complete.filters", !code.completion().items.empty() && code.completion().items[0].label == "tick" ? 1 : 0, 1);
            host.keys("return");
            P.eq("complete.method_call", ed.line(ln) == "            filter.tick()" && ed.caret().col == 24 ? 1 : 0, 1);
            P.eq("complete.signature", code.signatureOpen() && code.signature().label == "float tick(float x)" ? 1 : 0, 1);
            host.tick(2);
            writePng(P, host, "gui.code.signature.png", "signature");
            type(panel, host, "0.5f);");
            P.eq("complete.steps_over", ed.line(ln) == "            filter.tick(0.5f);" ? 1 : 0, 1);
            P.eq("complete.signature_closes", code.signatureOpen() ? 0 : 1, 1);
            host.keys("ctrl+space");
            P.eq("complete.ctrl_space", code.completionOpen() ? 1 : 0, 1);
            host.keys("escape");
            P.eq("complete.escape", !code.completionOpen() && ed.focused() ? 1 : 0, 1);

            // A library module: its tab and #include come with it, and go with Cmd-Z (the tab stays).
            const int tabs = panel.fileCount();
            type(panel, host, "\nLf");
            const bool lfo = code.completionOpen() && !code.completion().items.empty() && code.completion().items[0].label == "Lfo"
                          && code.completion().items[0].include == "lfo.fm";
            P.eq("complete.library_offered", lfo ? 1 : 0, 1);
            host.keys("return");
            const std::string after = ed.text();
            P.eq("complete.library_include", after.find("#include \"lfo.fm\"") != std::string::npos ? 1 : 0, 1);
            P.eq("complete.library_tab", panel.fileCount(), tabs + 1);
            host.keys("cmd+z");
            P.eq("complete.library_undo", ed.text().find("#include \"lfo.fm\"") == std::string::npos
                                              && ed.text().find("Lfo") == std::string::npos ? 1 : 0, 1);
            host.keys("backspace,backspace");                   // the "Lf" typed
        }

        // ---- SIGNATURE ----------------------------------------------------------------------------------------------
        {
            funkgui::TextEditor& ed = panel.editor();
            focusEditor(panel, host);
            type(panel, host, "fminf(");
            if (code.completionOpen())
                host.keys("escape");
            P.eq("signature.opens", code.signatureOpen() && code.signature().label == "float fminf(float x, float y)"
                                        && code.signature().active == 0 ? 1 : 0, 1);
            type(panel, host, "1,");
            P.eq("signature.next_param", code.signatureOpen() && code.signature().active == 1 ? 1 : 0, 1);
            host.keys("escape");
            P.eq("signature.escape", code.signatureOpen() ? 0 : 1, 1);
            // Back to the program as run.
            ed.selectAll();
            ed.insert(withSvf);
            settleChecks(panel, host);
        }

        // ---- HOVER --------------------------------------------------------------------------------------------------
        {
            funkgui::TextEditor& ed = panel.editor();
            const funkgui::Point at = ed.pointOf(posIn(ed, "filter.tick", 9));
            host.move(at.x + 3.0f, at.y + ed.rowHeight() * 0.5f);
            host.tick(40);
            std::printf("hover: %s\n", code.hoverText().c_str());
            P.eq("hover.opens", code.hoverOpen() && code.hoverText().find("float tick(float x)") != std::string::npos
                                    && code.hoverText().find("a method of Svf") != std::string::npos ? 1 : 0, 1);
            writePng(P, host, "gui.code.hover.png", "hover");
            host.move(at.x + 3.0f, at.y + 6.0f * ed.rowHeight());
            host.tick(2);
            P.eq("hover.moves_off", code.hoverOpen() ? 0 : 1, 1);
        }

        // ---- CHECK --------------------------------------------------------------------------------------------------
        {
            funkgui::TextEditor& ed = panel.editor();
            focusEditor(panel, host);
            const uint64_t runs = port.statusRevision();
            const pg::Sources playing = port.running();
            const funkgui::TextPos end = posIn(ed, lastLine(), static_cast<int>(std::string(lastLine()).size()));
            ed.setFocused(true);
            ed.setCaret(end);
            ed.insert("\n            nothing = 1;");
            const bool marked = waitFor(host, [&] { return code.errorCount() > 0 && !code.checking(); });
            const int ln = end.line + 1;
            bool squiggle = false, mark = false;
            for (const funkgui::Decoration& d : ed.decorations())
                squiggle = squiggle || (d.kind == funkgui::DecorationKind::error && d.from.line == ln && d.from.col == 12 && d.to.col == 19);
            for (const funkgui::LineMark& m : ed.lineMarks())
                mark = mark || (m.line == ln && m.kind == funkgui::LineMark::Kind::error);
            P.eq("check.in_time", marked ? 1 : 0, 1);
            P.eq("check.squiggle", squiggle ? 1 : 0, 1);
            P.eq("check.mark", mark ? 1 : 0, 1);
            P.eq("check.count", code.errorCount(), 1);
            P.eq("check.no_run", port.statusRevision() == runs && port.running() == playing ? 1 : 0, 1);
            const funkgui::Point at = ed.pointOf({ ln, 14 });
            host.move(at.x + 2.0f, at.y + ed.rowHeight() * 0.5f);
            host.tick(40);
            P.eq("check.hover_says", code.hoverText().find("use of undeclared identifier 'nothing'") != std::string::npos ? 1 : 0, 1);
            writePng(P, host, "gui.code.problems.png", "check");
            host.move(10.0f, 10.0f);
            ed.undo();
            P.eq("check.cleared", waitFor(host, [&] { return code.errorCount() == 0 && ed.lineMarks().empty() && !code.checking(); })
                                      ? 1 : 0, 1);
        }

        // ---- FORMAT -------------------------------------------------------------------------------------------------
        {
            funkgui::TextEditor& ed = panel.editor();
            focusEditor(panel, host);
            std::string messy;
            for (const std::string_view l : pg::lang::splitLines(withSvf))
            {
                const std::size_t first = l.find_first_not_of(' ');
                messy += std::string(first == std::string_view::npos ? std::string_view{} : l.substr(first)) + "\n";
            }
            messy.pop_back();
            ed.selectAll();
            ed.insert(messy);
            host.keys("shift+alt+f");
            P.eq("format.house_style", ed.text() == withSvf ? 1 : 0, 1);
            host.keys("cmd+z");
            P.eq("format.one_undo", ed.text() == messy ? 1 : 0, 1);
            // Format on Run.
            code.setFormatOnRun(true);
            P.eq("format.on_run_pref", funkgui::UiPreferences::get().getInt("codeFormatOnRun", 0, 0, 1), 1);
            host.keys("cmd+return");
            settleRun(processor, host);
            P.eq("format.on_run", ed.text() == withSvf && !port.running().empty() && port.running()[0].text == withSvf ? 1 : 0, 1);
            code.setFormatOnRun(false);
            settleChecks(panel, host);
        }

        // ---- FIND ---------------------------------------------------------------------------------------------------
        {
            funkgui::TextEditor& ed = panel.editor();
            focusEditor(panel, host);
            ed.setFocused(true);
            ed.setCaret({ 0, 0 });
            host.keys("cmd+f");
            P.eq("find.opens", code.findOpen() && code.takesKeys() ? 1 : 0, 1);
            type(panel, host, "gain");
            P.eq("find.count", code.findCount(), 2);
            P.eq("find.first", code.findCurrent(), 0);
            host.keys("return");
            P.eq("find.next", code.findCurrent(), 1);
            host.keys("cmd+g");
            P.eq("find.wraps", code.findCurrent(), 0);
            host.keys("shift+cmd+g");
            P.eq("find.back", code.findCurrent(), 1);
            int marked = 0;
            for (const funkgui::Decoration& d : ed.decorations())
                marked += d.kind == funkgui::DecorationKind::findMatch || d.kind == funkgui::DecorationKind::findCurrent ? 1 : 0;
            P.eq("find.marked", marked, 2);
            host.keys("cmd+alt+f");
            type(panel, host, "volume");
            host.tick(2);
            writePng(P, host, "gui.code.find.png", "find");
            host.keys("alt+return");
            P.eq("find.replace_all", ed.text().find("gain") == std::string::npos && ed.text().find("volume[i]") != std::string::npos ? 1 : 0, 1);
            host.keys("escape");
            int left = 0;
            for (const funkgui::Decoration& d : ed.decorations())
                left += d.kind == funkgui::DecorationKind::findMatch || d.kind == funkgui::DecorationKind::findCurrent ? 1 : 0;
            P.eq("find.closes", !code.findOpen() && left == 0 && ed.focused() ? 1 : 0, 1);
            host.keys("cmd+z");
            P.eq("find.undo_all", ed.text() == withSvf ? 1 : 0, 1);
        }

        // ---- LINES --------------------------------------------------------------------------------------------------
        {
            funkgui::TextEditor& ed = panel.editor();
            focusEditor(panel, host);
            const funkgui::TextPos rate = posIn(ed, "rate = r;", 2);
            ed.setCaret(rate);
            host.keys("cmd+/");
            P.eq("lines.comment", ed.line(rate.line) == "        // rate = r;" ? 1 : 0, 1);
            host.keys("cmd+/");
            P.eq("lines.uncomment", ed.line(rate.line) == "        rate = r;" ? 1 : 0, 1);
            host.keys("alt+down");
            P.eq("lines.move_down", ed.line(rate.line + 1) == "        rate = r;" && ed.caret().line == rate.line + 1 ? 1 : 0, 1);
            host.keys("alt+up");
            P.eq("lines.move_up", ed.text() == withSvf && ed.caret().line == rate.line ? 1 : 0, 1);
            host.keys("shift+alt+down");
            P.eq("lines.copy", ed.line(rate.line + 1) == "        rate = r;" && ed.line(rate.line) == "        rate = r;"
                                   && ed.lineCount() == static_cast<int>(pg::lang::splitLines(withSvf).size()) + 1 ? 1 : 0, 1);
            host.keys("shift+cmd+k");
            P.eq("lines.delete", ed.text() == withSvf ? 1 : 0, 1);
        }

        // ---- DEFINE -------------------------------------------------------------------------------------------------
        {
            funkgui::TextEditor& ed = panel.editor();
            const funkgui::Point at = ed.pointOf(posIn(ed, "filter.tick", 9));
            funkgui::Mods cmd;
            cmd.cmd = true;
            host.click(at.x + 3.0f, at.y + ed.rowHeight() * 0.5f, cmd);
            host.settle();
            const std::string& svf = pg::findLibraryFile("svf.fm")->source;
            const pg::lang::Pos tick = [&] {
                const std::vector<std::string_view> lines = pg::lang::splitLines(svf);
                for (std::size_t l = 0; l < lines.size(); ++l)
                    if (const std::size_t c = lines[l].find("float tick("); c != std::string_view::npos)
                        return pg::lang::Pos{ static_cast<int>(l), static_cast<int>(c) + 6 };
                return pg::lang::Pos{};
            }();
            P.eq("define.other_tab", panel.fileName(panel.activeFile()) == "svf.fm" ? 1 : 0, 1);
            P.eq("define.caret", panel.editor().caret().line == tick.line && panel.editor().caret().col == tick.col ? 1 : 0, 1);
            host.click(tab0.x, tab0.y);
            host.settle();
            funkgui::TextEditor& prog = panel.editor();
            prog.setFocused(true);
            prog.setCaret(posIn(prog, "rate = r;", 1));
            host.keys("cmd+b");
            P.eq("define.field", prog.caret() == posIn(prog, "float rate;", 6) ? 1 : 0, 1);
        }

        // ---- ORDER --------------------------------------------------------------------------------------------------
        {
            funkgui::TextEditor& ed = panel.editor();
            focusEditor(panel, host);
            ed.setFocused(true);
            ed.setCaret({ 0, 0 });
            ed.insert("float fminus_rt_sinf(float x)\n{\n    return x;\n}\n");
            const funkgui::TextPos end = posIn(ed, lastLine(), static_cast<int>(std::string(lastLine()).size()));
            ed.setCaret(end);
            ed.insert("\n            left[i] = sinf(left[i]);");
            host.move(10.0f, 10.0f);
            const bool checked = waitFor(host, [&] { return code.errorCount() > 0 && !code.checking(); });
            funkgui::TextPos error{ -1, -1 }, note{ -1, -1 };
            for (const funkgui::Decoration& d : ed.decorations())
                if (d.kind == funkgui::DecorationKind::error && error.line < 0)
                    error = d.from;
                else if (d.kind == funkgui::DecorationKind::info && note.line < 0)
                    note = d.from;
            P.eq("order.checked", checked ? 1 : 0, 1);
            P.eq("order.note_before_error", note.line >= 0 && error.line >= 0 && note < error ? 1 : 0, 1);
            const auto hoverAt = [&](funkgui::TextPos p) {
                ed.setCaret(p);                                  // in view (the note is at the file's top)
                ed.scrollToCaret();
                host.tick(1);
                const funkgui::Point at = ed.pointOf(p);
                host.move(at.x + 2.0f, at.y + ed.rowHeight() * 0.5f);
                host.tick(40);
                const std::string text = code.hoverOpen() ? code.hoverText() : std::string();
                host.move(10.0f, 10.0f);
                host.tick(2);
                return text;
            };
            const std::string onError = hoverAt(error), onNote = hoverAt(note);
            std::printf("hover on the error: %s\nhover on the note: %s\n", onError.c_str(), onNote.c_str());
            P.eq("order.error_card", onError.find("error: ") != std::string::npos && onError.find("reserves") != std::string::npos
                                         && onError.find("declared here") == std::string::npos ? 1 : 0, 1);
            P.eq("order.note_card", onNote.find("note: ") != std::string::npos && onNote.find("declared here") != std::string::npos
                                        && onNote.find("reserves") == std::string::npos ? 1 : 0, 1);
            ed.selectAll();
            ed.insert(withSvf);
            P.eq("order.cleared", waitFor(host, [&] { return code.errorCount() == 0 && ed.lineMarks().empty() && !code.checking(); })
                                      ? 1 : 0, 1);
        }

        // ---- RUN ONLY -----------------------------------------------------------------------------------------------
        {
            funkgui::TextEditor& ed = panel.editor();
            const auto notes = [&] {
                int n = 0;
                for (const funkgui::LineMark& m : ed.lineMarks())
                    n += m.kind == funkgui::LineMark::Kind::info ? 1 : 0;
                return n;
            };
            focusEditor(panel, host);
            ed.setFocused(true);
            ed.setCaret({ 0, 0 });
            ed.insert("#include \"host.fm\"\n");                // a Host no field points at: a note of RUN's alone
            host.keys("cmd+return");                             // before the check due
            settleRun(processor, host);
            P.eq("runonly.run_note", port.status().ok && notes() == 1 ? 1 : 0, 1);
            settleChecks(panel, host);
            P.eq("runonly.no_check_after_run", notes(), 1);
            const funkgui::TextPos end = posIn(ed, lastLine(), static_cast<int>(std::string(lastLine()).size()));
            ed.setCaret(end);
            ed.insert("\n            // edited");
            P.eq("runonly.edit_checked", waitFor(host, [&] { return notes() == 0 && !code.checking(); }) ? 1 : 0, 1);
            ed.undo();                                           // the files RUN compiled: its messages again
            P.eq("runonly.undo_run_again", waitFor(host, [&] { return notes() == 1 && !code.checking(); }) ? 1 : 0, 1);
            ed.selectAll();
            ed.insert(withSvf);
            host.keys("cmd+return");
            settleRun(processor, host);
            settleChecks(panel, host);
            P.eq("runonly.restored", notes() == 0 && code.errorCount() == 0 && port.running()[0].text == withSvf ? 1 : 0, 1);
        }

        // ---- FIND TABS ----------------------------------------------------------------------------------------------
        {
            const auto found = [](const funkgui::TextEditor& e) {
                int n = 0;
                for (const funkgui::Decoration& d : e.decorations())
                    n += d.kind == funkgui::DecorationKind::findMatch || d.kind == funkgui::DecorationKind::findCurrent ? 1 : 0;
                return n;
            };
            focusEditor(panel, host);
            panel.editor().setFocused(true);
            panel.editor().setCaret({ 0, 0 });
            host.keys("cmd+f");
            type(panel, host, "rate");
            P.ge("findtabs.program_marked", found(panel.editor()), 2.0);
            const funkgui::Point tab1 = panel.centreOfTab(1);
            host.click(tab1.x, tab1.y);                          // svf.fm, the bar open: its own matches
            host.settle();
            P.eq("findtabs.other_tab", panel.fileName(panel.activeFile()) == "svf.fm" && code.findOpen() ? 1 : 0, 1);
            host.keys("escape");
            P.eq("findtabs.closed", code.findOpen() ? 0 : 1, 1);
            host.click(tab0.x, tab0.y);
            host.settle();
            P.eq("findtabs.program_clear", panel.activeFile() == 0 && found(panel.editor()) == 0 ? 1 : 0, 1);
        }
    }

    // ---- PAPER ------------------------------------------------------------------------------------------------------
    {
        funkgui::UiPreferences::get().setTheme(1);
        pg::ui::Panel panel(processor.ports(), port);
        funkgui::HeadlessHost host(panel, 1, 2.0f);
        host.setZoom({ 100, 125, 150, 200 }, 100);
        host.settle();
        funkgui::TextEditor& ed = panel.editor();
        P.eq("paper.program", ed.text() == withSvf ? 1 : 0, 1);
        host.draw();
        const funkgui::Col comment = ed.palette()[pg::ui::csComment].colour;
        const funkgui::Col graphite = pg::ui::codePalette(funkgui::Theme::graphite())[pg::ui::csComment].colour;
        P.eq("paper.palette", comment.r != graphite.r || comment.g != graphite.g ? 1 : 0, 1);
        ed.setFocused(true);
        ed.setCaret(posIn(ed, "filter.set", 7));
        host.keys("ctrl+space");
        P.eq("paper.completion", panel.codeAssist().completionOpen() ? 1 : 0, 1);
        host.tick(2);
        writePng(P, host, "gui.code.paper.png", "paper");
        host.keys("escape");
    }

    // ---- CIRCUIT ----------------------------------------------------------------------------------------------------
    {
        funkgui::UiPreferences::get().setTheme(0);
        pg::ui::Panel panel(processor.ports(), port);
        funkgui::HeadlessHost host(panel, 0, 2.0f);
        host.setZoom({ 100, 125, 150, 200 }, 100);
        host.settle();
        pg::ui::CodeAssist& code = panel.codeAssist();
        P.eq("cir.library_tab", panel.addLibraryFile("screamer.cir") ? 1 : 0, 1);
        int tab = -1;
        for (int i = 0; i < 12; ++i)
            if (panel.fileName(i) == "screamer.cir")
                tab = i;
        const funkgui::Point at = panel.centreOfTab(tab);
        host.click(at.x, at.y);
        host.settle();
        funkgui::TextEditor& ed = panel.editor();
        P.eq("cir.shown", tab > 0 && panel.activeFile() == tab && ed.text() == pg::findLibraryFile("screamer.cir")->source ? 1 : 0, 1);
        P.eq("cir.highlighter", dynamic_cast<const pg::ui::CircuitHighlighter*>(ed.highlighter()) != nullptr ? 1 : 0, 1);
        P.eq("cir.palette", static_cast<int64_t>(ed.palette().size()), static_cast<int64_t>(pg::ui::csCount));
        P.eq("cir.language", std::string(code.language(tab)) == "SPICE" && std::string(code.language(0)) == "FMinus" ? 1 : 0, 1);
        focusEditor(panel, host);
        ed.setCaret(posIn(ed, "R4 "));
        host.keys("ctrl+space");
        P.eq("cir.no_completion", code.completionOpen() ? 0 : 1, 1);
        const std::string before = ed.text();
        host.keys("shift+alt+f");
        P.eq("cir.no_format", ed.text() == before ? 1 : 0, 1);
        // A broken line: the check generates the netlist (the program does not use its module) and squiggles it.
        settleChecks(panel, host);
        const uint64_t runs = port.statusRevision();
        const funkgui::TextPos line = posIn(ed, "C4 ");
        ed.setCaret({ line.line, 0 });
        type(panel, host, "Q1     n1  0   v2  2N3904\n");
        const bool marked = waitFor(host, [&] { return code.errorCount() > 0 && !code.checking(); });
        bool squiggle = false, mark = false;
        for (const funkgui::Decoration& d : ed.decorations())
            squiggle = squiggle || (d.kind == funkgui::DecorationKind::error && d.from.line == line.line);
        for (const funkgui::LineMark& m : ed.lineMarks())
            mark = mark || (m.line == line.line && m.kind == funkgui::LineMark::Kind::error);
        P.eq("cir.check_in_time", marked ? 1 : 0, 1);
        P.eq("cir.check_squiggle", squiggle && mark ? 1 : 0, 1);
        P.eq("cir.check_no_run", port.statusRevision() == runs ? 1 : 0, 1);
        const funkgui::Point p = ed.pointOf({ line.line, 1 });   // on "Q1", which the squiggle covers
        host.move(p.x + 2.0f, p.y + ed.rowHeight() * 0.5f);
        host.tick(40);
        P.eq("cir.hover_says", code.hoverText().find("transistor Q1") != std::string::npos ? 1 : 0, 1);
        writePng(P, host, "gui.code.cir.png", "cir");
        host.move(10.0f, 10.0f);
    }
    funkgui::UiPreferences::get().setTheme(0);
    return P.finish();
}
