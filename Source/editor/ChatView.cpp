// Source/editor/ChatView.cpp: see ChatView.h.
#include "editor/ChatView.h"

#include "editor/ChatItems.h"

#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>
#include <funkgui/widgets/TextEditor.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace testingplaygroundsynth::ui
{
    namespace
    {
        using funkgui::Point;
        using funkgui::Rect;

        constexpr float kPadX = 12.0f;               // the content's inset from the view's sides
        constexpr float kPadTop = 10.0f;
        constexpr float kPadBottom = 14.0f;
        constexpr float kBarW = 3.0f;                // the scroll bar
        constexpr float kGap = 12.0f;                // between blocks
        constexpr float kGapUser = 18.0f;            // around the user's bubbles
        constexpr float kBubblePadX = 12.0f;
        constexpr float kBubblePadY = 7.0f;
        constexpr float kBubbleShare = 0.86f;        // of the width, at most
        constexpr float kCodePad = 10.0f;
        constexpr float kCodeHeader = 24.0f;
        constexpr float kToolRow = 24.0f;
        constexpr float kThinkRow = 22.0f;
        constexpr float kCopyRow = 20.0f;
        constexpr float kWorkingRow = 26.0f;
        constexpr float kJump = 28.0f;
        constexpr float kWheelSmooth = 512.0f;       // px a unit of a trackpad's wheel (TextEditor's)
        constexpr float kWheelNotched = 292.0f;      // px a unit of a notched wheel: three lines a notch
        constexpr float kTileH = 112.0f;             // a sent picture's thumbnail, above its bubble
        constexpr float kTileMinW = 64.0f, kTileMaxW = 220.0f;
        constexpr float kTileGap = 6.0f;
        constexpr float kCardPad = 12.0f;            // a question card's inset
        constexpr float kOptionPadY = 7.0f;          // an option's, above and below its text
        constexpr float kOptionGap = 6.0f;
        constexpr float kOptionText = 34.0f;         // its text's inset (the ring or the box before it)
        constexpr float kToolPictureMaxW = 420.0f;   // a tool's picture (view_sound's) under its row
        constexpr float kCheckRow = 20.0f;           // a check's label line; its detail under it
        constexpr float kSubmitW = 132.0f, kSubmitH = 30.0f;
        constexpr uint32_t kA11yStride = 64;         // accessibility ids a block (its item, then its actions)

        const funkgui::TextStyle& styleOf(ChatView::Role r)
        {
            using R = ChatView::Role;
            switch (r)
            {
                case R::strong: return chat::kStrong;
                case R::h1: return chat::kH1;
                case R::h2: return chat::kH2;
                case R::h3: return chat::kH3;
                case R::small:
                case R::faint:
                case R::quote: return chat::kSmall;
                case R::smallStrong:
                case R::accent: return chat::kSmallStrong;
                case R::body:
                case R::code:
                case R::dim:
                case R::danger: break;
            }
            return chat::kBody;
        }

        funkgui::Col colourOf(ChatView::Role r, const chat::Palette& p)
        {
            using R = ChatView::Role;
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
            ChatView::Role role;
        };

        ChatView::Role strongOf(ChatView::Role base)
        {
            using R = ChatView::Role;
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
                case R::dim:
                case R::smallStrong:
                case R::danger: break;
            }
            return R::strong;
        }

        // Inline markdown: `code` and **strong** (other marks stay as typed).
        std::vector<Piece> inlinePieces(const std::string& s, ChatView::Role base)
        {
            std::vector<Piece> out;
            std::string cur;
            bool strong = false;
            const auto flush = [&] {
                if (!cur.empty())
                    out.push_back({ cur, strong ? strongOf(base) : base });
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
                        out.push_back({ s.substr(i + 1, j - i - 1), ChatView::Role::code });
                        i = j;
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

        // The tool entries' icons: what Claude did, by its first word (in any case).
        chat::Icon toolIcon(std::string t)
        {
            for (char& ch : t)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (t.rfind("ran", 0) == 0)
                return chat::Icon::play;
            if (t.rfind("set", 0) == 0)
                return chat::Icon::dial;
            if (t.rfind("measured", 0) == 0)
                return chat::Icon::wave;
            return chat::Icon::file;
        }

        std::string capitalised(std::string s)
        {
            if (!s.empty())
                s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
            return s;
        }

        bool failed(const std::string& t)
        {
            return t.find("error") != std::string::npos || t.find("did not compile") != std::string::npos;
        }
    }

    ChatView::ChatView(uint32_t a11yBase, Hooks hooks) : base_(a11yBase), hooks_(std::move(hooks)) {}

    float ChatView::adv(Role r) const
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
                if (advSmall_ <= 0.0f)
                    advSmall_ = measure(chat::kSmall);
                return advSmall_;
            case Role::h1:
                if (advH1_ <= 0.0f)
                    advH1_ = measure(chat::kH1);
                return advH1_;
            case Role::h2:
                if (advH2_ <= 0.0f)
                    advH2_ = measure(chat::kH2);
                return advH2_;
            case Role::body:
            case Role::strong:
            case Role::code:
            case Role::h3:
            case Role::dim:
            case Role::danger: break;
        }
        if (advBody_ <= 0.0f)
            advBody_ = measure(chat::kBody);
        return advBody_;
    }

    float ChatView::contentWidth() const noexcept { return std::max(80.0f, bounds_.w - 2.0f * kPadX - kBarW); }
    float ChatView::viewHeight() const noexcept { return bounds_.h; }
    float ChatView::maxScroll() const noexcept { return std::max(0.0f, contentH_ - viewHeight()); }
    bool ChatView::atEnd() const noexcept { return scroll_ >= maxScroll() - 1.0f; }

    void ChatView::scrollTo(float y)
    {
        scroll_ = std::clamp(y, 0.0f, maxScroll());
        follow_ = atEnd();
    }

    void ChatView::setBounds(Rect r)
    {
        const bool widthChanged = r.w != bounds_.w;
        const bool wasAtEnd = atEnd();
        bounds_ = r;
        if (widthChanged)
            for (Block& b : blocks_)
                b.key.clear();                       // every block lays out again at the next show()
        if (wasAtEnd || follow_)
            scroll_ = maxScroll();
        scroll_ = std::clamp(scroll_, 0.0f, maxScroll());
    }

    // ---- layout ---------------------------------------------------------------------------------------------------
    // The text of `paragraph` from x0, `w` wide, from y: wrapped at spaces (a word longer than the line broken), the
    // pieces' roles kept; returns the y under its last line.
    float ChatView::flow(Block& b, const std::string& paragraph, Role base, float x0, float w, float y, float lineH) const
    {
        const std::vector<Piece> pieces = inlinePieces(paragraph, base);
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
                        cur.parts.push_back({ run, p.role });
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
                cur.parts.push_back({ run, p.role });
                cur.chars += run.size();
            }
        }
        if (!cur.parts.empty())
            words.push_back(cur);
        if (words.empty())
            return y;

        const float a = adv(base);
        const auto cols = static_cast<std::size_t>(std::max(1.0f, std::floor(w / a)));
        std::size_t col = 0;                         // characters on the current line
        bool lineHasText = false;
        const auto place = [&](const std::string& text, Role role, bool spaceBefore) {
            const float x = x0 + static_cast<float>(col) * a;
            const bool merge = spaceBefore && !b.spans.empty() && b.spans.back().y == y && b.spans.back().role == role;
            if (merge)
                b.spans.back().text += " " + text;   // one span for a run of one role on a line
            else
                b.spans.push_back({ spaceBefore ? x + a : x, y, lineH, text, role });
            if (role == Role::code)
            {
                // Inline code's ground: one rounded box over the run, spaces included.
                if (merge && !b.shapes.empty() && b.shapes.back().kind == ShapeKind::inlineCode)
                    b.shapes.back().r.w += static_cast<float>(text.size() + 1) * a;
                else
                {
                    const float cx = (spaceBefore ? x + a : x) - 2.0f;
                    b.shapes.push_back({ { cx, y + 2.0f, static_cast<float>(text.size()) * a + 4.0f, lineH - 4.0f },
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
                    place(chunk, part.role, space);
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

    void ChatView::layoutCode(Block& b, const std::string& lang, const std::vector<std::string>& lines, float w,
                              float& y) const
    {
        const float top = y;
        if (advCode_ <= 0.0f)
            advCode_ = funkgui::text::width(funkgui::FontService::get().atlas(), "0000000000", chat::kCodeText) / 10.0f;
        const float a = advCode_;
        std::string all;
        for (const std::string& l : lines)
            all += l + "\n";
        b.spans.push_back({ kCodePad, top + 3.0f, 18.0f, lang.empty() ? std::string("code") : lang, Role::faint });
        b.actions.push_back({ { w - 64.0f, top + 3.0f, 58.0f, 18.0f }, ActionKind::copyCode, all });
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
                b.code.push_back(std::move(cl));
                y += chat::kCodeLine;
                from += len;
            } while (from < line.size());
        }
        y += kCodePad - 4.0f;
        b.shapes.push_back({ { 0.0f, top, w, y - top }, ShapeKind::codeCard });
    }

    void ChatView::layoutMarkdown(Block& b, const std::string& text, float w, float& y) const
    {
        const std::vector<std::string> lines = splitLines(text);
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
            y = flow(b, para, Role::body, 0.0f, w, y, chat::kBodyLine);
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
                std::size_t j = i + 1;
                for (; j < lines.size() && trimLeft(lines[j]).rfind("```", 0) != 0; ++j)
                    code.push_back(lines[j]);
                space(10.0f);
                layoutCode(b, lang, code, w, y);
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
                y = flow(b, t.substr(static_cast<std::size_t>(level) + 1), r, 0.0f, w, y, level <= 2 ? 22.0f : 20.0f);
                continue;
            }
            if (isRule(t))
            {
                flushPara();
                space(8.0f);
                b.shapes.push_back({ { 0.0f, y + 4.0f, w, 1.0f }, ShapeKind::rule });
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
                    b.shapes.push_back({ { indent + 4.0f, y + chat::kBodyLine * 0.5f - 2.0f, 4.0f, 4.0f }, ShapeKind::bullet });
                else
                    b.spans.push_back({ indent, y, chat::kBodyLine, numbered->first, Role::dim });
                const float textX = indent + (bullet ? 16.0f : adv(Role::body) * static_cast<float>(numbered->first.size() + 1));
                y = flow(b, item, Role::body, textX, w - textX, y, chat::kBodyLine);
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
                y = flow(b, quote, Role::quote, 12.0f, w - 12.0f, y, chat::kSmallLine + 2.0f);
                b.shapes.push_back({ { 0.0f, top + 2.0f, 2.0f, y - top - 4.0f }, ShapeKind::quoteBar });
                continue;
            }
            para += (para.empty() ? "" : " ") + t;
        }
        flushPara();
    }

    void ChatView::layoutBlock(Block& b, const std::vector<ChatEntry>& entries, int count, bool copyAfter, float w, bool live)
    {
        b.spans.clear();
        b.shapes.clear();
        b.code.clear();
        b.actions.clear();
        b.icons.clear();
        b.tiles.clear();
        b.value.clear();
        float y = 0.0f;
        const ChatEntry& e = entries[static_cast<std::size_t>(b.entry)];
        const std::string t = asciiText(e.text);
        switch (b.kind)
        {
            case ChatEntry::Kind::user:
            {
                b.label = "You";
                b.value = t;
                // The pictures, above the bubble at its right: rows of thumbnails kTileH high (each as wide as its
                // proportions ask, within limits, cropped to fill when a limit cuts it).
                float top = 0.0f;
                if (!e.pictures.empty())
                {
                    const float rowW = std::floor(w * kBubbleShare);
                    std::vector<Tile> row;
                    float rowWidth = 0.0f, y0 = 0.0f;
                    const auto flush = [&] {
                        float x = w - rowWidth;
                        for (Tile tl : row)
                        {
                            tl.r.x = x;
                            tl.r.y = y0;
                            x += tl.r.w + kTileGap;
                            b.tiles.push_back(tl);
                        }
                        row.clear();
                        rowWidth = 0.0f;
                        y0 += kTileH + kTileGap;
                    };
                    std::string names;
                    for (const ChatPicturePtr& pic : e.pictures)
                    {
                        const float aspect = pic->height > 0 ? static_cast<float>(pic->width) / static_cast<float>(pic->height) : 1.0f;
                        const float tw = std::clamp(std::round(kTileH * aspect), kTileMinW, std::min(kTileMaxW, rowW));
                        Tile tl;
                        tl.id = pic->id;
                        tl.r = { 0.0f, 0.0f, tw, kTileH };
                        // Cropped to the tile's proportions, about the middle.
                        const float want = tw / kTileH;
                        if (aspect > want)
                            tl.uv = { 0.5f * (1.0f - want / aspect), 0.0f, want / aspect, 1.0f };
                        else if (aspect < want)
                            tl.uv = { 0.0f, 0.5f * (1.0f - aspect / want), 1.0f, aspect / want };
                        if (!row.empty() && rowWidth + kTileGap + tw > rowW)
                            flush();
                        rowWidth += (row.empty() ? 0.0f : kTileGap) + tw;
                        row.push_back(tl);
                        names += (names.empty() ? "" : ", ") + asciiText(pic->name);
                    }
                    flush();
                    top = y0 - kTileGap;
                    b.value = std::to_string(e.pictures.size()) + (e.pictures.size() == 1 ? " picture (" : " pictures (") + names
                            + ")" + (t.empty() ? std::string() : ". " + t);
                    if (t.empty())
                    {
                        y = top;
                        break;
                    }
                    top += 6.0f;
                }
                const float maxInner = std::floor(w * kBubbleShare) - 2.0f * kBubblePadX;
                const float a = adv(Role::body);
                const auto cols = static_cast<std::size_t>(std::max(4.0f, std::floor(maxInner / a)));
                // The bubble is as wide as its longest line.
                Block tmp;
                float ty = kBubblePadY;
                for (const std::string& line : splitLines(t))
                {
                    if (line.empty())
                    {
                        ty += chat::kBodyLine;
                        continue;
                    }
                    std::string esc;
                    for (const char ch : line)
                        esc += ch == '`' || ch == '*' ? ' ' : ch;   // no markdown in the user's own words
                    ty = flow(tmp, esc, Role::body, 0.0f, maxInner, ty, chat::kBodyLine);
                }
                std::size_t widest = 0;
                for (const Span& s : tmp.spans)
                    widest = std::max(widest, static_cast<std::size_t>(std::lround((s.x) / a)) + s.text.size());
                const float inner = std::min(maxInner, static_cast<float>(std::min(widest, cols)) * a);
                const float bw = inner + 2.0f * kBubblePadX;
                const float x0 = w - bw;
                for (Span s : tmp.spans)
                {
                    s.x += x0 + kBubblePadX;
                    s.y += top;
                    b.spans.push_back(s);
                }
                y = top + ty + kBubblePadY;
                b.shapes.insert(b.shapes.begin(), Shape{ { x0, top, bw, y - top }, ShapeKind::bubble });
                break;
            }
            case ChatEntry::Kind::assistant:
                b.label = "Claude";
                b.value = t;
                layoutMarkdown(b, t, w, y);
                if (copyAfter)
                {
                    y += 2.0f;
                    b.actions.push_back({ { 0.0f, y, 58.0f, kCopyRow - 2.0f }, ActionKind::copyMessage, t });
                    y += kCopyRow - 2.0f;
                }
                break;
            case ChatEntry::Kind::thinking:
            {
                b.label = "Claude's thinking";
                b.value = t;
                b.actions.push_back({ { 0.0f, 0.0f, w, kThinkRow }, ActionKind::toggleThinking, {} });
                y = kThinkRow;
                if (open_[b.entry])
                {
                    y += 2.0f;
                    const float top = y;
                    for (const std::string& line : splitLines(t))
                        if (!trimLeft(line).empty())
                            y = flow(b, trimLeft(line), Role::quote, 14.0f, w - 14.0f, y, chat::kSmallLine + 2.0f);
                    b.shapes.push_back({ { 4.0f, top + 1.0f, 2.0f, y - top - 2.0f }, ShapeKind::quoteBar });
                    y += 4.0f;
                }
                break;
            }
            case ChatEntry::Kind::tool:
            {
                b.label = "What Claude did";
                const float a = adv(Role::small);
                const auto cols = static_cast<std::size_t>(std::max(4.0f, std::floor((w - 40.0f) / a)));
                y = 4.0f;
                for (int k = 0; k < count; ++k)
                {
                    std::string line = capitalised(asciiText(entries[static_cast<std::size_t>(b.entry + k)].text));
                    b.value += (b.value.empty() ? "" : "; ") + line;
                    if (line.size() > cols)
                        line = line.substr(0, cols - 3) + "...";
                    const bool bad = failed(line);
                    b.icons.push_back({ 18.0f, y + kToolRow * 0.5f, bad ? chat::Icon::alert : toolIcon(line),
                                        bad ? Role::danger : Role::dim });
                    b.spans.push_back({ 34.0f, y, kToolRow, line, bad ? Role::danger : Role::small });
                    y += kToolRow;
                    // What the tool showed Claude (view_sound's picture), under its row, as wide as fits.
                    for (const ChatPicturePtr& pic : entries[static_cast<std::size_t>(b.entry + k)].pictures)
                    {
                        const float aspect = pic->height > 0 ? static_cast<float>(pic->width) / static_cast<float>(pic->height) : 1.4f;
                        const float tw = std::floor(std::min(kToolPictureMaxW, w - 34.0f - 12.0f));
                        Tile tl;
                        tl.id = pic->id;
                        tl.r = { 34.0f, y + 2.0f, tw, std::floor(tw / aspect) };
                        b.tiles.push_back(tl);
                        y += tl.r.h + 8.0f;
                    }
                }
                y += 4.0f;
                b.shapes.push_back({ { 0.0f, 0.0f, w, y }, ShapeKind::toolCard });
                break;
            }
            case ChatEntry::Kind::error:
            {
                b.label = "Error";
                b.value = t;
                y = 8.0f;
                b.icons.push_back({ 16.0f, y + chat::kBodyLine * 0.5f, chat::Icon::alert, Role::danger });
                y = flow(b, t, Role::body, 32.0f, w - 44.0f, y, chat::kBodyLine) + 8.0f;
                b.shapes.push_back({ { 0.0f, 0.0f, w, y }, ShapeKind::errorCard });
                break;
            }
            case ChatEntry::Kind::question:
            {
                b.label = "Claude asks";
                b.value = t;
                b.questions = e.questions;
                b.answers = e.answers;
                b.open = e.open;
                const float inner = w - 2.0f * kCardPad;
                y = 10.0f;
                b.icons.push_back({ kCardPad + 6.0f, y + chat::kSmallLine * 0.5f, chat::Icon::question, Role::accent });
                const std::string title = b.questions.size() > 1
                                              ? "Claude asks " + std::to_string(b.questions.size()) + " questions"
                                              : std::string("Claude asks");
                b.spans.push_back({ kCardPad + 18.0f, y, chat::kSmallLine, title, Role::smallStrong });
                y += chat::kSmallLine + 8.0f;
                for (int qi = 0; qi < static_cast<int>(b.questions.size()); ++qi)
                {
                    const ChatQuestion& q = b.questions[static_cast<std::size_t>(qi)];
                    if (qi > 0)
                        y += 6.0f;
                    if (!q.header.empty())
                    {
                        std::string tag = asciiText(q.header);
                        for (char& ch : tag)
                            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                        b.spans.push_back({ kCardPad, y, chat::kSmallLine, tag, Role::accent });
                        y += chat::kSmallLine + 2.0f;
                    }
                    y = flow(b, asciiText(q.question) + (q.multi ? " (choose any)" : ""), Role::strong, kCardPad, inner, y,
                             chat::kBodyLine) + 6.0f;
                    for (int oi = 0; oi < static_cast<int>(q.options.size()); ++oi)
                    {
                        const ChatOption& o = q.options[static_cast<std::size_t>(oi)];
                        const bool chosen = picked(b, qi, oi);
                        const bool muted = !b.open && !chosen;
                        const float top = y;
                        float ty = flow(b, asciiText(o.label), muted ? Role::dim : Role::body, kCardPad + kOptionText,
                                        inner - kOptionText - 12.0f, top + kOptionPadY, chat::kBodyLine);
                        if (!o.description.empty())
                            ty = flow(b, asciiText(o.description), muted ? Role::faint : Role::small, kCardPad + kOptionText,
                                      inner - kOptionText - 12.0f, ty, chat::kSmallLine + 1.0f);
                        const Rect r{ kCardPad, top, inner, ty + kOptionPadY - top };
                        b.shapes.push_back({ r, ShapeKind::option, qi, oi });
                        if (b.open)
                            b.actions.push_back({ r, ActionKind::option, {}, qi, oi });
                        y = r.bottom() + kOptionGap;
                    }
                }
                if (b.open && needsSubmit(b))
                {
                    y += 2.0f;
                    b.actions.push_back({ { w - kCardPad - kSubmitW, y, kSubmitW, kSubmitH }, ActionKind::submit, {} });
                    y += kSubmitH + 8.0f;
                }
                const std::string hint = b.open ? std::string("Or write your own answer below.")
                                       : e.typed ? std::string("Answered in your message.")
                                       : std::all_of(b.answers.begin(), b.answers.end(), [](const std::string& a) { return a.empty(); })
                                           ? std::string("Not answered.")
                                           : std::string();
                if (!hint.empty())
                {
                    b.spans.push_back({ kCardPad, y, chat::kSmallLine, hint, Role::faint });
                    y += chat::kSmallLine;
                }
                y += 10.0f;
                b.shapes.insert(b.shapes.begin(), Shape{ { 0.0f, 0.0f, w, y }, ShapeKind::questionCard });
                break;
            }
            case ChatEntry::Kind::checks:
            {
                b.checks = e.checks;
                int pass = 0;
                for (const ChatCheck& ck : b.checks)
                    pass += ck.measured && ck.pass ? 1 : 0;
                const int all = static_cast<int>(b.checks.size());
                b.label = "Claude's checks";
                b.value = t;
                const float inner = w - 2.0f * kCardPad;
                y = 10.0f;
                b.icons.push_back({ kCardPad + 6.0f, y + chat::kSmallLine * 0.5f, chat::Icon::list,
                                    pass == all ? Role::accent : Role::danger });
                b.spans.push_back({ kCardPad + 18.0f, y, chat::kSmallLine,
                                    "Checks: " + std::to_string(pass) + " of " + std::to_string(all) + " pass",
                                    Role::smallStrong });
                if (live)
                    b.actions.push_back({ { w - kCardPad - 48.0f, y - 2.0f, 48.0f, chat::kSmallLine + 4.0f },
                                          ActionKind::clearChecks, {} });
                y += chat::kSmallLine + 8.0f;
                for (const ChatCheck& ck : b.checks)
                {
                    const bool ok = ck.measured && ck.pass;
                    b.icons.push_back({ kCardPad + 6.0f, y + kCheckRow * 0.5f,
                                        !ck.measured ? chat::Icon::alert : ok ? chat::Icon::check : chat::Icon::cross,
                                        ok ? Role::accent : ck.measured ? Role::danger : Role::dim });
                    y = flow(b, asciiText(ck.label), ok ? Role::body : Role::strong, kCardPad + 20.0f, inner - 20.0f, y, kCheckRow);
                    y = flow(b, asciiText(ck.detail), ok ? Role::faint : Role::small, kCardPad + 20.0f, inner - 20.0f, y,
                             chat::kSmallLine + 1.0f) + 5.0f;
                }
                y += 6.0f;
                b.shapes.insert(b.shapes.begin(), Shape{ { 0.0f, 0.0f, w, y }, ShapeKind::questionCard });
                break;
            }
            case ChatEntry::Kind::note:
            {
                b.label = "Note";
                b.value = t;
                const float a = adv(Role::faint);
                const float lw = std::floor(w * 0.88f);
                Block tmp;
                const float end = flow(tmp, t, Role::faint, 0.0f, lw, 0.0f, chat::kSmallLine);
                // Each line centred.
                for (Span s : tmp.spans)
                {
                    float lineW = 0.0f;
                    for (const Span& o : tmp.spans)
                        if (o.y == s.y)
                            lineW = std::max(lineW, o.x + static_cast<float>(o.text.size()) * a);
                    s.x += std::floor((w - lineW) * 0.5f);
                    b.spans.push_back(s);
                }
                y = end;
                break;
            }
        }
        b.h = y;
    }

    void ChatView::layoutEmpty(float w)
    {
        empty_ = EmptyLayout{};
        Block tmp;
        float y = 0.0f;
        const float a = adv(Role::h2);
        const float tw = static_cast<float>(emptyText_.title.size()) * a;
        tmp.spans.push_back({ std::floor((w - tw) * 0.5f), y, 22.0f, emptyText_.title, Role::h2 });
        y += 28.0f;
        const float bodyW = std::min(w, 360.0f);
        Block body;
        const float end = flow(body, emptyText_.body, Role::small, 0.0f, bodyW, 0.0f, chat::kSmallLine + 1.0f);
        const float sa = adv(Role::small);
        for (Span s : body.spans)
        {
            float lineW = 0.0f;
            for (const Span& o : body.spans)
                if (o.y == s.y)
                    lineW = std::max(lineW, o.x + static_cast<float>(o.text.size()) * sa);
            s.x += std::floor((w - lineW) * 0.5f);
            s.y += y;
            tmp.spans.push_back(s);
        }
        y += end + 16.0f;
        if (!ready_)
        {
            const float sw = 112.0f, kw = 156.0f, gap = 8.0f;
            const float x0 = std::floor((w - sw - gap - kw) * 0.5f);
            empty_.signIn = { x0, y, sw, 32.0f };
            empty_.addKey = { x0 + sw + gap, y, kw, 32.0f };
            y += 32.0f + 20.0f;
        }
        const float cw = std::min(w, 380.0f);
        for (const std::string& s : emptyText_.suggestions)
        {
            const float ca = adv(Role::small);
            const auto cols = static_cast<std::size_t>(std::max(4.0f, std::floor((cw - 40.0f) / ca)));
            empty_.chips.push_back({ std::floor((w - cw) * 0.5f), y, cw, 32.0f });
            empty_.chipText.push_back(s.size() > cols ? s.substr(0, cols - 3) + "..." : s);
            y += 32.0f + 8.0f;
        }
        empty_.spans = std::move(tmp.spans);
        empty_.h = y;
    }

    void ChatView::show(const ChatState& state, const Empty& empty)
    {
        const float w = contentWidth();
        const bool wasAtEnd = follow_ || atEnd();
        ready_ = state.ready();
        busy_ = state.busy;
        activity_ = asciiText(state.activity);
        if (state.entries.size() < entryCount_)
        {
            open_.clear();                           // a new chat
            picks_.clear();
        }
        entryCount_ = state.entries.size();
        text_ = state.entries.empty() ? std::string() : transcriptText(state.entries);

        // The blocks: one an entry, but a run of tool entries is one card.
        struct Desc
        {
            ChatEntry::Kind kind;
            int entry, count;
        };
        std::vector<Desc> descs;
        for (int i = 0; i < static_cast<int>(state.entries.size()); ++i)
        {
            const ChatEntry::Kind k = state.entries[static_cast<std::size_t>(i)].kind;
            if (k == ChatEntry::Kind::tool && !descs.empty() && descs.back().kind == ChatEntry::Kind::tool)
                ++descs.back().count;
            else
                descs.push_back({ k, i, 1 });
        }
        blocks_.resize(descs.size());
        std::size_t liveChecks = descs.size();               // the last checks card, while the checks are set
        for (std::size_t d = descs.size(); state.checks && d-- > 0;)
            if (descs[d].kind == ChatEntry::Kind::checks)
            {
                liveChecks = d;
                break;
            }
        float y = kPadTop;
        for (std::size_t d = 0; d < descs.size(); ++d)
        {
            Block& b = blocks_[d];
            const Desc& desc = descs[d];
            // A Copy goes under an answer that ends its turn: the next block is the user's, or there is none and
            // Claude is done.
            bool endsTurn = desc.kind == ChatEntry::Kind::assistant;
            for (std::size_t n = d + 1; endsTurn && n < descs.size(); ++n)
            {
                if (descs[n].kind == ChatEntry::Kind::user)
                    break;
                if (descs[n].kind == ChatEntry::Kind::assistant)
                    endsTurn = false;
            }
            if (endsTurn && d + 1 == descs.size() && state.busy)
                endsTurn = false;
            std::string key = std::to_string(static_cast<int>(desc.kind)) + '|' + std::to_string(desc.count) + '|'
                            + std::to_string(w) + '|' + (endsTurn ? '1' : '0') + '|'
                            + (desc.kind == ChatEntry::Kind::thinking && open_[desc.entry] ? '1' : '0') + '|';
            for (int k = 0; k < desc.count; ++k)
                key += state.entries[static_cast<std::size_t>(desc.entry + k)].text + '\x1f';
            for (int k = 0; k < desc.count; ++k)
                for (const ChatPicturePtr& pic : state.entries[static_cast<std::size_t>(desc.entry + k)].pictures)
                    key += 'p' + std::to_string(pic->id);
            if (desc.kind == ChatEntry::Kind::checks)
                key += d == liveChecks ? "L" : "l";
            if (desc.kind == ChatEntry::Kind::question)
            {
                const ChatEntry& q = state.entries[static_cast<std::size_t>(desc.entry)];
                key += std::string(q.open ? "o" : "c") + (q.typed ? "y" : "n");
                for (const std::string& a : q.answers)
                    key += '\x1e' + a;
            }
            b.kind = desc.kind;
            b.entry = desc.entry;
            if (key != b.key)
            {
                b.key = std::move(key);
                layoutBlock(b, state.entries, desc.count, endsTurn, w, d == liveChecks);
            }
            const bool user = b.kind == ChatEntry::Kind::user;
            const bool afterUser = d > 0 && blocks_[d - 1].kind == ChatEntry::Kind::user;
            if (d > 0)
                y += user || afterUser ? kGapUser : kGap;
            b.y = y;
            y += b.h;
        }
        const Block* last = blocks_.empty() ? nullptr : &blocks_.back();
        liveText_ = busy_ && last != nullptr
                 && (last->kind == ChatEntry::Kind::thinking || last->kind == ChatEntry::Kind::assistant);
        typingY_ = y + (blocks_.empty() ? 0.0f : kGap);
        if (busy_ && !(last != nullptr && last->kind == ChatEntry::Kind::thinking))
            y = typingY_ + kWorkingRow;
        emptyText_ = empty;
        if (blocks_.empty())
            layoutEmpty(w);
        contentH_ = y + kPadBottom;
        if (wasAtEnd)
            scroll_ = maxScroll();
        scroll_ = std::clamp(scroll_, 0.0f, maxScroll());
        follow_ = atEnd();
    }

    // ---- geometry ----------------------------------------------------------------------------------------------------
    Rect ChatView::onScreen(const Block& b, const Rect& r) const
    {
        return { bounds_.x + kPadX + r.x, bounds_.y + b.y - scroll_ + r.y, r.w, r.h };
    }

    bool ChatView::live(const Block& b) const { return liveText_ && &b == &blocks_.back(); }

    void ChatView::hit(Point p, int& block, int& action) const
    {
        block = action = -1;
        if (!bounds_.contains(p))
            return;
        for (int i = 0; i < static_cast<int>(blocks_.size()); ++i)
        {
            const Block& b = blocks_[static_cast<std::size_t>(i)];
            for (int k = 0; k < static_cast<int>(b.actions.size()); ++k)
                if (onScreen(b, b.actions[static_cast<std::size_t>(k)].r).contains(p))
                {
                    block = i;
                    action = k;
                    return;
                }
        }
    }

    std::vector<std::pair<int, int>> ChatView::actionsOf(ActionKind kind) const
    {
        std::vector<std::pair<int, int>> out;
        for (int i = 0; i < static_cast<int>(blocks_.size()); ++i)
        {
            const Block& b = blocks_[static_cast<std::size_t>(i)];
            for (int k = 0; k < static_cast<int>(b.actions.size()); ++k)
                if (b.actions[static_cast<std::size_t>(k)].kind == kind)
                    out.push_back({ i, k });
        }
        return out;
    }

    void ChatView::act(int block, int action)
    {
        if (block < 0 || block >= static_cast<int>(blocks_.size()))
            return;
        Block& b = blocks_[static_cast<std::size_t>(block)];
        if (action < 0 || action >= static_cast<int>(b.actions.size()))
            return;
        const Action& a = b.actions[static_cast<std::size_t>(action)];
        if (a.kind == ActionKind::toggleThinking)
        {
            open_[b.entry] = !open_[b.entry];
            b.key.clear();                           // lays out again at the next show()
            return;
        }
        if (a.kind == ActionKind::clearChecks)
        {
            if (hooks_.clearChecks)
                hooks_.clearChecks();
            return;
        }
        if (a.kind == ActionKind::option || a.kind == ActionKind::submit)
        {
            if (!b.open)
                return;
            std::vector<std::vector<bool>>& pk = picks_[b.entry];
            pk.resize(b.questions.size());
            for (std::size_t q = 0; q < b.questions.size(); ++q)
                pk[q].resize(b.questions[q].options.size(), false);
            if (a.kind == ActionKind::option)
            {
                std::vector<bool>& row = pk[static_cast<std::size_t>(a.q)];
                const bool was = row[static_cast<std::size_t>(a.o)];
                if (!b.questions[static_cast<std::size_t>(a.q)].multi)
                    std::fill(row.begin(), row.end(), false);
                row[static_cast<std::size_t>(a.o)] = !was || !b.questions[static_cast<std::size_t>(a.q)].multi;
                if (needsSubmit(b))
                    return;                          // Send answers sends them
            }
            if (!complete(b))
                return;
            std::vector<std::string> answers;
            for (std::size_t q = 0; q < b.questions.size(); ++q)
            {
                std::string chosen;
                for (std::size_t o = 0; o < b.questions[q].options.size(); ++o)
                    if (pk[q][o])
                        chosen += (chosen.empty() ? "" : ", ") + b.questions[q].options[o].label;
                answers.push_back(chosen);
            }
            if (hooks_.answer)
                hooks_.answer(answers);
            return;
        }
        if (host_ != nullptr)
            host_->copyText(a.payload);
        copiedFor_ = 1.4f;
        copiedBlock_ = block;
        copiedAction_ = action;
    }

    // ---- frame and input ------------------------------------------------------------------------------------------
    void ChatView::tick(float dt, bool, bool, Point pointer)
    {
        pointer_ = pointer;
        if (busy_)
            phase_ = std::fmod(phase_ + dt, 1.2f);
        if (copiedFor_ > 0.0f)
            copiedFor_ = std::max(0.0f, copiedFor_ - dt);
    }

    bool ChatView::settled() const { return !busy_ && copiedFor_ <= 0.0f; }

    funkgui::Cursor ChatView::cursorAt(Point p) const
    {
        int b = -1, a = -1;
        hit(p, b, a);
        if (b >= 0)
            return funkgui::Cursor::pointingHand;
        if (blocks_.empty())
        {
            const Point o{ bounds_.x + kPadX, bounds_.y + std::max(kPadTop, (bounds_.h - empty_.h) * 0.42f) };
            const Point q{ p.x - o.x, p.y - o.y };
            if (empty_.addKey.contains(q) || empty_.signIn.contains(q))
                return funkgui::Cursor::pointingHand;
            for (const Rect& r : empty_.chips)
                if (r.contains(q))
                    return funkgui::Cursor::pointingHand;
        }
        if (const auto j = centreOfJump(); j && std::hypot(p.x - j->x, p.y - j->y) <= kJump * 0.5f)
            return funkgui::Cursor::pointingHand;
        return funkgui::Cursor::normal;
    }

    void ChatView::pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController&, funkgui::HostServices&)
    {
        const Point p{ e.x, e.y };
        if (const auto j = centreOfJump(); j && std::hypot(p.x - j->x, p.y - j->y) <= kJump * 0.5f)
        {
            scroll_ = maxScroll();
            follow_ = true;
            return;
        }
        if (blocks_.empty())
        {
            const Point o{ bounds_.x + kPadX, bounds_.y + std::max(kPadTop, (bounds_.h - empty_.h) * 0.42f) };
            const Point q{ p.x - o.x, p.y - o.y };
            if (!empty_.addKey.isEmpty() && empty_.addKey.contains(q))
            {
                if (hooks_.addKey)
                    hooks_.addKey();
                return;
            }
            if (!empty_.signIn.isEmpty() && empty_.signIn.contains(q))
            {
                if (hooks_.signIn)
                    hooks_.signIn();
                return;
            }
            for (std::size_t i = 0; i < empty_.chips.size(); ++i)
                if (empty_.chips[i].contains(q) && hooks_.suggest)
                {
                    hooks_.suggest(emptyText_.suggestions[i]);
                    return;
                }
            return;
        }
        int b = -1, a = -1;
        hit(p, b, a);
        act(b, a);
    }

    bool ChatView::wheel(const funkgui::WheelEvent& e, funkgui::GestureController&, double)
    {
        if (!bounds_.contains({ e.x, e.y }) || maxScroll() <= 0.0f)
            return false;
        scroll_ = std::clamp(scroll_ - e.dy * (e.smooth ? kWheelSmooth : kWheelNotched), 0.0f, maxScroll());
        follow_ = atEnd();
        return true;
    }

    // ---- probes -----------------------------------------------------------------------------------------------------
    std::optional<Point> ChatView::centreOfSuggestion(int i) const
    {
        if (!blocks_.empty() || i < 0 || i >= static_cast<int>(empty_.chips.size()))
            return std::nullopt;
        const Point o{ bounds_.x + kPadX, bounds_.y + std::max(kPadTop, (bounds_.h - empty_.h) * 0.42f) };
        const Rect& r = empty_.chips[static_cast<std::size_t>(i)];
        return Point{ o.x + r.centreX(), o.y + r.centreY() };
    }

    std::optional<Point> ChatView::centreOfAddKey() const
    {
        if (!blocks_.empty() || empty_.addKey.isEmpty())
            return std::nullopt;
        const Point o{ bounds_.x + kPadX, bounds_.y + std::max(kPadTop, (bounds_.h - empty_.h) * 0.42f) };
        return Point{ o.x + empty_.addKey.centreX(), o.y + empty_.addKey.centreY() };
    }

    std::optional<Point> ChatView::centreOfSignIn() const
    {
        if (!blocks_.empty() || empty_.signIn.isEmpty())
            return std::nullopt;
        const Point o{ bounds_.x + kPadX, bounds_.y + std::max(kPadTop, (bounds_.h - empty_.h) * 0.42f) };
        return Point{ o.x + empty_.signIn.centreX(), o.y + empty_.signIn.centreY() };
    }

    std::optional<Point> ChatView::centreOfCopy(int n) const { return centreOfAction(ActionKind::copyCode, n); }

    int ChatView::tiles() const
    {
        int n = 0;
        for (const Block& b : blocks_)
            n += static_cast<int>(b.tiles.size());
        return n;
    }

    // ---- questions --------------------------------------------------------------------------------------------------
    bool ChatView::needsSubmit(const Block& b)
    {
        return b.questions.size() > 1 || (b.questions.size() == 1 && b.questions[0].multi);
    }

    bool ChatView::picked(const Block& b, int q, int o) const
    {
        if (q < 0 || q >= static_cast<int>(b.questions.size()) || o < 0
            || o >= static_cast<int>(b.questions[static_cast<std::size_t>(q)].options.size()))
            return false;
        if (b.open)
        {
            const auto it = picks_.find(b.entry);
            return it != picks_.end() && q < static_cast<int>(it->second.size())
                && o < static_cast<int>(it->second[static_cast<std::size_t>(q)].size())
                && it->second[static_cast<std::size_t>(q)][static_cast<std::size_t>(o)];
        }
        if (q >= static_cast<int>(b.answers.size()))
            return false;
        // The answer: the labels chosen, ", " between them.
        const std::string& a = b.answers[static_cast<std::size_t>(q)];
        const std::string& l = b.questions[static_cast<std::size_t>(q)].options[static_cast<std::size_t>(o)].label;
        return a == l || a.rfind(l + ", ", 0) == 0 || a.find(", " + l + ", ") != std::string::npos
            || (a.size() > l.size() + 2 && a.compare(a.size() - l.size() - 2, std::string::npos, ", " + l) == 0);
    }

    bool ChatView::complete(const Block& b) const
    {
        for (int q = 0; q < static_cast<int>(b.questions.size()); ++q)
        {
            bool any = false;
            for (int o = 0; o < static_cast<int>(b.questions[static_cast<std::size_t>(q)].options.size()); ++o)
                any = any || picked(b, q, o);
            if (!any)
                return false;
        }
        return !b.questions.empty();
    }

    int ChatView::openQuestion() const
    {
        for (int i = static_cast<int>(blocks_.size()); --i >= 0;)
            if (blocks_[static_cast<std::size_t>(i)].kind == ChatEntry::Kind::question && blocks_[static_cast<std::size_t>(i)].open)
                return i;
        return -1;
    }

    std::optional<Point> ChatView::centreOfOption(int q, int o) const
    {
        const int bi = openQuestion();
        if (bi < 0)
            return std::nullopt;
        const Block& b = blocks_[static_cast<std::size_t>(bi)];
        for (const Action& a : b.actions)
            if (a.kind == ActionKind::option && a.q == q && a.o == o)
            {
                const Rect r = onScreen(b, a.r);
                if (r.y < bounds_.y || r.bottom() > bounds_.bottom())
                    return std::nullopt;
                return Point{ r.centreX(), r.centreY() };
            }
        return std::nullopt;
    }

    std::optional<Point> ChatView::centreOfClearChecks() const
    {
        const std::vector<std::pair<int, int>> all = actionsOf(ActionKind::clearChecks);
        if (all.empty())
            return std::nullopt;
        const Block& b = blocks_[static_cast<std::size_t>(all.back().first)];
        const Rect r = onScreen(b, b.actions[static_cast<std::size_t>(all.back().second)].r);
        if (r.y < bounds_.y || r.bottom() > bounds_.bottom())
            return std::nullopt;
        return Point{ r.centreX(), r.centreY() };
    }

    int ChatView::checkRows() const
    {
        int n = 0;
        for (const Block& b : blocks_)
            n += b.kind == ChatEntry::Kind::checks ? static_cast<int>(b.checks.size()) : 0;
        return n;
    }

    std::optional<Point> ChatView::centreOfSubmit() const
    {
        const int bi = openQuestion();
        if (bi < 0)
            return std::nullopt;
        const Block& b = blocks_[static_cast<std::size_t>(bi)];
        for (const Action& a : b.actions)
            if (a.kind == ActionKind::submit)
            {
                const Rect r = onScreen(b, a.r);
                if (r.y < bounds_.y || r.bottom() > bounds_.bottom())
                    return std::nullopt;
                return Point{ r.centreX(), r.centreY() };
            }
        return std::nullopt;
    }

    bool ChatView::optionPicked(int q, int o) const
    {
        const int bi = openQuestion();
        return bi >= 0 && picked(blocks_[static_cast<std::size_t>(bi)], q, o);
    }
    std::optional<Point> ChatView::centreOfAnswerCopy(int n) const { return centreOfAction(ActionKind::copyMessage, n); }

    std::optional<Point> ChatView::centreOfAction(ActionKind kind, int n) const
    {
        const auto all = actionsOf(kind);
        if (n < 0 || n >= static_cast<int>(all.size()))
            return std::nullopt;
        const Block& b = blocks_[static_cast<std::size_t>(all[static_cast<std::size_t>(n)].first)];
        const Rect r = onScreen(b, b.actions[static_cast<std::size_t>(all[static_cast<std::size_t>(n)].second)].r);
        if (!bounds_.contains({ r.centreX(), r.centreY() }))
            return std::nullopt;
        return Point{ r.centreX(), r.centreY() };
    }

    int ChatView::copies() const { return static_cast<int>(actionsOf(ActionKind::copyCode).size()); }

    std::optional<Point> ChatView::centreOfThinking(int n) const
    {
        const auto all = actionsOf(ActionKind::toggleThinking);
        if (n < 0 || n >= static_cast<int>(all.size()))
            return std::nullopt;
        const Block& b = blocks_[static_cast<std::size_t>(all[static_cast<std::size_t>(n)].first)];
        const Rect r = onScreen(b, b.actions[static_cast<std::size_t>(all[static_cast<std::size_t>(n)].second)].r);
        const Point c{ r.x + 40.0f, r.centreY() };
        if (!bounds_.contains(c))
            return std::nullopt;
        return c;
    }

    bool ChatView::thinkingOpen(int n) const
    {
        const auto all = actionsOf(ActionKind::toggleThinking);
        if (n < 0 || n >= static_cast<int>(all.size()))
            return false;
        const auto it = open_.find(blocks_[static_cast<std::size_t>(all[static_cast<std::size_t>(n)].first)].entry);
        return it != open_.end() && it->second;
    }

    std::optional<Point> ChatView::centreOfJump() const
    {
        if (blocks_.empty() || scroll_ >= maxScroll() - 40.0f)
            return std::nullopt;
        return Point{ bounds_.centreX(), bounds_.bottom() - 10.0f - kJump * 0.5f };
    }

    // ---- drawing -------------------------------------------------------------------------------------------------------
    void ChatView::draw(funkgui::Canvas& c, const funkgui::Theme& th, bool) const
    {
        const chat::Palette p = chat::palette(th);
        const std::vector<funkgui::text::HighlightStyle> codePalette = funkgui::TextEditor::defaultPalette();
        c.pushClip(bounds_);
        const float x0 = bounds_.x + kPadX;
        const float w = contentWidth();
        const auto text = [&](const std::string& s, float x, float lineTop, float lineH, Role r, funkgui::Col col) {
            const funkgui::TextStyle& st = styleOf(r);
            c.text(s.c_str(), x, c.capCentreTop(lineTop + lineH * 0.5f, st), st, col);
        };

        if (blocks_.empty())
        {
            const float oy = bounds_.y + std::max(kPadTop, (bounds_.h - empty_.h) * 0.42f);
            for (const Span& s : empty_.spans)
                text(s.text, x0 + s.x, oy + s.y, s.lineH, s.role, colourOf(s.role, p));
            if (!empty_.signIn.isEmpty())
            {
                const Rect r{ x0 + empty_.signIn.x, oy + empty_.signIn.y, empty_.signIn.w, empty_.signIn.h };
                const bool hover = r.contains(pointer_);
                c.rrect(r.x, r.y, r.w, r.h, r.h * 0.5f, hover ? funkgui::mix(p.accent, p.text, 0.12f) : p.accent);
                chat::drawIcon(c, chat::Icon::person, r.x + 22.0f, r.centreY(), 13.0f, p.onAccent);
                text("Sign in", r.x + 38.0f, r.y, r.h, Role::smallStrong, p.onAccent);
            }
            if (!empty_.addKey.isEmpty())
            {
                const Rect r{ x0 + empty_.addKey.x, oy + empty_.addKey.y, empty_.addKey.w, empty_.addKey.h };
                const bool hover = r.contains(pointer_);
                c.rrect(r.x, r.y, r.w, r.h, r.h * 0.5f, hover ? p.hover : p.raised, 1.0f, p.composerEdge);
                chat::drawIcon(c, chat::Icon::key, r.x + 22.0f, r.centreY(), 13.0f, hover ? p.text : p.soft);
                text("Use an API key", r.x + 38.0f, r.y, r.h, Role::small, hover ? p.text : p.soft);
            }
            for (std::size_t i = 0; i < empty_.chips.size(); ++i)
            {
                const Rect r{ x0 + empty_.chips[i].x, oy + empty_.chips[i].y, empty_.chips[i].w, empty_.chips[i].h };
                const bool hover = r.contains(pointer_);
                c.rrect(r.x, r.y, r.w, r.h, 10.0f, hover ? p.hover : p.raised, 1.0f, p.cardEdge);
                chat::drawIcon(c, chat::Icon::arrowUp, r.x + 16.0f, r.centreY(), 11.0f, hover ? p.accent : p.faint);
                text(empty_.chipText[i], r.x + 30.0f, r.y, r.h, Role::small, hover ? p.text : p.soft);
            }
            c.popClip();
            return;
        }

        int hb = -1, ha = -1;
        hit(pointer_, hb, ha);
        for (int bi = 0; bi < static_cast<int>(blocks_.size()); ++bi)
        {
            const Block& b = blocks_[static_cast<std::size_t>(bi)];
            const float top = bounds_.y + b.y - scroll_;
            if (top > bounds_.bottom() || top + b.h < bounds_.y)
                continue;
            const bool hoverBlock = Rect{ bounds_.x, top, bounds_.w, b.h }.contains(pointer_);
            for (const Shape& s : b.shapes)
            {
                const Rect r = onScreen(b, s.r);
                switch (s.kind)
                {
                    case ShapeKind::bubble:
                        c.rrect4(r.x, r.y, r.w, r.h, 14.0f, 14.0f, 5.0f, 14.0f, p.bubble);
                        break;
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
                    case ShapeKind::toolCard:
                        c.rrect(r.x, r.y, r.w, r.h, 10.0f, p.raised, 1.0f, p.cardEdge);
                        break;
                    case ShapeKind::errorCard:
                        c.rrect(r.x, r.y, r.w, r.h, 10.0f, p.danger.withAlpha(0.10f), 1.0f, p.danger.withAlpha(0.5f));
                        break;
                    case ShapeKind::questionCard:
                        c.rrect(r.x, r.y, r.w, r.h, 12.0f, p.raised, 1.0f, b.open ? p.accent.withAlpha(0.45f) : p.cardEdge);
                        break;
                    case ShapeKind::option:
                    {
                        const bool on = picked(b, s.q, s.o);
                        const bool hover = b.open && r.contains(pointer_);
                        const funkgui::Col fill = on ? funkgui::mix(p.raised, p.accent, 0.16f) : hover ? p.hover : p.card;
                        c.rrect(r.x, r.y, r.w, r.h, 9.0f, fill, 1.0f, on ? p.accent : p.cardEdge);
                        const float ix = r.x + 17.0f, iy = r.y + kOptionPadY + chat::kBodyLine * 0.5f;
                        const bool multi = s.q >= 0 && s.q < static_cast<int>(b.questions.size())
                                        && b.questions[static_cast<std::size_t>(s.q)].multi;
                        const funkgui::Col ring = on ? p.accent : (b.open ? p.dim : p.faint);
                        if (multi)
                        {
                            c.rrect(ix - 6.5f, iy - 6.5f, 13.0f, 13.0f, 3.5f, on ? p.accent : ring.withAlpha(0.0f), 1.3f, ring);
                            if (on)
                                chat::drawIcon(c, chat::Icon::check, ix, iy, 9.0f, p.onAccent);
                        }
                        else
                        {
                            c.disc(ix, iy, 6.5f, ring.withAlpha(0.0f), 1.3f, ring);
                            if (on)
                                c.disc(ix, iy, 3.5f, p.accent);
                        }
                        break;
                    }
                }
            }
            for (const Tile& tl : b.tiles)
            {
                const Rect r = onScreen(b, tl.r);
                c.rrect(r.x, r.y, r.w, r.h, 12.0f, p.raised);
                c.image(tl.id, r, 12.0f, 1.0f, tl.uv);
                c.rrect(r.x, r.y, r.w, r.h, 12.0f, p.card.withAlpha(0.0f), 1.0f, p.cardEdge);
            }
            for (const Span& s : b.spans)
                text(s.text, x0 + s.x, top + s.y, s.lineH, s.role, colourOf(s.role, p));
            for (const IconAt& ic : b.icons)
                chat::drawIcon(c, ic.icon, x0 + ic.x, top + ic.y, 12.0f, colourOf(ic.role, p));
            if (!b.code.empty())
            {
                if (advCode_ <= 0.0f)
                    advCode_ = funkgui::text::width(funkgui::FontService::get().atlas(), "0000000000", chat::kCodeText) / 10.0f;
                const float ca = advCode_;
                for (const CodeLine& cl : b.code)
                {
                    const float ly = top + cl.y;
                    if (ly > bounds_.bottom() || ly + chat::kCodeLine < bounds_.y)
                        continue;
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
                        const funkgui::text::HighlightStyle& hs =
                            codePalette[std::min<std::size_t>(sp.style, codePalette.size() - 1)];
                        run(sp.start, sp.end, hs.resolve(th), hs.weight);
                        at = sp.end;
                    }
                    run(at, static_cast<int>(cl.text.size()), p.text, 0.0f);
                }
            }
            for (int ai = 0; ai < static_cast<int>(b.actions.size()); ++ai)
            {
                const Action& a = b.actions[static_cast<std::size_t>(ai)];
                const Rect r = onScreen(b, a.r);
                const bool hover = bi == hb && ai == ha;
                if (a.kind == ActionKind::option)
                    continue;                        // drawn as its shape
                if (a.kind == ActionKind::clearChecks)
                {
                    if (hover)
                        c.rrect(r.x, r.y, r.w, r.h, 6.0f, p.hover);
                    const std::string label = "Clear";
                    const float lw = static_cast<float>(label.size()) * adv(Role::small);
                    text(label, r.centreX() - lw * 0.5f, r.y, r.h, Role::small, hover ? p.text : p.dim);
                    continue;
                }
                if (a.kind == ActionKind::submit)
                {
                    const bool ready = complete(b);
                    if (ready)
                        c.rrect(r.x, r.y, r.w, r.h, r.h * 0.5f, hover ? funkgui::mix(p.accent, p.text, 0.12f) : p.accent);
                    else
                        c.rrect(r.x, r.y, r.w, r.h, r.h * 0.5f, p.card, 1.0f, p.cardEdge);
                    const std::string label = "Send answers";
                    const float lw = static_cast<float>(label.size()) * adv(Role::smallStrong);
                    text(label, r.centreX() - lw * 0.5f, r.y, r.h, Role::smallStrong, ready ? p.onAccent : p.faint);
                    continue;
                }
                if (a.kind == ActionKind::toggleThinking)
                {
                    const bool open = open_.count(b.entry) != 0 && open_.at(b.entry);
                    const funkgui::Col ink = hover ? p.text : p.dim;
                    chat::drawIcon(c, open ? chat::Icon::chevronDown : chat::Icon::chevronRight, r.x + 6.0f, r.centreY(), 9.0f, ink);
                    if (live(b))
                    {
                        text("Thinking", r.x + 18.0f, r.y, r.h, Role::small, ink);
                        const float dx = r.x + 18.0f + 8.0f * adv(Role::small) + 6.0f;
                        for (int k = 0; k < 3; ++k)
                        {
                            const float t = std::fmod(phase_ / 1.2f + 1.0f - static_cast<float>(k) * 0.18f, 1.0f);
                            const float amp = 0.35f + 0.65f * std::max(0.0f, std::sin(t * 3.14159265f));
                            c.disc(dx + static_cast<float>(k) * 7.0f, r.centreY() + 1.0f, 1.6f, funkgui::mix(p.card, ink, amp));
                        }
                    }
                    else
                        text("Thought process", r.x + 18.0f, r.y, r.h, Role::small, ink);
                    continue;
                }
                const bool copied = copiedFor_ > 0.0f && copiedBlock_ == bi && copiedAction_ == ai;
                if (a.kind == ActionKind::copyMessage && !hoverBlock && !copied)
                    continue;                        // shown under the pointer
                const funkgui::Col ink = copied ? p.accent : (hover ? p.text : p.dim);
                if (hover && !copied)
                    c.rrect(r.x, r.y, r.w, r.h, 6.0f, p.hover);
                chat::drawIcon(c, copied ? chat::Icon::check : chat::Icon::copy, r.x + 11.0f, r.centreY(), 10.0f, ink);
                text(copied ? "Copied" : "Copy", r.x + 22.0f, r.y, r.h, Role::small, ink);
            }
        }

        // While Claude works: three pulsing dots and what it is doing.
        const Block* last = blocks_.empty() ? nullptr : &blocks_.back();
        if (busy_ && !(last != nullptr && last->kind == ChatEntry::Kind::thinking && liveText_))
        {
            const float y = bounds_.y + typingY_ - scroll_;
            for (int k = 0; k < 3; ++k)
            {
                const float t = std::fmod(phase_ / 1.2f + 1.0f - static_cast<float>(k) * 0.18f, 1.0f);
                const float amp = 0.3f + 0.7f * std::max(0.0f, std::sin(t * 3.14159265f));
                c.disc(x0 + 5.0f + static_cast<float>(k) * 9.0f, y + kWorkingRow * 0.5f, 3.0f, funkgui::mix(p.card, p.accent, amp));
            }
            if (!activity_.empty())
                text(fitText(c, activity_, chat::kSmall, w - 40.0f), x0 + 36.0f, y, kWorkingRow, Role::small, p.dim);
        }

        // The scroll bar, and the way back to the end.
        if (maxScroll() > 0.0f)
        {
            const float trackH = bounds_.h - 8.0f;
            const float thumbH = std::max(24.0f, trackH * viewHeight() / contentH_);
            const float t = scroll_ / maxScroll();
            c.rrect(bounds_.right() - kBarW - 3.0f, bounds_.y + 4.0f + t * (trackH - thumbH), kBarW, thumbH, kBarW * 0.5f,
                    p.faint.withAlpha(0.7f));
        }
        if (const auto j = centreOfJump())
        {
            const bool hover = std::hypot(pointer_.x - j->x, pointer_.y - j->y) <= kJump * 0.5f;
            c.disc(j->x, j->y, kJump * 0.5f, hover ? p.hover : p.composer, 1.0f, p.composerEdge);
            chat::drawIcon(c, chat::Icon::chevronDown, j->x, j->y + 1.0f, 11.0f, hover ? p.text : p.soft);
        }
        c.popClip();
    }

    // ---- accessibility ----------------------------------------------------------------------------------------------
    void ChatView::accessibility(std::vector<funkgui::A11yItem>& out) const
    {
        funkgui::A11yItem list;
        list.id = base_;
        list.role = funkgui::a11yrole::list;
        list.bounds = bounds_;
        list.title = "Conversation with Claude";
        if (busy_ && !activity_.empty())
            list.value = activity_;
        out.push_back(list);
        if (blocks_.empty())
        {
            const Point o{ bounds_.x + kPadX, bounds_.y + std::max(kPadTop, (bounds_.h - empty_.h) * 0.42f) };
            funkgui::A11yItem intro;
            intro.id = base_ + 1;
            intro.parent = base_;
            intro.role = funkgui::A11yRole::staticText;
            intro.bounds = bounds_;
            intro.title = emptyText_.title;
            intro.value = emptyText_.body;
            intro.readOnly = true;
            out.push_back(intro);
            if (!empty_.signIn.isEmpty())
            {
                funkgui::A11yItem in;
                in.id = base_ + 14;
                in.parent = base_;
                in.role = funkgui::A11yRole::button;
                in.bounds = { o.x + empty_.signIn.x, o.y + empty_.signIn.y, empty_.signIn.w, empty_.signIn.h };
                in.title = "Sign in";
                out.push_back(in);
            }
            if (!empty_.addKey.isEmpty())
            {
                funkgui::A11yItem key;
                key.id = base_ + 2;
                key.parent = base_;
                key.role = funkgui::A11yRole::button;
                key.bounds = { o.x + empty_.addKey.x, o.y + empty_.addKey.y, empty_.addKey.w, empty_.addKey.h };
                key.title = "Use your own API key";
                out.push_back(key);
            }
            for (std::size_t i = 0; i < empty_.chips.size(); ++i)
            {
                funkgui::A11yItem chip;
                chip.id = base_ + 3 + static_cast<uint32_t>(i);
                chip.parent = base_;
                chip.role = funkgui::A11yRole::button;
                chip.bounds = { o.x + empty_.chips[i].x, o.y + empty_.chips[i].y, empty_.chips[i].w, empty_.chips[i].h };
                chip.title = "Suggestion: " + emptyText_.suggestions[i];
                out.push_back(chip);
            }
            return;
        }
        for (int bi = 0; bi < static_cast<int>(blocks_.size()); ++bi)
        {
            const Block& b = blocks_[static_cast<std::size_t>(bi)];
            const Rect whole = onScreen(b, { 0.0f, 0.0f, contentWidth(), b.h });
            const float y0 = std::max(whole.y, bounds_.y), y1 = std::min(whole.bottom(), bounds_.bottom());
            if (y1 <= y0)
                continue;                            // out of view
            funkgui::A11yItem item;
            item.id = base_ + 16 + static_cast<uint32_t>(bi) * kA11yStride;
            item.parent = base_;
            item.role = funkgui::A11yRole::listItem;
            item.bounds = { whole.x, y0, whole.w, y1 - y0 };
            item.title = b.label;
            item.value = b.value;
            item.readOnly = true;
            out.push_back(std::move(item));
            for (int ai = 0; ai < static_cast<int>(b.actions.size()) && ai < static_cast<int>(kA11yStride) - 1; ++ai)
            {
                const Action& a = b.actions[static_cast<std::size_t>(ai)];
                const Rect r = onScreen(b, a.r);
                if (r.y < bounds_.y || r.bottom() > bounds_.bottom())
                    continue;
                funkgui::A11yItem act;
                act.id = base_ + 16 + static_cast<uint32_t>(bi) * kA11yStride + 1 + static_cast<uint32_t>(ai);
                act.parent = base_;
                act.role = a.kind == ActionKind::toggleThinking ? funkgui::A11yRole::toggleButton : funkgui::A11yRole::button;
                act.bounds = r;
                if (a.kind == ActionKind::toggleThinking)
                {
                    act.title = "Show Claude's thinking";
                    act.checkable = true;
                    act.checked = open_.count(b.entry) != 0 && open_.at(b.entry);
                }
                else if (a.kind == ActionKind::option)
                {
                    const ChatQuestion& q = b.questions[static_cast<std::size_t>(a.q)];
                    const ChatOption& o = q.options[static_cast<std::size_t>(a.o)];
                    act.role = q.multi ? funkgui::A11yRole::toggleButton : funkgui::A11yRole::radioButton;
                    act.title = asciiText(o.label);
                    act.description = asciiText(o.description);
                    act.help = asciiText(q.question);
                    act.checkable = true;
                    act.checked = picked(b, a.q, a.o);
                }
                else if (a.kind == ActionKind::submit)
                {
                    act.title = "Send the answers";
                    act.enabled = complete(b);
                }
                else if (a.kind == ActionKind::clearChecks)
                {
                    act.title = "Clear the checks";
                    act.help = "The plugin stops measuring Claude's checks after each run";
                }
                else
                    act.title = a.kind == ActionKind::copyCode ? "Copy the code" : "Copy the answer";
                out.push_back(std::move(act));
            }
        }
        if (const auto j = centreOfJump())
        {
            funkgui::A11yItem jump;
            jump.id = base_ + 15;
            jump.parent = base_;
            jump.role = funkgui::A11yRole::button;
            jump.bounds = { j->x - kJump * 0.5f, j->y - kJump * 0.5f, kJump, kJump };
            jump.title = "Jump to the latest message";
            out.push_back(jump);
        }
    }

    bool ChatView::a11yAction(uint32_t id, funkgui::A11yAction, double, funkgui::GestureController&)
    {
        if (id < base_ || id >= base_ + (1u << 24))
            return false;
        const uint32_t k = id - base_;
        if (blocks_.empty())
        {
            if (k == 2 && hooks_.addKey)
                hooks_.addKey();
            else if (k == 14 && hooks_.signIn)
                hooks_.signIn();
            else if (k >= 3 && k - 3 < emptyText_.suggestions.size() && hooks_.suggest)
                hooks_.suggest(emptyText_.suggestions[k - 3]);
            return true;
        }
        if (k == 15)
        {
            scroll_ = maxScroll();
            follow_ = true;
            return true;
        }
        if (k >= 16 && (k - 16) % kA11yStride != 0)
            act(static_cast<int>((k - 16) / kA11yStride), static_cast<int>((k - 16) % kA11yStride) - 1);
        return true;
    }

    void ChatView::specLine(char* out, std::size_t n) const
    {
        int b = -1, a = -1;
        hit(pointer_, b, a);
        if (b >= 0)
        {
            const ActionKind k = blocks_[static_cast<std::size_t>(b)].actions[static_cast<std::size_t>(a)].kind;
            std::snprintf(out, n, "%s", k == ActionKind::toggleThinking ? "THINKING   SHOW OR HIDE WHAT CLAUDE THOUGHT"
                                        : k == ActionKind::copyCode     ? "COPY   THE CODE BLOCK TO THE CLIPBOARD"
                                        : k == ActionKind::option       ? "ANSWER   CHOOSE THIS FOR CLAUDE'S QUESTION"
                                        : k == ActionKind::submit       ? "SEND ANSWERS   CLAUDE GOES ON WITH THEM"
                                        : k == ActionKind::clearChecks  ? "CLEAR   THE PLUGIN STOPS MEASURING CLAUDE'S CHECKS"
                                                                        : "COPY   CLAUDE'S ANSWER TO THE CLIPBOARD");
            return;
        }
        std::snprintf(out, n, "%s", blocks_.empty() ? "CHAT   START FROM A SUGGESTION, OR TYPE BELOW"
                                                     : "CHAT   THE WHEEL SCROLLS   COPY UNDER AN ANSWER COPIES IT");
    }
}
