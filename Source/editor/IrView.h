// Source/editor/IrView.h: the IR view, a read-only tab of the program's card showing what FMinus made of the program,
// as LLVM IR (dsp/Compiler.h, "The IR"). One tab, at the right end of the files' tabs, which is never one of the
// program's files: not in the draft, the session, a project or an export, read by no #include, counted by no rule of
// the files' tabs (their number, their names); closing it is closing a tab. The explorer's LLVM IR section opens it, a
// row a view:
//
//   optimised  <program>.ll            what the JIT made the program's machine code from: FMinus's -O2 pipeline, its
//                                      own passes among them (what `fminus --emit ll` prints)
//   lowered    <program>.lowered.ll    as FMinus's code generator wrote it for this machine, before any pass ran
//   frozen     <program>.frozen.ll     the frozen copy's, optimised: the program compiled again with its dials as
//                                      constants (Patch.h, "Frozen dials"); its row shows while the copy plays
//
// Each view is a funkgui::TextEditor of its own, read only (it moves, selects, copies, finds and goes to a line; typing,
// pasting, undo and the line commands change nothing), highlighted as LLVM IR (IrHighlighter, in the code's palette),
// its caret and scroll kept while the tab is open; FMinus's language features (completion, signature help, hover,
// squiggles, Format, the check while typing) are off in it (CodeAssist.h). Its text is a few comment lines saying what
// it is and how the program stands (nothing compiled yet, its IR being made, the last RUN failed, the editor's files
// changed since, the frozen copy not playing, the text cut), then LLVM's text, brought up to date after each RUN that
// compiles and whenever the frozen copy changes. While the tab is open its Panel is an IR viewer
// (EditorPort::addIrViewer): the next compile keeps the IR, or the compiler's thread makes it for the program that
// plays; closed, nothing is kept.
#pragma once

#include "dsp/Compiler.h"
#include "dsp/Controls.h"

#include <funkgui/core/Theme.h>
#include <funkgui/text/Highlighter.h>
#include <funkgui/widgets/TextEditor.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace playground::ui::irview
{
    enum class View : uint8_t { optimised, lowered, frozen };
    inline constexpr int kViews = 3;

    // The view's name, from the program's file ("program.fm"): "program.ll", "program.lowered.ll", "program.frozen.ll".
    std::string name(View v, std::string_view programFile);
    // Its word, as the explorer's row has it at its right: "optimised", "lowered", "frozen".
    const char* word(View v);

    // What a view's text is made of: the IR there is, the last compile's outcome, whether the frozen copy plays and
    // whether the editor's files differ from the program that plays.
    struct Source
    {
        const IrStatus* ir = nullptr;
        const CompileStatus* status = nullptr;
        bool frozenPlays = false;
        bool edited = false;
        const Controls* controls = nullptr;          // the dials of the program that plays (the frozen copy's values)
    };
    // The view's text (above): its comment lines, then LLVM's text.
    std::string text(View v, std::string_view programFile, const Source& from);

    // A view's editor made one (read only, the code's font, line numbers, the bracket pair, the minimap, its
    // highlighter), in the code's palette for `theme`.
    void configure(funkgui::TextEditor& editor, const funkgui::Theme& theme);

    // LLVM IR, a line at a time (no state: nothing in it spans lines), in CodeAssist.h's styles: instructions as
    // keywords, types, %locals as members, @globals and labels as functions, comments, numbers and constants, strings,
    // the module's own words (define, declare, target, attributes, ...) as directives, and attributes, flags and
    // metadata (#0, !tbaa) faint.
    class IrHighlighter final : public funkgui::text::Highlighter
    {
    public:
        uint32_t line(std::string_view line, uint32_t stateIn, std::vector<funkgui::text::Span>& out) const override;
    };
}
