// Source/editor/Items.h: what the Panel is made of (Panel.cpp, ChatItems.cpp): an Item is one control or view, which
// the Panel ticks, draws, hit-tests, focuses and feeds input to; and the small text helpers they share.
#pragma once

#include "dsp/Sources.h"

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>
#include <funkgui/params/GestureController.h>
#include <funkgui/text/Highlighter.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace funkgui
{
    class TextEditor;
}

namespace playground::ui
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
        // What draw() reads that may change with no input to the Panel, no motion of the item's own (settled()) and no
        // change the Panel makes and marks: a parameter's value (a host's automation, Claude, a preset), a field the
        // program writes, a label or a state read through a callback. The Panel mixes every shown item's key each tick
        // and draws again when the mix moves (Panel::needsDraw, FunkGui's unchanged frames). 0, the default, for an
        // item whose picture changes only with those.
        virtual uint64_t drawKey() const { return 0; }
        virtual funkgui::Point centre() const = 0;
        virtual bool shown() const { return true; }      // false: not drawn, no input, no Tab stop
        virtual bool focusable() const { return true; }  // false: no Tab stop (a readout)
        virtual void focusChanged(bool /*focused*/) {}
        // A text field holding the caret: every key is its own first (Tab too), Escape (when it does not use it)
        // leaves it.
        virtual bool takesAllKeys() const { return false; }
    };

    // A draw key's mix (Item::drawKey, Panel::needsDraw): FNV-1a over what is added, in order.
    struct DrawKey
    {
        uint64_t h = 1469598103934665603ull;
        DrawKey& word(uint64_t v) noexcept
        {
            h ^= v;
            h *= 1099511628211ull;
            return *this;
        }
        template <typename T>
            requires std::is_integral_v<T>
        DrawKey& mix(T v) noexcept
        {
            return word(static_cast<uint64_t>(v));
        }
        DrawKey& mix(float v) noexcept { return word(std::bit_cast<uint32_t>(v)); }
        DrawKey& mix(std::string_view s) noexcept
        {
            for (const char c : s)
                word(static_cast<unsigned char>(c));
            return word(s.size());
        }
        DrawKey& mix(const char* s) noexcept { return mix(std::string_view(s != nullptr ? s : "")); }
        DrawKey& mix(const std::string& s) noexcept { return mix(std::string_view(s)); }
        DrawKey& mix(funkgui::Point p) noexcept { return mix(p.x).mix(p.y); }
        DrawKey& mix(const funkgui::Rect& r) noexcept { return mix(r.x).mix(r.y).mix(r.w).mix(r.h); }
        DrawKey& mix(const funkgui::TextEditor& e) noexcept;   // its text's revision, caret, selection, scroll, focus,
                                                               // decorations and bounds (Items.cpp)
    };

    // Motion with no input to the item (Item::settled): what it shows that something else moves, a host's automation,
    // Claude's or a preset's write, MIDI, a field the program writes (a live= mark, a shader's input). Each tick takes
    // the values it shows (0..1, up to four) or whether it saw one move; it is moving() from the tick that saw a move
    // until kFor seconds of none, so the Panel runs at full rate while it moves (FunkGui's FramePump runs a settled
    // editor at about 12 Hz) and automation points a frame or two apart keep it there. A value's move is a change of
    // more than kStep since the last one counted (smaller ones add up); the first values seen are none.
    class Stirred
    {
    public:
        static constexpr float kFor = 0.25f;             // seconds
        static constexpr float kStep = 1.0e-4f;          // of a value's 0..1

        void tick(float dt, bool moved) noexcept
        {
            quiet_ = moved ? 0.0f : std::min(quiet_ + std::max(dt, 0.0f), kFor);
        }
        void tick(float dt, std::initializer_list<float> values) noexcept
        {
            bool moved = false;
            std::size_t k = 0;
            for (const float v : values)
            {
                if (k == seen_.size())
                    break;
                moved = moved || std::fabs(v - seen_[k]) > kStep;
                ++k;
            }
            if (moved || !primed_)
                std::copy_n(values.begin(), k, seen_.begin());
            tick(dt, moved && primed_);
            primed_ = true;
        }
        bool moving() const noexcept { return quiet_ < kFor; }

    private:
        std::array<float, 4> seen_{};
        bool primed_ = false;
        float quiet_ = kFor;
    };

    inline constexpr funkgui::TextStyle kStatusStyle{ 10.0f, 1.4f, 0.03f, false };

    // `utf8` as the panel's font draws it: printable ASCII, the common typographic characters as ASCII ("--", "...",
    // "->"), the rest '?' (Items.cpp).
    std::string asciiText(std::string_view utf8);

    // FMinus's words, for the chat's code blocks (the editor has its own highlighter, CodeAssist.h): C's that FMinus has,
    // and its own (module, param, parallel, the vector types). It has no strings, and of the preprocessor only #include
    // and #pragma: a line that starts with # is one preprocessor span.
    inline funkgui::text::KeywordRules fminusRules()
    {
        funkgui::text::KeywordRules r;
        r.keywords = { "break", "const", "continue", "else", "false", "for", "if", "module", "param", "parallel",
                       "restrict", "return", "sizeof", "this", "true", "while" };
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

    // What a file of the program is, by its name: FMinus (the program and what it includes), a .gui file (the
    // interface, fminus/GuiSpec.h) or a circuit netlist (.cir, SPICE's, fminus/Circuits.h). A tab's highlighter and what
    // the code editor offers in it follow it (CodeAssist.h): renamed to another kind, a tab is made again.
    enum class FileKind : uint8_t
    {
        FMinus,
        Gui,
        Circuit,
    };

    inline FileKind fileKindOf(std::string_view name)
    {
        if (name.size() > 4 && name.substr(name.size() - 4) == ".gui")
            return FileKind::Gui;
        return isCircuitFile(name) ? FileKind::Circuit : FileKind::FMinus;
    }

    // A file of the program's interface: its .gui file, a shader one names (a .sc file, fminus/Shaders.h) or a .sh file
    // shaders include. While one shows, the CODE view's panel under the editor follows a choice of its own (Panel.h:
    // the panel under the code).
    inline bool isInterfaceFile(std::string_view name)
    {
        const auto ends = [name](std::string_view tail) {
            return name.size() > tail.size() && name.substr(name.size() - tail.size()) == tail;
        };
        return ends(".gui") || ends(".sc") || ends(".sh");
    }

    // A netlist's words, for a highlighter of keyword rules (a tab's is CodeAssist's CircuitHighlighter, over
    // lang::lexCirLine): its devices and the directives' words as types, ; comments.
    inline funkgui::text::KeywordRules circuitRules()
    {
        funkgui::text::KeywordRules r;
        r.keywords = { ".adaa", ".channels", ".end", ".input", ".method", ".model", ".op", ".output", ".oversample",
                       ".pot", ".prewarp", ".solver", ".switch", ".target", ".title" };
        r.types = { "OTA", "opamp" };
        r.lineComment = ";";
        r.blockOpen = "";
        r.blockClose = "";
        r.quotes = "";
        r.preprocessor = '\0';
        return r;
    }

    // The keyword rules of a file's kind.
    inline funkgui::text::KeywordRules rulesFor(FileKind kind)
    {
        return kind == FileKind::Gui ? guiRules() : kind == FileKind::Circuit ? circuitRules() : fminusRules();
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
