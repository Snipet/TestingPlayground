// Source/fminus/LangConditionals.cpp: FMinus's conditional compilation as a language service reads it (Lang.h,
// "Conditional compilation"): which groups of lines a compile reads, on the plugin's macros (kDefines) and the
// program's own (#define, #undef), and the macro a name is at a place. Plain C++ over the files' text, as FMinus's
// scanner reads them (its recipe 31): a directive is a line that starts with #, after blanks, outside a block comment;
// a skipped group's block comments and its conditionals' names are read, nothing else of it; macros are the
// compilation's, an #include's file read where its first #include in a group read is, once. Where FMinus would stop
// with an error (an #else without #if, a bad expression, an unterminated conditional), this reads on as well as it
// can: the check while typing and RUN say what is wrong.
#include "fminus/Lang.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace playground::lang
{
    namespace
    {
        bool blank(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }
        bool identStart(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

        // Past the blanks and the block comments that end on the line, from `i` (C reads a comment there as a space).
        std::size_t skipSpace(std::string_view s, std::size_t i)
        {
            while (i < s.size())
            {
                if (blank(s[i]))
                    ++i;
                else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*')
                {
                    const std::size_t end = s.find("*/", i + 2);
                    if (end == std::string_view::npos)
                        return s.size();
                    i = end + 2;
                }
                else
                    break;
            }
            return i;
        }

        std::string_view identAt(std::string_view s, std::size_t& i)
        {
            const std::size_t from = i;
            if (i < s.size() && identStart(s[i]))
                while (i < s.size() && isIdentChar(s[i]))
                    ++i;
            return s.substr(from, i - from);
        }

        // A directive's line without its comments, each run of blanks one space, trimmed: a macro's value as C compares
        // it.
        std::string squeezed(std::string_view s)
        {
            std::string out;
            bool space = false;
            for (std::size_t i = 0; i < s.size();)
            {
                if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/')
                    break;
                if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*')
                {
                    const std::size_t end = s.find("*/", i + 2);
                    i = end == std::string_view::npos ? s.size() : end + 2;
                    space = true;
                    continue;
                }
                if (blank(s[i]))
                {
                    space = true;
                    ++i;
                    continue;
                }
                if (space && !out.empty())
                    out += ' ';
                space = false;
                out += s[i++];
            }
            return out;
        }

        // ---- #if's expressions (C17 6.10.1: 64-bit integers, macros expanded as text) ----------------------------
        struct PpToken
        {
            enum class Kind : uint8_t { name, number, punct } kind = Kind::punct;
            std::string text;
        };

        // C's preprocessing tokens of `s` (comments are spaces); false for a character C does not read there.
        bool ppLex(std::string_view s, std::vector<PpToken>& out)
        {
            static constexpr std::string_view kTwo[] = { "<<", ">>", "<=", ">=", "==", "!=", "&&", "||" };
            for (std::size_t i = 0; i < s.size();)
            {
                const char c = s[i];
                if (blank(c))
                {
                    ++i;
                    continue;
                }
                if (c == '/' && i + 1 < s.size() && s[i + 1] == '/')
                    break;
                if (c == '/' && i + 1 < s.size() && s[i + 1] == '*')
                {
                    const std::size_t end = s.find("*/", i + 2);
                    i = end == std::string_view::npos ? s.size() : end + 2;
                    continue;
                }
                if (identStart(c))
                {
                    out.push_back({ PpToken::Kind::name, std::string(identAt(s, i)) });
                    continue;
                }
                if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9'))
                {
                    // A preprocessing number: digits, letters, _, ., and a sign after an exponent's letter.
                    const std::size_t from = i++;
                    while (i < s.size())
                    {
                        const char d = s[i];
                        if ((d == '+' || d == '-') && (s[i - 1] == 'e' || s[i - 1] == 'E' || s[i - 1] == 'p' || s[i - 1] == 'P'))
                            ++i;
                        else if (isIdentChar(d) || d == '.')
                            ++i;
                        else
                            break;
                    }
                    out.push_back({ PpToken::Kind::number, std::string(s.substr(from, i - from)) });
                    continue;
                }
                bool two = false;
                for (const std::string_view op : kTwo)
                    if (s.substr(i, 2) == op)
                    {
                        out.push_back({ PpToken::Kind::punct, std::string(op) });
                        i += 2;
                        two = true;
                        break;
                    }
                if (two)
                    continue;
                if (std::string_view("()!~-+*/%<>&^|?:,").find(c) == std::string_view::npos)
                    return false;
                out.push_back({ PpToken::Kind::punct, std::string(1, c) });
                ++i;
            }
            return true;
        }

        struct Def
        {
            std::string value;
            int file = -1, line = -1;
        };
        using Table = std::map<std::string, Def, std::less<>>;

        // FMinus's bounds on a compilation's conditions (recipe 31): nesting, tokens after expansion, and the work of
        // expanding, counted across the compilation.
        constexpr int kMaxDepth = 256;
        constexpr std::size_t kMaxTokens = 65536;
        constexpr std::size_t kMaxExpansion = 1048576;

        class Evaluator
        {
        public:
            Evaluator(const Table& macros, std::size_t& work) : macros_(macros), work_(work) {}

            // The expression's value; nothing when FMinus would refuse it.
            std::optional<int64_t> evaluate(std::string_view text)
            {
                std::vector<PpToken> raw;
                if (!ppLex(text, raw))
                    return std::nullopt;
                std::vector<std::string> active;
                if (!expand(raw, active, 0, tokens_))
                    return std::nullopt;
                at_ = 0;
                ok_ = true;
                depth_ = 0;
                const int64_t v = conditional(true);
                if (!ok_ || at_ != tokens_.size())
                    return std::nullopt;
                return v;
            }

        private:
            const Table& macros_;
            std::size_t& work_;
            std::vector<PpToken> tokens_;
            std::size_t at_ = 0;
            bool ok_ = true;
            int depth_ = 0;

            bool expand(const std::vector<PpToken>& in, std::vector<std::string>& active, int depth, std::vector<PpToken>& out)
            {
                if (depth > kMaxDepth)
                    return false;
                for (std::size_t i = 0; i < in.size(); ++i)
                {
                    const PpToken& t = in[i];
                    if (t.kind == PpToken::Kind::name && t.text == "defined")
                    {
                        // Its operand is never expanded: `defined NAME` or `defined ( NAME )`.
                        out.push_back(t);
                        std::size_t k = i + 1;
                        const bool paren = k < in.size() && in[k].text == "(";
                        const std::size_t last = std::min(in.size(), k + (paren ? 3 : 1));
                        for (; k < last; ++k)
                            out.push_back(in[k]);
                        i = last - 1;
                        continue;
                    }
                    const auto m = t.kind == PpToken::Kind::name ? macros_.find(t.text) : macros_.end();
                    if (m == macros_.end() || std::find(active.begin(), active.end(), t.text) != active.end())
                    {
                        out.push_back(t);
                        if (out.size() > kMaxTokens)
                            return false;
                        continue;
                    }
                    work_ += 1 + m->second.value.size();
                    if (work_ > kMaxExpansion)
                        return false;
                    std::vector<PpToken> value;
                    if (!ppLex(m->second.value, value))
                        return false;
                    active.push_back(t.text);
                    const bool ok = expand(value, active, depth + 1, out);
                    active.pop_back();
                    if (!ok)
                        return false;
                }
                return true;
            }

            const PpToken* peek() const { return at_ < tokens_.size() ? &tokens_[at_] : nullptr; }
            bool is(std::string_view p) const { return at_ < tokens_.size() && tokens_[at_].kind == PpToken::Kind::punct && tokens_[at_].text == p; }

            int64_t fail()
            {
                ok_ = false;
                return 0;
            }

            // `?:` and the binary operators by precedence climbing; `run`: whether the value is computed (C computes
            // only what && || and ?: choose, so an error past them is none).
            int64_t conditional(bool run)
            {
                if (++depth_ > kMaxDepth)
                    return fail();
                const int64_t c = binary(0, run);
                if (!ok_ || !is("?"))
                {
                    --depth_;
                    return c;
                }
                ++at_;
                const int64_t a = conditional(run && c != 0);
                if (!ok_ || !is(":"))
                    return fail();
                ++at_;
                const int64_t b = conditional(run && c == 0);
                --depth_;
                return c != 0 ? a : b;
            }

            static int precedence(std::string_view op)
            {
                if (op == "||") return 1;
                if (op == "&&") return 2;
                if (op == "|") return 3;
                if (op == "^") return 4;
                if (op == "&") return 5;
                if (op == "==" || op == "!=") return 6;
                if (op == "<" || op == ">" || op == "<=" || op == ">=") return 7;
                if (op == "<<" || op == ">>") return 8;
                if (op == "+" || op == "-") return 9;
                if (op == "*" || op == "/" || op == "%") return 10;
                return 0;
            }

            int64_t binary(int least, bool run)
            {
                int64_t left = unary(run);
                while (ok_)
                {
                    const PpToken* t = peek();
                    const int p = t != nullptr && t->kind == PpToken::Kind::punct ? precedence(t->text) : 0;
                    if (p == 0 || p <= least)
                        break;
                    const std::string op = t->text;
                    ++at_;
                    const bool rightRuns = run && !(op == "&&" && left == 0) && !(op == "||" && left != 0);
                    const int64_t right = binary(p, rightRuns);
                    if (!ok_)
                        break;
                    left = apply(op, left, right, run && rightRuns);
                }
                return left;
            }

            int64_t apply(const std::string& op, int64_t a, int64_t b, bool run)
            {
                const auto u = [](int64_t x) { return static_cast<uint64_t>(x); };
                const auto s = [](uint64_t x) { return static_cast<int64_t>(x); };
                if (op == "||") return (a != 0 || b != 0) ? 1 : 0;
                if (op == "&&") return (a != 0 && b != 0) ? 1 : 0;
                if (op == "|") return a | b;
                if (op == "^") return a ^ b;
                if (op == "&") return a & b;
                if (op == "==") return a == b ? 1 : 0;
                if (op == "!=") return a != b ? 1 : 0;
                if (op == "<") return a < b ? 1 : 0;
                if (op == ">") return a > b ? 1 : 0;
                if (op == "<=") return a <= b ? 1 : 0;
                if (op == ">=") return a >= b ? 1 : 0;
                if (op == "<<") return s(u(a) << (u(b) & 63u));
                if (op == ">>") return a >> (u(b) & 63u);
                if (op == "+") return s(u(a) + u(b));
                if (op == "-") return s(u(a) - u(b));
                if (op == "*") return s(u(a) * u(b));
                if (op == "/" || op == "%")
                {
                    if (b == 0)
                        return run ? fail() : 0;
                    if (a == std::numeric_limits<int64_t>::min() && b == -1)
                        return op == "/" ? a : 0;
                    return op == "/" ? a / b : a % b;
                }
                return fail();
            }

            int64_t unary(bool run)
            {
                if (++depth_ > kMaxDepth)
                    return fail();
                int64_t v = 0;
                if (is("!") || is("~") || is("-") || is("+"))
                {
                    const std::string op = tokens_[at_++].text;
                    const int64_t x = unary(run);
                    v = op == "!" ? (x == 0 ? 1 : 0) : op == "~" ? ~x
                      : op == "-" ? static_cast<int64_t>(0u - static_cast<uint64_t>(x)) : x;
                }
                else
                    v = primary(run);
                --depth_;
                return v;
            }

            int64_t primary(bool run)
            {
                const PpToken* t = peek();
                if (t == nullptr)
                    return fail();
                if (t->kind == PpToken::Kind::punct && t->text == "(")
                {
                    ++at_;
                    const int64_t v = conditional(run);
                    if (!ok_ || !is(")"))
                        return fail();
                    ++at_;
                    return v;
                }
                if (t->kind == PpToken::Kind::number)
                {
                    ++at_;
                    return number(t->text);
                }
                if (t->kind == PpToken::Kind::name)
                {
                    ++at_;
                    if (t->text == "defined")
                    {
                        const bool paren = is("(");
                        if (paren)
                            ++at_;
                        const PpToken* n = peek();
                        if (n == nullptr || n->kind != PpToken::Kind::name)
                            return fail();
                        ++at_;
                        if (paren)
                        {
                            if (!is(")"))
                                return fail();
                            ++at_;
                        }
                        return macros_.count(n->text) != 0 ? 1 : 0;
                    }
                    return t->text == "true" ? 1 : 0;     // false, and any other name, is 0
                }
                return fail();
            }

            // An integer as FMinus writes one, decimal, octal or hex, no suffix (it has no unsigned or long).
            int64_t number(const std::string& s)
            {
                uint64_t v = 0;
                std::size_t i = 0;
                unsigned base = 10;
                if (s.size() > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
                {
                    base = 16;
                    i = 2;
                    if (i == s.size())
                        return fail();
                }
                else if (s.size() > 1 && s[0] == '0')
                    base = 8;
                for (; i < s.size(); ++i)
                {
                    const char c = s[i];
                    unsigned d = 0;
                    if (c >= '0' && c <= '9')
                        d = static_cast<unsigned>(c - '0');
                    else if (base == 16 && c >= 'a' && c <= 'f')
                        d = static_cast<unsigned>(c - 'a' + 10);
                    else if (base == 16 && c >= 'A' && c <= 'F')
                        d = static_cast<unsigned>(c - 'A' + 10);
                    else
                        return fail();                    // a suffix, a point, an exponent: FMinus's error
                    if (d >= base)
                        return fail();
                    if (v > (static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - d) / base)
                        return fail();
                    v = v * base + d;
                }
                return static_cast<int64_t>(v);
            }
        };

        // ---- the walk ---------------------------------------------------------------------------------------------
        struct Walk
        {
            const Sources& files;
            Table macros;
            std::vector<FileConditionals> out;
            std::vector<bool> visited;
            std::size_t work = 0;
            // macroAt's place: the table in force when the walk reaches (stopFile, stopLine), kept in `seen`.
            int stopFile = -1, stopLine = -1;
            std::optional<Table> seen;

            Walk(const Sources& f, const Defines& defines) : files(f), out(f.size()), visited(f.size(), false)
            {
                reset(defines);
            }

            void reset(const Defines& defines)
            {
                macros.clear();
                work = 0;
                for (const auto& [name, value] : defines)
                    macros[name] = { value, -1, -1 };
            }

            static bool fminusFile(std::string_view name) { return !isCircuitFile(name) && !isGuiName(name); }
            static bool isGuiName(std::string_view name)
            {
                return name.size() > 4 && name.substr(name.size() - 4) == ".gui";   // GuiSpec.h's isGuiFile
            }

            int fileNamed(std::string_view name) const
            {
                for (std::size_t i = 0; i < files.size(); ++i)
                    if (files[i].name == name)
                        return static_cast<int>(i);
                return -1;
            }

            void see(int f, int line)
            {
                if (f == stopFile && line >= stopLine && !seen)
                    seen = macros;
            }

            void walkFile(int f, bool read)
            {
                visited[static_cast<std::size_t>(f)] = true;
                FileConditionals& fc = out[static_cast<std::size_t>(f)];
                fc.read = read;
                const std::string_view text = files[static_cast<std::size_t>(f)].text;
                std::vector<std::pair<int, std::string_view>> lines;
                directiveLines(text, lines);
                const int count = static_cast<int>(splitLines(text).size());
                fc.skipped.assign(static_cast<std::size_t>(count), 0);
                struct Open
                {
                    bool taken = false, skipping = false, inert = false;
                };
                std::vector<Open> stack;
                const auto skipping = [&] { return !stack.empty() && stack.back().skipping; };
                int lastLine = 0;                     // lines [lastLine, line) take the state before this directive
                const auto fill = [&](int upTo) {
                    for (int l = lastLine; l < upTo && l < count; ++l)
                        fc.skipped[static_cast<std::size_t>(l)] = skipping() ? 1 : 0;
                };
                for (const auto& [line, d] : lines)
                {
                    fill(line);
                    lastLine = line + 1;
                    see(f, line);
                    std::size_t rest = 0;
                    const std::string_view name = directiveName(d, &rest);
                    const std::string_view after = d.substr(std::min(rest, d.size()));
                    if (isConditional(name))
                    {
                        Conditional c;
                        c.line = line;
                        const bool opens = name == "if" || name == "ifdef" || name == "ifndef";
                        if (opens)
                        {
                            if (skipping())
                            {
                                stack.push_back({ true, true, true });
                                c.inert = true;
                            }
                            else
                            {
                                const bool v = holds(name, after);
                                stack.push_back({ v, !v, false });
                            }
                        }
                        else if (!stack.empty())
                        {
                            Open& o = stack.back();
                            c.inert = o.inert;
                            if (!o.inert)
                            {
                                if (name == "else")
                                {
                                    o.skipping = o.taken;
                                    o.taken = true;
                                }
                                else if (name == "endif")
                                    ;
                                else if (o.taken)
                                    o.skipping = true;     // an #elif after a group read: never computed
                                else
                                {
                                    const bool v = holds(name, after);
                                    o.taken = v;
                                    o.skipping = !v;
                                }
                            }
                            if (name == "endif")
                                stack.pop_back();
                        }
                        c.skipping = skipping();
                        fc.skipped[static_cast<std::size_t>(line)] = c.inert ? 1 : 0;
                        fc.directives.push_back(c);
                        continue;
                    }
                    fc.skipped[static_cast<std::size_t>(line)] = skipping() ? 1 : 0;
                    if (skipping())
                        continue;                      // a skipped group's other lines are text
                    if (name == "define")
                        define(after, f, line);
                    else if (name == "undef")
                    {
                        std::size_t i = skipSpace(after, 0);
                        macros.erase(std::string(identAt(after, i)));
                    }
                    else if (name == "include")
                    {
                        std::string included;
                        int a = 0, b = 0;
                        if (includeOn(d, included, a, b))
                            if (const int g = fileNamed(included); g >= 0 && !visited[static_cast<std::size_t>(g)]
                                                                   && fminusFile(files[static_cast<std::size_t>(g)].name))
                                walkFile(g, read);
                    }
                }
                fill(count);
                see(f, count);
            }

            // Whether the condition of an #if (#elif), #ifdef (#elifdef) or #ifndef (#elifndef) holds.
            bool holds(std::string_view name, std::string_view after)
            {
                if (name == "if" || name == "elif")
                {
                    Evaluator e(macros, work);
                    const std::optional<int64_t> v = e.evaluate(after);
                    return v.has_value() && *v != 0;
                }
                std::size_t i = skipSpace(after, 0);
                const std::string_view macro = identAt(after, i);
                const bool defined = !macro.empty() && macros.count(macro) != 0;
                return (name == "ifdef" || name == "elifdef") ? defined : !defined;
            }

            void define(std::string_view after, int f, int line)
            {
                std::size_t i = skipSpace(after, 0);
                const std::string_view name = identAt(after, i);
                if (name.empty() || name == "defined" || isKeyword(name) || isTypeWord(name))
                    return;                            // FMinus's error: no macro
                if (i < after.size() && after[i] == '(')
                    return;                            // a function-like macro: FMinus's error
                macros[std::string(name)] = { squeezed(after.substr(i)), f, line };
            }

            // The program from its first file, which reads the others it #includes.
            void run()
            {
                if (!files.empty() && fminusFile(files.front().name))
                    walkFile(0, true);
            }
        };
    }

    // ---- the API --------------------------------------------------------------------------------------------------
    Defines hostDefines()
    {
        Defines out;
        for (const Define& d : kDefines)
            out.emplace_back(d.name, d.value);
        return out;
    }

    void directiveLines(std::string_view text, std::vector<std::pair<int, std::string_view>>& out)
    {
        // As lexLine reads them: a line that starts with # (after blanks) outside a block comment is one directive;
        // a block comment opened on a line of code runs on to its */.
        bool inComment = false;
        int ln = 0;
        for (const std::string_view l : splitLines(text))
        {
            std::size_t i = 0;
            if (inComment)
            {
                const std::size_t end = l.find("*/");
                if (end == std::string_view::npos)
                {
                    ++ln;
                    continue;
                }
                i = end + 2;
                inComment = false;
            }
            else
            {
                std::size_t first = 0;
                while (first < l.size() && blank(l[first]))
                    ++first;
                if (first < l.size() && l[first] == '#')
                {
                    out.emplace_back(ln++, l);
                    continue;
                }
            }
            while (i < l.size())
            {
                if (l[i] == '/' && i + 1 < l.size() && l[i + 1] == '/')
                    break;
                if (l[i] == '/' && i + 1 < l.size() && l[i + 1] == '*')
                {
                    const std::size_t end = l.find("*/", i + 2);
                    if (end == std::string_view::npos)
                    {
                        inComment = true;
                        break;
                    }
                    i = end + 2;
                    continue;
                }
                ++i;
            }
            ++ln;
        }
    }

    std::string_view directiveName(std::string_view line, std::size_t* end)
    {
        std::size_t i = 0;
        while (i < line.size() && blank(line[i]))
            ++i;
        if (i >= line.size() || line[i] != '#')
        {
            if (end != nullptr)
                *end = 0;
            return {};
        }
        i = skipSpace(line, i + 1);
        const std::string_view name = identAt(line, i);
        if (end != nullptr)
            *end = i;
        return name;
    }

    bool isConditional(std::string_view name) noexcept
    {
        return name == "if" || name == "ifdef" || name == "ifndef" || name == "elif" || name == "elifdef"
            || name == "elifndef" || name == "else" || name == "endif";
    }

    std::vector<FileConditionals> conditionals(const Sources& files, const Defines& defines)
    {
        Walk w(files, defines);
        w.run();
        for (std::size_t f = 0; f < files.size(); ++f)
            if (!w.visited[f] && Walk::fminusFile(files[f].name))
            {
                w.reset(defines);
                w.walkFile(static_cast<int>(f), false);
            }
        return std::move(w.out);
    }

    std::optional<Macro> macroAt(const Sources& files, const Defines& defines, int file, int line, std::string_view name)
    {
        if (file < 0 || file >= static_cast<int>(files.size()))
            return std::nullopt;
        Walk w(files, defines);
        w.stopFile = file;
        w.stopLine = line;
        w.run();
        if (!w.seen)
            for (std::size_t f = 0; f < files.size() && !w.seen; ++f)
                if (!w.visited[f] && Walk::fminusFile(files[f].name))
                {
                    w.reset(defines);
                    w.walkFile(static_cast<int>(f), false);
                }
        if (!w.seen)
            return std::nullopt;
        const auto m = w.seen->find(name);
        if (m == w.seen->end())
            return std::nullopt;
        return Macro{ std::string(name), m->second.value, m->second.file, m->second.line };
    }

    std::vector<std::string> macroNames(const Sources& files, const Defines& defines)
    {
        std::vector<std::string> out;
        for (const auto& [name, value] : defines)
            out.push_back(name);
        for (const SourceFile& f : files)
        {
            if (isCircuitFile(f.name))
                continue;
            std::vector<std::pair<int, std::string_view>> lines;
            directiveLines(f.text, lines);
            for (const auto& [line, d] : lines)
            {
                std::size_t rest = 0;
                if (directiveName(d, &rest) != "define")
                    continue;
                std::size_t i = skipSpace(d, rest);
                const std::string_view n = identAt(d, i);
                if (!n.empty())
                    out.emplace_back(n);
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }
}
