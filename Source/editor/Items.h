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
#include <funkgui/text/Highlighter.h>

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

    // FMinus's words, for the editor's highlighter and the chat's code blocks: C's that FMinus has, and its own (module,
    // param, the vector types). It has no strings, and of the preprocessor only #include and #pragma: a line that starts
    // with # is one preprocessor span.
    inline funkgui::text::KeywordRules fminusRules()
    {
        funkgui::text::KeywordRules r;
        r.keywords = { "break", "const", "continue", "else", "false", "for", "if", "module", "param", "restrict",
                       "return", "sizeof", "this", "true", "while" };
        r.types = { "bool", "float", "float2", "float4", "float8", "int", "poly", "void" };
        r.lineComment = "//";
        r.blockOpen = "/*";
        r.blockClose = "*/";
        r.quotes = "";
        r.preprocessor = '#';
        return r;
    }

    // A .gui file's words (fminus/GuiSpec.h): its containers and decorations as keywords, its controls and displays as
    // types, its "texts" as strings; // and /* */ comments.
    inline funkgui::text::KeywordRules guiRules()
    {
        funkgui::text::KeywordRules r;
        r.keywords = { "box", "column", "divider", "grid", "label", "page", "panel", "row", "section", "spacer", "tabs" };
        r.types = { "buttons", "envelope", "knob", "led", "menu", "meter", "number", "scope", "slider", "spectrum",
                    "switch", "vu", "xy" };
        r.lineComment = "//";
        r.blockOpen = "/*";
        r.blockClose = "*/";
        r.quotes = "\"";
        r.preprocessor = '\0';
        return r;
    }

    // The rules a file's tab highlights with, by its name.
    inline funkgui::text::KeywordRules rulesFor(std::string_view name)
    {
        return name.size() > 4 && name.substr(name.size() - 4) == ".gui" ? guiRules() : fminusRules();
    }

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
