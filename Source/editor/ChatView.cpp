// Source/editor/ChatView.cpp: see ChatView.h.
#include "editor/ChatView.h"

#include "editor/ChatItems.h"
#include "editor/Markdown.h"

#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>
#include <funkgui/widgets/TextEditor.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace playground::ui
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
        constexpr float kToolRow = 24.0f;
        constexpr float kThinkRow = 22.0f;
        constexpr float kCopyRow = 20.0f;
        constexpr float kWorkingRow = 26.0f;
        constexpr float kJump = 28.0f;
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

        // The light markdown's own (Markdown.h), which this view lays out its answers in.
        using markdown::colourOf;
        using markdown::splitLines;
        using markdown::styleOf;
        using markdown::trimLeft;

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
    }

    ChatView::ChatView(uint32_t a11yBase, Hooks hooks) : base_(a11yBase), hooks_(std::move(hooks)) {}

    float ChatView::adv(Role r) const { return metrics_.adv(r); }

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
    // Markdown.h lays it out; its shapes and Copy buttons become the block's.
    void ChatView::take(Block& b, markdown::Layout&& l)
    {
        for (markdown::Span& s : l.spans)
            b.spans.push_back(std::move(s));
        for (const markdown::Shape& s : l.shapes)
        {
            ShapeKind k = ShapeKind::inlineCode;
            switch (s.kind)
            {
                case markdown::ShapeKind::inlineCode: k = ShapeKind::inlineCode; break;
                case markdown::ShapeKind::codeCard: k = ShapeKind::codeCard; break;
                case markdown::ShapeKind::bullet: k = ShapeKind::bullet; break;
                case markdown::ShapeKind::quoteBar: k = ShapeKind::quoteBar; break;
                case markdown::ShapeKind::rule: k = ShapeKind::rule; break;
            }
            b.shapes.push_back({ s.r, k });
        }
        for (markdown::CodeLine& cl : l.code)
            b.code.push_back(std::move(cl));
        for (markdown::Copy& cp : l.copies)
            b.actions.push_back({ cp.r, ActionKind::copyCode, std::move(cp.payload) });
    }

    float ChatView::flow(Block& b, const std::string& paragraph, Role base, float x0, float w, float y, float lineH,
                         bool markdown) const
    {
        markdown::Layout l;
        const float end = markdown::flow(l, metrics_, paragraph, base, x0, w, y, lineH, markdown);
        take(b, std::move(l));
        return end;
    }

    void ChatView::layoutMarkdown(Block& b, const std::string& text, const std::string& raw, float w, float& y) const
    {
        markdown::Layout l;
        markdown::layout(l, metrics_, text, raw, w, y);
        take(b, std::move(l));
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
                    // The user's own words as typed: no markdown read in them.
                    ty = flow(tmp, line, Role::body, 0.0f, maxInner, ty, chat::kBodyLine, false);
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
                layoutMarkdown(b, t, e.text, w, y);
                if (copyAfter)
                {
                    y += 2.0f;
                    b.actions.push_back({ { 0.0f, y, 58.0f, kCopyRow - 2.0f }, ActionKind::copyMessage, e.text });
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
            case ChatEntry::Kind::plan:
            {
                // The planning model's plan (or its answer to ask_planner): a row that folds it, and its text in light
                // markdown past a bar, open until folded.
                b.from = asciiText(e.from);
                b.answerPlan = e.answer;
                b.label = e.answer ? "The planner's answer" : "The plan";
                b.value = (b.from.empty() ? std::string() : b.from + ": ") + t;
                b.actions.push_back({ { 0.0f, 0.0f, w, kThinkRow }, ActionKind::togglePlan, {} });
                y = kThinkRow;
                if (!planClosed_[b.entry] && !t.empty())
                {
                    y += 2.0f;
                    const float top = y;
                    Block body;
                    float by = 0.0f;
                    constexpr float kIndent = 14.0f;
                    layoutMarkdown(body, t, e.text, w - kIndent, by);
                    for (Span s : body.spans)
                    {
                        s.x += kIndent;
                        s.y += top;
                        b.spans.push_back(std::move(s));
                    }
                    for (Shape s : body.shapes)
                    {
                        s.r.x += kIndent;
                        s.r.y += top;
                        b.shapes.push_back(s);
                    }
                    for (CodeLine l : body.code)
                    {
                        l.x += kIndent;
                        l.y += top;
                        b.code.push_back(std::move(l));
                    }
                    for (Action a : body.actions)
                    {
                        a.r.x += kIndent;
                        a.r.y += top;
                        b.actions.push_back(std::move(a));
                    }
                    y = top + by;
                    b.shapes.push_back({ { 4.0f, top + 1.0f, 2.0f, std::max(0.0f, y - top - 2.0f) }, ShapeKind::quoteBar });
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
                    const ChatEntry& row = entries[static_cast<std::size_t>(b.entry + k)];
                    std::string line = capitalised(asciiText(row.text));
                    b.value += (b.value.empty() ? "" : "; ") + line;
                    if (line.size() > cols)
                        line = line.substr(0, cols - 3) + "...";
                    const bool bad = row.failed;          // the tool's result was an error (ChatEntry::failed)
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
                b.chosen.clear();
                for (std::size_t qi = 0; qi < b.questions.size(); ++qi)
                    b.chosen.push_back(chosenOf(b.questions[qi], qi < b.answers.size() ? b.answers[qi] : std::string()));
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
            const float bw = 172.0f;
            empty_.addKey = { std::floor((w - bw) * 0.5f), y, bw, 32.0f };
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
        ++shows_;
        const float w = contentWidth();
        const bool wasAtEnd = follow_ || atEnd();
        ready_ = state.ready();
        busy_ = state.busy;
        activity_ = asciiText(state.activity);
        if (state.entries.size() < entryCount_ || state.conversation != conversation_)
        {
            open_.clear();                           // another conversation: what was opened, folded or picked in the
            planClosed_.clear();                     // last one (by entry) is not this one's
            picks_.clear();
        }
        entryCount_ = state.entries.size();
        conversation_ = state.conversation;
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
                            + (desc.kind == ChatEntry::Kind::thinking && open_[desc.entry] ? '1' : '0')
                            + (desc.kind == ChatEntry::Kind::plan && planClosed_[desc.entry] ? 'c' : 'o') + '|';
            for (int k = 0; k < desc.count; ++k)
                key += state.entries[static_cast<std::size_t>(desc.entry + k)].text
                     + (state.entries[static_cast<std::size_t>(desc.entry + k)].failed ? "\x1fx" : "\x1f");
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
                b.laidOut = ++layouts_;
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
                 && (last->kind == ChatEntry::Kind::thinking || last->kind == ChatEntry::Kind::assistant
                     || last->kind == ChatEntry::Kind::plan);
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
        // What was laid out (drawKey): each block's layout and place, the empty chat's words, the states drawn. The
        // Panel shows the conversation again once a second whether or not it changed: that draws nothing.
        DrawKey k;
        for (const Block& b : blocks_)
            k.mix(b.laidOut).mix(b.y).mix(b.h).mix(b.open).mix(b.answers.size());
        k.mix(emptyText_.title).mix(emptyText_.body).mix(emptyText_.suggestions.size()).mix(ready_).mix(busy_).mix(liveText_)
            .mix(activity_).mix(contentH_).mix(typingY_);
        for (const std::string& t : emptyText_.suggestions)
            k.mix(t);
        content_ = k.h;
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
        if (a.kind == ActionKind::togglePlan)
        {
            planClosed_[b.entry] = !planClosed_[b.entry];
            b.key.clear();
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

    uint64_t ChatView::drawKey() const
    {
        DrawKey k;
        k.mix(content_).mix(bounds_).mix(scroll_).mix(contentH_).mix(pointer_).mix(bar_.dragging()).mix(open_.size()).mix(planClosed_.size())
            .mix(copiedBlock_).mix(copiedAction_).mix(copiedFor_ > 0.0f);
        for (const auto& [entry, qs] : picks_)
            for (const std::vector<bool>& q : qs)
                for (const bool b : q)
                    k.mix(b);
        return k.h;
    }

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
            if (empty_.addKey.contains(q))
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
        if (Scroll s = scroller(); !e.popup && s.press(p, barArea()))
        {
            bar_ = s;                                    // the thumb drags (a press on its track jumps first)
            scroll_ = s.offset;
            follow_ = atEnd();
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
        // As the code scrolls (Scroll.h), a notch three body lines: the delta as it came, WheelEvent::reversed never read.
        Scroll s = scroller();
        if (!bounds_.contains({ e.x, e.y }) || !s.wheel(e.dy, e.smooth))
            return false;
        scroll_ = s.offset;
        follow_ = atEnd();
        return true;
    }

    Scroll ChatView::scroller() const noexcept
    {
        Scroll s = bar_;
        s.offset = scroll_;
        s.content = contentH_;
        s.view = viewHeight();
        s.row = chat::kBodyLine;
        s.clamp();
        return s;
    }

    void ChatView::pointerDrag(const funkgui::PointerEvent& e, funkgui::GestureController&)
    {
        if (!bar_.dragging())
            return;
        Scroll s = scroller();
        s.drag({ e.x, e.y }, barArea());
        bar_ = s;
        scroll_ = s.offset;
        follow_ = atEnd();
    }

    void ChatView::pointerUp(const funkgui::PointerEvent&, funkgui::GestureController&) { bar_.release(); }

    bool ChatView::key(const funkgui::KeyEvent& e, funkgui::GestureController&)
    {
        // Tabbed to (the message box keeps every key while it has the caret): the arrows a few lines, Page Up and Down
        // a view less two lines, Home the start, End the end (which it then follows).
        if (e.mods.cmd || e.mods.ctrl || e.mods.alt || blocks_.empty())
            return false;
        Scroll s = scroller();
        const float page = std::max(chat::kBodyLine, viewHeight() - 2.0f * chat::kBodyLine);
        using funkgui::Key;
        if (e.key == Key::up || e.key == Key::down)
            s.moveTo(s.offset + (e.key == Key::up ? -2.0f : 2.0f) * chat::kBodyLine);
        else if (e.key == Key::pageUp || e.key == Key::pageDown)
            s.moveTo(s.offset + (e.key == Key::pageUp ? -page : page));
        else if (e.key == Key::home || e.key == Key::end)
            s.moveTo(e.key == Key::home ? 0.0f : s.max());
        else
            return false;
        scroll_ = s.offset;
        follow_ = atEnd();
        return true;
    }

    void ChatView::reveal(int block)
    {
        if (block < 0 || block >= static_cast<int>(blocks_.size()))
            return;
        const Block& b = blocks_[static_cast<std::size_t>(block)];
        Scroll s = scroller();
        s.reveal(b.y - kPadTop, b.y + b.h + kPadBottom);
        scroll_ = s.offset;
        follow_ = atEnd();
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

    std::optional<Point> ChatView::centreOfCopy(int n) const { return centreOfAction(ActionKind::copyCode, n); }

    int ChatView::tiles() const
    {
        int n = 0;
        for (const Block& b : blocks_)
            n += static_cast<int>(b.tiles.size());
        return n;
    }

    std::string ChatView::shownText() const
    {
        std::string out;
        for (const Block& b : blocks_)
            for (const Span& sp : b.spans)
                out += sp.text + '\n';
        return out;
    }

    float ChatView::overlap() const
    {
        float most = 0.0f;
        for (const Block& b : blocks_)
            for (const Span& sp : b.spans)
            {
                const float end = sp.x + funkgui::text::width(funkgui::FontService::get().atlas(), sp.text.c_str(), styleOf(sp.role));
                for (const Span& next : b.spans)
                    if (next.y == sp.y && next.x > sp.x)
                        most = std::max(most, end - next.x);
            }
        return most;
    }

    int ChatView::failedRows() const
    {
        int n = 0;
        for (const Block& b : blocks_)
            if (b.kind == ChatEntry::Kind::tool)
                for (const IconAt& ic : b.icons)
                    n += ic.icon == chat::Icon::alert ? 1 : 0;
        return n;
    }

    bool ChatView::chosen(int q, int o) const
    {
        for (int i = static_cast<int>(blocks_.size()); --i >= 0;)
            if (blocks_[static_cast<std::size_t>(i)].kind == ChatEntry::Kind::question)
                return picked(blocks_[static_cast<std::size_t>(i)], q, o);
        return false;
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
        return q < static_cast<int>(b.chosen.size()) && o < static_cast<int>(b.chosen[static_cast<std::size_t>(q)].size())
            && b.chosen[static_cast<std::size_t>(q)][static_cast<std::size_t>(o)];
    }

    std::vector<bool> ChatView::chosenOf(const ChatQuestion& q, const std::string& answer)
    {
        // The answer is the labels chosen in the options' order, ", " between them (act()), and a label may hold ", "
        // itself: the options whose labels, so joined, are the answer whole; of several such sets, the fewest. None
        // for an answer that is no such join (one typed in a message).
        const std::size_t n = std::min<std::size_t>(q.options.size(), 16);
        std::vector<bool> out(q.options.size(), false);
        int best = -1;
        for (uint32_t set = 1; set < (1u << n); ++set)
        {
            std::string joined;
            for (std::size_t o = 0; o < n; ++o)
                if ((set >> o) & 1u)
                    joined += (joined.empty() ? "" : ", ") + q.options[o].label;
            const int count = std::popcount(set);
            if (joined == answer && (best < 0 || count < best))
            {
                best = count;
                for (std::size_t o = 0; o < n; ++o)
                    out[o] = ((set >> o) & 1u) != 0;
            }
        }
        return out;
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
    void ChatView::draw(funkgui::Canvas& c, const funkgui::Theme& th, bool focusRing) const
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
            if (!empty_.addKey.isEmpty())
            {
                const Rect r{ x0 + empty_.addKey.x, oy + empty_.addKey.y, empty_.addKey.w, empty_.addKey.h };
                const bool hover = r.contains(pointer_);
                c.rrect(r.x, r.y, r.w, r.h, r.h * 0.5f, hover ? funkgui::mix(p.accent, p.text, 0.12f) : p.accent);
                chat::drawIcon(c, chat::Icon::key, r.x + 22.0f, r.centreY(), 13.0f, p.onAccent);
                text("Add your API key", r.x + 38.0f, r.y, r.h, Role::smallStrong, p.onAccent);
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
                        markdown::drawShape(c, p, markdown::ShapeKind::inlineCode, r);
                        break;
                    case ShapeKind::codeCard:
                        markdown::drawShape(c, p, markdown::ShapeKind::codeCard, r);
                        break;
                    case ShapeKind::bullet:
                        markdown::drawShape(c, p, markdown::ShapeKind::bullet, r);
                        break;
                    case ShapeKind::quoteBar:
                        markdown::drawShape(c, p, markdown::ShapeKind::quoteBar, r);
                        break;
                    case ShapeKind::rule:
                        markdown::drawShape(c, p, markdown::ShapeKind::rule, r);
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
                const float ca = metrics_.codeAdv();
                for (const CodeLine& cl : b.code)
                {
                    const float ly = top + cl.y;
                    if (ly > bounds_.bottom() || ly + chat::kCodeLine < bounds_.y)
                        continue;
                    markdown::drawCode(c, th, p, codePalette, cl, x0, top, ca);
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
                if (a.kind == ActionKind::togglePlan)
                {
                    const bool open = !(planClosed_.count(b.entry) != 0 && planClosed_.at(b.entry));
                    const funkgui::Col ink = hover ? p.text : p.soft;
                    chat::drawIcon(c, open ? chat::Icon::chevronDown : chat::Icon::chevronRight, r.x + 6.0f, r.centreY(), 9.0f, ink);
                    chat::drawIcon(c, chat::Icon::list, r.x + 22.0f, r.centreY(), 10.0f, p.accent);
                    const std::string title = b.answerPlan ? "The planner answers" : (live(b) ? "Planning" : "Plan");
                    text(title, r.x + 34.0f, r.y, r.h, Role::smallStrong, ink);
                    const float tx = r.x + 34.0f + static_cast<float>(title.size()) * adv(Role::smallStrong) + 8.0f;
                    if (live(b))
                        for (int k = 0; k < 3; ++k)
                        {
                            const float t = std::fmod(phase_ / 1.2f + 1.0f - static_cast<float>(k) * 0.18f, 1.0f);
                            const float amp = 0.35f + 0.65f * std::max(0.0f, std::sin(t * 3.14159265f));
                            c.disc(tx + static_cast<float>(k) * 7.0f, r.centreY() + 1.0f, 1.6f, funkgui::mix(p.card, ink, amp));
                        }
                    else if (!b.from.empty())
                        text(b.from, tx, r.y, r.h, Role::faint, p.faint);
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

        // The scroll bar, and the way back to the end; the focus ring while tabbed to.
        scroller().draw(c, barArea(), th);
        if (focusRing)
            c.rrect(bounds_.x + 2.0f, bounds_.y + 2.0f, bounds_.w - 4.0f, bounds_.h - 4.0f, 8.0f, p.card.withAlpha(0.0f), 1.5f,
                    p.accent);
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
            if (!empty_.addKey.isEmpty())
            {
                funkgui::A11yItem key;
                key.id = base_ + 2;
                key.parent = base_;
                key.role = funkgui::A11yRole::button;
                key.bounds = { o.x + empty_.addKey.x, o.y + empty_.addKey.y, empty_.addKey.w, empty_.addKey.h };
                key.title = "Add your API key";
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
            // Every block, in view or not, so a screen reader reaches each: one out of view a line at the view's edge
            // on its side (inside the panel; its focus scrolls it in: a11yAction), its actions only while in view.
            funkgui::A11yItem item;
            item.id = base_ + 16 + static_cast<uint32_t>(bi) * kA11yStride;
            item.parent = base_;
            item.role = funkgui::A11yRole::listItem;
            item.bounds = y1 > y0                   ? Rect{ whole.x, y0, whole.w, y1 - y0 }
                        : whole.bottom() <= bounds_.y ? Rect{ whole.x, bounds_.y, whole.w, 1.0f }
                                                      : Rect{ whole.x, bounds_.bottom() - 1.0f, whole.w, 1.0f };
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
                act.role = a.kind == ActionKind::toggleThinking || a.kind == ActionKind::togglePlan ? funkgui::A11yRole::toggleButton
                                                                                                   : funkgui::A11yRole::button;
                act.bounds = r;
                if (a.kind == ActionKind::togglePlan)
                {
                    act.title = b.answerPlan ? "Show the planner's answer" : "Show the plan";
                    act.description = b.from;
                    act.checkable = true;
                    act.checked = !(planClosed_.count(b.entry) != 0 && planClosed_.at(b.entry));
                }
                else if (a.kind == ActionKind::toggleThinking)
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

    bool ChatView::a11yAction(uint32_t id, funkgui::A11yAction a, double, funkgui::GestureController&)
    {
        if (id < base_ || id >= base_ + (1u << 24))
            return false;
        const uint32_t k = id - base_;
        if (blocks_.empty())
        {
            if (k == 2 && hooks_.addKey)
                hooks_.addKey();
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
        else if (k >= 16 && (a == funkgui::A11yAction::focus || a == funkgui::A11yAction::press))
            reveal(static_cast<int>((k - 16) / kA11yStride));   // a block: scrolled into view
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
                                        : k == ActionKind::togglePlan   ? "PLAN   SHOW OR HIDE THE PLANNING MODEL'S PLAN"
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
