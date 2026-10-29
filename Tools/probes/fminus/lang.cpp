// PROBE name=fminus.lang timeout=600
//
// fminus.lang: FMinus's language service (fminus/Lang.h), its formatter, and the editor's check while typing
// (Patch::check):
//   LEXER     a line of each kind of token: names, keywords, type words, C's numbers (.5f, 0x1Fp-3, 1e-3), operators
//             longest first (->, ++, &&, ::), punctuation, a // comment; a directive is one token; a block comment
//             carries over the line's end; a character FMinus does not read is `unknown`
//   INDEX     a program of three files (program.fm, tools.fm it includes, svf.fm the library's) and host.fm: the
//             modules (Effect, Host, Svf), a param with its range in its signature, fields with their types, methods
//             with their parameters, a function of another file, locals and arguments with the scope they are seen in
//   COMPLETE  a typed prefix offers what it names first (a field, `fi` -> filter), with its type; after `.` a module's
//             members (methods and fields), after `->` the Host's fields; after `[[` the attributes; in #include's
//             quotes the files (the program's, host.fm, the library's); a local in its scope (and not after it), the
//             loop's variable in the loop; a builtin with its signature (tanhf, and tanf, FMinus's builtin since
//             c75caab); a keyword; a library module the program does
//             not read, which adds its #include (lfo.fm); nothing in a comment
//   SIGNATURE in a call: a method's (receiver's type resolved), a builtin's, a function's of another file, a vector
//             literal's lanes; the parameter the place is in, and its place in the label
//   HOVER     a method (its declaration and the usage header's line for it), a Host field (host.fm's line), a module
//             (its file's header), a local (const float y), a param (its range), a keyword, a builtin (tanhf, tanf)
//   DEFINE    a function of another file, a field, a module of the library's file, an #include's file
//   OCCUR     a field's uses in its file (the declaration, the calls); a loop variable's, not another loop's
//   UNCLOSED  a module whose `{` has no partner (a closer missing in a method, the file cut short) holds the rest of
//             the file: its members are completed, hovered and found there
//   FORMAT    every example and library file is the house style already (format changes nothing); three scrambled
//             copies of each (no indent; tokens single-spaced, braces K&R; no space at all where none is needed) format
//             back to it (comment gaps aside), each result formatted again unchanged; a messy program formats to the
//             house style; text with a character FMinus does not read is left as it is, with why
//   RENDER    each example of this product, formatted, renders what it rendered before, bit for bit (measure::Session:
//             an effect over noise, an instrument's chord); RANDOM programs with every file formatted (the program and
//             its library files) the same, until every library file this product's RANDOM uses has been through it;
//             the other library files formatted compile alone with the same modules, fields and methods
//   CHECK     Patch::check: every example is clean; a syntax error, an undeclared name, an #include of no file and a
//             missing method give compile()'s messages word for word; its time (FMinus's pass at -O0) against RUN's;
//             a call of tanf compiles clean, and a program's own tanf is FMinus's error, the check's as RUN's
#include "Probe.h"

#include "dsp/Measure.h"
#include "dsp/Sources.h"
#include "fminus/Lang.h"
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
            corpus.push_back({ f.name, f.source });
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
    }

    // ---- RENDER -----------------------------------------------------------------------------------------------------
    {
        int examples = 0, same = 0;
        for (const pg::Example& e : pg::kExamples)
        {
            ++examples;
            const pg::Sources files = pg::singleFile(e.source);
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
            if (rendered.count(f.name) != 0)
                continue;
            ++alone;
            std::string e1, e2;
            const std::string before = shapeOf(f.source, e1);
            const std::string after = shapeOf(lg::format(f.source, types).text, e2);
            aloneSame += !before.empty() && before == after ? 1 : 0;
            std::printf("%s alone: %s\n", f.name, !before.empty() && before == after ? "the same modules" : (e1 + e2).c_str());
        }
        P.eq("render.library_alone", aloneSame, alone);
    }

    // ---- CHECK ------------------------------------------------------------------------------------------------------
    {
        int clean = 0;
        double checkMs = 0.0, compileMs = 0.0;
        for (const pg::Example& e : pg::kExamples)
        {
            const pg::Patch::Result c = pg::Patch::check(pg::singleFile(e.source));
            const pg::Patch::Result full = pg::Patch::compile(pg::singleFile(e.source), 48000.0f);
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
    return P.finish();
}
