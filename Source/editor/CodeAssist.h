// Source/editor/CodeAssist.h: what makes the program's tabs (a funkgui::TextEditor each, Panel.cpp's FilesItem) a code
// editor, as a modern one is: FMinus's language service (fminus/Lang.h) over the tabs' files, and FunkGui's code-editor
// hooks (TextEditor.h, v0.22.0) in the plugin's look. The chat's message box is a TextEditor too, and none of this.
//
//   the look       line numbers, a column for the problems' marks, the caret's line, indent guides, the bracket pair at
//                  the caret, a minimap; FMinus highlighted by its own lexer (keywords, types and the program's module
//                  names, functions, members, numbers, comments, directives, attributes) in inks of the theme
//   keys           ( [ { and the quotes close themselves, Return after an opener indents and a closer outdents; and
//                    Ctrl-Space          completion (it also opens on a typed name, after `.` and `->`); Up, Down,
//                                        Page Up/Down choose, Return or Tab accept, Escape closes
//                    Shift-Cmd-Space     signature help (it also opens on `(` and `,` in a call)
//                    Shift-Alt-F         Format Document (fminus/Lang.h's format; the Format on Run switch in the
//                                        status line formats every FMinus tab before RUN)
//                    Cmd-F, Cmd-Alt-F    find, find and replace (the bar at the editor's top right: Return the next,
//                                        Shift-Return the one before, Tab the other field, in the replace field Return
//                                        replaces one and Alt-Return all, Escape closes); Cmd-G and Shift-Cmd-G the
//                                        next and the one before from the text too
//                    Cmd-/               the lines' // comments on or off
//                    Alt-Up, Alt-Down    the lines moved; with Shift, copied
//                    Shift-Cmd-K         the lines deleted;  Cmd-L the line selected;  Cmd-D the word, then its next
//                    Cmd-] and Cmd-[     the lines indented, outdented
//                    Cmd-B, Cmd-click    go to the definition (another tab's too); F12 does not reach a plugin
//                                        (FunkGui delivers no function keys)
//                    Ctrl-G              go to a line (and a column: 12:5), in a bar where the find bar sits; a click
//                                        on the status line's place opens it too
//   pointer        the word under the pointer, still for half a second: a card of what it is (its declaration, its
//                  documentation) and of the problems there
//   problems       squiggles and marks from the compiler's messages: RUN's (the files it compiled, as long as a tab
//                  still holds what was compiled), and a check while typing: 0.6 s after the last edit, the files go to
//                  the compiler's thread (EditorPort::check: FMinus's pass over the text and the main module's
//                  methods, without the code, the instances or a swap of what plays), and its messages replace them
//                  (RUN's again when the files are what RUN compiled: they say more; RUN itself cancels a check due)
//   the status     under the editor: the problems' count (a click jumps to the next), Checking..., the caret's line and
//   line           column, the indent, the language and the Format on Run switch (a machine-wide preference)
//
// The completion list, the signature and the hover card are drawn over everything (drawOverlays, after the Panel's
// items); the find bar and the status line with the editor. Message thread only.
//
// A view (the IR tab, IrView.h: the one past the files, Tabs::active() == Tabs::count()) is none of the program's files:
// no language feature reaches it (completion, signature help, hover, occurrences, the problems, Format, go to the
// definition) and it is read only (TextEditorOptions::readOnly): a key that would edit it (typing, Return, Backspace,
// Delete, cut, paste, undo, the line commands) changes nothing, is taken (never the host's) and says so in the status;
// it moves, selects, copies, finds (no replace) and goes to a line. Its status line says it is read only and LLVM IR.
#pragma once

#include "editor/Theme.h"
#include "fminus/Lang.h"

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/Input.h>
#include <funkgui/text/Highlighter.h>
#include <funkgui/text/LineEdit.h>
#include <funkgui/widgets/TextEditor.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace funkgui
{
    class HostServices;
}

namespace playground::ui
{
    // The code editor's own primitives, for probes: the status line and the find bar; the completion list, the
    // signature and the hover card.
    inline constexpr funkgui::Tag kCodeTag = funkgui::tags::firstProduct + 20;
    inline constexpr funkgui::Tag kCodePopupTag = funkgui::tags::firstProduct + 21;

    // The FMinus highlighter's styles (the palette's indices).
    enum CodeStyle : uint16_t
    {
        csPlain = 0, csKeyword, csType, csComment, csNumber, csOperator, csDirective, csString, csFunction, csMember,
        csAttribute, csPunct, csCount
    };

    // The module names the highlighter shows as types, shared with it (the language service's, kept up to date).
    struct TypeNames
    {
        std::vector<std::string> names;          // sorted
    };

    // FMinus by lang::lexLine, a line at a time (the state: inside a block comment).
    class CodeHighlighter final : public funkgui::text::Highlighter
    {
    public:
        explicit CodeHighlighter(std::shared_ptr<const TypeNames> types) : types_(std::move(types)) {}
        uint32_t line(std::string_view line, uint32_t stateIn, std::vector<funkgui::text::Span>& out) const override;

    private:
        std::shared_ptr<const TypeNames> types_;
    };

    // The highlighter's palette for a theme (GRAPHITE or PAPER): the theme's inks, and a few colours of its own.
    std::vector<funkgui::text::HighlightStyle> codePalette(const funkgui::Theme& th);

    // A compiler message, as the problems list parses it (Panel.h's Problem, without the editor's types).
    struct Diagnostic
    {
        enum class Severity : uint8_t { error, warning, info };
        std::string file;
        int line = 0, column = 0;                // 1-based; 0: no place
        Severity severity = Severity::error;
        std::string text;
    };

    class CodeAssist
    {
    public:
        // The tabs, as the assistant reaches them (FilesItem's).
        struct Tabs
        {
            std::function<int()> count;
            std::function<int()> active;
            std::function<funkgui::TextEditor&(int)> editor;
            std::function<std::string(int)> name;
            std::function<void(int)> show;                                        // shows a tab
            std::function<bool(const std::string&, const std::string&)> addQuiet; // a file as a tab, not shown
            std::function<void(const std::string&)> notice;                       // a word in the status
        };

        explicit CodeAssist(Tabs tabs);
        ~CodeAssist();
        CodeAssist(const CodeAssist&) = delete;
        CodeAssist& operator=(const CodeAssist&) = delete;

        // A tab's editor made a code editor for this file (an FMinus file or a .gui file: its highlighter, options).
        void configure(funkgui::TextEditor& ed, std::string_view fileName);
        void setTheme(const funkgui::Theme& th);         // every tab's palette follows it
        void setLibrary(const Sources& library);         // the library's files (completion offers their modules)
        void setInstrument(bool on);                     // the product's kind (the contract's snippets)
        void attach(funkgui::HostServices* host);        // the find bar's paste

        // The tabs' editors share `area`: the editor, and the status line under it.
        void setArea(const funkgui::Rect& area);
        funkgui::Rect editorRect() const;
        funkgui::Rect statusRect() const;

        // Input before the editor's: true when the assistant used it.
        bool key(const funkgui::KeyEvent&);
        bool pointerDown(const funkgui::PointerEvent&);
        bool wheel(const funkgui::WheelEvent&);
        bool over(funkgui::Point) const;                 // over an overlay or the status line (not the text)
        std::optional<funkgui::Cursor> cursorAt(funkgui::Point) const;
        bool takesKeys() const noexcept { return findFocus_ || gotoOpen_; }   // the find or go-to bar has the keys

        void tick(float dt, funkgui::Point pointer, bool focused);
        bool settled() const;
        void tabShown();                                 // another tab: the overlays close
        void focusChanged(bool focused);

        void drawGround(funkgui::Canvas&, const funkgui::Theme&) const;     // under the text: the editor's ground
        void drawInEditor(funkgui::Canvas&, const funkgui::Theme&) const;   // the find bar and the status line
        void drawOverlays(funkgui::Canvas&, const funkgui::Theme&, float panelW, float panelH) const;
        bool specLine(char* out, std::size_t n, funkgui::Point pointer) const;   // the footer's, over the status line

        // ---- problems ---------------------------------------------------------------------------------------------
        // Messages for `compiled`: squiggles and marks in each tab whose text was compiled. RUN's are kept: a check of
        // the same files shows them again instead of its own, which are fewer (no notes, nothing only RUN's second
        // pass finds).
        void setRunDiagnostics(const std::vector<Diagnostic>& messages, const Sources& compiled);
        void setCheckDiagnostics(const std::vector<Diagnostic>& messages, const Sources& compiled);
        // A check is due: the files changed and have rested (true once for each such rest).
        bool takeCheckRequest();
        void cancelCheck();                              // RUN: the files as they are need no check
        void checkSent() noexcept { checking_ = true; }
        void checkDone() noexcept { checking_ = false; }
        bool checking() const noexcept { return checking_; }
        // A check's outcome not seen yet (CheckStatus::revision): true once for each.
        bool takeCheckResult(uint64_t revision) noexcept
        {
            if (revision == checkSeen_)
                return false;
            checkSeen_ = revision;
            return true;
        }
        int errorCount() const;                          // the squiggles in every tab, by severity
        int warningCount() const;

        // ---- commands ---------------------------------------------------------------------------------------------
        bool formatFile();                               // the tab shown (an FMinus file); false with a notice when not
        int formatAll();                                 // every FMinus tab; how many changed
        bool formatOnRun() const;
        void setFormatOnRun(bool on);
        void openCompletion(bool explicitly);
        void openSignature();
        void openFind(bool replace);
        void closeFind();
        void setFindText(const std::string& text);       // probes: as typed in the find field
        void setReplaceText(const std::string& text);
        bool findNext(bool forward);
        bool replaceOne();
        int replaceAll();
        bool toggleComment();
        bool moveLines(bool down);
        bool copyLines(bool down);
        bool deleteLines();
        bool goToDefinition();                           // the word at the caret
        bool goToDefinitionAt(funkgui::Point p);
        void openGoTo();                                 // Ctrl-G: the go-to-line bar, its field empty
        void closeGoTo();
        bool goToLine(const std::string& typed);         // "12" or "12:5" (1-based): the caret there; false for none

        // ---- probes -----------------------------------------------------------------------------------------------
        bool completionOpen() const noexcept { return completion_.has_value(); }
        const lang::CompletionList& completion() const;
        int completionChoice() const noexcept { return choice_; }
        bool signatureOpen() const noexcept { return signature_.has_value(); }
        const lang::Signature& signature() const;
        bool hoverOpen() const noexcept { return hover_.has_value(); }
        std::string hoverText() const;                   // its code and documentation, and the problems there
        bool findOpen() const noexcept { return findOpen_; }
        bool goToOpen() const noexcept { return gotoOpen_; }
        int findCount() const noexcept { return static_cast<int>(matches_.size()); }
        int findCurrent() const noexcept { return current_; }
        lang::Service& service();                        // the index, brought up to date

    private:
        struct Diag
        {
            funkgui::TextRange range;
            funkgui::DecorationKind kind = funkgui::DecorationKind::error;
            std::string text;
        };
        struct DiagSet
        {
            uint32_t revision = 0;                       // the editor's when they were set
            std::vector<Diag> list;
        };

        funkgui::TextEditor& ed() const;
        int activeTab() const;
        bool fmFile(int tab) const;                      // one of the program's FMinus files (not a .gui, not a view)
        bool viewShown() const;                          // the tab shown is a view (the IR): no file of the program
        bool goToKey(const funkgui::KeyEvent& e);
        void sync();                                     // the service follows the tabs
        uint64_t filesRevision() const;
        lang::Pos pos(funkgui::TextPos p) const noexcept { return { p.line, p.col }; }
        void afterEdit(const funkgui::KeyEvent& e, uint32_t before, funkgui::TextPos caretBefore);
        void refreshCompletion();
        void accept();
        void closeOverlays();
        void updateOccurrences();
        void updateMatches();
        void showMatch(int k);
        bool findKey(const funkgui::KeyEvent& e);
        std::vector<int> selectedLines(int& first, int& last) const;
        void setDiagnostics(const std::vector<Diagnostic>& messages, const Sources& compiled);
        std::vector<std::string> diagnosticsAt(int tab, funkgui::TextPos p) const;

        // overlay geometry
        struct Popup
        {
            funkgui::Rect box, list, detail;
            int rows = 0;
        };
        Popup popupGeometry(float panelW, float panelH) const;
        funkgui::Rect findBarRect() const;
        funkgui::Rect goToBarRect() const;
        struct FindParts
        {
            funkgui::Rect bar, toggle, field, caseBtn, wordBtn, count, prev, next, close, replaceField, replaceBtn, allBtn;
        };
        FindParts findParts() const;
        struct StatusParts
        {
            funkgui::Rect problems, position, indent, language, formatOnRun;
        };
        StatusParts statusParts() const;

        Tabs tabs_;
        lang::Service service_;
        uint64_t synced_ = ~uint64_t{ 0 };
        std::shared_ptr<TypeNames> types_ = std::make_shared<TypeNames>();
        funkgui::HostServices* host_ = nullptr;
        funkgui::Rect area_{};
        bool themeDark_ = true;
        bool hasTheme_ = false;
        std::vector<funkgui::text::HighlightStyle> palette_;
        funkgui::Col note_ = codeSecond(funkgui::Theme::graphite());   // a note's ink, the theme's (setTheme)
        bool focused_ = false;
        mutable float panelW_ = 0.0f, panelH_ = 0.0f;    // the last overlays' bounds (drawOverlays keeps them)
        Sources library_;

        // diagnostics, by file name
        std::map<std::string, DiagSet> diags_;
        std::vector<Diagnostic> runMessages_;            // the last RUN's, and the files it compiled
        Sources runSources_;
        bool checking_ = false;
        uint64_t checkSeen_ = 0;
        uint64_t editedRevision_ = 0;                    // the files' revision at the last edit seen
        float sinceEdit_ = 0.0f;
        bool checkDue_ = false;

        // completion, signature, hover, occurrences
        std::optional<lang::CompletionList> completion_;
        bool explicit_ = false;
        int choice_ = 0;
        mutable int scrollRow_ = 0;                      // the list's first row shown (drawing keeps the choice in it)
        std::optional<lang::Signature> signature_;
        funkgui::TextPos signatureAt_{};
        std::optional<lang::Hover> hover_;
        std::vector<std::string> hoverProblems_;
        funkgui::Point hoverPoint_{};
        funkgui::Point lastPointer_{ -1.0e4f, -1.0e4f };
        float still_ = 0.0f;
        funkgui::TextPos occurrencesAt_{ -1, -1 };
        uint32_t occurrencesRev_ = 0;
        float caretStill_ = 0.0f;
        funkgui::TextPos lastCaret_{ -1, -1 };

        // find and replace
        bool findOpen_ = false, replaceOpen_ = false, findFocus_ = false;
        int findField_ = 0;                              // 0 find, 1 replace
        funkgui::text::LineEdit findEdit_, replaceEdit_;
        bool matchCase_ = false, wholeWord_ = false;
        std::vector<funkgui::TextRange> matches_;
        int current_ = -1;
        uint32_t matchesRev_ = 0;
        int matchesTab_ = -1;
        std::string matchesNeedle_;

        // go to a line
        bool gotoOpen_ = false;
        funkgui::text::LineEdit gotoEdit_;
        bool gotoBad_ = false;                           // the line typed is none of the text's
    };
}
