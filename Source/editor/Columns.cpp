// Source/editor/Columns.cpp: the columns' widths and the edges that set them (Columns.h).
#include "editor/Columns.h"

#include "editor/ChatStyle.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace playground::ui::columns
{
    int keptWidth(float px, bool chat)
    {
        if (!(px >= 1.0f))
            return 0;                                    // none (and NaN)
        const float lo = chat ? kChatMinW : kExplorerMinW, hi = chat ? kChatMaxW : kExplorerMaxW;
        return static_cast<int>(std::clamp(std::round(px), lo, hi));
    }

    Fit fit(const Asked& a)
    {
        Fit f;
        const float W = a.window;
        const float chatWanted = a.chatWant >= 1.0f ? std::clamp(std::round(a.chatWant), kChatMinW, kChatMaxW)
                                                    : std::clamp(std::round(W * kChatShare), kChatMinW, kChatAutoMaxW);
        const float explorerWanted = a.explorerWant >= 1.0f
                                         ? std::clamp(std::round(a.explorerWant), kExplorerMinW, kExplorerMaxW)
                                         : std::clamp(std::round(W * kExplorerShare), kExplorerMinW, kExplorerAutoMaxW);
        // What the chat and the explorer share: the window less its margins, the chat's gap and the program's least.
        const float room = std::floor(W - 2.0f * kMargin - (a.chat ? kChatGap : kChatRailGap) - kProgramMinW);
        float chat = a.chat ? std::max(kChatMinW, std::min(chatWanted, room)) : kChatRailW;
        float explorer = 0.0f;
        if (a.explorer)
        {
            // Beside each other, with the explorer's gap: the explorer gives way first, then the chat, each down to its
            // least; when even that is too much, the explorer folds away and the chat has the room alone.
            const float shared = room - kExplorerGap;
            float e = explorerWanted, c = a.chat ? chatWanted : kChatRailW;
            if (e + c > shared)
                e = std::max(kExplorerMinW, shared - c);
            if (a.chat && e + c > shared)
                c = std::max(kChatMinW, shared - e);
            if (e + c <= shared)
            {
                explorer = e;
                chat = c;
            }
        }
        f.explorer = explorer;
        f.chat = chat;
        // The edges' reach: the explorer's stops where the chat's width begins (the chat keeps its width first); the
        // chat's takes the explorer down to its least, never further.
        if (explorer > 0.0f)
            f.explorerMost = std::clamp(room - kExplorerGap - chat, kExplorerMinW, kExplorerMaxW);
        if (a.chat)
            f.chatMost = std::clamp(explorer > 0.0f ? room - kExplorerGap - kExplorerMinW : room, kChatMinW, kChatMaxW);
        return f;
    }

    // ---- an edge ----------------------------------------------------------------------------------------------------
    namespace
    {
        constexpr float kBandIn = 4.0f, kBandOut = 8.0f;   // the band: this far into the card, this far over the gap
        constexpr float kCorner = 14.0f;                 // the cards' corners: the hairline runs between them
        constexpr float kGripH = 32.0f;
    }

    EdgeItem::EdgeItem(uint32_t id, int grows, std::string title, std::string spec, Hooks hooks)
        : id_(id), grows_(grows >= 0 ? 1 : -1), title_(std::move(title)), spec_(std::move(spec)), hooks_(std::move(hooks))
    {
    }

    void EdgeItem::place(float edgeX, float top, float bottom, bool shown)
    {
        edgeX_ = edgeX;
        top_ = top;
        bottom_ = bottom;
        shown_ = shown;
        if (!shown)
        {
            band_ = {};
            dragging_ = false;
            return;
        }
        // Over the gap beside the edge (the explorer's gap at its right, the chat's at its left), a little into the card.
        const float x = grows_ > 0 ? edgeX - kBandIn : edgeX - kBandOut;
        band_ = { x, top, kBandIn + kBandOut, std::max(0.0f, bottom - top) };
    }

    void EdgeItem::draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const
    {
        if (!shown_)
            return;
        const bool focus = ring && focused_;
        if (!hovered_ && !dragging_ && !focus)
            return;                                      // at rest the card's own edge is the hairline
        const chat::Palette p = chat::palette(th);
        const funkgui::Col ink = dragging_ || focus ? p.accent : funkgui::mix(p.cardEdge, p.accent, 0.6f);
        const float y0 = top_ + kCorner, y1 = bottom_ - kCorner;
        if (y1 > y0)
            c.rrect(edgeX_ - 1.0f, y0, 2.0f, y1 - y0, 1.0f, ink);
        const float mid = 0.5f * (top_ + bottom_);
        c.rrect(edgeX_ - 2.0f, mid - 0.5f * kGripH, 4.0f, kGripH, 2.0f, ink);
        if (focus)
            c.rrect(edgeX_ - 6.0f, mid - 0.5f * kGripH - 4.0f, 12.0f, kGripH + 8.0f, 6.0f, p.card.withAlpha(0.0f), 1.5f,
                    p.accent);
    }

    void EdgeItem::pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController&, funkgui::HostServices&)
    {
        if (e.popup || !shown_)
            return;
        dragging_ = true;
        moved_ = false;
        startX_ = e.x;
        startW_ = hooks_.width();
        mostAtStart_ = std::max(hooks_.most(), startW_);   // the reach as the drag began: a column that comes or
                                                           // goes meanwhile changes no limit within the gesture
    }

    void EdgeItem::pointerDrag(const funkgui::PointerEvent& e, funkgui::GestureController&)
    {
        if (!dragging_)
            return;
        const float w = std::clamp(std::round(startW_ + static_cast<float>(grows_) * (e.x - startX_)), hooks_.least(),
                                   mostAtStart_);
        if (w != hooks_.width())
        {
            moved_ = true;
            hooks_.set(w, Keep::notYet);
        }
    }

    void EdgeItem::pointerUp(const funkgui::PointerEvent&, funkgui::GestureController&)
    {
        if (dragging_ && moved_)
            hooks_.set(hooks_.width(), Keep::now);       // the session and this Mac's default keep it
        dragging_ = false;
        moved_ = false;
    }

    void EdgeItem::doubleClick(const funkgui::PointerEvent&, funkgui::GestureController&)
    {
        if (!shown_)
            return;
        dragging_ = false;
        moved_ = false;
        hooks_.reset();
    }

    void EdgeItem::stepTo(float px)
    {
        if (!shown_)
            return;                                      // its card folded or away: nothing to set
        const float least = hooks_.least();
        const float w = std::clamp(std::round(px), least, std::max(least, hooks_.most()));
        // As a drag: only a width that moves the edge. A step against the reach (a column that gave way to the window
        // shows less than its user asked) keeps what was asked, so the column comes back to it as the window widens.
        if (w != hooks_.width())
            hooks_.set(w, Keep::step);
    }

    bool EdgeItem::key(const funkgui::KeyEvent& e, funkgui::GestureController&)
    {
        if (!shown_)
            return false;                                // hidden while focused: no key is the edge's
        if (e.key == funkgui::Key::left || e.key == funkgui::Key::right)
        {
            // The arrow moves the edge its way: the explorer's right edge to the right widens it, the chat's left edge to
            // the left widens the chat.
            const int way = e.key == funkgui::Key::right ? 1 : -1;
            stepTo(hooks_.width() + static_cast<float>(way * grows_) * (e.mods.shift ? kBigStep : kStep));
            return true;
        }
        if (e.key == funkgui::Key::home || e.key == funkgui::Key::end)
        {
            stepTo(e.key == funkgui::Key::home ? hooks_.least() : hooks_.most());
            return true;
        }
        if (e.key == funkgui::Key::del || e.key == funkgui::Key::backspace)
        {
            hooks_.reset();
            return true;
        }
        return false;
    }

    void EdgeItem::accessibility(std::vector<funkgui::A11yItem>& out) const
    {
        funkgui::A11yItem item;
        item.id = id_;
        item.role = funkgui::A11yRole::slider;
        item.bounds = band_;
        item.title = title_;
        const float w = hooks_.width();
        item.value = std::to_string(static_cast<int>(std::lround(w))) + " px";
        item.v = w;
        item.lo = hooks_.least();
        item.hi = std::max(hooks_.least(), hooks_.most());
        item.step = kStep;
        item.help = "Drag the edge, or the left and right arrows; a double-click or Delete resets it";
        out.push_back(std::move(item));
    }

    bool EdgeItem::a11yAction(uint32_t id, funkgui::A11yAction a, double value, funkgui::GestureController&)
    {
        if (id != id_)
            return false;
        switch (a)
        {
            case funkgui::A11yAction::increment: stepTo(hooks_.width() + kStep); break;
            case funkgui::A11yAction::decrement: stepTo(hooks_.width() - kStep); break;
            case funkgui::A11yAction::setValue: stepTo(static_cast<float>(value)); break;
            case funkgui::A11yAction::press:
            case funkgui::A11yAction::toggle:
            case funkgui::A11yAction::showMenu:
            case funkgui::A11yAction::focus: break;
        }
        return true;
    }

    void EdgeItem::specLine(char* out, std::size_t n) const
    {
        std::snprintf(out, n, "%s   %d PX   DRAG THE EDGE, OR THE ARROWS   A DOUBLE-CLICK OR DELETE RESETS IT", spec_.c_str(),
                      static_cast<int>(std::lround(hooks_.width())));
    }
}
