// Source/editor/Columns.h: the editor's three columns side by side (Panel.cpp): the explorer's card at the left, the
// program's card, the chat's card at the right; how wide each is for a window's width and what the user asked for,
// and the two edges a user drags to ask (the explorer's right edge, the chat's left edge: EdgeItem).
//
//   The chat     about a third of the window (kChatShare, kChatMinW to kChatAutoMaxW) until the user sets its width;
//                then that width, kChatMinW to kChatMaxW. Folded: a rail of kChatRailW.
//   The explorer about a sixth (kExplorerShare, kExplorerMinW to kExplorerAutoMaxW) until the user sets its width; then
//                that width, kExplorerMinW to kExplorerMaxW. Folded, in the PANEL view: nothing.
//   The program  what is left, never less than kProgramMinW. When the window is too narrow for what the two ask, the
//                explorer gives way first, then the chat, each down to its least; past that the explorer folds away
//                (still open in the session), as a window too narrow for it always did.
//
// A width the user sets (a drag of the edge, its keys, a screen reader's slider) is kept in the session
// (EditorPort::explorerWidth, chatWidth) and as this Mac's default for an instance that has set none
// (UiPreferences: kExplorerWidthKey, kChatWidthKey); 0 is none, the share of the window. A double-click on the edge,
// or Delete, resets it to 0 in both. A drag never pushes the other column: the chat keeps its width before the
// explorer does, so the explorer's edge stops where the chat's width begins, while the chat's pushes the explorer down
// to its least. Only a width that changes what shows is set: a key or a screen reader's step that moves nothing (a
// column that gave way to the window, already at its reach) leaves the width asked as it was, so the column comes back
// to it when the window widens. A hidden edge (its card folded or away) sets nothing.
//
// Message thread only (the item); fit() is plain arithmetic.
#pragma once

#include "editor/Items.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace playground::ui::columns
{
    // logical px
    inline constexpr float kMargin = 20.0f;              // the editor's margin, at the left and the right
    inline constexpr float kChatShare = 0.32f;           // the chat, of the window's width, until the user sets it
    inline constexpr float kChatMinW = 330.0f;           // its least
    inline constexpr float kChatAutoMaxW = 560.0f;       // the share's most
    inline constexpr float kChatMaxW = 760.0f;           // a width the user sets, at most
    inline constexpr float kChatGap = 12.0f;             // between the program's card and the chat's
    inline constexpr float kChatRailW = 44.0f;           // the chat folded: its rail's width
    inline constexpr float kChatRailGap = 12.0f;         // and the gap before it
    inline constexpr float kExplorerShare = 0.16f;       // the explorer, of the window's width, until the user sets it
    inline constexpr float kExplorerMinW = 180.0f;       // its least
    inline constexpr float kExplorerAutoMaxW = 250.0f;   // the share's most
    inline constexpr float kExplorerMaxW = 440.0f;       // a width the user sets, at most
    inline constexpr float kExplorerGap = 12.0f;         // between it and the program's card
    inline constexpr float kProgramMinW = 600.0f;        // the program's card keeps this much, whatever the others ask
    inline constexpr float kStep = 8.0f, kBigStep = 32.0f;   // an arrow key's step, with Shift

    // This Mac's defaults for an instance that has set no width (FunkGui's UiPreferences, int keys, px; 0: none).
    inline constexpr const char* kExplorerWidthKey = "explorerWidth";
    inline constexpr const char* kChatWidthKey = "chatWidth";
    // A step's width reaches this Mac's default once the steps pause this long (s): UiPreferences writes its file at
    // every change, and a held arrow repeats about thirty times a second.
    inline constexpr double kDefaultPause = 0.5;

    // How a new width is kept: not yet (a drag goes on); now (a drag's release, a reset: the session and this Mac's
    // default at once); a step (a key's or a screen reader's: the session at once, this Mac's default after
    // kDefaultPause without another).
    enum class Keep
    {
        notYet,
        now,
        step
    };

    struct Asked
    {
        float window = 0.0f;                             // the editor's width
        bool explorer = true;                            // the explorer open, in the CODE view
        bool chat = true;                                // the chat open (else its rail)
        bool noChat = false;                             // no chat at all, nor its rail (the code window's)
        float explorerWant = 0.0f, chatWant = 0.0f;      // the widths the user set; 0: none (the share)
    };

    struct Fit
    {
        float explorer = 0.0f;                           // the explorer's card; 0: away (folded, no room, PANEL)
        float chat = 0.0f;                               // the chat's card, or its rail (0: none, noChat)
        // The reach of each edge now, for a drag, the keys and a screen reader: the least and the most it can be made
        // (the most never takes the program's card under kProgramMinW, nor the chat's width from the explorer's edge).
        float explorerLeast = kExplorerMinW, explorerMost = kExplorerMinW;
        float chatLeast = kChatMinW, chatMost = kChatMinW;
    };

    // A width the user set, as it is kept: whole px within its column's range, 0 for none (and for anything under 1).
    int keptWidth(float px, bool chat);

    Fit fit(const Asked& asked);

    // One column's edge, a handle the user drags (the explorer's right edge, the chat's left): a band over the gap
    // beside the card's edge, its cursor the left-right one; a hairline over the edge brightens on hover and while it
    // is dragged or focused. A double-click resets the width; the arrows step it (Shift: kBigStep), Home and End take it
    // to its least and its most, Delete and Backspace reset it. To a screen reader it is a slider of px.
    class EdgeItem final : public Item
    {
    public:
        struct Hooks
        {
            std::function<float()> width;                // the card's width now
            std::function<float()> least, most;          // its reach now (Fit's)
            std::function<void(float px, Keep keep)> set;   // a new width, kept as `keep` says
            std::function<void()> reset;                 // the width the share gives (none kept)
        };

        // `grows`: +1 when a move to the right widens the card (the explorer's right edge), -1 when one to the left does
        // (the chat's left edge).
        EdgeItem(uint32_t id, int grows, std::string title, std::string spec, Hooks hooks);

        // The edge's x (the card's border), its top and bottom; hidden when the card is away or folded.
        void place(float edgeX, float top, float bottom, bool shown);
        funkgui::Rect band() const noexcept { return band_; }
        bool dragging() const noexcept { return dragging_; }

        void tick(float, bool hovered, bool focused, funkgui::Point) override
        {
            hovered_ = hovered;
            focused_ = focused;
        }
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override;
        bool contains(funkgui::Point p) const override { return shown_ && band_.contains(p); }
        funkgui::Cursor cursorAt(funkgui::Point) const override { return funkgui::Cursor::leftRight; }
        void pointerDown(const funkgui::PointerEvent&, funkgui::GestureController&, funkgui::HostServices&) override;
        void pointerDrag(const funkgui::PointerEvent&, funkgui::GestureController&) override;
        void pointerUp(const funkgui::PointerEvent&, funkgui::GestureController&) override;
        void doubleClick(const funkgui::PointerEvent&, funkgui::GestureController&) override;
        bool key(const funkgui::KeyEvent&, funkgui::GestureController&) override;
        void accessibility(std::vector<funkgui::A11yItem>& out) const override;
        bool a11yAction(uint32_t id, funkgui::A11yAction, double value, funkgui::GestureController&) override;
        void specLine(char* out, std::size_t n) const override;
        funkgui::Point centre() const override { return { edgeX_, 0.5f * (top_ + bottom_) }; }
        bool shown() const override { return shown_; }

    private:
        void stepTo(float px);                           // a key's or a screen reader's width (Keep::step), when it
                                                         // changes what shows

        uint32_t id_;
        int grows_;
        std::string title_, spec_;
        Hooks hooks_;
        funkgui::Rect band_;
        float edgeX_ = 0.0f, top_ = 0.0f, bottom_ = 0.0f;
        bool shown_ = false, hovered_ = false, focused_ = false;
        bool dragging_ = false, moved_ = false;
        float startX_ = 0.0f, startW_ = 0.0f, mostAtStart_ = 0.0f;
    };
}
