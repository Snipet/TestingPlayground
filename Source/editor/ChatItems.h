// Source/editor/ChatItems.h: the chat's views on the Panel (the chat column, Panel.h): the transcript and the message
// box. The rest of the column (the model, the thinking level, NEW, KEY, SEND or STOP, the key dialog) is the Panel's.
//
//   TranscriptItem  the conversation as text in a read-only funkgui::TextEditor (word wrap; select and copy work), one
//                   block an entry: YOU and CLAUDE above the turns, Claude's reasoning dim ("~"), what it did ("> wrote
//                   filters.fm"), errors ("!"), the chat's notes ("-"), code fences set apart. It follows the bottom
//                   while it is scrolled to the bottom. The editor draws printable ASCII: asciiText() turns the common
//                   typographic characters into ASCII (dashes, quotes, arrows, ...) and the rest into '?'.
//   InputItem       the message box (word wrap). Return sends; Shift-Return breaks the line; Tab moves the focus on.
#pragma once

#include "editor/Items.h"

#include "plugin/ChatPort.h"

#include <funkgui/text/Highlighter.h>
#include <funkgui/widgets/TextEditor.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace testingplaygroundsynth::ui
{
    std::string asciiText(std::string_view utf8);
    std::string transcriptText(const std::vector<ChatEntry>& entries);

    // The transcript's styles: the palette indices of transcriptPalette().
    class TranscriptHighlighter final : public funkgui::text::Highlighter
    {
    public:
        enum Style : uint16_t { plain = 0, speaker = 1, tool = 2, thinking = 3, error = 4, note = 5, code = 6, fence = 7 };
        uint32_t line(std::string_view line, uint32_t stateIn, std::vector<funkgui::text::Span>& out) const override;
    };
    std::vector<funkgui::text::HighlightStyle> transcriptPalette();

    class TranscriptItem final : public Item
    {
    public:
        TranscriptItem(funkgui::Rect bounds, uint32_t id);

        void setBounds(funkgui::Rect bounds);
        // The conversation (when its text changed); `hint` is shown in its place while it is empty.
        void show(const std::vector<ChatEntry>& entries, const std::string& hint);
        funkgui::TextEditor& editor() noexcept { return editor_; }
        const std::string& text() const noexcept { return text_; }
        void attach(funkgui::HostServices* host) { editor_.attach(host); }

        void tick(float dt, bool, bool, funkgui::Point) override { editor_.tick(dt); }
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override { editor_.draw(c, th, ring); }
        bool contains(funkgui::Point p) const override { return editor_.contains(p); }
        funkgui::Cursor cursorAt(funkgui::Point p) const override { return editor_.cursorAt(p); }
        void pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController&, funkgui::HostServices&) override
        {
            editor_.pointerDown(e);
        }
        void pointerDrag(const funkgui::PointerEvent& e, funkgui::GestureController&) override { editor_.pointerDrag(e); }
        void pointerUp(const funkgui::PointerEvent& e, funkgui::GestureController&) override { editor_.pointerUp(e); }
        void doubleClick(const funkgui::PointerEvent& e, funkgui::GestureController&) override { editor_.doubleClick(e); }
        bool wheel(const funkgui::WheelEvent& e, funkgui::GestureController&, double) override { return editor_.wheel(e); }
        bool key(const funkgui::KeyEvent& e, funkgui::GestureController&) override { return editor_.key(e); }
        void accessibility(std::vector<funkgui::A11yItem>& out) const override;
        bool a11yAction(uint32_t id, funkgui::A11yAction, double, funkgui::GestureController&) override
        {
            return id == editor_.a11yId();
        }
        void specLine(char* out, std::size_t n) const override;
        funkgui::Point centre() const override;
        void focusChanged(bool focused) override { editor_.setFocused(focused); }
        bool takesAllKeys() const override { return editor_.focused(); }

    private:
        funkgui::TextEditor editor_;
        std::string text_;
    };

    class InputItem final : public Item
    {
    public:
        InputItem(funkgui::Rect bounds, uint32_t id, std::function<void()> send);

        void setBounds(funkgui::Rect bounds) { editor_.setBounds(bounds); }
        void setPlaceholder(const std::string& text);
        funkgui::TextEditor& editor() noexcept { return editor_; }
        void attach(funkgui::HostServices* host) { editor_.attach(host); }

        void tick(float dt, bool, bool, funkgui::Point) override { editor_.tick(dt); }
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override { editor_.draw(c, th, ring); }
        bool contains(funkgui::Point p) const override { return editor_.contains(p); }
        funkgui::Cursor cursorAt(funkgui::Point p) const override { return editor_.cursorAt(p); }
        void pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController&, funkgui::HostServices&) override
        {
            editor_.pointerDown(e);
        }
        void pointerDrag(const funkgui::PointerEvent& e, funkgui::GestureController&) override { editor_.pointerDrag(e); }
        void pointerUp(const funkgui::PointerEvent& e, funkgui::GestureController&) override { editor_.pointerUp(e); }
        void doubleClick(const funkgui::PointerEvent& e, funkgui::GestureController&) override { editor_.doubleClick(e); }
        bool wheel(const funkgui::WheelEvent& e, funkgui::GestureController&, double) override { return editor_.wheel(e); }
        bool key(const funkgui::KeyEvent& e, funkgui::GestureController&) override;
        void accessibility(std::vector<funkgui::A11yItem>& out) const override;
        bool a11yAction(uint32_t id, funkgui::A11yAction, double, funkgui::GestureController&) override
        {
            return id == editor_.a11yId();
        }
        void specLine(char* out, std::size_t n) const override;
        funkgui::Point centre() const override;
        void focusChanged(bool focused) override { editor_.setFocused(focused); }
        bool takesAllKeys() const override { return editor_.focused(); }

    private:
        funkgui::TextEditor editor_;
        std::function<void()> send_;
        std::string placeholder_;
    };
}
