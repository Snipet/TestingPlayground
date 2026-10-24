// Source/editor/ChatItems.cpp: see ChatItems.h.
#include "editor/ChatItems.h"

#include <algorithm>
#include <cstdio>

namespace testingplayground::ui
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

    uint32_t TranscriptHighlighter::line(std::string_view line, uint32_t stateIn, std::vector<funkgui::text::Span>& out) const
    {
        const int n = static_cast<int>(line.size());
        const auto whole = [&](uint16_t style) {
            if (n > 0)
                out.push_back({ 0, n, style });
        };
        const bool isFence = line.substr(0, 3) == "```" || line.substr(0, 7) == "    ```" || line.substr(0, 7) == "  ~ ```";
        if (isFence)
        {
            whole(fence);
            return stateIn == 0 ? 1u : 0u;
        }
        if (stateIn == 1)
        {
            whole(code);
            return 1;
        }
        if (line == "YOU" || line == "CLAUDE")
            whole(speaker);
        else if (line.substr(0, 4) == "  > ")
            whole(tool);
        else if (line.substr(0, 4) == "  ~ ")
            whole(thinking);
        else if (line.substr(0, 2) == "! ")
            whole(error);
        else if (line.substr(0, 2) == "- ")
            whole(note);
        return 0;
    }

    std::vector<funkgui::text::HighlightStyle> transcriptPalette()
    {
        using S = funkgui::text::HighlightStyle;
        std::vector<S> p(8);
        p[TranscriptHighlighter::plain] = S{ S::Ink::ink100 };
        p[TranscriptHighlighter::speaker] = S{ S::Ink::ink52, {}, 0.08f };
        p[TranscriptHighlighter::tool] = S{ S::Ink::accent };
        p[TranscriptHighlighter::thinking] = S{ S::Ink::ink52 };
        p[TranscriptHighlighter::error] = S{ S::Ink::signal };
        p[TranscriptHighlighter::note] = S{ S::Ink::ink52 };
        p[TranscriptHighlighter::code] = S{ S::Ink::ink70 };
        p[TranscriptHighlighter::fence] = S{ S::Ink::ink32 };
        return p;
    }

    // ---- the transcript -------------------------------------------------------------------------------------------------
    TranscriptItem::TranscriptItem(funkgui::Rect bounds, uint32_t id) : editor_(id)
    {
        funkgui::TextEditorOptions o;
        o.style = funkgui::editorFont(funkgui::EditorFont::code);
        o.wordWrap = true;
        o.lineNumbers = false;
        o.readOnly = true;
        o.highlightLine = false;
        o.a11yTitle = "Chat with Claude";
        editor_.setOptions(o);
        editor_.setBounds(bounds);
        editor_.setHighlighter(std::make_unique<TranscriptHighlighter>());
        editor_.setPalette(transcriptPalette());
    }

    void TranscriptItem::setBounds(funkgui::Rect bounds)
    {
        const bool atEnd = editor_.scroll().y >= editor_.contentHeight() - editor_.textArea().h - 1.0f;
        editor_.setBounds(bounds);
        if (atEnd)
            editor_.setScroll(0.0f, 1.0e9f);
    }

    void TranscriptItem::show(const std::vector<ChatEntry>& entries, const std::string& hint)
    {
        std::string t = entries.empty() ? hint : transcriptText(entries);
        if (t == text_)
            return;
        // Following the end: when the view was at the bottom (or the text only grew a little past it), it stays there.
        const float bottom = editor_.contentHeight() - editor_.textArea().h;
        const bool atEnd = text_.empty() || editor_.scroll().y >= bottom - editor_.rowHeight() * 2.0f;
        const funkgui::Point keep = editor_.scroll();
        text_ = std::move(t);
        editor_.setText(text_);
        if (atEnd)
            editor_.setScroll(0.0f, 1.0e9f);
        else
            editor_.setScroll(keep.x, keep.y);
    }

    void TranscriptItem::accessibility(std::vector<funkgui::A11yItem>& out) const
    {
        funkgui::A11yItem item;
        editor_.accessibility(item);
        item.readOnly = true;
        out.push_back(std::move(item));
    }

    void TranscriptItem::specLine(char* out, std::size_t n) const
    {
        std::snprintf(out, n, "CHAT   SELECT TEXT AND CMD-C TO COPY IT   THE WHEEL SCROLLS");
    }

    funkgui::Point TranscriptItem::centre() const
    {
        const funkgui::Rect b = editor_.bounds();
        return { b.centreX(), b.centreY() };
    }

    // ---- the message box ------------------------------------------------------------------------------------------------
    InputItem::InputItem(funkgui::Rect bounds, uint32_t id, std::function<void()> send)
        : editor_(id), send_(std::move(send))
    {
        funkgui::TextEditorOptions o;
        o.style = funkgui::editorFont(funkgui::EditorFont::code);
        o.wordWrap = true;
        o.lineNumbers = false;
        o.autoIndent = false;
        o.highlightLine = false;
        o.a11yTitle = "Message to Claude";
        editor_.setOptions(o);
        editor_.setBounds(bounds);
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
}
