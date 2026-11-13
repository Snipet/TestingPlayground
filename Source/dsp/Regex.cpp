// Source/dsp/Regex.cpp: see Regex.h. A recursive-descent parser makes a tree of the pattern (Node), which is emitted as
// the program (Thompson's construction, each counted repetition written out); search() runs the program over the text
// one code point at a time with every live state in a sparse set (Pike's VM without captures).
#include "dsp/Regex.h"

#include <algorithm>
#include <utility>

namespace playground::regex
{
    namespace
    {
        constexpr uint32_t kMaxCode = 0x10FFFF;
        constexpr uint32_t kLoneByte = 0x110000;      // + the byte: a byte that is no part of a code point
        constexpr uint32_t kTop = kLoneByte + 0xFF;   // the largest "character"

        using Range = Regex::Range;
        using Op = Regex::Op;

        bool isAsciiWord(unsigned char c)
        {
            return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        }

        uint32_t otherCase(uint32_t c)
        {
            if (c >= 'a' && c <= 'z')
                return c - 32;
            if (c >= 'A' && c <= 'Z')
                return c + 32;
            return c;
        }

        // The code point at byte `at` of `t` and its length in bytes; a byte that starts none is kLoneByte + it.
        uint32_t decode(std::string_view t, std::size_t at, std::size_t& len)
        {
            const auto b = [&](std::size_t i) { return static_cast<uint32_t>(static_cast<unsigned char>(t[i])); };
            const auto tail = [&](std::size_t i) { return i < t.size() && (b(i) & 0xC0) == 0x80; };
            const uint32_t c = b(at);
            len = 1;
            if (c < 0x80)
                return c;
            if (c >= 0xC2 && c <= 0xDF && tail(at + 1))
            {
                len = 2;
                return ((c & 0x1F) << 6) | (b(at + 1) & 0x3F);
            }
            if (c >= 0xE0 && c <= 0xEF && tail(at + 1) && tail(at + 2))
            {
                const uint32_t cp = ((c & 0x0F) << 12) | ((b(at + 1) & 0x3F) << 6) | (b(at + 2) & 0x3F);
                if (cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF))
                {
                    len = 3;
                    return cp;
                }
            }
            if (c >= 0xF0 && c <= 0xF4 && tail(at + 1) && tail(at + 2) && tail(at + 3))
            {
                const uint32_t cp = ((c & 0x07) << 18) | ((b(at + 1) & 0x3F) << 12) | ((b(at + 2) & 0x3F) << 6) | (b(at + 3) & 0x3F);
                if (cp >= 0x10000 && cp <= kMaxCode)
                {
                    len = 4;
                    return cp;
                }
            }
            return kLoneByte + c;
        }

        // The byte UTF-8 starts code point `c` with (a lone byte: itself).
        unsigned char leadOf(uint32_t c)
        {
            if (c >= kLoneByte)
                return static_cast<unsigned char>(c - kLoneByte);
            if (c < 0x80)
                return static_cast<unsigned char>(c);
            if (c < 0x800)
                return static_cast<unsigned char>(0xC0 | (c >> 6));
            if (c < 0x10000)
                return static_cast<unsigned char>(0xE0 | (c >> 12));
            return static_cast<unsigned char>(0xF0 | (c >> 18));
        }

        // Sorted, overlaps and neighbours merged.
        void normalise(std::vector<Range>& r)
        {
            std::sort(r.begin(), r.end(), [](const Range& a, const Range& b) { return a.lo < b.lo || (a.lo == b.lo && a.hi < b.hi); });
            std::vector<Range> out;
            for (const Range& x : r)
                if (!out.empty() && x.lo <= out.back().hi + 1)
                    out.back().hi = std::max(out.back().hi, x.hi);
                else
                    out.push_back(x);
            r = std::move(out);
        }

        // Every character `r` (normalised) leaves out.
        std::vector<Range> complement(const std::vector<Range>& r)
        {
            std::vector<Range> out;
            uint32_t next = 0;
            for (const Range& x : r)
            {
                if (x.lo > next)
                    out.push_back({ next, x.lo - 1 });
                next = x.hi + 1;
            }
            if (next <= kTop)
                out.push_back({ next, kTop });
            return out;
        }

        // ASCII's letters in `r` in their other case too.
        void addCases(std::vector<Range>& r)
        {
            const std::size_t n = r.size();
            for (std::size_t i = 0; i < n; ++i)
                for (const auto& [lo, hi] : { std::pair<uint32_t, uint32_t>{ 'a', 'z' }, std::pair<uint32_t, uint32_t>{ 'A', 'Z' } })
                {
                    const uint32_t a = std::max(r[i].lo, lo), b = std::min(r[i].hi, hi);
                    if (a <= b)
                        r.push_back({ otherCase(a), otherCase(b) });
                }
            normalise(r);
        }

        // \d \w \s and [:name:]'s ranges (RE2's: ASCII's).
        struct NamedClass
        {
            const char* name;
            Range ranges[4];
            std::size_t count;
        };
        constexpr NamedClass kNamed[] = {
            { "alnum", { { '0', '9' }, { 'A', 'Z' }, { 'a', 'z' } }, 3 },
            { "alpha", { { 'A', 'Z' }, { 'a', 'z' } }, 2 },
            { "ascii", { { 0, 0x7F } }, 1 },
            { "blank", { { '\t', '\t' }, { ' ', ' ' } }, 2 },
            { "cntrl", { { 0, 0x1F }, { 0x7F, 0x7F } }, 2 },
            { "digit", { { '0', '9' } }, 1 },
            { "graph", { { 0x21, 0x7E } }, 1 },
            { "lower", { { 'a', 'z' } }, 1 },
            { "print", { { 0x20, 0x7E } }, 1 },
            { "punct", { { 0x21, 0x2F }, { 0x3A, 0x40 }, { 0x5B, 0x60 }, { 0x7B, 0x7E } }, 4 },
            { "space", { { '\t', '\r' }, { ' ', ' ' } }, 2 },
            { "upper", { { 'A', 'Z' } }, 1 },
            { "word", { { '0', '9' }, { 'A', 'Z' }, { 'a', 'z' }, { '_', '_' } }, 4 },
            { "xdigit", { { '0', '9' }, { 'A', 'F' }, { 'a', 'f' } }, 3 },
            { "perl-s", { { '\t', '\n' }, { '\f', '\r' }, { ' ', ' ' } }, 3 },   // \s: RE2's, without \v
        };

        bool namedClass(std::string_view name, std::vector<Range>& out)
        {
            for (const NamedClass& c : kNamed)
                if (name == c.name)
                {
                    out.insert(out.end(), c.ranges, c.ranges + c.count);
                    return true;
                }
            return false;
        }

        struct Node
        {
            enum class Kind : uint8_t
            {
                empty,
                ch,                                   // a, b: the code point and its other case (or itself)
                set,                                  // a: the set, b: negated
                any,                                  // b: not '\n'
                assertion,                            // op
                concat,
                alt,
                repeat,                               // kids[0], min to max (-1: no bound)
            };
            Kind kind = Kind::empty;
            Op op = Op::match;
            uint32_t a = 0, b = 0;
            int min = 0, max = 0;
            std::vector<int> kids;
        };

        constexpr int kNone = -2;                     // atom(): nothing to repeat (a flag group, (?i))

        struct Compiler
        {
            std::string_view p;
            std::size_t at = 0;
            bool fold = false, dotNewline = false;    // flags i and s
            int depth = 0;
            std::size_t quoteEnd = std::string_view::npos;   // in \Q...\E: where its text ends
            std::vector<Node> nodes;
            std::vector<std::vector<Range>> sets;
            std::vector<Regex::Inst> prog;
            std::size_t work = 0, workCap = 0;        // emit()'s calls, and at most (compile() sets it)
            std::string error;

            int fail(std::string why)
            {
                if (error.empty())
                    error = std::move(why);
                return -1;
            }
            std::string where(std::size_t i) const { return " (at character " + std::to_string(i + 1) + ")"; }
            int add(Node n)
            {
                nodes.push_back(std::move(n));
                return static_cast<int>(nodes.size()) - 1;
            }
            int literal(uint32_t c)
            {
                Node n;
                n.kind = Node::Kind::ch;
                n.a = c;
                n.b = fold ? otherCase(c) : c;
                return add(std::move(n));
            }
            int setOf(std::vector<Range> r, bool negated)
            {
                if (fold)
                    addCases(r);
                else
                    normalise(r);
                sets.push_back(std::move(r));
                Node n;
                n.kind = Node::Kind::set;
                n.a = static_cast<uint32_t>(sets.size() - 1);
                n.b = negated ? 1u : 0u;
                return add(std::move(n));
            }
            int assertion(Op op)
            {
                Node n;
                n.kind = Node::Kind::assertion;
                n.op = op;
                return add(std::move(n));
            }
            bool starts(std::string_view s) const { return p.substr(at, s.size()) == s; }
            bool quoting() const { return quoteEnd != std::string_view::npos; }

            // The next character of a \Q...\E's text as a literal of its own, so that a repetition after \E repeats
            // its last character alone (RE2's \Qab\E* is ab*); past the last, its \E.
            int quoted()
            {
                std::size_t len = 1;
                const uint32_t c = decode(p, at, len);
                at = std::min(at + len, quoteEnd);
                if (at == quoteEnd)
                {
                    at = std::min(quoteEnd + 2, p.size());
                    quoteEnd = std::string_view::npos;
                }
                return literal(c);
            }

            // alternation := concatenation ('|' concatenation)*
            int alternation()
            {
                std::vector<int> branches{ concatenation() };
                while (error.empty() && at < p.size() && p[at] == '|')
                {
                    ++at;
                    branches.push_back(concatenation());
                }
                if (!error.empty())
                    return -1;
                if (branches.size() == 1)
                    return branches.front();
                Node n;
                n.kind = Node::Kind::alt;
                n.kids = std::move(branches);
                return add(std::move(n));
            }

            int concatenation()
            {
                std::vector<int> parts;
                while (error.empty() && at < p.size() && (quoting() || (p[at] != '|' && p[at] != ')')))
                    if (const int a = repetition(); a >= 0)
                        parts.push_back(a);
                if (!error.empty())
                    return -1;
                if (parts.size() == 1)
                    return parts.front();
                Node n;
                n.kind = parts.empty() ? Node::Kind::empty : Node::Kind::concat;
                n.kids = std::move(parts);
                return add(std::move(n));
            }

            // {n}, {n,} or {n,m} at `at`: its bounds and its end; false when it is none (then '{' is a literal).
            bool counted(std::size_t from, int& min, int& max, std::size_t& end) const
            {
                std::size_t i = from + 1;
                const auto number = [&](int& out) {
                    const std::size_t first = i;
                    long v = 0;
                    while (i < p.size() && p[i] >= '0' && p[i] <= '9')
                        v = std::min(10L * v + (p[i++] - '0'), 1000000L);
                    out = static_cast<int>(v);
                    return i > first;
                };
                if (!number(min))
                    return false;
                max = min;
                if (i < p.size() && p[i] == ',')
                {
                    ++i;
                    if (!number(max))
                        max = -1;
                }
                if (i >= p.size() || p[i] != '}')
                    return false;
                end = i + 1;
                return true;
            }

            int repetition()
            {
                int a = atom();
                if (!error.empty())
                    return -1;
                std::size_t last = std::string_view::npos;    // where the repetition before this one began
                while (at < p.size() && !quoting())           // within \Q...\E, * is a character
                {
                    const std::size_t op = at;
                    int min = 0, max = -1;
                    if (p[at] == '*')
                        ++at;
                    else if (p[at] == '+')
                    {
                        min = 1;
                        ++at;
                    }
                    else if (p[at] == '?')
                    {
                        max = 1;
                        ++at;
                    }
                    else if (std::size_t end = 0; p[at] == '{' && counted(at, min, max, end))
                        at = end;
                    else
                        break;
                    if (at < p.size() && p[at] == '?')
                        ++at;                                     // lazy: the same lines match
                    if (a == kNone)
                        return fail("missing argument to repetition operator: " + std::string(p.substr(op, at - op)) + where(op));
                    if (last != std::string_view::npos)
                        return fail("bad repetition operator: " + std::string(p.substr(last, at - last)) + where(last));
                    if (min > kMaxRepeat || max > kMaxRepeat)
                        return fail("bad repetition operator: " + std::string(p.substr(op, at - op)) + ", a bound is at most "
                                    + std::to_string(kMaxRepeat) + where(op));
                    if (max >= 0 && max < min)
                        return fail("bad repetition operator: " + std::string(p.substr(op, at - op)) + ", its larger bound first"
                                    + where(op));
                    if (depth + 1 > kMaxDepth)
                        return fail("the pattern nests more than " + std::to_string(kMaxDepth) + " groups and repetitions deep");
                    Node n;
                    n.kind = Node::Kind::repeat;
                    n.kids = { a };
                    n.min = min;
                    n.max = max;
                    a = add(std::move(n));
                    last = op;
                }
                return a;
            }

            int atom()
            {
                if (quoting())
                    return quoted();
                const std::size_t start = at;
                switch (p[at])
                {
                case '(':
                    return group();
                case '[':
                    return charClass();
                case '.':
                {
                    ++at;
                    Node n;
                    n.kind = Node::Kind::any;
                    n.b = dotNewline ? 0u : 1u;
                    return add(std::move(n));
                }
                case '^':
                    ++at;
                    return assertion(Op::bol);
                case '$':
                    ++at;
                    return assertion(Op::eol);
                case '\\':
                    return escape();
                case '*':
                case '+':
                case '?':
                    return fail("missing argument to repetition operator: " + std::string(1, p[at]) + where(at));
                case '{':
                {
                    int min = 0, max = 0;
                    std::size_t end = 0;
                    if (counted(at, min, max, end))
                        return fail("missing argument to repetition operator: " + std::string(p.substr(at, end - at)) + where(at));
                    ++at;
                    return literal('{');
                }
                default:
                {
                    std::size_t len = 1;
                    const uint32_t c = decode(p, start, len);
                    at += len;
                    return literal(c);
                }
                }
            }

            int group()
            {
                const std::size_t open = at++;
                if (++depth > kMaxDepth)
                    return fail("the pattern nests more than " + std::to_string(kMaxDepth) + " groups and repetitions deep");
                const bool savedFold = fold, savedNewline = dotNewline;
                if (at < p.size() && p[at] == '?')
                {
                    ++at;
                    if (starts(":"))
                        ++at;
                    else if (starts("=") || starts("!") || starts("<=") || starts("<!"))
                        return fail("lookahead and lookbehind ((?=, (?!, (?<=, (?<!) are not supported: the search runs in linear "
                                    "time" + where(open));
                    else if (starts("P=") || starts("P>"))
                        return fail("backreferences are not supported: the search runs in linear time" + where(open));
                    else if (starts("P<") || starts("<"))
                    {
                        at += p[at] == 'P' ? 2 : 1;
                        const std::size_t name = at;
                        while (at < p.size() && isAsciiWord(static_cast<unsigned char>(p[at])))
                            ++at;
                        if (at == name || at >= p.size() || p[at] != '>')
                            return fail("invalid named group: " + std::string(p.substr(open, std::min<std::size_t>(at + 1, p.size()) - open))
                                        + where(open));
                        ++at;
                    }
                    else
                    {
                        // Flags: (?i) for the rest of the group it is in, (?i:...) for its own; (?-i) and (?i-s) clear.
                        bool negated = false, any = false;
                        while (true)
                        {
                            if (at >= p.size())
                                return fail("missing ): the ( at character " + std::to_string(open + 1) + " is not closed");
                            const char f = p[at++];
                            if (f == ')' || f == ':')
                            {
                                if (!any)
                                    return fail("invalid flags: " + std::string(p.substr(open, at - open)) + where(open));
                                if (f == ')')
                                {
                                    --depth;
                                    return kNone;                     // the flags stay set, to the enclosing group's end
                                }
                                break;
                            }
                            if (f == '-' && !negated)
                                negated = true;
                            else if (f == 'i')
                                fold = !negated;
                            else if (f == 's')
                                dotNewline = !negated;
                            else if (f != 'm' && f != 'U')
                                return fail("invalid flags: " + std::string(p.substr(open, at - open)) + " (only i, m, s and U)" + where(open));
                            any = any || f != '-';
                        }
                    }
                }
                const int inner = alternation();
                if (!error.empty())
                    return -1;
                if (at >= p.size())
                    return fail("missing ): the ( at character " + std::to_string(open + 1) + " is not closed");
                ++at;
                --depth;
                fold = savedFold;
                dotNewline = savedNewline;
                return inner;
            }

            // A class's escape at `at` (\d \w \s and their complements) into `out`; false when it is none.
            bool perlClass(std::vector<Range>& out)
            {
                if (p[at] != '\\' || at + 1 >= p.size())
                    return false;
                const char c = p[at + 1];
                const char lower = static_cast<char>(c | 0x20);
                if (lower != 'd' && lower != 'w' && lower != 's')
                    return false;
                std::vector<Range> r;
                namedClass(lower == 'd' ? "digit" : lower == 'w' ? "word" : "perl-s", r);
                if (c != lower)
                {
                    normalise(r);
                    r = complement(r);
                }
                out.insert(out.end(), r.begin(), r.end());
                at += 2;
                return true;
            }

            // An escaped character at `at` (the \): \t \n \r \f \v \a, \x41, \x{263A}, or \ and a punctuation mark.
            bool escapedChar(uint32_t& out)
            {
                const std::size_t start = at++;
                if (at >= p.size())
                {
                    fail("trailing \\");
                    return false;
                }
                const char c = p[at];
                const auto hex = [](char h) {
                    return h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                };
                switch (c)
                {
                case 't':
                    out = '\t';
                    break;
                case 'n':
                    out = '\n';
                    break;
                case 'r':
                    out = '\r';
                    break;
                case 'f':
                    out = '\f';
                    break;
                case 'v':
                    out = '\v';
                    break;
                case 'a':
                    out = 7;
                    break;
                case 'x':
                {
                    uint32_t v = 0;
                    std::size_t i = at + 1, digits = 0;
                    const bool braced = i < p.size() && p[i] == '{';
                    i += braced ? 1 : 0;
                    while (i < p.size() && hex(p[i]) >= 0 && (braced || digits < 2))
                    {
                        v = std::min<uint32_t>(16 * v + static_cast<uint32_t>(hex(p[i++])), kMaxCode + 1);
                        ++digits;
                    }
                    if (braced && (i >= p.size() || p[i] != '}'))
                        digits = 0;
                    if (digits == 0 || (!braced && digits != 2) || v > kMaxCode)
                    {
                        fail("invalid escape sequence: " + std::string(p.substr(start, std::min(i + 1, p.size()) - start)) + where(start));
                        return false;
                    }
                    out = v;
                    at = i + (braced ? 1 : 0);
                    return true;
                }
                case 'p':
                case 'P':
                    fail("\\p (Unicode's classes) is not supported: write the characters as a class, [...]" + where(start));
                    return false;
                default:
                    if (c >= '1' && c <= '9')
                    {
                        fail("backreferences are not supported: the search runs in linear time" + where(start));
                        return false;
                    }
                    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
                    {
                        fail("invalid escape sequence: \\" + std::string(1, c) + where(start));
                        return false;
                    }
                    {
                        std::size_t len = 1;
                        out = decode(p, at, len);             // a punctuation mark (or any other character) itself
                        at += len;
                        return true;
                    }
                }
                ++at;
                return true;
            }

            int escape()
            {
                if (at + 1 < p.size())
                    switch (p[at + 1])
                    {
                    case 'b':
                        at += 2;
                        return assertion(Op::edge);
                    case 'B':
                        at += 2;
                        return assertion(Op::noEdge);
                    case 'A':
                        at += 2;
                        return assertion(Op::bol);
                    case 'z':
                        at += 2;
                        return assertion(Op::eol);
                    case 'Q':
                    {
                        // \Q...\E: its text as it is (to \E, or the pattern's end), a character an atom (quoted()). An
                        // empty one is nothing, as a flag group is.
                        at += 2;
                        const std::size_t end = std::min(p.find("\\E", at), p.size());
                        if (at == end)
                        {
                            at = std::min(end + 2, p.size());
                            return kNone;
                        }
                        quoteEnd = end;
                        return quoted();
                    }
                    default:
                        break;
                    }
                std::vector<Range> r;
                if (perlClass(r))
                    return setOf(std::move(r), false);
                uint32_t c = 0;
                if (!escapedChar(c))
                    return -1;
                return literal(c);
            }

            int charClass()
            {
                const std::size_t open = at++;
                bool negated = false;
                if (at < p.size() && p[at] == '^')
                {
                    negated = true;
                    ++at;
                }
                std::vector<Range> ranges;
                for (bool first = true;; first = false)
                {
                    if (at >= p.size())
                        return fail("missing ]: the [ at character " + std::to_string(open + 1) + " is not closed");
                    if (p[at] == ']' && !first)
                    {
                        ++at;
                        break;
                    }
                    if (starts("[:"))
                    {
                        const std::size_t end = p.find(":]", at + 2);
                        if (end != std::string_view::npos)
                        {
                            std::string_view name = p.substr(at + 2, end - at - 2);
                            const bool complemented = !name.empty() && name.front() == '^';
                            name.remove_prefix(complemented ? 1 : 0);
                            std::vector<Range> r;
                            if (name == "perl-s" || !namedClass(name, r))
                                return fail("invalid character class: " + std::string(p.substr(at, end + 2 - at)) + where(at));
                            if (complemented)
                            {
                                normalise(r);
                                r = complement(r);
                            }
                            ranges.insert(ranges.end(), r.begin(), r.end());
                            at = end + 2;
                            continue;
                        }
                    }
                    if (perlClass(ranges))
                        continue;
                    const std::size_t from = at;
                    const auto one = [&](uint32_t& c) {
                        if (p[at] == '\\')
                            return escapedChar(c);
                        std::size_t len = 1;
                        c = decode(p, at, len);
                        at += len;
                        return true;
                    };
                    uint32_t lo = 0, hi = 0;
                    if (!one(lo))
                        return -1;
                    hi = lo;
                    if (at + 1 < p.size() && p[at] == '-' && p[at + 1] != ']')
                    {
                        ++at;
                        std::vector<Range> unused;
                        if (const std::size_t keep = at; perlClass(unused))
                            return fail("invalid character class range: " + std::string(p.substr(from, at - from)) + where(keep));
                        if (!one(hi))
                            return -1;
                        if (hi < lo)
                            return fail("invalid character class range: " + std::string(p.substr(from, at - from)) + where(from));
                    }
                    ranges.push_back({ lo, hi });
                }
                return setOf(std::move(ranges), negated);
            }

            std::size_t inst(Op op, uint32_t a = 0, uint32_t b = 0)
            {
                prog.push_back({ op, a, b });
                return prog.size() - 1;
            }
            uint32_t here() const { return static_cast<uint32_t>(prog.size()); }

            // The node's instructions onto prog. Its calls are counted against workCap: a repetition of a node that
            // writes nothing writes nothing, but one of many parts that write nothing ((?:()()()a){1000}) would still
            // take a call a part a copy.
            void emit(int i)
            {
                if (!error.empty())
                    return;
                if (prog.size() > kMaxProgram)
                {
                    fail("the pattern is too large: its repetitions written out make more than " + std::to_string(kMaxProgram)
                         + " instructions");
                    return;
                }
                if (++work > workCap)
                {
                    fail("the pattern is too large: its repetitions take too many steps to write out (give them fewer or smaller "
                         "counts)");
                    return;
                }
                const Node& n = nodes[static_cast<std::size_t>(i)];
                switch (n.kind)
                {
                case Node::Kind::empty:
                    return;
                case Node::Kind::ch:
                    inst(Op::ch, n.a, n.b);
                    return;
                case Node::Kind::set:
                    inst(Op::set, n.a, n.b);
                    return;
                case Node::Kind::any:
                    inst(Op::any, 0, n.b);
                    return;
                case Node::Kind::assertion:
                    inst(n.op);
                    return;
                case Node::Kind::concat:
                    for (const int k : n.kids)
                        emit(k);
                    return;
                case Node::Kind::alt:
                {
                    std::vector<std::size_t> jumps;
                    for (std::size_t k = 0; k + 1 < n.kids.size() && error.empty(); ++k)
                    {
                        const std::size_t split = inst(Op::split, here() + 1);
                        emit(n.kids[k]);
                        jumps.push_back(inst(Op::jump));
                        prog[split].b = here();
                    }
                    emit(n.kids.back());
                    for (const std::size_t j : jumps)
                        prog[j].a = here();
                    return;
                }
                case Node::Kind::repeat:
                {
                    // An operand that writes nothing (x{0}, (), (?:)) matches the empty string alone, and so does any
                    // count of it: the repetition writes nothing either (its split taken back), so nested counts of it
                    // take a call a level, not 1000^depth.
                    const int kid = n.kids.front(), min = n.min, max = n.max;
                    const auto copy = [&] {
                        const uint32_t before = here();
                        emit(kid);
                        return here() != before;
                    };
                    if (max < 0)
                    {
                        // x{n,}: n - 1 copies, then x+ (a copy and a split back); x*: a split over a copy and a jump back.
                        for (int k = 0; k + 1 < min && error.empty(); ++k)
                            if (!copy())
                                return;
                        if (min >= 1)
                        {
                            const uint32_t top = here();
                            if (copy())
                                inst(Op::split, top, here() + 1);
                        }
                        else
                        {
                            const std::size_t split = inst(Op::split, here() + 1);
                            if (!copy())
                            {
                                prog.pop_back();
                                return;
                            }
                            inst(Op::jump, static_cast<uint32_t>(split));
                            prog[split].b = here();
                        }
                        return;
                    }
                    // x{n,m}: n copies, then m - n each behind a split to the end.
                    for (int k = 0; k < min && error.empty(); ++k)
                        if (!copy())
                            return;
                    std::vector<std::size_t> outs;
                    for (int k = min; k < max && error.empty(); ++k)
                    {
                        outs.push_back(inst(Op::split, here() + 1));
                        if (!copy())
                        {
                            prog.pop_back();
                            outs.pop_back();
                            break;
                        }
                    }
                    for (const std::size_t s : outs)
                        prog[s].b = here();
                    return;
                }
                }
            }
        };
    }

    Regex Regex::compile(std::string_view pattern, bool ignoreCase)
    {
        Regex re;
        if (pattern.size() > kMaxPattern)
        {
            re.error_ = "the pattern is longer than " + std::to_string(kMaxPattern) + " bytes";
            return re;
        }
        Compiler c;
        c.p = pattern;
        c.fold = ignoreCase;
        const int root = c.alternation();
        if (c.error.empty() && c.at < pattern.size())
            c.fail("unexpected )" + c.where(c.at));
        if (c.error.empty())
        {
            c.workCap = 4 * kMaxProgram + c.nodes.size();   // a few calls an instruction, past what a pattern needs
            c.emit(root);
            c.inst(Op::match);
            if (c.prog.size() > kMaxProgram + 1)
                c.fail("the pattern is too large: its repetitions written out make more than " + std::to_string(kMaxProgram)
                       + " instructions");
        }
        if (!c.error.empty())
        {
            re.error_ = std::move(c.error);
            return re;
        }
        re.prog_ = std::move(c.prog);
        re.sets_ = std::move(c.sets);
        re.prepare();
        return re;
    }

    void Regex::prepare()
    {
        // The bytes a match may start with: what the start's closure reads first (its assertions passed over), unless
        // it reaches the match or reads any character (then every byte).
        firstAny_ = false;
        std::fill(std::begin(first_), std::end(first_), false);
        const auto mark = [&](uint32_t lo, uint32_t hi) {
            const auto byteRange = [&](uint32_t a, uint32_t b) {
                for (uint32_t x = a; x <= b; ++x)
                    first_[x] = true;
            };
            if (lo < 0x80)
                byteRange(lo, std::min<uint32_t>(hi, 0x7F));
            if (hi >= 0x80 && lo <= kMaxCode)
                byteRange(leadOf(std::max<uint32_t>(lo, 0x80)), leadOf(std::min(hi, kMaxCode)));
            if (hi >= kLoneByte)
                byteRange(std::max(lo, kLoneByte) - kLoneByte, hi - kLoneByte);
        };
        std::vector<char> seen(prog_.size(), 0);
        std::vector<uint32_t> todo{ 0 };
        while (!todo.empty() && !firstAny_)
        {
            const uint32_t pc = todo.back();
            todo.pop_back();
            if (seen[pc] != 0)
                continue;
            seen[pc] = 1;
            const Inst& in = prog_[pc];
            switch (in.op)
            {
            case Op::ch:
                mark(in.a, in.a);
                mark(in.b, in.b);
                break;
            case Op::set:
                if (in.b != 0)
                    firstAny_ = true;
                else
                    for (const Range& r : sets_[in.a])
                        mark(r.lo, r.hi);
                break;
            case Op::any:
            case Op::match:
                firstAny_ = true;
                break;
            case Op::bol:
            case Op::eol:
            case Op::edge:
            case Op::noEdge:
                todo.push_back(pc + 1);
                break;
            case Op::split:
                todo.push_back(in.b);
                todo.push_back(in.a);
                break;
            case Op::jump:
                todo.push_back(in.a);
                break;
            }
        }
        for (int k = 0; k < 2; ++k)
        {
            dense_[k].assign(prog_.size(), 0);
            start_[k].assign(prog_.size(), 0);
            sparse_[k].assign(prog_.size(), 0);
        }
        stack_.reserve(2 * prog_.size() + 2);
    }

    Found Regex::close(std::size_t l, uint32_t pc0, std::string_view t, std::size_t pos, std::uint64_t& steps)
    {
        std::vector<uint32_t>& dense = dense_[l];
        std::vector<uint32_t>& sparse = sparse_[l];
        std::size_t& count = count_[l];
        stack_.clear();
        stack_.push_back(pc0);
        while (!stack_.empty())
        {
            const uint32_t pc = stack_.back();
            stack_.pop_back();
            if (sparse[pc] < count && dense[sparse[pc]] == pc)
                continue;                                 // in the set already
            if (steps == 0)
                return Found::spent;
            --steps;
            sparse[pc] = static_cast<uint32_t>(count);
            dense[count++] = pc;
            const Inst& in = prog_[pc];
            switch (in.op)
            {
            case Op::match:
                return Found::yes;
            case Op::jump:
                stack_.push_back(in.a);
                break;
            case Op::split:
                stack_.push_back(in.b);
                stack_.push_back(in.a);
                break;
            case Op::bol:
                if (pos == 0)
                    stack_.push_back(pc + 1);
                break;
            case Op::eol:
                if (pos == t.size())
                    stack_.push_back(pc + 1);
                break;
            case Op::edge:
            case Op::noEdge:
            {
                const bool before = pos > 0 && isAsciiWord(static_cast<unsigned char>(t[pos - 1]));
                const bool after = pos < t.size() && isAsciiWord(static_cast<unsigned char>(t[pos]));
                if ((before != after) == (in.op == Op::edge))
                    stack_.push_back(pc + 1);
                break;
            }
            case Op::ch:
            case Op::set:
            case Op::any:
                break;                                    // it reads the next character: search()'s step
            }
        }
        return Found::no;
    }

    Found Regex::search(std::string_view t, std::uint64_t& steps)
    {
        if (!ok())
            return Found::no;
        std::size_t cur = 0, pos = 0;
        count_[0] = count_[1] = 0;
        while (true)
        {
            if (count_[cur] == 0 && !firstAny_)
            {
                // Nothing alive: on to the next byte a match may start with.
                while (pos < t.size() && !first_[static_cast<unsigned char>(t[pos])])
                    ++pos;
                if (pos == t.size())
                    return Found::no;
            }
            if (const Found f = close(cur, 0, t, pos, steps); f != Found::no)
                return f;                                 // a match may start at any character (unanchored)
            if (pos == t.size())
                return Found::no;
            if (steps == 0)
                return Found::spent;
            --steps;
            std::size_t len = 1;
            const uint32_t c = decode(t, pos, len);
            const std::size_t next = cur ^ 1;
            count_[next] = 0;
            for (std::size_t k = 0; k < count_[cur]; ++k)
            {
                const uint32_t pc = dense_[cur][k];
                const Inst& in = prog_[pc];
                bool takes = false;
                if (in.op == Op::ch)
                    takes = c == in.a || c == in.b;
                else if (in.op == Op::any)
                    takes = in.b == 0 || c != '\n';
                else if (in.op == Op::set)
                {
                    const std::vector<Range>& r = sets_[in.a];
                    const auto it = std::upper_bound(r.begin(), r.end(), c, [](uint32_t x, const Range& y) { return x < y.lo; });
                    takes = (it != r.begin() && c <= std::prev(it)->hi) != (in.b != 0);
                }
                if (takes)
                    if (const Found f = close(next, pc + 1, t, pos + len, steps); f != Found::no)
                        return f;
            }
            cur = next;
            pos += len;
        }
    }

    Found Regex::closeFrom(std::size_t l, uint32_t pc0, std::string_view t, std::size_t pos, std::size_t start, std::uint64_t& steps,
                           Kept& kept)
    {
        std::vector<uint32_t>& dense = dense_[l];
        std::vector<uint32_t>& sparse = sparse_[l];
        std::size_t& count = count_[l];
        stack_.clear();
        stack_.push_back(pc0);
        while (!stack_.empty())
        {
            const uint32_t pc = stack_.back();
            stack_.pop_back();
            if (sparse[pc] < count && dense[sparse[pc]] == pc)
                continue;                                 // in the set already, from a thread that started no later
            if (steps == 0)
                return Found::spent;
            --steps;
            sparse[pc] = static_cast<uint32_t>(count);
            start_[l][count] = start;
            dense[count++] = pc;
            const Inst& in = prog_[pc];
            switch (in.op)
            {
            case Op::match:
                if (!kept.any || start < kept.begin || (start == kept.begin && pos > kept.end))
                    kept = { true, start, pos };
                break;
            case Op::jump:
                stack_.push_back(in.a);
                break;
            case Op::split:
                stack_.push_back(in.b);
                stack_.push_back(in.a);
                break;
            case Op::bol:
                if (pos == 0)
                    stack_.push_back(pc + 1);
                break;
            case Op::eol:
                if (pos == t.size())
                    stack_.push_back(pc + 1);
                break;
            case Op::edge:
            case Op::noEdge:
            {
                const bool before = pos > 0 && isAsciiWord(static_cast<unsigned char>(t[pos - 1]));
                const bool after = pos < t.size() && isAsciiWord(static_cast<unsigned char>(t[pos]));
                if ((before != after) == (in.op == Op::edge))
                    stack_.push_back(pc + 1);
                break;
            }
            case Op::ch:
            case Op::set:
            case Op::any:
                break;                                    // it reads the next character: find()'s step
            }
        }
        return Found::no;
    }

    Found Regex::find(std::string_view t, std::size_t from, std::uint64_t& steps, std::size_t& begin, std::size_t& end)
    {
        if (!ok() || from > t.size())
            return Found::no;
        // The states of each list are in the order their threads started: those carried from the list before come
        // first (in its order), the thread that starts here last, so a state two threads reach keeps the earlier
        // start. Once a match is kept no thread starts again, and only threads that started no later than it go on
        // (one that started earlier may still match, further left); it ends when none is left.
        std::size_t cur = 0, pos = from;
        count_[0] = count_[1] = 0;
        Kept kept;
        while (true)
        {
            if (!kept.any)
            {
                if (count_[cur] == 0 && !firstAny_)
                {
                    while (pos < t.size() && !first_[static_cast<unsigned char>(t[pos])])
                        ++pos;
                    if (pos == t.size())
                        return Found::no;
                }
                if (closeFrom(cur, 0, t, pos, pos, steps, kept) == Found::spent)
                    return Found::spent;
            }
            bool alive = false;
            for (std::size_t k = 0; k < count_[cur] && !alive; ++k)
                alive = !kept.any || start_[cur][k] <= kept.begin;
            if (pos == t.size() || (kept.any && !alive))
                break;
            if (steps == 0)
                return Found::spent;
            --steps;
            std::size_t len = 1;
            const uint32_t c = decode(t, pos, len);
            const std::size_t next = cur ^ 1;
            count_[next] = 0;
            for (std::size_t k = 0; k < count_[cur]; ++k)
            {
                const std::size_t start = start_[cur][k];
                if (kept.any && start > kept.begin)
                    continue;
                const uint32_t pc = dense_[cur][k];
                const Inst& in = prog_[pc];
                bool takes = false;
                if (in.op == Op::ch)
                    takes = c == in.a || c == in.b;
                else if (in.op == Op::any)
                    takes = in.b == 0 || c != '\n';
                else if (in.op == Op::set)
                {
                    const std::vector<Range>& r = sets_[in.a];
                    const auto it = std::upper_bound(r.begin(), r.end(), c, [](uint32_t x, const Range& y) { return x < y.lo; });
                    takes = (it != r.begin() && c <= std::prev(it)->hi) != (in.b != 0);
                }
                if (takes && closeFrom(next, pc + 1, t, pos + len, start, steps, kept) == Found::spent)
                    return Found::spent;
            }
            cur = next;
            pos += len;
        }
        if (!kept.any)
            return Found::no;
        begin = kept.begin;
        end = kept.end;
        return Found::yes;
    }
}
