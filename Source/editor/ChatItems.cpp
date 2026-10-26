// Source/editor/ChatItems.cpp: see ChatItems.h.
#include "editor/ChatItems.h"

#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace testingplaygroundsynth::ui
{
    // ---- text ---------------------------------------------------------------------------------------------------------
    std::string asciiText(std::string_view s)
    {
        std::string out;
        out.reserve(s.size());
        for (std::size_t i = 0; i < s.size();)
        {
            const auto c = static_cast<unsigned char>(s[i]);
            if (c < 0x80)
            {
                if (c == '\t' || c == '\n' || (c >= 32 && c < 127))
                    out += static_cast<char>(c);
                ++i;
                continue;
            }
            // One UTF-8 sequence: its codepoint, or '?' for a malformed one.
            const int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            uint32_t cp = n == 4 ? c & 0x07u : n == 3 ? c & 0x0Fu : c & 0x1Fu;
            bool ok = n > 1 && i + static_cast<std::size_t>(n) <= s.size();
            for (int k = 1; ok && k < n; ++k)
            {
                const auto cc = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
                ok = (cc & 0xC0u) == 0x80u;
                cp = (cp << 6) | (cc & 0x3Fu);
            }
            i += ok ? static_cast<std::size_t>(n) : 1;
            if (!ok)
            {
                out += '?';
                continue;
            }
            switch (cp)
            {
                case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x202F: out += ' '; break;
                case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2212: out += '-'; break;
                case 0x2014: case 0x2015: out += "--"; break;
                case 0x2018: case 0x2019: case 0x201B: case 0x2032: out += '\''; break;
                case 0x201C: case 0x201D: case 0x201F: case 0x2033: out += '"'; break;
                case 0x2026: out += "..."; break;
                case 0x2022: case 0x2023: case 0x25CF: case 0x00B7: out += '*'; break;
                case 0x00D7: out += 'x'; break;
                case 0x00F7: out += '/'; break;
                case 0x2192: case 0x27F6: out += "->"; break;
                case 0x2190: out += "<-"; break;
                case 0x2194: out += "<->"; break;
                case 0x21D2: out += "=>"; break;
                case 0x2264: out += "<="; break;
                case 0x2265: out += ">="; break;
                case 0x2260: out += "!="; break;
                case 0x2248: out += "~="; break;
                case 0x00B1: out += "+-"; break;
                case 0x00B0: out += " deg"; break;
                case 0x00B5: case 0x03BC: out += 'u'; break;
                case 0x03C0: out += "pi"; break;
                case 0x221E: out += "inf"; break;
                case 0x2713: case 0x2714: out += "ok"; break;
                case 0x2717: case 0x2718: out += 'x'; break;
                case 0x00BD: out += "1/2"; break;
                case 0x00BC: out += "1/4"; break;
                case 0x00B2: out += "^2"; break;
                case 0x00B3: out += "^3"; break;
                case 0x221A: out += "sqrt"; break;
                case 0x2211: out += "sum"; break;
                case 0x0394: out += "delta"; break;
                case 0x03C9: out += "w"; break;
                case 0xFE0F: case 0x200B: case 0x200D: break;   // variation selectors, zero widths
                default: out += '?'; break;
            }
        }
        return out;
    }

    std::string transcriptText(const std::vector<ChatEntry>& entries)
    {
        std::string out;
        bool claudeHeader = false;                   // CLAUDE written for the turn under way
        ChatEntry::Kind previous = ChatEntry::Kind::note;
        bool first = true;
        for (const ChatEntry& e : entries)
        {
            const std::string text = asciiText(e.text);
            const bool fromClaude = e.kind == ChatEntry::Kind::assistant || e.kind == ChatEntry::Kind::thinking
                                 || e.kind == ChatEntry::Kind::tool;
            const bool tight = !first && e.kind == ChatEntry::Kind::tool && previous == ChatEntry::Kind::tool;
            if (!first)
                out += tight ? "" : "\n";
            first = false;
            if (e.kind == ChatEntry::Kind::user)
            {
                out += "YOU\n" + text + "\n";
                claudeHeader = false;
            }
            else if (fromClaude)
            {
                if (!claudeHeader)
                {
                    out += "CLAUDE\n";
                    claudeHeader = true;
                }
                std::string body = text;
                while (!body.empty() && body.back() == '\n')
                    body.pop_back();
                const char* prefix = e.kind == ChatEntry::Kind::thinking ? "  ~ " : e.kind == ChatEntry::Kind::tool ? "  > " : "";
                std::size_t start = 0;
                while (start <= body.size())
                {
                    const std::size_t end = body.find('\n', start);
                    out += prefix + body.substr(start, end == std::string::npos ? std::string::npos : end - start) + "\n";
                    if (end == std::string::npos)
                        break;
                    start = end + 1;
                }
            }
            else
            {
                out += std::string(e.kind == ChatEntry::Kind::error ? "! " : "- ") + text + "\n";
            }
            previous = e.kind;
        }
        return out;
    }

    // ---- the message box --------------------------------------------------------------------------------------------
    namespace
    {
        constexpr float kBoxPad = 6.0f;              // the editor's inset from the box (its frame is clipped away)
        constexpr float kEditorPadding = 4.0f;       // TextEditorOptions::padding
        constexpr float kBoxRadius = 14.0f;
    }

    InputItem::InputItem(uint32_t id, std::function<void()> send) : editor_(id), send_(std::move(send))
    {
        funkgui::TextEditorOptions o;
        o.style = chat::kBody;
        o.lineSpacing = 1.45f;
        o.wordWrap = true;
        o.lineNumbers = false;
        o.autoIndent = false;
        o.highlightLine = false;
        o.padding = kEditorPadding;
        o.a11yTitle = "Message to Claude";
        editor_.setOptions(o);
    }

    funkgui::Rect InputItem::editorBounds(funkgui::Rect box, float barH) const
    {
        return { box.x + kBoxPad, box.y + kBoxPad, box.w - 2.0f * kBoxPad, std::max(10.0f, box.h - barH - 2.0f * kBoxPad) };
    }

    void InputItem::setBounds(funkgui::Rect box, float barH)
    {
        box_ = box;
        barH_ = barH;
        editor_.setBounds(editorBounds(box, barH));
    }

    funkgui::Rect InputItem::bar() const noexcept { return { box_.x, box_.bottom() - barH_, box_.w, barH_ }; }

    float InputItem::desiredHeight(float width, float barH) const
    {
        // The rows the text takes at that width: the editor wraps at its own width, so measure there.
        const int rows = std::clamp(editor_.rowCount(), 1, kMaxRows);
        (void)width;
        return 2.0f * kBoxPad + static_cast<float>(rows) * editor_.rowHeight() + 2.0f * kEditorPadding + 2.0f + barH;
    }

    void InputItem::setPlaceholder(const std::string& text)
    {
        if (text == placeholder_)
            return;
        placeholder_ = text;
        funkgui::TextEditorOptions o = editor_.options();
        o.placeholder = text;
        editor_.setOptions(o);
    }

    bool InputItem::contains(funkgui::Point p) const
    {
        // The box above its bar: the bar's buttons are items of their own.
        return funkgui::Rect{ box_.x, box_.y, box_.w, box_.h - barH_ }.contains(p);
    }

    void InputItem::draw(funkgui::Canvas& c, const funkgui::Theme& th, bool) const
    {
        const chat::Palette p = chat::palette(th);
        const bool focused = editor_.focused();
        c.rrect(box_.x, box_.y, box_.w, box_.h, kBoxRadius, p.composer, 1.0f,
                focused ? funkgui::mix(p.composerEdge, p.accent, 0.38f) : p.composerEdge);
        // The editor without its square frame: a clip just inside it.
        const funkgui::Rect e = editor_.bounds();
        c.pushClip({ e.x + 1.5f, e.y + 1.5f, e.w - 3.0f, e.h - 3.0f });
        editor_.draw(c, th, false);
        c.popClip();
    }

    bool InputItem::key(const funkgui::KeyEvent& e, funkgui::GestureController&)
    {
        if (e.key == funkgui::Key::tab)
            return false;                                // the Panel moves the focus on
        if (e.key == funkgui::Key::enter && !e.mods.shift && !e.mods.cmd && !e.mods.ctrl && !e.mods.alt)
        {
            if (send_)
                send_();
            return true;
        }
        if (e.key == funkgui::Key::enter && e.mods.shift)
        {
            editor_.insert("\n");                    // TextEditor's own Return, without the modifier
            return true;
        }
        return editor_.key(e);
    }

    void InputItem::accessibility(std::vector<funkgui::A11yItem>& out) const
    {
        funkgui::A11yItem item;
        editor_.accessibility(item);
        out.push_back(std::move(item));
    }

    void InputItem::specLine(char* out, std::size_t n) const
    {
        std::snprintf(out, n, "MESSAGE   RETURN SENDS IT   SHIFT-RETURN BREAKS THE LINE");
    }

    funkgui::Point InputItem::centre() const
    {
        const funkgui::Rect b = editor_.bounds();
        return { b.centreX(), b.centreY() };
    }

    // ---- buttons ------------------------------------------------------------------------------------------------------
    ButtonItem::ButtonItem(uint32_t id, Spec spec) : id_(id), spec_(std::move(spec)) {}

    float ButtonItem::preferredWidth() const
    {
        if (spec_.shape != Shape::pill)
            return bounds_.h;
        const std::string label = spec_.label ? spec_.label() : std::string();
        const float tw = funkgui::text::width(funkgui::FontService::get().atlas(), label.c_str(), chat::kSmall);
        const bool icon = spec_.icon && spec_.icon() != chat::Icon::none;
        return std::ceil(12.0f + (icon ? 18.0f : 0.0f) + tw + (spec_.chevron ? 16.0f : 0.0f) + 10.0f);
    }

    void ButtonItem::draw(funkgui::Canvas& c, const funkgui::Theme& th, bool ring) const
    {
        const chat::Palette p = chat::palette(th);
        const bool on = enabled();
        const Look look = spec_.look ? spec_.look() : Look::ghost;
        const chat::Icon icon = spec_.icon ? spec_.icon() : chat::Icon::none;
        const funkgui::Rect& r = bounds_;
        const float radius = spec_.shape == Shape::square ? 8.0f : r.h * 0.5f;
        funkgui::Col fill = p.card.withAlpha(0.0f), edge = p.card.withAlpha(0.0f), ink = on ? p.soft : p.faint;
        if (look == Look::primary)
        {
            fill = on ? (hovered_ ? funkgui::mix(p.accent, p.text, 0.12f) : p.accent) : p.hover;
            ink = on ? p.onAccent : p.faint;
        }
        else if (look == Look::raised)
        {
            fill = p.composer;
            edge = p.composerEdge;
            ink = p.text;
        }
        else
        {
            if (look == Look::outlined)
                edge = p.composerEdge;
            if (hovered_ && on)
            {
                fill = p.hover;
                ink = p.text;
            }
        }
        if (spec_.shape == Shape::circle)
            c.disc(r.centreX(), r.centreY(), r.h * 0.5f, fill, look == Look::outlined ? 1.0f : 0.0f, edge);
        else
            c.rrect(r.x, r.y, r.w, r.h, radius, fill, look == Look::outlined || look == Look::raised ? 1.0f : 0.0f, edge);
        if (ring && focused_)
            c.rrect(r.x - 2.0f, r.y - 2.0f, r.w + 4.0f, r.h + 4.0f, radius + 2.0f, p.card.withAlpha(0.0f), 1.5f, p.accent);

        if (spec_.shape != Shape::pill)
        {
            chat::drawIcon(c, icon, r.centreX(), r.centreY(), spec_.shape == Shape::circle ? 13.0f : 14.0f, ink);
            return;
        }
        float x = r.x + 12.0f;
        if (icon != chat::Icon::none)
        {
            chat::drawIcon(c, icon, x + 6.0f, r.centreY(), 12.0f, ink);
            x += 18.0f;
        }
        const std::string label = spec_.label ? spec_.label() : std::string();
        const float room = r.right() - x - (spec_.chevron ? 22.0f : 10.0f);
        c.text(fitText(c, label, chat::kSmall, room).c_str(), x, c.capCentreTop(r.centreY(), chat::kSmall), chat::kSmall, ink);
        if (spec_.chevron)
            chat::drawIcon(c, chat::Icon::chevronDown, r.right() - 14.0f, r.centreY() + 0.5f, 8.0f, ink);
    }

    void ButtonItem::pointerDown(const funkgui::PointerEvent&, funkgui::GestureController&, funkgui::HostServices&)
    {
        if (enabled() && spec_.act)
            spec_.act();
    }

    bool ButtonItem::key(const funkgui::KeyEvent& e, funkgui::GestureController&)
    {
        if (e.key != funkgui::Key::enter && e.key != funkgui::Key::space)
            return false;
        if (enabled() && spec_.act)
            spec_.act();
        return true;
    }

    void ButtonItem::accessibility(std::vector<funkgui::A11yItem>& out) const
    {
        funkgui::A11yItem item;
        item.id = id_;
        item.role = spec_.checked ? funkgui::A11yRole::toggleButton : funkgui::A11yRole::button;
        item.bounds = bounds_;
        item.title = spec_.a11yTitle;
        if (spec_.checked)
        {
            item.checkable = true;
            item.checked = spec_.checked();
        }
        else if (spec_.label)
            item.value = spec_.label();
        item.enabled = enabled();
        out.push_back(std::move(item));
    }

    bool ButtonItem::a11yAction(uint32_t id, funkgui::A11yAction, double, funkgui::GestureController&)
    {
        if (id != id_)
            return false;
        if (enabled() && spec_.act)
            spec_.act();
        return true;
    }

    void ButtonItem::specLine(char* out, std::size_t n) const { std::snprintf(out, n, "%s", spec_.spec.c_str()); }
}
