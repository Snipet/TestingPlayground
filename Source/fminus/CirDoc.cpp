// Source/fminus/CirDoc.cpp: a .cir netlist as the schematic editor sees it (CirDoc.h).
#include "fminus/CirDoc.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <map>
#include <set>
#include <utility>

namespace playground::cir
{
    namespace
    {
        constexpr const char* kHeader = "*@ schematic 1 (the playground's drawing; SPICE reads *@ lines as comments)";

        bool blank(char c) noexcept { return c == ' ' || c == '\t'; }
        bool digit(char c) noexcept { return c >= '0' && c <= '9'; }
        bool letter(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
        char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

        // The text's lines, split at LF (one more than its line breaks).
        std::vector<std::string_view> splitAt(std::string_view text)
        {
            std::vector<std::string_view> out;
            std::size_t start = 0;
            while (true)
            {
                const std::size_t end = text.find('\n', start);
                if (end == std::string_view::npos)
                {
                    out.push_back(text.substr(start));
                    return out;
                }
                out.push_back(text.substr(start, end - start));
                start = end + 1;
            }
        }

        std::string_view withoutCr(std::string_view l) noexcept
        {
            if (!l.empty() && l.back() == '\r')
                l.remove_suffix(1);
            return l;
        }

        // A line as a write keeps it: a CRLF line's CR dropped (FMinus drops one CR, the editor every CR LF), a line
        // that ends in more CRs kept as it is (FMinus reads the rest of them as part of the line, so they stay), so that
        // FMinus reads the same, and writing again changes nothing.
        std::string_view lfLine(std::string_view l) noexcept
        {
            if (l.size() >= 2 && l[l.size() - 1] == '\r' && l[l.size() - 2] == '\r')
                return l;
            return withoutCr(l);
        }

        std::string joined(const std::vector<std::string>& lines)
        {
            std::string out;
            for (std::size_t i = 0; i < lines.size(); ++i)
                out += (i == 0 ? "" : "\n") + lines[i];
            return out;
        }

        std::vector<std::string> linesOf(std::string_view text)
        {
            std::vector<std::string> out;
            for (const std::string_view l : splitAt(text))
                out.emplace_back(l);
            return out;
        }

        std::string ptText(Pt p) { return std::to_string(p.x) + "," + std::to_string(p.y); }

        // An op's refusal of a point past kPartLimit.
        std::string farOut(Pt p)
        {
            return ptText(p) + " is too far out (a part's point is within -" + std::to_string(kPartLimit) + ".." + std::to_string(kPartLimit) + ")";
        }

        std::optional<Pt> ptOf(std::string_view s)
        {
            const std::size_t comma = s.find(',');
            if (comma == std::string_view::npos)
                return std::nullopt;
            const auto num = [](std::string_view t, int& v) {
                if (t.empty())
                    return false;
                bool neg = false;
                if (t.front() == '-' || t.front() == '+')
                {
                    neg = t.front() == '-';
                    t.remove_prefix(1);
                }
                if (t.empty() || t.size() > 6)
                    return false;
                int n = 0;
                for (const char c : t)
                {
                    if (!digit(c))
                        return false;
                    n = n * 10 + (c - '0');
                }
                v = neg ? -n : n;
                return v >= -kCoordLimit && v <= kCoordLimit;
            };
            Pt p;
            if (!num(s.substr(0, comma), p.x) || !num(s.substr(comma + 1), p.y))
                return std::nullopt;
            return p;
        }

        // ---- FMinus's scan (its Netlist.cpp): a logical line's tokens ------------------------------------------------
        struct Logical
        {
            std::vector<Token> tokens;
            int first = 0, last = 0;
            std::vector<int> lines;                     // its own lines: the first and its + continuations
        };

        // One physical line's tokens: cut at ';', blanks are spaces and tabs, `{...}` one token. Empty for a comment
        // or a blank line; `cont`: a continuation (its leading + left out).
        std::vector<Token> scanLine(std::string_view raw, int ln, bool& cont, bool& comment)
        {
            raw = withoutCr(raw);
            const std::size_t semi = raw.find(';');
            if (semi != std::string_view::npos)
                raw = raw.substr(0, semi);
            std::size_t i = 0;
            while (i < raw.size() && blank(raw[i]))
                ++i;
            cont = false;
            comment = i == raw.size() || raw[i] == '*';
            std::vector<Token> out;
            if (comment)
                return out;
            if (raw[i] == '+')
            {
                cont = true;
                ++i;
            }
            while (i < raw.size())
            {
                while (i < raw.size() && blank(raw[i]))
                    ++i;
                if (i == raw.size())
                    break;
                const std::size_t first = i;
                const auto toClose = [&](std::size_t at) {
                    const std::size_t close = raw.find('}', at);
                    return close == std::string_view::npos ? raw.size() : close + 1;
                };
                if (raw[i] == '{')
                    i = toClose(i);
                else
                    while (i < raw.size() && !blank(raw[i]))
                        i = raw[i] == '{' ? toClose(i) : i + 1;
                Token t;
                t.text = std::string(raw.substr(first, i - first));
                t.at = { ln, static_cast<int>(first) };
                t.len = static_cast<int>(i - first);
                out.push_back(std::move(t));
            }
            return out;
        }

        Token joinedTokens(const Token& a, const Token& b)
        {
            Token t = a;
            t.text += b.text;
            if (a.at.line == b.at.line)
                t.len = b.at.col + b.len - a.at.col;
            return t;
        }

        // key = value written with spaces, joined into one key=value token at the key's place (Netlist.cpp's).
        std::vector<Token> joinAssignments(std::vector<Token> tokens)
        {
            std::vector<Token> out;
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                Token t = tokens[i];
                if (t.text == "=" && !out.empty() && i + 1 < tokens.size())
                {
                    out.back() = joinedTokens(joinedTokens(out.back(), t), tokens[i + 1]);
                    ++i;
                    continue;
                }
                if (!t.text.empty() && t.text.back() == '=' && i + 1 < tokens.size())
                {
                    t = joinedTokens(t, tokens[i + 1]);
                    ++i;
                }
                else if (!t.text.empty() && t.text.front() == '=' && !out.empty())
                {
                    out.back() = joinedTokens(out.back(), t);
                    continue;
                }
                out.push_back(std::move(t));
            }
            return out;
        }

        // The logical lines up to the first .end (FMinus stops there); `endLine` its line, -1 for none.
        std::vector<Logical> scan(const std::vector<std::string>& lines, int& endLine)
        {
            std::vector<Logical> out;
            endLine = -1;
            for (std::size_t ln = 0; ln < lines.size(); ++ln)
            {
                bool cont = false, comment = false;
                std::vector<Token> toks = scanLine(lines[ln], static_cast<int>(ln), cont, comment);
                if (comment)
                    continue;
                if (cont)
                {
                    if (!out.empty())
                    {
                        for (Token& t : toks)
                            out.back().tokens.push_back(std::move(t));
                        out.back().last = static_cast<int>(ln);
                        out.back().lines.push_back(static_cast<int>(ln));
                    }
                    continue;
                }
                if (toks.empty())
                    continue;
                if (folded(toks.front().text) == ".end")
                {
                    endLine = static_cast<int>(ln);
                    break;
                }
                Logical l;
                l.tokens = std::move(toks);
                l.first = l.last = static_cast<int>(ln);
                l.lines.push_back(static_cast<int>(ln));
                out.push_back(std::move(l));
            }
            for (Logical& l : out)
                l.tokens = joinAssignments(std::move(l.tokens));
            return out;
        }

        std::string nodeKey(std::string_view name)
        {
            std::string k = folded(name);
            return k == "gnd" ? std::string("0") : k;
        }

        bool isTaper(std::string_view w)
        {
            const std::string f = folded(w);
            return f == "lin" || f == "audio" || f == "rlog";
        }

        bool plainName(std::string_view s)
        {
            if (s.empty() || !letter(s.front()))
                return false;
            return std::all_of(s.begin(), s.end(), [](char c) { return letter(c) || digit(c); });
        }

        bool nodeName(std::string_view s)
        {
            if (s.empty())
                return false;
            return std::all_of(s.begin(), s.end(), [](char c) { return letter(c) || digit(c) || c == '_'; });
        }

        bool generatedNode(std::string_view s)
        {
            return s.size() >= 2 && lower(s[0]) == 'n' && std::all_of(s.begin() + 1, s.end(), digit);
        }

        // A SPICE number as P1's lexer reads one: [+-]digits[.digits][e[+-]digits][suffix][letters] (10k, 2.2u, 1meg,
        // -60dB).
        bool spiceNumber(std::string_view s)
        {
            std::size_t i = 0;
            if (i < s.size() && (s[i] == '+' || s[i] == '-'))
                ++i;
            const std::size_t d0 = i;
            while (i < s.size() && digit(s[i]))
                ++i;
            bool any = i > d0;
            if (i < s.size() && s[i] == '.')
            {
                ++i;
                const std::size_t f0 = i;
                while (i < s.size() && digit(s[i]))
                    ++i;
                any = any || i > f0;
            }
            if (!any)
                return false;
            if (i + 1 < s.size() && lower(s[i]) == 'e' && (digit(s[i + 1]) || ((s[i + 1] == '+' || s[i + 1] == '-') && i + 2 < s.size() && digit(s[i + 2]))))
            {
                i += 2;
                while (i < s.size() && digit(s[i]))
                    ++i;
            }
            while (i < s.size() && letter(s[i]))
                ++i;
            return i == s.size();
        }

        // The sign of a SPICE number: +1, -1, 0 (zero), or +1 when it does not read (a rail's direction).
        int signOf(std::string_view s)
        {
            if (!spiceNumber(s))
                return 1;
            const bool neg = !s.empty() && s.front() == '-';
            bool nonzero = false;
            for (std::size_t i = 0; i < s.size(); ++i)
            {
                if (digit(s[i]) && s[i] != '0')
                    nonzero = true;
                if (lower(s[i]) == 'e' || (letter(s[i]) && i > 0))
                    break;
            }
            return !nonzero ? 0 : neg ? -1 : 1;
        }

        // The identifiers inside a {expr} token.
        std::vector<std::pair<std::size_t, std::size_t>> wordsIn(std::string_view expr)
        {
            std::vector<std::pair<std::size_t, std::size_t>> out;
            std::size_t i = 0;
            while (i < expr.size())
            {
                if (letter(expr[i]) || expr[i] == '_')
                {
                    const std::size_t a = i;
                    while (i < expr.size() && (letter(expr[i]) || digit(expr[i]) || expr[i] == '_'))
                        ++i;
                    out.emplace_back(a, i - a);
                }
                else if (digit(expr[i]) || expr[i] == '.')
                {
                    while (i < expr.size() && (letter(expr[i]) || digit(expr[i]) || expr[i] == '.'))
                        ++i;
                }
                else
                    ++i;
            }
            return out;
        }

        // ---- an element's line ------------------------------------------------------------------------------------
        Element elementOf(const Logical& l)
        {
            Element e;
            const std::vector<Token>& t = l.tokens;
            e.name = t[0].text;
            e.letter = lower(t[0].text[0]);
            e.firstLine = l.first;
            e.lastLine = l.last;
            e.tokens = t;
            const int n = static_cast<int>(t.size());
            const auto take = [&](Kind k, std::initializer_list<int> idx, int value) {
                const Symbol& s = symbol(k);
                int k0 = 0;
                for (const int i : idx)
                {
                    if (i >= n)
                    {
                        e.nodes.clear();
                        e.tokenOf.clear();
                        e.pinNames.clear();
                        // what it has, for renames; not drawn
                        for (int j = 1; j < n && j <= static_cast<int>(idx.size()); ++j)
                        {
                            e.nodes.push_back(t[static_cast<std::size_t>(j)]);
                            e.tokenOf.push_back(j);
                            e.pinNames.emplace_back();
                        }
                        e.why = std::string("a ") + s.word + " with too few tokens";
                        return;
                    }
                    e.nodes.push_back(t[static_cast<std::size_t>(i)]);
                    e.tokenOf.push_back(i);
                    e.pinNames.emplace_back(s.pins[static_cast<std::size_t>(k0++)].name);
                }
                e.kind = k;
                e.valueToken = value < n ? value : -1;
            };
            const auto hidden = [&](const char* why, int first, int count) {
                e.why = why;
                for (int i = first; i < n && i < first + count; ++i)
                {
                    if (t[static_cast<std::size_t>(i)].text.find_first_of("{}=()") != std::string::npos)
                        break;
                    e.nodes.push_back(t[static_cast<std::size_t>(i)]);
                    e.tokenOf.push_back(i);
                    e.pinNames.emplace_back();
                }
            };
            switch (e.letter)
            {
                case 'r': take(Kind::R, { 1, 2 }, 3); break;
                case 'c': take(Kind::C, { 1, 2 }, 3); break;
                case 'l': take(Kind::L, { 1, 2 }, 3); break;
                case 'd': take(Kind::D, { 1, 2 }, 3); break;
                case 'v':
                case 'i':
                    take(e.letter == 'v' ? Kind::V : Kind::I, { 1, 2 }, n >= 5 && folded(t[3].text) == "dc" ? 4 : 3);
                    break;
                case 'e': take(Kind::Vcvs, { 1, 2, 3, 4 }, 5); break;
                case 'g':
                    if (n >= 4 && folded(t[3].text) == "ota")
                        take(Kind::Ota, { 1, 2, 4, 5 }, 6);
                    else
                        take(Kind::Vccs, { 1, 2, 3, 4 }, 5);
                    break;
                case 'p':
                {
                    int brace = 0;
                    for (int i = 1; i < n; ++i)
                        if (!t[static_cast<std::size_t>(i)].text.empty() && t[static_cast<std::size_t>(i)].text.front() == '{')
                        {
                            brace = i;
                            break;
                        }
                    if (brace == 4)
                        take(Kind::Rheostat, { 1, 2 }, 3);
                    else if (brace == 5)
                        take(Kind::Pot, { 1, 2, 3 }, 4);
                    else
                        hidden("a pot with no {expr} where FMinus wants one", 1, 3);
                    break;
                }
                case 's': take(Kind::S, { 1, 2 }, 4); break;
                case 'x':
                    if (n >= 5 && folded(t[4].text) == "opamp")
                        take(Kind::OpAmp, { 1, 2, 3 }, -1);
                    else
                        hidden("a subcircuit that is no op amp: Phase 2", 1, std::max(0, n - 2));
                    break;
                case 'q': hidden("a BJT: Phase 2", 1, n >= 6 ? 4 : 3); break;
                case 'j': hidden("a JFET: Phase 2", 1, 3); break;
                case 'm': hidden("a MOSFET: Phase 2", 1, 4); break;
                case 'f':
                case 'h': hidden("a current-controlled source: Phase 2", 1, 2); break;
                case 'k': hidden("a coupling: Phase 2", 1, 0); break;
                case 't': hidden("a transmission line", 1, 4); break;
                case 'b': hidden("a behavioural source", 1, 2); break;
                case 'w': hidden("a current-controlled switch: Phase 2", 1, 2); break;
                default: hidden("an unknown element", 1, 0); break;
            }
            return e;
        }

        // The physical lines an element or a directive is written on: its first and its + continuations.
        std::vector<int> ownLines(const std::vector<std::string>& lines, int first, int last)
        {
            std::vector<int> out{ first };
            for (int ln = first + 1; ln <= last && ln < static_cast<int>(lines.size()); ++ln)
            {
                bool cont = false, comment = false;
                (void)scanLine(lines[static_cast<std::size_t>(ln)], ln, cont, comment);
                if (cont)
                    out.push_back(ln);
            }
            return out;
        }

        // A directive's physical lines: its first and the + lines after it, past comment and blank lines, as FMinus
        // joins them (a .model's parameters may go on).
        std::vector<int> directiveLines(const std::vector<std::string>& lines, int first)
        {
            std::vector<int> out{ first };
            for (int ln = first + 1; ln < static_cast<int>(lines.size()); ++ln)
            {
                bool cont = false, comment = false;
                (void)scanLine(lines[static_cast<std::size_t>(ln)], ln, cont, comment);
                if (cont)
                    out.push_back(ln);
                else if (!comment)
                    break;
            }
            return out;
        }
    }

    // ---- names ------------------------------------------------------------------------------------------------------
    std::string folded(std::string_view s)
    {
        std::string out(s);
        for (char& c : out)
            c = lower(c);
        return out;
    }

    bool naturalLess(std::string_view a, std::string_view b) noexcept
    {
        std::size_t i = 0, j = 0;
        while (i < a.size() && j < b.size())
        {
            if (digit(a[i]) && digit(b[j]))
            {
                std::size_t i1 = i, j1 = j;
                while (i1 < a.size() && digit(a[i1]))
                    ++i1;
                while (j1 < b.size() && digit(b[j1]))
                    ++j1;
                std::size_t i0 = i, j0 = j;
                while (i0 + 1 < i1 && a[i0] == '0')
                    ++i0;
                while (j0 + 1 < j1 && b[j0] == '0')
                    ++j0;
                if (i1 - i0 != j1 - j0)
                    return i1 - i0 < j1 - j0;
                const int c = a.substr(i0, i1 - i0).compare(b.substr(j0, j1 - j0));
                if (c != 0)
                    return c < 0;
                i = i1;
                j = j1;
                continue;
            }
            const char x = lower(a[i]), y = lower(b[j]);
            if (x != y)
                return x < y;
            ++i;
            ++j;
        }
        if ((i < a.size()) != (j < b.size()))
            return j < b.size();
        return a < b;                                     // ties: the bytes
    }

    // ---- the text helpers -------------------------------------------------------------------------------------------
    bool isLayoutLine(std::string_view line) noexcept
    {
        std::size_t i = 0;
        while (i < line.size() && blank(line[i]))
            ++i;
        return i + 1 < line.size() && line[i] == '*' && line[i + 1] == '@';
    }

    std::string stripLayout(std::string_view text)
    {
        std::string out;
        out.reserve(text.size());
        bool first = true;
        for (const std::string_view l : splitAt(text))
        {
            if (isLayoutLine(l))
                continue;
            if (!first)
                out += '\n';
            out += lfLine(l);                         // CRLF and LF: the same netlist
            first = false;
        }
        if (!out.empty() && out.back() != '\n')
            out += '\n';
        return out;
    }

    bool sameNetlist(std::string_view a, std::string_view b) { return stripLayout(a) == stripLayout(b); }

    std::vector<int> lineMap(std::string_view from, std::string_view to)
    {
        if (!sameNetlist(from, to))
            return {};
        const std::vector<std::string_view> f = splitAt(from), t = splitAt(to);
        std::vector<int> kept;
        for (std::size_t i = 0; i < t.size(); ++i)
            if (!isLayoutLine(t[i]))
                kept.push_back(static_cast<int>(i));
        std::vector<int> out(f.size(), -1);
        std::size_t k = 0;
        for (std::size_t i = 0; i < f.size(); ++i)
        {
            if (isLayoutLine(f[i]))
                continue;
            if (k < kept.size())
                out[i] = kept[k];
            ++k;
        }
        return out;
    }

    std::string sanitised(std::string_view text)
    {
        // funkgui::TextEditor::sanitise, by its rule: the text is read to its first NUL.
        std::string out;
        out.reserve(text.size());
        std::size_t i = 0;
        const std::size_t n = text.find('\0') == std::string_view::npos ? text.size() : text.find('\0');
        while (i < n)
        {
            const auto b0 = static_cast<unsigned char>(text[i]);
            if (b0 == '\r')
            {
                ++i;
                if (i < n && text[i] == '\n')
                    ++i;
                out += '\n';
                continue;
            }
            if (b0 == '\n' || b0 == '\t')
            {
                out += static_cast<char>(b0);
                ++i;
                continue;
            }
            ++i;
            uint32_t cp = b0;
            if (b0 >= 0x80u)
            {
                int extra = 0;
                if ((b0 & 0xE0u) == 0xC0u)
                    extra = 1;
                else if ((b0 & 0xF0u) == 0xE0u)
                    extra = 2;
                else if ((b0 & 0xF8u) == 0xF0u)
                    extra = 3;
                cp = extra == 0 ? 0 : 0x80u;              // any multi-byte code point is no printable ASCII
                for (int k = 0; k < extra; ++k)
                {
                    if (i >= n || (static_cast<unsigned char>(text[i]) & 0xC0u) != 0x80u)
                        break;                            // a broken sequence ends here (never past the NUL)
                    ++i;
                }
            }
            out += cp >= 32u && cp <= 126u ? static_cast<char>(cp) : '?';
        }
        return out;
    }

    bool sameProgram(const Sources& a, const Sources& b)
    {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            if (a[i].name != b[i].name)
                return false;
            if (a[i].text == b[i].text)
                continue;
            const std::string x = sanitised(a[i].text), y = sanitised(b[i].text);
            if (isCircuitFile(a[i].name) ? !sameNetlist(x, y) : x != y)
                return false;
        }
        return true;
    }

    // ---- read -------------------------------------------------------------------------------------------------------
    int Netlist::node(std::string_view name) const noexcept
    {
        const std::string k = nodeKey(name);
        for (std::size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].key == k)
                return static_cast<int>(i);
        return -1;
    }

    int Netlist::element(std::string_view name) const noexcept
    {
        const std::string k = folded(name);
        for (std::size_t i = 0; i < elements.size(); ++i)
            if (folded(elements[i].name) == k)
                return static_cast<int>(i);
        return -1;
    }

    Netlist read(std::string_view text)
    {
        Netlist nl;
        for (const std::string_view l : splitAt(text))
            nl.lines.emplace_back(withoutCr(l));
        const std::vector<Logical> logical = scan(nl.lines, nl.endLine);
        const auto addPin = [&](const Token& t, NodePin p) {
            const std::string k = nodeKey(t.text);
            for (Node& n : nl.nodes)
                if (n.key == k)
                {
                    n.pins.push_back(p);
                    return;
                }
            Node n;
            n.key = k;
            n.spelling = t.text;
            n.ground = k == "0";
            n.pins.push_back(p);
            nl.nodes.push_back(std::move(n));
        };
        for (const Logical& l : logical)
        {
            const std::vector<Token>& t = l.tokens;
            if (t.front().text.front() == '.')
            {
                const std::string word = folded(t.front().text);
                nl.directives.emplace_back(word, l.first);
                if ((word == ".input" || word == ".output") && t.size() >= 3)
                {
                    Terminal term;
                    term.name = t[1].text;
                    term.input = word == ".input";
                    term.nameToken = t[1];
                    term.node = t[2];
                    term.line = l.first;
                    term.tokens = t;
                    if (term.input && t[2].text == "-")
                        nl.laws.push_back(std::move(term));
                    else if (term.input ? !nl.input : !nl.output)
                    {
                        (term.input ? nl.input : nl.output) = term;
                        addPin(term.node, { -1, 0, !term.input, l.first });
                    }
                }
                else if (word == ".pot" && t.size() >= 2)
                {
                    PotLine p;
                    p.name = t[1].text;
                    p.line = l.first;
                    p.tokens = t;
                    for (std::size_t i = 2; i < t.size(); ++i)
                    {
                        if (isTaper(t[i].text))
                            p.taper = t[i].text;
                        else if (i == 2)
                            p.defaultText = t[i].text;
                    }
                    nl.pots.push_back(std::move(p));
                }
                else if (word == ".switch" && t.size() >= 2)
                {
                    SwitchLine w;
                    w.name = t[1].text;
                    w.line = l.first;
                    w.tokens = t;
                    const auto integer = [](const std::string& s) {
                        return !s.empty() && s.size() <= 2 && std::all_of(s.begin(), s.end(), digit) ? std::stoi(s) : 0;
                    };
                    w.positions = t.size() >= 3 ? integer(t[2].text) : 0;
                    w.defaultPosition = t.size() >= 4 ? integer(t[3].text) : 0;
                    nl.switches.push_back(std::move(w));
                }
                else if (word == ".model" && t.size() >= 2)
                    nl.models.emplace_back(t[1].text, l.first);
                continue;
            }
            Element e = elementOf(l);
            if (e.drawn() && nl.element(e.name) >= 0)
                e.why = "a second " + e.name + ", which FMinus refuses";   // the first of a name is drawn
            const int index = static_cast<int>(nl.elements.size());
            for (std::size_t k = 0; k < e.nodes.size(); ++k)
                addPin(e.nodes[k], { index, static_cast<int>(k), false, l.first });
            nl.lastElementLine = l.last;
            nl.elements.push_back(std::move(e));
        }
        return nl;
    }

    // ---- readLayout -------------------------------------------------------------------------------------------------
    Layout readLayout(std::string_view text)
    {
        Layout lay;
        int ln = -1;
        for (const std::string_view raw : splitAt(text))
        {
            ++ln;
            std::string_view l = raw;
            while (!l.empty() && l.back() == '\r')
                l.remove_suffix(1);                       // a comment FMinus never reads: every CR at its end goes
            if (!isLayoutLine(l))
                continue;
            lay.lines.push_back(ln);
            std::vector<std::string> t;
            {
                const std::size_t at = l.find("*@");
                std::size_t i = at + 2;
                while (i < l.size())
                {
                    while (i < l.size() && blank(l[i]))
                        ++i;
                    const std::size_t a = i;
                    while (i < l.size() && !blank(l[i]))
                        ++i;
                    if (i > a)
                        t.emplace_back(l.substr(a, i - a));
                }
            }
            if (t.empty())
                continue;
            const std::string kw = folded(t[0]);
            const std::string where = "line " + std::to_string(ln + 1) + ": ";
            if (kw == "schematic")
            {
                if (lay.version != 0)
                    continue;
                int v = 1;
                if (t.size() >= 2 && !t[1].empty() && t[1].size() <= 4 && std::all_of(t[1].begin(), t[1].end(), digit))
                    v = std::max(1, std::stoi(t[1]));
                else
                    lay.notes.push_back(where + "*@ schematic: '" + (t.size() >= 2 ? t[1] : std::string()) + "' is not a version");
                lay.version = v;
            }
            else if (kw == "input" || kw == "output")
            {
                std::optional<Pt> p;
                if (t.size() >= 3)
                    p = ptOf(t[2]);
                if (t.size() < 3 || !p)
                {
                    lay.notes.push_back(where + "*@ " + kw + ": expected '*@ " + kw + " NAME X,Y'");
                    continue;
                }
                (kw == "input" ? lay.inputs : lay.outputs).push_back({ t[1], *p, ln });
            }
            else if (kw == "part")
            {
                std::optional<Pt> p;
                if (t.size() >= 3)
                    p = ptOf(t[2]);
                if (t.size() < 3 || !p)
                {
                    lay.notes.push_back(where + "*@ part" + (t.size() >= 2 ? " " + t[1] : std::string()) + ": "
                                        + (t.size() >= 3 ? "'" + t[2] + "' is not a position" : "expected '*@ part NAME X,Y'"));
                    continue;
                }
                PartLine pl;
                pl.name = t[1];
                pl.at = *p;
                pl.line = ln;
                for (std::size_t i = 3; i < t.size(); ++i)
                {
                    if (i == 3)
                        if (const std::optional<Orient> o = orientOf(t[i]))
                        {
                            pl.orient = *o;
                            continue;
                        }
                    if (t[i].find('=') != std::string::npos && t[i].front() != '=')
                        pl.extra.push_back(t[i]);
                    else
                        lay.notes.push_back(where + "*@ part " + t[1] + ": '" + t[i] + "' is not an orientation or key=value");
                }
                lay.parts.push_back(std::move(pl));
            }
            else if (kw == "net")
            {
                if (t.size() < 2)
                {
                    lay.notes.push_back(where + "*@ net: expected '*@ net NODE ...'");
                    continue;
                }
                NetLine nlin;
                nlin.node = t[1];
                nlin.line = ln;
                Polyline poly;
                bool bad = false;
                const auto close = [&] {
                    if (!poly.empty())
                    {
                        bool ok = poly.size() >= 2;
                        for (std::size_t k = 1; k < poly.size(); ++k)
                            ok = ok && (poly[k].x == poly[k - 1].x || poly[k].y == poly[k - 1].y);
                        if (ok)
                            nlin.route.push_back(poly);
                        else
                            bad = true;
                    }
                    poly.clear();
                };
                for (std::size_t i = 2; i < t.size(); ++i)
                {
                    const std::string w = folded(t[i]);
                    if (w == "flag" && poly.empty() && nlin.route.empty())
                        nlin.flag = true;
                    else if (w.rfind("label=", 0) == 0)
                    {
                        if (const std::optional<Pt> p = ptOf(std::string_view(t[i]).substr(6)))
                            nlin.labels.push_back(*p);
                        else
                            bad = true;
                    }
                    else if (t[i] == "|")
                        close();
                    else if (const std::optional<Pt> p = ptOf(t[i]))
                        poly.push_back(*p);
                    else
                        bad = true;
                }
                close();
                if (bad)
                    lay.notes.push_back(where + "*@ net " + t[1] + ": a part of its route does not read (it is routed again)");
                lay.nets.push_back(std::move(nlin));
            }
            else
            {
                std::string s = "*@";
                for (const std::string& w : t)
                    s += " " + w;
                lay.unknown.push_back(std::move(s));
            }
        }
        return lay;
    }

    // ---- the drawing ------------------------------------------------------------------------------------------------
    int Drawing::part(std::string_view name) const noexcept
    {
        const std::string k = folded(name);
        for (int pass = 0; pass < 2; ++pass)
            for (std::size_t i = 0; i < parts.size(); ++i)
                if ((parts[i].element >= 0) == (pass == 0) && folded(parts[i].name) == k)
                    return static_cast<int>(i);
        return -1;
    }

    int Drawing::net(std::string_view name) const noexcept
    {
        const std::string k = nodeKey(name);
        for (std::size_t i = 0; i < nets.size(); ++i)
            if (nets[i].key == k)
                return static_cast<int>(i);
        return -1;
    }

    Box Drawing::bounds() const noexcept
    {
        Box b;
        for (const Part& p : parts)
            if (p.placed)
                b = b.joined(p.body);
        for (const Flag& f : flags)
            b = b.joined(f.box);
        for (const Net& n : nets)
            for (const UnitEdge& e : n.edges)
                b = b.joined(Box::of(e.a, e.a + (e.h ? Pt{ 1, 0 } : Pt{ 0, 1 })));
        return b;
    }

    namespace
    {
        void edgesOf(const Polyline& poly, std::vector<UnitEdge>& out)
        {
            for (std::size_t k = 1; k < poly.size(); ++k)
            {
                const Pt a = poly[k - 1], b = poly[k];
                if (a.y == b.y)
                    for (int x = std::min(a.x, b.x); x < std::max(a.x, b.x); ++x)
                        out.push_back({ { x, a.y }, true });
                else if (a.x == b.x)
                    for (int y = std::min(a.y, b.y); y < std::max(a.y, b.y); ++y)
                        out.push_back({ { a.x, y }, false });
            }
        }

        void sortEdges(std::vector<UnitEdge>& e)
        {
            std::sort(e.begin(), e.end());
            e.erase(std::unique(e.begin(), e.end()), e.end());
        }

        // The skeleton of a drawing: its parts (unplaced) and nets from the netlist, styles classified.
        Drawing skeleton(const Netlist& nl)
        {
            Drawing d;
            std::vector<int> partOf(nl.elements.size(), -1);
            for (std::size_t i = 0; i < nl.elements.size(); ++i)
            {
                const Element& e = nl.elements[i];
                if (!e.drawn())
                {
                    d.notDrawn.push_back(e.name + " (" + e.why + ")");
                    continue;
                }
                Part p;
                p.name = e.name;
                p.kind = e.kind;
                p.element = static_cast<int>(i);
                const Symbol& s = symbol(e.kind);
                for (int k = 0; k < s.pinCount; ++k)
                {
                    PinAt pin;
                    pin.name = s.pins[static_cast<std::size_t>(k)].name;
                    pin.token = s.pins[static_cast<std::size_t>(k)].token;
                    pin.net = nl.node(e.nodes[static_cast<std::size_t>(k)].text);
                    p.pins.push_back(std::move(pin));
                }
                partOf[i] = static_cast<int>(d.parts.size());
                d.parts.push_back(std::move(p));
            }
            int inputPart = -1, outputPart = -1;
            for (int t = 0; t < 2; ++t)
            {
                const std::optional<Terminal>& term = t == 0 ? nl.input : nl.output;
                if (!term)
                    continue;
                Part p;
                p.name = term->name;
                p.kind = t == 0 ? Kind::Input : Kind::Output;
                PinAt pin;
                pin.token = 2;
                pin.net = nl.node(term->node.text);
                p.pins.push_back(std::move(pin));
                (t == 0 ? inputPart : outputPart) = static_cast<int>(d.parts.size());
                d.parts.push_back(std::move(p));
            }
            for (const Node& n : nl.nodes)
            {
                Net net;
                net.name = n.spelling;
                net.key = n.key;
                net.totalPins = static_cast<int>(n.pins.size());
                net.style = n.ground ? Style::ground : Style::wired;
                for (const NodePin& p : n.pins)
                {
                    const int part = p.element >= 0 ? partOf[static_cast<std::size_t>(p.element)] : p.output ? outputPart : inputPart;
                    if (part >= 0)
                        net.pins.push_back({ part, p.element >= 0 ? p.pin : 0 });
                }
                d.nets.push_back(std::move(net));
            }
            // Rails: a net a V element ties to ground (not the input's or the output's), positive when its n+ is the
            // rail and its value is above 0, or its n- and below.
            const int inNet = nl.input ? nl.node(nl.input->node.text) : -1;
            const int outNet = nl.output ? nl.node(nl.output->node.text) : -1;
            for (const Element& e : nl.elements)
            {
                if (e.kind != Kind::V || e.nodes.size() != 2)
                    continue;
                const int a = nl.node(e.nodes[0].text), b = nl.node(e.nodes[1].text);
                if (a < 0 || b < 0 || a == b)
                    continue;
                const bool aGround = d.nets[static_cast<std::size_t>(a)].style == Style::ground;
                const bool bGround = d.nets[static_cast<std::size_t>(b)].style == Style::ground;
                if (aGround == bGround)
                    continue;
                const int rail = aGround ? b : a;
                if (rail == inNet || rail == outNet || d.nets[static_cast<std::size_t>(rail)].style == Style::rail)
                    continue;
                const int sign = e.valueToken >= 0 ? signOf(e.tokens[static_cast<std::size_t>(e.valueToken)].text) : 1;
                Net& r = d.nets[static_cast<std::size_t>(rail)];
                r.style = Style::rail;
                r.positive = rail == a ? sign > 0 : sign < 0;
            }
            return d;
        }

        bool signalNet(const Net& n) noexcept { return n.style == Style::wired || n.style == Style::flag; }

        // Flags, junctions and the overlaps, once every part is placed and every net routed.
        void finish(Drawing& d)
        {
            d.flags.clear();
            for (std::size_t ni = 0; ni < d.nets.size(); ++ni)
            {
                Net& n = d.nets[ni];
                n.junctions.clear();
                std::map<Pt, int> degree;
                for (const UnitEdge& e : n.edges)
                {
                    ++degree[e.a];
                    ++degree[e.a + (e.h ? Pt{ 1, 0 } : Pt{ 0, 1 })];
                }
                for (const auto& [p, k] : degree)
                    if (k >= 3)
                        n.junctions.push_back(p);
                if (n.style == Style::wired && !n.unroutable)
                    continue;
                for (const PinRef& r : n.pins)
                {
                    const PinAt& pin = d.parts[static_cast<std::size_t>(r.part)].pins[static_cast<std::size_t>(r.pin)];
                    Flag f;
                    f.pin = r;
                    f.at = pin.at;
                    f.out = pin.out;
                    f.net = static_cast<int>(ni);
                    f.box = flagBox(pin.at, pin.out);
                    d.flags.push_back(f);
                }
            }
            d.overlaps.clear();
            struct Item
            {
                std::string name;
                Box box;
                int part;
                int net;                                  // a flag's; -1 for a body
            };
            std::vector<Item> items;
            for (std::size_t i = 0; i < d.parts.size(); ++i)
                if (d.parts[i].placed)
                    items.push_back({ d.parts[i].name, d.parts[i].body, static_cast<int>(i), -1 });
            for (const Flag& f : d.flags)
            {
                const Part& p = d.parts[static_cast<std::size_t>(f.pin.part)];
                const std::string pin = p.element >= 0 ? "." + p.pins[static_cast<std::size_t>(f.pin.pin)].name : std::string();
                items.push_back({ p.name + pin + "'s flag", f.box, f.pin.part, f.net });
            }
            for (std::size_t i = 0; i < items.size(); ++i)
                for (std::size_t j = i + 1; j < items.size(); ++j)
                {
                    if (items[i].part == items[j].part)
                        continue;                         // a part's own flags touch where its symbol puts pins close
                    if (items[i].net >= 0 && items[i].net == items[j].net)
                        continue;                         // two tags of one net carry one name: they say the same
                    if (items[i].box.meets(items[j].box))
                        d.overlaps.emplace_back(items[i].name, items[j].name);
                }
        }

        // A route's canonical polylines (§4.4.4).
        std::vector<Polyline> chainsOf(const std::vector<UnitEdge>& edges)
        {
            struct Seg
            {
                Pt a, b;
                bool h;
            };
            std::vector<Seg> segs;
            {
                std::vector<UnitEdge> hs, vs;
                for (const UnitEdge& e : edges)
                    (e.h ? hs : vs).push_back(e);
                std::sort(hs.begin(), hs.end(), [](UnitEdge x, UnitEdge y) { return x.a.y < y.a.y || (x.a.y == y.a.y && x.a.x < y.a.x); });
                std::sort(vs.begin(), vs.end(), [](UnitEdge x, UnitEdge y) { return x.a.x < y.a.x || (x.a.x == y.a.x && x.a.y < y.a.y); });
                for (std::size_t i = 0; i < hs.size();)
                {
                    std::size_t j = i;
                    while (j + 1 < hs.size() && hs[j + 1].a.y == hs[i].a.y && hs[j + 1].a.x == hs[j].a.x + 1)
                        ++j;
                    segs.push_back({ hs[i].a, { hs[j].a.x + 1, hs[i].a.y }, true });
                    i = j + 1;
                }
                for (std::size_t i = 0; i < vs.size();)
                {
                    std::size_t j = i;
                    while (j + 1 < vs.size() && vs[j + 1].a.x == vs[i].a.x && vs[j + 1].a.y == vs[j].a.y + 1)
                        ++j;
                    segs.push_back({ vs[i].a, { vs[i].a.x, vs[j].a.y + 1 }, false });
                    i = j + 1;
                }
            }
            // ends meeting at each point, and the points inside a segment
            std::map<Pt, std::vector<std::pair<int, int>>> ends;   // point -> (segment, which end)
            for (std::size_t i = 0; i < segs.size(); ++i)
            {
                ends[segs[i].a].push_back({ static_cast<int>(i), 0 });
                ends[segs[i].b].push_back({ static_cast<int>(i), 1 });
            }
            const auto inside = [&](Pt p) {
                for (const Seg& s : segs)
                {
                    if (s.h && p.y == s.a.y && p.x > s.a.x && p.x < s.b.x)
                        return true;
                    if (!s.h && p.x == s.a.x && p.y > s.a.y && p.y < s.b.y)
                        return true;
                }
                return false;
            };
            std::map<Pt, bool> corner;
            for (const auto& [p, list] : ends)
                corner[p] = list.size() == 2 && segs[static_cast<std::size_t>(list[0].first)].h != segs[static_cast<std::size_t>(list[1].first)].h
                         && !inside(p);
            // the other end of segment s from point p, and the segment across a corner
            const auto other = [&](int s, Pt p) { return segs[static_cast<std::size_t>(s)].a == p ? segs[static_cast<std::size_t>(s)].b : segs[static_cast<std::size_t>(s)].a; };
            const auto across = [&](Pt p, int s) {
                for (const auto& [t, which] : ends[p])
                    if (t != s)
                        return t;
                return -1;
            };
            std::vector<bool> used(segs.size(), false);
            std::vector<Polyline> out;
            // open chains, from an end that is no corner
            std::vector<Pt> starts;
            for (const auto& [p, list] : ends)
                if (!corner[p])
                    starts.push_back(p);
            for (const Pt start : starts)
                for (const auto& [s0, which] : ends[start])
                {
                    if (used[static_cast<std::size_t>(s0)])
                        continue;
                    Polyline poly{ start };
                    int s = s0;
                    Pt at = start;
                    while (s >= 0 && !used[static_cast<std::size_t>(s)])
                    {
                        used[static_cast<std::size_t>(s)] = true;
                        at = other(s, at);
                        poly.push_back(at);
                        s = corner[at] ? across(at, s) : -1;
                    }
                    if (poly.back() < poly.front())
                        std::reverse(poly.begin(), poly.end());
                    out.push_back(std::move(poly));
                }
            // closed loops: from the smallest point, horizontally first
            for (std::size_t i = 0; i < segs.size(); ++i)
            {
                if (used[i])
                    continue;
                // the loop's points
                std::vector<int> loop;
                {
                    int s = static_cast<int>(i);
                    Pt at = segs[i].a;
                    while (s >= 0 && !used[static_cast<std::size_t>(s)])
                    {
                        used[static_cast<std::size_t>(s)] = true;
                        loop.push_back(s);
                        at = other(s, at);
                        s = across(at, s);
                    }
                }
                Pt least = segs[static_cast<std::size_t>(loop.front())].a;
                for (const int s : loop)
                    least = std::min({ least, segs[static_cast<std::size_t>(s)].a, segs[static_cast<std::size_t>(s)].b });
                int s = -1;
                for (const auto& [t, which] : ends[least])
                    if (segs[static_cast<std::size_t>(t)].h)
                        s = t;
                Polyline poly{ least };
                Pt at = least;
                for (std::size_t k = 0; k < loop.size() && s >= 0; ++k)
                {
                    at = other(s, at);
                    poly.push_back(at);
                    s = across(at, s);
                }
                out.push_back(std::move(poly));
            }
            std::sort(out.begin(), out.end());
            return out;
        }

        std::string routeText(const std::vector<UnitEdge>& edges)
        {
            std::string s;
            for (const Polyline& poly : chainsOf(edges))
            {
                s += s.empty() ? "" : " |";
                for (const Pt p : poly)
                    s += " " + ptText(p);
            }
            return s;
        }
    }

    // ---- reconcile --------------------------------------------------------------------------------------------------
    Drawing reconcile(std::string_view text, const SessionPins& pins)
    {
        return reconcile(read(text), readLayout(text), pins);
    }

    Drawing reconcile(const Netlist& nl, const Layout& layout, const SessionPins& pins)
    {
        Drawing d = skeleton(nl);
        d.version = layout.version == 0 ? 1 : layout.version;
        d.readOnly = d.version > 1;
        d.unknown = layout.unknown;
        d.notes = layout.notes;

        // Bind each line to its part, terminal or net by folded name: the first wins.
        const auto bound = [](std::vector<int>& lineOf, int i) { return lineOf[static_cast<std::size_t>(i)] >= 0; };
        std::vector<int> partLine(d.parts.size(), -1);
        std::vector<int> orphanParts;
        for (std::size_t i = 0; i < layout.parts.size(); ++i)
        {
            const PartLine& l = layout.parts[i];
            int p = -1;
            for (std::size_t k = 0; k < d.parts.size(); ++k)
                if (d.parts[k].element >= 0 && folded(d.parts[k].name) == folded(l.name))
                    p = static_cast<int>(k);
            if (p < 0)
                orphanParts.push_back(static_cast<int>(i));
            else if (bound(partLine, p))
                d.notes.push_back("line " + std::to_string(l.line + 1) + ": a second *@ part line for " + d.parts[static_cast<std::size_t>(p)].name
                                  + ": the first wins");
            else
                partLine[static_cast<std::size_t>(p)] = static_cast<int>(i);
        }
        int inputPart = -1, outputPart = -1;
        for (std::size_t k = 0; k < d.parts.size(); ++k)
        {
            if (d.parts[k].kind == Kind::Input)
                inputPart = static_cast<int>(k);
            if (d.parts[k].kind == Kind::Output)
                outputPart = static_cast<int>(k);
        }
        // terminals: (line index into inputs/outputs) bound, and the orphans
        int termLine[2] = { -1, -1 };
        std::vector<int> orphanTerms[2];
        for (int t = 0; t < 2; ++t)
        {
            const std::vector<TerminalLine>& lines = t == 0 ? layout.inputs : layout.outputs;
            const int part = t == 0 ? inputPart : outputPart;
            for (std::size_t i = 0; i < lines.size(); ++i)
            {
                if (part >= 0 && folded(lines[i].name) == folded(d.parts[static_cast<std::size_t>(part)].name))
                {
                    if (termLine[t] >= 0)
                        d.notes.push_back("line " + std::to_string(lines[i].line + 1) + ": a second *@ " + (t == 0 ? "input" : "output")
                                          + " line for " + lines[i].name + ": the first wins");
                    else
                        termLine[t] = static_cast<int>(i);
                }
                else
                    orphanTerms[t].push_back(static_cast<int>(i));
            }
        }
        std::vector<int> netLine(d.nets.size(), -1);
        std::vector<int> orphanNets;
        for (std::size_t i = 0; i < layout.nets.size(); ++i)
        {
            const NetLine& l = layout.nets[i];
            const int n = d.net(l.node);
            if (n < 0)
                orphanNets.push_back(static_cast<int>(i));
            else if (!signalNet(d.nets[static_cast<std::size_t>(n)]))
                continue;                                 // ground and rails are flags: a line for them says nothing
            else if (bound(netLine, n))
                d.notes.push_back("line " + std::to_string(l.line + 1) + ": a second *@ net line for " + d.nets[static_cast<std::size_t>(n)].name
                                  + ": the first wins");
            else
                netLine[static_cast<std::size_t>(n)] = static_cast<int>(i);
        }

        // Orphans adopted: a kind letter with exactly one orphan line and exactly one unplaced part takes it.
        {
            std::map<char, std::vector<int>> orphansOf, unplacedOf;
            for (const int i : orphanParts)
                orphansOf[lower(layout.parts[static_cast<std::size_t>(i)].name.empty() ? '?' : layout.parts[static_cast<std::size_t>(i)].name[0])].push_back(i);
            for (std::size_t k = 0; k < d.parts.size(); ++k)
                if (d.parts[k].element >= 0 && partLine[k] < 0)
                    unplacedOf[lower(d.parts[k].name[0])].push_back(static_cast<int>(k));
            for (const auto& [c, lines] : orphansOf)
            {
                const auto u = unplacedOf.find(c);
                if (lines.size() == 1 && u != unplacedOf.end() && u->second.size() == 1)
                {
                    partLine[static_cast<std::size_t>(u->second.front())] = lines.front();
                    continue;
                }
                std::string names;
                for (const int i : lines)
                    names += (names.empty() ? "" : ", ") + layout.parts[static_cast<std::size_t>(i)].name;
                std::string placed;
                if (u != unplacedOf.end())
                    for (const int k : u->second)
                        placed += (placed.empty() ? "" : ", ") + d.parts[static_cast<std::size_t>(k)].name;
                d.notes.push_back("*@ part " + names + (lines.size() == 1 ? " names" : " name") + " no part (dropped at the next write)"
                                  + (placed.empty() ? std::string() : ": " + placed + " placed again"));
            }
            for (int t = 0; t < 2; ++t)
            {
                const int part = t == 0 ? inputPart : outputPart;
                const std::vector<TerminalLine>& lines = t == 0 ? layout.inputs : layout.outputs;
                if (orphanTerms[t].size() == 1 && part >= 0 && termLine[t] < 0)
                    termLine[t] = orphanTerms[t].front();
                else
                    for (const int i : orphanTerms[t])
                        d.notes.push_back("*@ " + std::string(t == 0 ? "input " : "output ") + lines[static_cast<std::size_t>(i)].name
                                          + " names no terminal (dropped at the next write)");
            }
            std::vector<int> unrouted;
            for (std::size_t n = 0; n < d.nets.size(); ++n)
                if (d.nets[n].style == Style::wired && netLine[n] < 0 && d.nets[n].pins.size() >= 2)
                    unrouted.push_back(static_cast<int>(n));
            if (orphanNets.size() == 1 && unrouted.size() == 1)
                netLine[static_cast<std::size_t>(unrouted.front())] = orphanNets.front();
            else
                for (const int i : orphanNets)
                    d.notes.push_back("*@ net " + layout.nets[static_cast<std::size_t>(i)].node + " names no net (dropped at the next write)");
        }

        // Positions: the lines', then the session's.
        // A point past kPartLimit (its wires could not be written) is not taken: the part is placed again.
        const auto inReach = [&](const std::string& kw, const std::string& name, Pt at, int line) {
            if (withinReach(at))
                return true;
            d.notes.push_back((line >= 0 ? "line " + std::to_string(line + 1) + ": " : std::string()) + "*@ " + kw + " " + name + ": "
                              + farOut(at) + ": placed again");
            return false;
        };
        for (std::size_t k = 0; k < d.parts.size(); ++k)
        {
            Part& p = d.parts[k];
            if (p.element >= 0 && partLine[k] >= 0)
            {
                const PartLine& l = layout.parts[static_cast<std::size_t>(partLine[k])];
                if (!inReach("part", p.name, l.at, l.line))
                    continue;
                put(p, l.at, l.orient);
                p.extra = l.extra;
                p.source = Source::stored;
            }
            else if (p.element < 0)
            {
                const int t = p.kind == Kind::Input ? 0 : 1;
                if (termLine[t] >= 0)
                {
                    const TerminalLine& l = (t == 0 ? layout.inputs : layout.outputs)[static_cast<std::size_t>(termLine[t])];
                    if (!inReach(t == 0 ? "input" : "output", p.name, l.at, l.line))
                        continue;
                    put(p, l.at, Orient::r0);
                    p.source = Source::stored;
                }
            }
        }
        for (Part& p : d.parts)
        {
            if (p.placed)
                continue;
            if (p.element >= 0)
            {
                for (const PartLine& l : pins.parts)
                    if (folded(l.name) == folded(p.name) && withinReach(l.at))
                    {
                        put(p, l.at, l.orient);
                        p.extra = l.extra;
                        p.source = Source::session;
                        break;
                    }
            }
            else
                for (const TerminalLine& l : p.kind == Kind::Input ? pins.inputs : pins.outputs)
                    if (folded(l.name) == folded(p.name) && withinReach(l.at))
                    {
                        put(p, l.at, Orient::r0);
                        p.source = Source::session;
                        break;
                    }
        }
        // Nets: their style and their stored routes.
        for (std::size_t n = 0; n < d.nets.size(); ++n)
        {
            if (netLine[n] < 0)
                continue;
            const NetLine& l = layout.nets[static_cast<std::size_t>(netLine[n])];
            Net& net = d.nets[n];
            net.stored = true;
            net.labels = l.labels;
            std::sort(net.labels.begin(), net.labels.end());
            if (l.flag)
                net.style = Style::flag;
            else
            {
                for (const Polyline& poly : l.route)
                    edgesOf(poly, net.edges);
                sortEdges(net.edges);
            }
        }

        // The rest placed: the full layout when nothing is, else incremental placement; then every net repaired.
        const bool any = std::any_of(d.parts.begin(), d.parts.end(), [](const Part& p) { return p.placed; });
        if (!any)
            layoutAll(d, nl);
        else
            placeRest(d, nl);
        // within reach (a ladder of a thousand sections, an island right of a drawing at the edge): the nearest point
        for (Part& p : d.parts)
            if (p.placed && !withinReach(p.origin))
                put(p, { std::clamp(p.origin.x, -kPartLimit, kPartLimit), std::clamp(p.origin.y, -kPartLimit, kPartLimit) }, p.orient);
        repairAll(d, d.budget);
        if (d.budget.wide)
        {
            Box b;
            for (const Part& p : d.parts)
                b = b.joined(p.body);
            d.notes.push_back("the parts span " + std::to_string(b.x1 - b.x0 + 1) + " x " + std::to_string(b.y1 - b.y0 + 1)
                              + " units, past what the router wires (" + std::to_string(kRouteCells)
                              + " grid points): every net is drawn as name tags");
        }
        else
            for (const Net& n : d.nets)
                if (n.unroutable)
                    d.notes.push_back("net " + n.name + ": no route within the budget: drawn as name tags");
        finish(d);
        return d;
    }

    // ---- writing ----------------------------------------------------------------------------------------------------
    Layout layoutOf(const Netlist& nl, const Drawing& d)
    {
        (void)nl;
        Layout lay;
        lay.version = d.version;
        for (const Part& p : d.parts)
        {
            if (!p.placed)
                continue;
            if (p.element >= 0)
                lay.parts.push_back({ p.name, p.origin, p.orient, p.extra, -1 });
            else
                (p.kind == Kind::Input ? lay.inputs : lay.outputs).push_back({ p.name, p.origin, -1 });
        }
        for (const Net& n : d.nets)
        {
            if (!signalNet(n) || n.pins.empty())
                continue;
            NetLine l;
            l.node = n.name;
            l.labels = n.labels;
            if (n.style == Style::flag || n.unroutable)
                l.flag = true;
            else
            {
                if (n.pins.size() < 2 || n.edges.empty())
                    continue;
                l.route = chainsOf(n.edges);
            }
            lay.nets.push_back(std::move(l));
        }
        lay.unknown = d.unknown;
        return lay;
    }

    std::string writeBlock(const Netlist& nl, const Drawing& d)
    {
        Layout lay = layoutOf(nl, d);
        std::string out = std::string(kHeader) + "\n";
        for (const TerminalLine& t : lay.inputs)
            out += "*@ input " + t.name + " " + ptText(t.at) + "\n";
        for (const TerminalLine& t : lay.outputs)
            out += "*@ output " + t.name + " " + ptText(t.at) + "\n";
        std::sort(lay.parts.begin(), lay.parts.end(), [](const PartLine& a, const PartLine& b) { return naturalLess(a.name, b.name); });
        for (const PartLine& p : lay.parts)
        {
            out += "*@ part " + p.name + " " + ptText(p.at);
            if (p.orient != Orient::r0)
                out += std::string(" ") + orientName(p.orient);
            for (const std::string& e : p.extra)
                out += " " + e;
            out += "\n";
        }
        std::sort(lay.nets.begin(), lay.nets.end(), [](const NetLine& a, const NetLine& b) { return naturalLess(a.node, b.node); });
        for (const NetLine& n : lay.nets)
        {
            out += "*@ net " + n.node;
            if (n.flag)
                out += " flag";
            std::vector<Pt> labels = n.labels;
            std::sort(labels.begin(), labels.end());
            for (const Pt p : labels)
                out += " label=" + ptText(p);
            std::vector<UnitEdge> edges;
            for (const Polyline& poly : n.route)
                edgesOf(poly, edges);
            sortEdges(edges);
            out += routeText(edges) + "\n";
        }
        for (const std::string& u : lay.unknown)
            out += u + "\n";
        return out;
    }

    std::string withBlock(std::string_view text, std::string_view block)
    {
        std::vector<std::string_view> lines = splitAt(text);
        if (!text.empty() && text.back() == '\n')
            lines.pop_back();                             // the empty piece after the last line break
        if (text.empty())
            lines.clear();
        // every line as it is written (lfLine): a CRLF text comes out as the same text in LF, as the editor keeps a text
        // (TextEditor::sanitise), not with its block's lines alone in LF
        std::vector<std::string_view> kept;
        for (const std::string_view l : lines)
            if (!isLayoutLine(l))
                kept.push_back(lfLine(l));
        // the first .end, as FMinus finds it in the lines written
        std::size_t at = kept.size();
        for (std::size_t i = 0; i < kept.size(); ++i)
        {
            bool cont = false, comment = false;
            const std::vector<Token> t = scanLine(kept[i], 0, cont, comment);
            if (!comment && !cont && !t.empty() && folded(t.front().text) == ".end")
            {
                at = i;
                break;
            }
        }
        if (at == 0 && !kept.empty())
            at = 1;                                       // never line 1 (ngspice's title)
        std::string out;
        out.reserve(text.size() + block.size());
        for (std::size_t i = 0; i < kept.size(); ++i)
        {
            if (i == at)
                out += block;
            out += kept[i];
            out += '\n';
        }
        if (at >= kept.size())
            out += block;
        return out;
    }

    std::string written(std::string_view text, const SessionPins& pins)
    {
        const Netlist nl = read(text);
        const Drawing d = reconcile(nl, readLayout(text), pins);
        if (d.readOnly)
            return std::string(text);
        return withBlock(text, writeBlock(nl, d));
    }

    // ---- names for new parts and nodes ------------------------------------------------------------------------------
    namespace
    {
        bool nameTaken(const Netlist& nl, std::string_view name, const std::set<std::string>& also = {})
        {
            const std::string f = folded(name);
            if (also.count(f) != 0)
                return true;
            if (nl.element(name) >= 0)
                return true;
            for (const PotLine& p : nl.pots)
                if (folded(p.name) == f)
                    return true;
            for (const SwitchLine& s : nl.switches)
                if (folded(s.name) == f)
                    return true;
            for (const std::optional<Terminal>* t : { &nl.input, &nl.output })
                if (*t && folded((*t)->name) == f)
                    return true;
            for (const Terminal& t : nl.laws)
                if (folded(t.name) == f)
                    return true;
            return false;
        }

        std::string freshNodeAvoiding(const Netlist& nl, const std::set<std::string>& also)
        {
            for (int k = 1;; ++k)
            {
                const std::string n = "n" + std::to_string(k);
                if (nl.node(n) < 0 && also.count(n) == 0)
                    return n;
            }
        }

        std::string freshName(const Netlist& nl, std::string_view prefix, const std::set<std::string>& also = {})
        {
            for (int k = 1;; ++k)
            {
                const std::string n = std::string(prefix) + std::to_string(k);
                if (!nameTaken(nl, n, also))
                    return n;
            }
        }
    }

    std::string freshNode(const Netlist& nl) { return freshNodeAvoiding(nl, {}); }

    std::vector<std::string> placementLines(const Netlist& nl, Kind kind, std::string* name)
    {
        std::set<std::string> taken;
        const auto node = [&] {
            const std::string n = freshNodeAvoiding(nl, taken);
            taken.insert(n);
            return n;
        };
        const auto nodes = [&](int k) {
            std::string s;
            for (int i = 0; i < k; ++i)
                s += " " + node();
            return s;
        };
        std::vector<std::string> out;
        std::string n;
        switch (kind)
        {
            case Kind::R: n = freshName(nl, "R"); out.push_back(n + nodes(2) + " 10k"); break;
            case Kind::C: n = freshName(nl, "C"); out.push_back(n + nodes(2) + " 100n"); break;
            case Kind::L: n = freshName(nl, "L"); out.push_back(n + nodes(2) + " 10m"); break;
            case Kind::D: n = freshName(nl, "D"); out.push_back(n + nodes(2) + " 1N4148"); break;
            case Kind::Pot:
            case Kind::Rheostat:
            {
                int k = 1;
                while (nameTaken(nl, "P" + std::to_string(k)) || nameTaken(nl, "pot" + std::to_string(k)))
                    ++k;
                n = "P" + std::to_string(k);
                const std::string pot = "pot" + std::to_string(k);
                out.push_back(".pot " + pot + " 0.5 lin");
                out.push_back(n + nodes(kind == Kind::Pot ? 3 : 2) + " 100k {" + pot + "}");
                break;
            }
            case Kind::OpAmp: n = freshName(nl, "XU"); out.push_back(n + nodes(3) + " opamp"); break;
            case Kind::Vcvs: n = freshName(nl, "E"); out.push_back(n + nodes(4) + " 10"); break;
            case Kind::Vccs: n = freshName(nl, "G"); out.push_back(n + nodes(4) + " 1m"); break;
            case Kind::Ota:
            {
                n = freshName(nl, "G");
                const std::string a = node(), b = node(), c = node(), e = node();
                out.push_back(n + " " + a + " " + b + " OTA " + c + " " + e + " {100u}");
                break;
            }
            case Kind::V: n = freshName(nl, "V"); out.push_back(n + nodes(2) + " DC 9"); break;
            case Kind::I: n = freshName(nl, "I"); out.push_back(n + nodes(2) + " DC 1m"); break;
            case Kind::S:
            {
                int k = 1;
                while (nameTaken(nl, "S" + std::to_string(k)) || nameTaken(nl, "sw" + std::to_string(k)))
                    ++k;
                n = "S" + std::to_string(k);
                const std::string sw = "sw" + std::to_string(k);
                out.push_back(".switch " + sw + " 2 0");
                out.push_back(n + nodes(2) + " " + sw + " 1");
                break;
            }
            case Kind::Input:
                n = nl.input ? std::string() : "u";
                if (!n.empty())
                    out.push_back(".input " + n + " " + node());
                break;
            case Kind::Output:
                n = nl.output ? std::string() : "vout";
                if (!n.empty())
                    out.push_back(".output " + n + " " + node());
                break;
            case Kind::Unknown: break;
        }
        if (name != nullptr)
            *name = n;
        return out;
    }

    // ---- edits ------------------------------------------------------------------------------------------------------
    namespace
    {
        // The edits that make `before` into `after`: whole lines matched by their longest common subsequence, a run of
        // changed lines replaced by as many patched between their common prefix and suffix, any other run whole. Past
        // kLcsCells (thousands of lines between the first and the last change) the lines between them are one run.
        constexpr std::size_t kLcsCells = std::size_t{ 1 } << 22;

        std::vector<Edit> diffEdits(std::string_view before, std::string_view after)
        {
            std::vector<Edit> out;
            if (before == after)
                return out;
            const std::vector<std::string_view> A = splitAt(before), B = splitAt(after);
            const std::size_t nA = A.size(), nB = B.size();
            std::size_t pre = 0;
            while (pre < nA && pre < nB && A[pre] == B[pre])
                ++pre;
            std::size_t suf = 0;
            while (suf < nA - pre && suf < nB - pre && A[nA - 1 - suf] == B[nB - 1 - suf])
                ++suf;
            const std::size_t a0 = pre, a1 = nA - suf, b0 = pre, b1 = nB - suf;
            const std::size_t n = a1 - a0, m = b1 - b0;
            const bool whole = (n + 1) > kLcsCells / (m + 1);
            std::vector<int> lcs(whole ? 0 : (n + 1) * (m + 1), 0);
            const auto L = [&](std::size_t i, std::size_t j) -> int& { return lcs[i * (m + 1) + j]; };
            if (!whole)
                for (std::size_t i = n; i-- > 0;)
                    for (std::size_t j = m; j-- > 0;)
                        L(i, j) = A[a0 + i] == B[b0 + j] ? L(i + 1, j + 1) + 1 : std::max(L(i + 1, j), L(i, j + 1));
            struct Hunk
            {
                std::size_t i0, i1, j0, j1;
            };
            std::vector<Hunk> hunks;
            if (whole)
                hunks.push_back({ a0, a1, b0, b1 });
            else
            {
                std::size_t i = 0, j = 0;
                Hunk h{ a0, a0, b0, b0 };
                bool open = false;
                const auto close = [&] {
                    if (open)
                        hunks.push_back(h);
                    open = false;
                };
                while (i < n || j < m)
                {
                    if (i < n && j < m && A[a0 + i] == B[b0 + j])
                    {
                        close();
                        ++i;
                        ++j;
                        continue;
                    }
                    if (!open)
                    {
                        h = { a0 + i, a0 + i, b0 + j, b0 + j };
                        open = true;
                    }
                    if (j >= m || (i < n && L(i + 1, j) >= L(i, j + 1)))
                        h.i1 = a0 + ++i;
                    else
                        h.j1 = b0 + ++j;
                }
                close();
            }
            const auto lenOf = [&](std::size_t i) { return static_cast<int>(A[i].size()); };
            for (const Hunk& h : hunks)
            {
                if (h.i1 - h.i0 == h.j1 - h.j0)
                {
                    for (std::size_t k = 0; k < h.i1 - h.i0; ++k)
                    {
                        const std::string_view a = A[h.i0 + k], b = B[h.j0 + k];
                        if (a == b)
                            continue;                     // (a whole run's line that did not change)
                        std::size_t c = 0;
                        while (c < a.size() && c < b.size() && a[c] == b[c])
                            ++c;
                        std::size_t d = 0;
                        while (d < a.size() - c && d < b.size() - c && a[a.size() - 1 - d] == b[b.size() - 1 - d])
                            ++d;
                        out.push_back({ { static_cast<int>(h.i0 + k), static_cast<int>(c) },
                                        { static_cast<int>(h.i0 + k), static_cast<int>(a.size() - d) },
                                        std::string(b.substr(c, b.size() - c - d)) });
                    }
                    continue;
                }
                std::string text;
                if (h.i1 < nA)
                {
                    for (std::size_t j = h.j0; j < h.j1; ++j)
                        text += std::string(B[j]) + "\n";
                    out.push_back({ { static_cast<int>(h.i0), 0 }, { static_cast<int>(h.i1), 0 }, std::move(text) });
                }
                else if (h.i0 > 0)
                {
                    for (std::size_t j = h.j0; j < h.j1; ++j)
                        text += "\n" + std::string(B[j]);
                    out.push_back({ { static_cast<int>(h.i0 - 1), lenOf(h.i0 - 1) }, { static_cast<int>(nA - 1), lenOf(nA - 1) }, std::move(text) });
                }
                else
                {
                    for (std::size_t j = h.j0; j < h.j1; ++j)
                        text += (j == h.j0 ? "" : "\n") + std::string(B[j]);
                    out.push_back({ { 0, 0 }, { static_cast<int>(nA - 1), lenOf(nA - 1) }, std::move(text) });
                }
            }
            return out;
        }

        // ---- the text, line by line ----------------------------------------------------------------------------
        struct Lines
        {
            std::vector<std::string> lines;
            bool lastBreak = false;                       // the text ends in a line break

            explicit Lines(std::string_view text)
            {
                lines = linesOf(text);
                lastBreak = !text.empty() && text.back() == '\n';
                if (lastBreak)
                    lines.pop_back();
                if (text.empty())
                    lines.clear();
            }

            std::string text() const
            {
                std::string out = joined(lines);
                if (lastBreak && !lines.empty())
                    out += '\n';
                return out;
            }
        };

        // `text` with these tokens replaced (positions as read() gives them).
        std::string patched(std::string_view text, std::vector<std::pair<Token, std::string>> repl)
        {
            Lines l(text);
            std::sort(repl.begin(), repl.end(), [](const auto& a, const auto& b) { return b.first.at < a.first.at; });
            for (const auto& [t, s] : repl)
            {
                if (t.at.line < 0 || t.at.line >= static_cast<int>(l.lines.size()))
                    continue;
                std::string& line = l.lines[static_cast<std::size_t>(t.at.line)];
                const std::size_t a = std::min(line.size(), static_cast<std::size_t>(t.at.col));
                const std::size_t n = std::min(line.size() - a, static_cast<std::size_t>(t.len));
                line.replace(a, n, s);
            }
            return l.text();
        }

        std::string withoutLines(std::string_view text, const std::set<int>& gone)
        {
            Lines l(text);
            std::vector<std::string> kept;
            for (std::size_t i = 0; i < l.lines.size(); ++i)
                if (gone.count(static_cast<int>(i)) == 0)
                    kept.push_back(l.lines[i]);
            l.lines = std::move(kept);
            return l.text();
        }

        // `text` with `add` before line `at` (past the last: at the end).
        std::string withLines(std::string_view text, int at, const std::vector<std::string>& add)
        {
            Lines l(text);
            const std::size_t i = static_cast<std::size_t>(std::clamp(at, 0, static_cast<int>(l.lines.size())));
            if (i == l.lines.size())
                l.lastBreak = true;
            l.lines.insert(l.lines.begin() + static_cast<std::ptrdiff_t>(i), add.begin(), add.end());
            return l.text();
        }

        // A line's tokens, alone (an op's netlist line).
        std::vector<Token> tokensOf(std::string_view line, bool& ok)
        {
            bool cont = false, comment = false;
            std::vector<Token> t = joinAssignments(scanLine(line, 0, cont, comment));
            ok = !comment && !cont && !t.empty();
            return t;
        }

        struct Work
        {
            std::string text;
            Layout layout;
            Netlist nl;
            Drawing d;

            void redraw()
            {
                nl = read(text);
                d = reconcile(nl, layout);
                layout = layoutOf(nl, d);
            }

            PartLine* partLine(std::string_view name)
            {
                for (PartLine& p : layout.parts)
                    if (folded(p.name) == folded(name))
                        return &p;
                return nullptr;
            }

            NetLine* netLine(std::string_view node)
            {
                for (NetLine& n : layout.nets)
                    if (nodeKey(n.node) == nodeKey(node))
                        return &n;
                return nullptr;
            }

            TerminalLine* terminalLine(bool input, std::string_view name)
            {
                for (TerminalLine& t : input ? layout.inputs : layout.outputs)
                    if (folded(t.name) == folded(name))
                        return &t;
                return nullptr;
            }

            // Every node token of `key` (folded) spelled `to`.
            void renameNode(const std::string& key, const std::string& to)
            {
                std::vector<std::pair<Token, std::string>> repl;
                for (const Element& e : nl.elements)
                    for (const Token& t : e.nodes)
                        if (nodeKey(t.text) == key)
                            repl.emplace_back(t, to);
                for (const std::optional<Terminal>* t : { &nl.input, &nl.output })
                    if (*t && nodeKey((*t)->node.text) == key)
                        repl.emplace_back((*t)->node, to);
                text = patched(text, std::move(repl));
            }

            // Net line `from` merged into net line `into` (their routes and labels), `from` gone.
            void mergeLines(const std::string& from, const std::string& into, const std::vector<Pt>& path)
            {
                std::vector<Polyline> route;
                std::vector<Pt> labels;
                for (auto it = layout.nets.begin(); it != layout.nets.end();)
                    if (nodeKey(it->node) == nodeKey(from))
                    {
                        route.insert(route.end(), it->route.begin(), it->route.end());
                        labels.insert(labels.end(), it->labels.begin(), it->labels.end());
                        it = layout.nets.erase(it);
                    }
                    else
                        ++it;
                if (path.size() >= 2)
                    route.push_back(path);
                if (route.empty() && labels.empty())
                    return;
                NetLine* l = netLine(into);
                if (l == nullptr)
                {
                    layout.nets.push_back({ into, false, {}, {}, -1 });
                    l = &layout.nets.back();
                }
                l->route.insert(l->route.end(), route.begin(), route.end());
                l->labels.insert(l->labels.end(), labels.begin(), labels.end());
            }
        };

        std::string pinNames(Kind k)
        {
            std::string s;
            for (int i = 0; i < symbol(k).pinCount; ++i)
                s += (s.empty() ? "" : ", ") + std::string(symbol(k).pins[static_cast<std::size_t>(i)].name);
            return s;
        }

        // An end of connect: its net (-2: ground, which the netlist does not have yet), and its pin when it is one.
        struct Resolved
        {
            int net = -1;
            int part = -1, pin = -1;
            std::string label;                            // as the result says it ("R4.2", "n3")
        };

        std::string resolve(const Work& w, const End& e, Resolved& r)
        {
            if (!e.part.empty())
            {
                const int p = w.d.part(e.part);
                if (p < 0)
                {
                    const std::string whole = e.part + "." + e.pin;
                    if (const int n = w.nl.node(whole); n >= 0)
                    {
                        r.net = n;
                        r.label = whole;
                        return {};
                    }
                    if (const int el = w.nl.element(e.part); el >= 0)
                        return e.part + " is not drawn (" + w.nl.elements[static_cast<std::size_t>(el)].why + ")";
                    return "there is no part " + e.part;
                }
                const Part& part = w.d.parts[static_cast<std::size_t>(p)];
                int pin = part.element >= 0 ? pinIndex(part.kind, e.pin) : (e.pin.empty() || e.pin == "1" || e.pin == "2" ? 0 : -1);
                if (pin < 0)
                    return part.name + " has no pin '" + e.pin + "' (its pins: " + (part.element >= 0 ? pinNames(part.kind) : std::string("its one")) + ")";
                r.part = p;
                r.pin = pin;
                r.net = part.pins[static_cast<std::size_t>(pin)].net;
                r.label = part.name + (part.element >= 0 ? "." + part.pins[static_cast<std::size_t>(pin)].name : std::string());
                return {};
            }
            const int n = w.nl.node(e.node);
            r.label = e.node;
            if (n >= 0)
            {
                r.net = n;
                return {};
            }
            if (nodeKey(e.node) == "0")
            {
                r.net = -2;
                return {};
            }
            return "there is no node " + e.node;
        }

        // The node a pin is on: its token.
        Token pinToken(const Work& w, int part, int pin)
        {
            const Part& p = w.d.parts[static_cast<std::size_t>(part)];
            if (p.element >= 0)
                return w.nl.elements[static_cast<std::size_t>(p.element)].nodes[static_cast<std::size_t>(pin)];
            return (p.kind == Kind::Input ? w.nl.input : w.nl.output)->node;
        }

        std::string netsOf(const Work& w, const Part& p)
        {
            std::string s;
            for (const PinAt& pin : p.pins)
                if (pin.net >= 0)
                    s += (s.empty() ? "" : ", ") + w.d.nets[static_cast<std::size_t>(pin.net)].name;
            return s;
        }

        std::string where(const Part& p)
        {
            return ptText(p.origin) + (p.orient == Orient::r0 ? std::string() : std::string(" ") + orientName(p.orient));
        }

        // ---- the ops -------------------------------------------------------------------------------------------
        std::string doAdd(Work& w, const Op& op, Applied& out, std::vector<std::string>& placedNames)
        {
            if (op.lines.empty())
                return "nothing to add";
            if (op.at && !withinReach(*op.at))
                return farOut(*op.at);
            std::vector<std::string> directives;
            std::string element, elementName;
            for (const std::string& line : op.lines)
            {
                bool ok = false;
                const std::vector<Token> t = tokensOf(line, ok);
                if (!ok)
                    return "'" + line + "' is no netlist line";
                const std::string& head = t.front().text;
                if (head.front() == '.')
                {
                    const std::string word = folded(head);
                    const bool known = word == ".pot" || word == ".switch" || word == ".model" || word == ".input" || word == ".output";
                    if (!known)
                        return "add takes element lines and .pot, .switch, .model, .input and .output lines";
                    if (t.size() < (word == ".input" || word == ".output" ? 3u : 2u))
                        return "'" + line + "' is not a whole " + word + " line";
                    const std::string& name = t[1].text;
                    if (word == ".model")
                    {
                        for (const auto& [m, l] : w.nl.models)
                            if (folded(m) == folded(name))
                                return "model " + name + " exists";
                    }
                    else if (nameTaken(w.nl, name))
                        return "the name " + name + " is taken";
                    if (word == ".input" && t[2].text != "-" && w.nl.input)
                        return "the netlist has its input (" + w.nl.input->name + "): FMinus takes exactly one signal input";
                    if (word == ".output" && w.nl.output)
                        return "the netlist has its output (" + w.nl.output->name + ")";
                    directives.push_back(line);
                    continue;
                }
                if (!element.empty())
                    return "one element a line";
                Logical l;
                l.tokens = t;
                const Element e = elementOf(l);
                if (!e.drawn())
                    return e.name + ": " + e.why + " (the schematic draws R C L D P X E G V I S)";
                if (w.nl.element(e.name) >= 0)
                    return e.name + " exists";
                element = line;
                elementName = e.name;
            }
            if (!element.empty())
            {
                const int at = w.nl.lastElementLine >= 0 ? w.nl.lastElementLine + 1 : w.nl.endLine >= 0 ? w.nl.endLine : 1 << 30;
                std::vector<std::string> lines = directives;
                lines.push_back(element);
                w.text = withLines(w.text, at, lines);
                if (op.at)
                {
                    if (PartLine* old = w.partLine(elementName))
                        *old = { elementName, *op.at, op.orient.value_or(Orient::r0), {}, -1 };
                    else
                        w.layout.parts.push_back({ elementName, *op.at, op.orient.value_or(Orient::r0), {}, -1 });
                    placedNames.push_back(elementName);
                }
            }
            else
                for (const std::string& line : directives)
                {
                    bool ok = false;
                    const std::vector<Token> t = tokensOf(line, ok);
                    const std::string word = folded(t.front().text);
                    int at = -1;
                    if (word == ".model")
                    {
                        for (const auto& [m, l] : w.nl.models)
                            at = std::max(at, l + 1);
                        if (at < 0 && w.nl.lastElementLine >= 0)
                            at = w.nl.lastElementLine + 1;
                    }
                    else
                    {
                        for (const auto& [d, l] : w.nl.directives)
                            if (d == word)
                                at = std::max(at, l + 1);
                        if (at < 0 && !w.nl.elements.empty())
                            at = w.nl.elements.front().firstLine;
                    }
                    if (at < 0)
                        at = w.nl.endLine >= 0 ? w.nl.endLine : 1 << 30;
                    w.text = withLines(w.text, at, { line });
                    w.nl = read(w.text);
                    if (op.at && (word == ".input" || word == ".output") && t[2].text != "-")
                    {
                        (word == ".input" ? w.layout.inputs : w.layout.outputs).push_back({ t[1].text, *op.at, -1 });
                        placedNames.push_back(t[1].text);
                    }
                }
            w.redraw();
            const std::string name = !elementName.empty() ? elementName : std::string();
            if (const int p = name.empty() ? -1 : w.d.part(name); p >= 0)
            {
                const Part& part = w.d.parts[static_cast<std::size_t>(p)];
                out.said.push_back("added " + part.name + " at " + where(part) + " on " + netsOf(w, part));
                out.touched.push_back(part.name);
            }
            else
                for (const std::string& line : directives)
                    out.said.push_back("added " + line);
            return {};
        }

        std::string doPlace(Work& w, const Op& op, Applied& out, std::vector<std::string>& placedNames)
        {
            const int p = w.d.part(op.name);
            if (p < 0)
            {
                if (const int el = w.nl.element(op.name); el >= 0)
                    return op.name + " is not drawn (" + w.nl.elements[static_cast<std::size_t>(el)].why + ")";
                return "there is no part " + op.name;
            }
            if (!op.at)
                return "place needs X,Y";
            if (!withinReach(*op.at))
                return farOut(*op.at);
            const Part part = w.d.parts[static_cast<std::size_t>(p)];
            if (part.element >= 0)
            {
                const Orient o = op.orient.value_or(part.orient);
                if (PartLine* l = w.partLine(part.name))
                {
                    l->at = *op.at;
                    l->orient = o;
                }
                else
                    w.layout.parts.push_back({ part.name, *op.at, o, part.extra, -1 });
            }
            else
            {
                if (op.orient && *op.orient != Orient::r0)
                    return "a terminal does not turn";
                if (TerminalLine* l = w.terminalLine(part.kind == Kind::Input, part.name))
                    l->at = *op.at;
                else
                    (part.kind == Kind::Input ? w.layout.inputs : w.layout.outputs).push_back({ part.name, *op.at, -1 });
            }
            placedNames.push_back(part.name);
            w.redraw();
            const Part& now = w.d.parts[static_cast<std::size_t>(w.d.part(part.name))];
            out.said.push_back("placed " + now.name + " at " + where(now));
            out.touched.push_back(now.name);
            return {};
        }

        // The name the merge of nets a and b keeps (§6.3): ground; a node a directive names (the input's first); a
        // user's name over a generated n<k>; the earlier first mention. a, b: nodes, or -2 for ground not written yet.
        int kept(const Work& w, int a, int b)
        {
            const auto ground = [&](int x) { return x == -2 || w.nl.nodes[static_cast<std::size_t>(x)].ground; };
            const int in = w.nl.input ? w.nl.node(w.nl.input->node.text) : -1;
            const int outN = w.nl.output ? w.nl.node(w.nl.output->node.text) : -1;
            if (ground(a))
                return a;
            if (ground(b))
                return b;
            if (a == in)
                return a;
            if (b == in)
                return b;
            if (a == outN)
                return a;
            if (b == outN)
                return b;
            const bool ga = generatedNode(w.nl.nodes[static_cast<std::size_t>(a)].spelling);
            const bool gb = generatedNode(w.nl.nodes[static_cast<std::size_t>(b)].spelling);
            if (ga != gb)
                return ga ? b : a;
            return std::min(a, b);
        }

        // Nets a and b merged under `keep`'s name (a, b nodes or -2), the drawn path joining the route.
        void merge(Work& w, int a, int b, int keep, const std::vector<Pt>& path)
        {
            const int gone = keep == a ? b : a;
            const std::string to = keep == -2 ? std::string("0") : w.nl.nodes[static_cast<std::size_t>(keep)].spelling;
            const std::string goneName = w.nl.nodes[static_cast<std::size_t>(gone)].spelling;
            w.renameNode(w.nl.nodes[static_cast<std::size_t>(gone)].key, to);
            const bool keptSignal = keep >= 0 && w.d.nets[static_cast<std::size_t>(keep)].style != Style::ground
                                 && w.d.nets[static_cast<std::size_t>(keep)].style != Style::rail;
            if (keptSignal)
                w.mergeLines(goneName, to, path);
            else                                          // ground and rails are flags: the absorbed net's line goes
                w.layout.nets.erase(std::remove_if(w.layout.nets.begin(), w.layout.nets.end(),
                                                   [&](const NetLine& l) { return nodeKey(l.node) == nodeKey(goneName); }),
                                    w.layout.nets.end());
        }

        std::string doConnect(Work& w, const Op& op, Applied& out)
        {
            Resolved a, b;
            if (std::string why = resolve(w, op.a, a); !why.empty())
                return why;
            if (std::string why = resolve(w, op.b, b); !why.empty())
                return why;
            if (a.net == b.net)
            {
                if (a.net >= 0 && op.path.size() >= 2 && w.d.nets[static_cast<std::size_t>(a.net)].style == Style::wired)
                {
                    w.mergeLines("", w.nl.nodes[static_cast<std::size_t>(a.net)].spelling, op.path);
                    w.redraw();
                }
                out.said.push_back(a.label + " and " + b.label + " are on one net already");
                return {};
            }
            if (a.net < 0 && a.net != -2)
                return a.label + " is on no net";
            if (b.net < 0 && b.net != -2)
                return b.label + " is on no net";
            const int keep = kept(w, a.net, b.net);
            const int gone = keep == a.net ? b.net : a.net;
            const std::string goneName = w.nl.nodes[static_cast<std::size_t>(gone)].spelling;
            const std::string keptName = keep == -2 ? std::string("0") : w.nl.nodes[static_cast<std::size_t>(keep)].spelling;
            merge(w, a.net, b.net, keep, op.path);
            w.redraw();
            out.said.push_back("connected " + a.label + " and " + b.label + ": " + goneName + " is " + keptName + " now");
            out.touched.push_back(keptName);
            return {};
        }

        std::string doDisconnect(Work& w, const Op& op, Applied& out)
        {
            Resolved r;
            if (op.a.part.empty())
                return "disconnect takes Part.pin";
            if (std::string why = resolve(w, op.a, r); !why.empty())
                return why;
            if (r.part < 0)
                return "disconnect takes Part.pin";
            const Net& net = w.d.nets[static_cast<std::size_t>(r.net)];
            if (net.totalPins <= 1)
            {
                out.said.push_back(r.label + " is alone on " + net.name + " already");
                return {};
            }
            const std::string fresh = freshNode(w.nl);
            w.text = patched(w.text, { { pinToken(w, r.part, r.pin), fresh } });
            w.redraw();
            out.said.push_back("disconnected " + r.label + ": it is on " + fresh + " now");
            out.touched.push_back(fresh);
            return {};
        }

        std::string doCut(Work& w, const Op& op, Applied& out)
        {
            const int x = w.d.net(op.name);
            if (x < 0)
                return "there is no net " + op.name;
            const Net net = w.d.nets[static_cast<std::size_t>(x)];
            if (net.style == Style::ground || net.style == Style::rail)
                return net.name + " is drawn as flags: it has no wire to cut";
            std::vector<UnitEdge> cut;
            edgesOf(op.path, cut);
            std::vector<UnitEdge> rest;
            for (const UnitEdge& e : net.edges)
                if (std::find(cut.begin(), cut.end(), e) == cut.end())
                    rest.push_back(e);
            // the pieces of its pins under the wires that stay (name tags join all theirs)
            std::map<Pt, Pt> parent;
            const auto find = [&](Pt p) {
                if (parent.find(p) == parent.end())
                    parent[p] = p;
                while (!(parent[p] == p))
                    p = parent[p] = parent[parent[p]];
                return p;
            };
            const auto unite = [&](Pt a, Pt b) {
                const Pt ra = find(a), rb = find(b);
                if (!(ra == rb))
                    parent[std::max(ra, rb)] = std::min(ra, rb);
            };
            for (const UnitEdge& e : rest)
                unite(e.a, e.a + (e.h ? Pt{ 1, 0 } : Pt{ 0, 1 }));
            std::vector<Pt> pinAt;
            for (const PinRef& r : net.pins)
                pinAt.push_back(w.d.parts[static_cast<std::size_t>(r.part)].pins[static_cast<std::size_t>(r.pin)].at);
            const bool tags = net.style == Style::flag || net.unroutable;
            if (tags)
                for (const Pt p : pinAt)
                    unite(pinAt.front(), p);
            for (const Pt l : net.labels)
                if (parent.find(l) != parent.end())
                    unite(net.labels.front(), l);
            if (!net.labels.empty())
                for (const Pt l : net.labels)
                    if (parent.find(l) != parent.end())
                        for (const Pt p : pinAt)
                            if (find(p) == find(l))
                                unite(net.labels.front(), p);
            // the anchor (as the router's): the input's pin, else the output's, else the first pin in line order
            int anchor = -1;
            for (int pass = 0; pass < 2 && anchor < 0; ++pass)
                for (std::size_t k = 0; k < net.pins.size() && anchor < 0; ++k)
                    if (w.d.parts[static_cast<std::size_t>(net.pins[k].part)].kind == (pass == 0 ? Kind::Input : Kind::Output))
                        anchor = static_cast<int>(k);
            anchor = std::max(anchor, 0);
            if (net.pins.empty())
                return net.name + " has no pin drawn";
            const Pt home = find(pinAt[static_cast<std::size_t>(anchor)]);
            // each other piece a fresh node, in the order of its first pin
            std::vector<Pt> roots;
            std::vector<std::pair<Token, std::string>> repl;
            std::set<std::string> taken;
            std::map<Pt, std::string> nameOf;
            for (std::size_t k = 0; k < net.pins.size(); ++k)
            {
                const Pt r = find(pinAt[k]);
                if (r == home)
                    continue;
                if (nameOf.find(r) == nameOf.end())
                {
                    nameOf[r] = freshNodeAvoiding(w.nl, taken);
                    taken.insert(nameOf[r]);
                }
                repl.emplace_back(pinToken(w, net.pins[k].part, net.pins[k].pin), nameOf[r]);
            }
            // the route: each piece's wires go to its net
            if (NetLine* l = w.netLine(net.name))
            {
                l->route.clear();
                std::vector<UnitEdge> mine;
                std::map<std::string, std::vector<UnitEdge>> theirs;
                for (const UnitEdge& e : rest)
                {
                    const Pt r = find(e.a);
                    if (const auto it = nameOf.find(r); it != nameOf.end())
                        theirs[it->second].push_back(e);
                    else
                        mine.push_back(e);
                }
                for (const UnitEdge& e : mine)
                    l->route.push_back({ e.a, e.a + (e.h ? Pt{ 1, 0 } : Pt{ 0, 1 }) });
                for (const auto& [name, edges] : theirs)
                {
                    NetLine n{ name, false, {}, {}, -1 };
                    for (const UnitEdge& e : edges)
                        n.route.push_back({ e.a, e.a + (e.h ? Pt{ 1, 0 } : Pt{ 0, 1 }) });
                    w.layout.nets.push_back(std::move(n));
                }
            }
            if (repl.empty())
            {
                w.redraw();
                out.said.push_back("cut " + net.name + ": it still holds together");
                return {};
            }
            w.text = patched(w.text, std::move(repl));
            w.redraw();
            std::string names;
            for (const auto& [r, n] : nameOf)
                names += (names.empty() ? "" : ", ") + n;
            out.said.push_back("cut " + net.name + ": " + names + " split off");
            out.touched.push_back(net.name);
            return {};
        }

        // The pots an element's {expr} tokens name, in order, once each.
        std::vector<std::string> potsOf(const Netlist& nl, const Element& e)
        {
            std::vector<std::string> out;
            for (const Token& t : e.tokens)
            {
                if (t.text.empty() || t.text.front() != '{')
                    continue;
                for (const auto& [a, n] : wordsIn(t.text))
                {
                    const std::string word = t.text.substr(a, n);
                    for (const PotLine& p : nl.pots)
                        if (folded(p.name) == folded(word) && std::find(out.begin(), out.end(), p.name) == out.end())
                            out.push_back(p.name);
                }
            }
            return out;
        }

        // The pot a pot's {expr} is alone ({tone}: tone, as FMinus's Expr::Op::Name), "" when it is more than a name.
        std::string potAlone(const Netlist& nl, const Element& e)
        {
            for (const Token& t : e.tokens)
            {
                if (t.text.empty() || t.text.front() != '{')
                    continue;
                std::string inner = t.text.substr(1, t.text.back() == '}' && t.text.size() >= 2 ? t.text.size() - 2 : t.text.size() - 1);
                while (!inner.empty() && blank(inner.back()))
                    inner.pop_back();
                std::size_t a = 0;
                while (a < inner.size() && blank(inner[a]))
                    ++a;
                inner = inner.substr(a);
                for (const PotLine& p : nl.pots)
                    if (folded(p.name) == folded(inner))
                        return p.name;
                return {};
            }
            return {};
        }

        // Every {expr} reference to pot `from` spelled `to`.
        std::vector<std::pair<Token, std::string>> potRenames(const Netlist& nl, const std::string& from, const std::string& to)
        {
            std::vector<std::pair<Token, std::string>> repl;
            for (const Element& e : nl.elements)
                for (const Token& t : e.tokens)
                {
                    if (t.text.empty() || t.text.front() != '{')
                        continue;
                    std::string s = t.text;
                    bool changed = false;
                    const auto words = wordsIn(t.text);
                    for (auto it = words.rbegin(); it != words.rend(); ++it)
                        if (folded(t.text.substr(it->first, it->second)) == folded(from))
                        {
                            s.replace(it->first, it->second, to);
                            changed = true;
                        }
                    if (changed)
                        repl.emplace_back(t, s);
                }
            return repl;
        }

        bool goodValue(std::string_view v)
        {
            if (!v.empty() && v.front() == '{')
                return v.size() >= 2 && v.back() == '}';
            return spiceNumber(v);
        }

        std::string doSet(Work& w, const Op& op, Applied& out)
        {
            std::string key = folded(op.key);
            if (key == "value")
                key.clear();
            const std::string& v = op.value;
            if (v.empty())
                return "set needs a value";
            std::vector<std::pair<Token, std::string>> repl;
            std::string said;
            const int ei = w.nl.element(op.name);
            if (ei < 0)
            {
                // a terminal: its scale
                const std::optional<Terminal>* term = nullptr;
                for (const std::optional<Terminal>* t : { &w.nl.input, &w.nl.output })
                    if (*t && folded((*t)->name) == folded(op.name))
                        term = t;
                if (term == nullptr)
                    return "there is no part " + op.name;
                if (!key.empty() && key != "scale")
                    return "a terminal takes scale=";
                if (!spiceNumber(v))
                    return "'" + v + "' is not a number";
                const Terminal& t = **term;
                Token at;
                for (const Token& x : t.tokens)
                    if (folded(x.text).rfind("scale=", 0) == 0)
                        at = x;
                if (at.len > 0)
                    repl.emplace_back(at, "scale=" + v);
                else
                {
                    Token end = t.tokens.back();
                    end.at.col += end.len;
                    end.len = 0;
                    repl.emplace_back(end, " scale=" + v);
                }
                w.text = patched(w.text, std::move(repl));
                w.redraw();
                out.said.push_back("set " + t.name + "'s scale to " + v);
                out.touched.push_back(t.name);
                return {};
            }
            const Element e = w.nl.elements[static_cast<std::size_t>(ei)];   // a copy: redraw() replaces w.nl
            const auto token = [&](int i) -> std::optional<Token> {
                if (i < 0 || i >= static_cast<int>(e.tokens.size()))
                    return std::nullopt;
                return e.tokens[static_cast<std::size_t>(i)];
            };
            const auto setToken = [&](int i, const std::string& text) -> std::string {
                if (const std::optional<Token> t = token(i))
                {
                    repl.emplace_back(*t, text);
                    return {};
                }
                if (i == static_cast<int>(e.tokens.size()))
                {
                    Token end = e.tokens.back();
                    end.at.col += end.len;
                    end.len = 0;
                    repl.emplace_back(end, " " + text);
                    return {};
                }
                return e.name + "'s line is too short to set";
            };
            // where its value is, or goes: a half-typed line with none ("R1 a b") gets it at its end
            const int valueAt = e.valueToken >= 0 ? e.valueToken : static_cast<int>(e.tokens.size());
            const Kind k = e.kind;
            int brace = -1;
            for (std::size_t i = 1; i < e.tokens.size(); ++i)
                if (!e.tokens[i].text.empty() && e.tokens[i].text.front() == '{')
                {
                    brace = static_cast<int>(i);
                    break;
                }
            if (key.empty())
            {
                switch (k)
                {
                    case Kind::R:
                    case Kind::C:
                    case Kind::L:
                    case Kind::V:
                    case Kind::I:
                    case Kind::Vcvs:
                    case Kind::Vccs:
                    case Kind::Ota:
                        if (!goodValue(v))
                            return "'" + v + "' is not a SPICE number or {expr}";
                        if (std::string why = setToken(valueAt, v); !why.empty())
                            return why;
                        said = e.name + "'s value";
                        break;
                    case Kind::Pot:
                    case Kind::Rheostat:
                        if (!goodValue(v))
                            return "'" + v + "' is not a SPICE number or {expr}";
                        repl.emplace_back(e.tokens[static_cast<std::size_t>(v.front() == '{' ? brace : e.valueToken)], v);
                        said = e.name + (v.front() == '{' ? "'s expression" : "'s value");
                        break;
                    case Kind::D:
                        if (!nodeName(v))
                            return "'" + v + "' is not a model's name";
                        if (std::string why = setToken(3, v); !why.empty())
                            return why;
                        said = e.name + "'s model";
                        break;
                    case Kind::S:
                        key = "positions";
                        break;
                    case Kind::OpAmp:
                        return e.name + " (an op amp) has no value";
                    case Kind::Input:
                    case Kind::Output:
                    case Kind::Unknown:
                        return e.name + " is not drawn: set it in the netlist";
                }
            }
            if (!key.empty())
            {
                if (key == "model")
                {
                    if (k != Kind::D)
                        return "model= is a diode's";
                    if (!nodeName(v))
                        return "'" + v + "' is not a model's name";
                    if (std::string why = setToken(3, v); !why.empty())
                        return why;
                    said = e.name + "'s model";
                }
                else if (key == "gain")
                {
                    if (k != Kind::Vcvs && k != Kind::Vccs && k != Kind::Ota)
                        return "gain= is an E's, a G's or an OTA's";
                    if (!goodValue(v))
                        return "'" + v + "' is not a SPICE number or {expr}";
                    if (std::string why = setToken(valueAt, v); !why.empty())
                        return why;
                    said = e.name + "'s gain";
                }
                else if (key == "positions")
                {
                    if (k != Kind::S)
                        return "positions= is a switch's";
                    const bool ok = !v.empty() && std::all_of(v.begin(), v.end(), [](char c) { return digit(c) || c == ','; });
                    if (!ok)
                        return "'" + v + "' is not a list of positions, as 0,2";
                    // the positions: token 4 up to the first key=value
                    Token first, last;
                    for (std::size_t i = 4; i < e.tokens.size() && e.tokens[i].text.find('=') == std::string::npos; ++i)
                    {
                        if (i == 4)
                            first = e.tokens[i];
                        last = e.tokens[i];
                    }
                    if (first.len == 0)
                    {
                        if (std::string why = setToken(4, v); !why.empty())
                            return why;
                    }
                    else
                    {
                        Token span = first;
                        if (last.at.line == first.at.line)
                            span.len = last.at.col + last.len - first.at.col;
                        repl.emplace_back(span, v);
                    }
                    said = e.name + "'s positions";
                }
                else if (key == "taper")
                {
                    if (k != Kind::Pot && k != Kind::Rheostat)
                        return "taper= is a pot's";
                    if (!isTaper(v))
                        return "a taper is lin, audio or rlog";
                    // where FMinus reads it (taperOf): the line's taper=, else its .pot's word when its {expr} is that
                    // pot alone; with neither, a taper= goes on the line (a .pot's word would not reach this part)
                    Token own;
                    for (const Token& t : e.tokens)
                        if (folded(t.text).rfind("taper=", 0) == 0)
                            own = t;
                    if (own.len > 0)
                        repl.emplace_back(own, "taper=" + v);
                    const std::string alone = potAlone(w.nl, e);
                    if (alone.empty() && own.len == 0)
                    {
                        Token end = e.tokens.back();
                        end.at.col += end.len;
                        end.len = 0;
                        repl.emplace_back(end, " taper=" + v);
                    }
                    for (const PotLine& p : w.nl.pots)
                        if (!alone.empty() && folded(p.name) == folded(alone))
                        {
                            Token at;
                            for (std::size_t i = 2; i < p.tokens.size(); ++i)
                                if (isTaper(p.tokens[i].text))
                                    at = p.tokens[i];
                            if (at.len > 0)
                                repl.emplace_back(at, v);
                            else
                            {
                                Token end = p.tokens.back();
                                end.at.col += end.len;
                                end.len = 0;
                                repl.emplace_back(end, (p.tokens.size() == 2 ? " 0.5 " : " ") + v);
                            }
                        }
                    said = e.name + "'s taper";
                }
                else if (key == "pot")
                {
                    if (k != Kind::Pot && k != Kind::Rheostat)
                        return "pot= is a pot's";
                    if (!plainName(v))
                        return "'" + v + "' is not a pot's name (a letter, then letters and digits)";
                    const std::vector<std::string> pots = potsOf(w.nl, e);
                    if (pots.empty())
                        return e.name + "'s {expr} names no .pot";
                    if (folded(pots.front()) != folded(v) && nameTaken(w.nl, v))
                        return "the name " + v + " is taken";
                    for (const PotLine& p : w.nl.pots)
                        if (folded(p.name) == folded(pots.front()))
                            repl.emplace_back(p.tokens[1], v);
                    const auto refs = potRenames(w.nl, pots.front(), v);
                    repl.insert(repl.end(), refs.begin(), refs.end());
                    said = "pot " + pots.front() + " renamed";
                }
                else if (key == "switch")
                {
                    if (k != Kind::S)
                        return "switch= is a switch's";
                    if (!plainName(v))
                        return "'" + v + "' is not a switch's name (a letter, then letters and digits)";
                    if (e.tokens.size() < 4)
                        return e.name + " names no switch";
                    const std::string from = e.tokens[3].text;
                    if (folded(from) != folded(v) && nameTaken(w.nl, v))
                        return "the name " + v + " is taken";
                    for (const SwitchLine& s : w.nl.switches)
                        if (folded(s.name) == folded(from))
                            repl.emplace_back(s.tokens[1], v);
                    for (const Element& x : w.nl.elements)
                        if (x.kind == Kind::S && x.tokens.size() >= 4 && folded(x.tokens[3].text) == folded(from))
                            repl.emplace_back(x.tokens[3], v);
                    said = "switch " + from + " renamed";
                }
                else if (key == "scale")
                    return "scale= is a terminal's";
                else
                    return "set takes a value, or model=, taper=, pot=, gain=, switch=, positions= or scale=";
            }
            w.text = patched(w.text, std::move(repl));
            w.redraw();
            out.said.push_back("set " + said + " to " + v);
            out.touched.push_back(e.name);
            return {};
        }

        std::string doRename(Work& w, const Op& op, Applied& out)
        {
            const std::string& to = op.to;
            if (to.empty())
                return "rename needs a new name";
            if (const int ei = w.nl.element(op.name); ei >= 0)
            {
                const Element e = w.nl.elements[static_cast<std::size_t>(ei)];   // a copy: redraw() replaces w.nl
                if (lower(to.front()) != e.letter)
                    return "a new name keeps the kind's letter (" + std::string(1, static_cast<char>(e.letter - 'a' + 'A')) + ")";
                if (to.size() < 2 || !std::all_of(to.begin() + 1, to.end(), [](char c) { return letter(c) || digit(c) || c == '_'; }))
                    return "'" + to + "' is not a name: letters, digits and _ after the kind's letter";
                if (const int other = w.nl.element(to); other >= 0 && other != ei)
                    return to + " exists";
                w.text = patched(w.text, { { e.tokens.front(), to } });
                if (PartLine* l = w.partLine(e.name))
                    l->name = to;
                w.redraw();
                out.said.push_back("renamed " + e.name + " to " + to);
                out.touched.push_back(to);
                return {};
            }
            for (const std::optional<Terminal>* t : { &w.nl.input, &w.nl.output })
                if (*t && folded((*t)->name) == folded(op.name))
                {
                    const Terminal term = **t;
                    if (!plainName(to))
                        return "'" + to + "' is not a terminal's name (a letter, then letters and digits)";
                    if (folded(to) != folded(term.name) && nameTaken(w.nl, to))
                        return "the name " + to + " is taken";
                    w.text = patched(w.text, { { term.nameToken, to } });
                    if (TerminalLine* l = w.terminalLine(term.input, term.name))
                        l->name = to;
                    w.redraw();
                    out.said.push_back("renamed " + term.name + " to " + to);
                    out.touched.push_back(to);
                    return {};
                }
            const int n = w.nl.node(op.name);
            if (n < 0)
                return "there is no part, terminal or node " + op.name;
            if (!nodeName(to))
                return "'" + to + "' is not a node's name (letters, digits and _)";
            const std::string from = w.nl.nodes[static_cast<std::size_t>(n)].spelling;
            const int other = w.nl.node(to);
            if (other >= 0 && other != n)
            {
                // a name another net has: the two merge under it
                merge(w, n, other, other, {});
                w.redraw();
                out.said.push_back("renamed " + from + " to " + to + ": the nets merged");
                out.touched.push_back(to);
                return {};
            }
            if (other < 0 && nodeKey(to) == "0")
            {
                merge(w, n, -2, -2, {});
                w.redraw();
                out.said.push_back("renamed " + from + " to " + to + ": it is ground now");
                return {};
            }
            w.renameNode(w.nl.nodes[static_cast<std::size_t>(n)].key, to);
            if (NetLine* l = w.netLine(from))
                l->node = to;
            w.redraw();
            out.said.push_back("renamed " + from + " to " + to);
            out.touched.push_back(to);
            return {};
        }

        std::string doRemove(Work& w, const Op& op, Applied& out)
        {
            if (op.names.empty())
                return "delete needs a part";
            std::set<int> gone;
            std::set<int> elements;
            for (const std::string& name : op.names)
            {
                if (const int ei = w.nl.element(name); ei >= 0)
                {
                    const Element& e = w.nl.elements[static_cast<std::size_t>(ei)];
                    for (const int l : ownLines(w.nl.lines, e.firstLine, e.lastLine))
                        gone.insert(l);
                    elements.insert(ei);
                    w.layout.parts.erase(std::remove_if(w.layout.parts.begin(), w.layout.parts.end(), [&](const PartLine& p) { return folded(p.name) == folded(name); }),
                                         w.layout.parts.end());
                    continue;
                }
                bool found = false;
                for (const std::optional<Terminal>* t : { &w.nl.input, &w.nl.output })
                    if (*t && folded((*t)->name) == folded(name))
                    {
                        const Terminal& term = **t;
                        int last = term.line;
                        for (const Token& x : term.tokens)
                            last = std::max(last, x.at.line);
                        for (const int l : ownLines(w.nl.lines, term.line, last))
                            gone.insert(l);
                        auto& lines = term.input ? w.layout.inputs : w.layout.outputs;
                        lines.erase(std::remove_if(lines.begin(), lines.end(), [&](const TerminalLine& x) { return folded(x.name) == folded(name); }), lines.end());
                        found = true;
                    }
                if (!found)
                    return "there is no part " + name;
            }
            // a .pot or .switch line nothing else uses goes with them
            std::set<std::string> potsLeft, switchesLeft;
            for (std::size_t i = 0; i < w.nl.elements.size(); ++i)
            {
                if (elements.count(static_cast<int>(i)) != 0)
                    continue;
                const Element& e = w.nl.elements[i];
                for (const std::string& p : potsOf(w.nl, e))
                    potsLeft.insert(folded(p));
                if (e.letter == 's' && e.tokens.size() >= 4)
                    switchesLeft.insert(folded(e.tokens[3].text));
            }
            for (const int i : elements)
            {
                const Element& e = w.nl.elements[static_cast<std::size_t>(i)];
                for (const std::string& p : potsOf(w.nl, e))
                    if (potsLeft.count(folded(p)) == 0)
                        for (const PotLine& x : w.nl.pots)
                            if (folded(x.name) == folded(p))
                                gone.insert(x.line);
                if (e.letter == 's' && e.tokens.size() >= 4 && switchesLeft.count(folded(e.tokens[3].text)) == 0)
                    for (const SwitchLine& x : w.nl.switches)
                        if (folded(x.name) == folded(e.tokens[3].text))
                            gone.insert(x.line);
            }
            w.text = withoutLines(w.text, gone);
            w.redraw();
            std::string names;
            for (const std::string& n : op.names)
                names += (names.empty() ? "" : ", ") + n;
            out.said.push_back("deleted " + names);
            out.touched.insert(out.touched.end(), op.names.begin(), op.names.end());
            return {};
        }

        std::string doLabel(Work& w, const Op& op, Applied& out, bool style)
        {
            const int n = w.d.net(op.name);
            if (n < 0)
                return "there is no net " + op.name;
            const Net net = w.d.nets[static_cast<std::size_t>(n)];   // a copy: redraw() replaces w.d
            if (net.style == Style::ground || net.style == Style::rail)
                return net.name + " is drawn as flags always";
            NetLine* l = w.netLine(net.name);
            if (l == nullptr)
            {
                w.layout.nets.push_back({ net.name, false, {}, {}, -1 });
                l = &w.layout.nets.back();
            }
            if (style)
            {
                l->flag = op.flag;
                if (op.flag)
                    l->route.clear();
                out.said.push_back(net.name + (op.flag ? " drawn as name tags" : " drawn as wires"));
            }
            else
            {
                l->labels = op.points;
                out.said.push_back(net.name + "'s name tags placed");
            }
            w.redraw();
            out.touched.push_back(net.name);
            return {};
        }

        std::string doLayout(Work& w, const Op& op, Applied& out)
        {
            if (op.names.empty())
            {
                w.layout.parts.clear();
                w.layout.inputs.clear();
                w.layout.outputs.clear();
                std::vector<NetLine> kept;
                for (const NetLine& l : w.layout.nets)
                    if (l.flag)
                        kept.push_back({ l.node, true, {}, {}, -1 });
                w.layout.nets = std::move(kept);
                w.redraw();
                out.said.push_back("laid everything out again");
                return {};
            }
            for (const std::string& name : op.names)
            {
                const int p = w.d.part(name);
                if (p < 0)
                    return "there is no part " + name;
                const Part& part = w.d.parts[static_cast<std::size_t>(p)];
                if (part.element >= 0)
                    w.layout.parts.erase(std::remove_if(w.layout.parts.begin(), w.layout.parts.end(), [&](const PartLine& l) { return folded(l.name) == folded(name); }),
                                         w.layout.parts.end());
                else
                {
                    auto& lines = part.kind == Kind::Input ? w.layout.inputs : w.layout.outputs;
                    lines.erase(std::remove_if(lines.begin(), lines.end(), [&](const TerminalLine& l) { return folded(l.name) == folded(name); }), lines.end());
                }
            }
            w.redraw();
            std::string said;
            for (const std::string& name : op.names)
                if (const int p = w.d.part(name); p >= 0)
                    said += (said.empty() ? "" : ", ") + w.d.parts[static_cast<std::size_t>(p)].name + " at " + where(w.d.parts[static_cast<std::size_t>(p)]);
            out.said.push_back("placed again: " + said);
            out.touched.insert(out.touched.end(), op.names.begin(), op.names.end());
            return {};
        }

        // Paste: element, .pot, .switch and .model lines and *@ part and net lines; names that collide take the next
        // free one, every node but ground a fresh one, the positions offset to `at`.
        std::string doPaste(Work& w, const Op& op, Applied& out)
        {
            std::string pastedText;
            for (const std::string& l : op.lines)
                pastedText += l + "\n";
            const Netlist src = read(pastedText);
            const Layout lay = readLayout(pastedText);
            if (src.elements.empty())
                return "nothing to paste: no element line";
            for (const auto& [word, line] : src.directives)
                if (word != ".pot" && word != ".switch" && word != ".model")
                    return "paste takes element, .pot, .switch and .model lines (and *@ lines), not " + word;
            // text that is no netlist of drawn parts ("hello world" reads as an element H..., "Lorem ipsum dolor sit"
            // as an inductor of 'sit'): refused whole. A part's value is a SPICE number or {expr}, a diode's model a
            // .model's (pasted, or the netlist's) or a built-in's, a switch's name a .switch's.
            const auto known = [&](const std::vector<std::pair<std::string, int>>& a, const std::vector<std::pair<std::string, int>>& b,
                                   const std::string& name) {
                for (const auto* list : { &a, &b })
                    for (const auto& [m, l] : *list)
                        if (folded(m) == folded(name))
                            return true;
                return false;
            };
            for (const Element& e : src.elements)
            {
                std::string why = e.drawn() ? std::string() : e.why;
                const std::string value = e.valueToken >= 0 && e.valueToken < static_cast<int>(e.tokens.size())
                                              ? e.tokens[static_cast<std::size_t>(e.valueToken)].text
                                              : std::string();
                if (!why.empty())
                    return "paste takes the lines of parts the schematic draws, and '" + e.name + "' starts no such line (" + why + ")";
                if (e.kind == Kind::D)
                {
                    const std::string f = folded(value);
                    if (value.empty())
                        why = "a diode with no model";
                    else if (f != "1n914" && f != "1n4148" && !known(src.models, w.nl.models, value))
                        why = "no .model " + value;
                }
                else if (e.kind == Kind::S)
                {
                    bool found = false;
                    if (e.tokens.size() >= 4)
                        for (const std::vector<SwitchLine>* list : { &src.switches, &std::as_const(w.nl).switches })
                            for (const SwitchLine& x : *list)
                                found = found || folded(x.name) == folded(e.tokens[3].text);
                    if (!found)
                        why = e.tokens.size() >= 4 ? "no .switch " + e.tokens[3].text : std::string("a switch with no .switch");
                }
                else if (e.kind != Kind::OpAmp)
                {
                    if (value.empty())
                        why = "no value";
                    else if (!goodValue(value))
                        why = "'" + value + "' is no SPICE number or {expr}";
                }
                if (!why.empty())
                    return "paste takes the lines of parts the schematic draws, and '" + e.name + "' starts no such line (" + why + ")";
            }
            std::set<std::string> taken;
            const auto fresh = [&](const std::string& name) {
                std::string base = name;
                while (!base.empty() && digit(base.back()))
                    base.pop_back();
                if (base.empty())
                    base = name;
                if (!nameTaken(w.nl, name, taken))
                {
                    taken.insert(folded(name));
                    return name;
                }
                const std::string n = freshName(w.nl, base, taken);
                taken.insert(folded(n));
                return n;
            };
            std::map<std::string, std::string> names, pots, switches, nodes;
            for (const PotLine& p : src.pots)
                pots[folded(p.name)] = fresh(p.name);
            for (const SwitchLine& s : src.switches)
                switches[folded(s.name)] = fresh(s.name);
            for (const Element& e : src.elements)
                names[folded(e.name)] = fresh(e.name);
            std::set<std::string> nodeTaken;
            for (const Node& n : src.nodes)
            {
                if (n.ground)
                    continue;
                const std::string f = freshNodeAvoiding(w.nl, nodeTaken);
                nodeTaken.insert(f);
                nodes[n.key] = f;
            }
            // the lines rewritten
            std::vector<std::string> lines;
            for (const auto& [word, line] : src.directives)
            {
                std::vector<std::pair<Token, std::string>> repl;
                // the directive's own physical lines (a .model's + lines with it), its name patched where it is
                const std::vector<int> own = directiveLines(src.lines, line);
                std::string text;
                for (const int l : own)
                    text += (text.empty() ? "" : "\n") + src.lines[static_cast<std::size_t>(l)];
                if (word == ".pot")
                    for (const PotLine& p : src.pots)
                        if (p.line == line)
                            repl.emplace_back(p.tokens[1], pots[folded(p.name)]);
                if (word == ".switch")
                    for (const SwitchLine& s : src.switches)
                        if (s.line == line)
                            repl.emplace_back(s.tokens[1], switches[folded(s.name)]);
                if (word == ".model")
                {
                    bool exists = false;
                    for (const auto& [m, l] : src.models)
                        if (l == line)
                            for (const auto& [m2, l2] : w.nl.models)
                                exists = exists || folded(m) == folded(m2);
                    if (exists)
                        continue;                         // the netlist's own .model of that name serves
                }
                for (auto& r : repl)
                {
                    const auto it = std::find(own.begin(), own.end(), r.first.at.line);
                    r.first.at.line = it == own.end() ? 0 : static_cast<int>(it - own.begin());
                }
                for (const std::string& l : linesOf(patched(text, std::move(repl))))
                    lines.push_back(l);
            }
            for (const Element& e : src.elements)
            {
                std::vector<std::pair<Token, std::string>> repl;
                repl.emplace_back(e.tokens.front(), names[folded(e.name)]);
                for (const Token& t : e.nodes)
                    if (nodeKey(t.text) != "0")
                        repl.emplace_back(t, nodes[nodeKey(t.text)]);
                for (const Token& t : e.tokens)
                {
                    if (!t.text.empty() && t.text.front() == '{')
                    {
                        std::string s = t.text;
                        const auto words = wordsIn(t.text);
                        for (auto it = words.rbegin(); it != words.rend(); ++it)
                            if (const auto p = pots.find(folded(t.text.substr(it->first, it->second))); p != pots.end())
                                s.replace(it->first, it->second, p->second);
                        if (s != t.text)
                            repl.emplace_back(t, s);
                    }
                }
                if (e.kind == Kind::S && e.tokens.size() >= 4)
                    if (const auto s = switches.find(folded(e.tokens[3].text)); s != switches.end())
                        repl.emplace_back(e.tokens[3], s->second);
                // the element's own physical lines, its tokens moved onto them
                std::string text;
                for (const int l : ownLines(src.lines, e.firstLine, e.lastLine))
                    text += (text.empty() ? "" : "\n") + src.lines[static_cast<std::size_t>(l)];
                const std::vector<int> own = ownLines(src.lines, e.firstLine, e.lastLine);
                for (auto& r : repl)
                {
                    const auto it = std::find(own.begin(), own.end(), r.first.at.line);
                    r.first.at.line = it == own.end() ? 0 : static_cast<int>(it - own.begin());
                }
                for (const std::string& l : linesOf(patched(text, std::move(repl))))
                    lines.push_back(l);
            }
            // the drawing: positions from the selection's top-left to `at`, every one within what a line says
            const Pt to = op.at.value_or(Pt{ 0, 0 });
            for (const PartLine& p : lay.parts)
                if (names.find(folded(p.name)) != names.end() && !withinReach(p.at + to))
                    return p.name + ": " + farOut(p.at + to);
            for (const NetLine& n : lay.nets)
            {
                if (nodes.find(nodeKey(n.node)) == nodes.end())
                    continue;                             // not pasted (ground's, or a node no element names)
                std::vector<Pt> points = n.labels;
                for (const Polyline& poly : n.route)
                    points.insert(points.end(), poly.begin(), poly.end());
                for (const Pt q : points)
                    if (std::abs(q.x + to.x) > kCoordLimit || std::abs(q.y + to.y) > kCoordLimit)
                        return "net " + n.node + ": " + ptText(q + to) + " is past -" + std::to_string(kCoordLimit) + ".." + std::to_string(kCoordLimit);
            }
            const int at = w.nl.lastElementLine >= 0 ? w.nl.lastElementLine + 1 : w.nl.endLine >= 0 ? w.nl.endLine : 1 << 30;
            w.text = withLines(w.text, at, lines);
            for (const PartLine& p : lay.parts)
                if (const auto it = names.find(folded(p.name)); it != names.end())
                    w.layout.parts.push_back({ it->second, p.at + to, p.orient, p.extra, -1 });
            for (const NetLine& n : lay.nets)
                if (const auto it = nodes.find(nodeKey(n.node)); it != nodes.end())
                {
                    NetLine copy{ it->second, n.flag, {}, {}, -1 };
                    for (const Pt l : n.labels)
                        copy.labels.push_back(l + to);
                    for (const Polyline& poly : n.route)
                    {
                        Polyline moved;
                        for (const Pt p : poly)
                            moved.push_back(p + to);
                        copy.route.push_back(moved);
                    }
                    w.layout.nets.push_back(std::move(copy));
                }
            w.redraw();
            std::string said;
            for (const Element& e : src.elements)
            {
                said += (said.empty() ? "" : ", ") + names[folded(e.name)];
                out.touched.push_back(names[folded(e.name)]);
            }
            out.said.push_back("pasted " + said);
            return {};
        }

        std::string opText(const Op& op)
        {
            if (!op.text.empty())
                return op.text;
            switch (op.type)
            {
                case Op::Type::add: return "add " + (op.lines.empty() ? std::string() : op.lines.back());
                case Op::Type::place: return "place " + op.name;
                case Op::Type::connect: return "connect";
                case Op::Type::disconnect: return "disconnect";
                case Op::Type::cut: return "cut " + op.name;
                case Op::Type::set: return "set " + op.name;
                case Op::Type::rename: return "rename " + op.name;
                case Op::Type::remove: return "delete";
                case Op::Type::label: return "label " + op.name;
                case Op::Type::style: return "style " + op.name;
                case Op::Type::paste: return "paste";
                case Op::Type::layout: return "layout";
            }
            return "op";
        }
    }

    Applied apply(std::string_view text, const Drawing& d, std::span<const Op> ops, const SessionPins& pins)
    {
        (void)pins;
        Applied out;
        if (d.readOnly)
        {
            out.error = "the drawing is a newer playground's (*@ schematic " + std::to_string(d.version) + "): read only";
            return out;
        }
        Work w;
        w.text = std::string(text);
        w.nl = read(text);
        w.d = d;
        w.layout = layoutOf(w.nl, d);
        std::vector<std::string> placedNames;           // the parts an add, a place or a paste put down ...
        std::vector<std::size_t> placedBy;               // ... and the op that did
        std::vector<std::pair<std::string, std::size_t>> tagged;   // the nets a style op shows as name tags, and the op
        for (std::size_t i = 0; i < ops.size(); ++i)
        {
            const Op& op = ops[i];
            const std::size_t touchedBefore = out.touched.size();
            std::string why;
            switch (op.type)
            {
                case Op::Type::add: why = doAdd(w, op, out, placedNames); break;
                case Op::Type::place: why = doPlace(w, op, out, placedNames); break;
                case Op::Type::connect: why = doConnect(w, op, out); break;
                case Op::Type::disconnect: why = doDisconnect(w, op, out); break;
                case Op::Type::cut: why = doCut(w, op, out); break;
                case Op::Type::set: why = doSet(w, op, out); break;
                case Op::Type::rename: why = doRename(w, op, out); break;
                case Op::Type::remove: why = doRemove(w, op, out); break;
                case Op::Type::label: why = doLabel(w, op, out, false); break;
                case Op::Type::style: why = doLabel(w, op, out, true); break;
                case Op::Type::paste: why = doPaste(w, op, out); break;
                case Op::Type::layout: why = doLayout(w, op, out); break;
            }
            // a drawing the router wires stays one: its parts never spread past its grid (Budget::wide)
            if (why.empty() && !d.budget.wide && w.d.budget.wide)
            {
                Box b;
                for (const Part& p : w.d.parts)
                    b = b.joined(p.body);
                why = "the parts would span " + std::to_string(b.x1 - b.x0 + 1) + " x " + std::to_string(b.y1 - b.y0 + 1)
                    + " units, past what the router wires (" + std::to_string(kRouteCells) + " grid points)";
            }
            if (!why.empty())
            {
                Applied failed;
                failed.text = std::string(text);
                failed.error = "op " + std::to_string(i + 1) + " (" + opText(op) + "): " + why;
                return failed;
            }
            if (op.type == Op::Type::paste)
                for (std::size_t t = touchedBefore; t < out.touched.size(); ++t)
                    placedNames.push_back(out.touched[t]);
            placedBy.resize(placedNames.size(), i);
            if (op.type == Op::Type::style && op.flag)
                tagged.emplace_back(op.name, i);
        }
        const auto refused = [&](std::size_t k, const std::string& a, const std::string& b) {
            Applied failed;
            failed.text = std::string(text);
            failed.error = "op " + std::to_string(k + 1) + " (" + opText(ops[k]) + "): " + a + " would overlap " + b;
            return failed;
        };
        // a part placed (or pasted) onto another: refused, as a drop is
        for (const auto& [a, b] : w.d.overlaps)
            for (std::size_t k = 0; k < placedNames.size(); ++k)
            {
                const std::string n = folded(placedNames[k]);
                const auto names = [&](const std::string& item) {
                    const std::string f = folded(item);
                    return f == n || f.rfind(n + ".", 0) == 0 || f == n + "'s flag";
                };
                if (names(a) || names(b))
                    return refused(placedBy[k], a, b);
            }
        // a net shown as name tags whose tags meet another net's or a body (and did not before): refused too
        for (const auto& [net, k] : tagged)
        {
            const int n = w.d.net(net);
            if (n < 0)
                continue;                                 // renamed by a later op: its tags are that name's
            std::set<std::string> tags;
            for (const Flag& f : w.d.flags)
                if (f.net == n)
                {
                    const Part& p = w.d.parts[static_cast<std::size_t>(f.pin.part)];
                    tags.insert(p.name + (p.element >= 0 ? "." + p.pins[static_cast<std::size_t>(f.pin.pin)].name : std::string()) + "'s flag");
                }
            for (const auto& o : w.d.overlaps)
                if ((tags.count(o.first) != 0 || tags.count(o.second) != 0) && std::find(d.overlaps.begin(), d.overlaps.end(), o) == d.overlaps.end())
                    return refused(k, o.first, o.second);
        }
        out.text = withBlock(w.text, writeBlock(w.nl, w.d));
        out.edits = diffEdits(text, out.text);
        return out;
    }

    std::vector<Edit> editsBetween(std::string_view from, std::string_view to) { return diffEdits(from, to); }

    // ---- the clipboard and the fields -------------------------------------------------------------------------------
    std::string copied(std::string_view text, const Drawing& d, const std::vector<std::string>& names, Pt* origin)
    {
        const Netlist nl = read(text);
        std::set<int> parts, elements;
        Box box;
        for (const std::string& n : names)
        {
            const int i = d.part(n);
            if (i < 0 || d.parts[static_cast<std::size_t>(i)].element < 0)
                continue;
            const int e = d.parts[static_cast<std::size_t>(i)].element;
            if (e >= static_cast<int>(nl.elements.size()) || !nl.elements[static_cast<std::size_t>(e)].drawn())
                continue;
            parts.insert(i);
            elements.insert(e);
            box = box.joined(d.parts[static_cast<std::size_t>(i)].body);
        }
        if (elements.empty())
            return {};
        const Pt o{ box.x0, box.y0 };
        if (origin != nullptr)
            *origin = o;
        // the directives they need: their pots, switches and models, as the netlist writes them
        std::set<std::string> pots, switches, models;
        for (const int e : elements)
        {
            const Element& el = nl.elements[static_cast<std::size_t>(e)];
            for (const std::string& p : potsOf(nl, el))
                pots.insert(folded(p));
            if (el.kind == Kind::S && el.tokens.size() >= 4)
                switches.insert(folded(el.tokens[3].text));
            if (el.kind == Kind::D && el.valueToken >= 0 && el.valueToken < static_cast<int>(el.tokens.size()))
                models.insert(folded(el.tokens[static_cast<std::size_t>(el.valueToken)].text));
        }
        std::set<int> needed;
        for (const PotLine& p : nl.pots)
            if (pots.count(folded(p.name)) != 0)
                needed.insert(p.line);
        for (const SwitchLine& s : nl.switches)
            if (switches.count(folded(s.name)) != 0)
                needed.insert(s.line);
        for (const auto& [m, line] : nl.models)
            if (models.count(folded(m)) != 0)
                needed.insert(line);
        std::string out;
        const auto put = [&](int line) { out += std::string(lfLine(nl.lines[static_cast<std::size_t>(line)])) + "\n"; };
        for (const int line : needed)
            for (const int ln : directiveLines(nl.lines, line))   // its + lines too
                put(ln);
        for (const int e : elements)
        {
            const Element& el = nl.elements[static_cast<std::size_t>(e)];
            for (const int l : ownLines(nl.lines, el.firstLine, el.lastLine))
                put(l);
        }
        // the drawing, from the selection's top-left
        std::vector<const Part*> ps;
        for (const int i : parts)
            ps.push_back(&d.parts[static_cast<std::size_t>(i)]);
        std::sort(ps.begin(), ps.end(), [](const Part* a, const Part* b) { return naturalLess(a->name, b->name); });
        for (const Part* p : ps)
        {
            out += "*@ part " + p->name + " " + ptText(p->origin - o);
            if (p->orient != Orient::r0)
                out += std::string(" ") + orientName(p->orient);
            for (const std::string& x : p->extra)
                out += " " + x;
            out += "\n";
        }
        std::vector<const Net*> nets;
        for (const Net& n : d.nets)
        {
            if (!signalNet(n) || n.pins.empty() || static_cast<int>(n.pins.size()) != n.totalPins)
                continue;
            if (std::all_of(n.pins.begin(), n.pins.end(), [&](const PinRef& r) { return parts.count(r.part) != 0; }))
                nets.push_back(&n);
        }
        std::sort(nets.begin(), nets.end(), [](const Net* a, const Net* b) { return naturalLess(a->name, b->name); });
        for (const Net* n : nets)
        {
            const bool flag = n->style == Style::flag || n->unroutable;
            if (!flag && (n->pins.size() < 2 || n->edges.empty()))
                continue;
            out += "*@ net " + n->name;
            if (flag)
                out += " flag";
            std::vector<Pt> labels;
            for (const Pt l : n->labels)
                labels.push_back(l - o);
            std::sort(labels.begin(), labels.end());
            for (const Pt l : labels)
                out += " label=" + ptText(l);
            if (!flag)
            {
                std::vector<UnitEdge> edges;
                for (const UnitEdge& e : n->edges)
                    edges.push_back({ e.a - o, e.h });
                sortEdges(edges);
                out += routeText(edges);
            }
            out += "\n";
        }
        return out;
    }

    namespace
    {
        // A SPICE number's parts: its value, the mantissa as written, its suffix as written ("" for none), what follows.
        struct Spice
        {
            double value = 0.0;
            std::string mantissa, suffix, tail;
        };

        std::optional<Spice> spiceParts(std::string_view s)
        {
            if (!spiceNumber(s))
                return std::nullopt;
            std::size_t i = 0;
            if (i < s.size() && (s[i] == '+' || s[i] == '-'))
                ++i;
            while (i < s.size() && (digit(s[i]) || s[i] == '.'))
                ++i;
            if (i + 1 < s.size() && lower(s[i]) == 'e' && (digit(s[i + 1]) || s[i + 1] == '+' || s[i + 1] == '-'))
            {
                i += 2;
                while (i < s.size() && digit(s[i]))
                    ++i;
            }
            Spice out;
            out.mantissa = std::string(s.substr(0, i));
            // the mantissa in the C locale's way, whatever the process's (a SPICE number is never localised)
            double v = 0.0, frac = 0.0, scale = 1.0;
            bool neg = false, point = false;
            int exp = 0;
            std::size_t k = 0;
            if (k < out.mantissa.size() && (out.mantissa[k] == '+' || out.mantissa[k] == '-'))
                neg = out.mantissa[k++] == '-';
            for (; k < out.mantissa.size(); ++k)
            {
                const char c = out.mantissa[k];
                if (c == '.')
                    point = true;
                else if (digit(c))
                {
                    if (point)
                        frac += (scale *= 0.1) * (c - '0');
                    else
                        v = v * 10.0 + (c - '0');
                }
                else
                {
                    const bool eneg = k + 1 < out.mantissa.size() && out.mantissa[k + 1] == '-';
                    for (std::size_t j = k + 1; j < out.mantissa.size(); ++j)
                        if (digit(out.mantissa[j]) && exp < 1000)   // past any double's range: no more digits needed
                            exp = exp * 10 + (out.mantissa[j] - '0');
                    exp = eneg ? -exp : exp;
                    break;
                }
            }
            v += frac;
            for (; exp > 0; --exp)
                v *= 10.0;
            for (; exp < 0; ++exp)
                v *= 0.1;
            const std::string rest = folded(s.substr(i));
            double mul = 1.0;
            std::size_t used = 0;
            if (rest.rfind("meg", 0) == 0)
            {
                mul = 1e6;
                used = 3;
            }
            else if (!rest.empty())
            {
                used = 1;
                switch (rest[0])
                {
                    case 't': mul = 1e12; break;
                    case 'g': mul = 1e9; break;
                    case 'k': mul = 1e3; break;
                    case 'm': mul = 1e-3; break;
                    case 'u': mul = 1e-6; break;
                    case 'n': mul = 1e-9; break;
                    case 'p': mul = 1e-12; break;
                    case 'f': mul = 1e-15; break;
                    default: used = 0; break;
                }
            }
            out.suffix = std::string(s.substr(i, used));
            out.tail = std::string(s.substr(i + used));
            out.value = (neg ? -v : v) * mul;
            if (!std::isfinite(out.value))
                return std::nullopt;                     // 1e400: no step, no warning
            return out;
        }
    }

    std::optional<std::string> e12Stepped(std::string_view value, int dir, bool decade)
    {
        const std::optional<Spice> sp = spiceParts(value);
        if (!sp || dir == 0 || !(sp->value > 0.0) || !std::isfinite(sp->value))
            return std::nullopt;
        // the value as a mantissa index on the series and a decade (exponent of ten), exactly: E12's values in tenths of
        // their decade's first, so 4.7k is index 8 of decade 3
        constexpr int kE12[] = { 10, 12, 15, 18, 22, 27, 33, 39, 47, 56, 68, 82 };
        int dec = 0;
        double m = sp->value;
        while (m >= 10.0 * (1.0 - 1e-9))
        {
            m /= 10.0;
            ++dec;
        }
        while (m < 1.0 * (1.0 - 1e-9))
        {
            m *= 10.0;
            --dec;
        }
        // its place on the series: on a step (within a part in a thousand), else between two
        int at = -1;
        int below = -1, nearest = 0;
        for (int k = 0; k < 12; ++k)
        {
            const double s = kE12[k] / 10.0;
            if (std::fabs(m - s) <= s * 1e-3)
                at = k;
            if (s < m * (1.0 - 1e-3))
                below = k;
            if (std::fabs(m - s) < std::fabs(m - kE12[nearest] / 10.0))
                nearest = k;
        }
        int idx = 0;
        if (decade)
        {
            idx = at >= 0 ? at : nearest;                // a decade keeps its step (an off-series value its nearest)
            dec += dir > 0 ? 1 : -1;
        }
        else if (at >= 0)
            idx = at + (dir > 0 ? 1 : -1);
        else
            idx = dir > 0 ? below + 1 : below;           // off the series: the next value of it either way
        while (idx >= 12)
        {
            idx -= 12;
            ++dec;
        }
        while (idx < 0)
        {
            idx += 12;
            --dec;
        }
        if (dec > 12 || dec < -15)
            return std::nullopt;
        // written with the suffix of its size: f p n u m (none) k meg g t, by thousands
        struct Suffix
        {
            int dec;
            const char* lowerText;
            const char* upperText;
        };
        constexpr Suffix kSuffixes[] = { { -15, "f", "F" }, { -12, "p", "P" }, { -9, "n", "N" }, { -6, "u", "U" }, { -3, "m", "M" },
                                         { 0, "", "" },     { 3, "k", "K" },   { 6, "meg", "MEG" }, { 9, "g", "G" }, { 12, "t", "T" } };
        const int thousands = dec >= 0 ? dec / 3 * 3 : -((-dec + 2) / 3 * 3);
        const int shift = dec - thousands;               // 0, 1 or 2 digits before the point
        // the digits: E12's two significant ones, shifted
        const int sig = kE12[idx];                       // 10..82: d.d
        std::string digits;
        if (shift == 0)
            digits = sig % 10 == 0 ? std::to_string(sig / 10) : std::to_string(sig / 10) + "." + std::to_string(sig % 10);
        else if (shift == 1)
            digits = std::to_string(sig);
        else
            digits = std::to_string(sig * 10);
        const bool upper = std::any_of(sp->suffix.begin(), sp->suffix.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
        std::string suffix;
        for (const Suffix& s : kSuffixes)
            if (s.dec == thousands)
                suffix = upper ? s.upperText : s.lowerText;
        return digits + suffix + sp->tail;
    }

    std::string taperOf(const Netlist& nl, const Element& e)
    {
        for (const Token& t : e.tokens)
            if (folded(t.text).rfind("taper=", 0) == 0)
                return t.text.substr(6);
        if (const std::string pot = potAlone(nl, e); !pot.empty())
            for (const PotLine& p : nl.pots)
                if (folded(p.name) == folded(pot) && !p.taper.empty())
                    return p.taper;
        return "lin";
    }

    std::string milliWarning(std::string_view value, std::string* meg)
    {
        const std::optional<Spice> sp = spiceParts(value);
        if (!sp || sp->suffix.size() != 1 || lower(sp->suffix[0]) != 'm' || !(sp->value < 1.0))
            return {};
        if (meg != nullptr)
            *meg = sp->mantissa + "meg" + sp->tail;
        return "'" + std::string(value) + "' is " + sp->mantissa + " milliohm in SPICE; write " + sp->mantissa + "meg for a megohm";
    }

    // ---- edit_circuit's grammar ---------------------------------------------------------------------------------------
    namespace
    {
        std::vector<std::pair<std::size_t, std::string>> wordsOf(std::string_view s)
        {
            std::vector<std::pair<std::size_t, std::string>> out;
            std::size_t i = 0;
            while (i < s.size())
            {
                while (i < s.size() && blank(s[i]))
                    ++i;
                const std::size_t a = i;
                while (i < s.size() && !blank(s[i]))
                    ++i;
                if (i > a)
                    out.emplace_back(a, std::string(s.substr(a, i - a)));
            }
            return out;
        }

        End endOf(const std::string& w)
        {
            End e;
            const std::size_t dot = w.rfind('.');
            if (dot != std::string::npos && dot > 0 && dot + 1 < w.size())
            {
                e.part = w.substr(0, dot);
                e.pin = w.substr(dot + 1);
            }
            else
                e.node = w;
            return e;
        }
    }

    std::optional<Op> parseOp(std::string_view line, std::string& why)
    {
        why.clear();
        std::string_view s = withoutCr(line);
        const auto words = wordsOf(s);
        if (words.empty())
        {
            why = "an empty command";
            return std::nullopt;
        }
        Op op;
        op.text = std::string(s.substr(words.front().first));
        while (!op.text.empty() && blank(op.text.back()))
            op.text.pop_back();
        const std::string cmd = folded(words.front().second);
        const auto rest = [&](std::size_t k) {
            std::string r = k < words.size() ? std::string(s.substr(words[k].first)) : std::string();
            while (!r.empty() && blank(r.back()))
                r.pop_back();
            return r;
        };
        const auto need = [&](std::size_t lo, std::size_t hi, const char* usage) {
            if (words.size() < lo || words.size() > hi)
            {
                why = std::string("expected '") + usage + "'";
                return false;
            }
            return true;
        };
        if (cmd == "add")
        {
            if (words.size() < 2)
            {
                why = "expected 'add <netlist line> [at X,Y [orient]]'";
                return std::nullopt;
            }
            op.type = Op::Type::add;
            std::size_t end = words.size();
            if (end >= 4 && folded(words[end - 3].second) == "at" && ptOf(words[end - 2].second) && orientOf(words[end - 1].second))
            {
                op.at = ptOf(words[end - 2].second);
                op.orient = orientOf(words[end - 1].second);
                end -= 3;
            }
            else if (end >= 3 && folded(words[end - 2].second) == "at" && ptOf(words[end - 1].second))
            {
                op.at = ptOf(words[end - 1].second);
                end -= 2;
            }
            if (end < 2)
            {
                why = "expected 'add <netlist line> [at X,Y [orient]]'";
                return std::nullopt;
            }
            std::string l(s.substr(words[1].first, end < words.size() ? words[end].first - words[1].first : std::string_view::npos));
            while (!l.empty() && blank(l.back()))
                l.pop_back();
            op.lines.push_back(std::move(l));
            return op;
        }
        if (cmd == "place")
        {
            if (!need(3, 4, "place <Part|Terminal> X,Y [orient]"))
                return std::nullopt;
            op.type = Op::Type::place;
            op.name = words[1].second;
            op.at = ptOf(words[2].second);
            if (!op.at)
            {
                why = "'" + words[2].second + "' is not a position (X,Y)";
                return std::nullopt;
            }
            if (words.size() == 4)
            {
                op.orient = orientOf(words[3].second);
                if (!op.orient)
                {
                    why = "'" + words[3].second + "' is not an orientation (r0 r90 r180 r270 m0 m90 m180 m270)";
                    return std::nullopt;
                }
            }
            return op;
        }
        if (cmd == "connect")
        {
            if (!need(3, 3, "connect <end> <end>"))
                return std::nullopt;
            op.type = Op::Type::connect;
            op.a = endOf(words[1].second);
            op.b = endOf(words[2].second);
            return op;
        }
        if (cmd == "disconnect")
        {
            if (!need(2, 2, "disconnect <Part.pin>"))
                return std::nullopt;
            op.type = Op::Type::disconnect;
            op.a = endOf(words[1].second);
            if (op.a.part.empty())
            {
                why = "expected 'disconnect <Part.pin>'";
                return std::nullopt;
            }
            return op;
        }
        if (cmd == "set")
        {
            if (words.size() < 3)
            {
                why = "expected 'set <Part> <value | {expr}>' or 'set <Part> <key>=<value>'";
                return std::nullopt;
            }
            op.type = Op::Type::set;
            op.name = words[1].second;
            const std::string v = rest(2);
            const std::size_t eq = v.find('=');
            if (eq != std::string::npos && v.front() != '{' && eq > 0)
            {
                op.key = folded(v.substr(0, eq));
                op.value = v.substr(eq + 1);
                while (!op.value.empty() && blank(op.value.front()))
                    op.value.erase(op.value.begin());
                while (!op.key.empty() && blank(op.key.back()))
                    op.key.pop_back();
            }
            else
                op.value = v;
            return op;
        }
        if (cmd == "rename")
        {
            if (!need(3, 3, "rename <Part|node> <new name>"))
                return std::nullopt;
            op.type = Op::Type::rename;
            op.name = words[1].second;
            op.to = words[2].second;
            return op;
        }
        if (cmd == "delete")
        {
            if (words.size() < 2)
            {
                why = "expected 'delete <Part> {<Part>}'";
                return std::nullopt;
            }
            op.type = Op::Type::remove;
            for (std::size_t k = 1; k < words.size(); ++k)
                op.names.push_back(words[k].second);
            return op;
        }
        if (cmd == "layout")
        {
            op.type = Op::Type::layout;
            for (std::size_t k = 1; k < words.size(); ++k)
                op.names.push_back(words[k].second);
            return op;
        }
        why = "unknown command '" + words.front().second + "': add, place, connect, disconnect, set, rename, delete or layout";
        return std::nullopt;
    }
}
