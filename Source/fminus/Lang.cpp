// Source/fminus/Lang.cpp: see Lang.h (the formatter is LangFormat.cpp).
#include "fminus/Lang.h"

#include "fminus/Patch.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <utility>

namespace playground::lang
{
    std::string_view hostText() { return Patch::hostSource(); }

    namespace
    {
        constexpr std::array<std::string_view, 20> kKeywords = { "break",  "case",     "const",    "continue", "default",
                                                                 "else",   "enum",     "false",    "for",      "if",
                                                                 "module", "param",    "parallel", "restrict", "return",
                                                                 "sizeof", "switch",   "this",     "true",     "while" };
        constexpr std::array<std::string_view, 9> kTypeWords = { "bool", "double", "float", "float2", "float4", "float8",
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
            { "tanf", "float", "float x", "The tangent of x, in radians, by the math mode: a filter's prewarp, "
                                          "tanf(3.14159265f * fc / rate). A float vector too." },
            { "expf", "float", "float x", "e to the power x, by the math mode: expf(0.115129255f * dB) is a level's gain. "
                                          "A float vector too." },
            { "logf", "float", "float x", "The natural logarithm, by the math mode. A float vector too." },
            { "tanhf", "float", "float x", "The hyperbolic tangent, by the math mode: a soft clipper, -1 to 1. A float "
                                           "vector too." },
            // C's double functions that are one instruction: exact, the same in every math mode, on doubles only (an
            // argument converts to one). sinf and the others take a double rounded to a float.
            { "sqrt", "double", "double x", "The square root of a double, exact. sqrtf is a float's." },
            { "fabs", "double", "double x", "A double's absolute value, exact. fabsf is a float's." },
            { "floor", "double", "double x", "The largest whole number not above a double x, exact. floorf is a float's." },
            { "fmin", "double", "double x, double y", "The smaller of two doubles (a NaN argument is ignored), exact." },
            { "fmax", "double", "double x, double y", "The larger of two doubles (a NaN argument is ignored), exact." },
            { "fma", "double", "double x, double y, double z", "x * y + z rounded once, as C's: doubles, exact." },
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
            { "hash", "int", "int n", "n's 32 bits mixed into a well-spread int (lowbias32, a bijection), the same "
                                      "wherever it is computed: a seed that keeps two sources apart, randf(hash(voice) "
                                      "^ count)." },
            { "randf", "float", "int n", "A float uniform in [-1, 1) from hash(n)'s top 24 bits, with no state: white "
                                         "noise from a count of samples, out[i] = randf(count + i), which vectorises "
                                         "and is the same at any block size." },
        };

        struct WordDoc
        {
            std::string_view word, code, doc;
        };
        // The directives FMinus reads (hover on `#name`, completion after `#`); #include's and #pragma's are their own.
        constexpr WordDoc kDirectiveDocs[] = {
            { "if", "#if expression",
              "Reads the lines up to the next #elif, #else or #endif when the expression is not 0: C's integer "
              "arithmetic on numbers and macros; defined(NAME) is 1 when NAME is a macro, any other name 0. A skipped "
              "group is never compiled (its lines need not be FMinus)." },
            { "ifdef", "#ifdef NAME", "Reads the lines up to the next #elif, #else or #endif when NAME is a macro (the "
                                      "plugin defines DEBUG)." },
            { "ifndef", "#ifndef NAME", "Reads the lines up to the next #elif, #else or #endif when NAME is no macro: "
                                        "#ifndef LEVEL, #define LEVEL 2, #endif gives a default." },
            { "elif", "#elif expression", "The next group: read when no group before it was and the expression is not 0." },
            { "elifdef", "#elifdef NAME", "The next group: read when no group before it was and NAME is a macro." },
            { "elifndef", "#elifndef NAME", "The next group: read when no group before it was and NAME is no macro." },
            { "else", "#else", "The last group: read when no group before it was." },
            { "endif", "#endif", "Ends the conditional, in the file it began in." },
            { "define", "#define NAME value",
              "A macro, read in #if, #elif, #ifdef and #ifndef only: FMinus never substitutes one in code, where its "
              "name is an error (for a constant: an enum, or a const at the top level)." },
            { "undef", "#undef NAME", "Forgets a macro (the plugin's DEBUG too)." },
        };

        const WordDoc* directiveDoc(std::string_view name)
        {
            for (const WordDoc& w : kDirectiveDocs)
                if (w.word == name)
                    return &w;
            return nullptr;
        }

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
            { "const", "const float x = e;", "Never assigned after its initialiser; through a const pointer, read only. "
                                             "A const table of constants is stored once; at the top level, const tables "
                                             "and scalars are the program's, read by every function and module after "
                                             "them (1 MiB at most)." },
            { "this", "this", "The module's own address, in its method." },
            { "sizeof", "sizeof(type)", "The type's size in bytes, an int: calloc(n, sizeof(float))." },
            { "if", "if (condition) statement else statement", "An else closes the nearest if." },
            { "else", "if (condition) statement else statement", "" },
            { "for", "for (int i = 0; i < n; ++i) statement", "A loop; its declarations are the loop's." },
            { "while", "while (condition) statement", "" },
            { "break", "break;", "Leaves the innermost loop or switch." },
            { "continue", "continue;", "The innermost loop's next iteration (a switch's: the loop around it)." },
            { "switch", "switch (value) { case 1: ... break; default: ... }",
              "C's: on an int, a bool or an enum's value. Each case's value is an integer constant expression (an enum "
              "constant names one); the statements after a label run on through the next ones until a break." },
            { "case", "case VALUE:", "A switch's label: an integer constant expression (an enum constant, 2 * N). A "
                                     "declaration may follow it (C23's labels)." },
            { "default", "default:", "The switch's label for every value no case has; a switch without one does nothing "
                                     "then." },
            { "enum", "enum Wave { SAW, SQUARE = 4, TRIANGLE };",
              "C's enumeration: named int constants (SAW is 0, TRIANGLE 5), which size arrays and label cases; enum "
              "Wave is an int. Declared on its own, at the top level, in a module or in a block." },
            { "return", "return value;", "" },
            { "true", "true", "A bool: 1." },
            { "false", "false", "A bool: 0." },
            { "float", "float", "IEEE single precision. In FMinus a constant without the f suffix is a float too, unless "
                                "its arithmetic has a double." },
            { "double", "double",
              "IEEE double precision, 8 bytes: for an accumulator, a clock or slow state a float's 24 bits round away "
              "(t += 1e-6 at 90 never moves a float). A constant without the f suffix is a double only where its "
              "arithmetic has one (double y = x * 0.1; d < 0.1), else a float as always. No double vectors, no double "
              "sinf: sinf(d) computes in float; sqrt fabs floor fmin fmax fma are a double's builtins." },
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
                                                    "not clamp to it). A field's: what its displays span, and trace says it." },
            { "fminus::default", "fminus::default($0)", "A param's value when the program starts, and a double-click "
                                                        "on its dial." },
            { "clang::nonblocking", "clang::nonblocking", "May run on the audio thread: FMinus checks it cannot "
                                                          "allocate, lock or call what is not nonblocking itself." },
            { "clang::nonallocating", "clang::nonallocating", "Cannot reach malloc, calloc or free (checked)." },
            // The plugin's own, on a field (PanelSpec.h's FieldLook): FMinus passes them on unread.
            { "ui::unit", "ui::unit(\"$0\")", "A field's unit (\"dB\", \"Hz\"): its number display's readout shows it, and "
                                             "trace's lines say it." },
            { "ui::label", "ui::label(\"$0\")", "A field's label: what its displays are titled when the panel gives none." },
            { "ui::decimals", "ui::decimals($0)", "A field's places, 0 to 4: its number display's, when decimals= gives none." },
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
                    // a - or + with no operand before it is a sign: -5, (-1, 1)
                    const bool sign = (p == "-" || p == "+")
                                   && (i - 2 < from
                                       || (t[static_cast<std::size_t>(i - 2)].kind == Tok::op)
                                       || (t[static_cast<std::size_t>(i - 2)].kind == Tok::punct && t[static_cast<std::size_t>(i - 2)].text != ")"
                                           && t[static_cast<std::size_t>(i - 2)].text != "]")
                                       || (t[static_cast<std::size_t>(i - 2)].kind == Tok::keyword && t[static_cast<std::size_t>(i - 2)].text != "this"
                                           && t[static_cast<std::size_t>(i - 2)].text != "true" && t[static_cast<std::size_t>(i - 2)].text != "false"));
                    const bool tight = sign || x == "," || x == ";" || x == ")" || x == "]" || x == "." || x == "->" || x == "::"
                                    || p == "(" || p == "[" || p == "." || p == "->" || p == "::" || p == "!" || p == "~"
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

        // ---- C's integer constant expressions ---------------------------------------------------------------------
        // An integer constant expression over code tokens [from, to), folded as FMinus folds one (C11 6.6p6, its ints
        // 32 bits: + - * / % with C's errors for an overflow and a division by 0, a shift by its count's low five bits,
        // << dropping the bits shifted out and >> keeping the sign, ~ & | ^, comparisons and ! as 0 or 1, && || ?:
        // evaluating only what C does, so what they skip has no error), its names (enumeration constants) through
        // `lookup`: nothing when it is no constant, or not one FMinus would take. Its recursion is at most kMaxDepth
        // levels deep (a ?: branch or a unary operator one, a parenthesis two: 63 parentheses around a number): an
        // expression nested deeper folds to nothing, so no text runs a thread's stack out (the compiler's thread
        // folds every program's constants for its circuits' dials, Patch's checkedBindings).
        class Folder
        {
        public:
            using Lookup = std::function<std::optional<int64_t>(std::string_view)>;
            static constexpr int kMaxDepth = 128;

            Folder(const std::vector<Token>& t, int from, int to, const Lookup& lookup)
                : t_(t), k_(from), to_(std::min(to, static_cast<int>(t.size()))), lookup_(lookup)
            {
            }

            std::optional<int64_t> run()
            {
                const std::optional<int64_t> v = conditional();
                return v && k_ == to_ ? v : std::nullopt;
            }

        private:
            using V = std::optional<int64_t>;

            bool at(std::string_view x) const
            {
                return k_ < to_ && t_[static_cast<std::size_t>(k_)].kind != Tok::identifier && t_[static_cast<std::size_t>(k_)].text == x;
            }
            bool take(std::string_view x)
            {
                if (!at(x))
                    return false;
                ++k_;
                return true;
            }
            // An int's value, or what C refuses (an overflow), which an operand && || ?: skip may hold.
            V checked(int64_t v) const
            {
                if (v < std::numeric_limits<int32_t>::min() || v > std::numeric_limits<int32_t>::max())
                    return skip_ > 0 ? V(0) : std::nullopt;
                return v;
            }
            static int64_t wrap(int64_t v) { return static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(v))); }

            // One level of the recursion, counted while it lasts.
            struct Level
            {
                explicit Level(int& depth) : d(depth) { ++d; }
                ~Level() { --d; }
                Level(const Level&) = delete;
                Level& operator=(const Level&) = delete;
                int& d;
            };

            V conditional()
            {
                const Level level(depth_);
                if (depth_ > kMaxDepth)
                    return std::nullopt;
                const V c = logical(0);
                if (!c || !take("?"))
                    return c;
                skip_ += *c != 0 ? 0 : 1;
                const V a = conditional();
                skip_ -= *c != 0 ? 0 : 1;
                if (!a || !take(":"))
                    return std::nullopt;
                skip_ += *c != 0 ? 1 : 0;
                const V b = conditional();
                skip_ -= *c != 0 ? 1 : 0;
                if (!b)
                    return std::nullopt;
                return *c != 0 ? a : b;
            }

            // The binary operators, loosest first (C's precedence), level by level.
            V logical(int level)
            {
                static constexpr std::string_view kLevels[][4] = {
                    { "||" }, { "&&" }, { "|" }, { "^" }, { "&" }, { "==", "!=" }, { "<", "<=", ">", ">=" },
                    { "<<", ">>" }, { "+", "-" }, { "*", "/", "%" },
                };
                constexpr int kCount = static_cast<int>(std::size(kLevels));
                if (level == kCount)
                    return unary();
                V a = logical(level + 1);
                while (a)
                {
                    std::string_view op;
                    for (const std::string_view o : kLevels[level])
                        if (!o.empty() && at(o))
                            op = o;
                    if (op.empty())
                        return a;
                    ++k_;
                    // && and || skip their right side when the left decides
                    const bool skips = (op == "&&" && *a == 0) || (op == "||" && *a != 0);
                    skip_ += skips ? 1 : 0;
                    const V b = logical(level + 1);
                    skip_ -= skips ? 1 : 0;
                    if (!b)
                        return std::nullopt;
                    a = apply(op, *a, *b);
                }
                return a;
            }

            V apply(std::string_view op, int64_t a, int64_t b) const
            {
                if (op == "||")
                    return (a != 0 || b != 0) ? 1 : 0;
                if (op == "&&")
                    return (a != 0 && b != 0) ? 1 : 0;
                if (op == "|")
                    return a | b;
                if (op == "^")
                    return a ^ b;
                if (op == "&")
                    return a & b;
                if (op == "==")
                    return a == b ? 1 : 0;
                if (op == "!=")
                    return a != b ? 1 : 0;
                if (op == "<")
                    return a < b ? 1 : 0;
                if (op == "<=")
                    return a <= b ? 1 : 0;
                if (op == ">")
                    return a > b ? 1 : 0;
                if (op == ">=")
                    return a >= b ? 1 : 0;
                if (op == "<<")
                    return wrap(static_cast<int64_t>(static_cast<uint32_t>(a) << (b & 31)));
                if (op == ">>")
                    return a >> (b & 31);
                if (op == "+")
                    return checked(a + b);
                if (op == "-")
                    return checked(a - b);
                if (op == "*")
                    return checked(a * b);
                if (b == 0)
                    return skip_ > 0 ? V(0) : std::nullopt;     // a division by 0
                return checked(op == "/" ? a / b : a % b);
            }

            V unary()
            {
                const Level level(depth_);
                if (depth_ > kMaxDepth)
                    return std::nullopt;
                if (take("-"))
                {
                    const V v = unary();
                    return v ? checked(-*v) : std::nullopt;
                }
                if (take("+"))
                    return unary();
                if (take("~"))
                {
                    const V v = unary();
                    return v ? V(~*v) : std::nullopt;
                }
                if (take("!"))
                {
                    const V v = unary();
                    return v ? V(*v == 0 ? 1 : 0) : std::nullopt;
                }
                // a cast to int (or to an enumeration, an int)
                if (at("(") && k_ + 2 < to_ && t_[static_cast<std::size_t>(k_ + 1)].text == "int"
                    && t_[static_cast<std::size_t>(k_ + 2)].text == ")")
                {
                    k_ += 3;
                    return unary();
                }
                if (at("(") && k_ + 3 < to_ && t_[static_cast<std::size_t>(k_ + 1)].text == "enum"
                    && t_[static_cast<std::size_t>(k_ + 3)].text == ")")
                {
                    k_ += 4;
                    return unary();
                }
                return primary();
            }

            V primary()
            {
                if (k_ >= to_)
                    return std::nullopt;
                const Token& t = t_[static_cast<std::size_t>(k_)];
                if (take("("))
                {
                    const V v = conditional();
                    return v && take(")") ? v : std::nullopt;
                }
                if (t.kind == Tok::keyword && (t.text == "true" || t.text == "false"))
                {
                    ++k_;
                    return t.text == "true" ? 1 : 0;
                }
                if (t.kind == Tok::identifier)
                {
                    ++k_;
                    return lookup_ ? lookup_(t.text) : std::nullopt;
                }
                if (t.kind == Tok::number)
                {
                    ++k_;
                    return number(t.text);
                }
                return std::nullopt;
            }

            // An int constant: decimal, octal (a leading 0) or hex, at most INT_MAX; a float's is none.
            static V number(std::string_view x)
            {
                int base = 10;
                std::size_t i = 0;
                if (x.size() > 1 && x[0] == '0' && (x[1] == 'x' || x[1] == 'X'))
                {
                    base = 16;
                    i = 2;
                }
                else if (x.size() > 1 && x[0] == '0')
                    base = 8;
                if (i >= x.size())
                    return std::nullopt;
                int64_t v = 0;
                for (; i < x.size(); ++i)
                {
                    const char c = x[i];
                    const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
                    if (d >= base)
                        return std::nullopt;                  // a float's ., e or f, a suffix
                    v = v * base + d;
                    if (v > std::numeric_limits<int32_t>::max())
                        return std::nullopt;
                }
                return v;
            }

            const std::vector<Token>& t_;
            int k_ = 0, to_ = 0;
            const Lookup& lookup_;
            int skip_ = 0;
            int depth_ = 0;                      // the levels open (Level), each a conditional() or a unary()
        };

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
            // Its enumeration constants whose values are folded once every file is indexed (foldValues): the
            // constant (a decls index), its expression's code tokens [from, to) (none: from < 0, the one before
            // plus 1), the constant before it in its list (-1 for none).
            struct Pending
            {
                int decl = -1, from = -1, to = -1, before = -1;
            };
            std::vector<Pending> pending;
            std::string header;                  // the comment lines the file starts with
            FileConditionals cond;               // its conditionals as the compile reads them (Options::defines): the
                                                 // tokens of a group it skips are left out of the index
            // A netlist's (indexCircuit): its pots and switches, in order, as circuitDials() gives them.
            std::vector<CircuitPot> pots;
            std::vector<CircuitSwitch> switches;
        };

        // SPICE's number, as FMinus's circuit compiler reads one ([+-]digits[.digits][e[+-]digits], then t g meg k m u
        // n p f in any case, meg before m, then letters it ignores): nothing when `s` is none.
        std::optional<double> readSpiceNumber(std::string_view s)
        {
            std::size_t i = 0;
            if (i < s.size() && (s[i] == '+' || s[i] == '-'))
                ++i;
            bool any = false;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9')
                ++i, any = true;
            if (i < s.size() && s[i] == '.')
                for (++i; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i)
                    any = true;
            if (!any)
                return std::nullopt;
            if (i < s.size() && (s[i] == 'e' || s[i] == 'E'))
            {
                std::size_t j = i + 1;
                if (j < s.size() && (s[j] == '+' || s[j] == '-'))
                    ++j;
                if (j < s.size() && s[j] >= '0' && s[j] <= '9')
                {
                    while (j < s.size() && s[j] >= '0' && s[j] <= '9')
                        ++j;
                    i = j;
                }
            }
            // C's reading of the decimal number whatever the process's locale (a host's LC_NUMERIC may want a comma),
            // as FMinus's netlist reader has it
            double v = 0.0;
            std::istringstream in(std::string(s.substr(s[0] == '+' ? 1 : 0, i - (s[0] == '+' ? 1 : 0))));
            in.imbue(std::locale::classic());
            in >> v;
            if (in.fail())
                return std::nullopt;
            const std::string rest = lowered(s.substr(i));
            double scale = 1.0;
            std::size_t used = 0;
            if (rest.rfind("meg", 0) == 0)
                scale = 1e6, used = 3;
            else if (!rest.empty())
            {
                constexpr std::pair<char, double> kSuffixes[] = { { 't', 1e12 }, { 'g', 1e9 }, { 'k', 1e3 }, { 'm', 1e-3 },
                                                                  { 'u', 1e-6 }, { 'n', 1e-9 }, { 'p', 1e-12 }, { 'f', 1e-15 } };
                for (const auto& [c, k] : kSuffixes)
                    if (rest[0] == c)
                        scale = k, used = 1;
            }
            for (std::size_t k = used; k < rest.size(); ++k)
                if (!(rest[k] >= 'a' && rest[k] <= 'z'))
                    return std::nullopt;
            return v * scale;
        }

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
                    if (const int e = enumeration(i, {}, std::nullopt); e > i)
                    {
                        i = e;
                        continue;
                    }
                    if (const int e = function(i, {}); e > i)
                    {
                        i = e;
                        continue;
                    }
                    const int e = constants(i);
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

            // A type at i: [const] (type word | poly type word | enum tag | identifier); the index after it, -1 for none.
            int type(int i, std::string& out) const
            {
                int j = i;
                std::string s;
                if (is(j, "const"))
                {
                    s = "const ";
                    ++j;
                }
                if (is(j, "enum"))
                {
                    if (!ident(j + 1))
                        return -1;
                    s += "enum " + std::string(tok(j + 1).text);
                    j += 2;
                }
                else if (j < n_ && tok(j).kind == Tok::type)
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

            // After a declarator's name at j, C23's and FMinus's: its attribute specifiers ([[...]], what it declares
            // carries them), then its sizes ([N], or [R][C]: a 2-D array's, rows first), and specifiers after those
            // too (FMinus refuses them there; read so the index stays whole). `extent` gains the sizes as written
            // ("[4][16]"), `attrs` the attributes and `attrText` their text ("[[ui::unit(\"dB\")]]"). The index after.
            int declaratorTail(int j, std::string& extent, std::vector<Attribute>* attrs, std::string* attrText) const
            {
                while (j < n_)
                {
                    if (is(j, "[") && is(j + 1, "[") && partner(j) > j + 1)
                    {
                        const int close = partner(j);
                        if (attrs != nullptr)
                            readAttributes(j + 2, close - 1, *attrs);
                        if (attrText != nullptr)
                            *attrText += (attrText->empty() ? "" : " ") + joinTokens(t_, j, close + 1);
                        j = close + 1;
                        continue;
                    }
                    if (is(j, "[") && partner(j) > j)
                    {
                        extent += joinTokens(t_, j, partner(j) + 1);
                        j = partner(j) + 1;
                        continue;
                    }
                    break;
                }
                return j;
            }

            // The attributes of one specifier's tokens [from, to) (inside its [[ ]]), in order: `ns::name` or `name`,
            // with its arguments, comma-separated, each a string's characters (adjacent ones joined) or a number's
            // spelling. A name without a namespace that is none of C23's is dropped, as FMinus drops it (warning).
            void readAttributes(int from, int to, std::vector<Attribute>& out) const
            {
                static constexpr std::string_view kStandard[] = { "deprecated", "fallthrough", "maybe_unused", "nodiscard",
                                                                  "noreturn", "_Noreturn", "unsequenced", "reproducible" };
                int k = from;
                while (k < to)
                {
                    Attribute a;
                    while (k < to && !is(k, ",") && !is(k, "("))
                        a.name += std::string(tok(k++).text);
                    if (is(k, "(") && partner(k) > k && partner(k) < to)
                    {
                        const int close = partner(k);
                        int arg = k + 1;
                        while (arg < close)
                        {
                            int e = arg;
                            while (e < close && !is(e, ","))
                                e = (is(e, "(") || is(e, "[")) && partner(e) > e ? partner(e) + 1 : e + 1;
                            std::string text;
                            bool strings = e > arg;
                            for (int q = arg; q < e; ++q)
                                strings = strings && tok(q).kind == Tok::string;
                            if (strings)
                                for (int q = arg; q < e; ++q)
                                    text += stringValue(tok(q).text);
                            else
                                text = joinTokens(t_, arg, e);
                            a.args.push_back(std::move(text));
                            arg = e + 1;
                        }
                        k = close + 1;
                    }
                    std::string bare = a.name;
                    if (bare.size() > 4 && bare.rfind("__", 0) == 0 && bare.compare(bare.size() - 2, 2, "__") == 0)
                        bare = bare.substr(2, bare.size() - 4);
                    const bool kept = a.name.find("::") != std::string::npos
                                   || std::find(std::begin(kStandard), std::end(kStandard), bare) != std::end(kStandard);
                    if (!a.name.empty() && kept)
                        out.push_back(std::move(a));
                    while (k < to && !is(k, ","))
                        ++k;
                    ++k;
                }
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
                    if (const int en = enumeration(k, m.name, std::nullopt); en > k)
                    {
                        k = en;
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

            // An enumeration at i, C's: [const] enum [Tag] [: type] { NAME [= expr], ... [,] } [declarators] ; its tag
            // and its constants, a module's (seen by its methods) when `module` names one, a block's when `scope` is
            // given (to the block's end; `function` the block's), else the top level's. C23's fixed underlying type
            // (`: int`, `: bool`) is the tag's type, and declares the tag ahead of its list too (`enum Mode : int;`, a
            // tag without its list: no body). The names after its list (`enum Mode { SAW, SQUARE } mode;`) are what a
            // declaration of its type declares there, as FMinus has them: a block's locals, a module's fields, the
            // top level's constants (`const`, as every one there). The index after it; i when i starts none.
            int enumeration(int i, const std::string& module, std::optional<Pos> scopeEnd, const std::string& function = {})
            {
                const bool constant = is(i, "const");
                int j = constant ? i + 1 : i;
                if (!is(j, "enum"))
                    return i;
                const int keyword = j++;
                int tag = -1;
                if (ident(j))
                    tag = j++;
                std::string underlying = "int";
                const bool fixed = is(j, ":");
                if (fixed)
                {
                    j = type(j + 1, underlying);
                    if (j < 0)
                        return i;
                    if (underlying.rfind("const ", 0) == 0)
                        underlying.erase(0, 6);
                }
                const auto addTag = [&](int end, bool list) {
                    Symbol t;
                    t.kind = Kind::enumeration;
                    t.name = std::string(tok(tag).text);
                    t.type = underlying;
                    t.at = startOf(tok(tag));
                    t.container = module;
                    if (scopeEnd)
                        t.scope = { t.at, *scopeEnd };
                    t.signature = joinTokens(t_, keyword, end);
                    if (t.signature.size() > 160)
                        t.signature = std::string(headOf(t.signature, 156)) + " ...";
                    t.doc = docFor(tok(i).line);
                    t.body = list;
                    add(t);
                };
                if (fixed && tag >= 0 && is(j, ";"))
                {
                    addTag(j, false);
                    return j + 1;
                }
                if (!is(j, "{") || partner(j) <= j)
                    return i;
                const int open = j, close = partner(j);
                if (tag >= 0)
                    addTag(close + 1, true);
                int before = -1;
                for (int k = open + 1; k < close;)
                {
                    if (!ident(k))
                    {
                        ++k;
                        continue;
                    }
                    const Token& name = tok(k);
                    int e = k + 1, from = -1, to = -1;
                    if (is(e, "="))
                        from = e + 1;
                    while (e < close && !is(e, ","))
                        e = (is(e, "(") || is(e, "[")) && partner(e) > e ? partner(e) + 1 : e + 1;
                    if (from >= 0)
                        to = e;
                    Symbol c;
                    c.kind = Kind::constant;
                    c.name = std::string(name.text);
                    c.type = "int";
                    c.at = startOf(name);
                    c.container = module;
                    c.enumeration = tag >= 0 ? std::string(tok(tag).text) : std::string();
                    if (scopeEnd)
                        c.scope = { c.at, *scopeEnd };
                    c.signature = from >= 0 ? c.name + " = " + joinTokens(t_, from, to) : c.name;
                    if (c.signature.size() > 160)   // an expression as written (when it folds: its value, foldValues)
                        c.signature = std::string(headOf(c.signature, 156)) + " ...";
                    c.doc = docFor(name.line);
                    const int index = add(c);
                    f_.pending.push_back({ index, from, to, before });
                    before = index;
                    k = e + 1;
                }
                const int after = close + 1;
                if (is(after, ";"))
                    return after + 1;
                // The names of its type after its list, to the statement's ';'.
                const int stop = std::min(statementEnd(after, n_) + 1, n_);
                const std::string base = std::string(constant ? "const " : "")
                                       + (tag >= 0 ? "enum " + std::string(tok(tag).text) : underlying);
                int e = -1;
                if (scopeEnd)
                {
                    e = localDeclarators(after, base, stop, *scopeEnd, function);
                    if (is(e, ";"))
                        ++e;
                }
                else if (!module.empty())
                    e = fieldDeclarators(after, base, module, stop);
                else if (constant)
                    e = constantDeclarators(after, base, stop);
                return e > after ? e : after;
            }

            // The top level's constants at i, C's file-scope const objects: const type declarator (, declarator)* ;
            // (a table, a scalar, a pointer to an element), each with its initialiser. The index after its ';', i when
            // i starts none.
            int constants(int i)
            {
                if (!is(i, "const"))
                    return i;
                std::string base;
                const int j = type(i, base);
                if (j < 0)
                    return i;
                const int e = constantDeclarators(j, base, n_);
                return e < 0 ? i : e;
            }

            // The top level's constants of type `base` declared from j on (before `limit`): declarator (, declarator)* ;
            // The index after its ';', -1 when j starts none.
            int constantDeclarators(int j, const std::string& base, int limit)
            {
                bool any = false;
                while (j < limit)
                {
                    std::string ty = base;
                    j = pointer(j, ty);
                    if (!ident(j))
                        break;
                    const Token& name = tok(j++);
                    std::string extent;
                    j = declaratorTail(j, extent, nullptr, nullptr);
                    if (!any && !(is(j, "=") || is(j, ";") || is(j, ",")))
                        return -1;
                    any = true;
                    int e = j, from = -1;
                    if (is(e, "="))
                        from = ++e;
                    while (e < limit && !is(e, ",") && !is(e, ";"))
                        e = (is(e, "(") || is(e, "[") || is(e, "{")) && partner(e) > e ? partner(e) + 1 : e + 1;
                    Symbol c;
                    c.kind = Kind::constant;
                    c.name = std::string(name.text);
                    c.type = ty + extent;
                    c.at = startOf(name);
                    std::string init = from >= 0 ? joinTokens(t_, from, e) : std::string();
                    if (init.size() > 64)
                        init = std::string(headOf(init, 60)) + " ...";
                    c.signature = ty + (ty.back() == '*' ? "" : " ") + c.name + extent + (init.empty() ? "" : " = " + init);
                    c.doc = docFor(name.line);
                    add(c);
                    j = e;
                    if (is(j, ","))
                    {
                        ++j;
                        continue;
                    }
                    break;
                }
                if (!any)
                    return -1;
                return is(j, ";") ? j + 1 : j;
            }

            // Past a label at i (`case expr :` or `default :`, C23's: a block item of its own), where a statement may
            // start: after its `:` (a ?: inside the value has its own).
            int pastLabel(int i, int limit) const
            {
                int questions = 0;
                for (int j = i + 1; j < limit;)
                {
                    if ((is(j, "(") || is(j, "[")) && partner(j) > j)
                    {
                        j = partner(j) + 1;
                        continue;
                    }
                    if (is(j, "?"))
                        ++questions;
                    else if (is(j, ":"))
                    {
                        if (questions == 0)
                            return j + 1;
                        --questions;
                    }
                    else if (is(j, ";") || is(j, "{") || is(j, "}"))
                        return j;
                    ++j;
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
                std::string extent;
                (void)declaratorTail(j + 1, extent, &s.attributes, nullptr);
                int e = j + 1;
                while (e < limit && !is(e, ";"))
                    e = is(e, "(") || is(e, "[") ? std::max(e + 1, partner(e) + 1) : e + 1;
                s.signature = joinTokens(t_, i, e);
                s.doc = docFor(tok(i).line);
                add(s);
                return e + 1;
            }

            // A field declaration at i: [const] type [*] name [[attributes]]* ([N]){0,2} (, ...)* ;
            int fields(int i, const std::string& module, int limit)
            {
                std::string base;
                const int j = type(i, base);
                if (j < 0)
                    return i;
                const int e = fieldDeclarators(j, base, module, limit);
                return e < 0 ? i : e;
            }

            // A module's fields of type `base` declared from j on (before `limit`): [*] name [[attributes]]* ([N]){0,2}
            // (, ...)* ; the index after its ';', -1 when j starts none.
            int fieldDeclarators(int j, const std::string& base, const std::string& module, int limit)
            {
                bool any = false;
                while (j < limit)
                {
                    std::string ty = base;
                    j = pointer(j, ty);
                    if (!ident(j))
                        break;
                    const Token& name = tok(j++);
                    std::string extent, attrText;
                    std::vector<Attribute> attrs;
                    j = declaratorTail(j, extent, &attrs, &attrText);
                    ty += extent;
                    if (!any && !(is(j, ";") || is(j, ",")))
                        return -1;
                    any = true;
                    Symbol s;
                    s.kind = Kind::field;
                    s.name = std::string(name.text);
                    s.type = ty;
                    s.at = startOf(name);
                    s.container = module;
                    s.signature = ty + " " + s.name + (attrText.empty() ? "" : " " + attrText);
                    s.attributes = std::move(attrs);
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
                    return -1;
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
                const int j = type(i, base);
                if (j < 0)
                    return -1;
                return localDeclarators(j, base, limit, scopeEnd, function);
            }

            // A block's locals of type `base` declared from j on (before `limit`): [*] name ([N]){0,2} [= init] (, ...)* ;
            // the index of its ';' (or where it stopped), -1 when j starts none.
            int localDeclarators(int j, const std::string& base, int limit, Pos scopeEnd, const std::string& function)
            {
                bool any = false;
                while (j < limit)
                {
                    std::string ty = base;
                    j = pointer(j, ty);
                    if (!ident(j))
                        break;
                    const Token& name = tok(j++);
                    std::string extent;
                    j = declaratorTail(j, extent, nullptr, nullptr);
                    ty += extent;
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
                        if (is(k, "case") || is(k, "default"))
                        {
                            k = pastLabel(k, close);
                            start = true;
                            continue;
                        }
                        if (is(k, "enum") || (is(k, "const") && is(k + 1, "enum")))
                        {
                            const int blockClose = partner(blocks.back()) > 0 ? partner(blocks.back()) : close;
                            const int e = enumeration(k, {}, startOf(tok(blockClose)), function);
                            if (e > k)
                            {
                                k = e;
                                start = true;
                                continue;
                            }
                        }
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
                        if ((is(k, "if") || is(k, "while") || is(k, "switch")) && is(k + 1, "(") && partner(k + 1) > k + 1)
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

        // A netlist (a .cir file) as the module it generates (Lang.h): read line by line, never as FMinus. Its
        // logical lines are its lines with their continuations (a leading +) joined, comments left out; the
        // directives say the module's name and its methods' parameters.
        void indexCircuit(FileData& f, int index)
        {
            f.lines = splitLines(f.text);
            f.all = lexCir(f.text);
            f.code.clear();
            f.match.clear();
            f.includes.clear();
            f.pending.clear();
            f.decls.clear();
            f.bodies.clear();
            f.modules.clear();
            f.pots.clear();
            f.switches.clear();
            // The header: the * lines it starts with, each without its * and the space after it; the schematic's *@
            // lines (its drawing, fminus/CirDoc.h) are no part of it, wherever they are.
            f.header.clear();
            for (const std::string_view l : f.lines)
            {
                const std::string t = trimmed(l);
                if (t.size() >= 2 && t[0] == '*' && t[1] == '@')
                    continue;
                if (t.empty() || t[0] != '*')
                    break;
                std::string body = t.substr(1);
                if (!body.empty() && body.front() == ' ')
                    body.erase(body.begin());
                f.header += (f.header.empty() ? "" : "\n") + body;
            }
            std::vector<std::vector<Token>> logical;
            for (std::size_t ln = 0; ln < f.lines.size(); ++ln)
            {
                std::vector<Token> toks, code;
                lexCirLine(f.lines[ln], static_cast<int>(ln), toks);
                for (const Token& t : toks)
                    if (t.kind != Tok::comment)
                        code.push_back(t);
                if (code.empty())
                    continue;
                if (code[0].kind == Tok::punct && code[0].text == "+")
                {
                    // A continuation (lexCirLine's leading +): its tokens are the line's before it.
                    if (!logical.empty())
                        logical.back().insert(logical.back().end(), code.begin() + 1, code.end());
                    continue;
                }
                logical.push_back(std::move(code));
            }
            std::string title;
            Pos titleAt{};
            struct Arg
            {
                std::string name;
                Pos at{};
            };
            std::vector<Arg> pots, switches, laws;
            std::optional<Arg> signal;
            bool two = false;
            for (const std::vector<Token>& l : logical)
            {
                if (l.empty() || l[0].kind != Tok::directive)
                    continue;
                const std::string d = lowered(l[0].text);
                if (d == ".end")
                    break;
                if (l.size() < 2)
                    continue;
                const Arg arg{ std::string(l[1].text), startOf(l[1]) };
                if (d == ".title" && title.empty())
                {
                    title = arg.name;
                    titleAt = arg.at;
                }
                else if (d == ".pot")
                {
                    // .pot NAME [default] [lin|audio|rlog], as FMinus reads it (a default past 0..1 is its error)
                    pots.push_back(arg);
                    CircuitPot pot{ arg.name, 0.5f, {} };
                    for (std::size_t i = 2; i < l.size(); ++i)
                    {
                        const std::string w = lowered(l[i].text);
                        if (w == "lin" || w == "audio" || w == "rlog")
                            pot.taper = std::string(l[i].text);
                        else if (i == 2)
                            if (const std::optional<double> v = readSpiceNumber(l[i].text); v && *v >= 0.0 && *v <= 1.0)
                                pot.def = static_cast<float>(*v);
                    }
                    f.pots.push_back(std::move(pot));
                }
                else if (d == ".switch")
                {
                    // .switch NAME positions [default]: whole numbers, positions 2 to 8, the default below them
                    switches.push_back(arg);
                    CircuitSwitch sw{ arg.name, 2, 0 };
                    const auto whole = [](std::string_view t) {
                        int v = 0;
                        if (t.empty() || t.size() > 2)
                            return -1;
                        for (const char c : t)
                        {
                            if (c < '0' || c > '9')
                                return -1;
                            v = 10 * v + (c - '0');
                        }
                        return v;
                    };
                    if (l.size() >= 3)
                        if (const int n = whole(l[2].text); n >= 2 && n <= 8)
                            sw.positions = n;
                    if (l.size() >= 4)
                        if (const int k = whole(l[3].text); k >= 0 && k < sw.positions)
                            sw.def = k;
                    f.switches.push_back(std::move(sw));
                }
                else if (d == ".input")
                {
                    if (l.size() >= 3 && l[2].text == "-")
                        laws.push_back(arg);
                    else if (!signal)
                        signal = arg;
                }
                else if (d == ".channels")
                    two = l[1].text == "2";
            }
            std::string name = title;
            if (name.empty())
            {
                name = f.name;
                if (const std::size_t dot = name.rfind('.'); dot != std::string::npos && dot > 0)
                    name.resize(dot);
                if (!name.empty() && name[0] >= 'a' && name[0] <= 'z')
                    name[0] = static_cast<char>(name[0] - 'a' + 'A');
            }
            Symbol m;
            m.kind = Kind::module;
            m.name = name;
            m.type = name;
            m.signature = "module " + name;
            m.doc = f.header;
            m.file = index;
            m.at = titleAt;
            f.decls.push_back(m);
            const auto method = [&](const std::string& type, const char* n, std::vector<std::string> params, Pos at) {
                Symbol s;
                s.kind = Kind::method;
                s.name = n;
                s.type = type;
                s.container = name;
                s.signature = type + " " + n + "(";
                for (std::size_t k = 0; k < params.size(); ++k)
                    s.signature += (k == 0 ? "" : ", ") + params[k];
                s.signature += ")";
                s.params = std::move(params);
                s.file = index;
                s.at = at;
                s.body = true;
                f.decls.push_back(std::move(s));
            };
            method("void", "init", { "float rate" }, titleAt);
            if (!pots.empty() || !switches.empty())
            {
                std::vector<std::string> params;
                for (const Arg& a : pots)
                    params.push_back("float " + a.name);
                for (const Arg& a : switches)
                    params.push_back("int " + a.name);
                method("void", "set", std::move(params), !pots.empty() ? pots.front().at : switches.front().at);
            }
            {
                const std::string sample = two ? "float2" : "float";
                std::vector<std::string> params{ sample + " " + (signal ? signal->name : std::string("u")) };
                for (const Arg& a : laws)
                    params.push_back("float " + a.name);
                method(sample, "tick", std::move(params), signal ? signal->at : titleAt);
            }
            method("void", "clear", {}, titleAt);
        }

        void indexFile(FileData& f, int index)
        {
            if (isCircuitFile(f.name))
            {
                indexCircuit(f, index);
                return;
            }
            f.lines = splitLines(f.text);
            f.all = lex(f.text);
            f.code.clear();
            f.includes.clear();
            f.pending.clear();
            f.decls.clear();
            f.bodies.clear();
            f.modules.clear();
            for (const Token& t : f.all)
            {
                // A group a conditional skips is never read: no symbol, no #include, no bracket of it.
                if (t.line < static_cast<int>(f.cond.skipped.size()) && f.cond.skipped[static_cast<std::size_t>(t.line)] != 0)
                    continue;
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

        bool isEnumerator(const Symbol& s) { return s.kind == Kind::constant && s.type == "int"; }
        bool isScoped(const Symbol& s) { return !(s.scope.from == s.scope.to); }

        // ---- the order a compile reads the files' lines in --------------------------------------------------------------
        // Where each line of the files stands in the text a compile reads (C's translation unit): the first file from its
        // top (when it is the program's), an #include of another of them (in a group read) followed where it stands,
        // once; then each file the walk never reached, alone, in turn (a tab no #include reaches; each of the library's
        // files, a compile of its own). C's file scope holds a name from its declaration on, and FMinus's holds a module's
        // enumerations and their constants so (a C struct's): a use before them is FMinus's error, and Lang's nothing.
        class ReadOrder
        {
        public:
            ReadOrder() = default;
            explicit ReadOrder(const std::vector<FileData*>& files);

            // A line's place in the order (-1 for a file it never had); then the column orders a line's places.
            int line(int file, int ln) const
            {
                if (file < 0 || file >= static_cast<int>(lines_.size()) || lines_[static_cast<std::size_t>(file)].empty())
                    return -1;
                const std::vector<int>& at = lines_[static_cast<std::size_t>(file)];
                return at[static_cast<std::size_t>(std::clamp(ln, 0, static_cast<int>(at.size()) - 1))];
            }
            // Whether `a` of file fa stands before `b` of file fb (a place it never had: yes, as Lang was before it).
            bool before(int fa, Pos a, int fb, Pos b) const
            {
                const int ka = line(fa, a.line), kb = line(fb, b.line);
                if (ka < 0 || kb < 0)
                    return true;
                return ka < kb || (ka == kb && a.col < b.col);
            }

        private:
            std::vector<std::vector<int>> lines_;            // per file, per line
        };

        ReadOrder::ReadOrder(const std::vector<FileData*>& files)
        {
            lines_.resize(files.size());
            std::vector<bool> entered(files.size(), false);
            int next = 0;
            const auto indexOf = [&files](std::string_view name) {
                for (std::size_t i = 0; i < files.size(); ++i)
                    if (files[i]->name == name)
                        return static_cast<int>(i);
                return -1;
            };
            // A file's lines in turn, an #include's file (not entered yet) after its line: at most as deep as the files.
            const auto enter = [&](const auto& self, std::size_t f) -> void {
                entered[f] = true;
                std::vector<int>& at = lines_[f];
                at.assign(std::max<std::size_t>(files[f]->lines.size(), 1), 0);
                const auto& includes = files[f]->includes;
                std::size_t k = 0;
                for (std::size_t ln = 0; ln < at.size(); ++ln)
                {
                    at[ln] = next++;
                    for (; k < includes.size() && includes[k].second <= static_cast<int>(ln); ++k)
                        if (const int g = indexOf(includes[k].first); g >= 0 && !entered[static_cast<std::size_t>(g)])
                            self(self, static_cast<std::size_t>(g));
                }
            };
            if (!files.empty() && files[0]->program)
                enter(enter, 0);
            for (std::size_t f = 0; f < files.size(); ++f)
                if (!entered[f])
                    enter(enter, f);
        }

        // Every enumeration constant by name, with its file's index: in file order, then in declaration order.
        using Named = std::map<std::string_view, std::vector<std::pair<std::size_t, const Symbol*>>, std::less<>>;

        // The enumeration constant a name at `at` in files[file] means, as C's scopes choose it: the innermost block's
        // seen there (declared before it), else `module`'s own, else the top level's, else another module's declared
        // before `at` in the order the compile reads (C's file scope holds a struct's enumeration constants from there
        // on, as FMinus does a module's; FMinus refuses two of one name); a module's and the top level's first in the
        // files files[file] reads (`reads`), then in any. Never `self`, the constant whose expression the name is in;
        // nothing when no constant has that name.
        const Symbol* constantAt(const Named& named, const ReadOrder& order, std::size_t file, Pos at, const std::string& module,
                                 const std::vector<bool>& reads, const Symbol* self, std::string_view name)
        {
            const auto found = named.find(name);
            if (found == named.end())
                return nullptr;
            const std::vector<std::pair<std::size_t, const Symbol*>>& all = found->second;
            const Symbol* best = nullptr;
            for (const auto& [fi, s] : all)
                if (fi == file && s != self && isScoped(*s) && s->scope.from <= at && at <= s->scope.to
                    && (best == nullptr || best->at < s->at))
                    best = s;
            if (best != nullptr)
                return best;
            enum class Where { own, top, other };
            for (const Where where : { Where::own, Where::top, Where::other })
            {
                if (where == Where::own && module.empty())
                    continue;
                const auto in = [&](const std::string& container) {
                    return where == Where::own ? container == module
                         : where == Where::top ? container.empty()
                                               : !container.empty() && container != module;
                };
                for (const bool read : { true, false })
                    for (const auto& [fi, s] : all)
                        if (reads[fi] == read && s != self && !isScoped(*s) && in(s->container)
                            && (where != Where::other
                                || order.before(static_cast<int>(fi), s->at, static_cast<int>(file), at)))
                            return s;
            }
            return nullptr;
        }

        // The enumeration constants' values (FileData::pending), folded over every file once all are indexed, in the
        // order the compile reads them (`order`): a pass folds what it can (an expression whose names have values, the
        // constant before one plus 1), so the first folds every constant FMinus folds (a name is declared before its
        // use). Passes go on while one adds a value, kFoldPasses at most, for what FMinus refuses but Lang reads on (a
        // name of a list after it, of a file no #include reaches): each such link costs a pass, so a long chain of them
        // stays unfolded, never a pass for each. A name is the constant its place sees (constantAt): a block's, its
        // module's (its own, or that of the method it is in), the top level's, from the files its file reads (as
        // Impl::visible: its #includes, in turn theirs, and for a program's file what the program's first file reads),
        // another module's before it. Each signature then says its value, `SQUARE = 4`, or keeps its expression as
        // written when it does not fold.
        void foldValues(const std::vector<FileData*>& files, const ReadOrder& order)
        {
            const auto indexOf = [&files](std::string_view name) {
                for (std::size_t i = 0; i < files.size(); ++i)
                    if (files[i]->name == name)
                        return static_cast<int>(i);
                return -1;
            };
            std::vector<std::vector<bool>> reads(files.size(), std::vector<bool>(files.size(), false));
            for (std::size_t i = 0; i < files.size(); ++i)
            {
                std::vector<int> todo{ static_cast<int>(i) };
                if (files[i]->program && files[0]->program)
                    todo.push_back(0);
                while (!todo.empty())
                {
                    const int f = todo.back();
                    todo.pop_back();
                    if (f < 0 || reads[i][static_cast<std::size_t>(f)])
                        continue;
                    reads[i][static_cast<std::size_t>(f)] = true;
                    for (const auto& inc : files[static_cast<std::size_t>(f)]->includes)
                        todo.push_back(indexOf(inc.first));
                }
            }
            Named named;
            for (std::size_t fi = 0; fi < files.size(); ++fi)
                for (const Symbol& s : files[fi]->decls)
                    if (isEnumerator(s))
                        named[s.name].emplace_back(fi, &s);
            // The pending constants in the order the compile reads them (a file's own in its order: the sort is stable).
            struct Item
            {
                std::size_t file = 0;
                const FileData::Pending* p = nullptr;
                int line = 0, col = 0;
            };
            std::vector<Item> items;
            for (std::size_t fi = 0; fi < files.size(); ++fi)
                for (const FileData::Pending& p : files[fi]->pending)
                {
                    const Pos at = files[fi]->decls[static_cast<std::size_t>(p.decl)].at;
                    items.push_back({ fi, &p, order.line(static_cast<int>(fi), at.line), at.col });
                }
            std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
                return a.line != b.line ? a.line < b.line : a.col < b.col;
            });
            for (int pass = 0; pass < kFoldPasses; ++pass)
            {
                bool more = false;
                for (const Item& item : items)
                {
                    const std::size_t fi = item.file;
                    const FileData::Pending& p = *item.p;
                    FileData* f = files[fi];
                    Symbol& c = f->decls[static_cast<std::size_t>(p.decl)];
                    if (c.value)
                        continue;
                    std::optional<int64_t> v;
                    if (p.from >= 0)
                    {
                        const Pos at = startOf(f->code[static_cast<std::size_t>(p.from)]);
                        std::string module = c.container;
                        if (module.empty() && isScoped(c))
                            for (const Body& b : f->bodies)
                                if (startOf(f->code[static_cast<std::size_t>(b.open)]) < at
                                    && at <= startOf(f->code[static_cast<std::size_t>(b.close)]))
                                    module = b.module;     // a block's in a method sees its module's
                        const Folder::Lookup lookup = [&](std::string_view name) -> std::optional<int64_t> {
                            const Symbol* s = constantAt(named, order, fi, at, module, reads[fi], &c, name);
                            return s != nullptr ? s->value : std::nullopt;
                        };
                        v = Folder(f->code, p.from, p.to, lookup).run();
                    }
                    else if (p.before < 0)
                        v = 0;
                    else if (const std::optional<int64_t> b = f->decls[static_cast<std::size_t>(p.before)].value;
                             b && *b < std::numeric_limits<int32_t>::max())
                        v = *b + 1;
                    if (v)
                    {
                        c.value = v;
                        more = true;
                    }
                }
                if (!more)
                    break;
            }
            for (FileData* f : files)
                for (const FileData::Pending& p : f->pending)
                    if (Symbol& c = f->decls[static_cast<std::size_t>(p.decl)]; c.value)
                        c.signature = c.name + " = " + std::to_string(*c.value);
        }

        // ---- a declaration fitted to the hover card (Lang.h's kHoverWidth, kHoverLines) ---------------------------------
        // Each line of `code` in lines of at most `width` columns, at most `most` lines in all. A longer line is broken at
        // its cuts: after a comma inside a bracket (", "), before an attribute's `[[` outside them; never in a string.
        // The outermost cuts are tried first (an attribute before its list's commas, a function's parameters before a
        // parameter's own brackets), deeper ones only when those leave a line too long; the lines after the first start
        // under the bracket the first cut is in (an attribute's: under the first `[[`; four columns in when that is past
        // half the width), each filled with what fits. A piece no line holds is cut with " ...", and so is the last
        // line when more would follow it.
        std::string fitCode(std::string_view code, std::size_t width, std::size_t most)
        {
            struct Cut
            {
                std::size_t at = 0;                      // the piece after it starts here
                int depth = 0;                           // the brackets open there
                std::size_t under = 0;                   // the column the lines after the first start at
            };
            const auto rtrim = [](std::string x) {
                while (!x.empty() && x.back() == ' ')
                    x.pop_back();
                return x;
            };
            const auto cutShort = [&](std::string x) {
                x = rtrim(std::move(x));
                if (x.size() + 4 > width)
                    x = rtrim(std::string(headOf(x, width > 4 ? width - 4 : 0)));
                return x + " ...";
            };
            std::vector<std::string> out;
            bool cutOff = false;
            for (const std::string_view line : splitLines(code))
            {
                if (cutOff)
                    break;
                if (line.size() <= width)
                {
                    out.emplace_back(line);
                    continue;
                }
                std::vector<Cut> cuts;
                std::vector<std::size_t> open;
                const std::size_t attribute = line.find("[[");
                bool quoted = false;
                for (std::size_t i = 0; i < line.size(); ++i)
                {
                    const char c = line[i];
                    if (quoted)
                    {
                        if (c == '\\')
                            ++i;
                        else if (c == '"')
                            quoted = false;
                        continue;
                    }
                    if (c == '"')
                        quoted = true;
                    else if (c == '(' || c == '[' || c == '{')
                        open.push_back(i);
                    else if ((c == ')' || c == ']' || c == '}') && !open.empty())
                        open.pop_back();
                    else if (c == ',' && !open.empty() && i + 1 < line.size() && line[i + 1] == ' ')
                        cuts.push_back({ i + 2, static_cast<int>(open.size()), open.back() + 1 });
                    else if (c == ' ' && open.empty() && line.substr(i + 1, 2) == "[[")
                        cuts.push_back({ i + 1, 0, attribute });
                }
                // The lines the cuts of at most `depth` give, each filled with what fits.
                const auto fill = [&](int depth) {
                    std::vector<std::string> lines;
                    std::size_t under = std::string::npos, from = 0;
                    std::string cur;
                    for (std::size_t k = 0; k <= cuts.size(); ++k)
                    {
                        if (k < cuts.size() && cuts[k].depth > depth)
                            continue;
                        const std::size_t to = k < cuts.size() ? cuts[k].at : line.size();
                        const std::string piece(line.substr(from, to - from));
                        if (from == 0)
                            cur = piece;
                        else if (rtrim(cur + piece).size() <= width)
                            cur += piece;
                        else
                        {
                            lines.push_back(rtrim(cur));
                            cur = std::string(under, ' ') + piece;
                        }
                        if (under == std::string::npos && k < cuts.size())
                            under = cuts[k].under > width / 2 ? 4 : cuts[k].under;
                        from = to;
                    }
                    lines.push_back(rtrim(cur));
                    return lines;
                };
                int lo = std::numeric_limits<int>::max(), hi = -1;
                for (const Cut& k : cuts)
                {
                    lo = std::min(lo, k.depth);
                    hi = std::max(hi, k.depth);
                }
                std::vector<std::string> lines;
                for (int depth = lo; depth <= hi && lines.empty(); ++depth)
                {
                    std::vector<std::string> tried = fill(depth);
                    if (std::all_of(tried.begin(), tried.end(), [&](const std::string& l) { return l.size() <= width; }))
                        lines = std::move(tried);
                }
                if (lines.empty())
                    lines = hi >= 0 ? fill(hi) : std::vector<std::string>{ std::string(line) };
                for (std::string& l : lines)
                {
                    if (l.size() > width)
                    {
                        out.push_back(cutShort(std::move(l)));
                        cutOff = true;
                        break;
                    }
                    out.push_back(std::move(l));
                }
            }
            if (out.size() > most)
            {
                out.resize(most);
                if (!out.empty())
                    out.back() = cutShort(std::move(out.back()));
            }
            std::string joined;
            for (std::size_t k = 0; k < out.size(); ++k)
                joined += (k == 0 ? "" : "\n") + out[k];
            return joined;
        }
    }

    std::string memberDoc(const std::string& moduleDoc, std::string_view member)
    {
        if (member.empty())
            return {};                         // a name of nothing names no line (and finding "" never moves on)
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

    namespace
    {
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

    std::string stringValue(std::string_view literal)
    {
        std::string_view s = literal;
        if (!s.empty() && s.front() == '"')
            s.remove_prefix(1);
        std::string out;
        const auto utf8 = [&](uint32_t cp) {
            if (cp < 0x80)
                out += static_cast<char>(cp);
            else if (cp < 0x800)
            {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            else if (cp < 0x10000)
            {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            else
            {
                out += static_cast<char>(0xF0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        };
        for (std::size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '"')
                break;                                    // its closing quote
            if (s[i] != '\\' || i + 1 >= s.size())
            {
                out += s[i];
                continue;
            }
            const char e = s[++i];
            switch (e)
            {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'a': out += '\a'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'v': out += '\v'; break;
                case 'x':
                case 'u':
                case 'U':
                {
                    // \xHH... (a byte), \uXXXX and \UXXXXXXXX (a character, as UTF-8)
                    const std::size_t most = e == 'x' ? s.size() : i + 1 + (e == 'u' ? 4u : 8u);
                    std::size_t k = i + 1;
                    uint32_t v = 0;
                    while (k < s.size() && k < most && isHexDigit(s[k]))
                    {
                        v = v * 16 + static_cast<uint32_t>(isDigit(s[k]) ? s[k] - '0' : (std::tolower(static_cast<unsigned char>(s[k])) - 'a' + 10));
                        ++k;
                    }
                    if (k == i + 1 || (e != 'x' && k != most))
                    {
                        out += '\\';                    // not one: kept as written
                        out += e;
                        break;
                    }
                    if (e == 'x')
                        out += static_cast<char>(v & 0xFF);
                    else
                        utf8(v);
                    i = k - 1;
                    break;
                }
                default:
                    if (e >= '0' && e <= '7')
                    {
                        uint32_t v = 0;
                        std::size_t k = i;
                        while (k < s.size() && k < i + 3 && s[k] >= '0' && s[k] <= '7')
                            v = v * 8 + static_cast<uint32_t>(s[k++] - '0');
                        out += static_cast<char>(v & 0xFF);
                        i = k - 1;
                    }
                    else
                        out += e;                         // \" \\ \' \?: the character
                    break;
            }
        }
        return out;
    }
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

    std::string_view headOf(std::string_view text, std::size_t bytes) noexcept
    {
        if (text.size() <= bytes)
            return text;
        // Back to a character's first byte: the bytes kept end where one ends (a continuation byte is 10xxxxxx).
        while (bytes > 0 && (static_cast<unsigned char>(text[bytes]) & 0xC0u) == 0x80u)
            --bytes;
        return text.substr(0, bytes);
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
            if (c == '"')
            {
                // C's string literal, to its closing quote (a backslash escapes the next character), or to the line's
                // end, where FMinus says it is missing.
                int j = i + 1;
                while (j < n && at(j) != '"')
                    j += at(j) == '\\' && j + 1 < n ? 2 : 1;
                j = std::min(j + 1, n);
                push(Tok::string, i, j, false);
                i = j;
                continue;
            }
            if (isIdentStart(c))
            {
                int j = i;
                while (j < n && isIdentChar(at(j)))
                    ++j;
                const std::string_view w = s.substr(static_cast<std::size_t>(i), static_cast<std::size_t>(j - i));
                // After `::` a keyword is a name, as C23 reads one in an attribute: fminus::default.
                const bool scoped = !out.empty() && out.back().line == ln && out.back().text == "::";
                push(scoped ? Tok::identifier : isKeyword(w) ? Tok::keyword : isTypeWord(w) ? Tok::type : Tok::identifier, i, j,
                     false);
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
            // The longest operator first: <<= and >>= before << and >>, which come before < and >.
            static constexpr std::string_view kThree[] = { "<<=", ">>=" };
            static constexpr std::string_view kTwo[] = { "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<",
                                                         ">>", "->", "<=", ">=", "==", "!=", "&&", "||", "::" };
            bool two = false;
            for (const std::string_view op : kThree)
                if (at(i) == op[0] && at(i + 1) == op[1] && at(i + 2) == op[2])
                {
                    push(Tok::op, i, i + 3, false);
                    i += 3;
                    two = true;
                    break;
                }
            if (two)
                continue;
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
            if (std::strchr("+-*/%=<>!&|^~?:", c) != nullptr)
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

    // ---- a netlist's tokens -------------------------------------------------------------------------------------------
    namespace
    {
        bool isLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

        // The length of the SPICE value at the start of `w` ([+-]digits[.digits][e[+-]digits][suffix][letters]: 2.2k,
        // 10meg, 1u, 4.5V, -60dB; a sign only where `sign` allows one), or 0 when none starts there or what follows
        // its letters makes it a name (1N914, 2N3904: digits again).
        std::size_t spiceValue(std::string_view w, bool sign)
        {
            std::size_t k = 0;
            const auto at = [&](std::size_t i) { return i < w.size() ? w[i] : '\0'; };
            if (sign && (at(k) == '+' || at(k) == '-'))
                ++k;
            int digits = 0;
            while (isDigit(at(k)))
            {
                ++k;
                ++digits;
            }
            if (at(k) == '.')
            {
                ++k;
                while (isDigit(at(k)))
                {
                    ++k;
                    ++digits;
                }
            }
            if (digits == 0)
                return 0;
            if ((at(k) == 'e' || at(k) == 'E')
                && (isDigit(at(k + 1)) || ((at(k + 1) == '+' || at(k + 1) == '-') && isDigit(at(k + 2)))))
            {
                k += isDigit(at(k + 1)) ? 1 : 2;
                while (isDigit(at(k)))
                    ++k;
            }
            while (isLetter(at(k)))
                ++k;
            if (isDigit(at(k)) || at(k) == '_')
                return 0;
            return k;
        }
    }

    void lexCirLine(std::string_view s, int ln, std::vector<Token>& out)
    {
        const int n = static_cast<int>(s.size());
        const auto push = [&](Tok k, int from, int to) {
            Token t;
            t.kind = k;
            t.line = ln;
            t.col = from;
            t.len = to - from;
            t.text = s.substr(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from));
            out.push_back(t);
        };
        const auto at = [&](int k) { return k < n ? s[static_cast<std::size_t>(k)] : '\0'; };
        const auto lastNonBlank = [&]() {
            int last = n;
            while (last > 0 && isSpaceChar(at(last - 1)))
                --last;
            return last;
        };
        int i = 0;
        while (i < n && isSpaceChar(at(i)))
            ++i;
        if (i >= n)
            return;
        if (at(i) == '*')
        {
            push(Tok::comment, i, lastNonBlank());       // a comment line
            return;
        }
        bool first = true;                                // the line's first word: an element's name, or a directive
        if (at(i) == '+')
        {
            push(Tok::punct, i, i + 1);                   // a continuation of the line before
            ++i;
            first = false;
        }
        int braces = 0;                                   // inside { }: an expression of pots
        constexpr std::string_view kDelimiters = ";{}()=,:";
        while (i < n)
        {
            const char c = at(i);
            if (isSpaceChar(c))
            {
                ++i;
                continue;
            }
            if (c == ';')
            {
                push(Tok::comment, i, lastNonBlank());
                return;
            }
            if (kDelimiters.find(c) != std::string_view::npos)
            {
                push(Tok::punct, i, i + 1);
                braces += c == '{' ? 1 : c == '}' && braces > 0 ? -1 : 0;
                ++i;
                first = false;
                continue;
            }
            if (braces > 0)
            {
                if (c == '+' || c == '-' || c == '*' || c == '/' || c == '^')
                {
                    push(Tok::op, i, i + 1);
                    ++i;
                    continue;
                }
                if (isDigit(c) || (c == '.' && isDigit(at(i + 1))))
                {
                    const std::size_t len = spiceValue(s.substr(static_cast<std::size_t>(i)), false);
                    if (len > 0)
                    {
                        push(Tok::number, i, i + static_cast<int>(len));
                        i += static_cast<int>(len);
                        continue;
                    }
                }
                if (isIdentStart(c))
                {
                    int j = i;
                    while (j < n && isIdentChar(at(j)))
                        ++j;
                    push(Tok::identifier, i, j);
                    i = j;
                    continue;
                }
                push(Tok::unknown, i, i + 1);
                ++i;
                continue;
            }
            // A word, up to a blank or a delimiter: an element's name, a directive, a value or a name.
            int j = i;
            while (j < n && !isSpaceChar(at(j)) && kDelimiters.find(at(j)) == std::string_view::npos)
                ++j;
            const std::string_view w = s.substr(static_cast<std::size_t>(i), static_cast<std::size_t>(j - i));
            Tok k = Tok::identifier;
            if (first && w.size() > 1 && w[0] == '.' && isLetter(w[1]))
                k = Tok::directive;
            else if (first && isLetter(w[0]))
                k = Tok::keyword;
            else if (spiceValue(w, true) == w.size())
                k = Tok::number;
            push(k, i, j);
            first = false;
            i = j;
        }
    }

    std::optional<double> spiceNumber(std::string_view text)
    {
        return readSpiceNumber(text);
    }

    std::vector<Token> lexCir(std::string_view text)
    {
        std::vector<Token> out;
        int ln = 0;
        for (const std::string_view l : splitLines(text))
            lexCirLine(l, ln++, out);
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
            case Kind::constant:  return "constant";
            case Kind::enumeration: return "enum";
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
        std::vector<std::string> typeNames, constantNames, shadowNames;
        ReadOrder order;                                  // the files' lines as the compile reads them

        void rebuild()
        {
            view.clear();
            symbols.clear();
            std::set<std::string> types, constants, others;
            for (const auto& f : files)
            {
                view.push_back({ f->name, f->text, f->program });
                for (const Symbol& s : f->decls)
                {
                    symbols.push_back(s);
                    if (s.kind == Kind::module)
                        types.insert(s.name);
                    else if (s.kind == Kind::constant)
                        constants.insert(s.name);
                    else if (s.kind == Kind::local || s.kind == Kind::argument || s.kind == Kind::field || s.kind == Kind::param)
                        others.insert(s.name);
                }
            }
            typeNames.assign(types.begin(), types.end());
            constantNames.assign(constants.begin(), constants.end());
            shadowNames.clear();
            std::set_intersection(constants.begin(), constants.end(), others.begin(), others.end(), std::back_inserter(shadowNames));
        }

        static bool scoped(const Symbol& s) { return !(s.scope.from == s.scope.to); }

        // The files as Sources (the conditionals' and the macros' input).
        Sources sources() const
        {
            Sources out;
            for (const auto& f : files)
                out.push_back({ f->name, f->text });
            return out;
        }

        // Every file indexed again: their conditionals first (a skipped group is no part of the index), then each
        // file, the constants' values, and the lists.
        void reindex()
        {
            const std::vector<FileConditionals> cond = conditionals(sources(), options.defines);
            std::vector<FileData*> all;
            for (std::size_t i = 0; i < files.size(); ++i)
            {
                files[i]->cond = i < cond.size() ? cond[i] : FileConditionals{};
                indexFile(*files[i], static_cast<int>(i));
                all.push_back(files[i].get());
            }
            order = ReadOrder(all);
            foldValues(all, order);
            rebuild();
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

        // ---- the conditionals' lines --------------------------------------------------------------------------------
        // A conditional's, a #define's or an #undef's line at `at` (a directive the lexer reads as one, outside a block
        // comment): the directive's name, where its # and its name end, and the word at `at` past the name (a macro, or
        // `defined`), outside the line's comments. False for any other place.
        struct DirectiveWord
        {
            std::string_view name;               // "ifdef"
            int hash = 0, nameEnd = 0;           // the # and the end of the name
            int from = 0, to = 0;                // the word at the place ([from, to), empty when none)
            bool first = false;                  // the first word after the name (a #define's macro)
        };

        static std::optional<DirectiveWord> directiveWordAt(const FileData& f, Pos at)
        {
            if (at.line < 0 || at.line >= static_cast<int>(f.lines.size()))
                return std::nullopt;
            bool directive = false;
            for (const Token& t : f.all)
                directive = directive || (t.line == at.line && t.kind == Tok::directive);
            if (!directive)
                return std::nullopt;
            const std::string_view line = f.lines[static_cast<std::size_t>(at.line)];
            std::size_t rest = 0;
            DirectiveWord w;
            w.name = directiveName(line, &rest);
            if (!isConditional(w.name) && w.name != "define" && w.name != "undef")
                return std::nullopt;
            w.hash = static_cast<int>(line.find('#'));
            w.nameEnd = static_cast<int>(rest);
            const int n = static_cast<int>(line.size());
            int comment = n;                             // the line's first comment
            for (std::size_t k = rest; k + 1 < line.size(); ++k)
                if (line[k] == '/' && (line[k + 1] == '/' || line[k + 1] == '*'))
                {
                    comment = static_cast<int>(k);
                    break;
                }
            int a = std::clamp(at.col, 0, n), b = a;
            while (a > w.nameEnd && isIdentChar(line[static_cast<std::size_t>(a - 1)]))
                --a;
            while (b < comment && isIdentChar(line[static_cast<std::size_t>(b)]))
                ++b;
            if (a < b && (a >= comment || isDigit(line[static_cast<std::size_t>(a)])))
                a = b;                                   // in a comment, or a number
            w.from = a;
            w.to = b;
            int first = w.nameEnd;
            while (first < n && (line[static_cast<std::size_t>(first)] == ' ' || line[static_cast<std::size_t>(first)] == '\t'))
                ++first;
            w.first = a == first;
            return w;
        }

        // The macro the word of `w` names on line `at.line` of `file`, as the compile sees it there.
        std::optional<Macro> macroOf(int file, Pos at, const DirectiveWord& w) const
        {
            const FileData* f = fileAt(file);
            if (f == nullptr || w.from >= w.to)
                return std::nullopt;
            const std::string_view word = f->lines[static_cast<std::size_t>(at.line)].substr(static_cast<std::size_t>(w.from),
                                                                                              static_cast<std::size_t>(w.to - w.from));
            return macroAt(sources(), options.defines, file, at.line, word);
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

        // A local, an argument or a block's constant named `name` seen at p (the innermost: declared last before p);
        // with `tag`, a block's enumeration of that tag (C's tags are a name space of their own). An argument's own
        // name in its function's parentheses (its scope is the body) is that argument.
        const Symbol* localNamed(const FileData& f, std::string_view name, Pos p, bool tag = false) const
        {
            const Symbol* best = nullptr;
            for (const Symbol& s : f.decls)
            {
                const bool kind = tag ? s.kind == Kind::enumeration && scoped(s)
                                      : s.kind == Kind::local || s.kind == Kind::argument || (s.kind == Kind::constant && scoped(s));
                if (kind && s.name == name && ((s.scope.from <= p && p <= s.scope.to) || s.at == p)
                    && (best == nullptr || best->at < s.at))
                    best = &s;
            }
            return best;
        }

        // A module's own constant (an enumeration's, seen by its methods) named `name`, or with `tag` its enumeration
        // (its list's, before a tag C23 declared ahead of it).
        const Symbol* moduleConstant(std::string_view module, std::string_view name, bool tag) const
        {
            const Symbol* best = nullptr;
            for (const auto* set : { &files, &library })
                for (const auto& f : *set)
                    for (const Symbol& s : f->decls)
                        if (s.kind == (tag ? Kind::enumeration : Kind::constant) && s.container == module && !scoped(s)
                            && s.name == name && (best == nullptr || (!best->body && s.body)))
                            best = &s;
            return best;
        }

        // What the file's scope holds, in the files a file sees: a function or a module named `name` (a definition
        // before a prototype), a constant (an enumeration's, a const of the top level), or with `tag` an enumeration
        // (its list's before a tag declared ahead of it); else a module's enumeration or its constant, which C's file
        // scope holds too (a struct's are the file's), from its place on: with `at`, only one before that place of
        // `file` in the order the compile reads (ReadOrder).
        const Symbol* globalNamed(int file, std::string_view name, bool tag = false, const Pos* at = nullptr) const
        {
            const auto top = [tag](const Symbol& s, std::size_t) {
                if (tag)
                    return s.kind == Kind::enumeration && s.container.empty() && !scoped(s);
                return s.kind == Kind::function || s.kind == Kind::module
                    || (s.kind == Kind::constant && s.container.empty() && !scoped(s));
            };
            const auto ofModule = [&](const Symbol& s, std::size_t fi) {
                return s.kind == (tag ? Kind::enumeration : Kind::constant) && !s.container.empty() && !scoped(s)
                    && (at == nullptr || order.before(static_cast<int>(fi), s.at, file, *at));
            };
            const auto find = [&](const auto& wanted) {
                const Symbol* best = nullptr;
                for (const int fi : visible(file))
                    for (const Symbol& s : files[static_cast<std::size_t>(fi)]->decls)
                        if (s.name == name && wanted(s, static_cast<std::size_t>(fi)) && (best == nullptr || (!best->body && s.body)))
                            best = &s;
                if (best == nullptr)
                    for (std::size_t fi = 0; fi < files.size(); ++fi)
                        for (const Symbol& s : files[fi]->decls)
                            if (s.name == name && wanted(s, fi) && (best == nullptr || (!best->body && s.body)))
                                best = &s;
                return best;
            };
            const Symbol* best = find(top);
            return best != nullptr ? best : find(ofModule);
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
            const bool tag = isText(*f, i - 1, "enum");      // `enum Wave`: the tag, never a variable
            if (const Symbol* s = localNamed(*f, t.text, p, tag))
                return s;
            if (const std::string m = moduleAt(*f, p); !m.empty())
            {
                if (!tag)
                    if (const Symbol* s = memberNamed(m, t.text))
                        return s;
                if (const Symbol* s = moduleConstant(m, t.text, tag))
                    return s;
            }
            return globalNamed(file, t.text, tag, &p);       // another module's: before p
        }
    };

    Service::Service() : impl_(new Impl) {}
    Service::~Service() { delete impl_; }

    void Service::setOptions(const Options& o)
    {
        const bool other = o.defines != impl_->options.defines;
        impl_->options = o;
        if (other && !impl_->files.empty())
            impl_->reindex();                            // other groups skipped
    }

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
        // A file's index is part of its symbols, and a file's #define reaches the files after it: index every file
        // again when any changed (they are small).
        if (changed)
            m.reindex();
    }

    void Service::setLibrary(const Sources& library)
    {
        impl_->library.clear();
        for (const SourceFile& s : library)
        {
            auto f = std::make_unique<FileData>();
            f->name = s.name;
            f->text = s.text;
            if (!isCircuitFile(s.name))                  // each as a compile of it alone reads it
                f->cond = conditionals({ s }, impl_->options.defines).front();
            indexFile(*f, -1);
            impl_->library.push_back(std::move(f));
        }
        std::vector<FileData*> all;
        for (const auto& f : impl_->library)
            all.push_back(f.get());
        foldValues(all, ReadOrder(all));
    }

    const std::vector<Service::File>& Service::files() const noexcept { return impl_->view; }
    const FileConditionals& Service::fileConditionals(int file) const noexcept
    {
        static const FileConditionals none;
        const FileData* f = impl_->fileAt(file);
        return f != nullptr ? f->cond : none;
    }
    int Service::fileIndex(std::string_view name) const noexcept { return impl_->indexOf(name); }
    const std::vector<Symbol>& Service::symbols() const noexcept { return impl_->symbols; }
    const std::vector<std::string>& Service::typeNames() const noexcept { return impl_->typeNames; }
    const std::vector<std::string>& Service::constantNames() const noexcept { return impl_->constantNames; }
    const std::vector<std::string>& Service::shadowNames() const noexcept { return impl_->shadowNames; }

    std::vector<uint8_t> Service::shadowed(int file) const
    {
        const Impl& m = *impl_;
        std::vector<uint8_t> out;
        const FileData* f = m.fileAt(file);
        if (f == nullptr || m.shadowNames.empty())
            return out;
        for (std::size_t i = 0; i < f->code.size(); ++i)
        {
            const Token& t = f->code[i];
            if (t.kind != Tok::identifier
                || !std::binary_search(m.shadowNames.begin(), m.shadowNames.end(), t.text, std::less<>{}))
                continue;
            const Symbol* s = m.resolve(file, static_cast<int>(i));
            out.push_back(s != nullptr && s->kind != Kind::constant ? 1 : 0);
        }
        return out;
    }

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
                for (const WordDoc& d : kDirectiveDocs)
                    offer(std::string(d.word), Kind::word, std::string(d.code), std::string(d.doc), {}, 1);
                return finish();
            }
            // A condition's or an #undef's macro: the plugin's and the program's (#if's `defined` too).
            {
                std::size_t k = 0;
                while (k < afterHash.size() && isIdentChar(afterHash[k]))
                    ++k;
                const std::string dname = afterHash.substr(0, k);
                if (isConditional(dname) || dname == "undef")
                {
                    if (dname == "else" || dname == "endif")
                        return finish();
                    for (const std::string& macro : macroNames(m.sources(), m.options.defines))
                    {
                        std::string code = "#define " + macro;
                        for (const auto& [n, v] : m.options.defines)
                            if (n == macro && !v.empty())
                                code += " " + v;
                        offer(macro, Kind::constant, code, {}, {}, 0);
                    }
                    if (dname == "if" || dname == "elif")
                        offer("defined", Kind::keyword, "defined(NAME)", "1 when NAME is a macro, else 0.", "defined($0)", 1);
                    return finish();
                }
                if (dname == "define")
                    return finish();
            }
            if (afterHash == "pragma")
            {
                offer("panel", Kind::word, "#pragma panel ...", "Lays out the dials and displays.", {}, 0);
                if (m.options.instrument)
                    offer("mono", Kind::word, "#pragma mono [dial]", "One voice, the newest key held: a note while it "
                                                                     "sounds calls Voice's legato(float note, float "
                                                                     "velocity). A bool dial named after it switches it.",
                          {}, 0);
                return finish();
            }
            // #pragma mono's dial (Patch.h, "Mono"): the main module's bool and int params.
            if (m.options.instrument && afterHash.rfind("pragma mono", 0) == 0 && trimmed(afterHash.substr(11)).empty())
            {
                for (const auto& g : m.files)
                    for (const Symbol& s : g->decls)
                        if (s.kind == Kind::param && s.container == "Voice" && (s.type == "bool" || s.type == "int"))
                            offer(s.name, Kind::param, s.signature, s.doc, {}, 1);
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
                // A display's source (meter, vu, led, number, scope): in and out, the main module's float, int and
                // bool fields, and Shared's through the main module's Shared* field ("shared->level"; after "shared->",
                // Shared's own names).
                const std::string word = rest.substr(0, rest.find(' '));
                const bool display = word == "meter" || word == "vu" || word == "led" || word == "number" || word == "scope";
                if (display)
                {
                    const auto scalar = [](const Symbol& s) { return s.type == "float" || s.type == "double" || s.type == "int" || s.type == "bool"; };
                    const auto squeezed = [](std::string t) {
                        t.erase(std::remove(t.begin(), t.end(), ' '), t.end());
                        return t;
                    };
                    std::string via;                     // the main module's first Shared* field
                    for (const auto& g : m.files)
                        for (const Symbol& s : g->decls)
                            if (via.empty() && s.kind == Kind::field && (s.container == "Effect" || s.container == "Voice")
                                && squeezed(s.type) == "Shared*")
                                via = s.name;
                    const bool afterArrow = !via.empty() && trimmed(rest.substr(word.size())) == via + "->";
                    if (rest != word && !afterArrow)
                        return finish();                 // past the source: a range, as, a "label"
                    for (const auto& g : m.files)
                        for (const Symbol& s : g->decls)
                        {
                            if (s.kind != Kind::field || !scalar(s))
                                continue;
                            if (!afterArrow && (s.container == "Effect" || s.container == "Voice"))
                                offer(s.name, Kind::field, s.signature, s.doc, {}, 1);
                            else if (s.container == "Shared" && !via.empty())
                                offer(afterArrow ? s.name : via + "->" + s.name, Kind::field, s.signature, s.doc, {}, 2);
                        }
                    if (!afterArrow)
                    {
                        offer("out", Kind::word, "the output", "The plugin's output, left and right, in dBFS.", {}, 0);
                        if (!m.options.instrument)
                            offer("in", Kind::word, "the input", "The effect's input, left and right, in dBFS.", {}, 0);
                    }
                    return finish();
                }
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

        const Pos p = out.replace.from;
        const std::string module = m.moduleAt(*f, p);
        // After `enum`: the enumerations' tags (C's tags are a name space of their own): a block's in scope, the
        // module's, the top level's and another module's before the place (the file's from there on, in C) of the files
        // the program reads; a tag's list before C23's declaration of it ahead of the list.
        if (const int b = Impl::lastBefore(*f, p); b >= 0 && Impl::isText(*f, b, "enum"))
        {
            for (const Symbol& s : f->decls)
                if (s.kind == Kind::enumeration && Impl::scoped(s) && s.scope.from < p && p <= s.scope.to)
                    offer(s.name, s.kind, s.signature, s.doc, {}, s.body ? 0 : 1);
            for (const int fi : m.visible(file))
                for (const Symbol& s : m.files[static_cast<std::size_t>(fi)]->decls)
                    if (s.kind == Kind::enumeration && !Impl::scoped(s)
                        && (s.container.empty() || s.container == module || m.order.before(fi, s.at, file, p)))
                        offer(s.name, s.kind, s.signature, s.doc, {},
                              (s.container == module && !module.empty() ? 1 : 2) + (s.body ? 0 : 2));
            return finish();
        }

        // Everything a name may be here.
        for (const Symbol& s : f->decls)
            if ((s.kind == Kind::local || s.kind == Kind::argument || (s.kind == Kind::constant && Impl::scoped(s)))
                && s.scope.from < p && p <= s.scope.to && s.at < p)
            {
                const std::size_t before = items.size();
                offer(s.name, s.kind, s.kind == Kind::constant ? s.signature : s.type, s.doc, {}, 0);
                withCode(before, s.signature);
            }
        const Symbol* function = m.functionAt(*f, p);
        if (!module.empty())
        {
            for (const Symbol* s : m.membersOf(module))
                if (!(s->at == p))
                {
                    const std::size_t before = items.size();
                    offer(s->name, s->kind, s->kind == Kind::method ? s->signature : s->type, s->doc,
                          s->kind == Kind::method ? s->name + (s->params.empty() ? "()" : "($0)") : std::string(), 1);
                    withCode(before, s->signature);
                }
            // the module's own enumerations' constants
            for (const auto& g : m.files)
                for (const Symbol& s : g->decls)
                    if (s.kind == Kind::constant && s.container == module && !Impl::scoped(s) && !(s.at == p))
                    {
                        const std::size_t before = items.size();
                        offer(s.name, s.kind, s.signature, s.doc, {}, 1);
                        withCode(before, s.signature);
                    }
        }
        std::set<std::string> modulesSeen;
        // The file's scope: functions, modules, the top level's constants and other modules' enumerations' constants
        // (the file's from their place on: those before this one).
        for (const int fi : m.visible(file))
            for (const Symbol& s : m.files[static_cast<std::size_t>(fi)]->decls)
                if ((s.kind == Kind::function || s.kind == Kind::module
                     || (s.kind == Kind::constant && (s.container.empty() || s.container != module) && !Impl::scoped(s)
                         && (s.container.empty() || m.order.before(fi, s.at, file, p))))
                    && !(fi == file && s.at == p))
                {
                    if (s.kind == Kind::module)
                        modulesSeen.insert(s.name);
                    const std::size_t before = items.size();
                    offer(s.name, s.kind, s.kind == Kind::module ? std::string("module") : s.signature,
                          s.doc.size() > 600 ? s.doc.substr(0, 600) + "..." : s.doc,
                          s.kind == Kind::function ? s.name + (s.params.empty() ? "()" : "($0)") : std::string(), 2);
                    if (s.kind == Kind::constant)
                        withCode(before, s.signature);
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
        std::optional<Hover> h = hoverOf(file, at);
        if (h)
            h->code = fitCode(h->code, static_cast<std::size_t>(kHoverWidth), static_cast<std::size_t>(kHoverLines));
        return h;
    }

    std::optional<Hover> Service::hoverOf(int file, Pos at) const
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
        // A conditional's line, a #define's or an #undef's: the directive, `defined`, a macro (the plugin's, the
        // program's as that line sees it, or none).
        if (const std::optional<Impl::DirectiveWord> w = Impl::directiveWordAt(*f, at))
        {
            const std::string_view line = f->lines[static_cast<std::size_t>(at.line)];
            Hover h;
            if (at.col >= w->hash && at.col <= w->nameEnd)
            {
                const WordDoc* d = directiveDoc(w->name);
                if (d == nullptr)
                    return std::nullopt;
                h.range = { { at.line, w->hash }, { at.line, w->nameEnd } };
                h.code = std::string(d->code);
                h.doc = std::string(d->doc);
                return h;
            }
            if (w->from >= w->to)
                return std::nullopt;
            const std::string word(line.substr(static_cast<std::size_t>(w->from), static_cast<std::size_t>(w->to - w->from)));
            h.range = { { at.line, w->from }, { at.line, w->to } };
            if (word == "defined" && w->name != "define" && w->name != "undef")
            {
                h.code = "defined(NAME)";
                h.doc = "1 when NAME is a macro here, else 0: #if defined(DEBUG) && LEVEL >= 2.";
                return h;
            }
            if (w->name == "define" && w->first)
            {
                h.code = trimmed(line.substr(static_cast<std::size_t>(w->hash)));
                h.doc = std::string(directiveDoc("define")->doc);
                if (const std::string above = commentsAbove(*f, at.line); !above.empty())
                    h.doc = above + "\n\n" + h.doc;
                return h;
            }
            if (const std::optional<Macro> mac = m.macroOf(file, at, *w))
            {
                h.code = "#define " + mac->name + (mac->value.empty() ? "" : " " + mac->value);
                if (mac->file < 0)
                {
                    for (const Define& d : kDefines)
                        if (mac->name == d.name)
                            h.doc = d.doc;
                    if (h.doc.empty())
                        h.doc = "Defined by the plugin.";
                }
                else if (const FileData* g = m.fileAt(mac->file))
                {
                    h.doc = g->name + ":" + std::to_string(mac->line + 1);
                    if (const std::string above = commentsAbove(*g, mac->line); !above.empty())
                        h.doc = above + "\n\n" + h.doc;
                }
                return h;
            }
            h.code = word;
            h.doc = "No macro here: #ifdef skips its group, #ifndef reads it, and #if reads the name as 0.";
            return h;
        }
        const int i = Impl::tokenAt(*f, at);
        if (i < 0)
            return std::nullopt;
        const Token& t = f->code[static_cast<std::size_t>(i)];
        Hover h;
        h.range = { startOf(t), endOf(t) };
        // An attribute's name inside [[ ]] (`ui::unit`, `fminus::range`): what it does, whatever else has the word.
        if (t.kind == Tok::identifier)
        {
            const auto text = [&](int k) {
                return k >= 0 && k < static_cast<int>(f->code.size()) ? f->code[static_cast<std::size_t>(k)].text : std::string_view{};
            };
            bool inside = false;
            for (int k = i - 1; k >= 0 && k >= i - 256 && !inside; --k)
                if (text(k) == "[" && text(k + 1) == "[" && f->match[static_cast<std::size_t>(k)] > i)
                    inside = true;
            const bool before = text(i + 1) == "::", after = text(i - 1) == "::";
            const bool argument = text(i - 1) == "(" || text(i - 1) == ",";
            if (inside && !(argument && !before))
            {
                const std::string name = after ? std::string(text(i - 2)) + "::" + std::string(t.text)
                                       : before ? std::string(t.text) + "::" + std::string(text(i + 2)) : std::string(t.text);
                h.code = "[[" + name + "]]";
                for (const AttributeInfo& x : kAttributes)
                    if (x.label == name)
                        h.doc = std::string(x.doc);
                if (h.doc.empty())
                    h.doc = name.find("::") == std::string::npos
                                ? "An attribute without a namespace: FMinus ignores one that is none of C23's, with a warning "
                                  "(-Wunknown-attributes); the plugin's are ui::unit, ui::label, ui::decimals."
                                : "An attribute FMinus passes on as written. The plugin reads ui::unit, ui::label, ui::decimals "
                                  "and fminus::range on a field (its displays and trace); any other is the program's own.";
                return h;
            }
        }
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
                case Kind::constant:
                {
                    h.code = s->signature;
                    std::string what;
                    if (isEnumerator(*s))
                        what = (s->enumeration.empty() ? std::string("an enumeration constant, an int")
                                                       : "a constant of enum " + s->enumeration + ", an int")
                             + (s->container.empty() ? "" : " (in module " + s->container + ")");
                    else
                        what = "a constant of the program (" + s->type + "): stored once, read by every function and "
                               "module after it";
                    h.doc = what + (s->doc.empty() ? "" : "\n" + s->doc);
                    break;
                }
                case Kind::enumeration:
                    h.code = s->signature;
                    h.doc = "an enumeration: enum " + s->name + " is " + (s->type == "bool" ? "a bool" : "an int")
                          + (s->doc.empty() ? "" : "\n" + s->doc);
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
            // The names its code declares, a local's or a function's and its parameters': none is a constant there,
            // whatever constant has its name too (the card draws them plain, as the editor draws the uses).
            if (s->kind == Kind::local || s->kind == Kind::argument || s->kind == Kind::field || s->kind == Kind::param
                || s->kind == Kind::method || s->kind == Kind::function)
            {
                h.plain.push_back(s->name);
                std::vector<Token> parts;
                for (const std::string& p : s->params)
                {
                    parts.clear();
                    bool inner = false;
                    lexLine(p, 0, inner, parts);
                    std::string_view last;
                    for (const Token& x : parts)
                    {
                        if (x.text == "[")               // its extent's names are uses
                            break;
                        if (x.kind == Tok::identifier)
                            last = x.text;
                    }
                    if (!last.empty() && std::find(h.plain.begin(), h.plain.end(), last) == h.plain.end())
                        h.plain.emplace_back(last);
                }
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
        // A macro in a conditional's line: its #define (the program's; the plugin's has none to go to).
        if (const std::optional<Impl::DirectiveWord> w = Impl::directiveWordAt(*f, at))
        {
            if (w->from >= w->to || (w->name == "define" && w->first))
                return std::nullopt;
            const std::optional<Macro> mac = m.macroOf(file, at, *w);
            const FileData* g = mac ? m.fileAt(mac->file) : nullptr;
            if (g == nullptr)
                return std::nullopt;
            const std::string_view def = g->lines[static_cast<std::size_t>(mac->line)];
            std::size_t rest = 0;
            (void)directiveName(def, &rest);
            const std::size_t col = def.find(mac->name, rest);
            return Location{ mac->file, g->name, { mac->line, col == std::string_view::npos ? 0 : static_cast<int>(col) } };
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

    // ---- the circuits' dials ---------------------------------------------------------------------------------------
    int CircuitBinding::count() const noexcept
    {
        int n = 1;
        for (const int d : dims)
            n *= std::max(d, 0);
        return n;
    }

    bool CircuitBinding::dialled() const noexcept
    {
        return !own && std::any_of(slots.begin(), slots.end(), [](int s) { return s >= 0; });
    }

    std::string CircuitBinding::path(int k) const
    {
        std::string index;
        for (std::size_t d = dims.size(); d-- > 0;)
        {
            const int extent = std::max(dims[d], 1);
            index = "[" + std::to_string(k % extent) + "]" + index;
            k /= extent;
        }
        return field + index;
    }

    CircuitDials Service::circuitDials(bool instrument) const
    {
        const Impl& m = *impl_;
        CircuitDials out;
        const std::string main = instrument ? "Voice" : "Effect";
        // Only the files FMinus compiles: the program's first and what it #includes, in turn theirs (a tab no #include
        // reaches, a netlist or a module of the program's main name, makes no binding).
        const std::vector<int> reached = m.visible(0);
        const auto compiled = [&](int fi) { return std::find(reached.begin(), reached.end(), fi) != reached.end(); };
        // The modules the program's netlists generate (the first of a name: FMinus refuses a second), by name.
        std::vector<std::pair<std::string, const FileData*>> circuits;
        for (std::size_t fi = 0; fi < m.files.size(); ++fi)
            if (const auto& f = m.files[fi]; f->program && isCircuitFile(f->name) && compiled(static_cast<int>(fi)))
                for (const Symbol& x : f->decls)
                    if (x.kind == Kind::module
                        && std::none_of(circuits.begin(), circuits.end(), [&](const auto& c) { return c.first == x.name; }))
                        circuits.emplace_back(x.name, f.get());
        const auto circuitOf = [&](std::string_view name) -> const FileData* {
            for (const auto& [n, f] : circuits)
                if (n == name)
                    return f;
            return nullptr;
        };
        // The main module, in an FMinus file of the program: its params and its fields, in declaration order.
        std::vector<const Symbol*> params, fields;
        for (const Symbol& x : m.symbols)
        {
            const FileData* f = m.fileAt(x.file);
            if (f == nullptr || !f->program || isCircuitFile(f->name) || !compiled(x.file))
                continue;
            if (x.kind == Kind::module && x.name == main && out.module.empty())
            {
                out.module = main;
                out.file = f->name;
            }
            if (x.container != main)
                continue;
            if (x.kind == Kind::param)
                params.push_back(&x);
            else if (x.kind == Kind::field)
                fields.push_back(&x);
        }
        if (out.module.empty())
            return out;
        const auto order = [](const Symbol* a, const Symbol* b) { return a->file != b->file ? a->file < b->file : a->at < b->at; };
        std::stable_sort(params.begin(), params.end(), order);
        std::stable_sort(fields.begin(), fields.end(), order);
        for (const Symbol* p : params)
            out.params.push_back(p->name);
        for (const Symbol* x : fields)
        {
            if (x->type.find('*') != std::string::npos)
                continue;                                // a pointer holds no module
            std::string base = x->type.substr(0, x->type.find('['));
            base = trimmed(base);
            const FileData* netlist = circuitOf(base);
            if (netlist == nullptr)
                continue;
            CircuitBinding b;
            b.field = x->name;
            b.module = base;
            b.file = netlist->name;
            for (std::size_t at = x->type.find('['); at != std::string::npos; at = x->type.find('[', at + 1))
            {
                const std::size_t close = x->type.find(']', at);
                std::string extent;
                for (std::size_t k = at + 1; k < close && close != std::string::npos; ++k)
                    if (x->type[k] != ' ')
                        extent += x->type[k];
                // A number, or an expression of enumeration constants as FMinus folds it (`amp[CHANNELS]`): a name is the
                // main module's own constant, else the top level's, else another module's before the field (the file's
                // from there on in C, as FMinus has it), of a file the program compiles. What does not fold to 1 to 2^30
                // is 0 (unknown), and Patch then gives the binding no dials.
                const std::vector<Token> toks = lex(extent);
                const Folder::Lookup lookup = [&](std::string_view name) -> std::optional<int64_t> {
                    for (int where = 0; where < 3; ++where)   // the main module's, the top level's, another module's
                        for (const Symbol& c : m.symbols)
                            if (isEnumerator(c) && c.name == name && !isScoped(c) && compiled(c.file)
                                && (where == 0 ? c.container == main
                                    : where == 1 ? c.container.empty()
                                                 : !c.container.empty() && c.container != main
                                                       && m.order.before(c.file, c.at, x->file, x->at)))
                                return c.value;
                    return std::nullopt;
                };
                const std::optional<int64_t> v = Folder(toks, 0, static_cast<int>(toks.size()), lookup).run();
                b.dims.push_back(v && *v >= 1 && *v <= (int64_t{ 1 } << 30) ? static_cast<int>(*v) : 0);
            }
            b.pots = netlist->pots;
            b.switches = netlist->switches;
            out.bindings.push_back(std::move(b));
        }
        // The set() calls of the program's FMinus files: a member call named set whose receiver is of a netlist's
        // module (that module's fields keep their own driving), or whose receiver Lang cannot type (every one).
        const auto isFmText = [](const std::string& n) {
            const auto ends = [&n](std::string_view e) { return n.size() > e.size() && n.compare(n.size() - e.size(), e.size(), e) == 0; };
            return !isCircuitFile(n) && !ends(".gui") && !ends(".sc") && !ends(".sh");
        };
        std::vector<std::pair<std::string, std::string>> owners;   // module -> its note's place and words
        for (std::size_t fi = 0; fi < m.files.size(); ++fi)
        {
            const FileData& f = *m.files[fi];
            if (!f.program || !isFmText(f.name) || !compiled(static_cast<int>(fi)))
                continue;
            const int n = static_cast<int>(f.code.size());
            for (int k = 2; k + 1 < n; ++k)
            {
                const Token& t = f.code[static_cast<std::size_t>(k)];
                if (t.kind != Tok::identifier || t.text != "set" || !Impl::isText(f, k + 1, "(")
                    || !(Impl::isText(f, k - 1, ".") || Impl::isText(f, k - 1, "->")))
                    continue;
                const std::string receiver = baseType(m.typeOf(static_cast<int>(fi), k - 2));
                const std::string at = f.name + ":" + std::to_string(t.line + 1) + ": ";
                for (const CircuitBinding& b : out.bindings)
                {
                    if (!receiver.empty() && receiver != b.module)
                        continue;
                    if (std::any_of(owners.begin(), owners.end(), [&](const auto& o) { return o.first == b.module; }))
                        continue;
                    std::string why = at + b.module + "'s pots have no dials: the program calls its set()";
                    if (receiver.empty())
                    {
                        std::string what;
                        for (int j = k - 2; j >= 0 && f.code[static_cast<std::size_t>(j)].line == t.line; --j)
                        {
                            const Token& w = f.code[static_cast<std::size_t>(j)];
                            if (w.kind != Tok::identifier && w.text != "." && w.text != "->" && w.text != "[" && w.text != "]"
                                && w.kind != Tok::number)
                                break;
                            what = std::string(w.text) + what;
                        }
                        why = at + b.module + "'s pots have no dials: the program calls " + (what.empty() ? "a" : what + ".")
                            + "set(), whose receiver the editor cannot tell is not one";
                    }
                    owners.emplace_back(b.module, why);
                }
            }
        }
        for (CircuitBinding& b : out.bindings)
            for (const auto& [module, why] : owners)
                if (module == b.module && b.args() > 0)
                {
                    b.own = true;
                    b.why = why;
                }
        // The dials' names: the pot's (or switch's) own unless another dial has it (case aside), else <field>_<pot>,
        // numbered on when that is taken too; within a control's name (31 characters).
        std::vector<std::string> taken;
        for (const std::string& p : out.params)
            taken.push_back(lowered(p));
        std::vector<std::string> wanted;
        for (const CircuitBinding& b : out.bindings)
            if (!b.own)
            {
                for (const CircuitPot& p : b.pots)
                    wanted.push_back(lowered(p.name));
                for (const CircuitSwitch& w : b.switches)
                    wanted.push_back(lowered(w.name));
            }
        const auto uses = [](const std::vector<std::string>& v, const std::string& x) {
            return static_cast<int>(std::count(v.begin(), v.end(), x));
        };
        std::vector<std::string> given;
        constexpr std::size_t kLongest = sizeof(Control{}.name) - 1;
        const auto fresh = [&](std::string name) {
            if (name.size() > kLongest)
                name.resize(kLongest);
            std::string tried = name;
            for (int k = 2; uses(taken, lowered(tried)) > 0 || uses(given, lowered(tried)) > 0; ++k)
            {
                const std::string tail = "_" + std::to_string(k);
                tried = name.substr(0, std::min(name.size(), kLongest - tail.size())) + tail;
            }
            given.push_back(lowered(tried));
            return tried;
        };
        for (CircuitBinding& b : out.bindings)
        {
            if (b.own)
                continue;
            const auto name = [&](const std::string& own) {
                const std::string l = lowered(own);
                if (uses(taken, l) == 0 && uses(wanted, l) == 1 && uses(given, l) == 0)
                {
                    given.push_back(l);
                    return own;
                }
                return fresh(b.field + "_" + own);
            };
            for (const CircuitPot& p : b.pots)
                b.names.push_back(name(p.name));
            for (const CircuitSwitch& w : b.switches)
                b.names.push_back(name(w.name));
        }
        return out;
    }

    CircuitDials circuitDials(const Service& s, bool instrument)
    {
        return s.circuitDials(instrument);
    }

    void assignSlots(CircuitDials& d, int taken)
    {
        int next = std::clamp(taken, 0, kMaxControls);
        for (CircuitBinding& b : d.bindings)
        {
            b.slots.assign(static_cast<std::size_t>(b.args()), -1);
            if (b.own || b.names.empty())
                continue;
            for (int& slot : b.slots)
            {
                slot = next < kMaxControls ? next : -1;
                ++next;
            }
        }
    }

    Control potControl(const std::string& name, float def)
    {
        Control c;
        std::snprintf(c.name, sizeof c.name, "%s", name.c_str());
        c.type = ControlType::real;
        c.min = 0.0f;
        c.max = 1.0f;
        c.def = std::clamp(def, 0.0f, 1.0f);
        c.used = true;
        std::snprintf(c.unit, sizeof c.unit, "%%");
        c.decimals = 0;
        c.scale = 100.0f;
        return c;
    }

    Control switchControl(const std::string& name, int positions, int def)
    {
        Control c;
        std::snprintf(c.name, sizeof c.name, "%s", name.c_str());
        c.type = ControlType::integer;
        c.min = 0.0f;
        c.max = static_cast<float>(std::max(positions, 2) - 1);
        c.def = static_cast<float>(std::clamp(def, 0, std::max(positions, 2) - 1));
        c.used = true;
        return c;
    }

    // ---- an instrument's Shared hooks, and Voice's legato -------------------------------------------------------------
    namespace
    {
        struct HookWant
        {
            const char* name;
            const char* declaration;
            std::vector<std::string> params;     // each parameter's type
        };

        // The method `w` of module `container` in the program `s` holds: declared as wanted, or otherwise.
        SharedHook findHook(const Service& s, const char* container, const HookWant& w)
        {
            // A parameter as declared ("float note"; "float" without a name) without its name.
            const auto typeOf = [](const std::string& p) {
                const std::size_t space = p.find_last_of(' ');
                return space == std::string::npos ? p : p.substr(0, space);
            };
            SharedHook h;
            h.name = w.name;
            h.declaration = w.declaration;
            for (const Symbol& m : s.symbols())
            {
                if (m.kind != Kind::method || !m.body || m.container != container || m.name != w.name)
                    continue;
                bool same = m.type == "void" && m.params.size() == w.params.size();
                for (std::size_t a = 0; same && a < w.params.size(); ++a)
                    same = typeOf(m.params[a]) == w.params[a];
                (same ? h.declared : h.otherwise) = true;
                h.file = m.file;
                h.at = m.at;
                break;
            }
            return h;
        }
    }

    std::array<SharedHook, kSharedHooks> sharedHooks(const Service& s)
    {
        const HookWant wants[kSharedHooks] = {
            { "tick", "void tick(int n)", { "int" } },
            { "noteOn", "void noteOn(float note, float velocity)", { "float", "float" } },
            { "noteOff", "void noteOff(float note)", { "float" } },
        };
        std::array<SharedHook, kSharedHooks> out{};
        for (int k = 0; k < kSharedHooks; ++k)
            out[static_cast<std::size_t>(k)] = findHook(s, "Shared", wants[k]);
        return out;
    }

    SharedHook legatoHook(const Service& s)
    {
        return findHook(s, "Voice", { "legato", "void legato(float note, float velocity)", { "float", "float" } });
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
