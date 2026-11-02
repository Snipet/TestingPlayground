// PROBE name=fminus.lang timeout=600
//
// fminus.lang: FMinus's language service (fminus/Lang.h), its formatter, and the editor's check while typing
// (Patch::check):
//   LEXER     a line of each kind of token: names, keywords, type words, C's numbers (.5f, 0x1Fp-3, 1e-3), operators
//             longest first (->, ++, &&, ::), punctuation, a // comment; a directive is one token; a block comment
//             carries over the line's end; a character FMinus does not read is `unknown`
//   NETLIST   a circuit netlist's tokens (lexCirLine, a .cir file's): a * line a comment, .title a directive, R1 (an
//             element's name) a keyword, 2.2k, 10u and -60dB numbers, 1N914 a name, {1 - tone} punctuation, a number,
//             an operator and a name, a ; comment, a continuation's +; every line of every library netlist lexes so
//             (nothing unknown, a * line one comment, a . line a directive first, an element's line its name first)
//   SUMMARY   a netlist as the module it generates, line by line (no FMinus): the library's screamer.cir is module
//             Screamer, its documentation its * header, with void init(float rate), void set(float drive, float tone,
//             float level), float tick(float u) and void clear(); otalp.cir's tick takes its law input, float tick(float
//             u, float iabc), and it has no set (no pot); a netlist without .title is named by its file; in a program
//             that does not include it, completion offers Screamer, adding its #include "screamer.cir"; one that does
//             gets its methods after `pedal.`, with the usage header's line for each, and goes to its .input line; a
//             netlist whose first lines are the schematic's *@ lines still has its * header as its documentation
//   INDEX     a program of three files (program.fm, tools.fm it includes, svf.fm the library's) and host.fm: the
//             modules (Effect, Host, Svf), a param with its range in its signature, fields with their types, methods
//             with their parameters, a function of another file, locals and arguments with the scope they are seen in
//   COMPLETE  a typed prefix offers what it names first (a field, `fi` -> filter), with its type; after `.` a module's
//             members (methods and fields), after `->` the Host's fields; after `[[` the attributes; in #include's
//             quotes the files (the program's, host.fm, the library's); a local in its scope (and not after it), the
//             loop's variable in the loop; a builtin with its signature (tanhf, and tanf, FMinus's builtin since
//             c75caab); a keyword; a library module the program does
//             not read, which adds its #include (lfo.fm); nothing in a comment; a #pragma panel display's source: in
//             and out, the main module's float, int and bool fields and Shared's through its pointer (shared->level),
//             after `shared->` Shared's own names, no array and no param
//   SIGNATURE in a call: a method's (receiver's type resolved), a builtin's, a function's of another file, a vector
//             literal's lanes; the parameter the place is in, and its place in the label
//   HOVER     a method (its declaration and the usage header's line for it), a Host field (host.fm's line), a module
//             (its file's header), a local (const float y), a param (its range), a keyword, a builtin (tanhf, tanf)
//   DEFINE    a function of another file, a field, a module of the library's file, an #include's file
//   OCCUR     a field's uses in its file (the declaration, the calls); a loop variable's, not another loop's
//   UNCLOSED  a module whose `{` has no partner (a closer missing in a method, the file cut short) holds the rest of
//             the file: its members are completed, hovered and found there
//   FORMAT    every example and library file is the house style already (format changes nothing; a netlist is no
//             FMinus and is left out: the editor never formats one), and so is a program of the newer constructs
//             (an enumeration a line a constant and one on a line, the top level's tables, one broken over lines, a
//             switch with a block after a label and a statement on a label's line, the bits); three scrambled
//             copies of each (no indent; tokens single-spaced, braces K&R; no space at all where none is needed) format
//             back to it (comment gaps aside), each result formatted again unchanged; a messy program formats to the
//             house style, a messy switch and enumeration too, a table on one long line broken after its commas within
//             kWidth columns and stable; text with a character FMinus does not read is left as it is, with why
//   RENDER    each example of this product (with the library files it includes), formatted, renders what it rendered
//             before, bit for bit (measure::Session:
//             an effect over noise, an instrument's chord); RANDOM programs with every file formatted (the program and
//             its library files) the same, until every library file this product's RANDOM uses has been through it;
//             the other library files formatted compile alone with the same modules, fields and methods; the newer
//             constructs' program, its scrambled copy and that copy formatted, the same bits
//   CHECK     Patch::check: every example is clean; a syntax error, an undeclared name, an #include of no file and a
//             missing method give compile()'s messages word for word; its time (FMinus's pass at -O0) against RUN's;
//             a call of tanf compiles clean, and a program's own tanf is FMinus's error, the check's as RUN's
//   NEWER     the language of FMinus b023685, over a program of two files (tools.fm: an enumeration, the top level's
//             tables and a scalar, a function with a switch; program.fm: their uses, a module's enumeration, a block's,
//             an enumeration constant folded from the other file's): the bitwise operators lexed longest first (<<=,
//             >>=, &=, |=, ^=, <<, >>, & | ^ ~), switch, case, default and enum keywords, fminus::default's default a
//             name; the highlighter's styles (keywords, operators, the constants of both files and their tags);
//             completion of the constants (each file's, the module's own, a block's in its scope and not after it),
//             the tags after `enum`, the new keywords and builtins (hash, randf) with their signatures; hover (a
//             constant's value and enumeration, a table's declaration and its file, a tag's list, switch's meaning,
//             randf's); signature help in hash( and randf(; go to definition of a constant, a table and a tag in the
//             other file; a constant's occurrences; a circuit field's extent of enumeration constants folded; and
//             Patch::check's messages as RUN's for a switch on a float and a write to a constant table, the program
//             itself clean
#include "Probe.h"

#include "dsp/Measure.h"
#include "dsp/Sources.h"
#include "editor/CodeAssist.h"
#include "fminus/Lang.h"
#include "fminus/LibraryIncludes.h"
#include "fminus/Patch.h"
#include "fminus/Random.h"

#include "fminus/FMinus.h"

#include "Examples.h"
#include "Library.h"
#include "Product.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace pg = playground;
namespace lg = playground::lang;

namespace
{
    // The place of the n-th (0-based) `needle` in `text`, plus `extra` columns.
    lg::Pos posOf(const std::string& text, std::string_view needle, int extra = 0, int nth = 0)
    {
        std::size_t at = 0;
        for (int k = 0; k <= nth; ++k)
        {
            at = text.find(needle, k == 0 ? 0 : at + 1);
            if (at == std::string::npos)
                return { -1, -1 };
        }
        lg::Pos p;
        for (std::size_t i = 0; i < at; ++i)
        {
            if (text[i] == '\n')
            {
                ++p.line;
                p.col = 0;
            }
            else
                ++p.col;
        }
        p.col += extra;
        return p;
    }

    const lg::Completion* item(const lg::CompletionList& list, std::string_view label)
    {
        for (const lg::Completion& c : list.items)
            if (c.label == label)
                return &c;
        return nullptr;
    }

    std::string labels(const lg::CompletionList& list, std::size_t n = 8)
    {
        std::string s;
        for (std::size_t i = 0; i < list.items.size() && i < n; ++i)
            s += (s.empty() ? "" : " ") + list.items[i].label;
        return s;
    }

    std::string replaced(std::string text, const std::string& from, const std::string& to)
    {
        const std::size_t at = text.find(from);
        if (at != std::string::npos)
            text.replace(at, from.size(), to);
        return text;
    }

    const char* const kProgram =
        "// A probe's program.\n"
        "#include \"host.fm\"\n"
        "#include \"tools.fm\"\n"
        "module Effect\n"
        "{\n"
        "    param float cutoff [[fminus::range(20, 20000)]] [[fminus::default(1000)]];   // Hz\n"
        "    Svf filter;\n"
        "    Host* host;\n"
        "    float rate;\n"
        "\n"
        "    void init(float r)\n"
        "    {\n"
        "        rate = r;\n"
        "    }\n"
        "\n"
        "    void clear()\n"
        "    {\n"
        "        for (int i = 0; i < 2; ++i)\n"
        "            rate = 0;\n"
        "    }\n"
        "\n"
        "    void process(float* restrict left, float* restrict right, int n)\n"
        "    {\n"
        "        for (int i = 0; i < n; ++i)\n"
        "        {\n"
        "            filter.set(cutoff[i], 0.5f, rate);\n"
        "            const float y = twice(filter.tick(left[i]));\n"
        "            left[i] = y * host->bpm;\n"
        "            right[i] = y;\n"
        "        }\n"
        "    }\n"
        "};\n";

    const char* const kTools =
        "// tools.fm: helpers.\n"
        "#include \"svf.fm\"\n"
        "\n"
        "// Twice x: louder.\n"
        "float twice(float x)\n"
        "{\n"
        "    return 2 * x;\n"
        "}\n";

    // The newer constructs (FMinus b023685) in the house style: tools.fm's enumeration, tables and switch, and a
    // program of this product's kind that reads them (its own enumeration's and a block's too).
    const char* const kNewTools =
        "// tools.fm: an enumeration, the top level's constants and a switch.\n"
        "\n"
        "// The waves shape() draws.\n"
        "enum Wave\n"
        "{\n"
        "    SAW,\n"
        "    SQUARE = 4,                                 // a constant of its own value\n"
        "    TRIANGLE\n"
        "};\n"
        "enum { STEPS = 8, MASK = STEPS - 1 };          // a pattern's steps, and its index's mask\n"
        "\n"
        "// A pattern in semitones.\n"
        "const float kSteps[STEPS] = { 0, 3, 7, 12, 7, 3, 0, -5 };\n"
        "const float kRatios[32] = { 1.0f, 1.0625f, 1.125f, 1.1875f, 1.25f, 1.3125f, 1.375f, 1.4375f, 1.5f, 1.5625f, 1.625f,\n"
        "    1.6875f, 1.75f, 1.8125f, 1.875f, 1.9375f, 2.0f, 2.0625f, 2.125f, 2.1875f, 2.25f, 2.3125f, 2.375f, 2.4375f, 2.5f,\n"
        "    2.5625f, 2.625f, 2.6875f, 2.75f, 2.8125f, 2.875f, 2.9375f };\n"
        "const float kGain = 0.25f;\n"
        "\n"
        "float shape(enum Wave w, float phase)\n"
        "{\n"
        "    switch (w)\n"
        "    {\n"
        "        case SAW:\n"
        "            return 2 * phase - 1;\n"
        "        case SQUARE:\n"
        "        {\n"
        "            const float s = phase < 0.5f ? 1 : -1;\n"
        "            return s;\n"
        "        }\n"
        "        case TRIANGLE: return 1 - 4 * fabsf(phase - 0.5f);\n"
        "        default:\n"
        "            break;\n"
        "    }\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "// A step of the pattern from a count of samples: its bits from 12 up, under the mask.\n"
        "int stepOf(int count)\n"
        "{\n"
        "    int s = (count >> 12) & MASK;\n"
        "    s ^= count >> 20 & 1;\n"
        "    s |= 0;\n"
        "    s &= ~8;\n"
        "    s <<= 1;\n"
        "    s >>= 1;\n"
        "    return s;\n"
        "}\n";

    std::string newProgram()
    {
        const std::string body =
            "        for (int i = 0; i < n; ++i)\n"
            "        {\n"
            "            enum { SPREAD = 3 };\n"
            "            const float noise = randf(hash(SPREAD) ^ count);\n"
            "            const int at = stepOf(count) % COLS;\n"
            "            phase += 0.002f * kRatios[at] * expf(0.0577622650f * kSteps[at & MASK]);\n"
            "            phase -= floorf(phase);\n"
            "            const float y = kGain * shape((enum Wave)wave[i], phase) + 0.05f * noise;\n";
        const std::string head = "#include \"tools.fm\"\n"
                                 "\n"
                                 "enum { COLS = STEPS * 2 };                      // folded from tools.fm's STEPS\n"
                                 "\n";
        if constexpr (pg::product::kIsInstrument)
            return head + "module Voice\n"
                          "{\n"
                          "    enum Stage { HELD, LET_GO };\n"
                          "    param int wave [[fminus::range(0, 5)]] [[fminus::default(4)]];\n"
                          "    int count;\n"
                          "    float phase, env;\n"
                          "    enum Stage stage;\n"
                          "\n"
                          "    void start(float note, float velocity)\n"
                          "    {\n"
                          "        env = velocity;\n"
                          "        stage = HELD;\n"
                          "        count = 0;\n"
                          "    }\n"
                          "\n"
                          "    void stop()\n"
                          "    {\n"
                          "        stage = LET_GO;\n"
                          "    }\n"
                          "\n"
                          "    bool process(float* restrict left, float* restrict right, int n)\n"
                          "    {\n"
                 + body +
                          "            if (stage == LET_GO)\n"
                          "                env *= 0.999f;\n"
                          "            left[i] += env * y;\n"
                          "            right[i] += env * y;\n"
                          "            count = (count + 1) & 0x3FFFFFFF;\n"
                          "        }\n"
                          "        return stage == HELD || env > 0.0001f;\n"
                          "    }\n"
                          "};\n";
        return head + "module Effect\n"
                      "{\n"
                      "    enum Stage { HELD, LET_GO };\n"
                      "    param int wave [[fminus::range(0, 5)]] [[fminus::default(4)]];\n"
                      "    int count;\n"
                      "    float phase;\n"
                      "    enum Stage stage;\n"
                      "\n"
                      "    void process(float* restrict left, float* restrict right, int n)\n"
                      "    {\n"
             + body +
                      "            left[i] = 0.5f * left[i] + y;\n"
                      "            right[i] = 0.5f * right[i] + y;\n"
                      "            count = (count + 1) & 0x3FFFFFFF;\n"
                      "        }\n"
                      "        stage = HELD;\n"
                      "    }\n"
                      "};\n";
    }

    std::string libraryText(std::string_view name)
    {
        const pg::LibraryFile* f = pg::findLibraryFile(name);
        return f != nullptr ? f->source : "";
    }

    pg::Sources programFiles(const std::string& program)
    {
        return { { "program.fm", program }, { "tools.fm", kTools }, { "svf.fm", libraryText("svf.fm") } };
    }

    // Every line's tokens joined by single spaces (or, `tight`, by nothing where two would not run together), no
    // indent; with `knr`, a `{` alone on its line joined to the line before.
    std::string scramble(const std::string& text, bool tight, bool knr)
    {
        std::string out;
        bool inComment = false;
        int ln = 0;
        for (const std::string_view l : lg::splitLines(text))
        {
            std::vector<lg::Token> toks;
            lg::lexLine(l, ln++, inComment, toks);
            std::string s;
            for (const lg::Token& t : toks)
            {
                const bool words = !s.empty() && lg::isIdentChar(s.back()) && lg::isIdentChar(t.text.front());
                const bool glue = !s.empty() && ((s.back() == '-' && t.text.front() == '-') || (s.back() == '+' && t.text.front() == '+')
                                                 || (s.back() == '/' && (t.text.front() == '/' || t.text.front() == '*'))
                                                 || (s.back() == '&' && t.text.front() == '&'));
                if (!s.empty() && (!tight || t.kind == lg::Tok::comment || words || glue))
                    s += ' ';
                s += t.text;
            }
            if (knr && s == "{" && !out.empty())
            {
                const std::size_t start = out.rfind('\n', out.size() - 2);
                const std::string prev = out.substr(start == std::string::npos ? 0 : start + 1);
                if (prev.find("//") == std::string::npos && prev.rfind('#', 0) != 0 && prev.find('=') == std::string::npos
                    && prev != "\n")
                {
                    out.pop_back();
                    out += " {\n";
                    continue;
                }
            }
            out += s + "\n";
        }
        while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n')
            out.pop_back();
        return out;
    }

    // Every line's indent taken away.
    std::string dedent(const std::string& text)
    {
        std::string out;
        for (const std::string_view l : lg::splitLines(text))
        {
            const std::size_t first = l.find_first_not_of(' ');
            out += std::string(first == std::string_view::npos ? std::string_view{} : l.substr(first)) + "\n";
        }
        out.pop_back();
        return out;
    }

    // The text with every comment after code one space from it (what the scrambles lose).
    std::string commentGaps(const std::string& text)
    {
        std::string out;
        for (const std::string_view l : lg::splitLines(text))
        {
            std::string s(l);
            const std::size_t c = s.find("//");
            const std::size_t first = s.find_first_not_of(' ');
            if (c != std::string::npos && first != std::string::npos && first < c && s[first] != '#')
            {
                std::size_t e = c;
                while (e > 0 && s[e - 1] == ' ')
                    --e;
                s = s.substr(0, e) + " " + s.substr(c);
            }
            out += s + "\n";
        }
        return out;
    }

    // One render of the files: an effect over a second of noise, an instrument's chord held half a second and released.
    bool render(const pg::Sources& files, pg::measure::Session::Render& out, std::string& errors)
    {
        pg::measure::Session session(files, 48000.0);
        if (!session.compiled())
        {
            errors = session.errors();
            return false;
        }
        pg::measure::Request req;
        req.holdSeconds = 0.5f;
        req.tailSeconds = 0.5f;
        req.notes = { 48, 52, 55 };
        const std::size_t n = 48000;
        if constexpr (pg::product::kIsInstrument)
            out = session.render(req, nullptr, nullptr, n);
        else
        {
            std::vector<float> l(n), r(n);
            uint32_t x = 12345;
            for (std::size_t i = 0; i < n; ++i)
            {
                x = x * 1664525u + 1013904223u;
                l[i] = 0.25f * (static_cast<float>(x >> 8) / 8388608.0f - 1.0f);
                x = x * 1664525u + 1013904223u;
                r[i] = 0.25f * (static_cast<float>(x >> 8) / 8388608.0f - 1.0f);
            }
            out = session.render(req, &l, &r, n);
        }
        return true;
    }

    bool sameBits(const pg::measure::Session::Render& a, const pg::measure::Session::Render& b)
    {
        return a.l.size() == b.l.size() && a.r.size() == b.r.size() && !a.l.empty()
            && std::memcmp(a.l.data(), b.l.data(), a.l.size() * sizeof(float)) == 0
            && std::memcmp(a.r.data(), b.r.data(), a.r.size() * sizeof(float)) == 0;
    }

    double rms(const std::vector<float>& x)
    {
        double s = 0.0;
        for (const float v : x)
            s += static_cast<double>(v) * static_cast<double>(v);
        return x.empty() ? 0.0 : std::sqrt(s / static_cast<double>(x.size()));
    }

    // Every file of `files` formatted, with the program's module names.
    pg::Sources formatAll(const pg::Sources& files, bool& ok)
    {
        lg::Service s;
        s.setFiles(files);
        pg::Sources out;
        ok = true;
        for (const pg::SourceFile& f : files)
        {
            if (pg::isCircuitFile(f.name))
            {
                out.push_back(f);                      // a netlist: never formatted (the editor's Format is FMinus's)
                continue;
            }
            const lg::Formatted r = lg::format(f.text, s.typeNames());
            ok = ok && r.ok;
            out.push_back({ f.name, r.text });
        }
        return out;
    }

    // A module's shape as FMinus compiles it: its fields (name, type, offset) and methods.
    std::string shapeOf(const std::string& source, std::string& errors)
    {
        fminus::Program::Options o;
        o.optimisation = 0;
        o.fileName = "harness.fm";
        auto program = fminus::Program::compile(source, errors, o);
        if (!program)
            return {};
        std::string s;
        for (const fminus::Program::Module& m : program->modules())
        {
            s += m.name + " " + std::to_string(m.size) + ":";
            for (const auto& f : m.fields)
                s += " " + f.name + "/" + f.type + "@" + std::to_string(f.offset);
            for (const std::string& x : m.methods)
                s += " " + x;
            s += "\n";
        }
        return s;
    }
}

PROBE(fminus, lang)
{
    // ---- LEXER ------------------------------------------------------------------------------------------------------
    {
        std::vector<lg::Token> t;
        bool inComment = false;
        lg::lexLine("x = .5f + 0x1Fp-3 * a->b[1e-3]; ++i && q::r // c", 0, inComment, t);
        const auto kindAt = [&](std::size_t i) { return i < t.size() ? static_cast<int>(t[i].kind) : -1; };
        const auto textAt = [&](std::size_t i) { return i < t.size() ? std::string(t[i].text) : std::string(); };
        P.eq("lexer.count", static_cast<int64_t>(t.size()), 20);
        P.eq("lexer.number_dot", textAt(2) == ".5f" && kindAt(2) == static_cast<int>(lg::Tok::number) ? 1 : 0, 1);
        P.eq("lexer.number_hex", textAt(4) == "0x1Fp-3" ? 1 : 0, 1);
        P.eq("lexer.arrow", textAt(7) == "->" && kindAt(7) == static_cast<int>(lg::Tok::op) ? 1 : 0, 1);
        P.eq("lexer.exponent", textAt(10) == "1e-3" ? 1 : 0, 1);
        P.eq("lexer.increment", textAt(13) == "++" ? 1 : 0, 1);
        P.eq("lexer.and", textAt(15) == "&&" ? 1 : 0, 1);
        P.eq("lexer.scope", textAt(17) == "::" && kindAt(16) == static_cast<int>(lg::Tok::identifier) ? 1 : 0, 1);
        P.eq("lexer.comment", !t.empty() && t.back().kind == lg::Tok::comment && t.back().text == "// c" ? 1 : 0, 1);
        t.clear();
        lg::lexLine("param float4 v; module this parallel restrict", 0, inComment, t);
        P.eq("lexer.keywords", t.size() == 8 && t[0].kind == lg::Tok::keyword && t[1].kind == lg::Tok::type
                                   && t[2].kind == lg::Tok::identifier && t[4].kind == lg::Tok::keyword
                                   && t[5].kind == lg::Tok::keyword && t[6].kind == lg::Tok::keyword && t[7].kind == lg::Tok::keyword
                                   ? 1 : 0, 1);
        t.clear();
        lg::lexLine("  #include \"a.fm\" // why", 0, inComment, t);
        P.eq("lexer.directive", t.size() == 1 && t[0].kind == lg::Tok::directive && t[0].col == 2 ? 1 : 0, 1);
        t.clear();
        lg::lexLine("a /* open", 0, inComment, t);
        const bool opened = inComment;
        lg::lexLine("still */ b", 1, inComment, t);
        P.eq("lexer.block_comment", opened && !inComment && t.size() == 4 && t[1].open && t[3].text == "b" ? 1 : 0, 1);
        t.clear();
        lg::lexLine("x @ 'c'", 0, inComment, t);
        P.eq("lexer.unknown", t.size() >= 2 && t[1].kind == lg::Tok::unknown ? 1 : 0, 1);
    }

    // ---- NETLIST ----------------------------------------------------------------------------------------------------
    {
        const auto lexed = [](std::string_view line) {
            std::vector<lg::Token> t;
            lg::lexCirLine(line, 0, t);
            return t;
        };
        // The kinds of a line's tokens, a letter each: c comment, d directive, k keyword, n number, p punct, o op,
        // i identifier, ? anything else.
        const auto kinds = [&](std::string_view line) {
            std::string out;
            for (const lg::Token& t : lexed(line))
                out += t.kind == lg::Tok::comment     ? 'c'
                     : t.kind == lg::Tok::directive   ? 'd'
                     : t.kind == lg::Tok::keyword     ? 'k'
                     : t.kind == lg::Tok::number      ? 'n'
                     : t.kind == lg::Tok::punct       ? 'p'
                     : t.kind == lg::Tok::op          ? 'o'
                     : t.kind == lg::Tok::identifier  ? 'i'
                                                      : '?';
            return out;
        };
        // A library netlist's line that starts with `start`, as it is there.
        const auto lineOf = [](const char* file, std::string_view start) {
            const std::string text = libraryText(file);
            for (const std::string_view l : lg::splitLines(text))
                if (l.rfind(start, 0) == 0)
                    return std::string(l);
            return std::string();
        };
        const std::string pot = lineOf("diodetone.cir", "Ptone");
        std::printf("netlist: '%s' -> %s\n", pot.c_str(), kinds(pot).c_str());
        P.eq("netlist.comment_line", kinds(lineOf("diodetone.cir", "* diodetone.cir")) == "c" ? 1 : 0, 1);
        P.eq("netlist.directive", kinds(lineOf("diodetone.cir", ".title")) == "di" && kinds(".title X") == "di" ? 1 : 0, 1);
        const std::string r1Line = lineOf("diodetone.cir", "R1");
        const std::vector<lg::Token> r1 = lexed(r1Line);   // its tokens' texts are views of r1Line
        P.eq("netlist.element_keyword", kinds(r1Line) == "kiin" && r1.size() == 4 && r1[0].text == "R1" && r1[3].text == "2.2k" ? 1 : 0, 1);
        P.eq("netlist.expression", kinds(pot) == "kiinpnoipc" ? 1 : 0, 1);
        const std::vector<lg::Token> pt = lexed(pot);
        P.eq("netlist.expression_text", pt.size() == 10 && pt[4].text == "{" && pt[5].text == "1" && pt[6].text == "-"
                                            && pt[7].text == "tone" && pt[8].text == "}" && pt[9].text == "; a rheostat" ? 1 : 0, 1);
        P.eq("netlist.model_card", kinds(".model 1N914 D(IS=2.52n N=1.752)") == "diipipnipnp" ? 1 : 0, 1);
        P.eq("netlist.model_name", lexed("D1 a 0 1N914")[3].kind == lg::Tok::identifier ? 1 : 0, 1);
        P.eq("netlist.law_input", kinds(lineOf("otalp.cir", ".input  iabc")) == "diiipiipnpn" ? 1 : 0, 1);
        P.eq("netlist.negative_value", kinds(".target solver=-60dB level=4.5V") == "dipnipn" ? 1 : 0, 1);
        P.eq("netlist.continuation", kinds("+ scale=1") == "pipn" && kinds("  + R2 a b 1k") == "piiin" ? 1 : 0, 1);
        int lines = 0, good = 0;
        for (const pg::LibraryFile& f : pg::kLibrary)
        {
            if (!pg::isCircuitFile(f.name))
                continue;
            for (const std::string_view l : lg::splitLines(f.source))
            {
                std::size_t a = 0;
                while (a < l.size() && l[a] == ' ')
                    ++a;
                if (a == l.size())
                    continue;
                ++lines;
                const std::string k = kinds(l);
                const bool ok = k.find('?') == std::string::npos
                             && (l[a] == '*'   ? k == "c"
                                 : l[a] == '.' ? k[0] == 'd'
                                 : l[a] == '+' ? k[0] == 'p'
                                               : k[0] == 'k');
                good += ok ? 1 : 0;
                if (!ok)
                    std::printf("netlist %s: '%s' -> %s\n", f.name, std::string(l).c_str(), k.c_str());
            }
        }
        std::printf("netlist: %d library netlist lines lexed\n", lines);
        P.ge("netlist.library_lines", static_cast<double>(lines), 60.0);
        P.eq("netlist.library_lexes", good, lines);
    }

    // ---- SUMMARY ----------------------------------------------------------------------------------------------------
    {
        pg::Sources library;
        for (const pg::LibraryFile& f : pg::kLibrary)
            library.push_back({ f.name, f.source });
        const std::string uses = std::string(pg::product::kIsInstrument ? "module Voice\n" : "module Effect\n")
                               + "{\n    Screamer pedal;\n    float x;\n    void init(float rate)\n    {\n        pedal.\n    }\n};\n";
        lg::Service c;
        c.setLibrary(library);
        c.setFiles({ { "program.fm", "#include \"screamer.cir\"\n" + uses }, { "screamer.cir", libraryText("screamer.cir") },
                     { "otalp.cir", libraryText("otalp.cir") }, { "bare.cir", ".input u in\n.output y y\nR1 in y 1k\nC1 y 0 1n\n" },
                     { "drawn.cir", "*@ schematic 1 (the playground's drawing; SPICE reads *@ lines as comments)\n*@ part R1 0,0\n"
                                    "* drawn.cir: a netlist whose drawing comes first\n*@ net y 2,0 4,0\n* its usage\n"
                                    ".input u in\n.output y y\nR1 in y 1k\nC1 y 0 1n\n" } });
        const auto method = [&](std::string_view module, std::string_view name) -> const lg::Symbol* {
            for (const lg::Symbol& x : c.symbols())
                if (x.kind == lg::Kind::method && x.container == module && x.name == name)
                    return &x;
            return nullptr;
        };
        const auto signature = [&](std::string_view module, std::string_view name) {
            const lg::Symbol* m = method(module, name);
            return m != nullptr ? m->signature : std::string("(none)");
        };
        const lg::Symbol* screamer = nullptr;
        for (const lg::Symbol& x : c.symbols())
            if (x.kind == lg::Kind::module && x.name == "Screamer")
                screamer = &x;
        std::printf("summary: Screamer's %s; %s; %s; %s\n", signature("Screamer", "init").c_str(), signature("Screamer", "set").c_str(),
                    signature("Screamer", "tick").c_str(), signature("Screamer", "clear").c_str());
        P.eq("summary.module", screamer != nullptr && screamer->file == c.fileIndex("screamer.cir")
                                   && screamer->doc.rfind("screamer.cir: the Tube Screamer's", 0) == 0 ? 1 : 0, 1);
        P.eq("summary.init", signature("Screamer", "init") == "void init(float rate)" ? 1 : 0, 1);
        P.eq("summary.set", signature("Screamer", "set") == "void set(float drive, float tone, float level)" ? 1 : 0, 1);
        P.eq("summary.tick", signature("Screamer", "tick") == "float tick(float u)" ? 1 : 0, 1);
        P.eq("summary.clear", signature("Screamer", "clear") == "void clear()" ? 1 : 0, 1);
        P.eq("summary.no_step", method("Screamer", "step") == nullptr ? 1 : 0, 1);
        P.eq("summary.law_input", signature("OtaLp", "tick") == "float tick(float u, float iabc)" && method("OtaLp", "set") == nullptr ? 1 : 0, 1);
        P.eq("summary.named_by_file", signature("Bare", "tick") == "float tick(float u)" ? 1 : 0, 1);
        // The schematic's *@ lines (fminus/CirDoc.h) are no part of the header, even before it.
        const lg::Symbol* drawn = nullptr;
        for (const lg::Symbol& x : c.symbols())
            if (x.kind == lg::Kind::module && x.name == "Drawn")
                drawn = &x;
        P.eq("summary.layout_lines_no_header", drawn != nullptr && drawn->doc == "drawn.cir: a netlist whose drawing comes first\nits usage" ? 1 : 0, 1);
        const std::vector<std::string>& types = c.typeNames();
        P.eq("summary.a_type", std::find(types.begin(), types.end(), "Screamer") != types.end() ? 1 : 0, 1);
        // Its methods after `pedal.`, with the usage header's line.
        const std::string program = "#include \"screamer.cir\"\n" + uses;
        const lg::CompletionList m = c.complete(0, posOf(program, "pedal.\n", 6));
        std::printf("summary: pedal. -> %s\n", labels(m).c_str());
        bool tick = false;
        for (const lg::Completion& k : m.items)
            tick = tick || (k.label == "tick" && k.detail == "float tick(float u)" && k.doc.find("one sample") != std::string::npos);
        P.eq("summary.members", m.member && tick && m.items.size() == 4 ? 1 : 0, 1);
        const std::optional<lg::Location> d = c.definition(0, posOf(program, "Screamer pedal", 2));
        P.eq("summary.definition", d && d->fileName == "screamer.cir" && d->at == posOf(libraryText("screamer.cir"), ".title Screamer", 7) ? 1 : 0, 1);
        // A program that does not include it: the library's module, which adds its #include.
        lg::Service lib;
        lib.setLibrary(library);
        const std::string none = std::string(pg::product::kIsInstrument ? "module Voice\n" : "module Effect\n") + "{\n    Scr\n};\n";
        lib.setFiles({ { "program.fm", none } });
        const lg::CompletionList offered = lib.complete(0, posOf(none, "    Scr\n", 7));
        std::printf("summary: Scr -> %s\n", labels(offered).c_str());
        bool adds = false;
        for (const lg::Completion& k : offered.items)
            adds = adds || (k.label == "Screamer" && k.kind == lg::Kind::module && k.include == "screamer.cir");
        P.eq("summary.library_module_adds_include", adds ? 1 : 0, 1);
    }

    // ---- INDEX ------------------------------------------------------------------------------------------------------
    const std::string program = kProgram;
    lg::Service s;
    s.setFiles(programFiles(program));
    {
        lg::Service lib;
        pg::Sources library;
        for (const pg::LibraryFile& f : pg::kLibrary)
            library.push_back({ f.name, f.source });
        s.setLibrary(library);
    }
    {
        const auto find = [&](std::string_view name, lg::Kind kind) -> const lg::Symbol* {
            for (const lg::Symbol& x : s.symbols())
                if (x.name == name && x.kind == kind)
                    return &x;
            return nullptr;
        };
        const std::vector<std::string>& types = s.typeNames();
        P.eq("index.types", std::find(types.begin(), types.end(), "Effect") != types.end()
                                && std::find(types.begin(), types.end(), "Host") != types.end()
                                && std::find(types.begin(), types.end(), "Svf") != types.end() ? 1 : 0, 1);
        const lg::Symbol* cutoff = find("cutoff", lg::Kind::param);
        P.eq("index.param", cutoff != nullptr && cutoff->container == "Effect" && cutoff->type == "float"
                                && cutoff->signature.find("fminus::range(20, 20000)") != std::string::npos && cutoff->doc == "Hz" ? 1 : 0, 1);
        const lg::Symbol* filter = find("filter", lg::Kind::field);
        P.eq("index.field", filter != nullptr && filter->type == "Svf" && filter->at.line == 6 ? 1 : 0, 1);
        const lg::Symbol* host = find("host", lg::Kind::field);
        P.eq("index.pointer_field", host != nullptr && host->type == "Host*" ? 1 : 0, 1);
        const lg::Symbol* process = find("process", lg::Kind::method);
        P.eq("index.method", process != nullptr && process->params.size() == 3 && process->params[0] == "float* restrict left" ? 1 : 0, 1);
        const lg::Symbol* twice = find("twice", lg::Kind::function);
        P.eq("index.function", twice != nullptr && twice->file == s.fileIndex("tools.fm") && twice->doc == "Twice x: louder." ? 1 : 0, 1);
        const lg::Symbol* y = find("y", lg::Kind::local);
        P.eq("index.local", y != nullptr && y->type == "const float" && y->scope.to.line == 29 ? 1 : 0, 1);
        const lg::Symbol* bpm = find("bpm", lg::Kind::field);
        P.eq("index.host", bpm != nullptr && s.files()[static_cast<std::size_t>(bpm->file)].name == "host.fm" ? 1 : 0, 1);
    }

    // ---- COMPLETE ---------------------------------------------------------------------------------------------------
    {
        const std::string fi = replaced(program, "            right[i] = y;\n", "            fi\n");
        s.setFiles(programFiles(fi));
        lg::CompletionList c = s.complete(0, posOf(fi, "            fi\n", 14));
        std::printf("complete fi: %s\n", labels(c).c_str());
        P.eq("complete.prefix_first", !c.items.empty() && c.items[0].label == "filter" && c.items[0].kind == lg::Kind::field
                                          && c.items[0].detail == "Svf" ? 1 : 0, 1);
        P.eq("complete.prefix_range", c.replace.from.col == 12 && c.replace.to.col == 14 && c.prefix == "fi" ? 1 : 0, 1);

        const std::string dot = replaced(program, "            right[i] = y;\n", "            filter.\n");
        s.setFiles(programFiles(dot));
        c = s.complete(0, posOf(dot, "filter.\n", 7));
        std::printf("complete filter.: %s\n", labels(c, 20).c_str());
        const lg::Completion* tick = item(c, "tick");
        P.eq("complete.members", c.member && tick != nullptr && tick->kind == lg::Kind::method && item(c, "lp") != nullptr
                                     && item(c, "set") != nullptr && item(c, "cutoff") == nullptr ? 1 : 0, 1);
        P.eq("complete.method_inserts_call", tick != nullptr && tick->insert == "tick($0)" ? 1 : 0, 1);

        const std::string arrow = replaced(program, "            right[i] = y;\n", "            host->b\n");
        s.setFiles(programFiles(arrow));
        c = s.complete(0, posOf(arrow, "host->b\n", 7));
        std::printf("complete host->b: %s\n", labels(c).c_str());
        P.eq("complete.host_fields", c.member && item(c, "bpm") != nullptr && item(c, "beat") != nullptr && item(c, "beatsPerBar") != nullptr
                                         && item(c, "sustain") == nullptr ? 1 : 0, 1);

        s.setFiles(programFiles(program));
        c = s.complete(0, posOf(program, "[[fminus::default", 4));
        P.eq("complete.attributes", item(c, "fminus::range") != nullptr && item(c, "fminus::default") != nullptr ? 1 : 0, 1);
        c = s.complete(0, posOf(program, "#include \"tools.fm\"", 10));
        std::printf("complete include: %s\n", labels(c, 20).c_str());
        P.eq("complete.include_files", item(c, "tools.fm") != nullptr && item(c, "host.fm") != nullptr && item(c, "lfo.fm") != nullptr
                                           && item(c, "program.fm") == nullptr ? 1 : 0, 1);

        // Locals in their scope: y after its declaration, the loop's i in the loop; not after it.
        const std::string inLoop = replaced(program, "            right[i] = y;\n", "            right[i] = y;\n            \n");
        s.setFiles(programFiles(inLoop));
        c = s.complete(0, { 29, 12 });                          // the blank line in the loop
        P.eq("complete.locals_in_scope", !c.items.empty() && item(c, "y") != nullptr && item(c, "i") != nullptr
                                             && item(c, "left") != nullptr && c.items[0].kind == lg::Kind::local ? 1 : 0, 1);
        const std::string afterLoop = replaced(program, "        }\n    }\n};\n", "        }\n        \n    }\n};\n");
        s.setFiles(programFiles(afterLoop));
        c = s.complete(0, { 30, 8 });                           // the blank line after the loop
        P.eq("complete.out_of_scope", item(c, "y") == nullptr && item(c, "i") == nullptr && item(c, "left") != nullptr ? 1 : 0, 1);

        const std::string tan = replaced(program, "            right[i] = y;\n", "            tan\n");
        s.setFiles(programFiles(tan));
        c = s.complete(0, posOf(tan, "tan\n", 3));
        const lg::Completion* tanh = item(c, "tanhf");
        P.eq("complete.builtin", tanh != nullptr && tanh->kind == lg::Kind::builtin && tanh->detail == "float tanhf(float x)" ? 1 : 0, 1);
        const lg::Completion* tanf = item(c, "tanf");
        P.eq("complete.builtin_tanf", tanf != nullptr && tanf->kind == lg::Kind::builtin && tanf->detail == "float tanf(float x)" ? 1 : 0, 1);

        const std::string lfo = replaced(program, "            right[i] = y;\n", "            Lf\n");
        s.setFiles(programFiles(lfo));
        c = s.complete(0, posOf(lfo, "Lf\n", 2));
        const lg::Completion* lfoItem = item(c, "Lfo");
        P.eq("complete.library_module", lfoItem != nullptr && lfoItem->include == "lfo.fm" ? 1 : 0, 1);

        const std::string kw = replaced(program, "    float rate;\n", "    float rate;\n    pa\n");
        s.setFiles(programFiles(kw));
        c = s.complete(0, posOf(kw, "    pa\n", 6));
        P.eq("complete.keywords", item(c, "param") != nullptr && item(c, "parallel") != nullptr ? 1 : 0, 1);

        s.setFiles(programFiles(program));
        c = s.complete(0, posOf(program, "A probe", 3));
        P.eq("complete.not_in_comments", c.items.empty() ? 1 : 0, 1);

        // A #pragma panel display's source, Shared's fields through the main module's pointer among them.
        const std::string panel = "#pragma panel number \n#pragma panel number sh\n#pragma panel meter shared->l\n"
                                  "module Shared\n{\n    float level;\n    int keys;\n    float table[4];\n};\n\n"
                                  "module Effect\n{\n    param float gain;\n    Shared* shared;\n    float env;\n"
                                  "    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
        s.setFiles({ { "program.fm", panel } });
        c = s.complete(0, { 0, 21 });
        std::printf("complete a display's source: %s\n", labels(c, 20).c_str());
        P.eq("complete.display_sources", item(c, "env") != nullptr && item(c, "shared->level") != nullptr && item(c, "shared->keys") != nullptr
                                             && item(c, "out") != nullptr && item(c, "gain") == nullptr && item(c, "shared->table") == nullptr
                                             && item(c, "shared") == nullptr ? 1 : 0, 1);
        c = s.complete(0, { 1, 23 });
        P.eq("complete.display_shared", !c.items.empty() && c.items[0].label.rfind("shared->", 0) == 0 && item(c, "env") == nullptr ? 1 : 0, 1);
        c = s.complete(0, { 2, 29 });
        std::printf("complete after shared->: %s\n", labels(c, 20).c_str());
        P.eq("complete.display_after_arrow", item(c, "level") != nullptr && item(c, "shared->level") == nullptr && c.prefix == "l" ? 1 : 0, 1);
    }

    // ---- SIGNATURE --------------------------------------------------------------------------------------------------
    {
        s.setFiles(programFiles(program));
        std::optional<lg::Signature> g = s.signature(0, posOf(program, "0.5f, rate", 0));
        std::printf("signature: %s active %d\n", g ? g->label.c_str() : "-", g ? g->active : -1);
        P.eq("signature.method", g && g->label == "void set(float fc, float res, float rate)" && g->active == 1 ? 1 : 0, 1);
        P.eq("signature.param_range", g && g->params.size() == 3
                                          && g->label.substr(static_cast<std::size_t>(g->params[1].first),
                                                             static_cast<std::size_t>(g->params[1].second - g->params[1].first)) == "float res"
                                          ? 1 : 0, 1);
        P.eq("signature.doc", g && g->doc.find("the cutoff in Hz") != std::string::npos ? 1 : 0, 1);
        g = s.signature(0, posOf(program, "filter.tick(left", 12));
        P.eq("signature.nested", g && g->label == "float tick(float x)" && g->active == 0 ? 1 : 0, 1);
        g = s.signature(0, posOf(program, "twice(filter", 6));
        P.eq("signature.other_file", g && g->label == "float twice(float x)" ? 1 : 0, 1);
        const std::string calls = replaced(program, "            right[i] = y;\n",
                                           "            right[i] = fminf(y, 1) + reduce_add((float4)(1, 2, 3, 4));\n");
        s.setFiles(programFiles(calls));
        g = s.signature(0, posOf(calls, "fminf(y, 1)", 9));
        P.eq("signature.builtin", g && g->label == "float fminf(float x, float y)" && g->active == 1 ? 1 : 0, 1);
        g = s.signature(0, posOf(calls, "3, 4))", 1));
        P.eq("signature.vector", g && g->label == "(float4)(float x, float y, float z, float w)" && g->active == 2 ? 1 : 0, 1);
    }

    // ---- HOVER ------------------------------------------------------------------------------------------------------
    {
        s.setFiles(programFiles(program));
        std::optional<lg::Hover> h = s.hover(0, posOf(program, "tick(left", 1));
        std::printf("hover tick: [%s] %s\n", h ? h->code.c_str() : "-", h ? h->doc.c_str() : "");
        P.eq("hover.method", h && h->code == "float tick(float x)" && h->doc.find("a method of Svf") != std::string::npos
                                 && h->doc.find("one sample in") != std::string::npos ? 1 : 0, 1);
        h = s.hover(0, posOf(program, "host->bpm", 7));
        P.eq("hover.host_field", h && h->code == "float bpm" && h->doc.find("the tempo") != std::string::npos ? 1 : 0, 1);
        h = s.hover(0, posOf(program, "Svf filter", 1));
        P.eq("hover.module", h && h->code == "module Svf" && h->doc.find("state-variable") != std::string::npos ? 1 : 0, 1);
        h = s.hover(0, posOf(program, "y * host", 0));
        P.eq("hover.local", h && h->code == "const float y" ? 1 : 0, 1);
        h = s.hover(0, posOf(program, "cutoff[i]", 2));
        P.eq("hover.param", h && h->code.find("[[fminus::range(20, 20000)]]") != std::string::npos ? 1 : 0, 1);
        h = s.hover(0, posOf(program, "param", 1));
        P.eq("hover.keyword", h && h->doc.find("dials") != std::string::npos ? 1 : 0, 1);
        const std::string calls = replaced(program, "            right[i] = y;\n", "            right[i] = tanhf(y);\n");
        s.setFiles(programFiles(calls));
        h = s.hover(0, posOf(calls, "tanhf", 2));
        P.eq("hover.builtin", h && h->code == "float tanhf(float x)" ? 1 : 0, 1);
        const std::string tangent = replaced(program, "            right[i] = y;\n", "            right[i] = tanf(y);\n");
        s.setFiles(programFiles(tangent));
        h = s.hover(0, posOf(tangent, "tanf", 2));
        P.eq("hover.builtin_tanf", h && h->code == "float tanf(float x)" && h->doc.find("prewarp") != std::string::npos ? 1 : 0, 1);
    }

    // ---- DEFINE and OCCUR -------------------------------------------------------------------------------------------
    {
        s.setFiles(programFiles(program));
        std::optional<lg::Location> d = s.definition(0, posOf(program, "twice(", 1));
        P.eq("define.other_file", d && d->fileName == "tools.fm" && d->at.line == 4 && d->at.col == 6 ? 1 : 0, 1);
        d = s.definition(0, posOf(program, "filter.set", 2));
        P.eq("define.field", d && d->file == 0 && d->at.line == 6 && d->at.col == 8 ? 1 : 0, 1);
        d = s.definition(0, posOf(program, "Svf filter", 0));
        P.eq("define.library_module", d && d->fileName == "svf.fm" && d->at == posOf(libraryText("svf.fm"), "module Svf", 7) ? 1 : 0, 1);
        d = s.definition(0, posOf(program, "tools.fm", 2));
        P.eq("define.include", d && d->fileName == "tools.fm" && d->at.line == 0 ? 1 : 0, 1);

        const std::vector<lg::Range> f = s.occurrences(0, posOf(program, "filter.tick", 0));
        P.eq("occur.field", static_cast<int64_t>(f.size()), 3);
        const std::vector<lg::Range> i = s.occurrences(0, posOf(program, "int i = 0; i < n", 4));
        bool inLoop = true;
        for (const lg::Range& r : i)
            inLoop = inLoop && r.from.line >= 23 && r.from.line <= 29;
        P.eq("occur.loop_variable", static_cast<int64_t>(i.size()), 7);
        P.eq("occur.not_other_loop", inLoop ? 1 : 0, 1);
    }

    // ---- UNCLOSED ---------------------------------------------------------------------------------------------------
    {
        // The loop's `}` gone: each closer pairs with an inner opener, and the module's `{` has none.
        const std::string open = replaced(program, "            right[i] = y;\n        }\n", "            fi\n");
        s.setFiles(programFiles(open));
        const lg::CompletionList c = s.complete(0, posOf(open, "            fi\n", 14));
        std::printf("complete fi (unclosed): %s\n", labels(c).c_str());
        P.eq("unclosed.complete", !c.items.empty() && c.items[0].label == "filter" && c.items[0].kind == lg::Kind::field ? 1 : 0, 1);
        const std::optional<lg::Hover> h = s.hover(0, posOf(open, "cutoff[i]", 2));
        P.eq("unclosed.hover", h && h->code.find("[[fminus::range(20, 20000)]]") != std::string::npos ? 1 : 0, 1);
        P.eq("unclosed.occurrences", static_cast<int64_t>(s.occurrences(0, posOf(open, "filter.tick", 0)).size()), 3);
        const std::string cut = open.substr(0, open.find("            fi\n") + 15);   // the file ends in the method
        s.setFiles(programFiles(cut));
        P.eq("unclosed.cut_short", item(s.complete(0, posOf(cut, "            fi\n", 14)), "filter") != nullptr ? 1 : 0, 1);
    }

    // ---- FORMAT -----------------------------------------------------------------------------------------------------
    std::vector<std::string> types;
    {
        lg::Service all;
        pg::Sources everything = { { "host.fm", std::string(pg::Patch::hostSource()) } };
        for (const pg::LibraryFile& f : pg::kLibrary)
            everything.push_back({ f.name, f.source });
        all.setFiles(everything);
        types = all.typeNames();
    }
    {
        std::vector<std::pair<std::string, std::string>> corpus;
        for (const pg::Example& e : pg::kExamples)
            corpus.push_back({ std::string("example ") + e.title, e.source });
        for (const pg::LibraryFile& f : pg::kLibrary)
            if (!pg::isCircuitFile(f.name))
                corpus.push_back({ f.name, f.source });
        corpus.push_back({ "the newer constructs' tools.fm", kNewTools });
        corpus.push_back({ "the newer constructs' program.fm", newProgram() });
        int house = 0, back = 0, again = 0, refused = 0;
        for (const auto& [name, text] : corpus)
        {
            const lg::Formatted r = lg::format(text, types);
            house += r.ok && !r.changed ? 1 : 0;
            if (r.changed)
                std::printf("not the house style: %s\n", name.c_str());
            for (int mode = 0; mode < 3; ++mode)
            {
                const std::string scrambled = mode == 0 ? dedent(text) : scramble(text, mode == 2, true);
                const lg::Formatted f = lg::format(scrambled, types);
                const lg::Formatted g = lg::format(f.text, types);
                refused += f.ok ? 0 : 1;
                const bool same = mode == 0 ? f.text == text : commentGaps(f.text) == commentGaps(text);
                back += same ? 1 : 0;
                again += g.ok && !g.changed ? 1 : 0;
                if (!same)
                    std::printf("mode %d does not format back: %s\n", mode, name.c_str());
            }
        }
        const auto n = static_cast<int64_t>(corpus.size());
        P.eq("format.house_style", house, n);
        P.eq("format.scrambled_back", back, 3 * n);
        P.eq("format.idempotent", again, 3 * n);
        P.eq("format.none_refused", refused, 0);

        const std::string messy = "module Effect {\n"
                                  "param float g [[ fminus::range( 0,1 ) ]];\n"
                                  "Svf* f;float x;\n"
                                  "void process(float*restrict left,float*restrict right,int n){\n"
                                  "for(int i=0;i<n;++i){\n"
                                  "if(x>1) x=1;\n"
                                  "else if (x<-1)\n"
                                  "x=-1;\n"
                                  "else{\n"
                                  "x*=0.5f;}\n"
                                  "left[i]=x>0?x:-x*(float)n;\n"
                                  "}\n"
                                  "}\n"
                                  "};\n";
        const std::string want = "module Effect\n"
                                 "{\n"
                                 "    param float g [[fminus::range(0, 1)]];\n"
                                 "    Svf* f; float x;\n"
                                 "    void process(float* restrict left, float* restrict right, int n)\n"
                                 "    {\n"
                                 "        for (int i = 0; i < n; ++i)\n"
                                 "        {\n"
                                 "            if (x > 1) x = 1;\n"
                                 "            else if (x < -1)\n"
                                 "                x = -1;\n"
                                 "            else\n"
                                 "            {\n"
                                 "                x *= 0.5f;\n"
                                 "            }\n"
                                 "            left[i] = x > 0 ? x : -x * (float)n;\n"
                                 "        }\n"
                                 "    }\n"
                                 "};\n";
        const lg::Formatted m = lg::format(messy, {});
        if (m.text != want)
            std::printf("messy formatted:\n%s", m.text.c_str());
        P.eq("format.messy", m.ok && m.text == want ? 1 : 0, 1);
        const lg::Formatted bad = lg::format("float x = 1; @\n", {});
        P.eq("format.refuses_unknown", !bad.ok && bad.text == "float x = 1; @\n" && bad.why.find("'@'") != std::string::npos ? 1 : 0, 1);
        const lg::Pos mapped = lg::mapFormatted(messy, m.text, posOf(messy, "x*=0.5f", 3));
        P.eq("format.maps_caret", mapped == posOf(m.text, "x *= 0.5f", 5) ? 1 : 0, 1);

        // The newer constructs, messy: a switch's labels and bodies, a block after a label, an enumeration's braces,
        // the bits spaced and ~ tight, a cast to an enumeration.
        const std::string messySwitch = "enum Mode {A,B=1<<2,C};\n"
                                        "enum Wave{\n"
                                        "X,\n"
                                        "Y=X+3\n"
                                        "};\n"
                                        "int pick(int x){\n"
                                        "switch(x&~3){\n"
                                        "case A:return x<<1;\n"
                                        "case B:{\n"
                                        "int t=x^0x0F;\n"
                                        "return t|1;}\n"
                                        "case C:\n"
                                        "x>>=2;\n"
                                        "default:break;\n"
                                        "}\n"
                                        "return (enum Mode)x==A?1:0;\n"
                                        "}\n";
        const std::string wantSwitch = "enum Mode { A, B = 1 << 2, C };\n"
                                       "enum Wave\n"
                                       "{\n"
                                       "    X,\n"
                                       "    Y = X + 3\n"
                                       "};\n"
                                       "int pick(int x)\n"
                                       "{\n"
                                       "    switch (x & ~3)\n"
                                       "    {\n"
                                       "        case A: return x << 1;\n"
                                       "        case B:\n"
                                       "        {\n"
                                       "            int t = x ^ 0x0F;\n"
                                       "            return t | 1;\n"
                                       "        }\n"
                                       "        case C:\n"
                                       "            x >>= 2;\n"
                                       "        default: break;\n"
                                       "    }\n"
                                       "    return (enum Mode)x == A ? 1 : 0;\n"
                                       "}\n";
        const lg::Formatted ms = lg::format(messySwitch, {});
        if (ms.text != wantSwitch)
            std::printf("messy switch formatted:\n%s", ms.text.c_str());
        P.eq("format.messy_switch_enum", ms.ok && ms.text == wantSwitch ? 1 : 0, 1);
        P.eq("format.messy_switch_stable", lg::format(ms.text, {}).changed ? 0 : 1, 1);

        // A table on one long line: broken after its commas, every line within kWidth, its tokens the same, stable.
        std::string table = "const float kTable[48] = {";
        for (int k = 0; k < 48; ++k)
            table += (k == 0 ? " " : ", ") + std::to_string(k * 37 % 101) + ".25f";
        table += " };\n";
        const lg::Formatted tw = lg::format(table, {});
        int longest = 0, lines = 0;
        for (const std::string_view l : lg::splitLines(tw.text))
        {
            longest = std::max(longest, static_cast<int>(l.size()));
            lines += l.empty() ? 0 : 1;
        }
        std::printf("a table of %zu columns: %d lines, the longest %d\n%s", table.size() - 1, lines, longest, tw.text.c_str());
        P.eq("format.table_wrapped", tw.ok && tw.changed && lines >= 3 && longest <= lg::kWidth ? 1 : 0, 1);
        P.eq("format.table_continuation", tw.text.find("\n    ") != std::string::npos ? 1 : 0, 1);
        P.eq("format.table_stable", lg::format(tw.text, {}).changed ? 0 : 1, 1);
    }

    // ---- RENDER -----------------------------------------------------------------------------------------------------
    {
        int examples = 0, same = 0;
        for (const pg::Example& e : pg::kExamples)
        {
            ++examples;
            const pg::Sources files = pg::withLibraryIncludes(pg::singleFile(e.source));
            bool ok = false;
            const pg::Sources formatted = formatAll(files, ok);
            pg::measure::Session::Render a, b;
            std::string why;
            const bool ra = render(files, a, why), rb = ok && render(formatted, b, why);
            const bool bits = ra && rb && sameBits(a, b) && rms(a.l) > 1e-4;
            same += bits ? 1 : 0;
            std::printf("render %s: %s (rms %.4f)\n", e.title, bits ? "the same bits" : ("DIFFERS " + why).c_str(), rms(a.l));
        }
        P.eq("render.examples", same, examples);

        // RANDOM programs, every file formatted, until the library files they use have all been through.
        std::set<std::string> used, rendered;
        int programs = 0, programsSame = 0;
        for (uint32_t seed = 1; seed <= 48; ++seed)
        {
            const pg::random::Program r = pg::random::make(seed * 7919u);
            bool fresh = false;
            for (const std::string& l : r.library)
                fresh = fresh || rendered.count(l) == 0;
            for (const std::string& l : r.library)
                used.insert(l);
            if (!fresh && seed > 4)
                continue;
            pg::Sources files = pg::singleFile(r.text);
            for (const std::string& l : r.library)
                files.push_back({ l, libraryText(l) });
            bool ok = false;
            const pg::Sources formatted = formatAll(files, ok);
            pg::measure::Session::Render a, b;
            std::string why;
            const bool bits = render(files, a, why) && ok && render(formatted, b, why) && sameBits(a, b);
            ++programs;
            programsSame += bits ? 1 : 0;
            if (bits)
                for (const std::string& l : r.library)
                    rendered.insert(l);
            else
                std::printf("random %u differs: %s\n", seed * 7919u, why.c_str());
        }
        std::printf("random programs rendered: %d, library files through them: %zu of %zu\n", programs, rendered.size(), used.size());
        P.eq("render.random", programsSame, programs);
        P.eq("render.random_library_covered", static_cast<int64_t>(rendered.size()), static_cast<int64_t>(used.size()));
        // The other library files: alone, formatted, the same modules (fields, offsets, methods) as compiled before.
        int alone = 0, aloneSame = 0;
        for (const pg::LibraryFile& f : pg::kLibrary)
        {
            if (rendered.count(f.name) != 0 || pg::isCircuitFile(f.name))
                continue;                              // a netlist is never formatted
            ++alone;
            std::string e1, e2;
            const std::string before = shapeOf(f.source, e1);
            const std::string after = shapeOf(lg::format(f.source, types).text, e2);
            aloneSame += !before.empty() && before == after ? 1 : 0;
            std::printf("%s alone: %s\n", f.name, !before.empty() && before == after ? "the same modules" : (e1 + e2).c_str());
        }
        P.eq("render.library_alone", aloneSame, alone);

        // The newer constructs' program: as written, scrambled with no space where none is needed, and that copy
        // formatted, the same bits.
        {
            const pg::Sources files = { { "program.fm", newProgram() }, { "tools.fm", kNewTools } };
            pg::Sources tight;
            for (const pg::SourceFile& f : files)
                tight.push_back({ f.name, scramble(f.text, true, true) });
            bool ok = false;
            const pg::Sources formatted = formatAll(tight, ok);
            pg::measure::Session::Render a, b, c;
            std::string why;
            const bool bits = render(files, a, why) && render(tight, b, why) && ok && render(formatted, c, why) && sameBits(a, b)
                           && sameBits(a, c) && rms(a.l) > 1e-4;
            std::printf("render the newer constructs: %s (rms %.4f)\n", bits ? "the same bits" : ("DIFFERS " + why).c_str(), rms(a.l));
            P.eq("render.newer", bits ? 1 : 0, 1);
            P.eq("render.newer_formats_back", commentGaps(formatted[0].text) == commentGaps(files[0].text)
                                                  && commentGaps(formatted[1].text) == commentGaps(files[1].text) ? 1 : 0, 1);
        }
    }

    // ---- CHECK ------------------------------------------------------------------------------------------------------
    {
        int clean = 0;
        double checkMs = 0.0, compileMs = 0.0;
        for (const pg::Example& e : pg::kExamples)
        {
            const pg::Sources files = pg::withLibraryIncludes(pg::singleFile(e.source));
            const pg::Patch::Result c = pg::Patch::check(files);
            const pg::Patch::Result full = pg::Patch::compile(files, 48000.0f);
            clean += c.errors.empty() && c.patch == nullptr ? 1 : 0;
            checkMs += c.compileMs;
            compileMs += full.compileMs;
        }
        const double count = static_cast<double>(std::size(pg::kExamples));
        std::printf("check: %.1f ms an example; RUN's compile %.1f ms\n", checkMs / count, compileMs / count);
        P.eq("check.examples_clean", clean, static_cast<int64_t>(std::size(pg::kExamples)));
        P.le("check.ms", checkMs / count, 1000.0);
        P.le("check.faster_than_run", checkMs / count, compileMs / count);

        const std::string base = pg::kExamples[0].source;
        const char* const kMethod = pg::product::kIsInstrument ? "bool process(" : "void process(";
        const std::vector<std::pair<std::string, pg::Sources>> broken = {
            { "syntax", pg::singleFile(base + "module {\n") },
            { "undeclared", pg::singleFile(base + "float broken() { return nothing; }\n") },
            { "include", pg::singleFile("#include \"nofile.fm\"\n" + base) },
            { "method", pg::singleFile(replaced(base, kMethod, pg::product::kIsInstrument ? "bool proc(" : "void proc(")) },
        };
        for (const auto& [name, files] : broken)
        {
            const pg::Patch::Result c = pg::Patch::check(files);
            const pg::Patch::Result full = pg::Patch::compile(files, 48000.0f);
            std::printf("check %s: %s", name.c_str(), c.errors.c_str());
            P.eq("check." + name + "_as_compile", !c.errors.empty() && c.errors == full.errors ? 1 : 0, 1);
        }

        // tanf, FMinus's builtin since c75caab (Lang's table has it): a call compiles with no message; a program's own
        // function of that name is FMinus's error, the check's as RUN's.
        {
            const pg::Sources call = pg::singleFile(base + "float prewarp(float fc) { return tanf(3.14159265f * fc / 48000.0f); }\n");
            const pg::Patch::Result c = pg::Patch::compile(call, 48000.0f);
            P.eq("check.tanf_builtin", c.patch != nullptr && c.errors.empty() && pg::Patch::check(call).errors.empty() ? 1 : 0, 1);
            const pg::Sources own = pg::singleFile(base + "float tanf(float x) { return sinf(x) / cosf(x); }\n");
            const pg::Patch::Result o = pg::Patch::check(own), full = pg::Patch::compile(own, 48000.0f);
            std::printf("check own tanf: %s", o.errors.c_str());
            P.eq("check.tanf_own_refused", o.errors.find("'tanf' is a builtin") != std::string::npos && o.errors == full.errors ? 1 : 0, 1);
        }
    }

    // ---- NEWER ------------------------------------------------------------------------------------------------------
    {
        // the tokens
        {
            std::vector<lg::Token> t;
            bool inComment = false;
            lg::lexLine("a <<= b >>= c &= d |= e ^= f << g >> h & i | j ^ ~k <= l", 0, inComment, t);
            std::string ops;
            for (const lg::Token& x : t)
                ops += x.kind == lg::Tok::op ? std::string(x.text) + " " : std::string();
            std::printf("newer: operators %s\n", ops.c_str());
            P.eq("newer.bit_operators", ops == "<<= >>= &= |= ^= << >> & | ^ ~ <= " ? 1 : 0, 1);
            t.clear();
            lg::lexLine("switch case default enum [[fminus::default(3)]]", 0, inComment, t);
            P.eq("newer.keywords", t.size() == 14 && t[0].kind == lg::Tok::keyword && t[1].kind == lg::Tok::keyword
                                       && t[2].kind == lg::Tok::keyword && t[3].kind == lg::Tok::keyword ? 1 : 0, 1);
            P.eq("newer.attribute_default_a_name", t.size() == 14 && t[8].text == "default" && t[8].kind == lg::Tok::identifier ? 1 : 0, 1);
        }

        // the index of two files
        const std::string prog = newProgram();
        const char* const kMain = pg::product::kIsInstrument ? "Voice" : "Effect";
        lg::Options opts;
        opts.instrument = pg::product::kIsInstrument;
        lg::Service n;
        n.setOptions(opts);
        const auto files = [&](const std::string& text) { return pg::Sources{ { "program.fm", text }, { "tools.fm", kNewTools } }; };
        n.setFiles(files(prog));
        const auto sym = [&](std::string_view name, lg::Kind kind) -> const lg::Symbol* {
            for (const lg::Symbol& x : n.symbols())
                if (x.name == name && x.kind == kind)
                    return &x;
            return nullptr;
        };
        const lg::Symbol* square = sym("SQUARE", lg::Kind::constant);
        const lg::Symbol* triangle = sym("TRIANGLE", lg::Kind::constant);
        const lg::Symbol* mask = sym("MASK", lg::Kind::constant);
        const lg::Symbol* cols = sym("COLS", lg::Kind::constant);
        const lg::Symbol* wave = sym("Wave", lg::Kind::enumeration);
        const lg::Symbol* steps = sym("kSteps", lg::Kind::constant);
        const lg::Symbol* gain = sym("kGain", lg::Kind::constant);
        const lg::Symbol* held = sym("HELD", lg::Kind::constant);
        const lg::Symbol* spread = sym("SPREAD", lg::Kind::constant);
        const int tools = n.fileIndex("tools.fm");
        std::printf("newer: %s; %s; %s; %s; %s\n", square ? square->signature.c_str() : "-", mask ? mask->signature.c_str() : "-",
                    cols ? cols->signature.c_str() : "-", wave ? wave->signature.c_str() : "-", steps ? steps->signature.c_str() : "-");
        P.eq("newer.enum_values", square != nullptr && square->signature == "SQUARE = 4" && square->enumeration == "Wave"
                                      && square->doc == "a constant of its own value" && triangle != nullptr && triangle->value == 5
                                      && mask != nullptr && mask->value == 7 && mask->enumeration.empty() ? 1 : 0, 1);
        P.eq("newer.enum_other_file", cols != nullptr && cols->value == 16 && cols->file == 0 && cols->signature == "COLS = 16" ? 1 : 0, 1);
        P.eq("newer.enum_tag", wave != nullptr && wave->signature == "enum Wave { SAW, SQUARE = 4, TRIANGLE }" && wave->file == tools
                                   && wave->doc == "The waves shape() draws." ? 1 : 0, 1);
        P.eq("newer.table", steps != nullptr && steps->type == "const float[STEPS]" && steps->file == tools
                                && steps->signature == "const float kSteps[STEPS] = { 0, 3, 7, 12, 7, 3, 0, -5 }"
                                && steps->doc == "A pattern in semitones." ? 1 : 0, 1);
        P.eq("newer.scalar", gain != nullptr && gain->signature == "const float kGain = 0.25f" ? 1 : 0, 1);
        P.eq("newer.module_enum", held != nullptr && held->container == kMain && held->scope.from == held->scope.to ? 1 : 0, 1);
        P.eq("newer.block_enum", spread != nullptr && !(spread->scope.from == spread->scope.to) && spread->value == 3 ? 1 : 0, 1);
        const std::vector<std::string>& names = n.constantNames();
        const auto named = [&](const char* x) { return std::binary_search(names.begin(), names.end(), std::string(x)); };
        P.eq("newer.constant_names", named("SQUARE") && named("kSteps") && named("kGain") && named("COLS") && named("HELD")
                                         && named("SPREAD") && !named("Wave") && !named("shape") ? 1 : 0, 1);

        // the highlighter, with the service's names (the editor's TypeNames)
        {
            auto names2 = std::make_shared<pg::ui::TypeNames>();
            names2->names = n.typeNames();
            names2->constants = n.constantNames();
            const pg::ui::CodeHighlighter hl(names2);
            const auto styleOf = [&](const std::string& line, const std::string& word) -> int {
                const std::size_t at = line.find(word);
                std::vector<funkgui::text::Span> spans;
                hl.line(line, 0, spans);
                for (const funkgui::text::Span& sp : spans)
                    if (at != std::string::npos && sp.start == static_cast<int>(at) && sp.end == static_cast<int>(at + word.size()))
                        return sp.style;
                return -1;
            };
            const std::string use = "            const float y = kGain * shape((enum Wave)wave[i], phase) + 0.05f * noise;";
            P.eq("newer.highlight_table", styleOf("phase += 0.002f * kRatios[at] * expf(0.0577622650f * kSteps[at & MASK]);", "kSteps"),
                 pg::ui::csConstant);
            P.eq("newer.highlight_constant", styleOf(use, "kGain"), pg::ui::csConstant);
            P.eq("newer.highlight_other_file", styleOf("            const int at = stepOf(count) % COLS;", "COLS"), pg::ui::csConstant);
            P.eq("newer.highlight_tag", styleOf(use, "Wave"), pg::ui::csType);
            P.eq("newer.highlight_enum", styleOf(use, "enum"), pg::ui::csKeyword);
            P.eq("newer.highlight_case", styleOf("        case SQUARE:", "case") == pg::ui::csKeyword
                                             && styleOf("        case SQUARE:", "SQUARE") == pg::ui::csConstant ? 1 : 0, 1);
            P.eq("newer.highlight_switch", styleOf("    switch (w)", "switch"), pg::ui::csKeyword);
            P.eq("newer.highlight_bits", styleOf("    s &= ~8;", "&=") == pg::ui::csOperator && styleOf("    s &= ~8;", "~") == pg::ui::csOperator
                                             && styleOf("    s <<= 1;", "<<=") == pg::ui::csOperator ? 1 : 0, 1);
        }

        // completion
        {
            // The text with `from` replaced by `to` (a line, or more), completed at the end of `to`'s first line.
            const auto completeAt = [&](const std::string& from, const std::string& to) {
                const std::string text = replaced(prog, from, to);
                n.setFiles(files(text));
                return n.complete(0, posOf(text, to, static_cast<int>(to.find('\n'))));
            };
            const std::string line = "            phase -= floorf(phase);\n";
            lg::CompletionList c = completeAt(line, "            SQ\n");
            const lg::Completion* sq = item(c, "SQUARE");
            P.eq("newer.complete_constant", sq != nullptr && sq->kind == lg::Kind::constant && sq->detail == "SQUARE = 4" ? 1 : 0, 1);
            c = completeAt(line, "            kSt\n");
            const lg::Completion* ks = item(c, "kSteps");
            P.eq("newer.complete_table", ks != nullptr && !c.items.empty() && c.items[0].label == "kSteps"
                                             && ks->detail == "const float kSteps[STEPS] = { 0, 3, 7, 12, 7, 3, 0, -5 }" ? 1 : 0, 1);
            c = completeAt(line, "            HE\n");
            P.eq("newer.complete_module_constant", item(c, "HELD") != nullptr && item(c, "HELD")->kind == lg::Kind::constant ? 1 : 0, 1);
            c = completeAt(line, "            SP\n");
            P.eq("newer.complete_block_constant", item(c, "SPREAD") != nullptr ? 1 : 0, 1);
            c = completeAt("            enum { SPREAD = 3 };\n", "            SP\n            enum { SPREAD = 3 };\n");
            P.eq("newer.complete_block_constant_before", item(c, "SPREAD") == nullptr ? 1 : 0, 1);
            c = completeAt("    enum Stage stage;\n", "    enum \n");
            std::printf("newer: after enum: %s\n", labels(c).c_str());
            P.eq("newer.complete_tags", item(c, "Wave") != nullptr && item(c, "Stage") != nullptr && item(c, "SQUARE") == nullptr
                                            && item(c, "float") == nullptr ? 1 : 0, 1);
            c = completeAt(line, "            sw\n");
            const lg::Completion* sw = item(c, "switch");
            P.eq("newer.complete_keywords", sw != nullptr && sw->kind == lg::Kind::keyword
                                                && item(completeAt(line, "            ca\n"), "case") != nullptr
                                                && item(completeAt(line, "            de\n"), "default") != nullptr
                                                && item(completeAt(line, "            en\n"), "enum") != nullptr ? 1 : 0, 1);
            c = completeAt(line, "            ran\n");
            const lg::Completion* rf = item(c, "randf");
            const lg::CompletionList hs = completeAt(line, "            has\n");
            P.eq("newer.complete_builtins", rf != nullptr && rf->kind == lg::Kind::builtin && rf->detail == "float randf(int n)"
                                                && item(hs, "hash") != nullptr && item(hs, "hash")->detail == "int hash(int n)" ? 1 : 0, 1);
            n.setFiles(files(prog));
        }

        // hover, signature help, definitions, occurrences
        {
            std::optional<lg::Hover> h = n.hover(tools, posOf(kNewTools, "case SQUARE", 6));
            std::printf("newer: hover SQUARE [%s] %s\n", h ? h->code.c_str() : "-", h ? h->doc.c_str() : "");
            P.eq("newer.hover_constant", h && h->code == "SQUARE = 4" && h->doc.find("a constant of enum Wave") != std::string::npos ? 1 : 0, 1);
            h = n.hover(0, posOf(prog, "kSteps[at", 2));
            std::printf("newer: hover kSteps [%s] %s\n", h ? h->code.c_str() : "-", h ? h->doc.c_str() : "");
            P.eq("newer.hover_table", h && h->code == "const float kSteps[STEPS] = { 0, 3, 7, 12, 7, 3, 0, -5 }"
                                          && h->doc.find("a constant of the program") != std::string::npos
                                          && h->doc.find("(in tools.fm)") != std::string::npos ? 1 : 0, 1);
            h = n.hover(0, posOf(prog, "Wave)wave", 1));
            P.eq("newer.hover_tag", h && h->code == "enum Wave { SAW, SQUARE = 4, TRIANGLE }" && h->doc.find("an enumeration") != std::string::npos ? 1 : 0, 1);
            h = n.hover(0, posOf(prog, "COLS = STEPS", 1));
            P.eq("newer.hover_folded", h && h->code == "COLS = 16" ? 1 : 0, 1);
            h = n.hover(tools, posOf(kNewTools, "switch (w)", 2));
            P.eq("newer.hover_switch", h && h->doc.find("integer constant") != std::string::npos ? 1 : 0, 1);
            h = n.hover(0, posOf(prog, "randf(hash", 2));
            P.eq("newer.hover_randf", h && h->code == "float randf(int n)" && h->doc.find("white noise") != std::string::npos ? 1 : 0, 1);
            std::optional<lg::Signature> g = n.signature(0, posOf(prog, "hash(SPREAD", 5));
            P.eq("newer.signature_hash", g && g->label == "int hash(int n)" && g->active == 0 ? 1 : 0, 1);
            g = n.signature(0, posOf(prog, "^ count);", 0));
            P.eq("newer.signature_randf", g && g->label == "float randf(int n)" ? 1 : 0, 1);
            std::optional<lg::Location> d = n.definition(0, posOf(prog, "MASK]", 1));
            P.eq("newer.define_constant", d && d->fileName == "tools.fm" && d->at == posOf(kNewTools, "MASK = STEPS") ? 1 : 0, 1);
            d = n.definition(0, posOf(prog, "kSteps[at", 1));
            P.eq("newer.define_table", d && d->fileName == "tools.fm" && d->at == posOf(kNewTools, "kSteps[STEPS]") ? 1 : 0, 1);
            d = n.definition(0, posOf(prog, "Wave)wave", 1));
            P.eq("newer.define_tag", d && d->fileName == "tools.fm" && d->at == posOf(kNewTools, "Wave\n{") ? 1 : 0, 1);
            d = n.definition(0, posOf(prog, "HELD;", 1));
            P.eq("newer.define_module_constant", d && d->file == 0 && d->at == posOf(prog, "HELD, LET_GO") ? 1 : 0, 1);
            P.eq("newer.occur_constant", static_cast<int64_t>(n.occurrences(tools, posOf(kNewTools, "MASK = STEPS")).size()), 2);
            P.eq("newer.occur_steps", static_cast<int64_t>(n.occurrences(tools, posOf(kNewTools, "STEPS = 8")).size()), 3);
        }

        // a circuit field whose extent is an enumeration constant: Lang folds it
        {
            const std::string circuit = std::string("#include \"screamer.cir\"\nenum { CHANNELS = 2 };\nmodule ") + kMain
                                      + "\n{\n    Screamer amp[CHANNELS];\n};\n";
            lg::Service cs;
            cs.setFiles({ { "program.fm", circuit }, { "screamer.cir", libraryText("screamer.cir") } });
            const lg::CircuitDials dials = lg::circuitDials(cs, pg::product::kIsInstrument);
            P.eq("newer.circuit_extent", dials.bindings.size() == 1 && dials.bindings[0].dims == std::vector<int>{ 2 }
                                             && dials.bindings[0].count() == 2 && dials.bindings[0].names.size() == 3 ? 1 : 0, 1);
        }

        // Patch::check: the program clean; a switch on a float and a write to a constant table as RUN says them
        {
            const pg::Patch::Result c = pg::Patch::check(files(prog)), full = pg::Patch::compile(files(prog), 48000.0f);
            std::printf("newer: check %s; compile %s\n", c.errors.c_str(), full.errors.c_str());
            P.eq("newer.check_clean", c.errors.empty() && full.errors.empty() && full.patch != nullptr ? 1 : 0, 1);
            const std::string base = pg::kExamples[0].source;
            const pg::Sources onFloat = pg::singleFile(base + "int pick(float x)\n{\n    switch (x)\n    {\n        case 0: return 1;\n"
                                                              "        default: return 0;\n    }\n}\n");
            const pg::Patch::Result s1 = pg::Patch::check(onFloat), f1 = pg::Patch::compile(onFloat, 48000.0f);
            std::printf("newer: check a switch on a float: %s", s1.errors.c_str());
            P.eq("newer.check_switch_float", s1.errors.find("error: statement requires expression of integer type ('float' invalid)")
                                                     != std::string::npos && s1.errors == f1.errors ? 1 : 0, 1);
            const pg::Sources write = pg::singleFile(base + "const float kTable[4] = { 1, 2, 3, 4 };\nvoid poke(int i)\n{\n"
                                                            "    kTable[i] = 0;\n}\n");
            const pg::Patch::Result s2 = pg::Patch::check(write), f2 = pg::Patch::compile(write, 48000.0f);
            std::printf("newer: check a write to a constant table: %s", s2.errors.c_str());
            P.eq("newer.check_table_write", s2.errors.find("error: cannot assign to variable 'kTable' with const-qualified type 'const float[4]'")
                                                    != std::string::npos && s2.errors == f2.errors ? 1 : 0, 1);
        }
    }
    return P.finish();
}
