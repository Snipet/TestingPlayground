// Source/fminus/Lang.h: FMinus's language service (JUCE-free, FunkGui-free, FMinus-free): what a code editor (the
// plugin's, Source/editor/CodeAssist.h) or a tool (the chat's, the MCP server's) knows about a program's text without
// compiling it. Plain C++20 over a program's files (Sources.h), positions as the editor has them: a 0-based line and a
// byte offset into it (the editor's text is ASCII).
//
//   lexLine(), lex()   FMinus's tokens, as its scanner (Lexer.l) reads them: identifiers, its keywords (C's that it
//                      has, and module, param, parallel, this, restrict, sizeof), its type words (float, int, bool,
//                      void, float2, float4, float8, poly), numbers (C's: 1.5f, .5, 2e3, 0x1F, 0x1p-3f), operators,
//                      punctuation, // and /* */ comments (a block comment spans lines: lexLine carries the state) and
//                      directives (a line whose first non-blank character is #: #include "file" and #pragma, one
//                      token each). FMinus has no strings: a quote outside a directive, and any other character
//                      FMinus does not read, is `unknown`.
//   lexCirLine(),      a circuit netlist's tokens (a .cir file, SPICE's: fminus/Circuits.h), on the same kinds: a
//   lexCir()           comment (a line whose first non-blank character is *, and ; to the line's end), a directive
//                      (.title, .pot, ...: the word), a keyword (an element's name, the line's first word: R1, Ptone,
//                      XU1), a number (SPICE's values: 2.2k, 10meg, 1u, -60dB; 1N914 is a name), punctuation ({ } ( )
//                      = , : and a continuation line's leading +), an operator (+ - * / ^ inside braces) and the
//                      identifiers (nodes, pots, models, keys). Never FMinus's lexer: a netlist is no FMinus.
//   Service            an index of the program's files, host.fm (the plugin's own file, Patch::hostSource) and the
//                      library's (Library.h, offered by name), rebuilt when a file changes. A netlist (a .cir file,
//                      the program's or the library's) is read line by line into the one module it generates: its
//                      name (.title, else the file's stem, its first letter capitalised), its documentation (the *
//                      lines it starts with), and the methods every configuration has, as the generated module
//                      declares them: init(float rate), set(its pots' floats, then its switches' ints; none without
//                      either), tick(the signal, then the law inputs: float2 and floats with .channels 2) and clear()
//                      (step and the fields depend on the configuration, which the self-test may change: left out);
//     symbols          every declaration: modules, their fields, params and methods, functions, their arguments and
//                      the locals of their bodies, each with its type, signature, place, the scope it is seen in and
//                      its documentation (the comment lines over it, else the one after it on its line; a module's
//                      member without either: the lines of its module's usage header that name it, `filter.lp` or
//                      `host->bpm`)
//     complete()       what may be typed at a place: after `.` or `->` the members of what is before it (a module's
//                      fields, params and methods, a vector's lanes); after `[[` the attributes; in `#include "` the
//                      files; after `#pragma ` its words; else the locals and arguments in scope, the module's own
//                      members (in a method), the functions and modules of the files the program reads, the
//                      library's modules (which add their #include), the builtins with their signatures, the
//                      keywords and type words and a few snippets (a for loop, the contract's methods). Ranked as an
//                      editor does: what starts with the typed prefix (its case first), then what holds its letters in
//                      order; nearer scopes first
//     signature()      in a call's parentheses: the function's, the method's or the builtin's signature, its
//                      parameters' places in it and the one the caret is in (a vector literal's lanes too)
//     hover()          what a word is: its declaration as code and its documentation; a keyword's meaning
//     definition()     where the word under a place is declared (another file's too); an #include line: that file
//     occurrences()    the places of the word under a place that name the same declaration, in its file
//   circuitDials()     the dials a program's circuits get (Patch.h's "Circuits"), over the files FMinus compiles (the
//                      first and what it #includes, in turn): each field of the main module whose type is a netlist's
//                      module (a scalar or an array), that netlist's pots and switches read line by line (their
//                      defaults, a switch's positions), whether the program calls that module's set() itself (a member
//                      call named set, in its FMinus files, whose receiver is of that module, or one whose
//                      receiver is not known: the test is conservative, so a pot is never driven twice) and the dials'
//                      names (the pot's when no other dial has it, else <field>_<pot>). The plugin's compile (Patch)
//                      and the GUI sandbox both take them from here, so a panel drawn for a circuit program has its dials
//   format()           the house style of the examples and the library (LangFormat.cpp): Allman braces, four spaces
//                      a level, a single statement under its if/for/while/else a level deeper, continuation lines
//                      under their open parenthesis (else a level deeper), C's spacing (binary operators and commas
//                      spaced, unary and member operators tight, `float* p`, `(int)x`, `if (`, `f(`), trailing
//                      blanks and runs of blank lines removed, a comment after code kept at its column where it fits.
//                      Lines are only re-indented, re-spaced and broken before a `{` or after a `}`: never joined. It
//                      checks its own work: the formatted text's tokens are the original's, in order and with the same
//                      spelling, and every directive and comment is the same text, or it returns the text unchanged
//                      with why. Idempotent: formatting its output again changes nothing.
//
// Every query is pure and fast (a program's files are small: the index is rebuilt in well under a millisecond) and
// runs on the calling thread; nothing here allocates on, or is meant for, the audio thread.
#pragma once

#include "dsp/Controls.h"
#include "dsp/Sources.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playground::lang
{
    struct Pos
    {
        int line = 0;
        int col = 0;

        friend constexpr bool operator==(Pos a, Pos b) noexcept { return a.line == b.line && a.col == b.col; }
        friend constexpr bool operator<(Pos a, Pos b) noexcept { return a.line < b.line || (a.line == b.line && a.col < b.col); }
        friend constexpr bool operator<=(Pos a, Pos b) noexcept { return !(b < a); }
    };

    struct Range
    {
        Pos from{};
        Pos to{};
        friend constexpr bool operator==(const Range& a, const Range& b) noexcept { return a.from == b.from && a.to == b.to; }
    };

    // ---- the lexer --------------------------------------------------------------------------------------------------
    enum class Tok : uint8_t { identifier, keyword, type, number, op, punct, comment, directive, unknown };

    struct Token
    {
        Tok kind = Tok::unknown;
        int line = 0, col = 0, len = 0;          // a line's bytes [col, col + len)
        std::string_view text{};                 // into the text lexed
        bool open = false;                       // a comment: a block comment not closed on this line
    };

    // One line's tokens appended to `out` (no line break in `line`). `inComment`: whether the line starts inside a
    // block comment; left saying whether the next line does.
    void lexLine(std::string_view line, int lineNo, bool& inComment, std::vector<Token>& out);
    std::vector<Token> lex(std::string_view text);           // every line's, in order
    std::vector<std::string_view> splitLines(std::string_view text);   // at '\n' (one more than the line breaks)

    bool isKeyword(std::string_view word) noexcept;
    bool isTypeWord(std::string_view word) noexcept;
    bool isBuiltin(std::string_view word) noexcept;
    bool isIdentChar(char c) noexcept;

    // `#include "name"` on this line: the name and its columns (inside the quotes); false for any other line.
    bool includeOn(std::string_view line, std::string& name, int& from, int& to);

    // A netlist's line's tokens appended to `out` (no line break in `line`; a netlist has no state across lines: a
    // continuation line's leading + is punctuation), and every line's, in order.
    void lexCirLine(std::string_view line, int lineNo, std::vector<Token>& out);
    std::vector<Token> lexCir(std::string_view text);
    // A netlist's number as FMinus's circuit compiler reads it ([+-]digits[.digits][e[+-]digits], then a suffix t g
    // meg k m u n p f in any case, meg before m, then letters it ignores: 2.2k, 10meg, 1u, 100nF): nothing for none.
    std::optional<double> spiceNumber(std::string_view text);

    // ---- the index --------------------------------------------------------------------------------------------------
    enum class Kind : uint8_t
    {
        keyword, type, module, function, method, field, param, argument, local, builtin, snippet, file, attribute,
        lane, word
    };
    const char* kindName(Kind k) noexcept;                   // "module", "method", ... (completion's detail, tests)

    struct Symbol
    {
        Kind kind = Kind::local;
        std::string name;
        std::string type;                        // its type ("float", "Svf", "float*", "float[16]"); a function's result
        std::string signature;                   // its declaration as code: "float tick(float x)", "param float q [[...]]"
        std::vector<std::string> params;         // a function's or method's parameters, as declared ("float x")
        std::string container;                   // the module of a field, param or method
        std::string doc;                         // its documentation, plain lines
        int file = -1;                           // Service::files() index
        Pos at{};                                // its name
        Range scope{};                           // where a local or an argument is seen (its block, from its name)
        bool body = false;                       // a function or method with a body (not a prototype)
    };

    struct Completion
    {
        std::string label;
        Kind kind = Kind::keyword;
        std::string detail;                      // its type or signature, one line
        std::string code;                        // its declaration as code ("Svf filter"), when it has one
        std::string doc;
        std::string insert;                      // what accepting it types; "$0" marks where the caret goes
        std::string include;                     // a library file it needs: its #include and its tab ("svf.fm")
        int rank = 0;                            // lower first (the order it is listed in)
    };

    struct CompletionList
    {
        Range replace{};                         // the typed prefix the chosen item replaces
        std::string prefix;
        std::vector<Completion> items;
        bool member = false;                     // after `.` or `->`
    };

    struct Signature
    {
        std::string label;                       // "float fminf(float x, float y)"
        std::vector<std::pair<int, int>> params; // each parameter's bytes [from, to) in label
        int active = 0;                          // the parameter the place is in
        std::string doc;
    };

    struct Hover
    {
        Range range{};                           // the word
        std::string code;                        // its declaration, as code
        std::string doc;
    };

    struct Location
    {
        int file = -1;                           // Service::files() index
        std::string fileName;
        Pos at{};
    };

    struct Options
    {
        bool instrument = false;                 // the product's kind: the contract's snippets (Voice or Effect)
    };

    // ---- a program's circuits as dials (Patch.h's "Circuits"; circuitDials() below) ----------------------------------
    struct CircuitPot
    {
        std::string name;                        // .pot NAME, as written: set()'s float argument, in .pot order
        float def = 0.5f;                        // its default (the line's, else 0.5)
        std::string taper;                       // its taper word as written ("audio"; "" when none): set()'s, never
                                                 // the dial's, which is linear

        friend bool operator==(const CircuitPot&, const CircuitPot&) = default;
    };

    struct CircuitSwitch
    {
        std::string name;                        // .switch NAME: set()'s int argument, after the pots
        int positions = 2;                       // 2 to 8: 0 to positions - 1
        int def = 0;                             // its default position

        friend bool operator==(const CircuitSwitch&, const CircuitSwitch&) = default;
    };

    struct CircuitBinding
    {
        std::string field;                       // the main module's field ("amp")
        std::string module;                      // its type: the module the netlist generates ("Screamer")
        std::string file;                        // the netlist ("screamer.cir")
        std::vector<int> dims;                   // a scalar's none, amp[2]'s { 2 }; an extent Lang cannot read: 0
        std::vector<CircuitPot> pots;
        std::vector<CircuitSwitch> switches;
        bool own = false;                        // the program calls this module's set() itself: no dials
        std::string why;                         // then the note that says so ("program.fm:31: Screamer's pots have
                                                 // no dials: the program calls its set()")
        std::vector<std::string> names;          // the dials' names, each pot's then each switch's (empty when own)
        std::vector<int> slots;                  // each dial's control slot (assignSlots, Patch's), -1 for none: past
                                                 // kMaxControls, or a binding the compile found otherwise

        int count() const noexcept;              // its elements (1 for a scalar; 0 when an extent is unknown)
        int args() const noexcept { return static_cast<int>(pots.size() + switches.size()); }
        bool dialled() const noexcept;           // a dial drives it: not own, and a slot is not -1
        // An element's path: amp[1] (amp for a scalar), element k in row-major order.
        std::string path(int k) const;

        friend bool operator==(const CircuitBinding&, const CircuitBinding&) = default;
    };

    struct CircuitDials
    {
        std::string module, file;                // the main module (Effect or Voice) and its file; "" when none
        std::vector<std::string> params;         // its own params, in order: the dials before the circuits'
        std::vector<CircuitBinding> bindings;    // its fields of a netlist's module, in declaration order
    };

    class Service
    {
    public:
        Service();
        ~Service();
        Service(const Service&) = delete;
        Service& operator=(const Service&) = delete;

        void setOptions(const Options& o);
        // The program's files (the first the program), re-indexed where they changed. host.fm follows them (the
        // plugin's own file, unless one of them has its name).
        void setFiles(const Sources& files);
        // The library's files (Library.h): their modules are offered by name in a program that does not include them,
        // and accepting one adds its #include (Completion::include).
        void setLibrary(const Sources& library);

        struct File
        {
            std::string name;
            std::string text;
            bool program = false;                // one of setFiles' (else host.fm, read-only)
        };
        const std::vector<File>& files() const noexcept;
        int fileIndex(std::string_view name) const noexcept;    // -1 for none

        CompletionList complete(int file, Pos at) const;
        std::optional<Signature> signature(int file, Pos at) const;
        std::optional<Hover> hover(int file, Pos at) const;
        std::optional<Location> definition(int file, Pos at) const;
        std::vector<Range> occurrences(int file, Pos at) const;

        // Every declaration of every file (in file order), and the module names known (sorted): the highlighter's
        // and the formatter's types.
        const std::vector<Symbol>& symbols() const noexcept;
        const std::vector<std::string>& typeNames() const noexcept;
        // The word (identifier) at a place: its range, empty when none touches it.
        Range wordAt(int file, Pos at) const;
        // The program's circuits as dials (circuitDials() below).
        CircuitDials circuitDials(bool instrument) const;

    private:
        struct Impl;
        Impl* impl_;
    };

    // The dials of the program `s` holds (its files set; `instrument` names the main module: Voice, else Effect), Lang.h's
    // header: the bindings' slots left empty (assignSlots).
    CircuitDials circuitDials(const Service& s, bool instrument);
    // Each binding's slots (CircuitBinding::slots) after the first `taken` controls (the program's own params with
    // dials): in binding order, each pot then each switch, -1 past kMaxControls; an own binding's all -1.
    void assignSlots(CircuitDials& d, int taken);

    // A circuit dial's Control (Controls.h), as the plugin's slot holds it: a pot a real 0..1 at its default, shown 0 to
    // 100 % (scale 100, no decimals), linear (the taper is set()'s); a switch an int over its positions at its default.
    // Patch makes the plugin's from these, the GUI sandbox its mocks.
    Control potControl(const std::string& name, float def);
    Control switchControl(const std::string& name, int positions, int def);

    // ---- the formatter (LangFormat.cpp) -----------------------------------------------------------------------------
    struct Formatted
    {
        std::string text;                        // the formatted text (the original when !ok)
        bool ok = true;                          // false: the text left as it was, `why` says why
        bool changed = false;
        std::string why;
    };

    // `typeNames`: the program's module names (a `*` after one is a pointer's, `Svf* f`); the ones the text declares
    // itself are found anyway.
    Formatted format(std::string_view text, const std::vector<std::string>& typeNames = {});
    // A place in `before` moved to the same token's place in `after` (a formatted text: the same tokens), so the caret
    // stays on what it was on.
    Pos mapFormatted(std::string_view before, std::string_view after, Pos p);
}
