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
//   gui.code.conditional.png an #if 0 and an #ifndef DEBUG group dimmed, #ifdef DEBUG's group read, the directives'
//                            parts, a -Wswitch warning's squiggle and mark after RUN and its row in the problems
//   gui.code.doubles.png     a module with a double, a 2-D array and a field's attributes: its strings coloured, the
//                            hover card of the field with its attributes (on two lines: lang::kHoverWidth)
//   gui.code.hover_long.png  the hover card of a function of eight parameters, on two lines; a local named as a
//                            constant drawn plain, the constant's uses in the constants' colour
//   gui.code.hover_local.png the hover card of that local: its name plain, as the editor draws it
//   gui.code.size_9.png, gui.code.size_12.png, gui.code.size_24.png (and .paper.png each)
//                            the text size (CodeAssist.h's codesize) at 9, 12 and 24 px: the program and, at 24 px,
//                            the completion list scaled with it; the status line saying the size (not at 12)
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
//   COND      FMinus's conditionals and warnings: an #if 0 group, an #ifndef DEBUG group and a switch under #ifdef
//             DEBUG that misses an enumeration's constant typed after the main module and RUN: it compiles and plays,
//             one warning and no error (the status line's count), an amber squiggle and mark on the switch's value and
//             no note's beside it, the problems list's row a warning; the tab's highlighter dims both skipped groups'
//             lines (csInactive) and not the group read; hover on DEBUG says the plugin defines it; written to
//             gui.code.conditional.png; then the program before it again
//   FIND TABS Escape after the bar was open over two tabs leaves no match marked in either
//   CIRCUIT   a netlist's tab (the library's screamer.cir, added by LIBRARY with its #include): its own highlighter
//             (lang::lexCirLine) in the code's palette, SPICE in the status line, no completion (Ctrl-Space) and no
//             Format (Shift-Alt-F changes nothing); a broken line typed into it (a transistor, Phase 2's): the check
//             generates the netlist and squiggles that line, the program that plays playing on; its #include taken
//             out, RUN compiles and plays, and the tab keeps the check's squiggle after RUN (RUN generates only the
//             netlists an #include reaches)
//   CIRLAYOUT the schematic's *@ lines are invisible to the program: a *@ part line typed into the netlist's tab and
//             edited leaves the status RUNNING, asks for no check, and the check's squiggle stays; the squiggled Q1
//             deleted and undone (the same bytes again) asks for a check, which puts its squiggle back; a RUN in flight
//             (the netlist included again, Q1 an error) lands its messages on the moved lines after a write gathers
//             *@ lines from mid-file before .end (cir::lineMap)
//   TYPES     a module named on a line before it is declared: the line is drawn plain, and after the declaration is
//             typed (the line itself not edited) it keeps its colour until the index has the name, then is drawn in the
//             types' colour with no input (CodeAssist::sync rehighlights the editors, whose spans FunkGui keeps), the
//             Panel asking for that frame; the same through Claude's write_file while the editor has not the keys
//             (the check's sync, the files rested, recolours it: no caret rests in it, no pointer hovers it); the Code
//             view's tick, recording and highlighter costs printed; idle, the Code view asks for no frame
//   SHADOW    a local named as a constant of the top level: its declaration and its use drawn plain (an identifier's
//             ink), the constant's declaration and its use in another function in the constants' colour, once the
//             check's sync has the index; a function of eight parameters hovered: its card's code on two lines, the
//             second under its parenthesis (gui.code.hover_long.png); the local hovered: no glyph of its card in the
//             constants' colour (gui.code.hover_local.png), the constant hovered: its name's; the tab renamed by its
//             name's field, and renamed back: the local still plain, the constant still coloured
//   SIZE      the text size, this machine's preference: Cmd-=, Cmd-+, Cmd-- and Cmd-0 step it through 9, 10, 11, 12, 13,
//             14, 16, 18, 20, 24 px, stopping at 9 and 24 (the keys still taken there), every file tab drawn in it, the
//             row height round(px * 1.4); the line at the text's top and the caret where they were; the status line's
//             size (none at 12 px), a click on it 12 px again; Cmd-wheel a step a notch both ways, a pad's deltas
//             gathered to a step, a plain wheel scrolling and leaving the size; the completion list's rows, its width,
//             the hover card and the find bar larger at 24 px than at 12; a second Panel follows the preference with
//             no input of its own (and a Panel made after reads it); the IR view drawn in it and its keys stepping
//             it; a netlist's Schematic view keeps Cmd-= and Cmd-- for its zoom, the size unchanged
#include "Probe.h"

#include "editor/CodeAssist.h"
#include "editor/Panel.h"
#include "fminus/CirDoc.h"
#include "fminus/Lang.h"
#include "plugin/ChatAgent.h"
#include "plugin/ClaudeApi.h"
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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace pg = playground;
namespace cir = playground::cir;

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

        // ---- COND ---------------------------------------------------------------------------------------------------
        {
            funkgui::TextEditor& ed = panel.editor();
            focusEditor(panel, host);
            ed.setFocused(true);
            ed.setCaret({ ed.lineCount() - 1, 0 });
            ed.insert("\n#if 0\n"
                      "float aside(float x) { return x * x; }   // set aside: never compiled\n"
                      "#endif\n"
                      "enum Wave { SAW, SQUARE, NOISE };\n"
                      "#ifndef DEBUG\n"
                      "float plain(float x) { return x; }\n"
                      "#endif\n"
                      "float wave(enum Wave w)\n"
                      "{\n"
                      "#ifdef DEBUG\n"
                      "    switch (w)\n"
                      "    {\n"
                      "        case SAW: return 1;\n"
                      "        case SQUARE: return -1;\n"
                      "    }\n"
                      "#endif\n"
                      "    return 0;\n"
                      "}\n");
            host.keys("cmd+return");
            settleRun(processor, host);
            settleChecks(panel, host);
            const int sw = posIn(ed, "    switch (w)").line;
            bool squiggle = false, mark = false, noteThere = false;
            for (const funkgui::Decoration& d : ed.decorations())
            {
                squiggle = squiggle || (d.kind == funkgui::DecorationKind::warning && d.from.line == sw);
                noteThere = noteThere || (d.kind == funkgui::DecorationKind::info && d.from.line == sw);
            }
            for (const funkgui::LineMark& m : ed.lineMarks())
                mark = mark || (m.line == sw && m.kind == funkgui::LineMark::Kind::warning);
            int warnRows = 0, errorRows = 0;
            for (const pg::ui::Problem& p : panel.problems())
            {
                warnRows += p.warning ? 1 : 0;
                errorRows += p.error ? 1 : 0;
            }
            std::printf("cond: status %d, %d error(s), %d warning(s); notes: %s", port.status().ok ? 1 : 0, code.errorCount(),
                        code.warningCount(), port.status().notes.c_str());
            P.eq("cond.plays", port.status().ok && port.running()[0].text == ed.text() ? 1 : 0, 1);
            P.eq("cond.counts", code.warningCount() == 1 && code.errorCount() == 0 ? 1 : 0, 1);
            P.eq("cond.squiggle_and_mark", squiggle && mark && !noteThere ? 1 : 0, 1);
            P.eq("cond.problem_row", warnRows == 1 && errorRows == 0 ? 1 : 0, 1);
            // The tab's highlighter: each line from the first, as the editor runs it.
            const funkgui::text::Highlighter* hl = ed.highlighter();
            std::vector<bool> dim(static_cast<std::size_t>(ed.lineCount()), false);
            uint32_t st = 0;
            for (int l = 0; l < ed.lineCount() && hl != nullptr; ++l)
            {
                std::vector<funkgui::text::Span> spans;
                st = hl->line(ed.line(l), st, spans);
                dim[static_cast<std::size_t>(l)] = spans.size() == 1 && spans[0].style == pg::ui::csInactive;
            }
            const auto dimAt = [&](std::string_view needle) {
                const int l = posIn(ed, needle).line;
                return l >= 0 && dim[static_cast<std::size_t>(l)];
            };
            P.eq("cond.dims_skipped", dimAt("float aside(") && dimAt("float plain(") ? 1 : 0, 1);
            P.eq("cond.reads_debug", !dimAt("    switch (w)") && !dimAt("        case SAW") && !dimAt("enum Wave") ? 1 : 0, 1);
            const int ifdef = posIn(ed, "#ifdef DEBUG").line;
            const funkgui::Point at = ed.pointOf({ ifdef, 9 });
            host.move(at.x + 2.0f, at.y + ed.rowHeight() * 0.5f);
            host.tick(40);
            P.eq("cond.hover_debug", code.hoverText().find("Defined by the plugin") != std::string::npos ? 1 : 0, 1);
            host.move(10.0f, 10.0f);
            host.tick(2);
            ed.setCaret({ posIn(ed, "#if 0").line, 0 });
            ed.scrollToCaret();
            host.tick(2);
            writePng(P, host, "gui.code.conditional.png", "cond");
            ed.selectAll();
            ed.insert(withSvf);
            host.keys("cmd+return");
            settleRun(processor, host);
            settleChecks(panel, host);
            P.eq("cond.restored", code.errorCount() + code.warningCount() == 0 && port.running()[0].text == withSvf ? 1 : 0, 1);
        }

        // ---- DOUBLES: a double, a 2-D array and a field's attributes (FMinus 3ed1ba6) -------------------------------
        {
            funkgui::TextEditor& ed = panel.editor();
            focusEditor(panel, host);
            ed.setFocused(true);
            ed.setCaret({ ed.lineCount() - 1, 0 });
            ed.insert("\nmodule Meter\n"
                      "{\n"
                      "    double clock [[ui::unit(\"s\")]];\n"
                      "    float level [[ui::unit(\"dB\"), ui::label(\"LEVEL\")]] [[fminus::range(-60, 0)]];\n"
                      "    float bank[4][16];\n"
                      "\n"
                      "    void tick(float x)\n"
                      "    {\n"
                      "        clock += 1.0 / 48000;\n"
                      "        level = (float)sqrt(fabs(clock)) + x;\n"
                      "        bank[1][2] = level;\n"
                      "    }\n"
                      "};\n");
            host.keys("cmd+return");
            settleRun(processor, host);
            settleChecks(panel, host);
            std::printf("doubles: status %d, %d error(s), %d warning(s): %s%s", port.status().ok ? 1 : 0, code.errorCount(),
                        code.warningCount(), port.status().errors.c_str(), port.status().notes.c_str());
            P.eq("doubles.plays", port.status().ok && port.running()[0].text == ed.text() ? 1 : 0, 1);
            P.eq("doubles.clean", code.errorCount() + code.warningCount() == 0 ? 1 : 0, 1);
            // The tab's highlighter: a string in an attribute a string, `double` a type.
            const funkgui::text::Highlighter* hl = ed.highlighter();
            const auto styleOn = [&](std::string_view lineText, std::string_view word) {
                const int l = posIn(ed, lineText).line;
                if (hl == nullptr || l < 0)
                    return -1;
                const std::string text = ed.line(l);
                const std::size_t at = text.find(word);
                std::vector<funkgui::text::Span> spans;
                hl->line(text, 0, spans);
                for (const funkgui::text::Span& sp : spans)
                    if (at != std::string::npos && sp.start == static_cast<int>(at) && sp.end == static_cast<int>(at + word.size()))
                        return static_cast<int>(sp.style);
                return -1;
            };
            P.eq("doubles.string_coloured", styleOn("    float level [[", "\"dB\""), pg::ui::csString);
            P.eq("doubles.double_a_type", styleOn("    double clock", "double"), pg::ui::csType);
            const funkgui::Point at = ed.pointOf(posIn(ed, "        level = (float)", 9));
            host.move(at.x + 3.0f, at.y + ed.rowHeight() * 0.5f);
            host.tick(40);
            std::printf("doubles: hover %s\n", code.hoverText().c_str());
            P.eq("doubles.hover_attributes", code.hoverOpen()
                                                 && code.hoverText().find("[[ui::unit(\"dB\"), ui::label(\"LEVEL\")]]") != std::string::npos
                                                 && code.hoverText().find("a field of Meter") != std::string::npos ? 1 : 0, 1);
            writePng(P, host, "gui.code.doubles.png", "doubles");
            host.move(10.0f, 10.0f);
            host.tick(2);
            ed.selectAll();
            ed.insert(withSvf);
            host.keys("cmd+return");
            settleRun(processor, host);
            settleChecks(panel, host);
            P.eq("doubles.restored", code.errorCount() + code.warningCount() == 0 && port.running()[0].text == withSvf ? 1 : 0, 1);
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
        // Its #include taken out: RUN does not generate it (the program compiles and plays), and its tab keeps the
        // check's squiggle after RUN, and after the check of the files RUN compiled.
        const funkgui::Point tab0 = panel.centreOfTab(0);
        host.click(tab0.x, tab0.y);
        host.settle();
        funkgui::TextEditor& prog = panel.editor();
        std::string text = prog.text();
        const std::string include = "#include \"screamer.cir\"\n";
        const std::size_t cut = text.find(include);
        if (cut != std::string::npos)
            text.erase(cut, include.size());
        prog.selectAll();
        prog.insert(text);
        host.keys("cmd+return");
        settleRun(processor, host);
        settleChecks(panel, host);
        host.click(at.x, at.y);
        host.settle();
        const auto squiggled = [&] {
            bool on = false;
            for (const funkgui::Decoration& d : panel.editor().decorations())
                on = on || (d.kind == funkgui::DecorationKind::error && d.from.line == line.line);
            return on;
        };
        P.eq("cir.uninclude_runs", cut != std::string::npos && port.status().ok && panel.activeFile() == tab ? 1 : 0, 1);
        P.eq("cir.uninclude_keeps_squiggle", squiggled() && code.errorCount() > 0 ? 1 : 0, 1);

        // ---- CIRLAYOUT -------------------------------------------------------------------------------------------------
        // The schematic's *@ lines (fminus/CirDoc.h) are invisible to the program: a *@ part line typed into the
        // netlist's tab, then edited, leaves the status RUNNING, asks for no check, and the check's squiggle stays.
        settleChecks(panel, host);
        waitFor(host, [&] { return panel.statusText().rfind("RUNNING", 0) == 0; });   // past Format's notice
        P.eq("cirlayout.running_before", panel.statusText().rfind("RUNNING", 0) == 0 ? 1 : 0, 1);
        const uint64_t checks = port.checkStatus().revision;
        funkgui::TextEditor& net = panel.editor();
        focusEditor(panel, host);
        net.setCaret(posIn(net, ".end"));
        type(panel, host, "*@ part R4 3,4\n");
        host.tick(50);                                    // past the check's rest
        net.setCaret(posIn(net, "*@ part R4 3,4", 14));
        host.keys("backspace");
        type(panel, host, "9");
        for (int k = 0; k < 6; ++k)
        {
            host.tick(20);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        settleChecks(panel, host);
        P.eq("cirlayout.edited_line", posIn(net, "*@ part R4 3,9").line > 0 ? 1 : 0, 1);
        P.eq("cirlayout.status_running", panel.statusText().rfind("RUNNING", 0) == 0 ? 1 : 0, 1);
        P.eq("cirlayout.no_check", port.checkStatus().revision == checks && !code.checking() ? 1 : 0, 1);
        P.eq("cirlayout.squiggle_stays", squiggled() && code.errorCount() > 0 ? 1 : 0, 1);

        // The same bytes again ask for a check: the squiggled Q1 selected and deleted, then undone within the check's
        // rest (TextEditor drops a decoration whose text goes; its undo does not bring it back). The files are the ones
        // the drawing-only edits above left, so the check is asked, and its squiggle comes back.
        {
            const std::string kept = net.text();
            const uint64_t asked = port.checkStatus().revision;
            net.setCaret({ line.line, 0 });
            host.keys("shift+right,shift+right,backspace");
            const bool gone = !squiggled();
            for (int k = 0; k < 3 && net.text() != kept; ++k)
                host.keys("cmd+z");
            const bool back = net.text() == kept;
            for (int k = 0; k < 6; ++k)
            {
                host.tick(20);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            settleChecks(panel, host);
            std::printf("cirlayout: Q1 deleted and undone: its squiggle %s at once, the text %s, %d check(s) asked\n",
                        gone ? "went" : "stayed", back ? "the same bytes" : "not the same",
                        static_cast<int>(port.checkStatus().revision - asked));
            P.eq("cirlayout.undo_checked", back && port.checkStatus().revision > asked ? 1 : 0, 1);
            P.eq("cirlayout.undo_squiggle_back", squiggled() && code.errorCount() > 0 ? 1 : 0, 1);
        }

        // A RUN in flight lands its messages on the lines they moved to: *@ lines written mid-file, the netlist
        // included again (its Q1 an error RUN gives), RUN, and at once a write that gathers the *@ lines before .end
        // (the element lines move up); RUN's messages land on Q1's line where it is now.
        host.click(tab0.x, tab0.y);
        host.settle();
        prog.setCaret({ 0, 0 });
        prog.insert(include);
        host.click(at.x, at.y);
        host.settle();
        net.setCaret({ 1, 0 });
        net.insert("*@ part R1 30,20\n*@ part C1 26,20 r90\n");
        host.tick(2);
        const int mid = posIn(net, "Q1 ").line;
        const std::string sent = net.text();
        host.keys("cmd+return");
        const cir::Applied gathered = cir::apply(sent, cir::reconcile(sent), {});
        std::vector<funkgui::TextEdit> edits;
        for (const cir::Edit& e : gathered.edits)
            edits.push_back({ { e.from.line, e.from.col }, { e.to.line, e.to.col }, e.text });
        const bool wrote = net.applyEdits(edits) && net.text() == gathered.text;
        net.setDecorations({}, funkgui::kDiagnosticDecorations);   // only RUN's messages from here on
        settleRun(processor, host);
        waitFor(host, [&] { return !port.compiling(); });
        host.settle();
        const int now = posIn(net, "Q1 ").line;
        bool landed = false, elsewhere = false;
        for (const funkgui::Decoration& d : net.decorations())
        {
            landed = landed || (d.kind == funkgui::DecorationKind::error && d.from.line == now);
            elsewhere = elsewhere || (d.kind == funkgui::DecorationKind::error && d.from.line != now);
        }
        std::printf("cirlayout: Q1 on line %d when RUN was sent, %d after the write; RUN %s\n", mid + 1, now + 1,
                    port.status().ok ? "compiled" : "failed");
        P.eq("cirlayout.gathered", wrote && now == mid - 2 && cir::sameNetlist(sent, net.text()) ? 1 : 0, 1);
        P.eq("cirlayout.run_lands_moved", !port.status().ok && landed && !elsewhere ? 1 : 0, 1);
    }
    // ---- TYPES ------------------------------------------------------------------------------------------------------
    // The editor keeps each line's spans (FunkGui's TextEditor): a module declared after a line that names it recolours
    // that line once the index has it (CodeAssist::sync rehighlights), and the Panel draws it with no input.
    {
        funkgui::UiPreferences::get().setTheme(0);
        pg::ui::Panel panel(processor.ports(), port);
        funkgui::HeadlessHost host(panel, 0, 2.0f);
        host.settle();
        pg::ui::CodeAssist& code = panel.codeAssist();
        const funkgui::Point tab0 = panel.centreOfTab(0);
        host.click(tab0.x, tab0.y);
        funkgui::TextEditor& ed = panel.editor();
        const std::string body = pg::product::kIsInstrument
            ? "module Voice\n{\n    Thing thing;\n    Zork zork;\n    void start(float n, float v) {}\n"
              "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n"
            : "module Effect\n{\n    Thing thing;\n    Zork zork;\n"
              "    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
        ed.selectAll();
        ed.insert("// Types probe.\nmodule Thing\n{\n    float x;\n};\n" + body);
        settleChecks(panel, host);
        // The first glyph's colour in [from, to) of a line, as drawn (0: none).
        const auto inkOf = [&](const funkgui::PrimList& frame, std::string_view word) -> uint32_t {
            const funkgui::TextPos at = posIn(ed, std::string(word) + " ");
            if (at.line < 0)
                return 0;
            const funkgui::Point a = ed.pointOf(at), b = ed.pointOf({ at.line, at.col + static_cast<int>(word.size()) });
            for (const funkgui::Prim& p : frame.prims)
            {
                if (p.tag != funkgui::tags::editText || static_cast<int>(p.d2[2] + 0.5f) != static_cast<int>(funkgui::PrimKind::text))
                    continue;
                const float cx = 0.5f * (p.x0 + p.x1), cy = 0.5f * (p.y0 + p.y1);
                if (cx >= a.x && cx < b.x && cy >= a.y && cy < a.y + ed.rowHeight())
                    return p.c0;
            }
            return 0;
        };
        uint32_t thing = 0, zork = 0;
        {
            const funkgui::PrimList& frame = host.draw();
            thing = inkOf(frame, "Thing");
            zork = inkOf(frame, "Zork");
        }
        P.eq("types.drawn", thing != 0 && zork != 0 ? 1 : 0, 1);
        P.eq("types.unknown_plain", thing != zork ? 1 : 0, 1);
        // Zork declared at the end: its line above, unedited, keeps its spans until the index has the name.
        const uint32_t before = code.highlights();
        ed.setCaret(ed.endPos());
        ed.insert("module Zork\n{\n    float y;\n};\n");
        P.eq("types.stale_until_synced", inkOf(host.draw(), "Zork") == zork ? 1 : 0, 1);
        bool asked = false;
        for (int k = 0; k < 120 && code.highlights() == before; ++k)
        {
            host.tick(1, 1.0f / 12.0f);
            if (code.highlights() != before)
                asked = panel.needsDraw();           // the tick that recoloured asks for a frame
            else
                host.draw();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        P.eq("types.rehighlighted", code.highlights() != before ? 1 : 0, 1);
        P.eq("types.draw_asked", asked ? 1 : 0, 1);
        P.eq("types.recoloured", inkOf(host.draw(), "Zork") == thing ? 1 : 0, 1);
        // Claude's write_file (Claude Desktop's call, merged into the tab) while the editor does not have the keys: no
        // caret rests in it and no pointer hovers it, so the check's sync, once the files have rested, is what recolours
        // a module named before its declaration (CodeAssist::takeCheckRequest).
        {
            settleChecks(panel, host);
            host.click(4.0f, static_cast<float>(panel.height()) - 4.0f);   // beside everything: the editor loses the keys
            settleChecks(panel, host);
            std::string name;
            for (const pg::SourceFile& f : port.draft().files)
                if (f.text.find("// Types probe.") != std::string::npos)
                    name = f.name;
            std::string text = ed.text();
            const std::string field = "    Zork zork;\n";
            if (const std::size_t at = text.find(field); at != std::string::npos)
                text.insert(at + field.size(), "    Quux quux;\n");
            text += "module Quux\n{\n    float z;\n};\n";
            const uint32_t was = code.highlights();
            const pg::ChatAgent::McpResult wrote = processor.chatAgent().mcpCall(
                "write_file", pg::claude::object({ { "name", juce::String(name) }, { "content", juce::String(text) } }));
            host.tick(1, 1.0f / 12.0f);                  // merged
            P.eq("types.write_merged", !wrote.isError && !name.empty() && posIn(ed, "Quux quux;").line >= 0 ? 1 : 0, 1);
            P.eq("types.write_unfocused", ed.focused() ? 0 : 1, 1);
            P.eq("types.write_stale", inkOf(host.draw(), "Quux") == zork ? 1 : 0, 1);
            bool drawAsked = false;
            for (int k = 0; k < 120 && code.highlights() == was; ++k)
            {
                host.tick(1, 1.0f / 12.0f);
                if (code.highlights() != was)
                    drawAsked = panel.needsDraw();
                else
                    host.draw();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            P.eq("types.write_rehighlighted", code.highlights() != was && drawAsked ? 1 : 0, 1);
            P.eq("types.write_recoloured", inkOf(host.draw(), "Quux") == thing ? 1 : 0, 1);
        }

        // SHADOW: a local named as a constant drawn as a local (plain) where it is in scope, the constant's own uses in the
        // constants' colour, once the index has them (the check's sync); and a declaration wider than the hover card
        // hovered: its lines (lang::kHoverWidth), the second under its parenthesis.
        {
            const std::string mainModule = pg::product::kIsInstrument
                ? "module Voice\n{\n    void start(float n, float v) {}\n"
                  "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n"
                : "module Effect\n{\n    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
            ed.selectAll();
            ed.insert("// Shadow probe.\nenum { GAIN = 2 };\n\n"
                      "float scaled(float x)\n{\n    float GAIN = 0.5f;\n    return GAIN * x;\n}\n\n"
                      "float louder(float x)\n{\n    return x * GAIN;\n}\n\n"
                      "// spread: a declaration wider than the hover card, which shows it on two lines, the second\n"
                      "// under its parenthesis (a blank line under this, so the card has no documentation).\n\n"
                      "float spread(float x, float wet, float dry, float left, float right, float width, float depth, float mix)\n"
                      "{\n    return x;\n}\n\n" + mainModule);
            ed.setCaret({ 0, 0 });
            ed.scrollToCaret();
            settleChecks(panel, host);
            // The first glyph's colour at a place, as drawn (0: none).
            const auto inkAt = [&](const funkgui::PrimList& frame, funkgui::TextPos at) -> uint32_t {
                if (at.line < 0)
                    return 0;
                const funkgui::Point a = ed.pointOf(at), b = ed.pointOf({ at.line, at.col + 1 });
                for (const funkgui::Prim& p : frame.prims)
                {
                    if (p.tag != funkgui::tags::editText || static_cast<int>(p.d2[2] + 0.5f) != static_cast<int>(funkgui::PrimKind::text))
                        continue;
                    const float cx = 0.5f * (p.x0 + p.x1), cy = 0.5f * (p.y0 + p.y1);
                    if (cx >= a.x && cx < b.x && cy >= a.y && cy < a.y + ed.rowHeight())
                        return p.c0;
                }
                return 0;
            };
            // The inks of an identifier's plain glyph, of the constant's declaration and its use, of the local's.
            struct Inks
            {
                uint32_t plain = 0, decl = 0, constant = 0, local = 0, use = 0;
                bool localPlain() const { return plain != 0 && local == plain && use == plain; }
                bool constantColoured() const { return decl != 0 && constant == decl && decl != plain; }
            };
            const auto inks = [&](const char* when) {
                const funkgui::PrimList& frame = host.draw();
                Inks k;
                k.plain = inkAt(frame, posIn(ed, "    return x;", 11));
                k.decl = inkAt(frame, posIn(ed, "GAIN = 2"));
                k.constant = inkAt(frame, posIn(ed, "x * GAIN", 4));
                k.local = inkAt(frame, posIn(ed, "GAIN = 0.5f"));
                k.use = inkAt(frame, posIn(ed, "GAIN * x"));
                std::printf("shadow%s: plain %08x, the constant %08x and its use %08x, the local %08x and its use %08x\n", when,
                            k.plain, k.decl, k.constant, k.local, k.use);
                return k;
            };
            const Inks drawn = inks("");
            const uint32_t decl = drawn.decl;
            P.eq("shadow.local_plain", drawn.localPlain() ? 1 : 0, 1);
            P.eq("shadow.constant_coloured", drawn.constantColoured() ? 1 : 0, 1);
            const auto hoverAt = [&](funkgui::TextPos at) {
                host.move(10.0f, 10.0f);
                host.tick(2);
                const funkgui::Point over = ed.pointOf(at);
                host.move(over.x + 2.0f, over.y + ed.rowHeight() * 0.5f);
                host.tick(40);
            };
            hoverAt(posIn(ed, "spread(", 2));
            std::printf("shadow: hover %s\n", code.hoverText().c_str());
            P.eq("shadow.hover_wrapped", code.hoverOpen()
                                             && code.hoverText().find("float left, float right,\n             float width, float depth, float mix)")
                                                    != std::string::npos ? 1 : 0, 1);
            writePng(P, host, "gui.code.hover_long.png", "shadow");
            // The hover card's code draws the local's name as the editor does (lang::Hover::plain), the constant's in the
            // constants' colour: the glyphs of the card in that colour.
            const auto cardGlyphsIn = [&](uint32_t ink) {
                int n = 0;
                for (const funkgui::Prim& p : host.draw().prims)
                    n += p.tag == pg::ui::kCodePopupTag && static_cast<int>(p.d2[2] + 0.5f) == static_cast<int>(funkgui::PrimKind::text)
                             && p.c0 == ink ? 1 : 0;
                return n;
            };
            hoverAt(posIn(ed, "GAIN * x", 1));
            const bool localCard = code.hoverOpen() && code.hoverText().find("a local") != std::string::npos;
            const int localGlyphs = cardGlyphsIn(decl);
            writePng(P, host, "gui.code.hover_local.png", "shadow_card");
            hoverAt(posIn(ed, "x * GAIN", 5));
            const bool constantCard = code.hoverOpen() && code.hoverText().find("GAIN = 2") != std::string::npos;
            const int constantGlyphs = cardGlyphsIn(decl);
            std::printf("shadow: the card's glyphs in the constants' colour: the local's %d, the constant's %d\n", localGlyphs,
                        constantGlyphs);
            P.eq("shadow.card_local_plain", localCard && decl != 0 && localGlyphs == 0 ? 1 : 0, 1);
            P.eq("shadow.card_constant_coloured", constantCard && constantGlyphs >= 4 ? 1 : 0, 1);
            host.move(10.0f, 10.0f);
            host.tick(2);
            // The tab renamed (the same kind: its editor and highlighter kept) and renamed back: the shadows' flags are
            // the tab's by its name, and its highlighter follows it (CodeAssist::sync).
            const auto renameTab = [&](const std::string& to) {
                const funkgui::Point t0 = panel.centreOfTab(0);
                host.doubleClick(t0.x, t0.y);
                std::string keys;
                for (const char ch : to)
                    keys += std::string(keys.empty() ? "" : ",") + ch;
                host.keys((keys + ",return").c_str());
                settleChecks(panel, host);
                return panel.fileName(0) == to && !panel.renamingFile();
            };
            const std::string was = panel.fileName(0);
            const bool renamed = renameTab("shadow.fm");
            const Inks afterRename = inks(" (renamed)");
            P.eq("shadow.renamed_plain", renamed && afterRename.localPlain() && afterRename.constantColoured() ? 1 : 0, 1);
            const bool back = renameTab(was);
            const Inks afterBack = inks(" (renamed back)");
            P.eq("shadow.renamed_back_plain", back && afterBack.localPlain() && afterBack.constantColoured() ? 1 : 0, 1);
            host.click(4.0f, static_cast<float>(panel.height()) - 4.0f);   // beside everything: the keys as they were
            settleChecks(panel, host);
        }

        // The Code view's cost (printed, not checked: the machine is shared): a frame's tick and recording, and the
        // highlighter's lines (each runs once per edited line since FunkGui v0.25.0, not per drawn row).
        settleChecks(panel, host);
        ed.selectAll();
        ed.insert(withSvf);
        settleChecks(panel, host);
        constexpr int kFrames = 120;
        auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < kFrames; ++k)
            host.tick(1, 1.0f / 12.0f);
        const double tickMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / kFrames;
        t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < kFrames; ++k)
            (void)host.draw();
        const double drawMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / kFrames;
        int lines = 0;
        const pg::ui::CodeHighlighter hl(std::make_shared<pg::ui::TypeNames>());
        std::vector<funkgui::text::Span> spans;
        t0 = std::chrono::steady_clock::now();
        for (int pass = 0; pass < 20; ++pass)
            for (const pg::LibraryFile& f : pg::kLibrary)
                for (const std::string_view l : pg::lang::splitLines(f.source))
                {
                    spans.clear();
                    (void)hl.line(l, 0, spans);
                    ++lines;
                }
        const double lineUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / lines;
        std::printf("code view: a tick %.3f ms, a recording %.3f ms; the highlighter %.3f us a line (%d lines)\n", tickMs,
                    drawMs, lineUs, lines);
        int idle = 0;
        for (int k = 0; k < 11; ++k)
        {
            host.tick(1, 1.0f / 12.0f);
            idle += panel.needsDraw() ? 1 : 0;
        }
        P.eq("types.code_view_idle", idle, 0);
    }
    // ---- SIZE -------------------------------------------------------------------------------------------------------
    {
        namespace cs = pg::ui::codesize;
        funkgui::UiPreferences::get().setTheme(0);
        funkgui::UiPreferences::get().setInt(cs::kKey, cs::kDefault);
        pg::ui::Panel panel(processor.ports(), port);
        funkgui::HeadlessHost host(panel, 0, 2.0f);
        host.setZoom({ 100, 125, 150, 200 }, 100);
        host.settle();
        pg::ui::Panel other(processor.ports(), port);    // another editor of the plugin's, open meanwhile
        funkgui::HeadlessHost otherHost(other, 0, 2.0f);
        otherHost.settle();
        pg::ui::CodeAssist& code = panel.codeAssist();
        P.eq("size.default", panel.codeTextSize(), cs::kDefault);
        P.eq("size.default_no_status", code.sizeRect().w > 0.0f ? 1 : 0, 0);

        // The program with svf.fm and lines enough to scroll.
        const funkgui::Point tab0 = panel.centreOfTab(0);
        host.click(tab0.x, tab0.y);
        std::string text = withSvf;
        for (int k = 0; k < 240; ++k)
            text += "// filler line " + std::to_string(k) + "\n";
        panel.editor().selectAll();
        panel.editor().insert(text);
        (void)panel.addLibraryFile("svf.fm");            // there already, or added
        host.click(tab0.x, tab0.y);
        settleChecks(panel, host);
        focusEditor(panel, host);

        // A key, as the editor's host gives it: Cmd (Ctrl off the Mac) and the character.
        const auto cmdKey = [&](pg::ui::Panel& pn, char32_t ch, bool shift = false) {
            funkgui::KeyEvent e;
            e.key = funkgui::Key::character;
            e.ch = ch;
            e.mods.cmd = true;
            e.mods.shift = shift;
            pn.key(e);
        };
        // Every file tab in `tenths`, the row height FunkGui's.
        const auto allAt = [&](pg::ui::Panel& pn, int tenths) {
            bool all = pn.codeTextSize() == tenths;
            for (int i = 0; i < pn.fileCount(); ++i)
            {
                const funkgui::TextEditor& e = pn.fileEditor(i);
                all = all && std::abs(e.options().style.px - static_cast<float>(tenths) / 10.0f) < 1.0e-4f
                      && std::abs(e.rowHeight() - std::round(static_cast<float>(tenths) / 10.0f * 1.4f)) < 1.0e-4f;
            }
            return all ? 1 : 0;
        };
        constexpr int kTop = 100;
        const funkgui::TextPos kCaret{ 120, 4 };
        {
            funkgui::TextEditor& ed = panel.editor();
            ed.setScroll(0.0f, static_cast<float>(ed.rowOf({ kTop, 0 })) * ed.rowHeight());
            ed.setCaret(kCaret);
        }
        P.eq("size.top_set", panel.editor().visibleLineRange().first, kTop);
        const auto placeKept = [&] {
            const funkgui::TextEditor& ed = panel.editor();
            return ed.visibleLineRange().first == kTop && ed.caret().line == kCaret.line && ed.caret().col == kCaret.col ? 1 : 0;
        };

        // The overlays at 12 px: the completion list, the hover card, the find bar.
        struct Overlays
        {
            funkgui::Rect completion, hover, find;
            float row = 0.0f;
        };
        const auto overlays = [&]() {
            Overlays o;
            funkgui::TextEditor& ed = panel.editor();
            ed.setFocused(true);
            ed.setCaret(posIn(ed, "filter.set", 7));
            ed.scrollToCaret();
            host.keys("ctrl+space");
            host.tick(1);
            o.completion = code.completionBox();
            o.row = code.completionRowHeight();
            host.keys("escape");
            const funkgui::Point at = ed.pointOf(posIn(ed, "filter.tick", 9));
            host.move(at.x + 3.0f, at.y + ed.rowHeight() * 0.5f);
            host.tick(40);
            o.hover = code.hoverBox();
            host.move(at.x + 3.0f, at.y + 6.0f * ed.rowHeight());
            host.tick(2);
            host.keys("cmd+f");
            o.find = code.findBox();
            host.keys("escape");
            focusEditor(panel, host);
            ed.setFocused(true);
            return o;
        };
        const Overlays at12 = overlays();
        {
            funkgui::TextEditor& ed = panel.editor();
            ed.setScroll(0.0f, static_cast<float>(ed.rowOf({ kTop, 0 })) * ed.rowHeight());
            ed.setCaret(kCaret);
        }

        // The keys, and the place kept.
        cmdKey(panel, U'=');
        P.eq("size.cmd_equals", allAt(panel, 130), 1);
        P.eq("size.place_kept", placeKept(), 1);
        P.eq("size.preference", cs::read(), 130);
        cmdKey(panel, U'=', true);                       // Cmd-+: Shift-= on most keyboards
        P.eq("size.cmd_plus", allAt(panel, 140), 1);
        cmdKey(panel, U'+');                             // a keypad's or a layout's own +
        P.eq("size.cmd_plus_char", allAt(panel, 160), 1);
        cmdKey(panel, U'-');
        P.eq("size.cmd_minus", allAt(panel, 140), 1);
        cmdKey(panel, U'0');
        P.eq("size.cmd_zero", allAt(panel, 120), 1);
        P.eq("size.place_kept_back", placeKept(), 1);
        std::string up;
        for (int k = 0; k < 9; ++k)
        {
            cmdKey(panel, U'=');
            up += std::to_string(panel.codeTextSize()) + " ";
        }
        std::printf("up from 12 px: %s\n", up.c_str());
        P.eq("size.steps_up", up == "130 140 160 180 200 240 240 240 240 " ? 1 : 0, 1);
        P.eq("size.most", allAt(panel, cs::kMost), 1);
        P.eq("size.place_kept_most", placeKept(), 1);
        P.eq("size.status_shown", code.sizeRect().w > 0.0f ? 1 : 0, 1);
        otherHost.tick(2);
        P.eq("size.other_follows", allAt(other, cs::kMost), 1);
        {
            pg::ui::Panel later(processor.ports(), port);
            P.eq("size.later_reads", allAt(later, cs::kMost), 1);
        }
        host.tick(2);
        const Overlays at24 = overlays();
        std::printf("at 12 px: completion %.0f x %.0f (rows %.0f), hover %.0f x %.0f, find %.0f x %.0f\n",
                    static_cast<double>(at12.completion.w), static_cast<double>(at12.completion.h), static_cast<double>(at12.row),
                    static_cast<double>(at12.hover.w), static_cast<double>(at12.hover.h), static_cast<double>(at12.find.w),
                    static_cast<double>(at12.find.h));
        std::printf("at 24 px: completion %.0f x %.0f (rows %.0f), hover %.0f x %.0f, find %.0f x %.0f\n",
                    static_cast<double>(at24.completion.w), static_cast<double>(at24.completion.h), static_cast<double>(at24.row),
                    static_cast<double>(at24.hover.w), static_cast<double>(at24.hover.h), static_cast<double>(at24.find.w),
                    static_cast<double>(at24.find.h));
        P.near("size.completion_rows", static_cast<double>(at24.row), 2.0 * static_cast<double>(at12.row), 0.5);
        P.ge("size.completion_wider", static_cast<double>(at24.completion.w), 1.3 * static_cast<double>(at12.completion.w));
        P.le("size.completion_capped", static_cast<double>(at24.completion.w),
             std::max(330.0, static_cast<double>(code.editorRect().w)) + 0.5);
        P.ge("size.completion_taller", static_cast<double>(at24.completion.h), 1.5 * static_cast<double>(at12.completion.h));
        P.ge("size.hover_opens", at12.hover.h > 0.0f && at24.hover.h > 0.0f ? 1.0 : 0.0, 1.0);
        P.ge("size.hover_larger", static_cast<double>(at24.hover.w * at24.hover.h), 1.5 * static_cast<double>(at12.hover.w * at12.hover.h));
        P.ge("size.find_taller", static_cast<double>(at24.find.h), 1.5 * static_cast<double>(at12.find.h));
        P.ge("size.find_wider", static_cast<double>(at24.find.w), static_cast<double>(at12.find.w));
        // ... and in the picture: the completion list open at 24 px.
        {
            funkgui::TextEditor& ed = panel.editor();
            ed.setCaret(posIn(ed, "filter.set", 7));
            ed.scrollToCaret();
            host.keys("ctrl+space");
            host.tick(2);
            writePng(P, host, "gui.code.size_24.png", "size_24");
            host.keys("escape");
        }

        // The status line's size: a click puts 12 px back.
        {
            const funkgui::Rect r = code.sizeRect();
            host.click(r.centreX(), r.centreY());
            P.eq("size.status_click", allAt(panel, cs::kDefault), 1);
            P.eq("size.status_gone", code.sizeRect().w > 0.0f ? 1 : 0, 0);
            otherHost.tick(2);
            P.eq("size.other_back", allAt(other, cs::kDefault), 1);
            writePng(P, host, "gui.code.size_12.png", "size_12");
        }

        // Down to the least, and the wheel.
        focusEditor(panel, host);
        std::string down;
        for (int k = 0; k < 6; ++k)
        {
            cmdKey(panel, U'-');
            down += std::to_string(panel.codeTextSize()) + " ";
        }
        std::printf("down from 12 px: %s\n", down.c_str());
        P.eq("size.steps_down", down == "110 100 90 90 90 90 " ? 1 : 0, 1);
        P.eq("size.least", allAt(panel, cs::kLeast), 1);
        {
            const funkgui::Rect a = panel.editor().textArea();
            const float x = a.x + a.w * 0.5f, y = a.y + 40.0f;
            funkgui::Mods cmd;
            cmd.cmd = true;
            host.wheel(x, y, 0.25f, false, cmd);         // a notch away from you
            P.eq("size.wheel_up", allAt(panel, 100), 1);
            host.wheel(x, y, -0.25f, false, cmd);
            P.eq("size.wheel_down", allAt(panel, 90), 1);
            for (int k = 0; k < 3; ++k)
                host.wheel(x, y, 0.05f, true, cmd);      // a pad: three small deltas make a step
            P.eq("size.wheel_pad", allAt(panel, 100), 1);
            funkgui::Mods ctrl;
            ctrl.ctrl = true;
            host.wheel(x, y, -0.25f, false, ctrl);       // Ctrl-wheel (off the Mac, Cmd's)
            P.eq("size.wheel_ctrl", allAt(panel, 90), 1);
            const float scrolled = panel.editor().scroll().y;
            host.wheel(x, y, -0.25f, false, {});
            P.eq("size.wheel_plain_scrolls", allAt(panel, 90) == 1 && panel.editor().scroll().y > scrolled ? 1 : 0, 1);
        }
        writePng(P, host, "gui.code.size_9.png", "size_9");

        // The IR view: drawn in the size, its keys stepping it.
        panel.openIrView(0);
        host.settle();
        P.eq("size.ir_shown", panel.irShown() ? 1 : 0, 1);
        P.eq("size.ir_drawn_in_it", std::abs(panel.editor().options().style.px - 9.0f) < 1.0e-4f ? 1 : 0, 1);
        cmdKey(panel, U'=');
        P.eq("size.ir_key", allAt(panel, 100) == 1 && std::abs(panel.editor().options().style.px - 10.0f) < 1.0e-4f ? 1 : 0, 1);
        panel.closeIrView();
        host.settle();

        // A netlist's Schematic view keeps Cmd-= and Cmd-- for its zoom.
        P.eq("size.cir_tab", panel.addLibraryFile("screamer.cir") ? 1 : 0, 1);
        for (int i = 0; i < panel.fileCount(); ++i)
            if (panel.fileName(i) == "screamer.cir")
            {
                const funkgui::Point t = panel.centreOfTab(i);
                host.click(t.x, t.y);
            }
        host.settle();
        const funkgui::Point pill = panel.centreOfViewPill(true);
        host.click(pill.x, pill.y);
        host.settle();
        P.eq("size.schematic_shown", panel.schematicShown() ? 1 : 0, 1);
        const float zoomWas = panel.schematic().zoom();
        cmdKey(panel, U'=');
        host.settle();
        std::printf("schematic zoom %.2f -> %.2f\n", static_cast<double>(zoomWas), static_cast<double>(panel.schematic().zoom()));
        P.eq("size.schematic_zooms", panel.schematic().zoom() > zoomWas && panel.codeTextSize() == 100 ? 1 : 0, 1);
        cmdKey(panel, U'-');
        P.eq("size.schematic_zooms_out", panel.codeTextSize() == 100 ? 1 : 0, 1);
        panel.setSchematic(false);
        host.settle();

        // PAPER at each size.
        for (const int tenths : { 90, 120, 240 })
        {
            funkgui::UiPreferences::get().setTheme(1);
            cs::write(tenths);
            pg::ui::Panel paper(processor.ports(), port);
            funkgui::HeadlessHost paperHost(paper, 1, 2.0f);
            paperHost.settle();
            const funkgui::Point t0 = paper.centreOfTab(0);
            paperHost.click(t0.x, t0.y);
            paperHost.settle();
            const std::string name = "gui.code.size_" + std::to_string(tenths / 10) + ".paper.png";
            writePng(P, paperHost, name.c_str(), "size_paper_" + std::to_string(tenths / 10));
        }
        funkgui::UiPreferences::get().setTheme(0);
        cs::write(cs::kDefault);
    }
    funkgui::UiPreferences::get().setTheme(0);
    return P.finish();
}
