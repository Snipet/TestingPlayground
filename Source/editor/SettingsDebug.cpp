// Source/editor/SettingsDebug.cpp: the settings card's Debug page (SettingsPages.h). Rows of `name  value` in
// sections, read-only, with Copy all (plain text, for a bug report) at the top: the processor's sections
// (EditorPort::debugRows: versions, the program, audio, shaders, stores, Claude Desktop, the environment) and the
// graphics rows the Panel gives (the host's, through Panel::setHostDebug, and the editor's own), second.
//
// It costs nothing unless it shows: the card counts it as a viewer (EditorPort::addDebugViewer) while it shows, the
// processor gathers only then, and the card reads the rows again once a second (SettingsCard::sync). A folder's row has
// a button that shows it in the file manager (EditorPort::showPath). Never a key, a chat's word, a file's text or the
// user's audio: the rows hold names, numbers, paths and states.
#include "editor/SettingsPages.h"

#include "editor/ChatStyle.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace playground::ui::settings
{
    namespace
    {
        constexpr float kNameW = 200.0f;                 // a row's name, before its value

        void build(Builder& b)
        {
            const std::vector<DebugSection>& sections = b.ctx.card.debug();
            const std::string copy = "Copy all";
            b.button(kCopyAll, { 0.0f, b.y, Builder::pillW(copy, false, true), kRowH }, copy,
                     "Copy all: the page as plain text, for a bug report")
                .icon = chat::Icon::copy;
            b.line(Line::Kind::note, "Read once a second while this page shows.",
                   { Builder::pillW(copy, false, true) + 12.0f, b.y, b.inner - Builder::pillW(copy, false, true) - 12.0f, kRowH });
            b.y += kRowStep;
            if (sections.empty())
            {
                b.line(Line::Kind::value, "Reading...", { 0.0f, b.y, b.inner, kTextRow });
                b.y += kTextRow;
                return;
            }
            const std::string word = showFolderWord();
            const float showW = Builder::pillW(word, false, false);
            int paths = 0;
            for (const DebugSection& s : sections)
            {
                std::string title = s.title;             // in capitals, as every section's heading
                std::transform(title.begin(), title.end(), title.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
                b.heading(title);
                for (const DebugRow& r : s.rows)
                {
                    // A folder's row as tall as its button, its texts centred on it.
                    const float h = r.path ? kRowH : kTextRow;
                    b.line(Line::Kind::label, r.name, { 0.0f, b.y, kNameW - 10.0f, h });
                    const float valueW = b.inner - kNameW - (r.path ? showW + 8.0f : 0.0f);
                    b.line(Line::Kind::value, r.value, { kNameW, b.y, std::max(40.0f, valueW), h });
                    if (r.path)
                        b.button(kShowPath + paths++, { b.inner - showW, b.y, showW, kRowH }, word, word + ": " + r.name);
                    b.y += h + 2.0f;
                }
            }
        }

        void act(const Control& c, Context& ctx)
        {
            if (c.act != Act::item)
                return;
            if (c.index == kCopyAll)
            {
                if (ctx.host != nullptr)
                    ctx.host->copyText(ctx.card.debugText());
                return;
            }
            int n = kShowPath;
            for (const DebugSection& s : ctx.card.debug())
                for (const DebugRow& r : s.rows)
                {
                    if (!r.path)
                        continue;
                    if (n++ == c.index)
                    {
                        std::string why;
                        ctx.port.showPath(r.value, why);
                        return;
                    }
                }
        }
    }

    const Page kDebugPage{
        "debug",
        "Debug",
        "What this build is and what it is doing now: for a bug report.",
        nullptr,
        build,
        act,
        nullptr,                                         // the card reads it once a second itself
    };
}
