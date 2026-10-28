// Source/fminus/Lang.cpp: see Lang.h (the formatter is LangFormat.cpp).
#include "fminus/Lang.h"

#include "fminus/Patch.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <utility>

namespace playground::lang
{
    namespace
    {
        constexpr std::array<std::string_view, 16> kKeywords = { "break", "const", "continue", "else", "false", "for",
                                                                 "if", "module", "param", "parallel", "restrict",
                                                                 "return", "sizeof", "this", "true", "while" };
        constexpr std::array<std::string_view, 8> kTypeWords = { "bool", "float", "float2", "float4", "float8",
                                                                 "int", "poly", "void" };

        // ---- the builtins: name, result, parameters, what it does ---------------------------------------------------
        struct BuiltinInfo
        {
            std::string_view name, result, params, doc;
        };
        constexpr BuiltinInfo kBuiltins[] = {
            { "fabsf", "float", "float x", "The absolute value, exact. A float vector too, lane by lane." },
            { "sqrtf", "float", "float x", "The square root, exact. A float vector too, lane by lane." },
            { "floorf", "float", "float x", "The largest whole number not above x, exact. A float vector too." },
            { "fminf", "float", "float x, float y", "The smaller of the two (a NaN argument is ignored), exact. Float vectors too." },
            { "fmaxf", "float", "float x, float y", "The larger of the two (a NaN argument is ignored), exact. Float vectors too." },
            { "sinf", "float", "float x", "The sine of x, in radians, by the program's math mode: one result for a float "
                                          "wherever it is computed. A float vector too." },
            { "cosf", "float", "float x", "The cosine of x, in radians, by the math mode. A float vector too." },
            { "expf", "float", "float x", "e to the power x, by the math mode: expf(0.115129255f * dB) is a level's gain. "
                                          "A float vector too." },
            { "logf", "float", "float x", "The natural logarithm, by the math mode. A float vector too." },
            { "tanhf", "float", "float x", "The hyperbolic tangent, by the math mode: a soft clipper, -1 to 1. A float "
                                           "vector too." },
            { "malloc", "void*", "int size", "size bytes, zeroed and aligned to 64 (null when there are none). It "
                                             "allocates: call it in init, never in process, which FMinus refuses." },
            { "calloc", "void*", "int count, int size", "count elements of size bytes, zeroed: calloc(n, sizeof(float)) "
                                                        "for a delay line. Init only, as malloc." },
            { "free", "void", "void* p", "Gives back what malloc or calloc gave: in end." },
            { "reduce_add", "float", "float4 v", "A float vector's lanes summed in order: ((v.x + v.y) + v.z) + v.w." },
            { "reduce_min", "float", "float4 v", "A float vector's smallest lane (NaN lanes ignored)." },
            { "reduce_max", "float", "float4 v", "A float vector's largest lane (NaN lanes ignored)." },
            { "poly_lanes", "int", "", "How many lanes a poly float has on this machine (4 with NEON)." },
            { "poly_index", "poly float", "", "A poly float whose lanes hold their own index: 0, 1, 2, ..." },
            { "poly_load", "poly float", "const float* p, poly bool active",
              "The poly_lanes() floats at p, lane i from p[i]; with `active` (optional), only its lanes, the others 0 "
              "and not read." },
            { "poly_store", "void", "float* p, poly float v, poly bool active",
              "v into the poly_lanes() floats at p; with `active` (optional), only its lanes." },
            { "fft_create", "void*", "int n", "A real FFT's plan for n points (a multiple of 32 made of 2s, 3s and 5s; "
                                              "null for another n). It allocates: in init." },
            { "fft_create_complex", "void*", "int n", "A complex FFT's plan for n points (a multiple of 16 made of 2s, "
                                                      "3s and 5s). It allocates: in init." },
            { "fft_forward", "void", "void* plan, const float* in, float2* out",
              "n floats into n/2 + 1 bins (float2: re, im) with a real plan, or n float2 into n bins with a complex "
              "one. Unnormalised; allocates nothing." },
            { "fft_inverse", "void", "void* plan, const float2* in, float* out",
              "Bins back to n samples (float2 to float2 with a complex plan): n times the signal, unnormalised." },
            { "fft_destroy", "void", "void* plan", "Frees a plan: in end." },
        };

        struct WordDoc
        {
            std::string_view word, code, doc;
        };
        constexpr WordDoc kWordDocs[] = {
            { "module", "module Name { fields; params; methods };",
              "FMinus's class: a struct with member functions. Its fields start at 0 (a method such as init sets them); "
              "its name is a type from here on." },
            { "param", "param float name [[fminus::range(lo, hi)]] [[fminus::default(v)]];",
              "A field the host sets: the main module's are the plugin's dials, in order. A method reads it as "
              "name[i], its value at sample i of the call." },
            { "parallel", "parallel for (...) s    parallel { s1 s2 }",
              "The loop's iterations, or the block's statements, at once on the machine's cores, all of them finished "
              "where the statement ends." },
            { "restrict", "float* restrict p", "A promise (C99's) that nothing else reaches this memory while the "
                                               "pointer is used: the optimiser may vectorise freely." },
            { "const", "const float x = e;", "Never assigned after its initialiser; through a const pointer, read only." },
            { "this", "this", "The module's own address, in its method." },
            { "sizeof", "sizeof(type)", "The type's size in bytes, an int: calloc(n, sizeof(float))." },
            { "if", "if (condition) statement else statement", "An else closes the nearest if." },
            { "else", "if (condition) statement else statement", "" },
            { "for", "for (int i = 0; i < n; ++i) statement", "A loop; its declarations are the loop's." },
            { "while", "while (condition) statement", "" },
            { "break", "break;", "Leaves the innermost loop." },
            { "continue", "continue;", "The innermost loop's next iteration." },
            { "return", "return value;", "" },
            { "true", "true", "A bool: 1." },
            { "false", "false", "A bool: 0." },
            { "float", "float", "IEEE single precision. In FMinus a constant without the f suffix is a float too." },
            { "int", "int", "32 bits, signed; / and % as C's (1 / 2 is 0)." },
            { "bool", "bool", "0 or 1; a number becomes x != 0." },
            { "void", "void", "No value: a function's result. void* is an address with no type." },
            { "float2", "float2", "OpenCL C's vector of 2 floats: lane-wise operators, .x .y, (float2)(a, b)." },
            { "float4", "float4", "OpenCL C's vector of 4 floats: lane-wise operators, swizzles (.x, .wzyx, .s0), "
                                  "(float4)(a, b, c, d), reduce_add." },
            { "float8", "float8", "A vector of 8 floats (.s0 to .s7); keep it inside FMinus." },
            { "poly", "poly float    poly bool", "A vector as wide as the machine's (poly_lanes() lanes): "
                                                 "poly_load, poly_store, poly_index." },
        };

        struct AttributeInfo
        {
            std::string_view label, insert, doc;
        };
        constexpr AttributeInfo kAttributes[] = {
            { "fminus::range", "fminus::range($0)", "A param's range: what its dial spans, low and high (FMinus does "
                                                    "not clamp to it)." },
            { "fminus::default", "fminus::default($0)", "A param's value when the program starts, and a double-click "
                                                        "on its dial." },
            { "clang::nonblocking", "clang::nonblocking", "May run on the audio thread: FMinus checks it cannot "
                                                          "allocate, lock or call what is not nonblocking itself." },
            { "clang::nonallocating", "clang::nonallocating", "Cannot reach malloc, calloc or free (checked)." },
        };

        constexpr std::string_view kPanelWords[] = { "section", "row", "label", "small", "style", "options", "color",
                                                     "meter", "vu", "led", "number", "scope" };
        constexpr std::string_view kPanelStyles[] = { "knob", "vintage", "fader", "switch", "buttons" };

        const BuiltinInfo* builtinNamed(std::string_view name)
        {
            for (const BuiltinInfo& b : kBuiltins)
                if (b.name == name)
                    return &b;
            return nullptr;
        }

        const WordDoc* wordDoc(std::string_view word)
        {
            for (const WordDoc& w : kWordDocs)
                if (w.word == word)
                    return &w;
            return nullptr;
        }

        bool isSpaceChar(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }
        bool isDigit(char c) { return c >= '0' && c <= '9'; }
        bool isHexDigit(char c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
        bool isIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

        std::string trimmed(std::string_view s)
        {
            std::size_t a = 0, b = s.size();
            while (a < b && isSpaceChar(s[a]))
                ++a;
            while (b > a && isSpaceChar(s[b - 1]))
                --b;
            return std::string(s.substr(a, b - a));
        }

        std::string lowered(std::string_view s)
        {
            std::string out(s);
            for (char& c : out)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        Pos startOf(const Token& t) { return { t.line, t.col }; }
        Pos endOf(const Token& t) { return { t.line, t.col + t.len }; }

        // A type without its qualifiers, pointer and array: "Svf" of "const Svf*", "float" of "float[16]".
        std::string baseType(std::string_view type)
        {
            std::string s(type);
            if (s.rfind("const ", 0) == 0)
                s = s.substr(6);
            if (const std::size_t b = s.find('['); b != std::string::npos)
                s = s.substr(0, b);
            while (!s.empty() && (s.back() == '*' || s.back() == ' '))
                s.pop_back();
            return s;
        }

        // The element of an array or a pointer: "float" of "float[16]" and of "float*"; "" for anything else.
        std::string elementOf(std::string_view type)
        {
            std::string s(type);
            if (const std::size_t b = s.find('['); b != std::string::npos)
                return s.substr(0, b);
            if (!s.empty() && s.back() == '*')
            {
                s.pop_back();
                if (s.rfind("const ", 0) == 0)
                    s = s.substr(6);
                return s;
            }
            return {};
        }

        int lanesOf(std::string_view type)
        {
            const std::string b = baseType(type);
            if (b == "float2")
                return 2;
            if (b == "float4")
                return 4;
            if (b == "float8")
                return 8;
            return 0;
        }

        // Tokens joined as code reads: a space between words, none inside brackets or before , ; and after ( [.
        std::string joinTokens(const std::vector<Token>& t, int from, int to)
        {
            std::string out;
            for (int i = from; i < to && i < static_cast<int>(t.size()); ++i)
            {
                const std::string_view x = t[static_cast<std::size_t>(i)].text;
                if (!out.empty())
                {
                    const std::string_view p = t[static_cast<std::size_t>(i - 1)].text;
                    const bool tight = x == "," || x == ";" || x == ")" || x == "]" || x == "." || x == "->" || x == "::"
                                    || p == "(" || p == "[" || p == "." || p == "->" || p == "::" || p == "!"
                                    || (x == "(" && (t[static_cast<std::size_t>(i - 1)].kind == Tok::identifier || p == "sizeof"))
                                    || (x == "[" && p != "]" && t[static_cast<std::size_t>(i - 1)].kind == Tok::identifier)
                                    || (x == "[" && p == "[") || (x == "]" && p == "]") || (x == "*" && t[static_cast<std::size_t>(i - 1)].kind == Tok::type)
                                    || (x == "*" && t[static_cast<std::size_t>(i - 1)].kind == Tok::identifier
                                        && i + 1 < to && t[static_cast<std::size_t>(i + 1)].kind != Tok::number);
                    if (!tight)
                        out += ' ';
                }
                out += x;
            }
            return out;
        }

        // ---- one file, indexed --------------------------------------------------------------------------------------
        struct Body
        {
            std::string module;                  // "" for a function
            int open = 0, close = 0;             // code token indices of its braces
        };

        struct ModuleSpan
        {
            std::string name;
            int open = 0, close = 0;             // its braces
        };

        struct FileData
        {
            std::string name, text;
            bool program = false;
            std::vector<std::string_view> lines;
            std::vector<Token> all;              // every token
            std::vector<Token> code;             // without comments and directives
            std::vector<int> match;              // a bracket's partner (code indices), -1 for none
            std::vector<std::pair<std::string, int>> includes;   // #include's file names, with their lines
            std::vector<Symbol> decls;
            std::vector<Body> bodies;
            std::vector<ModuleSpan> modules;
            std::string header;                  // the comment lines the file starts with
        };

        // The comment lines over line `ln` (contiguous `//` lines, no blank between), as plain lines.
        std::string commentsAbove(const FileData& f, int ln)
        {
            std::vector<std::string> got;
            for (int i = ln - 1; i >= 0; --i)
            {
                const std::string t = trimmed(f.lines[static_cast<std::size_t>(i)]);
                if (t.rfind("//", 0) != 0)
                    break;
                std::string body = t.substr(2);
                if (!body.empty() && body.front() == ' ')
                    body.erase(body.begin());
                got.push_back(body);
            }
            std::string out;
            for (auto it = got.rbegin(); it != got.rend(); ++it)
                out += (out.empty() ? "" : "\n") + *it;
            return out;
        }

        // The comment after code on line `ln`, as text.
        std::string commentAfter(const FileData& f, int ln)
        {
            for (const Token& t : f.all)
                if (t.line == ln && t.kind == Tok::comment && t.text.rfind("//", 0) == 0)
                {
                    bool code = false;
                    for (const Token& c : f.code)
                        code = code || (c.line == ln && c.col < t.col);
                    if (!code)
                        return {};
                    return trimmed(t.text.substr(2));
                }
            return {};
        }

        class Indexer
        {
        public:
            Indexer(FileData& f, int index) : f_(f), t_(f.code), n_(static_cast<int>(f.code.size())), file_(index) {}

            void run()
            {
                int i = 0;
                while (i < n_)
                {
                    if (is(i, "module") && ident(i + 1))
                    {
                        i = moduleAt(i);
                        continue;
                    }
                    const int e = function(i, {});
                    i = e > i ? e : i + 1;
                }
            }

        private:
            bool is(int i, std::string_view s) const
            {
                return i >= 0 && i < n_ && t_[static_cast<std::size_t>(i)].kind != Tok::identifier
                    && t_[static_cast<std::size_t>(i)].text == s;
            }
            bool ident(int i) const { return i >= 0 && i < n_ && t_[static_cast<std::size_t>(i)].kind == Tok::identifier; }
            const Token& tok(int i) const { return t_[static_cast<std::size_t>(i)]; }
            int partner(int i) const { return i >= 0 && i < n_ ? f_.match[static_cast<std::size_t>(i)] : -1; }

            // A type at i: [const] (type word | poly type word | identifier); the index after it, -1 for none.
            int type(int i, std::string& out) const
            {
                int j = i;
                std::string s;
                if (is(j, "const"))
                {
                    s = "const ";
                    ++j;
                }
                if (j < n_ && tok(j).kind == Tok::type)
                {
                    if (tok(j).text == "poly")
                    {
                        s += "poly ";
                        ++j;
                        if (j < n_ && tok(j).kind == Tok::type)
                            s += tok(j++).text;
                    }
                    else
                        s += tok(j++).text;
                }
                else if (ident(j))
                    s += tok(j++).text;
                else
                    return -1;
                out = s;
                return j;
            }

            // '*' and its qualifiers; the type gains the '*'.
            int pointer(int i, std::string& type) const
            {
                if (!is(i, "*"))
                    return i;
                type += "*";
                ++i;
                while (is(i, "const") || is(i, "restrict"))
                    ++i;
                return i;
            }

            int add(Symbol s)
            {
                s.file = file_;
                f_.decls.push_back(std::move(s));
                return static_cast<int>(f_.decls.size()) - 1;
            }

            std::string docFor(int line) const
            {
                std::string d = commentsAbove(f_, line);
                if (d.empty())
                    d = commentAfter(f_, line);
                return d;
            }

            int moduleAt(int i)
            {
                Symbol m;
                m.kind = Kind::module;
                m.name = std::string(tok(i + 1).text);
                m.type = m.name;
                m.at = startOf(tok(i + 1));
                m.signature = "module " + m.name;
                m.doc = docFor(tok(i).line);
                int j = i + 2;
                while (j < n_ && !is(j, "{") && !is(j, ";"))
                    ++j;
                add(m);
                if (!is(j, "{"))
                    return j + 1;
                const int close = partner(j) >= 0 ? partner(j) : n_;
                f_.modules.push_back({ m.name, j, close });
                int k = j + 1;
                while (k < close)
                {
                    if (is(k, "param"))
                    {
                        k = param(k, m.name, close);
                        continue;
                    }
                    int e = function(k, m.name);
                    if (e > k)
                    {
                        k = e;
                        continue;
                    }
                    e = fields(k, m.name, close);
                    if (e > k)
                    {
                        k = e;
                        continue;
                    }
                    k = skipStatement(k, close);
                }
                int after = close + 1;
                if (is(after, ";"))
                    ++after;
                return after;
            }

            // The next statement's start: after its ';' at this depth, or at the limit.
            int skipStatement(int k, int limit) const
            {
                while (k < limit)
                {
                    if (is(k, ";"))
                        return k + 1;
                    if (is(k, "(") || is(k, "[") || is(k, "{"))
                    {
                        const int p = partner(k);
                        if (p < 0)
                            return limit;
                        k = p + 1;
                        if (is(k - 1, "}"))
                            return k;
                        continue;
                    }
                    ++k;
                }
                return limit;
            }

            int param(int i, const std::string& module, int limit)
            {
                std::string ty;
                int j = type(i + 1, ty);
                if (j < 0 || !ident(j))
                    return skipStatement(i, limit);
                Symbol s;
                s.kind = Kind::param;
                s.name = std::string(tok(j).text);
                s.type = ty;
                s.at = startOf(tok(j));
                s.container = module;
                int e = j + 1;
                while (e < limit && !is(e, ";"))
                    e = is(e, "(") || is(e, "[") ? std::max(e + 1, partner(e) + 1) : e + 1;
                s.signature = joinTokens(t_, i, e);
                s.doc = docFor(tok(i).line);
                add(s);
                return e + 1;
            }

            // A field declaration at i: [const] type [*] name ([N])? (, [*] name ([N])?)* ;
            int fields(int i, const std::string& module, int limit)
            {
                std::string base;
                int j = type(i, base);
                if (j < 0)
                    return i;
                bool any = false;
                while (j < limit)
                {
                    std::string ty = base;
                    j = pointer(j, ty);
                    if (!ident(j))
                        break;
                    const Token& name = tok(j++);
                    if (is(j, "[") && partner(j) > j)
                    {
                        ty += joinTokens(t_, j, partner(j) + 1);
                        j = partner(j) + 1;
                    }
                    if (!any && !(is(j, ";") || is(j, ",")))
                        return i;
                    any = true;
                    Symbol s;
                    s.kind = Kind::field;
                    s.name = std::string(name.text);
                    s.type = ty;
                    s.at = startOf(name);
                    s.container = module;
                    s.signature = ty + " " + s.name;
                    s.doc = docFor(name.line);
                    add(s);
                    if (is(j, ","))
                    {
                        ++j;
                        continue;
                    }
                    break;
                }
                if (!any)
                    return i;
                return is(j, ";") ? j + 1 : j;
            }

            // A function (or a method of `module`) at i: [const] type [*] name ( params ) attributes { body } | ;
            int function(int i, const std::string& module)
            {
                std::string ty;
                int j = type(i, ty);
                if (j < 0)
                    return i;
                j = pointer(j, ty);
                if (!ident(j) || !is(j + 1, "("))
                    return i;
                const int nameAt = j, open = j + 1, close = partner(open);
                if (close < 0)
                    return i;
                int e = close + 1;
                while (is(e, "[") && partner(e) > e)
                    e = partner(e) + 1;
                Symbol s;
                s.kind = module.empty() ? Kind::function : Kind::method;
                s.name = std::string(tok(nameAt).text);
                s.type = ty;
                s.at = startOf(tok(nameAt));
                s.container = module;
                s.signature = joinTokens(t_, i, e);
                s.doc = docFor(tok(i).line);
                // The parameters, split at the commas between the parentheses.
                std::vector<std::pair<int, int>> parts;
                int from = open + 1;
                for (int k = open + 1; k <= close; ++k)
                {
                    if (k == close || is(k, ","))
                    {
                        if (k > from)
                            parts.push_back({ from, k });
                        from = k + 1;
                        continue;
                    }
                    if ((is(k, "(") || is(k, "[")) && partner(k) > k)
                        k = partner(k);
                }
                for (const auto& [a, b] : parts)
                {
                    const std::string p = joinTokens(t_, a, b);
                    if (p != "void")
                        s.params.push_back(p);
                }
                s.body = is(e, "{") && partner(e) > e;
                add(s);
                if (!s.body)
                    return is(e, ";") ? e + 1 : e;
                const int bodyOpen = e, bodyClose = partner(e);
                f_.bodies.push_back({ module, bodyOpen, bodyClose });
                const Range scope{ startOf(tok(bodyOpen)), startOf(tok(bodyClose)) };
                for (const auto& [a, b] : parts)
                {
                    // [const] type [*] name ([N])?
                    std::string pt;
                    int k = type(a, pt);
                    if (k < 0)
                        continue;
                    k = pointer(k, pt);
                    if (!ident(k) || k >= b)
                        continue;
                    Symbol arg;
                    arg.kind = Kind::argument;
                    arg.name = std::string(tok(k).text);
                    arg.type = pt;
                    if (is(k + 1, "["))
                        arg.type += "*";
                    arg.at = startOf(tok(k));
                    arg.scope = scope;
                    arg.container = s.name;
                    arg.signature = joinTokens(t_, a, b);
                    add(arg);
                }
                bodyScan(bodyOpen, bodyClose, s.name);
                return bodyClose + 1;
            }

            // Where a statement that starts at i ends: its block's close, or its ';'.
            int statementEnd(int i, int limit) const
            {
                if (is(i, "{") && partner(i) > i)
                    return partner(i);
                while (i < limit)
                {
                    if (is(i, ";"))
                        return i;
                    if ((is(i, "(") || is(i, "[") || is(i, "{")) && partner(i) > i)
                    {
                        i = partner(i) + 1;
                        continue;
                    }
                    ++i;
                }
                return limit;
            }

            // A declaration of locals at i (the scope they are seen in ends at `scopeEnd`); the index of its ';' (or
            // where it stopped), -1 when i starts none.
            int locals(int i, int limit, Pos scopeEnd, const std::string& function)
            {
                std::string base;
                int j = type(i, base);
                if (j < 0)
                    return -1;
                bool any = false;
                while (j < limit)
                {
                    std::string ty = base;
                    j = pointer(j, ty);
                    if (!ident(j))
                        break;
                    const Token& name = tok(j++);
                    if (is(j, "[") && partner(j) > j)
                    {
                        ty += joinTokens(t_, j, partner(j) + 1);
                        j = partner(j) + 1;
                    }
                    if (!any && !(is(j, "=") || is(j, ";") || is(j, ",")))
                        return -1;
                    any = true;
                    Symbol s;
                    s.kind = Kind::local;
                    s.name = std::string(name.text);
                    s.type = ty;
                    s.at = startOf(name);
                    s.scope = { s.at, scopeEnd };
                    s.container = function;
                    s.signature = ty + " " + s.name;
                    s.doc = commentAfter(f_, name.line);
                    add(s);
                    // Past the initialiser, to the next declarator or the end.
                    while (j < limit && !is(j, ",") && !is(j, ";"))
                    {
                        if ((is(j, "(") || is(j, "[") || is(j, "{")) && partner(j) > j)
                            j = partner(j) + 1;
                        else
                            ++j;
                    }
                    if (is(j, ","))
                    {
                        ++j;
                        continue;
                    }
                    break;
                }
                return any ? j : -1;
            }

            void bodyScan(int open, int close, const std::string& function)
            {
                std::vector<int> blocks{ open };
                bool start = true;
                int k = open + 1;
                while (k < close)
                {
                    if (is(k, "{"))
                    {
                        blocks.push_back(k);
                        start = true;
                        ++k;
                        continue;
                    }
                    if (is(k, "}"))
                    {
                        if (blocks.size() > 1)
                            blocks.pop_back();
                        start = true;
                        ++k;
                        continue;
                    }
                    if (is(k, ";"))
                    {
                        start = true;
                        ++k;
                        continue;
                    }
                    if (start)
                    {
                        const int f = is(k, "parallel") && is(k + 1, "for") ? k + 1 : k;
                        if (is(f, "for") && is(f + 1, "(") && partner(f + 1) > f + 1)
                        {
                            const int rp = partner(f + 1);
                            const int end = statementEnd(rp + 1, close);
                            locals(f + 2, rp, end < n_ ? startOf(tok(end)) : endOf(tok(n_ - 1)), function);
                            k = rp + 1;
                            start = true;
                            continue;
                        }
                        if ((is(k, "if") || is(k, "while")) && is(k + 1, "(") && partner(k + 1) > k + 1)
                        {
                            k = partner(k + 1) + 1;
                            start = true;
                            continue;
                        }
                        if (is(k, "else") || is(k, "parallel"))
                        {
                            ++k;
                            continue;
                        }
                        const int blockClose = partner(blocks.back()) > 0 ? partner(blocks.back()) : close;
                        const int e = locals(k, close, startOf(tok(blockClose)), function);
                        if (e > k)
                        {
                            k = e;
                            start = false;
                            continue;
                        }
                        start = false;
                    }
                    ++k;
                }
            }

            FileData& f_;
            const std::vector<Token>& t_;
            int n_;
            int file_;
        };

        void indexFile(FileData& f, int index)
        {
            f.lines = splitLines(f.text);
            f.all = lex(f.text);
            f.code.clear();
            f.includes.clear();
            f.decls.clear();
            f.bodies.clear();
            f.modules.clear();
            for (const Token& t : f.all)
            {
                if (t.kind == Tok::directive)
                {
                    std::string name;
                    int a = 0, b = 0;
                    if (includeOn(f.lines[static_cast<std::size_t>(t.line)], name, a, b))
                        f.includes.push_back({ name, t.line });
                    continue;
                }
                if (t.kind != Tok::comment)
                    f.code.push_back(t);
            }
            // The brackets' partners.
            f.match.assign(f.code.size(), -1);
            std::vector<int> stack;
            for (std::size_t i = 0; i < f.code.size(); ++i)
            {
                const std::string_view x = f.code[i].text;
                if (f.code[i].kind != Tok::punct)
                    continue;
                if (x == "(" || x == "[" || x == "{")
                    stack.push_back(static_cast<int>(i));
                else if (x == ")" || x == "]" || x == "}")
                {
                    const char want = x == ")" ? '(' : x == "]" ? '[' : '{';
                    // An unbalanced closer: pop to its opener when there is one, else leave it unmatched.
                    auto it = std::find_if(stack.rbegin(), stack.rend(),
                                           [&](int k) { return f.code[static_cast<std::size_t>(k)].text[0] == want; });
                    if (it == stack.rend())
                        continue;
                    const int open = *it;
                    stack.erase(std::next(it).base(), stack.end());
                    f.match[static_cast<std::size_t>(open)] = static_cast<int>(i);
                    f.match[i] = open;
                }
            }
            // The header: the comment lines the file starts with.
            f.header = commentsAbove(f, [&] {
                int ln = 0;
                while (ln < static_cast<int>(f.lines.size()) && trimmed(f.lines[static_cast<std::size_t>(ln)]).rfind("//", 0) == 0)
                    ++ln;
                return ln;
            }());
            Indexer(f, index).run();
            // A module without comments over it, the first module in its file: the file's header is its documentation
            // (a library file may define functions before it: svf.fm's svfG, the filters' coefficients).
            for (Symbol& s : f.decls)
                if (s.kind == Kind::module && s.doc.empty())
                {
                    bool first = true;
                    for (const Symbol& o : f.decls)
                        if (o.kind == Kind::module && o.at.line < s.at.line)
                            first = false;
                    if (first)
                        s.doc = f.header;
                }
        }

        // The lines of a module's documentation that name `member` (`filter.lp`, `host->bpm`, `y = filter.tick(x);`)
        // and the lines that continue them (indented deeper), as one text.
        std::string memberDoc(const std::string& moduleDoc, std::string_view member)
        {
            const std::vector<std::string_view> lines = splitLines(moduleDoc);
            for (std::size_t i = 0; i < lines.size(); ++i)
            {
                const std::string_view l = lines[i];
                std::size_t at = 0;
                bool found = false;
                while ((at = l.find(member, at)) != std::string_view::npos)
                {
                    const bool left = at >= 1 && (l[at - 1] == '.' || (at >= 2 && l[at - 1] == '>' && l[at - 2] == '-'));
                    const bool right = at + member.size() >= l.size() || !isIdentChar(l[at + member.size()]);
                    if (left && right)
                    {
                        found = true;
                        break;
                    }
                    at += member.size();
                }
                if (!found)
                    continue;
                const auto indent = [](std::string_view s) {
                    std::size_t k = 0;
                    while (k < s.size() && s[k] == ' ')
                        ++k;
                    return k;
                };
                // The usage (`y = filter.tick(x);`), then what it says (after a gap of three spaces or more, and the
                // lines indented deeper that go on with it), as a paragraph.
                std::string first = trimmed(l);
                std::string usage = first, what;
                if (const std::size_t gap = first.find("   "); gap != std::string::npos)
                {
                    usage = trimmed(first.substr(0, gap));
                    what = trimmed(first.substr(gap));
                }
                const std::size_t base = indent(l);
                for (std::size_t k = i + 1; k < lines.size(); ++k)
                {
                    if (trimmed(lines[k]).empty() || indent(lines[k]) <= base)
                        break;
                    what += (what.empty() ? "" : " ") + trimmed(lines[k]);
                }
                return what.empty() ? usage : usage + "\n" + what;
            }
            return {};
        }

        // How well `label` matches the typed `prefix`: 0 starts with it (its case), 1 starts with it, 2 holds its
        // letters in order from the first; -1 none.
        int matchScore(std::string_view label, std::string_view prefix)
        {
            if (prefix.empty())
                return 0;
            if (label.rfind(prefix, 0) == 0)
                return 0;
            const std::string l = lowered(label), p = lowered(prefix);
            if (l.rfind(p, 0) == 0)
                return 1;
            if (l.empty() || l[0] != p[0])
                return -1;
            std::size_t k = 0;
            for (const char c : l)
                if (k < p.size() && c == p[k])
                    ++k;
            return k == p.size() ? 2 : -1;
        }
    }

    // ---- the lexer --------------------------------------------------------------------------------------------------
    bool isKeyword(std::string_view word) noexcept
    {
        return std::find(kKeywords.begin(), kKeywords.end(), word) != kKeywords.end();
    }
    bool isTypeWord(std::string_view word) noexcept
    {
        return std::find(kTypeWords.begin(), kTypeWords.end(), word) != kTypeWords.end();
    }
    bool isBuiltin(std::string_view word) noexcept { return builtinNamed(word) != nullptr; }
    bool isIdentChar(char c) noexcept { return isIdentStart(c) || isDigit(c); }

    std::vector<std::string_view> splitLines(std::string_view text)
    {
        std::vector<std::string_view> out;
        std::size_t from = 0;
        while (true)
        {
            const std::size_t nl = text.find('\n', from);
            if (nl == std::string_view::npos)
            {
                out.push_back(text.substr(from));
                return out;
            }
            out.push_back(text.substr(from, nl - from));
            from = nl + 1;
        }
    }

    void lexLine(std::string_view s, int ln, bool& inComment, std::vector<Token>& out)
    {
        const int n = static_cast<int>(s.size());
        const auto push = [&](Tok k, int from, int to, bool open) {
            Token t;
            t.kind = k;
            t.line = ln;
            t.col = from;
            t.len = to - from;
            t.text = s.substr(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from));
            t.open = open;
            out.push_back(t);
        };
        int i = 0;
        if (inComment)
        {
            const std::size_t end = s.find("*/");
            if (end == std::string_view::npos)
            {
                if (n > 0)
                    push(Tok::comment, 0, n, true);
                return;
            }
            push(Tok::comment, 0, static_cast<int>(end) + 2, false);
            i = static_cast<int>(end) + 2;
            inComment = false;
        }
        else
        {
            int first = 0;
            while (first < n && isSpaceChar(s[static_cast<std::size_t>(first)]))
                ++first;
            if (first < n && s[static_cast<std::size_t>(first)] == '#')
            {
                int last = n;
                while (last > first && isSpaceChar(s[static_cast<std::size_t>(last - 1)]))
                    --last;
                push(Tok::directive, first, last, false);
                return;
            }
        }
        const auto at = [&](int k) { return k < n ? s[static_cast<std::size_t>(k)] : '\0'; };
        while (i < n)
        {
            const char c = at(i);
            if (isSpaceChar(c))
            {
                ++i;
                continue;
            }
            if (c == '/' && at(i + 1) == '/')
            {
                int last = n;
                while (last > i && isSpaceChar(at(last - 1)))
                    --last;
                push(Tok::comment, i, last, false);
                return;
            }
            if (c == '/' && at(i + 1) == '*')
            {
                const std::size_t end = s.find("*/", static_cast<std::size_t>(i) + 2);
                if (end == std::string_view::npos)
                {
                    int last = n;
                    while (last > i && isSpaceChar(at(last - 1)))
                        --last;
                    push(Tok::comment, i, last, true);
                    inComment = true;
                    return;
                }
                push(Tok::comment, i, static_cast<int>(end) + 2, false);
                i = static_cast<int>(end) + 2;
                continue;
            }
            if (isIdentStart(c))
            {
                int j = i;
                while (j < n && isIdentChar(at(j)))
                    ++j;
                const std::string_view w = s.substr(static_cast<std::size_t>(i), static_cast<std::size_t>(j - i));
                push(isKeyword(w) ? Tok::keyword : isTypeWord(w) ? Tok::type : Tok::identifier, i, j, false);
                i = j;
                continue;
            }
            if (isDigit(c) || (c == '.' && isDigit(at(i + 1))))
            {
                int j = i;
                if (c == '0' && (at(i + 1) == 'x' || at(i + 1) == 'X'))
                {
                    j += 2;
                    while (j < n && (isHexDigit(at(j)) || at(j) == '.'))
                        ++j;
                    if (at(j) == 'p' || at(j) == 'P')
                    {
                        ++j;
                        if (at(j) == '+' || at(j) == '-')
                            ++j;
                        while (isDigit(at(j)))
                            ++j;
                    }
                }
                else
                {
                    while (isDigit(at(j)))
                        ++j;
                    if (at(j) == '.')
                    {
                        ++j;
                        while (isDigit(at(j)))
                            ++j;
                    }
                    if (at(j) == 'e' || at(j) == 'E')
                    {
                        int k = j + 1;
                        if (at(k) == '+' || at(k) == '-')
                            ++k;
                        if (isDigit(at(k)))
                        {
                            j = k;
                            while (isDigit(at(j)))
                                ++j;
                        }
                    }
                }
                while (j < n && isIdentChar(at(j)))     // a suffix (f), or what follows a number directly
                    ++j;
                push(Tok::number, i, j, false);
                i = j;
                continue;
            }
            static constexpr std::string_view kTwo[] = { "++", "--", "+=", "-=", "*=", "/=", "%=", "->", "<=", ">=",
                                                         "==", "!=", "&&", "||", "::" };
            bool two = false;
            for (const std::string_view op : kTwo)
                if (at(i) == op[0] && at(i + 1) == op[1])
                {
                    push(Tok::op, i, i + 2, false);
                    i += 2;
                    two = true;
                    break;
                }
            if (two)
                continue;
            if (std::strchr("+-*/%=<>!&?:", c) != nullptr)
                push(Tok::op, i, i + 1, false);
            else if (std::strchr("()[]{},;.", c) != nullptr)
                push(Tok::punct, i, i + 1, false);
            else
                push(Tok::unknown, i, i + 1, false);
            ++i;
        }
    }

    std::vector<Token> lex(std::string_view text)
    {
        std::vector<Token> out;
        bool inComment = false;
        int ln = 0;
        for (const std::string_view l : splitLines(text))
            lexLine(l, ln++, inComment, out);
        return out;
    }

    bool includeOn(std::string_view line, std::string& name, int& from, int& to)
    {
        std::size_t i = 0;
        while (i < line.size() && isSpaceChar(line[i]))
            ++i;
        if (i >= line.size() || line[i] != '#')
            return false;
        ++i;
        while (i < line.size() && isSpaceChar(line[i]))
            ++i;
        if (line.substr(i, 7) != "include")
            return false;
        i += 7;
        while (i < line.size() && isSpaceChar(line[i]))
            ++i;
        if (i >= line.size() || line[i] != '"')
            return false;
        const std::size_t close = line.find('"', i + 1);
        const std::size_t end = close == std::string_view::npos ? line.size() : close;
        name = std::string(line.substr(i + 1, end - i - 1));
        from = static_cast<int>(i) + 1;
        to = static_cast<int>(end);
        return true;
    }

    const char* kindName(Kind k) noexcept
    {
        switch (k)
        {
            case Kind::keyword:   return "keyword";
            case Kind::type:      return "type";
            case Kind::module:    return "module";
            case Kind::function:  return "function";
            case Kind::method:    return "method";
            case Kind::field:     return "field";
            case Kind::param:     return "param";
            case Kind::argument:  return "argument";
            case Kind::local:     return "local";
            case Kind::builtin:   return "builtin";
            case Kind::snippet:   return "snippet";
            case Kind::file:      return "file";
            case Kind::attribute: return "attribute";
            case Kind::lane:      return "lane";
            case Kind::word:      return "word";
        }
        return "";
    }

    // ---- the service ------------------------------------------------------------------------------------------------
    struct Service::Impl
    {
        Options options;
        std::vector<std::unique_ptr<FileData>> files;     // the program's, then host.fm when no file has its name
        std::vector<std::unique_ptr<FileData>> library;
        std::vector<File> view;                           // files(), as the API gives them
        std::vector<Symbol> symbols;
        std::vector<std::string> typeNames;

        void rebuild()
        {
            view.clear();
            symbols.clear();
            std::set<std::string> types;
            for (const auto& f : files)
            {
                view.push_back({ f->name, f->text, f->program });
                for (const Symbol& s : f->decls)
                {
                    symbols.push_back(s);
                    if (s.kind == Kind::module)
                        types.insert(s.name);
                }
            }
            typeNames.assign(types.begin(), types.end());
        }

        int indexOf(std::string_view name) const
        {
            for (std::size_t i = 0; i < files.size(); ++i)
                if (files[i]->name == name)
                    return static_cast<int>(i);
            return -1;
        }

        // The files a file sees: what the program reads (its #includes, in turn theirs), and what the file reads.
        std::vector<int> visible(int file) const
        {
            std::vector<int> out;
            std::vector<int> todo{ 0 };
            if (file > 0)
                todo.push_back(file);
            while (!todo.empty())
            {
                const int f = todo.back();
                todo.pop_back();
                if (f < 0 || f >= static_cast<int>(files.size()) || std::find(out.begin(), out.end(), f) != out.end())
                    continue;
                out.push_back(f);
                for (const auto& inc : files[static_cast<std::size_t>(f)]->includes)
                    todo.push_back(indexOf(inc.first));
            }
            return out;
        }

        const FileData* fileAt(int i) const
        {
            return i >= 0 && i < static_cast<int>(files.size()) ? files[static_cast<std::size_t>(i)].get() : nullptr;
        }

        // ---- names --------------------------------------------------------------------------------------------------
        const Symbol* moduleNamed(std::string_view name) const
        {
            for (const auto& f : files)
                for (const Symbol& s : f->decls)
                    if (s.kind == Kind::module && s.name == name)
                        return &s;
            for (const auto& f : library)
                for (const Symbol& s : f->decls)
                    if (s.kind == Kind::module && s.name == name)
                        return &s;
            return nullptr;
        }

        // A module's members, its own file's (program files first, then the library's).
        std::vector<const Symbol*> membersOf(std::string_view module) const
        {
            std::vector<const Symbol*> out;
            const auto scan = [&](const std::vector<std::unique_ptr<FileData>>& set) {
                for (const auto& f : set)
                    for (const Symbol& s : f->decls)
                        if (s.container == module && (s.kind == Kind::field || s.kind == Kind::param || s.kind == Kind::method))
                            out.push_back(&s);
            };
            scan(files);
            if (out.empty())
                scan(library);
            return out;
        }

        const Symbol* memberNamed(std::string_view module, std::string_view name) const
        {
            for (const Symbol* s : membersOf(module))
                if (s->name == name)
                    return s;
            return nullptr;
        }

        // The module whose braces hold a place of a file ("" for none). A module whose `{` has no partner (its close
        // is the code's end) holds the rest of the file.
        std::string moduleAt(const FileData& f, Pos p) const
        {
            for (const ModuleSpan& m : f.modules)
            {
                const Pos end = m.close < static_cast<int>(f.code.size())
                                    ? startOf(f.code[static_cast<std::size_t>(m.close)])
                                    : Pos{ std::numeric_limits<int>::max(), 0 };
                if (startOf(f.code[static_cast<std::size_t>(m.open)]) < p && p <= end)
                    return m.name;
            }
            return {};
        }

        // The function or method whose body holds a place, as its symbol (null for none).
        const Symbol* functionAt(const FileData& f, Pos p) const
        {
            for (const Body& b : f.bodies)
                if (startOf(f.code[static_cast<std::size_t>(b.open)]) < p && p <= startOf(f.code[static_cast<std::size_t>(b.close)]))
                {
                    // The function whose name comes last before the body.
                    const Symbol* best = nullptr;
                    for (const Symbol& s : f.decls)
                        if ((s.kind == Kind::function || s.kind == Kind::method) && s.body
                            && s.at < startOf(f.code[static_cast<std::size_t>(b.open)]) && (best == nullptr || best->at < s.at))
                            best = &s;
                    return best;
                }
            return nullptr;
        }

        // A local or argument named `name` seen at p (the innermost: declared last before p).
        const Symbol* localNamed(const FileData& f, std::string_view name, Pos p) const
        {
            const Symbol* best = nullptr;
            for (const Symbol& s : f.decls)
                if ((s.kind == Kind::local || s.kind == Kind::argument) && s.name == name && s.scope.from <= p
                    && p <= s.scope.to && (best == nullptr || best->at < s.at))
                    best = &s;
            return best;
        }

        // A function or module named `name` in the files a file sees (a definition before a prototype).
        const Symbol* globalNamed(int file, std::string_view name) const
        {
            const Symbol* best = nullptr;
            for (const int fi : visible(file))
                for (const Symbol& s : files[static_cast<std::size_t>(fi)]->decls)
                    if ((s.kind == Kind::function || s.kind == Kind::module) && s.name == name && (best == nullptr || (!best->body && s.body)))
                        best = &s;
            if (best == nullptr)
                for (const auto& f : files)
                    for (const Symbol& s : f->decls)
                        if ((s.kind == Kind::function || s.kind == Kind::module) && s.name == name && (best == nullptr || (!best->body && s.body)))
                            best = &s;
            return best;
        }

        // ---- tokens -------------------------------------------------------------------------------------------------
        static bool isText(const FileData& f, int i, std::string_view s)
        {
            return i >= 0 && i < static_cast<int>(f.code.size()) && f.code[static_cast<std::size_t>(i)].kind != Tok::identifier
                && f.code[static_cast<std::size_t>(i)].text == s;
        }
        static bool isIdent(const FileData& f, int i)
        {
            return i >= 0 && i < static_cast<int>(f.code.size()) && f.code[static_cast<std::size_t>(i)].kind == Tok::identifier;
        }

        // The code token touching p (an identifier first), -1 for none.
        static int tokenAt(const FileData& f, Pos p)
        {
            for (std::size_t i = 0; i < f.code.size(); ++i)
            {
                const Token& t = f.code[i];
                if (t.kind == Tok::identifier && t.line == p.line && t.col <= p.col && p.col <= t.col + t.len)
                    return static_cast<int>(i);
            }
            for (std::size_t i = 0; i < f.code.size(); ++i)
            {
                const Token& t = f.code[i];
                if (t.line == p.line && t.col <= p.col && p.col < t.col + t.len)
                    return static_cast<int>(i);
            }
            return -1;
        }

        // The index of the last code token that ends at or before p, -1 for none.
        static int lastBefore(const FileData& f, Pos p)
        {
            int last = -1;
            for (std::size_t i = 0; i < f.code.size(); ++i)
            {
                if (p < endOf(f.code[i]))
                    break;
                last = static_cast<int>(i);
            }
            return last;
        }

        // ---- resolving ----------------------------------------------------------------------------------------------
        // The type of the expression whose last token is code index e (a name, a member, an element, a call), "" when
        // it is not known.
        std::string typeOf(int file, int e, int depth = 0) const
        {
            const FileData* f = fileAt(file);
            if (f == nullptr || e < 0 || depth > 16)
                return {};
            const Token& t = f->code[static_cast<std::size_t>(e)];
            if (isText(*f, e, "]"))
            {
                const int o = f->match[static_cast<std::size_t>(e)];
                return o > 0 ? elementOf(typeOf(file, o - 1, depth + 1)) : std::string();
            }
            if (isText(*f, e, ")"))
            {
                const int o = f->match[static_cast<std::size_t>(e)];
                if (o > 0 && isIdent(*f, o - 1))
                {
                    if (const Symbol* s = resolve(file, o - 1, depth + 1))
                        return s->kind == Kind::function || s->kind == Kind::method ? s->type : std::string();
                    if (const BuiltinInfo* b = builtinNamed(f->code[static_cast<std::size_t>(o - 1)].text))
                        return std::string(b->result);
                }
                return {};
            }
            if (isText(*f, e, "this"))
                return moduleAt(*f, startOf(t)) + "*";
            if (t.kind == Tok::identifier)
            {
                const Symbol* s = resolve(file, e, depth + 1);
                if (s == nullptr || s->kind == Kind::function || s->kind == Kind::method)
                    return {};
                return s->type;
            }
            return {};
        }

        // What the identifier at code index i names, null when nothing (a lane of a vector, a builtin, a typo).
        const Symbol* resolve(int file, int i, int depth = 0) const
        {
            const FileData* f = fileAt(file);
            if (f == nullptr || !isIdent(*f, i) || depth > 16)
                return nullptr;
            const Token& t = f->code[static_cast<std::size_t>(i)];
            if (isText(*f, i - 1, ".") || isText(*f, i - 1, "->"))
            {
                const std::string base = baseType(typeOf(file, i - 2, depth + 1));
                return base.empty() ? nullptr : memberNamed(base, t.text);
            }
            const Pos p = startOf(t);
            if (const Symbol* s = localNamed(*f, t.text, p))
                return s;
            if (const std::string m = moduleAt(*f, p); !m.empty())
                if (const Symbol* s = memberNamed(m, t.text))
                    return s;
            return globalNamed(file, t.text);
        }
    };

    Service::Service() : impl_(new Impl) {}
    Service::~Service() { delete impl_; }

    void Service::setOptions(const Options& o) { impl_->options = o; }

    void Service::setFiles(const Sources& files)
    {
        Impl& m = *impl_;
        std::vector<std::unique_ptr<FileData>> next;
        bool changed = m.files.empty();
        for (std::size_t i = 0; i < files.size(); ++i)
        {
            const SourceFile& s = files[i];
            if (i < m.files.size() && m.files[i]->program && m.files[i]->name == s.name && m.files[i]->text == s.text)
            {
                next.push_back(std::move(m.files[i]));
                continue;
            }
            changed = true;
            auto f = std::make_unique<FileData>();
            f->name = s.name;
            f->text = s.text;
            f->program = true;
            next.push_back(std::move(f));
        }
        if (findFile(files, "host.fm") == nullptr)
        {
            const std::size_t k = files.size();
            if (k < m.files.size() && !m.files[k]->program)
                next.push_back(std::move(m.files[k]));
            else
            {
                changed = true;
                auto f = std::make_unique<FileData>();
                f->name = "host.fm";
                f->text = std::string(Patch::hostSource());
                next.push_back(std::move(f));
            }
        }
        changed = changed || next.size() != m.files.size();
        m.files = std::move(next);
        // A file's index is part of its symbols: index every file again when any changed (they are small).
        if (changed)
        {
            for (std::size_t i = 0; i < m.files.size(); ++i)
                indexFile(*m.files[i], static_cast<int>(i));
            m.rebuild();
        }
    }

    void Service::setLibrary(const Sources& library)
    {
        impl_->library.clear();
        for (const SourceFile& s : library)
        {
            auto f = std::make_unique<FileData>();
            f->name = s.name;
            f->text = s.text;
            indexFile(*f, -1);
            impl_->library.push_back(std::move(f));
        }
    }

    const std::vector<Service::File>& Service::files() const noexcept { return impl_->view; }
    int Service::fileIndex(std::string_view name) const noexcept { return impl_->indexOf(name); }
    const std::vector<Symbol>& Service::symbols() const noexcept { return impl_->symbols; }
    const std::vector<std::string>& Service::typeNames() const noexcept { return impl_->typeNames; }

    Range Service::wordAt(int file, Pos at) const
    {
        const FileData* f = impl_->fileAt(file);
        if (f == nullptr || at.line < 0 || at.line >= static_cast<int>(f->lines.size()))
            return { at, at };
        const std::string_view l = f->lines[static_cast<std::size_t>(at.line)];
        int a = std::clamp(at.col, 0, static_cast<int>(l.size())), b = a;
        while (a > 0 && isIdentChar(l[static_cast<std::size_t>(a - 1)]))
            --a;
        while (b < static_cast<int>(l.size()) && isIdentChar(l[static_cast<std::size_t>(b)]))
            ++b;
        return { { at.line, a }, { at.line, b } };
    }

    // ---- completion -------------------------------------------------------------------------------------------------
    CompletionList Service::complete(int file, Pos at) const
    {
        const Impl& m = *impl_;
        CompletionList out;
        const FileData* f = m.fileAt(file);
        if (f == nullptr || at.line < 0 || at.line >= static_cast<int>(f->lines.size()))
            return out;
        const std::string_view line = f->lines[static_cast<std::size_t>(at.line)];
        const int col = std::clamp(at.col, 0, static_cast<int>(line.size()));
        at.col = col;
        // In a comment: nothing.
        for (const Token& t : f->all)
            if (t.line == at.line && t.kind == Tok::comment && t.col < col
                && (col < t.col + t.len || t.open || t.text.rfind("//", 0) == 0))
                return out;
        int a = col;
        while (a > 0 && isIdentChar(line[static_cast<std::size_t>(a - 1)]))
            --a;
        out.prefix = std::string(line.substr(static_cast<std::size_t>(a), static_cast<std::size_t>(col - a)));
        out.replace = { { at.line, a }, at };
        if (!out.prefix.empty() && isDigit(out.prefix[0]))
            return out;                                  // a number

        std::vector<Completion> items;
        const auto offer = [&](std::string label, Kind kind, std::string detail, std::string doc, std::string insert,
                               int group) {
            const int score = matchScore(label, out.prefix);
            if (score < 0)
                return;
            Completion c;
            c.rank = score * 100 + group;
            c.label = std::move(label);
            c.kind = kind;
            c.detail = std::move(detail);
            c.doc = std::move(doc);
            c.insert = insert.empty() ? c.label : std::move(insert);
            items.push_back(std::move(c));
        };
        // The last item offered, given its declaration as code (a symbol's).
        const auto withCode = [&](std::size_t before, const std::string& code) {
            if (items.size() > before)
                items.back().code = code;
        };
        const auto finish = [&] {
            std::stable_sort(items.begin(), items.end(), [](const Completion& x, const Completion& y) {
                if (x.rank != y.rank)
                    return x.rank < y.rank;
                return lowered(x.label) < lowered(y.label);
            });
            std::set<std::string> seen;
            for (Completion& c : items)
                if (seen.insert(c.label).second)
                    out.items.push_back(std::move(c));
            if (out.items.size() > 200)
                out.items.resize(200);
            for (std::size_t i = 0; i < out.items.size(); ++i)
                out.items[i].rank = static_cast<int>(i);
            return out;
        };

        // A directive: #include's files, #pragma's words.
        const std::string head = trimmed(line.substr(0, static_cast<std::size_t>(a)));
        if (!head.empty() && head[0] == '#')
        {
            std::string name;
            int from = 0, to = 0;
            if (includeOn(line, name, from, to) && col >= from && col <= to)
            {
                out.prefix = std::string(line.substr(static_cast<std::size_t>(from), static_cast<std::size_t>(col - from)));
                out.replace = { { at.line, from }, { at.line, to } };
                for (const auto& g : m.files)
                    if (g->name != f->name)
                        offer(g->name, Kind::file, g->program ? "a file of the program" : "the plugin's (tempo, wheels)",
                              trimmed(g->header).substr(0, 400), {}, 0);
                for (const auto& g : m.library)
                    offer(g->name, Kind::file, "the library", g->header, {}, 1);
                return finish();
            }
            const std::string afterHash = trimmed(head.substr(1));
            if (afterHash.empty())
            {
                offer("include", Kind::word, "#include \"file\"", "Reads the file there, once.", "include \"$0\"", 0);
                offer("pragma", Kind::word, "#pragma panel ...", "The plugin's panel lines (sections, labels, styles, "
                                                                 "displays); FMinus ignores them.", "pragma panel ", 0);
                return finish();
            }
            if (afterHash == "pragma")
            {
                offer("panel", Kind::word, "#pragma panel ...", "Lays out the dials and displays.", {}, 0);
                return finish();
            }
            if (afterHash.rfind("pragma panel", 0) == 0)
            {
                const std::string rest = trimmed(afterHash.substr(12));
                if (rest.empty())
                    for (const std::string_view w : kPanelWords)
                        offer(std::string(w), Kind::word, "#pragma panel " + std::string(w), {}, {}, 0);
                else if (rest.rfind("style", 0) == 0)
                    for (const std::string_view w : kPanelStyles)
                        offer(std::string(w), Kind::word, "a dial style", {}, {}, 0);
                // The main module's params, for the lines that name dials.
                if (!rest.empty())
                    for (const auto& g : m.files)
                        for (const Symbol& s : g->decls)
                            if (s.kind == Kind::param && (s.container == "Effect" || s.container == "Voice"))
                                offer(s.name, Kind::param, s.signature, s.doc, {}, 1);
                return finish();
            }
            return out;
        }

        // Inside [[ ]]: the attributes.
        {
            int k = a;
            while (k > 0 && (isIdentChar(line[static_cast<std::size_t>(k - 1)]) || line[static_cast<std::size_t>(k - 1)] == ':'))
                --k;
            int s = k;
            while (s > 0 && line[static_cast<std::size_t>(s - 1)] == ' ')
                --s;
            const bool afterOpen = s >= 2 && line.substr(static_cast<std::size_t>(s - 2), 2) == "[[";
            const bool afterComma = s >= 1 && line[static_cast<std::size_t>(s - 1)] == ','
                                 && line.rfind("[[", static_cast<std::size_t>(s)) != std::string_view::npos
                                 && (line.rfind("]]", static_cast<std::size_t>(s)) == std::string_view::npos
                                     || line.rfind("]]", static_cast<std::size_t>(s)) < line.rfind("[[", static_cast<std::size_t>(s)));
            if (afterOpen || afterComma)
            {
                out.prefix = std::string(line.substr(static_cast<std::size_t>(k), static_cast<std::size_t>(col - k)));
                out.replace = { { at.line, k }, at };
                for (const AttributeInfo& x : kAttributes)
                    offer(std::string(x.label), Kind::attribute, std::string(x.insert.substr(0, x.insert.find('$'))), std::string(x.doc),
                          std::string(x.insert), 0);
                return finish();
            }
        }

        // After `.` or `->`: the members of what is before it.
        {
            int k = a;
            while (k > 0 && line[static_cast<std::size_t>(k - 1)] == ' ')
                --k;
            const bool dot = k >= 1 && line[static_cast<std::size_t>(k - 1)] == '.'
                          && !(k >= 2 && isDigit(line[static_cast<std::size_t>(k - 2)]) && [&] {
                                 int q = k - 2;
                                 while (q >= 0 && isIdentChar(line[static_cast<std::size_t>(q)]))
                                     --q;
                                 return q + 1 < k - 1 && isDigit(line[static_cast<std::size_t>(q + 1)]);
                             }());
            const bool arrow = k >= 2 && line.substr(static_cast<std::size_t>(k - 2), 2) == "->";
            if (dot || arrow)
            {
                out.member = true;
                const Pos opAt{ at.line, k - (arrow ? 2 : 1) };
                int opIndex = -1;
                for (std::size_t i = 0; i < f->code.size(); ++i)
                    if (startOf(f->code[i]) == opAt)
                        opIndex = static_cast<int>(i);
                const std::string type = opIndex > 0 ? m.typeOf(file, opIndex - 1) : std::string();
                const std::string base = baseType(type);
                if (const int lanes = lanesOf(base); lanes > 0)
                {
                    static constexpr const char* kXyzw[4] = { "x", "y", "z", "w" };
                    for (int l = 0; l < std::min(lanes, 4); ++l)
                        offer(kXyzw[l], Kind::lane, "float: lane " + std::to_string(l), "A lane of the " + base + ".", {}, 0);
                    for (int l = 0; l < lanes; ++l)
                        offer("s" + std::to_string(l), Kind::lane, "float: lane " + std::to_string(l), "A lane of the " + base + ".",
                              {}, 1);
                    return finish();
                }
                for (const Symbol* s : m.membersOf(base))
                {
                    const std::size_t before = items.size();
                    offer(s->name, s->kind, s->kind == Kind::method ? s->signature : s->type,
                          s->doc.empty() ? memberDoc(m.moduleNamed(base) ? m.moduleNamed(base)->doc : std::string(), s->name)
                                         : s->doc,
                          s->kind == Kind::method ? s->name + (s->params.empty() ? "()" : "($0)") : std::string(), 0);
                    withCode(before, s->signature);
                }
                return finish();
            }
        }

        // Everything a name may be here.
        const Pos p = out.replace.from;
        for (const Symbol& s : f->decls)
            if ((s.kind == Kind::local || s.kind == Kind::argument) && s.scope.from < p && p <= s.scope.to && s.at < p)
            {
                const std::size_t before = items.size();
                offer(s.name, s.kind, s.type, s.doc, {}, 0);
                withCode(before, s.signature);
            }
        const std::string module = m.moduleAt(*f, p);
        const Symbol* function = m.functionAt(*f, p);
        if (!module.empty())
            for (const Symbol* s : m.membersOf(module))
                if (!(s->at == p))
                {
                    const std::size_t before = items.size();
                    offer(s->name, s->kind, s->kind == Kind::method ? s->signature : s->type, s->doc,
                          s->kind == Kind::method ? s->name + (s->params.empty() ? "()" : "($0)") : std::string(), 1);
                    withCode(before, s->signature);
                }
        std::set<std::string> modulesSeen;
        for (const int fi : m.visible(file))
            for (const Symbol& s : m.files[static_cast<std::size_t>(fi)]->decls)
                if ((s.kind == Kind::function || s.kind == Kind::module) && !(fi == file && s.at == p))
                {
                    if (s.kind == Kind::module)
                        modulesSeen.insert(s.name);
                    offer(s.name, s.kind, s.kind == Kind::function ? s.signature : "module",
                          s.doc.size() > 600 ? s.doc.substr(0, 600) + "..." : s.doc,
                          s.kind == Kind::function ? s.name + (s.params.empty() ? "()" : "($0)") : std::string(), 2);
                }
        for (const BuiltinInfo& b : kBuiltins)
            offer(std::string(b.name), Kind::builtin,
                  std::string(b.result) + " " + std::string(b.name) + "(" + std::string(b.params) + ")", std::string(b.doc),
                  std::string(b.name) + (b.params.empty() ? "()" : "($0)"), 3);
        // The library's modules the program does not read: accepting one adds its #include (and its tab).
        if (function == nullptr || !out.prefix.empty())
            for (const auto& g : m.library)
                for (const Symbol& s : g->decls)
                    if (s.kind == Kind::module && modulesSeen.count(s.name) == 0)
                    {
                        const int before = static_cast<int>(items.size());
                        offer(s.name, Kind::module, "library: " + g->name + " (adds its #include)",
                              s.doc.size() > 600 ? s.doc.substr(0, 600) + "..." : s.doc, {}, 4);
                        if (static_cast<int>(items.size()) > before)
                            items.back().include = g->name;
                    }
        for (const std::string_view w : kKeywords)
            if (const WordDoc* d = wordDoc(w))
                offer(std::string(w), Kind::keyword, std::string(d->code), std::string(d->doc), {}, 5);
            else
                offer(std::string(w), Kind::keyword, {}, {}, {}, 5);
        for (const std::string_view w : kTypeWords)
        {
            const WordDoc* d = wordDoc(w);
            offer(std::string(w), Kind::type, "type", d != nullptr ? std::string(d->doc) : std::string(), {}, 5);
        }
        // Snippets: a loop over the block, and in the main module the contract's methods.
        if (function != nullptr)
            offer("for", Kind::snippet, "for (int i = 0; i < n; ++i)", "A loop over the call's samples.",
                  "for (int i = 0; i < n; ++i)\n{\n    $0\n}", 6);
        const std::string main = m.options.instrument ? "Voice" : "Effect";
        if (module == main && function == nullptr)
        {
            if (m.options.instrument)
            {
                offer("process", Kind::snippet, "bool process(float* restrict left, float* restrict right, int n)",
                      "A voice's next n samples into the buffers (they arrive at 0); false once it is silent for good.",
                      "bool process(float* restrict left, float* restrict right, int n)\n{\n    for (int i = 0; i < n; ++i)\n"
                      "    {\n        $0\n    }\n    return true;\n}", 6);
                offer("start", Kind::snippet, "void start(float note, float velocity)",
                      "A note-on: the MIDI note (69 is A 440 Hz) and 0..1.",
                      "void start(float note, float velocity)\n{\n    $0\n}", 6);
                offer("stop", Kind::snippet, "void stop()", "The note-off.", "void stop()\n{\n    $0\n}", 6);
            }
            else
                offer("process", Kind::snippet, "void process(float* restrict left, float* restrict right, int n)",
                      "n samples of both channels, in place, on the audio thread.",
                      "void process(float* restrict left, float* restrict right, int n)\n{\n    for (int i = 0; i < n; ++i)\n"
                      "    {\n        $0\n    }\n}", 6);
            offer("init", Kind::snippet, "void init(float rate)", "When the program starts and the rate changes, off "
                                                                  "the audio thread: allocate here.",
                  "void init(float rate)\n{\n    $0\n}", 6);
            offer("end", Kind::snippet, "void end()", "Before the program goes: free what init allocated.",
                  "void end()\n{\n    $0\n}", 6);
        }
        return finish();
    }

    // ---- signature help ---------------------------------------------------------------------------------------------
    std::optional<Signature> Service::signature(int file, Pos at) const
    {
        const Impl& m = *impl_;
        const FileData* f = m.fileAt(file);
        if (f == nullptr)
            return std::nullopt;
        int k = Impl::lastBefore(*f, at);
        int commas = 0;
        while (k >= 0)
        {
            const Token& t = f->code[static_cast<std::size_t>(k)];
            const std::string_view x = t.text;
            if (t.kind == Tok::punct && (x == ")" || x == "]"))
            {
                const int o = f->match[static_cast<std::size_t>(k)];
                if (o < 0)
                    return std::nullopt;
                k = o - 1;
                continue;
            }
            if (t.kind == Tok::punct && (x == ";" || x == "{" || x == "}"))
                return std::nullopt;
            if (t.kind == Tok::punct && x == "[")
            {
                commas = 0;
                --k;
                continue;
            }
            if (t.kind == Tok::punct && x == ",")
            {
                ++commas;
                --k;
                continue;
            }
            if (t.kind == Tok::punct && x == "(")
            {
                // A call: a name, or a vector literal's (float4)( ...
                Signature s;
                std::string result, name, doc;
                std::vector<std::string> params;
                bool found = false;
                if (Impl::isIdent(*f, k - 1))
                {
                    const std::string_view callee = f->code[static_cast<std::size_t>(k - 1)].text;
                    if (const Symbol* sym = m.resolve(file, k - 1); sym != nullptr && (sym->kind == Kind::function || sym->kind == Kind::method))
                    {
                        result = sym->type;
                        name = sym->name;
                        params = sym->params;
                        doc = sym->doc;
                        if (doc.empty() && sym->kind == Kind::method)
                            if (const Symbol* mod = m.moduleNamed(sym->container))
                                doc = memberDoc(mod->doc, sym->name);
                        found = true;
                    }
                    else if (const BuiltinInfo* b = builtinNamed(callee))
                    {
                        result = std::string(b->result);
                        name = std::string(b->name);
                        std::string list(b->params);
                        std::replace(list.begin(), list.end(), ',', '\n');
                        for (const std::string_view part : splitLines(list))
                            if (!trimmed(part).empty())
                                params.push_back(trimmed(part));
                        doc = std::string(b->doc);
                        found = true;
                    }
                }
                else if (Impl::isText(*f, k - 1, ")") && f->match[static_cast<std::size_t>(k - 1)] >= 0)
                {
                    const int o = f->match[static_cast<std::size_t>(k - 1)];
                    const std::string type = joinTokens(f->code, o + 1, k - 1);
                    if (const int lanes = lanesOf(type); lanes > 0)
                    {
                        result = type;
                        name = "(" + type + ")";
                        static constexpr const char* kXyzw[4] = { "x", "y", "z", "w" };
                        for (int l = 0; l < lanes; ++l)
                            params.push_back(lanes <= 4 ? std::string("float ") + kXyzw[l] : "float s" + std::to_string(l));
                        doc = "A vector literal: its lanes in order (a scalar broadcasts to every lane).";
                        found = true;
                    }
                }
                else if (Impl::isText(*f, k - 1, "sizeof"))
                {
                    result = "int";
                    name = "sizeof";
                    params = { "type" };
                    doc = "The type's size in bytes.";
                    found = true;
                }
                if (!found)
                {
                    commas = 0;                         // a grouping parenthesis: the call is further out
                    --k;
                    continue;
                }
                s.label = (name.front() == '(' ? name : result + " " + name) + "(";
                for (std::size_t i = 0; i < params.size(); ++i)
                {
                    if (i > 0)
                        s.label += ", ";
                    const int from = static_cast<int>(s.label.size());
                    s.label += params[i];
                    s.params.push_back({ from, static_cast<int>(s.label.size()) });
                }
                s.label += ")";
                s.active = params.empty() ? 0 : std::min(commas, static_cast<int>(params.size()) - 1);
                s.doc = doc;
                return s;
            }
            --k;
        }
        return std::nullopt;
    }

    // ---- hover ------------------------------------------------------------------------------------------------------
    std::optional<Hover> Service::hover(int file, Pos at) const
    {
        const Impl& m = *impl_;
        const FileData* f = m.fileAt(file);
        if (f == nullptr || at.line < 0 || at.line >= static_cast<int>(f->lines.size()))
            return std::nullopt;
        // An #include's file.
        {
            std::string name;
            int a = 0, b = 0;
            if (includeOn(f->lines[static_cast<std::size_t>(at.line)], name, a, b) && at.col >= a - 1 && at.col <= b + 1)
            {
                Hover h;
                h.range = { { at.line, a }, { at.line, b } };
                h.code = "#include \"" + name + "\"";
                if (const FileData* g = m.fileAt(m.indexOf(name)))
                    h.doc = g->header;
                else
                    for (const auto& g2 : m.library)
                        if (g2->name == name)
                            h.doc = g2->header + "\n\n(the library's: LIBRARY adds it as a tab)";
                if (h.doc.empty())
                    h.doc = "No tab has this name: add the file (+), or LIBRARY's.";
                return h;
            }
        }
        const int i = Impl::tokenAt(*f, at);
        if (i < 0)
            return std::nullopt;
        const Token& t = f->code[static_cast<std::size_t>(i)];
        Hover h;
        h.range = { startOf(t), endOf(t) };
        if (t.kind == Tok::keyword || t.kind == Tok::type)
        {
            const WordDoc* d = wordDoc(t.text);
            if (d == nullptr)
                return std::nullopt;
            h.code = std::string(d->code);
            h.doc = std::string(d->doc);
            return h;
        }
        if (t.kind != Tok::identifier)
            return std::nullopt;
        if (const Symbol* s = m.resolve(file, i))
        {
            switch (s->kind)
            {
                case Kind::local:
                    h.code = s->signature;
                    h.doc = (s->container.empty() ? "a local" : "a local of " + s->container) + (s->doc.empty() ? "" : ": " + s->doc);
                    break;
                case Kind::argument:
                    h.code = s->signature;
                    h.doc = "an argument of " + s->container + (s->doc.empty() ? "" : ": " + s->doc);
                    break;
                case Kind::field:
                case Kind::param:
                case Kind::method:
                {
                    h.code = s->signature;
                    std::string doc = s->doc;
                    if (doc.empty())
                        if (const Symbol* mod = m.moduleNamed(s->container))
                            doc = memberDoc(mod->doc, s->name);
                    const char* what = s->kind == Kind::field ? "a field of " : s->kind == Kind::param ? "a param of " : "a method of ";
                    h.doc = what + s->container + (doc.empty() ? "" : "\n" + doc);
                    break;
                }
                case Kind::function:
                    h.code = s->signature;
                    h.doc = s->doc;
                    break;
                case Kind::module:
                {
                    h.code = s->signature;
                    h.doc = s->doc;
                    std::string members;
                    for (const Symbol* x : m.membersOf(s->name))
                        if (x->kind == Kind::method)
                            members += (members.empty() ? "" : "  ") + x->name + "()";
                    if (!members.empty())
                        h.doc += (h.doc.empty() ? "" : "\n\n") + std::string("methods: ") + members;
                    break;
                }
                case Kind::keyword:
                case Kind::type:
                case Kind::builtin:
                case Kind::snippet:
                case Kind::file:
                case Kind::attribute:
                case Kind::lane:
                case Kind::word:
                    h.code = s->signature;
                    break;
            }
            if (s->file >= 0 && s->file != file && s->file < static_cast<int>(m.files.size()))
                h.doc += (h.doc.empty() ? "" : "\n\n") + std::string("(in ") + m.files[static_cast<std::size_t>(s->file)]->name + ")";
            return h;
        }
        if (const BuiltinInfo* b = builtinNamed(t.text))
        {
            h.code = std::string(b->result) + " " + std::string(b->name) + "(" + std::string(b->params) + ")";
            h.doc = std::string(b->doc);
            return h;
        }
        // A vector's lane (v.x, v.s3).
        if (Impl::isText(*f, i - 1, ".") && lanesOf(m.typeOf(file, i - 2)) > 0)
        {
            h.code = "float " + std::string(t.text);
            h.doc = "lanes of a " + baseType(m.typeOf(file, i - 2));
            return h;
        }
        return std::nullopt;
    }

    // ---- go to definition -------------------------------------------------------------------------------------------
    std::optional<Location> Service::definition(int file, Pos at) const
    {
        const Impl& m = *impl_;
        const FileData* f = m.fileAt(file);
        if (f == nullptr || at.line < 0 || at.line >= static_cast<int>(f->lines.size()))
            return std::nullopt;
        std::string name;
        int a = 0, b = 0;
        if (includeOn(f->lines[static_cast<std::size_t>(at.line)], name, a, b) && at.col >= a - 1 && at.col <= b + 1)
        {
            const int g = m.indexOf(name);
            if (g < 0)
                return std::nullopt;
            return Location{ g, name, {} };
        }
        const int i = Impl::tokenAt(*f, at);
        if (i < 0)
            return std::nullopt;
        const Symbol* s = m.resolve(file, i);
        if (s == nullptr || s->file < 0)
            return std::nullopt;
        // A prototype's name: its definition.
        if (s->kind == Kind::function && !s->body)
            if (const Symbol* d = m.globalNamed(file, s->name))
                s = d;
        return Location{ s->file, m.files[static_cast<std::size_t>(s->file)]->name, s->at };
    }

    // ---- occurrences ------------------------------------------------------------------------------------------------
    std::vector<Range> Service::occurrences(int file, Pos at) const
    {
        const Impl& m = *impl_;
        std::vector<Range> out;
        const FileData* f = m.fileAt(file);
        if (f == nullptr)
            return out;
        const int i = Impl::tokenAt(*f, at);
        if (i < 0 || !Impl::isIdent(*f, i))
            return out;
        const Token& t = f->code[static_cast<std::size_t>(i)];
        const Symbol* s = m.resolve(file, i);
        for (std::size_t k = 0; k < f->code.size(); ++k)
        {
            const Token& x = f->code[k];
            if (x.kind != Tok::identifier || x.text != t.text)
                continue;
            if (s != nullptr)
            {
                const Symbol* y = m.resolve(file, static_cast<int>(k));
                if (y == nullptr || !(y->file == s->file && y->at == s->at))
                    continue;
            }
            out.push_back({ startOf(x), endOf(x) });
        }
        return out;
    }
}
