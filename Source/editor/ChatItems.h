// Source/editor/ChatItems.h: the chat column's controls (the Panel lays them out; ChatView.h is the conversation):
//
//   InputItem   the message box: a funkgui::TextEditor (word wrap, no line numbers) inside a rounded box that grows
//               with its text from one line to kMaxRows, then scrolls; its bottom bar holds the model and thinking
//               pills and the send button (ButtonItems the Panel places there). Return sends; Shift-Return breaks the
//               line; Tab moves the focus on.
//   ButtonItem  a button of the chat column: an icon in a rounded square (the header's new chat and key), a pill (a
//               label, an icon, a chevron when a menu opens: the model, the thinking level) or a round button (send,
//               which turns into stop). It acts on press, as the panel's other buttons do, and on Return or Space
//               when focused.
//
// asciiText() turns text into what the chat's font draws (printable ASCII: the common typographic characters as ASCII,
// the rest '?'); transcriptText() is the conversation as plain text (YOU and CLAUDE above the turns), what the probes
// read.
#pragma once

#include "editor/ChatStyle.h"
#include "editor/Items.h"

#include "plugin/ChatPort.h"

#include <funkgui/widgets/TextEditor.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playground::ui
{
    std::string asciiText(std::string_view utf8);
    std::string transcriptText(const std::vector<ChatEntry>& entries);

    class InputItem final : public Item
    {
    public:
        static constexpr int kMaxRows = 8;

        InputItem(uint32_t id, std::function<void()> send, std::function<void(uint32_t)> detach = {});

        // The pictures attached to the next message, shown in a row above the text (a remove button each), and the
        // files still being read (a placeholder each). The Panel lays the box out again when the row comes or goes.
        void setPictures(std::vector<ChatPicturePtr> pictures, int pending);
        int pictureCount() const noexcept { return static_cast<int>(pictures_.size()); }
        float stripHeight() const noexcept;
        std::optional<funkgui::Point> centreOfRemove(int i) const;   // for probes
        funkgui::Rect pictureRect(int i) const;                     // the i-th tile (attached, then pending)

        // The box, and the height of its bottom bar (which the Panel's buttons sit in).
        void setBounds(funkgui::Rect box, float barH);
        funkgui::Rect box() const noexcept { return box_; }
        funkgui::Rect bar() const noexcept;
        // The box's height for its text at `width`: one row to kMaxRows.
        float desiredHeight(float width, float barH) const;
        void setPlaceholder(const std::string& text);
        const std::string& placeholder() const noexcept { return placeholder_; }
        funkgui::TextEditor& editor() noexcept { return editor_; }
        const funkgui::TextEditor& editor() const noexcept { return editor_; }
        void attach(funkgui::HostServices* host) { editor_.attach(host); }

        void tick(float dt, bool, bool, funkgui::Point p) override
        {
            editor_.tick(dt);
            pointer_ = p;
        }
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override;
        bool contains(funkgui::Point p) const override;
        funkgui::Cursor cursorAt(funkgui::Point p) const override;
        void pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController&, funkgui::HostServices&) override;
        void pointerDrag(const funkgui::PointerEvent& e, funkgui::GestureController&) override { editor_.pointerDrag(e); }
        void pointerUp(const funkgui::PointerEvent& e, funkgui::GestureController&) override { editor_.pointerUp(e); }
        void doubleClick(const funkgui::PointerEvent& e, funkgui::GestureController&) override { editor_.doubleClick(e); }
        bool wheel(const funkgui::WheelEvent& e, funkgui::GestureController&, double) override { return editor_.wheel(e); }
        bool key(const funkgui::KeyEvent& e, funkgui::GestureController&) override;
        void accessibility(std::vector<funkgui::A11yItem>& out) const override;
        bool a11yAction(uint32_t id, funkgui::A11yAction, double, funkgui::GestureController&) override;
        void specLine(char* out, std::size_t n) const override;
        funkgui::Point centre() const override;
        void focusChanged(bool focused) override { editor_.setFocused(focused); }
        bool takesAllKeys() const override { return editor_.focused(); }
        bool shown() const override { return !hidden_; }
        void setHidden(bool hidden) { hidden_ = hidden; }   // the chat folded away

    private:
        funkgui::Rect editorBounds(funkgui::Rect box, float barH) const;

        int removeAt(funkgui::Point p) const;            // the remove button under p, -1 for none

        uint32_t id_;
        funkgui::TextEditor editor_;
        std::function<void()> send_;
        std::function<void(uint32_t)> detach_;
        std::vector<ChatPicturePtr> pictures_;
        int pending_ = 0;
        funkgui::Point pointer_{ -1.0e4f, -1.0e4f };
        std::string placeholder_;
        funkgui::Rect box_;
        float barH_ = 0.0f;
        bool hidden_ = false;
    };

    class ButtonItem final : public Item
    {
    public:
        enum class Shape : uint8_t { square, pill, circle };
        enum class Look : uint8_t { ghost, outlined, primary, raised };   // raised: a switch's chosen half
        struct Spec
        {
            Shape shape = Shape::pill;
            std::function<chat::Icon()> icon;         // none when null
            std::function<std::string()> label;       // a pill's text
            bool chevron = false;                     // a menu opens
            std::function<Look()> look;               // ghost when null
            std::function<bool()> enabled;            // enabled when null
            std::function<bool()> shown;              // shown when null
            std::function<bool()> checked;            // a switch, on or off, when set (a plain button when null)
            std::function<void()> act;
            std::string a11yTitle;
            std::string spec;                         // the footer's line
        };

        ButtonItem(uint32_t id, Spec spec);

        void setBounds(funkgui::Rect r) { bounds_ = r; }
        funkgui::Rect bounds() const noexcept { return bounds_; }
        float preferredWidth() const;                 // a pill's, for its label now

        void tick(float, bool hovered, bool focused, funkgui::Point) override
        {
            hovered_ = hovered;
            focused_ = focused;
        }
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const override;
        bool contains(funkgui::Point p) const override { return bounds_.contains(p); }
        funkgui::Cursor cursorAt(funkgui::Point) const override
        {
            return enabled() ? funkgui::Cursor::pointingHand : funkgui::Cursor::normal;
        }
        void pointerDown(const funkgui::PointerEvent&, funkgui::GestureController&, funkgui::HostServices&) override;
        bool key(const funkgui::KeyEvent& e, funkgui::GestureController&) override;
        void accessibility(std::vector<funkgui::A11yItem>& out) const override;
        bool a11yAction(uint32_t id, funkgui::A11yAction, double, funkgui::GestureController&) override;
        void specLine(char* out, std::size_t n) const override;
        funkgui::Point centre() const override { return { bounds_.centreX(), bounds_.centreY() }; }
        bool shown() const override { return !hidden_ && (!spec_.shown || spec_.shown()); }
        void setHidden(bool hidden) { hidden_ = hidden; }   // the chat folded away (spec.shown aside)

    private:
        bool enabled() const { return !spec_.enabled || spec_.enabled(); }

        uint32_t id_;
        Spec spec_;
        funkgui::Rect bounds_;
        bool hovered_ = false, focused_ = false;
        bool hidden_ = false;
    };
}
