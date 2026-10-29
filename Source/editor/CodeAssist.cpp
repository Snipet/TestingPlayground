// Source/editor/CodeAssist.cpp: see CodeAssist.h.
#include "editor/CodeAssist.h"

#include "editor/ChatStyle.h"
#include "editor/Items.h"
#include "editor/Theme.h"

#include <funkgui/canvas/Tags.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace playground::ui
{
    namespace
    {
        constexpr float kStatusH = 22.0f;                // the status line under the editor
        constexpr float kRowH = 22.0f;                   // a completion row
        constexpr int   kRows = 10;                      // completion rows shown at once
        constexpr float kPopupW = 440.0f;
        constexpr float kDetailH = 78.0f;                // the chosen item's signature and documentation
        constexpr float kPad = 8.0f;
        constexpr float kFindW = 420.0f;
        constexpr float kFindRowH = 32.0f;
        constexpr float kCardMaxW = 540.0f;              // a hover card's or the signature's width, at most
        constexpr float kHoverDelay = 0.5f;              // s still over a word
        // A key that would edit a read-only text (the IR view): short enough for the card's header at its least width,
        // and no acronym (the header writes the status in sentence case).
        constexpr const char* kReadOnlyNotice = "READ ONLY: A VIEW OF THE COMPILED PROGRAM";
        constexpr float kCheckDelay = 0.6f;              // s after the last edit
        constexpr float kOccurrenceDelay = 0.25f;        // s the caret rests
        constexpr const char* kFormatOnRunKey = "codeFormatOnRun";
        constexpr int kMaxFind = 200;                    // characters in the find bar's fields
        constexpr int kMaxGoTo = 16;                     // characters in the go-to-line field ("123456:789")
        constexpr float kGoToW = 330.0f;
        constexpr int kMaxMatches = 10000;
        constexpr funkgui::TextStyle kCode = funkgui::editorFont(funkgui::EditorFont::code);
        constexpr funkgui::TextStyle kCardCode{ 11.5f, 0.0f, 0.0f, true };

        using funkgui::Col;
        using funkgui::Rect;
        using funkgui::TextPos;

        float widthOf(std::string_view s, const funkgui::TextStyle& st)
        {
            const std::string str(s);
            return funkgui::text::width(funkgui::FontService::get().atlas(), str.c_str(), st);
        }

        // Documentation as a card shows it: a comment's lines wrapped at its width (long ones) joined into paragraphs
        // (a blank line or a short line ends one), the lines of a usage table (indented) kept as lines, their columns'
        // gaps narrowed and their continuations joined.
        std::string reflow(const std::string& doc)
        {
            std::string out;
            bool table = false;                          // the last line was a table's
            int tableIndent = 0;
            std::size_t lastLength = 0;                  // the last line's, as written
            for (const std::string_view raw : lang::splitLines(doc))
            {
                std::size_t k = 0;
                while (k < raw.size() && raw[k] == ' ')
                    ++k;
                std::string l(raw.substr(k));
                while (!l.empty() && l.back() == ' ')
                    l.pop_back();
                if (l.empty())
                {
                    if (!out.empty() && out.back() != '\n')
                        out += '\n';
                    out += '\n';
                    table = false;
                    continue;
                }
                // Columns: a run of three spaces or more becomes two.
                std::string narrowed;
                int spaces = 0;
                for (const char c : l)
                {
                    spaces = c == ' ' ? spaces + 1 : 0;
                    if (c != ' ' || spaces <= 2)
                        narrowed += c;
                }
                const int indent = static_cast<int>(k);
                const bool continues = (table && indent > tableIndent) || (indent < 2 && lastLength >= 60);
                lastLength = raw.size();
                if (out.empty() || out.back() == '\n')
                    out += narrowed;
                else if (!continues)
                    out += '\n' + narrowed;             // a table's row, or a line of its own
                else
                    out += ' ' + narrowed;               // a paragraph's, or a row's continuation
                if (indent >= 2 && !(table && indent > tableIndent))
                {
                    table = true;
                    tableIndent = indent;
                }
                else if (indent < 2)
                    table = false;
            }
            while (!out.empty() && out.back() == '\n')
                out.pop_back();
            return out;
        }

        // `text` broken into lines of at most maxW px (at spaces where it can), its own lines kept; none for no text.
        std::vector<std::string> wrap(const std::string& text, const funkgui::TextStyle& st, float maxW, std::size_t maxLines)
        {
            std::vector<std::string> out;
            if (text.empty())
                return out;
            for (const std::string_view para : lang::splitLines(text))
            {
                std::string line(para);
                while (!line.empty() && widthOf(line, st) > maxW && out.size() < maxLines)
                {
                    std::size_t cut = std::string::npos;
                    for (std::size_t k = line.size() - 1; k > 0; --k)
                        if (line[k] == ' ' && widthOf(std::string_view(line).substr(0, k), st) <= maxW)
                        {
                            cut = k;
                            break;
                        }
                    if (cut == std::string::npos)
                    {
                        std::size_t k = line.size();
                        while (k > 1 && widthOf(std::string_view(line).substr(0, k), st) > maxW)
                            --k;
                        out.push_back(line.substr(0, k));
                        line = line.substr(k);
                        continue;
                    }
                    out.push_back(line.substr(0, cut));
                    std::size_t next = cut;
                    while (next < line.size() && line[next] == ' ')
                        ++next;
                    line = line.substr(next);
                }
                if (out.size() >= maxLines)
                    break;
                out.push_back(line);
            }
            if (out.size() > maxLines)
                out.resize(maxLines);
            return out;
        }

        bool fmName(std::string_view name) { return !(name.size() > 4 && name.substr(name.size() - 4) == ".gui"); }

        // A key that would change the text: typing (Alt's symbols, Shift-Alt-F's Format too), Return, Space, Backspace,
        // Delete, cut, paste, undo and redo, and the line commands (Cmd-/, Cmd-] and Cmd-[, Shift-Cmd-K, Alt-Up and
        // Alt-Down). Cmd-Return is RUN's, which the Panel takes first; Tab moves the focus on from a read-only text.
        bool editKey(const funkgui::KeyEvent& e)
        {
            const bool cmd = e.mods.cmd;
            switch (e.key)
            {
                case funkgui::Key::enter:
                    return !cmd && !e.mods.ctrl;
                case funkgui::Key::space:
                    return !cmd && !e.mods.ctrl;
                case funkgui::Key::backspace:
                case funkgui::Key::del:
                    return true;
                case funkgui::Key::up:
                case funkgui::Key::down:
                    return e.mods.alt && !cmd;
                case funkgui::Key::character:
                {
                    if (!cmd)
                        return !e.mods.ctrl;
                    const char32_t c = e.ch >= U'A' && e.ch <= U'Z' ? e.ch - U'A' + U'a' : e.ch;
                    return c == U'v' || c == U'x' || c == U'z' || c == U'y' || c == U'/' || c == U']' || c == U'['
                        || (c == U'k' && e.mods.shift);
                }
                case funkgui::Key::tab:
                case funkgui::Key::escape:
                case funkgui::Key::left:
                case funkgui::Key::right:
                case funkgui::Key::home:
                case funkgui::Key::end:
                case funkgui::Key::pageUp:
                case funkgui::Key::pageDown:
                    return false;
            }
            return false;
        }

        bool identStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

        // How a completion's kind looks: a letter in a rounded square of its colour.
        struct KindLook
        {
            char letter;
            Col dark, light;
        };
        KindLook kindLook(lang::Kind k)
        {
            switch (k)
            {
                case lang::Kind::module:    return { 'M', { 0xE2, 0xA3, 0x36, 255 }, { 0xB0, 0x6E, 0x00, 255 } };
                case lang::Kind::function:
                case lang::Kind::method:
                case lang::Kind::builtin:   return { 'f', { 0xB4, 0x8E, 0xF7, 255 }, { 0x7A, 0x3E, 0xB8, 255 } };
                case lang::Kind::field:     return { 'F', { 0x75, 0xBE, 0xFF, 255 }, { 0x1F, 0x6F, 0xC2, 255 } };
                case lang::Kind::param:     return { 'P', { 0x5E, 0xD3, 0xB5, 255 }, { 0x0E, 0x86, 0x6A, 255 } };
                case lang::Kind::local:
                case lang::Kind::argument:  return { 'v', { 0x7F, 0xD4, 0xE8, 255 }, { 0x1E, 0x7E, 0x96, 255 } };
                case lang::Kind::lane:      return { 'x', { 0x7F, 0xD4, 0xE8, 255 }, { 0x1E, 0x7E, 0x96, 255 } };
                case lang::Kind::snippet:   return { '{', { 0xC8, 0xC8, 0xC8, 255 }, { 0x55, 0x55, 0x55, 255 } };
                case lang::Kind::file:      return { '#', { 0xC5, 0x86, 0xC0, 255 }, { 0x9A, 0x3E, 0xA8, 255 } };
                case lang::Kind::attribute: return { '@', { 0x9A, 0x9A, 0x9A, 255 }, { 0x66, 0x66, 0x66, 255 } };
                case lang::Kind::keyword:
                case lang::Kind::type:
                case lang::Kind::word:      return { 'k', { 0x9A, 0x9A, 0x9A, 255 }, { 0x66, 0x66, 0x66, 255 } };
            }
            return { '?', { 0x9A, 0x9A, 0x9A, 255 }, { 0x66, 0x66, 0x66, 255 } };
        }

        funkgui::text::HighlightStyle custom(Col c, float weight = 0.0f)
        {
            funkgui::text::HighlightStyle s;
            s.ink = funkgui::text::HighlightStyle::Ink::custom;
            s.colour = c;
            s.weight = weight;
            return s;
        }
        funkgui::text::HighlightStyle ink(funkgui::text::HighlightStyle::Ink i, float weight = 0.0f)
        {
            funkgui::text::HighlightStyle s;
            s.ink = i;
            s.weight = weight;
            return s;
        }

        // The colours the editor's ground and its status line take.
        Col editorGround(const funkgui::Theme& th)
        {
            const bool dark = th.ground.r < 128;
            return dark ? funkgui::mix(th.ground, Col{ 0, 0, 0, 255 }, 0.18f) : funkgui::mix(th.ground, Col{ 255, 255, 255, 255 }, 0.80f);
        }

        int lineIndent(const std::string& l)
        {
            int k = 0;
            while (k < static_cast<int>(l.size()) && (l[static_cast<std::size_t>(k)] == ' ' || l[static_cast<std::size_t>(k)] == '\t'))
                ++k;
            return k;
        }

        // A position after `text` inserted at `at`.
        TextPos after(TextPos at, std::string_view text)
        {
            for (const char c : text)
            {
                if (c == '\n')
                {
                    ++at.line;
                    at.col = 0;
                }
                else
                    ++at.col;
            }
            return at;
        }
    }

    // ---- the highlighter --------------------------------------------------------------------------------------------
    uint32_t CodeHighlighter::line(std::string_view l, uint32_t stateIn, std::vector<funkgui::text::Span>& out) const
    {
        bool inComment = stateIn != 0;
        std::vector<lang::Token> toks;
        lang::lexLine(l, 0, inComment, toks);
        const auto span = [&](int a, int b, uint16_t st) {
            if (b > a)
                out.push_back({ a, b, st });
        };
        bool attr = false;
        for (std::size_t i = 0; i < toks.size(); ++i)
        {
            const lang::Token& t = toks[i];
            const lang::Token* next = i + 1 < toks.size() ? &toks[i + 1] : nullptr;
            const lang::Token* prev = i > 0 ? &toks[i - 1] : nullptr;
            uint16_t st = csPlain;
            if (!attr && t.text == "[" && next != nullptr && next->text == "[")
                attr = true;
            switch (t.kind)
            {
                case lang::Tok::comment:   st = csComment; break;
                case lang::Tok::keyword:   st = csKeyword; break;
                case lang::Tok::type:      st = csType; break;
                case lang::Tok::number:    st = csNumber; break;
                case lang::Tok::op:        st = csOperator; break;
                case lang::Tok::punct:     st = csPunct; break;
                case lang::Tok::unknown:   st = csPlain; break;
                case lang::Tok::identifier:
                {
                    const bool call = next != nullptr && next->text == "(";
                    if (prev != nullptr && (prev->text == "." || prev->text == "->"))
                        st = call ? csFunction : csMember;
                    else if (call)
                        st = csFunction;
                    else if (types_ != nullptr && std::binary_search(types_->names.begin(), types_->names.end(), std::string(t.text)))
                        st = csType;
                    break;
                }
                case lang::Tok::directive:
                {
                    // `#include "file"`, `#pragma panel ... "TEXT" ...`, a trailing comment.
                    const std::string_view d = t.text;
                    std::size_t k = 1;
                    while (k < d.size() && (d[k] == ' ' || d[k] == '\t'))
                        ++k;
                    while (k < d.size() && lang::isIdentChar(d[k]))
                        ++k;
                    span(t.col, t.col + static_cast<int>(k), csDirective);
                    const bool pragma = d.find("pragma") != std::string_view::npos && d.find("pragma") < k;
                    bool firstWord = true;
                    while (k < d.size())
                    {
                        const char c = d[k];
                        if (c == '"')
                        {
                            std::size_t e = d.find('"', k + 1);
                            e = e == std::string_view::npos ? d.size() : e + 1;
                            span(t.col + static_cast<int>(k), t.col + static_cast<int>(e), csString);
                            k = e;
                        }
                        else if (c == '/' && k + 1 < d.size() && d[k + 1] == '/')
                        {
                            span(t.col + static_cast<int>(k), t.col + static_cast<int>(d.size()), csComment);
                            k = d.size();
                        }
                        else if (lang::isIdentChar(c) || c == '-' || c == '.')
                        {
                            std::size_t e = k;
                            while (e < d.size() && (lang::isIdentChar(d[e]) || d[e] == '-' || d[e] == '.'))
                                ++e;
                            const bool number = std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '.';
                            span(t.col + static_cast<int>(k), t.col + static_cast<int>(e),
                                 number ? csNumber : (pragma && firstWord ? csDirective : csMember));
                            firstWord = false;
                            k = e;
                        }
                        else
                            ++k;
                    }
                    continue;
                }
            }
            if (attr)
                st = t.kind == lang::Tok::comment ? csComment : csAttribute;
            span(t.col, t.col + t.len, st);
            if (attr && t.text == "]" && prev != nullptr && prev->text == "]")
                attr = false;
        }
        return inComment ? 1u : 0u;
    }

    std::vector<funkgui::text::HighlightStyle> codePalette(const funkgui::Theme& th)
    {
        using Ink = funkgui::text::HighlightStyle::Ink;
        const bool dark = th.ground.r < 128;
        std::vector<funkgui::text::HighlightStyle> p(csCount);
        p[csPlain] = ink(Ink::ink100);
        p[csKeyword] = ink(Ink::accent, 0.02f);
        p[csType] = custom(codeSecond(th));              // the product's second colour (Theme.h), not an error's
        p[csComment] = dark ? custom({ 0x76, 0x8E, 0x6F, 255 }) : custom({ 0x5B, 0x7A, 0x55, 255 });
        p[csNumber] = dark ? custom({ 0xB5, 0xCE, 0xA8, 255 }) : custom({ 0x2E, 0x7D, 0x32, 255 });
        p[csOperator] = ink(Ink::ink70);
        p[csDirective] = dark ? custom({ 0xC5, 0x86, 0xC0, 255 }) : custom({ 0x9A, 0x3E, 0xA8, 255 });
        p[csString] = dark ? custom({ 0xCE, 0x91, 0x78, 255 }) : custom({ 0xA0, 0x45, 0x1F, 255 });
        p[csFunction] = dark ? custom({ 0xDC, 0xDC, 0xAA, 255 }) : custom({ 0x79, 0x5E, 0x26, 255 });
        p[csMember] = dark ? custom({ 0x9C, 0xDC, 0xFE, 255 }) : custom({ 0x0B, 0x53, 0x94, 255 });
        p[csAttribute] = ink(Ink::ink52);
        p[csPunct] = ink(Ink::ink70);
        return p;
    }

    // ---- the assistant ----------------------------------------------------------------------------------------------
    CodeAssist::CodeAssist(Tabs tabs) : tabs_(std::move(tabs)) {}
    CodeAssist::~CodeAssist() = default;

    funkgui::TextEditor& CodeAssist::ed() const { return tabs_.editor(tabs_.active()); }
    int CodeAssist::activeTab() const { return tabs_.active(); }
    bool CodeAssist::fmFile(int tab) const { return tab >= 0 && tab < tabs_.count() && fmName(tabs_.name(tab)); }
    bool CodeAssist::viewShown() const { return activeTab() >= tabs_.count(); }

    void CodeAssist::configure(funkgui::TextEditor& e, std::string_view fileName)
    {
        funkgui::TextEditorOptions o = e.options();
        o.indentGuides = true;
        o.matchBrackets = true;
        o.autoClosePairs = true;
        o.braceIndent = true;
        o.minimap = true;
        o.minimapWidth = 72.0f;
        o.markColumn = true;
        o.highlightLine = true;
        o.inks.info = custom(note_);                     // a note's squiggle and ring: Theme.h's codeSecond, not signal
        e.setOptions(o);
        if (fmName(fileName))
        {
            e.setHighlighter(std::make_unique<CodeHighlighter>(types_));
            if (!palette_.empty())
                e.setPalette(palette_);
            else
                e.setPalette(codePalette(funkgui::Theme::graphite()));
        }
        else
        {
            e.setHighlighter(std::make_unique<funkgui::text::KeywordHighlighter>(guiRules()));
            e.setPalette(funkgui::TextEditor::defaultPalette());
        }
    }

    void CodeAssist::setTheme(const funkgui::Theme& th)
    {
        const bool dark = th.ground.r < 128;
        if (hasTheme_ && dark == themeDark_)
            return;
        hasTheme_ = true;
        themeDark_ = dark;
        palette_ = codePalette(th);
        note_ = codeSecond(th);
        for (int i = 0; i < tabs_.count(); ++i)
        {
            funkgui::TextEditor& e = tabs_.editor(i);
            funkgui::TextEditorOptions o = e.options();
            o.inks.info = custom(note_);
            e.setOptions(o);
            if (fmFile(i))
                e.setPalette(palette_);
        }
    }

    void CodeAssist::setLibrary(const Sources& library)
    {
        library_ = library;
        service_.setLibrary(library);
    }

    void CodeAssist::setInstrument(bool on)
    {
        lang::Options o;
        o.instrument = on;
        service_.setOptions(o);
    }

    void CodeAssist::attach(funkgui::HostServices* host) { host_ = host; }

    void CodeAssist::setArea(const funkgui::Rect& area) { area_ = area; }
    funkgui::Rect CodeAssist::editorRect() const { return { area_.x, area_.y, area_.w, std::max(0.0f, area_.h - kStatusH) }; }
    funkgui::Rect CodeAssist::statusRect() const { return { area_.x, area_.bottom() - kStatusH, area_.w, kStatusH }; }

    uint64_t CodeAssist::filesRevision() const
    {
        uint64_t h = 1469598103934665603ull;
        const auto mixIn = [&](uint64_t v) {
            h ^= v;
            h *= 1099511628211ull;
        };
        for (int i = 0; i < tabs_.count(); ++i)
        {
            mixIn(tabs_.editor(i).revision());
            for (const char c : tabs_.name(i))
                mixIn(static_cast<unsigned char>(c));
            mixIn(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&tabs_.editor(i))));
        }
        mixIn(static_cast<uint64_t>(tabs_.count()));
        return h;
    }

    void CodeAssist::sync()
    {
        const uint64_t r = filesRevision();
        if (r == synced_)
            return;
        synced_ = r;
        Sources files;
        for (int i = 0; i < tabs_.count(); ++i)
            files.push_back({ tabs_.name(i), tabs_.editor(i).text() });
        service_.setFiles(files);
        types_->names = service_.typeNames();
    }

    lang::Service& CodeAssist::service()
    {
        sync();
        return service_;
    }

    // ---- keys -------------------------------------------------------------------------------------------------------
    bool CodeAssist::key(const funkgui::KeyEvent& e)
    {
        if (gotoOpen_)
            return goToKey(e);
        if (findFocus_)
            return findKey(e);
        funkgui::TextEditor& E = ed();
        if (!E.focused())
            return E.key(e);
        const bool cmd = e.mods.cmd;
        hover_.reset();
        // Ctrl-G: go to a line, in any tab.
        if (e.key == funkgui::Key::character && e.mods.ctrl && !cmd && !e.mods.alt && (e.ch == U'g' || e.ch == U'G'))
        {
            openGoTo();
            return true;
        }
        // A read-only text (the IR view) refuses every edit, and takes the key, which is never the host's.
        if (E.options().readOnly && editKey(e))
        {
            tabs_.notice(kReadOnlyNotice);
            return true;
        }
        if (completion_)
        {
            const int n = static_cast<int>(completion_->items.size());
            switch (e.key)
            {
                case funkgui::Key::up:
                    choice_ = choice_ <= 0 ? n - 1 : choice_ - 1;
                    return true;
                case funkgui::Key::down:
                    choice_ = choice_ >= n - 1 ? 0 : choice_ + 1;
                    return true;
                case funkgui::Key::pageUp:
                    choice_ = std::max(0, choice_ - kRows);
                    return true;
                case funkgui::Key::pageDown:
                    choice_ = std::min(n - 1, choice_ + kRows);
                    return true;
                case funkgui::Key::enter:
                case funkgui::Key::tab:
                    if (!cmd && !e.mods.shift && !e.mods.ctrl)
                    {
                        accept();
                        return true;
                    }
                    break;
                case funkgui::Key::escape:
                    completion_.reset();
                    return true;
                case funkgui::Key::character:
                case funkgui::Key::left:
                case funkgui::Key::right:
                case funkgui::Key::home:
                case funkgui::Key::end:
                case funkgui::Key::backspace:
                case funkgui::Key::del:
                case funkgui::Key::space:
                    break;
            }
            if (choice_ < scrollRow_)
                scrollRow_ = choice_;
        }
        if (e.key == funkgui::Key::escape)
        {
            if (signature_)
            {
                signature_.reset();
                return true;
            }
            if (findOpen_)
            {
                closeFind();
                return true;
            }
            return false;                                // the Panel's: the caret leaves the text
        }
        if (e.key == funkgui::Key::space && e.mods.ctrl && !cmd)
        {
            openCompletion(true);
            return true;
        }
        if (e.key == funkgui::Key::space && cmd && e.mods.shift)
        {
            openSignature();
            return true;
        }
        if (e.key == funkgui::Key::character)
        {
            const char32_t c = e.ch;
            const bool f = c == 'f' || c == 'F' || c == U'ƒ' || c == U'Ï';   // Alt-F, Shift-Alt-F on a Mac
            const auto is = [&](char k) { return c == static_cast<char32_t>(k) || c == static_cast<char32_t>(std::toupper(k)); };
            if (e.mods.alt && e.mods.shift && !cmd && f)
            {
                formatFile();
                return true;
            }
            if (cmd && e.mods.alt && f)
            {
                openFind(true);
                return true;
            }
            if (cmd && !e.mods.alt && is('f'))
            {
                openFind(false);
                return true;
            }
            if (cmd && is('g'))
            {
                findNext(!e.mods.shift);
                return true;
            }
            if (cmd && c == '/')
            {
                toggleComment();
                return true;
            }
            if (cmd && !e.mods.shift && is('b'))
            {
                goToDefinition();
                return true;
            }
            if (cmd && e.mods.shift && is('k'))
            {
                deleteLines();
                return true;
            }
            if (cmd && !e.mods.shift && is('l'))
            {
                int first = 0, last = 0;
                selectedLines(first, last);
                const TextPos to = last + 1 < E.lineCount() ? TextPos{ last + 1, 0 } : E.endPos();
                E.select({ E.hasSelection() ? std::min(E.selectionStart().line, first) : first, 0 }, to);
                return true;
            }
            if (cmd && !e.mods.shift && is('d'))
            {
                if (!E.hasSelection())
                {
                    const funkgui::TextRange w = E.wordRangeAt(E.caret());
                    if (!w.empty())
                        E.select(w.from, w.to);
                }
                else
                {
                    const std::string s = E.selectedText();
                    if (s.find('\n') == std::string::npos)
                        E.find(s, true, true, true);
                }
                E.scrollToCaret();
                return true;
            }
            if (cmd && (c == ']' || c == '['))
            {
                int first = 0, last = 0;
                selectedLines(first, last);
                std::vector<funkgui::TextEdit> edits;
                for (int l = first; l <= last; ++l)
                {
                    const std::string& text = E.line(l);
                    if (c == ']')
                    {
                        if (!text.empty())
                            edits.push_back({ { l, 0 }, { l, 0 }, "    " });
                    }
                    else
                    {
                        int k = 0;
                        while (k < 4 && k < static_cast<int>(text.size()) && text[static_cast<std::size_t>(k)] == ' ')
                            ++k;
                        if (k > 0)
                            edits.push_back({ { l, 0 }, { l, k }, "" });
                    }
                }
                if (!edits.empty())
                    E.applyEdits(std::move(edits));
                return true;
            }
        }
        if ((e.key == funkgui::Key::up || e.key == funkgui::Key::down) && e.mods.alt && !cmd)
        {
            const bool down = e.key == funkgui::Key::down;
            if (e.mods.shift)
                copyLines(down);
            else
                moveLines(down);
            return true;
        }
        const uint32_t before = E.revision();
        const TextPos caretBefore = E.caret();
        const bool used = E.key(e);
        afterEdit(e, before, caretBefore);
        return used;
    }

    void CodeAssist::afterEdit(const funkgui::KeyEvent& e, uint32_t before, TextPos caretBefore)
    {
        funkgui::TextEditor& E = ed();
        const bool fm = fmFile(activeTab());
        if (E.revision() == before)
        {
            // A move: the list closes when the caret leaves its word; the signature follows the caret.
            if (completion_ && (E.caret().line != completion_->replace.from.line || E.caret().col < completion_->replace.from.col
                                || E.caret() != caretBefore))
                refreshCompletion();
            if (signature_)
                openSignature();
            return;
        }
        if (!fm)
            return;
        const bool typed = e.key == funkgui::Key::character && !e.mods.cmd && !e.mods.ctrl && e.ch < 128;
        const char c = typed ? static_cast<char>(e.ch) : '\0';
        const std::string& line = E.line(E.caret().line);
        const int col = E.caret().col;
        const char before1 = col >= 2 ? line[static_cast<std::size_t>(col - 2)] : '\0';
        if (completion_)
            refreshCompletion();
        else if (typed && (identStart(c) || c == '.' || (c == '>' && before1 == '-')))
        {
            // The first letter of a word (not a number's), or a member's dot.
            bool word = true;
            if (identStart(c))
            {
                int a = col - 1;
                while (a > 0 && lang::isIdentChar(line[static_cast<std::size_t>(a - 1)]))
                    --a;
                word = a >= 0 && !std::isdigit(static_cast<unsigned char>(line[static_cast<std::size_t>(a)]));
            }
            if (c == '.' && col >= 2 && std::isdigit(static_cast<unsigned char>(before1)))
                word = false;
            if (word)
                openCompletion(false);
        }
        else if (e.key == funkgui::Key::backspace && completion_)
            refreshCompletion();
        if (typed && (c == '(' || c == ','))
            openSignature();
        else if (signature_)
            openSignature();
    }

    void CodeAssist::openCompletion(bool explicitly)
    {
        funkgui::TextEditor& E = ed();
        if (!fmFile(activeTab()) || E.hasSelection())
            return;
        sync();
        lang::CompletionList list = service_.complete(activeTab(), pos(E.caret()));
        explicit_ = explicitly;
        if (list.items.empty() || (!explicitly && list.prefix.empty() && !list.member))
        {
            completion_.reset();
            return;
        }
        // Typed whole, a word needs no list (Return then breaks the line).
        if (!explicitly && list.items.front().label == list.prefix && (list.items.size() == 1 || list.items[1].label.rfind(list.prefix, 0) != 0))
        {
            completion_.reset();
            return;
        }
        const std::string keep = completion_ && choice_ < static_cast<int>(completion_->items.size())
                                   ? completion_->items[static_cast<std::size_t>(choice_)].label
                                   : std::string();
        completion_ = std::move(list);
        choice_ = 0;
        scrollRow_ = 0;
        if (!keep.empty())
            for (std::size_t i = 0; i < completion_->items.size(); ++i)
                if (completion_->items[i].label == keep && completion_->items[i].rank < 100)
                {
                    choice_ = static_cast<int>(i);
                    break;
                }
    }

    void CodeAssist::refreshCompletion()
    {
        if (!completion_)
            return;
        const lang::Pos from = completion_->replace.from;
        const funkgui::TextPos c = ed().caret();
        if (c.line != from.line || c.col < from.col)
        {
            completion_.reset();
            return;
        }
        openCompletion(explicit_);
        if (completion_ && !(completion_->replace.from == from))
            completion_.reset();
    }

    void CodeAssist::accept()
    {
        if (!completion_ || completion_->items.empty())
            return;
        funkgui::TextEditor& E = ed();
        const lang::Completion item = completion_->items[static_cast<std::size_t>(std::clamp(choice_, 0, static_cast<int>(completion_->items.size()) - 1))];
        const TextPos from{ completion_->replace.from.line, completion_->replace.from.col };
        TextPos to = E.caret();
        // A file name in #include's quotes replaces the whole name.
        if (item.kind == lang::Kind::file)
            to = { completion_->replace.to.line, std::max(to.col, completion_->replace.to.col) };
        completion_.reset();
        // The text, its later lines indented as the line it goes on; the caret where "$0" was.
        std::string text = item.insert;
        const std::string indent = E.line(from.line).substr(0, static_cast<std::size_t>(lineIndent(E.line(from.line))));
        std::string out;
        for (const char ch : text)
        {
            out += ch;
            if (ch == '\n')
                out += indent;
        }
        // A call's parenthesis is typed, as if by hand: the editor closes it and steps over the closer.
        const bool call = out.size() > 4 && out.compare(out.size() - 4, 4, "($0)") == 0;
        if (call)
            out.erase(out.size() - 4);
        const std::size_t mark = out.find("$0");
        if (mark != std::string::npos)
            out.erase(mark, 2);
        std::vector<funkgui::TextEdit> edits;
        int shift = 0;
        if (!item.include.empty())
        {
            // The library file as a tab (when there is none), and its #include in this file.
            bool tab = false;
            for (int i = 0; i < tabs_.count(); ++i)
                tab = tab || tabs_.name(i) == item.include;
            if (!tab)
                for (const SourceFile& f : library_)
                    if (f.name == item.include && tabs_.addQuiet)
                        tabs_.addQuiet(f.name, f.text);
            const std::string line = "#include \"" + item.include + "\"";
            if (E.text().find(line) == std::string::npos)
            {
                // After the last #include before the first code, else after the comment lines the file starts with.
                int at = 0, lastInclude = -1, comments = 0;
                bool leading = true;
                for (int l = 0; l < E.lineCount(); ++l)
                {
                    const std::string& s = E.line(l);
                    const std::string t = s.substr(static_cast<std::size_t>(lineIndent(s)));
                    const bool comment = t.rfind("//", 0) == 0;
                    if (t.rfind("#include", 0) == 0)
                        lastInclude = l;
                    else if (!t.empty() && !comment && t[0] != '#')
                        break;
                    if (leading && comment)
                        comments = l + 1;
                    leading = leading && comment;
                }
                at = lastInclude >= 0 ? lastInclude + 1 : comments;
                if (at <= from.line)
                {
                    edits.push_back({ { at, 0 }, { at, 0 }, line + "\n" });
                    shift = 1;
                }
            }
        }
        edits.push_back({ from, to, out });
        if (!E.applyEdits(std::move(edits)) && out.empty())
            return;
        const TextPos start{ from.line + shift, from.col };
        E.setCaret(mark != std::string::npos ? after(start, std::string_view(out).substr(0, mark)) : after(start, out));
        if (call)
        {
            funkgui::KeyEvent open;
            open.key = funkgui::Key::character;
            open.ch = U'(';
            E.key(open);
        }
        E.scrollToCaret();
        if (call)
            openSignature();
        if (!item.include.empty())
            tabs_.notice("ADDED " + item.include);
    }

    void CodeAssist::openSignature()
    {
        if (!fmFile(activeTab()))
            return;
        sync();
        signature_ = service_.signature(activeTab(), pos(ed().caret()));
        signatureAt_ = ed().caret();
    }

    void CodeAssist::closeOverlays()
    {
        completion_.reset();
        signature_.reset();
        hover_.reset();
    }

    void CodeAssist::tabShown()
    {
        closeOverlays();
        gotoOpen_ = false;
        if (ed().options().readOnly)
        {
            replaceOpen_ = false;                        // nothing to replace in a read-only text (openFind's rule)
            findField_ = 0;
        }
        if (findOpen_)
            updateMatches();
    }

    void CodeAssist::focusChanged(bool focused)
    {
        focused_ = focused;
        if (!focused)
        {
            completion_.reset();
            signature_.reset();
            findFocus_ = false;
            gotoOpen_ = false;
        }
    }

    // ---- pointer ----------------------------------------------------------------------------------------------------
    bool CodeAssist::over(funkgui::Point p) const
    {
        if (statusRect().contains(p))
            return true;
        if (findOpen_ && findBarRect().contains(p))
            return true;
        if (gotoOpen_ && goToBarRect().contains(p))
            return true;
        if (completion_ && popupGeometry(panelW_, panelH_).box.contains(p))
            return true;
        return false;
    }

    std::optional<funkgui::Cursor> CodeAssist::cursorAt(funkgui::Point p) const
    {
        if (statusRect().contains(p))
        {
            const StatusParts s = statusParts();
            return s.formatOnRun.contains(p) || s.position.contains(p) || (s.problems.contains(p) && !viewShown())
                       ? funkgui::Cursor::pointingHand
                       : funkgui::Cursor::normal;
        }
        if (gotoOpen_ && goToBarRect().contains(p))
            return funkgui::Cursor::normal;
        if (findOpen_ && findBarRect().contains(p))
            return funkgui::Cursor::pointingHand;
        if (completion_ && popupGeometry(panelW_, panelH_).box.contains(p))
            return funkgui::Cursor::pointingHand;
        return std::nullopt;
    }

    bool CodeAssist::pointerDown(const funkgui::PointerEvent& e)
    {
        const funkgui::Point p{ e.x, e.y };
        hover_.reset();
        if (completion_)
        {
            const Popup g = popupGeometry(panelW_, panelH_);
            if (g.list.contains(p))
            {
                const int row = scrollRow_ + static_cast<int>((p.y - g.list.y) / kRowH);
                if (row >= 0 && row < static_cast<int>(completion_->items.size()))
                {
                    choice_ = row;
                    accept();
                }
                return true;
            }
            if (g.box.contains(p))
                return true;
            completion_.reset();
        }
        signature_.reset();
        if (gotoOpen_)
        {
            if (goToBarRect().contains(p))
                return true;                             // the field keeps the keys
            closeGoTo();
        }
        if (statusRect().contains(p))
        {
            const StatusParts s = statusParts();
            if (s.formatOnRun.contains(p))
                setFormatOnRun(!formatOnRun());
            else if (s.position.contains(p))
                openGoTo();
            else if (s.problems.contains(p) && !viewShown())
            {
                // The next problem after the caret, in the tab shown, else the first in another.
                funkgui::TextEditor& E = ed();
                const funkgui::TextPos c = E.caret();
                const funkgui::Decoration* best = nullptr;
                const funkgui::Decoration* first = nullptr;
                for (const funkgui::Decoration& d : E.decorations())
                {
                    if (d.kind != funkgui::DecorationKind::error && d.kind != funkgui::DecorationKind::warning)
                        continue;
                    if (first == nullptr)
                        first = &d;
                    if (best == nullptr && c < d.from)
                        best = &d;
                }
                if (best == nullptr)
                    best = first;
                if (best != nullptr)
                {
                    E.setCaret(best->from);
                    E.scrollToCaret();
                }
                else
                    for (int i = 0; i < tabs_.count(); ++i)
                        for (const funkgui::Decoration& d : tabs_.editor(i).decorations())
                            if (i != activeTab() && (d.kind == funkgui::DecorationKind::error || d.kind == funkgui::DecorationKind::warning))
                            {
                                tabs_.show(i);
                                tabs_.editor(i).setCaret(d.from);
                                tabs_.editor(i).scrollToCaret();
                                return true;
                            }
            }
            return true;
        }
        if (findOpen_ && findBarRect().contains(p))
        {
            const FindParts f = findParts();
            findFocus_ = true;
            ed().setFocused(false);
            if (f.toggle.contains(p))
                replaceOpen_ = !replaceOpen_ && !ed().options().readOnly;
            else if (f.caseBtn.contains(p))
            {
                matchCase_ = !matchCase_;
                updateMatches();
            }
            else if (f.wordBtn.contains(p))
            {
                wholeWord_ = !wholeWord_;
                updateMatches();
            }
            else if (f.prev.contains(p))
                findNext(false);
            else if (f.next.contains(p))
                findNext(true);
            else if (f.close.contains(p))
                closeFind();
            else if (replaceOpen_ && f.replaceField.contains(p))
                findField_ = 1;
            else if (replaceOpen_ && f.replaceBtn.contains(p))
                replaceOne();
            else if (replaceOpen_ && f.allBtn.contains(p))
                replaceAll();
            else
                findField_ = 0;
            return true;
        }
        findFocus_ = false;
        if (e.mods.cmd && !e.popup && ed().contains(p) && goToDefinitionAt(p))
            return true;
        return false;
    }

    bool CodeAssist::wheel(const funkgui::WheelEvent& e)
    {
        hover_.reset();                                  // the text moves under it
        still_ = 0.0f;
        if (!completion_)
            return false;
        const Popup g = popupGeometry(panelW_, panelH_);
        if (!g.box.contains({ e.x, e.y }))
            return false;
        const int n = static_cast<int>(completion_->items.size());
        const float dy = e.reversed ? -e.dy : e.dy;
        const int step = dy > 0.0f ? -1 : (dy < 0.0f ? 1 : 0);
        scrollRow_ = std::clamp(scrollRow_ + step * (e.smooth ? 1 : 3), 0, std::max(0, n - g.rows));
        return true;
    }

    // ---- time -------------------------------------------------------------------------------------------------------
    void CodeAssist::tick(float dt, funkgui::Point pointer, bool focused)
    {
        focused_ = focused;
        funkgui::TextEditor& E = ed();
        // The check: the files changed, and have rested.
        if (const uint64_t r = filesRevision(); r != editedRevision_)
        {
            editedRevision_ = r;
            sinceEdit_ = 0.0f;
            checkDue_ = true;
        }
        else
            sinceEdit_ += dt;
        // Find's matches follow the text.
        if (findOpen_ && (E.revision() != matchesRev_ || activeTab() != matchesTab_))
            updateMatches();
        // The hover card: the pointer still over a word.
        if (std::fabs(pointer.x - lastPointer_.x) > 0.5f || std::fabs(pointer.y - lastPointer_.y) > 0.5f)
        {
            lastPointer_ = pointer;
            still_ = 0.0f;
            if (hover_)
            {
                const Rect word{ E.pointOf({ hover_->range.from.line, hover_->range.from.col }).x - 2.0f,
                                 E.pointOf({ hover_->range.from.line, hover_->range.from.col }).y - 2.0f,
                                 E.pointOf({ hover_->range.to.line, hover_->range.to.col }).x
                                     - E.pointOf({ hover_->range.from.line, hover_->range.from.col }).x + 4.0f,
                                 E.rowHeight() + 4.0f };
                if (!word.contains(pointer))
                    hover_.reset();
            }
        }
        else if (still_ < kHoverDelay)
        {
            still_ += dt;
            if (still_ >= kHoverDelay && !hover_ && !completion_ && E.contains(pointer) && !over(pointer))
            {
                if (const std::optional<TextPos> at = E.posAt(pointer))
                {
                    const funkgui::TextRange w = E.wordRangeAt(*at);
                    const float x0 = E.pointOf(w.from).x, x1 = E.pointOf(w.to).x;
                    std::vector<std::string> problems = diagnosticsAt(activeTab(), *at);
                    std::optional<lang::Hover> h;
                    if (!w.empty() && pointer.x >= x0 && pointer.x <= x1 && fmFile(activeTab()))
                    {
                        sync();
                        h = service_.hover(activeTab(), pos(*at));
                    }
                    if (!h && !problems.empty())
                    {
                        lang::Hover only;
                        only.range = { pos(*at), pos(*at) };
                        h = only;
                    }
                    if (h)
                    {
                        hover_ = h;
                        hoverProblems_ = std::move(problems);
                        hoverPoint_ = pointer;
                    }
                }
            }
        }
        // The word at the caret's other places.
        if (E.caret() != lastCaret_ || E.revision() != occurrencesRev_)
        {
            if (E.caret() != lastCaret_)
            {
                // Off the highlighted word: they go at once.
                bool on = false;
                for (const funkgui::Decoration& d : E.decorations())
                    on = on || (d.kind == funkgui::DecorationKind::highlight && d.from <= E.caret() && E.caret() <= d.to);
                if (!on)
                    E.setDecorations({}, funkgui::decorationBit(funkgui::DecorationKind::highlight));
            }
            lastCaret_ = E.caret();
            occurrencesRev_ = E.revision();
            caretStill_ = 0.0f;
            occurrencesAt_ = { -1, -1 };
        }
        else if (occurrencesAt_.line < 0)
        {
            caretStill_ += dt;
            if (caretStill_ >= kOccurrenceDelay)
                updateOccurrences();
        }
    }

    bool CodeAssist::settled() const
    {
        if (checkDue_)
            return false;
        if (still_ < kHoverDelay && ed().contains(lastPointer_) && !hover_)
            return false;
        if (focused_ && occurrencesAt_.line < 0 && ed().focused())
            return false;
        return true;
    }

    bool CodeAssist::takeCheckRequest()
    {
        if (!checkDue_ || sinceEdit_ < kCheckDelay)
            return false;
        checkDue_ = false;
        return true;
    }

    void CodeAssist::cancelCheck()
    {
        editedRevision_ = filesRevision();
        checkDue_ = false;
    }

    void CodeAssist::updateOccurrences()
    {
        funkgui::TextEditor& E = ed();
        occurrencesAt_ = E.caret();
        std::vector<funkgui::Decoration> list;
        if (focused_ && E.focused() && !E.hasSelection() && fmFile(activeTab()))
        {
            sync();
            for (const lang::Range& r : service_.occurrences(activeTab(), pos(E.caret())))
                list.push_back({ { r.from.line, r.from.col }, { r.to.line, r.to.col }, funkgui::DecorationKind::highlight });
            if (list.size() < 2)
                list.clear();                            // the word alone: nothing to point at
        }
        E.setDecorations(std::move(list), funkgui::decorationBit(funkgui::DecorationKind::highlight));
    }

    // ---- problems ---------------------------------------------------------------------------------------------------
    void CodeAssist::setRunDiagnostics(const std::vector<Diagnostic>& messages, const Sources& compiled)
    {
        runMessages_ = messages;
        runSources_ = compiled;
        setDiagnostics(messages, compiled);
    }

    void CodeAssist::setCheckDiagnostics(const std::vector<Diagnostic>& messages, const Sources& compiled)
    {
        // The files RUN compiled (the editor opened on them, an edit undone, a check that waited behind RUN): RUN's
        // messages, which hold the check's and what only RUN finds (its second pass, the notes).
        if (compiled == runSources_)
            setDiagnostics(runMessages_, compiled);
        else
            setDiagnostics(messages, compiled);
    }

    void CodeAssist::setDiagnostics(const std::vector<Diagnostic>& messages, const Sources& compiled)
    {
        for (int i = 0; i < tabs_.count(); ++i)
        {
            const std::string name = tabs_.name(i);
            funkgui::TextEditor& E = tabs_.editor(i);
            const SourceFile* f = findFile(compiled, name);
            if (f == nullptr || funkgui::TextEditor::sanitise(f->text) != E.text())
                continue;                                // not what was compiled: the next check's
            DiagSet set;
            set.revision = E.revision();
            std::vector<funkgui::Decoration> decos;
            std::vector<funkgui::LineMark> marks;
            for (const Diagnostic& d : messages)
            {
                if (d.file != name || d.line > E.lineCount())
                    continue;
                // A message about the file without a place (a method the main module lacks): its first line.
                const int ln = std::max(1, d.line) - 1;
                const std::string& text = E.line(ln);
                int col = std::clamp(d.column - 1, 0, static_cast<int>(text.size()));
                // The token there: a word, a number, else one character (nothing at a line's end, or without a place).
                int end = col;
                if (col < static_cast<int>(text.size()) && d.line >= 1)
                {
                    bool inC = false;
                    std::vector<lang::Token> toks;
                    lang::lexLine(text, ln, inC, toks);
                    end = col + 1;
                    for (const lang::Token& t : toks)
                        if (t.col <= col && col < t.col + t.len && t.kind != lang::Tok::comment && t.kind != lang::Tok::directive)
                            end = t.col + t.len;
                        else if (t.kind == lang::Tok::directive && t.col <= col && col < t.col + t.len)
                        {
                            // An #include's file name.
                            std::string inc;
                            int a = 0, b = 0;
                            if (lang::includeOn(text, inc, a, b) && col >= a - 1 && col <= b)
                            {
                                col = a;
                                end = b;
                            }
                        }
                }
                const funkgui::DecorationKind kind = d.severity == Diagnostic::Severity::error     ? funkgui::DecorationKind::error
                                                   : d.severity == Diagnostic::Severity::warning ? funkgui::DecorationKind::warning
                                                                                                  : funkgui::DecorationKind::info;
                const funkgui::LineMark::Kind mark = d.severity == Diagnostic::Severity::error     ? funkgui::LineMark::Kind::error
                                                   : d.severity == Diagnostic::Severity::warning ? funkgui::LineMark::Kind::warning
                                                                                                  : funkgui::LineMark::Kind::info;
                decos.push_back({ { ln, col }, { ln, end }, kind });
                marks.push_back({ ln, mark });
                set.list.push_back({ { { ln, col }, { ln, end } }, kind, d.text });
            }
            // In the editor's order (it keeps decorations by `from`, stably), so that diagnosticsAt pairs a squiggle
            // with its message by index: a note may point before its error, and the plugin's own come after FMinus's.
            std::stable_sort(set.list.begin(), set.list.end(),
                             [](const Diag& a, const Diag& b) { return a.range.from < b.range.from; });
            E.setDecorations(std::move(decos), funkgui::kDiagnosticDecorations);
            E.setLineMarks(std::move(marks));
            diags_[name] = std::move(set);
        }
    }

    int CodeAssist::errorCount() const
    {
        int n = 0;
        for (int i = 0; i < tabs_.count(); ++i)
            for (const funkgui::Decoration& d : tabs_.editor(i).decorations())
                n += d.kind == funkgui::DecorationKind::error ? 1 : 0;
        return n;
    }

    int CodeAssist::warningCount() const
    {
        int n = 0;
        for (int i = 0; i < tabs_.count(); ++i)
            for (const funkgui::Decoration& d : tabs_.editor(i).decorations())
                n += d.kind == funkgui::DecorationKind::warning ? 1 : 0;
        return n;
    }

    std::vector<std::string> CodeAssist::diagnosticsAt(int tab, TextPos p) const
    {
        std::vector<std::string> out;
        if (tab < 0 || tab >= tabs_.count())
            return out;                                  // a view (the IR): none of the program's files
        const funkgui::TextEditor& E = tabs_.editor(tab);
        const auto it = diags_.find(tabs_.name(tab));
        if (it == diags_.end())
            return out;
        const DiagSet& set = it->second;
        std::vector<const funkgui::Decoration*> now;
        for (const funkgui::Decoration& d : E.decorations())
            if (d.kind == funkgui::DecorationKind::error || d.kind == funkgui::DecorationKind::warning || d.kind == funkgui::DecorationKind::info)
                now.push_back(&d);
        for (std::size_t k = 0; k < now.size(); ++k)
        {
            const funkgui::Decoration& d = *now[k];
            const funkgui::TextPos to = d.from == d.to ? funkgui::TextPos{ d.to.line, d.to.col + 1 } : d.to;
            if (!(d.from <= p && p <= to))
                continue;
            // Its message: the same place while the text is as it was; else by order, when none went; else the nearest.
            const Diag* m = nullptr;
            if (set.revision == E.revision() || now.size() == set.list.size())
                m = k < set.list.size() ? &set.list[k] : nullptr;
            if (m == nullptr)
                for (const Diag& x : set.list)
                    if (m == nullptr || std::abs(x.range.from.line - d.from.line) < std::abs(m->range.from.line - d.from.line))
                        m = &x;
            if (m != nullptr)
                out.push_back((d.kind == funkgui::DecorationKind::error ? "error: " : d.kind == funkgui::DecorationKind::warning ? "warning: " : "note: ")
                              + m->text);
        }
        return out;
    }

    // ---- commands ---------------------------------------------------------------------------------------------------
    bool CodeAssist::formatOnRun() const
    {
        return funkgui::UiPreferences::get().getInt(kFormatOnRunKey, 0, 0, 1) != 0;
    }

    void CodeAssist::setFormatOnRun(bool on)
    {
        funkgui::UiPreferences::get().setInt(kFormatOnRunKey, on ? 1 : 0);
        tabs_.notice(on ? "FORMAT ON RUN: ON" : "FORMAT ON RUN: OFF");
    }

    bool CodeAssist::formatFile()
    {
        const int tab = activeTab();
        if (!fmFile(tab))
        {
            tabs_.notice(viewShown() ? kReadOnlyNotice : "FORMAT: FMINUS FILES ONLY");
            return false;
        }
        sync();
        funkgui::TextEditor& E = ed();
        const std::string old = E.text();
        const lang::Formatted r = lang::format(old, service_.typeNames());
        if (!r.ok)
        {
            tabs_.notice("FORMAT: " + upper(r.why));
            return false;
        }
        if (!r.changed)
            return true;
        const lang::Pos c = lang::mapFormatted(old, r.text, pos(E.caret()));
        const funkgui::Point scroll = E.scroll();
        E.replaceText(r.text);
        E.setCaret({ c.line, c.col });
        E.setScroll(scroll.x, scroll.y);
        E.scrollToCaret();
        completion_.reset();
        signature_.reset();
        return true;
    }

    int CodeAssist::formatAll()
    {
        sync();
        int changed = 0;
        for (int i = 0; i < tabs_.count(); ++i)
        {
            if (!fmFile(i))
                continue;
            funkgui::TextEditor& E = tabs_.editor(i);
            const std::string old = E.text();
            const lang::Formatted r = lang::format(old, service_.typeNames());
            if (!r.ok || !r.changed)
                continue;
            const lang::Pos c = lang::mapFormatted(old, r.text, pos(E.caret()));
            E.replaceText(r.text);
            E.setCaret({ c.line, c.col });
            ++changed;
        }
        return changed;
    }

    std::vector<int> CodeAssist::selectedLines(int& first, int& last) const
    {
        const funkgui::TextEditor& E = ed();
        first = E.selectionStart().line;
        last = E.selectionEnd().line;
        if (E.hasSelection() && last > first && E.selectionEnd().col == 0)
            --last;                                      // a selection that ends at a line's start leaves that line
        std::vector<int> out;
        for (int l = first; l <= last; ++l)
            out.push_back(l);
        return out;
    }

    bool CodeAssist::toggleComment()
    {
        if (!fmFile(activeTab()))
            return false;
        funkgui::TextEditor& E = ed();
        int first = 0, last = 0;
        selectedLines(first, last);
        bool all = true;
        int indent = 1 << 30;
        for (int l = first; l <= last; ++l)
        {
            const std::string& s = E.line(l);
            const int k = lineIndent(s);
            if (k == static_cast<int>(s.size()))
                continue;                                // blank
            indent = std::min(indent, k);
            all = all && s.compare(static_cast<std::size_t>(k), 2, "//") == 0;
        }
        if (indent == (1 << 30))
            return false;
        std::vector<funkgui::TextEdit> edits;
        for (int l = first; l <= last; ++l)
        {
            const std::string& s = E.line(l);
            const int k = lineIndent(s);
            if (k == static_cast<int>(s.size()))
                continue;
            if (all)
            {
                const int n = s.compare(static_cast<std::size_t>(k), 3, "// ") == 0 ? 3 : 2;
                edits.push_back({ { l, k }, { l, k + n }, "" });
            }
            else
                edits.push_back({ { l, indent }, { l, indent }, "// " });
        }
        return E.applyEdits(std::move(edits));
    }

    bool CodeAssist::moveLines(bool down)
    {
        funkgui::TextEditor& E = ed();
        int first = 0, last = 0;
        selectedLines(first, last);
        if ((down && last + 1 >= E.lineCount()) || (!down && first == 0))
            return false;
        const TextPos caret = E.caret(), anchor = E.anchor();
        std::string block;
        for (int l = first; l <= last; ++l)
            block += (l > first ? "\n" : "") + E.line(l);
        const int other = down ? last + 1 : first - 1;
        const std::string text = down ? E.line(other) + "\n" + block : block + "\n" + E.line(other);
        const int from = down ? first : other, to = down ? other : last;
        if (!E.replaceRange({ from, 0 }, { to, static_cast<int>(E.line(to).size()) }, text))
            return false;
        const int d = down ? 1 : -1;
        E.select({ anchor.line + d, anchor.col }, { caret.line + d, caret.col });
        E.scrollToCaret();
        return true;
    }

    bool CodeAssist::copyLines(bool down)
    {
        funkgui::TextEditor& E = ed();
        int first = 0, last = 0;
        selectedLines(first, last);
        const TextPos caret = E.caret(), anchor = E.anchor();
        std::string block;
        for (int l = first; l <= last; ++l)
            block += (l > first ? "\n" : "") + E.line(l);
        const int n = last - first + 1;
        if (!E.replaceRange({ last, static_cast<int>(E.line(last).size()) }, { last, static_cast<int>(E.line(last).size()) }, "\n" + block))
            return false;
        const int d = down ? n : 0;
        E.select({ anchor.line + d, anchor.col }, { caret.line + d, caret.col });
        E.scrollToCaret();
        return true;
    }

    bool CodeAssist::deleteLines()
    {
        funkgui::TextEditor& E = ed();
        int first = 0, last = 0;
        selectedLines(first, last);
        const int col = E.caret().col;
        bool ok = false;
        if (last + 1 < E.lineCount())
            ok = E.replaceRange({ first, 0 }, { last + 1, 0 }, "");
        else if (first > 0)
            ok = E.replaceRange({ first - 1, static_cast<int>(E.line(first - 1).size()) }, E.endPos(), "");
        else
            ok = E.replaceRange({ 0, 0 }, E.endPos(), "");
        const int l = std::min(first, E.lineCount() - 1);
        E.setCaret({ l, std::min(col, static_cast<int>(E.line(l).size())) });
        return ok;
    }

    bool CodeAssist::goToDefinition()
    {
        if (!fmFile(activeTab()))
            return false;
        sync();
        const std::optional<lang::Location> loc = service_.definition(activeTab(), pos(ed().caret()));
        if (!loc)
        {
            tabs_.notice("NO DEFINITION HERE");
            return false;
        }
        if (loc->file < 0 || loc->file >= tabs_.count())
        {
            tabs_.notice(upper(loc->fileName) + " IS THE PLUGIN'S OWN FILE: HOVER A NAME TO READ IT");
            return false;
        }
        if (loc->file != activeTab())
            tabs_.show(loc->file);
        funkgui::TextEditor& E = tabs_.editor(loc->file);
        E.setCaret({ loc->at.line, loc->at.col });
        E.scrollToCaret();
        closeOverlays();
        return true;
    }

    bool CodeAssist::goToDefinitionAt(funkgui::Point p)
    {
        const std::optional<TextPos> at = ed().posAt(p);
        if (!at || ed().wordRangeAt(*at).empty())
            return false;
        ed().setCaret(*at);
        goToDefinition();
        return true;
    }

    // ---- find and replace -------------------------------------------------------------------------------------------
    void CodeAssist::openFind(bool replace)
    {
        funkgui::TextEditor& E = ed();
        gotoOpen_ = false;
        findOpen_ = true;
        replaceOpen_ = !E.options().readOnly && (replaceOpen_ || replace);   // nothing to replace in a read-only text
        findFocus_ = true;
        findField_ = replace && !findEdit_.buffer.empty() && !E.hasSelection() ? 1 : 0;
        const std::string sel = E.selectedText();
        if (!sel.empty() && sel.find('\n') == std::string::npos)
            findEdit_.set(sel, kMaxFind);
        else
            findEdit_.set(findEdit_.buffer, kMaxFind);
        E.setFocused(false);
        completion_.reset();
        signature_.reset();
        updateMatches();
    }

    void CodeAssist::closeFind()
    {
        findOpen_ = false;
        findFocus_ = false;
        matches_.clear();
        current_ = -1;
        // Every tab's: one shown while the bar was open keeps its matches (one shown again with the bar open has its
        // own set again by updateMatches).
        for (int i = 0; i < tabs_.count(); ++i)
            tabs_.editor(i).setDecorations({}, funkgui::kFindDecorations);
        ed().setDecorations({}, funkgui::kFindDecorations);   // a view's (the IR's) too
        if (focused_)
            ed().setFocused(true);
    }

    void CodeAssist::setFindText(const std::string& text)
    {
        if (!findOpen_)
            openFind(false);
        findEdit_.set(text, kMaxFind);
        findEdit_.allSelected = false;
        updateMatches();
        if (!matches_.empty())
            findNext(true);
    }

    void CodeAssist::setReplaceText(const std::string& text)
    {
        replaceOpen_ = true;
        replaceEdit_.set(text, kMaxFind);
        replaceEdit_.allSelected = false;
    }

    void CodeAssist::updateMatches()
    {
        funkgui::TextEditor& E = ed();
        matchesRev_ = E.revision();
        matchesTab_ = activeTab();
        matches_.clear();
        const std::string& needle = findEdit_.buffer;
        matchesNeedle_ = needle;
        if (!needle.empty())
        {
            const auto low = [&](char c) { return matchCase_ ? c : static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
            for (int l = 0; l < E.lineCount() && static_cast<int>(matches_.size()) < kMaxMatches; ++l)
            {
                const std::string& s = E.line(l);
                for (std::size_t k = 0; k + needle.size() <= s.size(); ++k)
                {
                    bool same = true;
                    for (std::size_t j = 0; j < needle.size() && same; ++j)
                        same = low(s[k + j]) == low(needle[j]);
                    if (!same)
                        continue;
                    if (wholeWord_ && ((k > 0 && lang::isIdentChar(s[k - 1]))
                                       || (k + needle.size() < s.size() && lang::isIdentChar(s[k + needle.size()]))))
                        continue;
                    matches_.push_back({ { l, static_cast<int>(k) }, { l, static_cast<int>(k + needle.size()) } });
                    k += needle.size() - 1;
                }
            }
        }
        // The current one: the selection's, else none.
        current_ = -1;
        for (std::size_t k = 0; k < matches_.size(); ++k)
            if (E.hasSelection() && matches_[k].from == E.selectionStart() && matches_[k].to == E.selectionEnd())
                current_ = static_cast<int>(k);
        std::vector<funkgui::Decoration> decos;
        for (std::size_t k = 0; k < matches_.size(); ++k)
            decos.push_back({ matches_[k].from, matches_[k].to,
                              static_cast<int>(k) == current_ ? funkgui::DecorationKind::findCurrent : funkgui::DecorationKind::findMatch });
        E.setDecorations(std::move(decos), funkgui::kFindDecorations);
    }

    void CodeAssist::showMatch(int k)
    {
        if (k < 0 || k >= static_cast<int>(matches_.size()))
            return;
        funkgui::TextEditor& E = ed();
        E.select(matches_[static_cast<std::size_t>(k)].from, matches_[static_cast<std::size_t>(k)].to);
        E.scrollToCaret();
        updateMatches();
    }

    bool CodeAssist::findNext(bool forward)
    {
        if (findEdit_.buffer.empty())
        {
            if (!findOpen_)
                openFind(false);
            return false;
        }
        if (matchesRev_ != ed().revision() || matchesTab_ != activeTab() || matchesNeedle_ != findEdit_.buffer)
            updateMatches();
        if (matches_.empty())
            return false;
        funkgui::TextEditor& E = ed();
        const TextPos from = forward ? E.selectionEnd() : E.selectionStart();
        int k = -1;
        if (forward)
        {
            for (std::size_t i = 0; i < matches_.size() && k < 0; ++i)
                if (from <= matches_[i].from && !(E.hasSelection() && matches_[i].from == E.selectionStart()))
                    k = static_cast<int>(i);
            if (k < 0)
                k = 0;
        }
        else
        {
            for (int i = static_cast<int>(matches_.size()) - 1; i >= 0 && k < 0; --i)
                if (matches_[static_cast<std::size_t>(i)].to <= from && !(E.hasSelection() && matches_[static_cast<std::size_t>(i)].from == E.selectionStart()))
                    k = i;
            if (k < 0)
                k = static_cast<int>(matches_.size()) - 1;
        }
        showMatch(k);
        return true;
    }

    bool CodeAssist::replaceOne()
    {
        funkgui::TextEditor& E = ed();
        if (current_ < 0 || current_ >= static_cast<int>(matches_.size()))
            return findNext(true);
        const funkgui::TextRange m = matches_[static_cast<std::size_t>(current_)];
        if (!E.replaceRange(m.from, m.to, replaceEdit_.buffer))
            return false;
        updateMatches();
        findNext(true);
        return true;
    }

    int CodeAssist::replaceAll()
    {
        funkgui::TextEditor& E = ed();
        updateMatches();
        std::vector<funkgui::TextEdit> edits;
        for (const funkgui::TextRange& m : matches_)
            edits.push_back({ m.from, m.to, replaceEdit_.buffer });
        const int n = static_cast<int>(edits.size());
        if (n == 0 || !E.applyEdits(std::move(edits)))
            return 0;
        updateMatches();
        tabs_.notice("REPLACED " + std::to_string(n));
        return n;
    }

    bool CodeAssist::findKey(const funkgui::KeyEvent& e)
    {
        funkgui::text::LineEdit& f = findField_ == 0 ? findEdit_ : replaceEdit_;
        const char32_t c = e.ch;
        switch (e.key)
        {
            case funkgui::Key::escape:
                closeFind();
                return true;
            case funkgui::Key::enter:
                if (findField_ == 1)
                {
                    if (e.mods.alt)
                        replaceAll();                    // (Cmd-Return is RUN's)
                    else
                        replaceOne();
                }
                else
                    findNext(!e.mods.shift);
                return true;
            case funkgui::Key::tab:
                if (replaceOpen_)
                    findField_ ^= 1;
                return true;
            case funkgui::Key::up:
            case funkgui::Key::down:
                findNext(e.key == funkgui::Key::down);
                return true;
            case funkgui::Key::character:
                if (e.mods.cmd)
                {
                    if (c == 'v' || c == 'V')
                    {
                        if (host_ != nullptr)
                            f.paste(host_->clipboardText(), kMaxFind);
                    }
                    else if (c == 'g' || c == 'G')
                        findNext(!e.mods.shift);
                    else if ((c == 'f' || c == 'F' || c == U'ƒ') && e.mods.alt && !ed().options().readOnly)
                    {
                        replaceOpen_ = true;
                        findField_ = 1;
                    }
                    else if (c == 'f' || c == 'F' || c == 'a' || c == 'A')
                        f.allSelected = !f.buffer.empty();
                    if (findField_ == 0)
                        updateMatches();
                    return true;
                }
                break;
            case funkgui::Key::left:
            case funkgui::Key::right:
            case funkgui::Key::pageUp:
            case funkgui::Key::pageDown:
            case funkgui::Key::home:
            case funkgui::Key::end:
            case funkgui::Key::backspace:
            case funkgui::Key::del:
            case funkgui::Key::space:
                break;
        }
        const std::string before = f.buffer;
        f.key(e, kMaxFind);
        if (findField_ == 0 && f.buffer != before)
        {
            // Incremental: the first match from the selection's start.
            funkgui::TextEditor& E = ed();
            const TextPos from = E.selectionStart();
            E.setCaret(from);
            updateMatches();
            for (std::size_t k = 0; k < matches_.size(); ++k)
                if (from <= matches_[k].from)
                {
                    showMatch(static_cast<int>(k));
                    return true;
                }
            if (!matches_.empty())
                showMatch(0);
        }
        return true;
    }

    // ---- go to a line -----------------------------------------------------------------------------------------------
    void CodeAssist::openGoTo()
    {
        if (findOpen_)
            closeFind();
        completion_.reset();
        signature_.reset();
        hover_.reset();
        gotoOpen_ = true;
        gotoBad_ = false;
        gotoEdit_.set("", kMaxGoTo);
        ed().setFocused(false);                          // the field has the keys
    }

    void CodeAssist::closeGoTo()
    {
        if (!gotoOpen_)
            return;
        gotoOpen_ = false;
        gotoBad_ = false;
        if (focused_)
            ed().setFocused(true);
    }

    bool CodeAssist::goToLine(const std::string& typed)
    {
        funkgui::TextEditor& E = ed();
        long line = 0, col = 0;
        std::size_t k = 0;
        while (k < typed.size() && typed[k] == ' ')
            ++k;
        const std::size_t start = k;
        while (k < typed.size() && std::isdigit(static_cast<unsigned char>(typed[k])) != 0 && line < 100000000)
            line = line * 10 + (typed[k++] - '0');
        if (k == start || line < 1 || line > E.lineCount())
            return false;
        if (k < typed.size() && typed[k] == ':')
        {
            ++k;
            while (k < typed.size() && std::isdigit(static_cast<unsigned char>(typed[k])) != 0 && col < 100000000)
                col = col * 10 + (typed[k++] - '0');
        }
        while (k < typed.size() && typed[k] == ' ')
            ++k;
        if (k != typed.size())
            return false;
        E.setCaret({ static_cast<int>(line - 1), col > 0 ? static_cast<int>(col - 1) : 0 });   // clamped to the line
        // The line two fifths down the text, where the eye looks for it.
        const float y = static_cast<float>(E.rowOf(E.caret())) * E.rowHeight() - 0.4f * E.textArea().h;
        E.setScroll(E.scroll().x, std::max(0.0f, y));
        E.scrollToCaret();
        return true;
    }

    bool CodeAssist::goToKey(const funkgui::KeyEvent& e)
    {
        switch (e.key)
        {
            case funkgui::Key::escape:
                closeGoTo();
                return true;
            case funkgui::Key::enter:
                if (goToLine(gotoEdit_.buffer))
                    closeGoTo();
                else
                    gotoBad_ = true;
                return true;
            case funkgui::Key::character:
                if (e.mods.cmd)
                {
                    if ((e.ch == U'v' || e.ch == U'V') && host_ != nullptr)
                        gotoEdit_.paste(host_->clipboardText(), kMaxGoTo);
                    else if (e.ch == U'a' || e.ch == U'A')
                        gotoEdit_.allSelected = !gotoEdit_.buffer.empty();
                    gotoBad_ = false;
                    return true;
                }
                if (e.mods.ctrl)
                {
                    if (e.ch == U'g' || e.ch == U'G')
                        closeGoTo();                     // Ctrl-G again
                    return true;
                }
                if (!(e.ch >= U'0' && e.ch <= U'9') && e.ch != U':')
                    return true;                         // a line's number only
                break;
            case funkgui::Key::tab:
            case funkgui::Key::up:
            case funkgui::Key::down:
            case funkgui::Key::pageUp:
            case funkgui::Key::pageDown:
            case funkgui::Key::space:
                return true;
            case funkgui::Key::left:
            case funkgui::Key::right:
            case funkgui::Key::home:
            case funkgui::Key::end:
            case funkgui::Key::backspace:
            case funkgui::Key::del:
                break;
        }
        gotoEdit_.key(e, kMaxGoTo);
        gotoBad_ = false;
        return true;
    }

    funkgui::Rect CodeAssist::goToBarRect() const
    {
        const Rect e = editorRect();
        const float w = std::min(kGoToW, e.w - 100.0f);
        const float minimap = 72.0f + 18.0f;
        return { e.right() - w - minimap, e.y + 4.0f, w, kFindRowH + 4.0f };
    }

    // ---- geometry ---------------------------------------------------------------------------------------------------
    CodeAssist::Popup CodeAssist::popupGeometry(float panelW, float panelH) const
    {
        Popup g;
        if (!completion_)
            return g;
        const funkgui::TextEditor& E = ed();
        const funkgui::Point at = E.pointOf({ completion_->replace.from.line, completion_->replace.from.col });
        const int n = static_cast<int>(completion_->items.size());
        g.rows = std::min(n, kRows);
        const float listH = static_cast<float>(g.rows) * kRowH;
        // The chosen item's detail: its signature (a line) and its documentation (up to four lines), or nothing.
        float detailH = 0.0f;
        if (choice_ >= 0 && choice_ < n)
        {
            const lang::Completion& it = completion_->items[static_cast<std::size_t>(choice_)];
            const std::size_t docLines = wrap(reflow(it.doc), chat::kSmall, kPopupW - 24.0f, it.detail.empty() ? 4 : 3).size();
            const bool hasDoc = !it.doc.empty();
            if (!it.detail.empty() || hasDoc)
                detailH = 12.0f + (it.detail.empty() ? 0.0f : 18.0f) + (hasDoc ? 14.0f * static_cast<float>(docLines) : 0.0f);
        }
        detailH = std::min(detailH, kDetailH);
        const float h = listH + detailH + 2.0f * 4.0f;
        float x = std::min(at.x - 30.0f, panelW - kPopupW - 8.0f);
        x = std::max(8.0f, x);
        float y = at.y + E.rowHeight() + 2.0f;
        if (y + h > panelH - 8.0f)
            y = std::max(8.0f, at.y - h - 2.0f);
        g.box = { x, y, kPopupW, h };
        g.list = { x + 4.0f, y + 4.0f, kPopupW - 8.0f, listH };
        g.detail = { x + 4.0f, y + 4.0f + listH, kPopupW - 8.0f, detailH };
        return g;
    }

    funkgui::Rect CodeAssist::findBarRect() const
    {
        const Rect e = editorRect();
        const float w = std::min(kFindW, e.w - 100.0f);
        const float h = replaceOpen_ ? 2.0f * kFindRowH + 6.0f : kFindRowH + 4.0f;
        const float minimap = 72.0f + 18.0f;
        return { e.right() - w - minimap, e.y + 4.0f, w, h };
    }

    CodeAssist::FindParts CodeAssist::findParts() const
    {
        FindParts f;
        f.bar = findBarRect();
        const float y = f.bar.y + 3.0f, h = kFindRowH - 4.0f;
        f.toggle = { f.bar.x + 3.0f, y, 18.0f, replaceOpen_ ? 2.0f * kFindRowH - 2.0f : h };
        const float right = f.bar.right() - 4.0f;
        f.close = { right - 22.0f, y + 2.0f, 22.0f, h - 4.0f };
        f.next = { f.close.x - 24.0f, y + 2.0f, 22.0f, h - 4.0f };
        f.prev = { f.next.x - 24.0f, y + 2.0f, 22.0f, h - 4.0f };
        f.count = { f.prev.x - 70.0f, y, 66.0f, h };
        f.wordBtn = { f.count.x - 26.0f, y + 3.0f, 24.0f, h - 6.0f };
        f.caseBtn = { f.wordBtn.x - 26.0f, y + 3.0f, 24.0f, h - 6.0f };
        f.field = { f.toggle.right() + 4.0f, y, std::max(40.0f, f.caseBtn.x - 4.0f - f.toggle.right() - 4.0f), h };
        const float y2 = y + kFindRowH;
        f.allBtn = { right - 40.0f, y2 + 2.0f, 40.0f, h - 4.0f };
        f.replaceBtn = { f.allBtn.x - 62.0f, y2 + 2.0f, 58.0f, h - 4.0f };
        f.replaceField = { f.field.x, y2, f.field.w, h };
        return f;
    }

    CodeAssist::StatusParts CodeAssist::statusParts() const
    {
        StatusParts s;
        const Rect r = statusRect();
        const funkgui::TextStyle& st = chat::kSmall;
        float x = r.right() - 8.0f;
        const auto take = [&](const std::string& text, float extra) {
            const float w = widthOf(text, st) + 16.0f + extra;
            x -= w;
            return Rect{ x, r.y, w, r.h };
        };
        s.formatOnRun = take("Format on Run", 14.0f);
        s.language = take(viewShown() ? "LLVM IR" : fmFile(activeTab()) ? "FMinus" : "GUI", 0.0f);
        s.indent = take(viewShown() ? "Spaces: 2" : "Spaces: 4", 0.0f);
        const funkgui::TextEditor& E = ed();
        char buf[64];
        std::snprintf(buf, sizeof buf, "Ln %d, Col %d", E.caret().line + 1, E.caret().col + 1);
        s.position = take(buf, E.hasSelection() ? widthOf(" (999 selected)", st) : 0.0f);
        s.problems = { r.x + 4.0f, r.y, std::max(0.0f, std::min(200.0f, s.position.x - r.x - 8.0f)), r.h };
        return s;
    }

    // ---- drawing ----------------------------------------------------------------------------------------------------
    void CodeAssist::drawGround(funkgui::Canvas& c, const funkgui::Theme& th) const
    {
        const Rect r = editorRect();
        const funkgui::Canvas::Scope s(c, kCodeTag, false);
        c.rrect(r.x, r.y, r.w, r.h + kStatusH, 3.0f, editorGround(th));
    }

    void CodeAssist::drawInEditor(funkgui::Canvas& c, const funkgui::Theme& th) const
    {
        const chat::Palette p = chat::palette(th);
        const bool dark = th.ground.r < 128;
        const funkgui::Canvas::Scope s(c, kCodeTag, false);
        const funkgui::TextStyle& st = chat::kSmall;
        // The status line.
        {
            const Rect r = statusRect();
            c.rrect(r.x, r.y, r.w, r.h, 0.0f, dark ? funkgui::mix(editorGround(th), th.ink100, 0.03f) : funkgui::mix(editorGround(th), th.ink100, 0.035f));
            c.hairlineH(r.x, r.y, r.w, th.ink16);
            const StatusParts parts = statusParts();
            const float mid = r.centreY();
            const float top = c.capCentreTop(mid, st);
            // The problems' count; a view's (the IR's) place says it is read only, and its size.
            const int errors = errorCount(), warnings = warningCount();
            float x = parts.problems.x + 6.0f;
            constexpr Col kRed{ 0xE5, 0x48, 0x4D, 0xFF }, kAmber{ 0xE2, 0xA3, 0x36, 0xFF };
            if (viewShown())
            {
                char lines[64];
                std::snprintf(lines, sizeof lines, "Read only   %d lines", ed().lineCount());
                chat::drawIcon(c, chat::Icon::chip, x + 5.0f, mid, 10.0f, p.faint);
                c.text(lines, x + 16.0f, top, st, p.faint);
                x += 16.0f + widthOf(lines, st) + 12.0f;
            }
            else if (errors == 0 && warnings == 0)
            {
                chat::drawIcon(c, chat::Icon::check, x + 5.0f, mid, 10.0f, p.faint);
                c.text("No problems", x + 14.0f, top, st, p.faint);
                x += 14.0f + widthOf("No problems", st) + 12.0f;
            }
            else
            {
                c.disc(x + 5.0f, mid, 5.0f, errors > 0 ? kRed : p.faint);
                c.segment(x + 3.0f, mid - 2.0f, x + 7.0f, mid + 2.0f, 1.2f, editorGround(th));
                c.segment(x + 3.0f, mid + 2.0f, x + 7.0f, mid - 2.0f, 1.2f, editorGround(th));
                const std::string e = std::to_string(errors);
                c.text(e.c_str(), x + 14.0f, top, st, errors > 0 ? p.text : p.faint);
                x += 14.0f + widthOf(e, st) + 10.0f;
                const float tx[4] = { x + 5.0f, x + 10.0f, x, x + 5.0f };
                const float ty[4] = { mid - 5.0f, mid + 4.0f, mid + 4.0f, mid - 5.0f };
                c.polyline(tx, ty, 4, 1.3f, warnings > 0 ? kAmber : p.faint);
                const std::string w = std::to_string(warnings);
                c.text(w.c_str(), x + 14.0f, top, st, warnings > 0 ? p.text : p.faint);
                x += 14.0f + widthOf(w, st) + 12.0f;
            }
            if (checking_)
                c.text("Checking...", x, top, st, p.faint);
            const funkgui::TextEditor& E = ed();
            char buf[96];
            if (E.hasSelection())
                std::snprintf(buf, sizeof buf, "Ln %d, Col %d (%zu selected)", E.caret().line + 1, E.caret().col + 1, E.selectedText().size());
            else
                std::snprintf(buf, sizeof buf, "Ln %d, Col %d", E.caret().line + 1, E.caret().col + 1);
            c.text(buf, parts.position.right() - 8.0f, top, st, p.soft, funkgui::Align::right);
            c.text(viewShown() ? "Spaces: 2" : "Spaces: 4", parts.indent.right() - 8.0f, top, st, p.faint, funkgui::Align::right);
            c.text(viewShown() ? "LLVM IR" : fmFile(activeTab()) ? "FMinus" : "GUI", parts.language.right() - 8.0f, top, st, p.faint,
                   funkgui::Align::right);
            const bool on = formatOnRun();
            const Rect box{ parts.formatOnRun.x + 6.0f, mid - 5.0f, 10.0f, 10.0f };
            c.rrect(box.x, box.y, box.w, box.h, 2.0f, on ? p.accent : p.card, 1.0f, on ? p.accent : p.dim);
            if (on)
                chat::drawIcon(c, chat::Icon::check, box.centreX(), box.centreY(), 9.0f, p.onAccent);
            c.text("Format on Run", parts.formatOnRun.right() - 8.0f, top, st, on ? p.text : p.faint, funkgui::Align::right);
        }
        // The find bar.
        if (findOpen_)
        {
            const FindParts f = findParts();
            c.rrect(f.bar.x + 1.0f, f.bar.y + 2.0f, f.bar.w, f.bar.h, 6.0f, Col{ 0, 0, 0, static_cast<uint8_t>(dark ? 90 : 30) });
            c.rrect(f.bar.x, f.bar.y, f.bar.w, f.bar.h, 6.0f, p.raised, 1.0f, p.composerEdge);
            chat::drawIcon(c, replaceOpen_ ? chat::Icon::chevronDown : chat::Icon::chevronRight, f.toggle.centreX(),
                           f.bar.y + 3.0f + (kFindRowH - 4.0f) * 0.5f, 10.0f, p.soft);
            const auto field = [&](const Rect& r, const funkgui::text::LineEdit& edit, bool active, const char* placeholder) {
                c.rrect(r.x, r.y + 2.0f, r.w, r.h - 4.0f, 4.0f, p.composer, 1.0f, active && findFocus_ ? p.accent : p.composerEdge);
                const float t = c.capCentreTop(r.centreY(), kCode);
                const funkgui::Canvas::ClipScope clip(c, { r.x + 2.0f, r.y, r.w - 4.0f, r.h });
                if (edit.buffer.empty())
                    c.text(placeholder, r.x + 6.0f, t, kCode, p.faint);
                else
                {
                    if (edit.allSelected)
                        c.rrect(r.x + 5.0f, r.y + 5.0f, std::min(widthOf(edit.buffer, kCode), r.w - 10.0f) + 2.0f, r.h - 10.0f, 2.0f, th.accentDim);
                    c.text(edit.buffer.c_str(), r.x + 6.0f, t, kCode, p.text);
                }
                if (active && findFocus_)
                {
                    const float cx = r.x + 6.0f + widthOf(std::string_view(edit.buffer).substr(0, static_cast<std::size_t>(std::clamp(edit.caret, 0, static_cast<int>(edit.buffer.size())))), kCode);
                    c.rrect(cx, r.y + 6.0f, 1.5f, r.h - 12.0f, 0.0f, p.accent);
                }
            };
            field(f.field, findEdit_, findField_ == 0, "Find");
            const auto toggle = [&](const Rect& r, const char* label, bool on) {
                if (on)
                    c.rrect(r.x, r.y, r.w, r.h, 4.0f, th.accentDim, 1.0f, p.accent);
                c.text(label, r.centreX(), c.capCentreTop(r.centreY(), st), st, on ? p.text : p.soft, funkgui::Align::centre);
            };
            toggle(f.caseBtn, "Aa", matchCase_);
            toggle(f.wordBtn, "ab", wholeWord_);
            char count[32];
            if (findEdit_.buffer.empty())
                count[0] = '\0';
            else if (matches_.empty())
                std::snprintf(count, sizeof count, "No results");
            else if (current_ >= 0)
                std::snprintf(count, sizeof count, "%d of %zu", current_ + 1, matches_.size());
            else
                std::snprintf(count, sizeof count, "%zu found", matches_.size());
            c.text(count, f.count.centreX(), c.capCentreTop(f.count.centreY(), st), st,
                   !findEdit_.buffer.empty() && matches_.empty() ? Col{ 0xE5, 0x48, 0x4D, 0xFF } : p.soft, funkgui::Align::centre);
            chat::drawIcon(c, chat::Icon::chevronUp, f.prev.centreX(), f.prev.centreY(), 10.0f, p.soft);
            chat::drawIcon(c, chat::Icon::chevronDown, f.next.centreX(), f.next.centreY(), 10.0f, p.soft);
            chat::drawIcon(c, chat::Icon::cross, f.close.centreX(), f.close.centreY(), 9.0f, p.soft);
            if (replaceOpen_)
            {
                field(f.replaceField, replaceEdit_, findField_ == 1, "Replace");
                c.rrect(f.replaceBtn.x, f.replaceBtn.y, f.replaceBtn.w, f.replaceBtn.h, 4.0f, p.hover);
                c.text("Replace", f.replaceBtn.centreX(), c.capCentreTop(f.replaceBtn.centreY(), st), st, p.text, funkgui::Align::centre);
                c.rrect(f.allBtn.x, f.allBtn.y, f.allBtn.w, f.allBtn.h, 4.0f, p.hover);
                c.text("All", f.allBtn.centreX(), c.capCentreTop(f.allBtn.centreY(), st), st, p.text, funkgui::Align::centre);
            }
        }
        // The go-to-line bar: one field, where the find bar sits.
        if (gotoOpen_)
        {
            const Rect b = goToBarRect();
            c.rrect(b.x + 1.0f, b.y + 2.0f, b.w, b.h, 6.0f, Col{ 0, 0, 0, static_cast<uint8_t>(dark ? 90 : 30) });
            c.rrect(b.x, b.y, b.w, b.h, 6.0f, p.raised, 1.0f, p.composerEdge);
            const Rect r{ b.x + 4.0f, b.y + 3.0f, b.w - 8.0f, kFindRowH - 4.0f };
            constexpr Col kRed{ 0xE5, 0x48, 0x4D, 0xFF };
            c.rrect(r.x, r.y + 2.0f, r.w, r.h - 4.0f, 4.0f, p.composer, 1.0f, gotoBad_ ? kRed : p.accent);
            const float t = c.capCentreTop(r.centreY(), kCode);
            const funkgui::Canvas::ClipScope clip(c, { r.x + 2.0f, r.y, r.w - 4.0f, r.h });
            const funkgui::text::LineEdit& edit = gotoEdit_;
            if (edit.buffer.empty())
            {
                char hint[96];
                std::snprintf(hint, sizeof hint, "Go to line 1 to %d (or line:column)", ed().lineCount());
                c.text(hint, r.x + 6.0f, t, kCode, p.faint);
            }
            else
            {
                if (edit.allSelected)
                    c.rrect(r.x + 5.0f, r.y + 5.0f, std::min(widthOf(edit.buffer, kCode), r.w - 10.0f) + 2.0f, r.h - 10.0f, 2.0f, th.accentDim);
                c.text(edit.buffer.c_str(), r.x + 6.0f, t, kCode, gotoBad_ ? kRed : p.text);
            }
            const float cx = r.x + 6.0f + widthOf(std::string_view(edit.buffer).substr(0, static_cast<std::size_t>(std::clamp(edit.caret, 0, static_cast<int>(edit.buffer.size())))), kCode);
            c.rrect(cx, r.y + 6.0f, 1.5f, r.h - 12.0f, 0.0f, p.accent);
        }
    }

    void CodeAssist::drawOverlays(funkgui::Canvas& c, const funkgui::Theme& th, float panelW, float panelH) const
    {
        panelW_ = panelW;
        panelH_ = panelH;
        const chat::Palette p = chat::palette(th);
        const bool dark = th.ground.r < 128;
        const funkgui::Canvas::Scope s(c, kCodePopupTag, false);
        const funkgui::TextEditor& E = ed();
        const auto card = [&](const Rect& r) {
            c.rrect(r.x + 1.0f, r.y + 3.0f, r.w, r.h, 7.0f, Col{ 0, 0, 0, static_cast<uint8_t>(dark ? 110 : 40) }, 0.0f, Col{}, 6.0f);
            c.rrect(r.x, r.y, r.w, r.h, 7.0f, p.raised, 1.0f, p.composerEdge);
        };
        const std::vector<funkgui::text::HighlightStyle>& pal = palette_.empty() ? codePalette(th) : palette_;
        // A line of code, highlighted as the editor does.
        const auto codeLine = [&](const std::string& text, float x, float top, float maxW, const funkgui::TextStyle& style) {
            CodeHighlighter hl(types_);
            std::vector<funkgui::text::Span> spans;
            hl.line(text, 0, spans);
            float pen = x;
            int at = 0;
            const auto run = [&](int a, int b, Col ink) {
                if (b <= a)
                    return;
                const std::string piece = text.substr(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a));
                if (pen - x + widthOf(piece, style) > maxW)
                    return;
                c.text(piece.c_str(), pen, top, style, ink);
                pen += widthOf(piece, style);
            };
            for (const funkgui::text::Span& sp : spans)
            {
                run(at, sp.start, th.ink100);
                run(sp.start, sp.end, sp.style < pal.size() ? pal[sp.style].resolve(th) : th.ink100);
                at = sp.end;
            }
            run(at, static_cast<int>(text.size()), th.ink100);
        };

        // The signature: above the caret's line.
        if (signature_ && E.focused())
        {
            const lang::Signature& sig = *signature_;
            const float lineH = 18.0f;
            std::vector<std::string> doc = wrap(reflow(sig.doc), chat::kSmall, kCardMaxW - 2.0f * kPad, 4);
            float inner = std::max(widthOf(sig.label, kCardCode), 120.0f);
            for (const std::string& l : doc)
                inner = std::max(inner, widthOf(l, chat::kSmall));
            const float w = std::min(kCardMaxW, inner + 2.0f * kPad);
            const float h = kPad * 2.0f + lineH + (doc.empty() ? 0.0f : 4.0f + 15.0f * static_cast<float>(doc.size()));
            const funkgui::Point at = E.pointOf(E.caret());
            float x = std::clamp(at.x - 20.0f, 8.0f, panelW - w - 8.0f);
            float y = at.y - h - 4.0f;
            if (y < 8.0f)
                y = at.y + E.rowHeight() + 4.0f + (completion_ ? popupGeometry(panelW, panelH).box.h + 4.0f : 0.0f);
            const Rect r{ x, y, w, h };
            card(r);
            // The label, the active parameter in the accent, underlined.
            const float top = c.capCentreTop(y + kPad + lineH * 0.5f, kCardCode);
            float pen = x + kPad;
            int from = 0;
            const auto piece = [&](int a, int b, Col ink) {
                if (b <= a)
                    return;
                const std::string t = sig.label.substr(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a));
                c.text(t.c_str(), pen, top, kCardCode, ink);
                pen += widthOf(t, kCardCode);
            };
            if (sig.active >= 0 && sig.active < static_cast<int>(sig.params.size()))
            {
                const auto [a, b] = sig.params[static_cast<std::size_t>(sig.active)];
                piece(from, a, p.soft);
                const float x0 = pen;
                const funkgui::TextStyle strong{ kCardCode.px, kCardCode.tracking, 0.06f, true };
                const std::string t = sig.label.substr(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a));
                c.text(t.c_str(), pen, top, strong, p.accent);
                pen += widthOf(t, kCardCode);
                c.segment(x0, y + kPad + lineH - 1.0f, pen, y + kPad + lineH - 1.0f, 1.0f, p.accent);
                from = b;
            }
            piece(from, static_cast<int>(sig.label.size()), p.soft);
            float dy = y + kPad + lineH + 4.0f;
            for (const std::string& l : doc)
            {
                c.text(l.c_str(), x + kPad, dy, chat::kSmall, p.dim);
                dy += 15.0f;
            }
        }

        // The completion list.
        if (completion_ && E.focused())
        {
            const Popup g = popupGeometry(panelW, panelH);
            card(g.box);
            const int n = static_cast<int>(completion_->items.size());
            int first = std::clamp(scrollRow_, 0, std::max(0, n - g.rows));
            if (choice_ < first)
                first = choice_;
            if (choice_ >= first + g.rows)
                first = choice_ - g.rows + 1;
            scrollRow_ = first;
            c.pushClip(g.list);
            for (int k = 0; k < g.rows && first + k < n; ++k)
            {
                const lang::Completion& it = completion_->items[static_cast<std::size_t>(first + k)];
                const float y = g.list.y + static_cast<float>(k) * kRowH;
                if (first + k == choice_)
                    c.rrect(g.list.x, y, g.list.w, kRowH, 4.0f, funkgui::mix(p.raised, th.accent, dark ? 0.22f : 0.16f));
                const KindLook look = kindLook(it.kind);
                const Col kc = dark ? look.dark : look.light;
                c.rrect(g.list.x + 6.0f, y + 4.0f, 14.0f, 14.0f, 3.0f, kc.withAlpha(0.16f), 1.0f, kc.withAlpha(0.7f));
                const char letter[2] = { look.letter, '\0' };
                c.text(letter, g.list.x + 13.0f, c.capCentreTop(y + 11.0f, chat::kSmallStrong), chat::kSmallStrong, kc, funkgui::Align::centre);
                // The label, the letters typed in the accent.
                const float top = c.capCentreTop(y + kRowH * 0.5f, kCode);
                float pen = g.list.x + 28.0f;
                const std::string& prefix = completion_->prefix;
                std::size_t k2 = 0;
                for (std::size_t j = 0; j < it.label.size(); ++j)
                {
                    const char ch = it.label[j];
                    const bool hit = k2 < prefix.size()
                                  && std::tolower(static_cast<unsigned char>(ch)) == std::tolower(static_cast<unsigned char>(prefix[k2]));
                    if (hit)
                        ++k2;
                    const char one[2] = { ch, '\0' };
                    c.text(one, pen, top, kCode, hit ? p.accent : p.text);
                    pen += widthOf(std::string_view(one, 1), kCode);
                }
                // Its detail, at the right, faint.
                const float room = g.list.right() - 8.0f - (pen + 16.0f);
                if (room > 40.0f && !it.detail.empty())
                {
                    const std::string d = fitText(c, it.detail, chat::kSmall, room);
                    c.text(d.c_str(), g.list.right() - 8.0f, c.capCentreTop(y + kRowH * 0.5f, chat::kSmall), chat::kSmall,
                           first + k == choice_ ? p.dim : p.faint, funkgui::Align::right);
                }
            }
            c.popClip();
            // The chosen item's signature and documentation.
            if (choice_ >= 0 && choice_ < n && g.detail.h > 0.0f)
            {
                const lang::Completion& it = completion_->items[static_cast<std::size_t>(choice_)];
                const Rect d = g.detail;
                c.hairlineH(d.x + 4.0f, d.y + 2.0f, d.w - 8.0f, p.cardEdge);
                float y = d.y + 9.0f;
                if (!it.detail.empty())
                {
                    codeLine(it.code.empty() ? it.detail : it.code, d.x + 8.0f, y, d.w - 16.0f, kCardCode);
                    y += 18.0f;
                }
                for (const std::string& l : wrap(reflow(it.doc), chat::kSmall, d.w - 16.0f, it.detail.empty() ? 4 : 3))
                {
                    c.text(l.c_str(), d.x + 8.0f, y, chat::kSmall, p.dim);
                    y += 14.0f;
                }
            }
        }

        // The hover card: above the word (below it when there is no room).
        if (hover_)
        {
            const lang::Hover& h = *hover_;
            const float innerW = kCardMaxW - 2.0f * kPad;
            std::vector<std::string> code;
            for (const std::string_view l : lang::splitLines(h.code))
                if (!l.empty())
                    code.emplace_back(l);
            std::vector<std::string> doc = wrap(reflow(h.doc), chat::kSmall, innerW, 14);
            std::vector<std::string> problems;
            for (const std::string& pr : hoverProblems_)
                for (const std::string& l : wrap(pr, chat::kSmall, innerW - 14.0f, 4))
                    problems.push_back(l);
            float w = 160.0f;
            for (const std::string& l : code)
                w = std::max(w, widthOf(l, kCardCode));
            for (const std::string& l : doc)
                w = std::max(w, widthOf(l, chat::kSmall));
            for (const std::string& l : problems)
                w = std::max(w, widthOf(l, chat::kSmall) + 14.0f);
            w = std::min(kCardMaxW, w + 2.0f * kPad);
            const float codeH = 18.0f * static_cast<float>(code.size());
            const float docH = 14.0f * static_cast<float>(doc.size());
            const float probH = 14.0f * static_cast<float>(problems.size());
            const float gap = (code.empty() ? 0.0f : 1.0f) + (doc.empty() ? 0.0f : 1.0f) + (problems.empty() ? 0.0f : 1.0f);
            const float hgt = 2.0f * kPad + codeH + docH + probH + std::max(0.0f, gap - 1.0f) * 9.0f;
            const funkgui::Point word = E.pointOf({ h.range.from.line, h.range.from.col });
            float x = std::clamp(hoverPoint_.x - 24.0f, 8.0f, panelW - w - 8.0f);
            float y = word.y - hgt - 4.0f;
            if (y < 8.0f)
                y = word.y + E.rowHeight() + 4.0f;
            const Rect r{ x, y, w, hgt };
            card(r);
            float dy = y + kPad;
            for (const std::string& l : problems)
            {
                constexpr Col kRed{ 0xE5, 0x48, 0x4D, 0xFF }, kAmber{ 0xE2, 0xA3, 0x36, 0xFF };
                const bool err = l.rfind("error: ", 0) == 0, warn = l.rfind("warning: ", 0) == 0;
                if (err || warn || l.rfind("note: ", 0) == 0)
                    c.disc(x + kPad + 4.0f, dy + 6.0f, 3.5f, err ? kRed : warn ? kAmber : codeSecond(th));
                c.text(l.c_str(), x + kPad + 14.0f, dy, chat::kSmall, p.text);
                dy += 14.0f;
            }
            if (!problems.empty() && (!code.empty() || !doc.empty()))
            {
                c.hairlineH(x + kPad, dy + 3.0f, w - 2.0f * kPad, p.cardEdge);
                dy += 9.0f;
            }
            for (const std::string& l : code)
            {
                codeLine(l, x + kPad, dy + 2.0f, w - 2.0f * kPad, kCardCode);
                dy += 18.0f;
            }
            if (!code.empty() && !doc.empty())
            {
                c.hairlineH(x + kPad, dy + 3.0f, w - 2.0f * kPad, p.cardEdge);
                dy += 9.0f;
            }
            for (const std::string& l : doc)
            {
                c.text(l.c_str(), x + kPad, dy, chat::kSmall, p.dim);
                dy += 14.0f;
            }
        }
    }

    bool CodeAssist::specLine(char* out, std::size_t n, funkgui::Point pointer) const
    {
        if (statusRect().contains(pointer))
        {
            const StatusParts s = statusParts();
            if (s.formatOnRun.contains(pointer))
                std::snprintf(out, n, "FORMAT ON RUN   %s   A CLICK TURNS IT %s   SHIFT-ALT-F FORMATS THE FILE NOW",
                              formatOnRun() ? "ON: RUN FORMATS EVERY FMINUS FILE FIRST" : "OFF", formatOnRun() ? "OFF" : "ON");
            else if (s.position.contains(pointer))
                std::snprintf(out, n, "LINE AND COLUMN   A CLICK (OR CTRL-G) GOES TO A LINE");
            else if (viewShown())
                std::snprintf(out, n, "LLVM IR   READ ONLY: WHAT FMINUS MADE OF THE PROGRAM   CMD-F FINDS   CTRL-G GOES TO A LINE");
            else if (s.problems.contains(pointer))
                std::snprintf(out, n, "PROBLEMS   %d ERRORS   %d WARNINGS   A CLICK GOES TO THE NEXT   CHECKED AS YOU TYPE",
                              errorCount(), warningCount());
            else
                std::snprintf(out, n, "CTRL-SPACE COMPLETES   SHIFT-ALT-F FORMATS   CMD-F FINDS   CMD-/ COMMENTS   CMD-CLICK GOES TO A DEFINITION");
            return true;
        }
        if (findOpen_ && findBarRect().contains(pointer))
        {
            std::snprintf(out, n, "FIND   RETURN THE NEXT   SHIFT-RETURN THE ONE BEFORE   TAB THE OTHER FIELD   ESCAPE CLOSES");
            return true;
        }
        if (gotoOpen_ && goToBarRect().contains(pointer))
        {
            std::snprintf(out, n, "GO TO LINE   A LINE'S NUMBER, OR LINE:COLUMN   RETURN GOES THERE   ESCAPE CLOSES");
            return true;
        }
        return false;
    }

    const lang::CompletionList& CodeAssist::completion() const
    {
        static const lang::CompletionList none;
        return completion_ ? *completion_ : none;
    }

    const lang::Signature& CodeAssist::signature() const
    {
        static const lang::Signature none;
        return signature_ ? *signature_ : none;
    }

    std::string CodeAssist::hoverText() const
    {
        std::string s;
        for (const std::string& p : hoverProblems_)
            s += p + "\n";
        if (hover_)
            s += hover_->code + "\n" + hover_->doc;
        return s;
    }
}
