// Source/editor/ChatView.h: the chat's conversation (the chat column, Panel.h), drawn the way a chat app draws one
// (ChatStyle.h has the look):
//   the user's messages   right-aligned, in rounded bubbles; the pictures it had above it, as thumbnails
//   Claude's answers      full width, in light markdown: paragraphs, - and 1. lists, # headings, **bold**, `code`,
//                         > quotes, --- rules, and ``` code blocks (rounded cards, highlighted as FMinus, each with a
//                         Copy); under the last answer of a turn a Copy for the whole answer, shown under the pointer
//   Claude's thinking     a row that opens and closes ("Thinking" while it streams), its summary dim beside a rule
//   what Claude did       a rounded card, a row and an icon an action (a file written, a run, a dial set, a sound
//                         measured); one whose result was an error (ChatEntry::failed) in the danger colour
//   errors and notes      an error in a tinted card; the chat's own notes small and centred
//   Claude's questions    a card (ask_user): each question, its options as buttons (a ring for one choice, a box for
//                         several); one question of one choice is answered by its click, more by Send answers; once
//                         answered, the choices stay marked
//   while Claude works    three pulsing dots and what it is doing ("Writing osc.fm (12 KB)")
//   an empty chat         a title, a line, a button for the API key while none is stored, and suggestions to start
//                         from (a click puts one in the message box: nothing is sent until the user sends it)
// It scrolls with the wheel (a thin bar while it overflows) and, tabbed to, with the arrows, Page Up and Down, Home and
// End; it follows the end while it is at the end, and a round button jumps back to the end. A screen reader has every
// block, and its focus on one out of view scrolls it in. Each block keeps its layout until its text, its width or its
// state changes, so a long conversation costs only its streaming end. Text is drawn as printable ASCII (asciiText,
// ChatItems.h); a Copy copies the entry's own text. The user's words are drawn as typed (no markdown).
#pragma once

#include "editor/ChatStyle.h"
#include "editor/Items.h"
#include "editor/Scroll.h"

#include "plugin/ChatPort.h"

#include <funkgui/text/Highlighter.h>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace playground::ui
{
    class ChatView final : public Item
    {
    public:
        struct Hooks
        {
            std::function<void(const std::string&)> suggest;   // a suggestion: its text into the message box
            std::function<void()> addKey;                      // the empty chat's API key button
            std::function<void(const std::vector<std::string>&)> answer;   // Claude's question answered (ask_user)
            std::function<void()> clearChecks;                 // the checks card's Clear (set_checks)
        };
        struct Empty
        {
            std::string title, body;
            std::vector<std::string> suggestions;
        };

        // a11yBase: the first of the view's accessibility ids (it uses up to 2^24 of them).
        ChatView(uint32_t a11yBase, Hooks hooks);

        void setBounds(funkgui::Rect r);
        funkgui::Rect bounds() const noexcept { return bounds_; }
        void attach(funkgui::HostServices* host) { host_ = host; }
        void show(const ChatState& state, const Empty& empty);
        const std::string& text() const noexcept { return text_; }   // the conversation as plain text (probes)

        // For probes.
        int blocks() const noexcept { return static_cast<int>(blocks_.size()); }
        int blockLayouts() const noexcept { return static_cast<int>(layouts_); }   // blocks laid out so far
        int shows() const noexcept { return shows_; }                                 // show()'s calls so far
        int tiles() const;                                            // the pictures shown in the conversation
        std::optional<funkgui::Point> centreOfSuggestion(int i) const;
        std::optional<funkgui::Point> centreOfAddKey() const;
        std::optional<funkgui::Point> centreOfCopy(int n) const;      // the n-th code block's Copy, while it is in view
        std::optional<funkgui::Point> centreOfAnswerCopy(int n) const;   // the n-th answer's Copy, while in view
        std::optional<funkgui::Point> centreOfThinking(int n) const;  // the n-th thinking row, while it is in view
        std::optional<funkgui::Point> centreOfJump() const;           // while the view is not at the end
        // The open question's option o of question q, and its Send answers (while in view).
        std::optional<funkgui::Point> centreOfOption(int q, int o) const;
        std::optional<funkgui::Point> centreOfSubmit() const;
        std::optional<funkgui::Point> centreOfClearChecks() const;   // the live checks card's Clear, while in view
        int checkRows() const;                                        // the rows of the checks cards shown
        bool optionPicked(int q, int o) const;                        // marked in the open question
        bool thinkingOpen(int n) const;
        int copies() const;                                           // the code blocks' Copy actions
        bool atEnd() const noexcept;
        void scrollTo(float y);
        float scroll() const noexcept { return scroll_; }
        std::string shownText() const;                                // every span drawn, a line each
        float overlap() const;          // the most a span's text, as drawn, reaches past the start of the next on its line
        int failedRows() const;                                       // tool rows drawn as failed
        bool chosen(int q, int o) const;                              // marked in the last question, open or answered
        float contentHeight() const noexcept { return contentH_; }

        // Item
        void tick(float dt, bool hovered, bool focused, funkgui::Point pointer) override;
        void draw(funkgui::Canvas&, const funkgui::Theme&, bool focusRing) const override;
        uint64_t drawKey() const override;               // what show() laid out, the scroll and the hover
        bool contains(funkgui::Point p) const override { return bounds_.contains(p); }
        funkgui::Cursor cursorAt(funkgui::Point p) const override;
        void pointerExit() override { pointer_ = { -1.0e4f, -1.0e4f }; }
        void pointerDown(const funkgui::PointerEvent&, funkgui::GestureController&, funkgui::HostServices&) override;
        void pointerDrag(const funkgui::PointerEvent&, funkgui::GestureController&) override;   // the thumb's
        void pointerUp(const funkgui::PointerEvent&, funkgui::GestureController&) override;
        bool wheel(const funkgui::WheelEvent&, funkgui::GestureController&, double) override;
        // Its scroll as Scroll.h has it (the offset, the content, the view; a notch three body lines), and where its
        // bar is.
        Scroll scroller() const noexcept;
        funkgui::Rect barArea() const noexcept { return { bounds_.x, bounds_.y + 4.0f, bounds_.w, bounds_.h - 8.0f }; }
        void accessibility(std::vector<funkgui::A11yItem>&) const override;
        bool a11yAction(uint32_t id, funkgui::A11yAction, double, funkgui::GestureController&) override;
        void specLine(char* out, std::size_t n) const override;
        bool settled() const override;
        funkgui::Point centre() const override { return { bounds_.centreX(), bounds_.centreY() }; }
        bool focusable() const override { return !hidden_; }   // a Tab stop: its keys scroll (key())
        bool key(const funkgui::KeyEvent&, funkgui::GestureController&) override;
        bool shown() const override { return !hidden_; }
        void setHidden(bool hidden) { hidden_ = hidden; }   // the chat folded away

        // How a piece of text is set (ChatView.cpp resolves the style and the colour).
        enum Role : uint8_t { body, strong, code, h1, h2, h3, dim, small, smallStrong, faint, danger, quote, accent,
                              smallCode };

    private:
        struct Span
        {
            float x = 0.0f, y = 0.0f, lineH = 0.0f;
            std::string text;
            Role role = body;
        };
        enum class ShapeKind : uint8_t { bubble, inlineCode, codeCard, bullet, quoteBar, rule, toolCard, errorCard,
                                         questionCard, option };
        struct Shape
        {
            funkgui::Rect r;
            ShapeKind kind = ShapeKind::bubble;
            int q = -1, o = -1;                       // an option's question and its place
        };
        struct CodeLine
        {
            float x = 0.0f, y = 0.0f;
            std::string text;
            std::vector<funkgui::text::Span> spans;
        };
        enum class ActionKind : uint8_t { copyMessage, copyCode, toggleThinking, option, submit, clearChecks, togglePlan };
        struct Action
        {
            funkgui::Rect r;
            ActionKind kind = ActionKind::copyMessage;
            std::string payload;                      // what Copy copies
            int q = -1, o = -1;                       // an option's question and its place
        };
        struct Tile
        {
            funkgui::Rect r;
            uint32_t id = 0;                          // the picture's (ChatPicture::id), drawn from FunkGui's registry
            funkgui::Rect uv{ 0.0f, 0.0f, 1.0f, 1.0f };
        };
        struct IconAt
        {
            float x = 0.0f, y = 0.0f;
            chat::Icon icon = chat::Icon::none;
            Role role = dim;
        };
        struct Block
        {
            ChatEntry::Kind kind = ChatEntry::Kind::note;
            int entry = 0;                            // its first entry
            std::string key;                          // what its layout was made from
            uint32_t laidOut = 0;                     // which layout of the view's it is (drawKey: it changed)
            float y = 0.0f, h = 0.0f;                 // in the content
            std::vector<Span> spans;
            std::vector<Shape> shapes;
            std::vector<CodeLine> code;
            std::vector<Action> actions;
            std::vector<IconAt> icons;
            std::vector<Tile> tiles;
            std::string label;                        // the accessibility title
            std::string value;                        // the accessibility value (its text)
            // A question's (ask_user).
            std::vector<ChatQuestion> questions;
            std::vector<std::string> answers;
            std::vector<std::vector<bool>> chosen;    // an answered one's options chosen (chosenOf)
            bool open = false;
            // The checks' (set_checks).
            std::vector<ChatCheck> checks;
            // A plan's (the planning model's, ChatAgent.h).
            std::string from;
            bool answerPlan = false;
        };
        struct EmptyLayout
        {
            std::vector<Span> spans;
            funkgui::Rect addKey;                     // empty when there is no key button
            std::vector<funkgui::Rect> chips;
            std::vector<std::string> chipText;
            float h = 0.0f;
        };

        float contentWidth() const noexcept;
        float viewHeight() const noexcept;
        float maxScroll() const noexcept;
        float adv(Role r) const;
        void layoutBlock(Block& b, const std::vector<ChatEntry>& entries, int count, bool copyAfter, float w, bool live);
        // text: as drawn (asciiText); raw: the entry's own, line for line (what Copy copies).
        void layoutMarkdown(Block& b, const std::string& text, const std::string& raw, float w, float& y) const;
        // markdown false: the paragraph as plain text (the user's own words).
        float flow(Block& b, const std::string& paragraph, Role base, float x0, float w, float y, float lineH,
                   bool markdown = true) const;
        void layoutCode(Block& b, const std::string& lang, const std::vector<std::string>& lines, const std::string& copied,
                        float w, float& y) const;
        void reveal(int block);                            // scrolled to show the block whole (as far as it fits)
        static std::vector<bool> chosenOf(const ChatQuestion& q, const std::string& answer);
        void layoutEmpty(float w);
        funkgui::Rect onScreen(const Block& b, const funkgui::Rect& r) const;   // content -> panel px
        bool live(const Block& b) const;          // the block still streaming
        void hit(funkgui::Point p, int& block, int& action) const;
        void act(int block, int action);
        bool picked(const Block& b, int q, int o) const;   // an option marked: chosen, or picked in an open question
        bool complete(const Block& b) const;               // every question of the open one has a pick
        static bool needsSubmit(const Block& b);           // more than one question, or one of several choices
        int openQuestion() const;                          // the open question's block, -1 for none
        std::vector<std::pair<int, int>> actionsOf(ActionKind kind) const;
        std::optional<funkgui::Point> centreOfAction(ActionKind kind, int n) const;

        uint32_t base_;
        Hooks hooks_;
        funkgui::HostServices* host_ = nullptr;
        funkgui::Rect bounds_;
        std::vector<Block> blocks_;
        std::map<int, bool> open_;                    // thinking blocks opened, by their entry
        std::map<int, bool> planClosed_;              // plans folded, by their entry (a plan shows open)
        std::map<int, std::vector<std::vector<bool>>> picks_;   // an open question's picks, by its entry
        EmptyLayout empty_;
        Empty emptyText_;
        bool ready_ = false;                          // the chat can send (no key button)
        bool busy_ = false;
        bool liveText_ = false;                       // the last block streams (thinking or an answer)
        std::string activity_;
        std::string text_;
        size_t entryCount_ = 0;
        uint64_t conversation_ = 0;                   // ChatState::conversation shown
        float contentH_ = 0.0f;
        float scroll_ = 0.0f;
        Scroll bar_;                                  // the thumb's drag (its offset: scroll_, scroller())
        bool follow_ = true;                          // the view stays at the end as it grows
        float typingY_ = 0.0f;                        // where the working row is, in the content
        float phase_ = 0.0f;                          // the dots' animation
        float copiedFor_ = 0.0f;                      // the "Copied" confirmation, seconds left
        int copiedBlock_ = -1, copiedAction_ = -1;
        funkgui::Point pointer_{ -1.0e4f, -1.0e4f };
        mutable float advBody_ = 0.0f, advSmall_ = 0.0f, advCode_ = 0.0f, advH1_ = 0.0f, advH2_ = 0.0f;
        bool hidden_ = false;
        uint64_t content_ = 0;                        // what show() laid out, mixed (drawKey)
        uint32_t layouts_ = 0;                        // blocks laid out so far (Block::laidOut)
        int shows_ = 0;                               // show()'s calls (probes)
    };
}
