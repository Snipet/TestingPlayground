// Source/editor/Items.h: what the Panel is made of (Panel.cpp, ChatItems.cpp): an Item is one control or view, which
// the Panel ticks, draws, hit-tests, focuses and feeds input to; and the small text helpers they share.
#pragma once

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>
#include <funkgui/params/GestureController.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace testingplayground::ui
{
    class Item
    {
    public:
        virtual ~Item() = default;

        virtual void tick(float dt, bool hovered, bool focused, funkgui::Point pointer) = 0;
        virtual void draw(funkgui::Canvas&, const funkgui::Theme&, bool focusRing) const = 0;
        virtual bool contains(funkgui::Point) const = 0;
        virtual funkgui::Cursor cursorAt(funkgui::Point) const { return funkgui::Cursor::normal; }
        virtual void pointerMove(const funkgui::PointerEvent&) {}
        virtual void pointerExit() {}
        virtual void pointerDown(const funkgui::PointerEvent&, funkgui::GestureController&, funkgui::HostServices&) = 0;
        virtual void pointerDrag(const funkgui::PointerEvent&, funkgui::GestureController&) {}
        virtual void pointerUp(const funkgui::PointerEvent&, funkgui::GestureController&) {}
        virtual void doubleClick(const funkgui::PointerEvent&, funkgui::GestureController&) {}
        virtual bool wheel(const funkgui::WheelEvent&, funkgui::GestureController&, double /*nowSec*/) { return false; }
        virtual bool key(const funkgui::KeyEvent&, funkgui::GestureController&) { return false; }
        virtual void accessibility(std::vector<funkgui::A11yItem>&) const = 0;
        virtual bool a11yAction(uint32_t id, funkgui::A11yAction, double value, funkgui::GestureController&) = 0;
        virtual void specLine(char* out, std::size_t n) const = 0;
        virtual bool settled() const { return true; }
        virtual funkgui::Point centre() const = 0;
        virtual bool shown() const { return true; }      // false: not drawn, no input, no Tab stop
        virtual bool focusable() const { return true; }  // false: no Tab stop (a readout)
        virtual void focusChanged(bool /*focused*/) {}
        // A text field holding the caret: every key is its own first (Tab too), Escape (when it does not use it)
        // leaves it.
        virtual bool takesAllKeys() const { return false; }
    };

    inline constexpr funkgui::TextStyle kStatusStyle{ 10.0f, 1.4f, 0.03f, false };

    inline std::string upper(std::string_view s)
    {
        std::string out(s);
        for (char& c : out)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return out;
    }

    // The longest prefix of whole "   "-separated fields of `line` that fits in `maxW` px of kMicro.
    inline std::string fitFields(const funkgui::Canvas& c, const char* line, float maxW)
    {
        std::string s = line;
        while (!s.empty() && c.textWidth(s.c_str(), funkgui::type::kMicro) > maxW)
        {
            const std::size_t cut = s.rfind("   ");
            if (cut == std::string::npos)
                return {};
            s.resize(cut);
        }
        return s;
    }

    // `s` cut to `maxW` px of `style`, with an ellipsis when it is cut.
    inline std::string fitText(const funkgui::Canvas& c, std::string s, const funkgui::TextStyle& style, float maxW)
    {
        if (c.textWidth(s.c_str(), style) <= maxW)
            return s;
        while (!s.empty() && c.textWidth((s + "...").c_str(), style) > maxW)
            s.pop_back();
        return s + "...";
    }
}
