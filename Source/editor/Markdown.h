// Source/editor/Markdown.h: the editor's light markdown, laid out and drawn: the chat's answers (ChatView.h) and the
// What's new card (UpdateCards.h) take it.
//
//   paragraphs          lines joined until a blank line, wrapped at spaces (a word longer than the line broken)
//   - and 1. lists      an item's indented lines its own; two spaces a level, three levels
//   # headings          #, ## and ### (and deeper, as ###)
//   **bold**, `code`    inside any of these; other marks stay as typed
//   > quotes, --- rules
//   ``` code blocks     a rounded card, highlighted as FMinus, its language named and a Copy (Layout::copies)
//   [text](url)         only with Options::links (the chat has none: its links stay as typed): the text drawn in the
//                       accent, its places in Layout::links for the owner to hit; what a click opens is the owner's
//
// Text is laid out as the chat's ChatStyle sets it, in columns of each role's advance (Metrics: measured once, on first
// use, from FunkGui's font: the message thread's), from x 0 and a y the caller gives, so the owner places the layout
// where it draws it. Callers give the text as printable ASCII (asciiText, Items.h) and the raw text a code block's Copy
// copies. Moved out of ChatView.cpp unchanged (gui.chat holds the chat's pictures to it); the links are the card's.
#pragma once

#include "editor/ChatStyle.h"

#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/text/Highlighter.h>

#include <cstdint>
#include <string>
#include <vector>

namespace playground::ui::markdown
{
    // How a piece of text is set (styleOf and colourOf resolve the style and the colour).
    enum Role : uint8_t { body, strong, code, h1, h2, h3, dim, small, smallStrong, faint, danger, quote, accent,
                          smallCode };

    constexpr float kCodePad = 10.0f;            // a code card's inset
    constexpr float kCodeHeader = 24.0f;         // its language's row, over a hairline

    struct Span
    {
        float x = 0.0f, y = 0.0f, lineH = 0.0f;
        std::string text;
        Role role = body;
        bool link = false;                       // a link's text (Options::links): drawn in the accent
    };
    enum class ShapeKind : uint8_t { inlineCode, codeCard, bullet, quoteBar, rule };
    struct Shape
    {
        funkgui::Rect r;
        ShapeKind kind = ShapeKind::inlineCode;
    };
    struct CodeLine
    {
        float x = 0.0f, y = 0.0f;
        std::string text;
        std::vector<funkgui::text::Span> spans;
    };
    struct Copy                                  // a code block's Copy: where it is, what it copies
    {
        funkgui::Rect r;
        std::string payload;
    };
    struct Link                                  // a link's text on one line (a link that wraps has one a line)
    {
        funkgui::Rect r;
        std::string url;
    };

    struct Layout
    {
        std::vector<Span> spans;
        std::vector<Shape> shapes;
        std::vector<CodeLine> code;
        std::vector<Copy> copies;
        std::vector<Link> links;
    };

    struct Options
    {
        bool links = false;                      // [text](url) read as a link
    };

    // Each role's advance (the fonts are set in columns), measured once a Metrics on first use.
    class Metrics
    {
    public:
        float adv(Role r) const;
        float codeAdv() const;                   // a code card's

    private:
        mutable float body_ = 0.0f, small_ = 0.0f, code_ = 0.0f, h1_ = 0.0f, h2_ = 0.0f;
    };

    const funkgui::TextStyle& styleOf(Role r);
    funkgui::Col colourOf(Role r, const chat::Palette& p);

    std::vector<std::string> splitLines(const std::string& s);
    std::string trimLeft(const std::string& s);

    // The text of `paragraph` from x0, `w` wide, from y: wrapped at spaces (a word longer than the line broken), the
    // pieces' roles kept; returns the y under its last line. markdown false: the paragraph as plain text (the user's
    // own words).
    float flow(Layout& out, const Metrics& m, const std::string& paragraph, Role base, float x0, float w, float y,
               float lineH, bool markdown = true, const Options& options = {});
    void layoutCode(Layout& out, const Metrics& m, const std::string& lang, const std::vector<std::string>& lines,
                    const std::string& copied, float w, float& y);
    // text: as drawn (asciiText); raw: the text's own, line for line (what a code block's Copy copies). y moves past it.
    void layout(Layout& out, const Metrics& m, const std::string& text, const std::string& raw, float w, float& y,
                const Options& options = {});

    // Drawing, at the owner's offsets (the layout's x 0 at x0, its y 0 at top).
    void drawText(funkgui::Canvas& c, const std::string& s, float x, float lineTop, float lineH, Role r, funkgui::Col col);
    void drawShape(funkgui::Canvas& c, const chat::Palette& p, ShapeKind kind, const funkgui::Rect& r);
    void drawCode(funkgui::Canvas& c, const funkgui::Theme& th, const chat::Palette& p,
                  const std::vector<funkgui::text::HighlightStyle>& codePalette, const CodeLine& cl, float x0, float top,
                  float codeAdv);
    // A whole layout (the card's): its shapes, spans and code lines, those under `clip` alone; a link under `hover`
    // (its index in links, -1 none) drawn underlined. Copy buttons are the owner's.
    void draw(funkgui::Canvas& c, const funkgui::Theme& th, const Layout& l, const Metrics& m, float x0, float top,
              const funkgui::Rect& clip, int hover = -1);
}
