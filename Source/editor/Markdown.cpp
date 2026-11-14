// Source/editor/Markdown.cpp: see Markdown.h. Moved out of ChatView.cpp as it was (the links aside).
#include "editor/Markdown.h"

#include "editor/ChatItems.h"
#include "editor/Items.h"

#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>
#include <funkgui/widgets/TextEditor.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>

namespace playground::ui::markdown
{
    const funkgui::TextStyle& styleOf(Role r)
    {
        using R = Role;
        switch (r)
        {
            case R::strong: return chat::kStrong;
            case R::h1: return chat::kH1;
            case R::h2: return chat::kH2;
            case R::h3: return chat::kH3;
            case R::small:
            case R::faint:
            case R::quote:
            case R::smallCode: return chat::kSmall;
            case R::smallStrong:
            case R::accent: return chat::kSmallStrong;
            case R::body:
            case R::code:
            case R::dim:
            case R::danger: break;
        }
        return chat::kBody;
    }

    funkgui::Col colourOf(Role r, const chat::Palette& p)
    {
        using R = Role;
        switch (r)
        {
            case R::dim:
            case R::small:
            case R::quote: return p.dim;
            case R::faint: return p.faint;
            case R::danger: return p.danger;
            case R::accent: return p.accent;
            case R::body:
            case R::strong:
            case R::code:
            case R::smallCode:
            case R::h1:
            case R::h2:
            case R::h3:
            case R::smallStrong: break;
        }
        return p.text;
    }

    std::vector<std::string> splitLines(const std::string& s)
    {
        std::vector<std::string> out;
        std::size_t start = 0;
        while (true)
        {
            const std::size_t end = s.find('\n', start);
            out.push_back(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        return out;
    }

    std::string trimLeft(const std::string& s)
    {
        std::size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
            ++i;
        return s.substr(i);
    }

    namespace
    {
        int leadingSpaces(const std::string& s)
        {
            int n = 0;
            for (const char c : s)
            {
                if (c == ' ')
                    ++n;
                else if (c == '\t')
                    n += 4;
                else
                    break;
            }
            return n;
        }

        // "- item", "* item", "+ item": the item's text; else nullopt.
        std::optional<std::string> bulletItem(const std::string& trimmed)
        {
            if (trimmed.size() >= 2 && (trimmed[0] == '-' || trimmed[0] == '*' || trimmed[0] == '+') && trimmed[1] == ' ')
                return trimmed.substr(2);
            return std::nullopt;
        }

        // "12. item" or "12) item": the number and the text; else nullopt.
        std::optional<std::pair<std::string, std::string>> numberedItem(const std::string& trimmed)
        {
            std::size_t i = 0;
            while (i < trimmed.size() && i < 3 && std::isdigit(static_cast<unsigned char>(trimmed[i])) != 0)
                ++i;
            if (i == 0 || i + 1 >= trimmed.size() || (trimmed[i] != '.' && trimmed[i] != ')') || trimmed[i + 1] != ' ')
                return std::nullopt;
            return std::make_pair(trimmed.substr(0, i + 1), trimmed.substr(i + 2));
        }

        int headingLevel(const std::string& trimmed)
        {
            int n = 0;
            while (n < static_cast<int>(trimmed.size()) && trimmed[static_cast<std::size_t>(n)] == '#')
                ++n;
            return n >= 1 && n <= 6 && static_cast<int>(trimmed.size()) > n && trimmed[static_cast<std::size_t>(n)] == ' ' ? n : 0;
        }

        bool isRule(const std::string& trimmed)
        {
            if (trimmed.size() < 3)
                return false;
            const char c = trimmed[0];
            if (c != '-' && c != '*' && c != '_')
                return false;
            return std::all_of(trimmed.begin(), trimmed.end(), [c](char x) { return x == c || x == ' '; });
        }

        // A piece of a paragraph in one role.
        struct Piece
        {
            std::string text;
            Role role;
            std::string url;                         // a link's (Options::links)
        };

        Role strongOf(Role base)
        {
            using R = Role;
            switch (base)
            {
                case R::small:
                case R::faint:
                case R::quote: return R::smallStrong;
                case R::h1:
                case R::h2:
                case R::h3:
                case R::accent: return base;
                case R::body:
                case R::strong:
                case R::code:
                case R::smallCode:
                case R::dim:
                case R::smallStrong:
                case R::danger: break;
            }
            return R::strong;
        }

        // Inline code in a paragraph of `base`: set at the base's size (a small one's at the small size), so that it
        // fills the cells it is laid out in.
        Role codeOf(Role base)
        {
            using R = Role;
            switch (base)
            {
                case R::small:
                case R::smallStrong:
                case R::faint:
                case R::quote:
                case R::accent:
                case R::smallCode: return R::smallCode;
                case R::body:
                case R::strong:
                case R::code:
                case R::h1:
                case R::h2:
                case R::h3:
                case R::dim:
                case R::danger: break;
            }
            return R::code;
        }

        bool isCode(Role r) { return r == Role::code || r == Role::smallCode; }

        // Inline markdown: `code` and **strong** (other marks stay as typed); with links, [text](url) too.
        std::vector<Piece> inlinePieces(const std::string& s, Role base, bool links)
        {
            std::vector<Piece> out;
            std::string cur;
            bool strong = false;
            const auto flush = [&] {
                if (!cur.empty())
                    out.push_back({ cur, strong ? strongOf(base) : base, {} });
                cur.clear();
            };
            for (std::size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] == '`')
                {
                    const std::size_t j = s.find('`', i + 1);
                    if (j != std::string::npos && j > i + 1)
                    {
                        flush();
                        out.push_back({ s.substr(i + 1, j - i - 1), codeOf(base), {} });
                        i = j;
                        continue;
                    }
                }
                if (links && s[i] == '[')
                {
                    // [text](url): a text with no ] and an address with no space or ) (the changelog's own links).
                    const std::size_t j = s.find(']', i + 1);
                    const std::size_t k = j != std::string::npos && j + 1 < s.size() && s[j + 1] == '(' ? s.find(')', j + 2)
                                                                                                       : std::string::npos;
                    if (k != std::string::npos && j > i + 1 && k > j + 2 && s.find(' ', j + 2) > k)
                    {
                        flush();
                        out.push_back({ s.substr(i + 1, j - i - 1), strong ? strongOf(base) : base, s.substr(j + 2, k - j - 2) });
                        i = k;
                        continue;
                    }
                }
                if (s[i] == '*' && i + 1 < s.size() && s[i + 1] == '*')
                {
                    flush();
                    strong = !strong;
                    ++i;
                    continue;
                }
                cur += s[i];
            }
            flush();
            return out;
        }
    }

    float Metrics::adv(Role r) const
    {
        const auto measure = [](const funkgui::TextStyle& st) {
            return funkgui::text::width(funkgui::FontService::get().atlas(), "0000000000", st) / 10.0f;
        };
        switch (r)
        {
            case Role::small:
            case Role::smallStrong:
            case Role::faint:
            case Role::quote:
            case Role::accent:
            case Role::smallCode:
                if (small_ <= 0.0f)
                    small_ = measure(chat::kSmall);
                return small_;
            case Role::h1:
                if (h1_ <= 0.0f)
                    h1_ = measure(chat::kH1);
                return h1_;
            case Role::h2:
                if (h2_ <= 0.0f)
                    h2_ = measure(chat::kH2);
                return h2_;
            case Role::body:
            case Role::strong:
            case Role::code:
            case Role::h3:
            case Role::dim:
            case Role::danger: break;
        }
        if (body_ <= 0.0f)
            body_ = measure(chat::kBody);
        return body_;
    }

    float Metrics::codeAdv() const
    {
        if (code_ <= 0.0f)
            code_ = funkgui::text::width(funkgui::FontService::get().atlas(), "0000000000", chat::kCodeText) / 10.0f;
        return code_;
    }

    // ---- layout ---------------------------------------------------------------------------------------------------
    float flow(Layout& out, const Metrics& m, const std::string& paragraph, Role base, float x0, float w, float y,
               float lineH, bool markdown, const Options& options)
    {
        const std::vector<Piece> pieces = markdown ? inlinePieces(paragraph, base, options.links)
                                                   : std::vector<Piece>{ { paragraph, base, {} } };
        // Words: runs of pieces between spaces.
        struct Word
        {
            std::vector<Piece> parts;
            std::size_t chars = 0;
        };
        std::vector<Word> words;
        Word cur;
        for (const Piece& p : pieces)
        {
            std::string run;
            for (const char ch : p.text)
            {
                if (ch == ' ')
                {
                    if (!run.empty())
                    {
                        cur.parts.push_back({ run, p.role, p.url });
                        cur.chars += run.size();
                        run.clear();
                    }
                    if (!cur.parts.empty())
                    {
                        words.push_back(cur);
                        cur = Word{};
                    }
                }
                else
                    run += ch;
            }
            if (!run.empty())
            {
                cur.parts.push_back({ run, p.role, p.url });
                cur.chars += run.size();
            }
        }
        if (!cur.parts.empty())
            words.push_back(cur);
        if (words.empty())
            return y;

        const float a = m.adv(base);
        const auto cols = static_cast<std::size_t>(std::max(1.0f, std::floor(w / a)));
        std::size_t col = 0;                         // characters on the current line
        bool lineHasText = false;
        const auto place = [&](const std::string& text, Role role, bool spaceBefore, const std::string& url) {
            const float x = x0 + static_cast<float>(col) * a;
            const bool link = !url.empty();
            const bool merge = spaceBefore && !out.spans.empty() && out.spans.back().y == y && out.spans.back().role == role
                            && out.spans.back().link == link
                            && (!link || (!out.links.empty() && out.links.back().url == url && out.links.back().r.y == y));
            if (merge)
                out.spans.back().text += " " + text;   // one span for a run of one role on a line
            else
                out.spans.push_back({ spaceBefore ? x + a : x, y, lineH, text, role, link });
            if (link)
            {
                // Its place on this line, for the owner's hit: one rect a link a line.
                const float from = spaceBefore ? x + a : x, to = from + static_cast<float>(text.size()) * a;
                if (merge)
                    out.links.back().r.w = to - out.links.back().r.x;
                else
                    out.links.push_back({ { from, y, to - from, lineH }, url });
            }
            if (isCode(role))
            {
                // Inline code's ground: one rounded box over the run, spaces included.
                if (merge && !out.shapes.empty() && out.shapes.back().kind == ShapeKind::inlineCode)
                    out.shapes.back().r.w += static_cast<float>(text.size() + 1) * a;
                else
                {
                    const float cx = (spaceBefore ? x + a : x) - 2.0f;
                    out.shapes.push_back({ { cx, y + 2.0f, static_cast<float>(text.size()) * a + 4.0f, lineH - 4.0f },
                                         ShapeKind::inlineCode });
                }
            }
            col += text.size() + (spaceBefore ? 1 : 0);
            lineHasText = true;
        };
        for (const Word& word : words)
        {
            const bool needSpace = lineHasText;
            if (lineHasText && col + 1 + word.chars > cols)
            {
                y += lineH;
                col = 0;
                lineHasText = false;
            }
            bool first = true;
            for (const Piece& part : word.parts)
            {
                std::string rest = part.text;
                while (!rest.empty())
                {
                    const bool space = first && needSpace && lineHasText;
                    const std::size_t used = col + (space ? 1 : 0);
                    const std::size_t room = cols > used ? cols - used : 0;
                    if (room == 0)
                    {
                        y += lineH;
                        col = 0;
                        lineHasText = false;
                        continue;
                    }
                    const std::string chunk = rest.substr(0, room);
                    rest.erase(0, chunk.size());
                    // A space goes before the word's first part when the line has text already.
                    place(chunk, part.role, space, part.url);
                    first = false;
                    if (!rest.empty())
                    {
                        y += lineH;
                        col = 0;
                        lineHasText = false;
                    }
                }
            }
        }
        return y + lineH;
    }

    void layoutCode(Layout& out, const Metrics& m, const std::string& lang, const std::vector<std::string>& lines,
                    const std::string& copied, float w, float& y)
    {
        const float top = y;
        const float a = m.codeAdv();
        out.spans.push_back({ kCodePad, top + 3.0f, 18.0f, lang.empty() ? std::string("code") : lang, Role::faint });
        out.copies.push_back({ { w - 64.0f, top + 3.0f, 58.0f, 18.0f }, copied });
        y = top + kCodeHeader;
        const funkgui::text::KeywordHighlighter hl(fminusRules());
        uint32_t state = 0;
        const auto cols = static_cast<std::size_t>(std::max(8.0f, std::floor((w - 2.0f * kCodePad) / a)));
        for (const std::string& line : lines)
        {
            std::vector<funkgui::text::Span> spans;
            state = hl.line(line, state, spans);
            std::size_t from = 0;
            do
            {
                const std::size_t len = std::min(cols, line.size() - from);
                CodeLine cl{ kCodePad, y, line.substr(from, len), {} };
                for (const funkgui::text::Span& s : spans)
                {
                    const int s0 = std::max(s.start, static_cast<int>(from));
                    const int s1 = std::min(s.end, static_cast<int>(from + len));
                    if (s1 > s0)
                        cl.spans.push_back({ s0 - static_cast<int>(from), s1 - static_cast<int>(from), s.style });
                }
                out.code.push_back(std::move(cl));
                y += chat::kCodeLine;
                from += len;
            } while (from < line.size());
        }
        y += kCodePad - 4.0f;
        out.shapes.push_back({ { 0.0f, top, w, y - top }, ShapeKind::codeCard });
    }

    void layout(Layout& out, const Metrics& m, const std::string& text, const std::string& raw, float w, float& y,
                const Options& options)
    {
        const std::vector<std::string> lines = splitLines(text);
        // The entry's own lines, line for line (asciiText keeps every line break and makes none): a code block's Copy
        // copies them, not what is drawn.
        const std::vector<std::string> rawLines = splitLines(raw);
        std::string para;
        bool first = true;
        const auto space = [&](float gap) {
            if (!first)
                y += gap;
            first = false;
        };
        const auto flushPara = [&] {
            if (para.empty())
                return;
            space(8.0f);
            y = flow(out, m, para, Role::body, 0.0f, w, y, chat::kBodyLine, true, options);
            para.clear();
        };
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            const std::string& line = lines[i];
            const std::string t = trimLeft(line);
            if (t.rfind("```", 0) == 0)
            {
                flushPara();
                std::string lang = t.substr(3);
                while (!lang.empty() && lang.back() == ' ')
                    lang.pop_back();
                std::vector<std::string> code;
                std::string copied;
                std::size_t j = i + 1;
                for (; j < lines.size() && trimLeft(lines[j]).rfind("```", 0) != 0; ++j)
                {
                    code.push_back(lines[j]);
                    copied += (j < rawLines.size() ? rawLines[j] : lines[j]) + "\n";
                }
                space(10.0f);
                layoutCode(out, m, lang, code, copied, w, y);
                i = j;                               // past the closing fence (or the end)
                continue;
            }
            if (t.empty())
            {
                flushPara();
                continue;
            }
            if (const int level = headingLevel(t); level > 0)
            {
                flushPara();
                space(12.0f);
                const Role r = level == 1 ? Role::h1 : (level == 2 ? Role::h2 : Role::h3);
                y = flow(out, m, t.substr(static_cast<std::size_t>(level) + 1), r, 0.0f, w, y, level <= 2 ? 22.0f : 20.0f, true,
                         options);
                continue;
            }
            if (isRule(t))
            {
                flushPara();
                space(8.0f);
                out.shapes.push_back({ { 0.0f, y + 4.0f, w, 1.0f }, ShapeKind::rule });
                y += 9.0f;
                continue;
            }
            const auto bullet = bulletItem(t);
            const auto numbered = bullet ? std::nullopt : numberedItem(t);
            if (bullet || numbered)
            {
                flushPara();
                const float indent = 14.0f * static_cast<float>(std::min(3, leadingSpaces(line) / 2));
                // The item's lines: those after it that are indented and are no item of their own.
                std::string item = bullet ? *bullet : numbered->second;
                while (i + 1 < lines.size())
                {
                    const std::string& nx = lines[i + 1];
                    const std::string nt = trimLeft(nx);
                    if (nt.empty() || leadingSpaces(nx) < 2 || bulletItem(nt) || numberedItem(nt) || nt.rfind("```", 0) == 0)
                        break;
                    item += " " + nt;
                    ++i;
                }
                space(first ? 0.0f : 4.0f);
                if (bullet)
                    out.shapes.push_back({ { indent + 4.0f, y + chat::kBodyLine * 0.5f - 2.0f, 4.0f, 4.0f }, ShapeKind::bullet });
                else
                    out.spans.push_back({ indent, y, chat::kBodyLine, numbered->first, Role::dim });
                const float textX = indent + (bullet ? 16.0f : m.adv(Role::body) * static_cast<float>(numbered->first.size() + 1));
                y = flow(out, m, item, Role::body, textX, w - textX, y, chat::kBodyLine, true, options);
                continue;
            }
            if (t.rfind("> ", 0) == 0 || t == ">")
            {
                flushPara();
                std::string quote = t.size() > 2 ? t.substr(2) : std::string();
                while (i + 1 < lines.size())
                {
                    const std::string nt = trimLeft(lines[i + 1]);
                    if (nt.rfind(">", 0) != 0)
                        break;
                    quote += " " + (nt.size() > 2 ? nt.substr(2) : std::string());
                    ++i;
                }
                space(8.0f);
                const float top = y;
                y = flow(out, m, quote, Role::quote, 12.0f, w - 12.0f, y, chat::kSmallLine + 2.0f, true, options);
                out.shapes.push_back({ { 0.0f, top + 2.0f, 2.0f, y - top - 4.0f }, ShapeKind::quoteBar });
                continue;
            }
            para += (para.empty() ? "" : " ") + t;
        }
        flushPara();
    }

    // ---- drawing ---------------------------------------------------------------------------------------------------

    void drawText(funkgui::Canvas& c, const std::string& s, float x, float lineTop, float lineH, Role r, funkgui::Col col)
    {
        const funkgui::TextStyle& st = styleOf(r);
        c.text(s.c_str(), x, c.capCentreTop(lineTop + lineH * 0.5f, st), st, col);
    }

    void drawShape(funkgui::Canvas& c, const chat::Palette& p, ShapeKind kind, const funkgui::Rect& r)
    {
        switch (kind)
        {
            case ShapeKind::inlineCode:
                c.rrect(r.x, r.y, r.w, r.h, 4.0f, p.code, 1.0f, p.codeEdge);
                break;
            case ShapeKind::codeCard:
                c.rrect(r.x, r.y, r.w, r.h, 9.0f, p.code, 1.0f, p.codeEdge);
                c.hairlineH(r.x + 1.0f, r.y + kCodeHeader - 2.0f, r.w - 2.0f, p.codeEdge);
                break;
            case ShapeKind::bullet:
                c.disc(r.centreX(), r.centreY(), 2.0f, p.dim);
                break;
            case ShapeKind::quoteBar:
                c.rrect(r.x, r.y, r.w, std::max(0.0f, r.h), 1.0f, p.cardEdge);
                break;
            case ShapeKind::rule:
                c.hairlineH(r.x, r.y, r.w, p.cardEdge);
                break;
        }
    }

    void drawCode(funkgui::Canvas& c, const funkgui::Theme& th, const chat::Palette& p,
                  const std::vector<funkgui::text::HighlightStyle>& codePalette, const CodeLine& cl, float x0, float top,
                  float codeAdv)
    {
        const float ly = top + cl.y;
        const float ca = codeAdv;
        const float tt = c.capCentreTop(ly + chat::kCodeLine * 0.5f, chat::kCodeText);
        int at = 0;
        const auto run = [&](int from, int to, funkgui::Col col, float weight) {
            if (to <= from)
                return;
            funkgui::TextStyle st = chat::kCodeText;
            st.weight = weight;
            const std::string piece = cl.text.substr(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from));
            c.text(piece.c_str(), x0 + cl.x + static_cast<float>(from) * ca, tt, st, col);
        };
        for (const funkgui::text::Span& sp : cl.spans)
        {
            run(at, sp.start, p.text, 0.0f);
            const funkgui::text::HighlightStyle& hs = codePalette[std::min<std::size_t>(sp.style, codePalette.size() - 1)];
            run(sp.start, sp.end, hs.resolve(th), hs.weight);
            at = sp.end;
        }
        run(at, static_cast<int>(cl.text.size()), p.text, 0.0f);
    }

    void draw(funkgui::Canvas& c, const funkgui::Theme& th, const Layout& l, const Metrics& m, float x0, float top,
              const funkgui::Rect& clip, int hover)
    {
        const chat::Palette p = chat::palette(th);
        const auto visible = [&](float y, float h) { return y + h >= clip.y && y <= clip.bottom(); };
        for (const Shape& s : l.shapes)
            if (visible(top + s.r.y, s.r.h))
                drawShape(c, p, s.kind, { x0 + s.r.x, top + s.r.y, s.r.w, s.r.h });
        for (const Span& s : l.spans)
            if (visible(top + s.y, s.lineH))
                drawText(c, s.text, x0 + s.x, top + s.y, s.lineH, s.role, s.link ? p.accent : colourOf(s.role, p));
        if (hover >= 0 && hover < static_cast<int>(l.links.size()))
        {
            // The hovered link underlined, every line of it.
            const std::string& url = l.links[static_cast<std::size_t>(hover)].url;
            for (const Link& k : l.links)
                if (k.url == url && visible(top + k.r.y, k.r.h))
                    c.hairlineH(x0 + k.r.x, top + k.r.y + k.r.h - 3.0f, k.r.w, p.accent);
        }
        if (!l.code.empty())
        {
            const std::vector<funkgui::text::HighlightStyle> codePalette = funkgui::TextEditor::defaultPalette();
            const float ca = m.codeAdv();
            for (const CodeLine& cl : l.code)
                if (visible(top + cl.y, chat::kCodeLine))
                    drawCode(c, th, p, codePalette, cl, x0, top, ca);
        }
    }
}
