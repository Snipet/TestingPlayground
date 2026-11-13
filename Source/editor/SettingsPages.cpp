// Source/editor/SettingsPages.cpp: the settings card's page table (SettingsPages.h), its Builder, and the pages small
// enough to live here: General, Editor, Projects, AI and About. Audio and MIDI is SettingsDevices.cpp's, Debug
// SettingsDebug.cpp's. Each control writes where its setting already lives, and each page reads it there each layout.
#include "editor/SettingsPages.h"

#include "editor/ChatStyle.h"
#include "editor/CodeAssist.h"
#include "editor/Dropdown.h"
#include "editor/Items.h"
#include "editor/Theme.h"

#include "Product.h"

#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>

namespace playground::ui::settings
{
    // ---- the Builder ---------------------------------------------------------------------------------------------------

    float Builder::textW(const std::string& s, const funkgui::TextStyle& st)
    {
        return funkgui::text::width(funkgui::FontService::get().atlas(), s.c_str(), st);
    }

    // A run of spaces, tabs or CRs is one space, and a line break (JUCE's "Can't open the audio device!\n\nThis may
    // be...") starts a new line, never an empty one: the font has no glyph for either.
    std::vector<std::string> Builder::wrapped(const std::string& s, const funkgui::TextStyle& st, float maxW)
    {
        std::vector<std::string> out;
        std::string line;
        std::size_t at = 0;
        while (at < s.size())
        {
            std::size_t end = s.find_first_of(" \t\r\n", at);
            if (end == std::string::npos)
                end = s.size();
            const std::string word = s.substr(at, end - at);
            if (!word.empty())
            {
                const std::string longer = line.empty() ? word : line + " " + word;
                if (!line.empty() && textW(longer, st) > maxW)
                {
                    out.push_back(line);
                    line = word;
                }
                else
                    line = longer;
            }
            if (end < s.size() && s[end] == '\n' && !line.empty())
            {
                out.push_back(line);
                line.clear();
            }
            at = end + 1;
        }
        if (!line.empty())
            out.push_back(line);
        return out;
    }

    float Builder::pillW(const std::string& label, bool menu, bool icon)
    {
        return std::ceil(14.0f + (icon ? 18.0f : 0.0f) + textW(label, chat::kSmall) + (menu ? dropdown::kChevronRoom + 10.0f : 14.0f));
    }

    float Builder::switchW(const std::string& name)
    {
        return std::ceil(kSwitchW + 10.0f + textW(name, chat::kSmall) + 8.0f);
    }

    void Builder::line(Line::Kind kind, std::string text, funkgui::Rect r)
    {
        at_.lines.push_back({ kind, std::move(text), r });
    }

    void Builder::heading(const std::string& title)
    {
        if (y > 0.0f)
            y += kSectionGap;
        line(Line::Kind::heading, title, { 0.0f, y, inner, 16.0f });
        y += kHeadingH;
    }

    void Builder::label(const std::string& text, float w)
    {
        line(Line::Kind::label, text, { 0.0f, y, w, kRowH });
    }

    void Builder::row(const std::string& name, std::string value)
    {
        line(Line::Kind::label, name, { 0.0f, y, kLabelW - 10.0f, kTextRow });
        line(Line::Kind::value, std::move(value), { ctrlX, y, ctrlW, kTextRow });
        y += kTextRow + 4.0f;
    }

    void Builder::notes(const std::string& text, float x, float w, Line::Kind kind)
    {
        for (std::string& l : wrapped(text, chat::kSmall, w))
        {
            line(kind, std::move(l), { x, y, w, 16.0f });
            y += 16.0f;
        }
    }

    Control& Builder::control(Act act, int index, funkgui::Rect r, std::string shown, std::string title, bool enabled)
    {
        Control c;
        c.act = act;
        c.index = index;
        c.r = r;
        c.label = std::move(shown);
        c.title = std::move(title);
        c.enabled = enabled;
        at_.controls.push_back(std::move(c));
        return at_.controls.back();
    }

    Control& Builder::select(int code, funkgui::Rect r, std::string shown, std::string title, bool enabled)
    {
        Control& c = control(Act::item, code, r, std::move(shown), std::move(title), enabled);
        c.menu = true;
        return c;
    }

    Control& Builder::button(int code, funkgui::Rect r, std::string shown, std::string title, bool enabled)
    {
        return control(Act::item, code, r, std::move(shown), std::move(title), enabled);
    }

    Control& Builder::toggle(int code, funkgui::Rect r, std::string shown, std::string title, bool on)
    {
        Control& c = control(Act::item, code, r, std::move(shown), std::move(title), true);
        c.toggle = true;
        c.on = on;
        return c;
    }

    const char* showFolderWord()
    {
#if defined(_WIN32)
        return "Show in Explorer";
#elif defined(__APPLE__)
        return "Show in Finder";
#else
        return "Show in File Manager";
#endif
    }

    namespace
    {
        uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001B3ull; }
        uint64_t mix(uint64_t h, const std::string& s) { return mix(h, std::hash<std::string>{}(s)); }

        const char* const kThemes[] = { "Graphite", "Paper" };

        // A select's menu: the choices (id i + 1), the current one ticked, `pick` called with the chosen index.
        void menu(const Control& c, Context& ctx, const std::vector<std::string>& labels, int current, const std::vector<bool>& enabled,
                  std::function<void(int)> pick)
        {
            funkgui::HostServices* host = ctx.host;
            if (host == nullptr)
                return;
            funkgui::MenuRequest request;
            for (std::size_t i = 0; i < labels.size(); ++i)
                request.items.push_back(dropdown::choice(static_cast<int>(i) + 1, labels[i], static_cast<int>(i) == current,
                                                         i < enabled.size() ? enabled[i] : true));
            dropdown::dropdownMenu(request, c.r, current + 1);
            request.theme = productTheme(funkgui::Theme::byIndex(host->themeIndex()));
            SettingsCard* card = &ctx.card;
            const int n = static_cast<int>(labels.size());
            host->showMenu(request, [card, n, then = std::move(pick)](int chosen) {
                if (!card->isOpen() || chosen < 1 || chosen > n)
                    return;
                then(chosen - 1);
                card->changed();
            });
        }

        // ---- General: the theme, the zoom, the version ------------------------------------------------------------------
        int themeNow(const Context& ctx)
        {
            return ctx.host != nullptr ? ctx.host->themeIndex() : funkgui::UiPreferences::get().theme();
        }

        void buildGeneral(Builder& b)
        {
            const Context& ctx = b.ctx;
            b.heading("APPEARANCE");
            b.label("Theme");
            const int theme = std::clamp(themeNow(ctx), 0, 1);
            b.select(kTheme, { b.ctrlX, b.y, 160.0f, kRowH }, kThemes[theme], "Theme");
            b.y += kRowStep;
            b.label("Zoom");
            const auto steps = ctx.host != nullptr ? ctx.host->zoomSteps() : std::span<const int>{};
            if (!steps.empty())
            {
                char z[16];
                std::snprintf(z, sizeof z, "%d%%", ctx.host->zoomPercent());
                b.select(kZoom, { b.ctrlX, b.y, 160.0f, kRowH }, z, "Zoom");
                b.y += kRowStep;
            }
            else
            {
                b.line(Line::Kind::value, "The window's own size (no zoom steps here)", { b.ctrlX, b.y, b.ctrlW, kRowH });
                b.y += kRowStep;
            }
            b.notes(std::string("For every ") + product::kName + " editor on " + kThisMachine
                        + ". The top bar's theme button and the status bar's zoom set the same.",
                    b.ctrlX, b.ctrlW);
            b.heading("VERSION");
            b.row("Version", std::string(product::kName) + " " + product::kVersion);
        }

        void actGeneral(const Control& c, Context& ctx)
        {
            if (c.act != Act::item)
                return;
            if (c.index == kTheme)
                menu(c, ctx, { kThemes[0], kThemes[1] }, std::clamp(themeNow(ctx), 0, 1), {},
                     [](int i) { funkgui::UiPreferences::get().setTheme(i); });
            else if (c.index == kZoom && ctx.host != nullptr)
            {
                funkgui::HostServices* host = ctx.host;
                std::vector<std::string> labels;
                std::vector<bool> fits;
                std::vector<int> steps;
                int current = -1;
                for (const int s : host->zoomSteps())
                {
                    char z[16];
                    std::snprintf(z, sizeof z, "%d%%", s);
                    if (s == host->zoomPercent())
                        current = static_cast<int>(labels.size());
                    labels.push_back(z);
                    fits.push_back(host->zoomFits(s));
                    steps.push_back(s);
                }
                menu(c, ctx, labels, current, fits, [host, steps](int i) { host->setZoomPercent(steps[static_cast<std::size_t>(i)]); });
            }
        }

        uint64_t keyGeneral(const Context& ctx)
        {
            return mix(mix(funkgui::UiPreferences::get().revision(), static_cast<uint64_t>(themeNow(ctx))),
                       static_cast<uint64_t>(ctx.host != nullptr ? ctx.host->zoomPercent() : 0));
        }

        // ---- Editor: Format on Run (the code text size comes with its own package) --------------------------------------
        void buildEditor(Builder& b)
        {
            b.heading("CODE");
            b.label("Run");
            const std::string what = "Format on Run";
            b.toggle(kFormatOnRun, { b.ctrlX, b.y, Builder::switchW(what), kRowH }, what, "Format on Run", CodeAssist::formatOnRun());
            b.y += kRowH + 2.0f;
            b.notes("Run formats every FMinus file first (Format Document), as the code's status line sets it.", b.ctrlX, b.ctrlW);
        }

        void actEditor(const Control& c, Context&)
        {
            if (c.act == Act::item && c.index == kFormatOnRun)
                CodeAssist::setFormatOnRun(!CodeAssist::formatOnRun());
        }

        uint64_t keyEditor(const Context&) { return funkgui::UiPreferences::get().revision(); }

        // ---- Projects: the folder, shown in the file manager ------------------------------------------------------------
        void buildProjects(Builder& b)
        {
            b.heading("FOLDER");
            b.label("Projects");
            const std::string folder = b.ctx.port.projectFolder();
            b.line(Line::Kind::value, folder, { b.ctrlX, b.y, b.ctrlW, kRowH });
            b.y += kRowStep;
            const std::string word = showFolderWord();
            b.button(kShowProjects, { b.ctrlX, b.y, Builder::pillW(word, false, false), kRowH }, word,
                     std::string(word) + ": the projects folder");
            b.y += kRowStep;
            b.notes(std::string("Each project is a file there, its versions and presets in it. ") + product::kName
                        + " keeps its own folder; nothing in it leaves " + kThisMachine + ".",
                    b.ctrlX, b.ctrlW);
        }

        void actProjects(const Control& c, Context& ctx)
        {
            std::string why;
            if (c.act == Act::item && c.index == kShowProjects)
                ctx.port.showPath(ctx.port.projectFolder(), why);
        }

        uint64_t keyProjects(const Context& ctx) { return mix(0, ctx.port.projectFolder()); }

        // ---- AI: the provider, its model, its key, in words -------------------------------------------------------------
        SettingsCard::Ai aiOf(const Context& ctx)
        {
            return ctx.card.hooks().ai ? ctx.card.hooks().ai() : SettingsCard::Ai{};
        }

        void buildAi(Builder& b)
        {
            const SettingsCard::Ai ai = aiOf(b.ctx);
            b.heading("THE CHAT");
            b.row("Provider", ai.provider.empty() ? std::string("Anthropic") : ai.provider);
            b.row("Model", ai.model.empty() ? std::string("none chosen yet") : ai.model);
            b.row("Key", ai.keyless ? std::string("none needed (a server on ") + kThisMachine + " or the local network)"
                         : !ai.hasKey ? std::string("none stored")
                         : !ai.keyProblem.empty() ? "stored, but " + ai.keyProblem
                                                  : std::string("stored, sealed on ") + kThisMachine);
            b.y += 6.0f;
            const std::string word = "Provider and key...";
            b.button(kProviderKey, { b.ctrlX, b.y, Builder::pillW(word, false, false), kRowH }, word,
                     "Provider and key: the chat's key dialog");
            b.y += kRowStep;
            b.notes("The chat's key dialog keeps the provider, its server and its key; the chat's own pills choose the model.",
                    b.ctrlX, b.ctrlW);
        }

        void actAi(const Control& c, Context& ctx)
        {
            if (c.act != Act::item || c.index != kProviderKey)
                return;
            ctx.card.close();                            // the key dialog over the panel, as the chat's key opens it
            if (ctx.card.hooks().keyDialog)
                ctx.card.hooks().keyDialog();
        }

        uint64_t keyAi(const Context& ctx)
        {
            const SettingsCard::Ai ai = aiOf(ctx);
            return mix(mix(mix(mix(0, ai.provider), ai.model), ai.keyProblem), static_cast<uint64_t>(ai.hasKey) * 2u + ai.keyless);
        }

        // ---- About ------------------------------------------------------------------------------------------------------
        void buildAbout(Builder& b)
        {
            std::string name = product::kName;              // in capitals, as every section's heading
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
            b.heading(name);
            b.row("Version", product::kVersion);
            b.row("Kind", product::kIsInstrument ? "an instrument (a synth)" : "an effect");
            b.row("Made by", product::kCompany);
            b.row("Website", "testingplayground.seanfunk.xyz");
            b.heading("LICENCES");
            b.notes("Built with JUCE, FunkGui, FMinus and LLVM, bgfx and the JetBrains Mono font. The licences of what it "
                    "draws with (bgfx, bx, bimg, bgfx.cmake and the font) are in the licences folder beside the Standalone "
                    "and in the Claude Desktop extension; the source, open, holds the rest.",
                    0.0f, b.inner);
        }
    }

    const Page kGeneralPage{ "general", "General", "How every editor looks, and this version.", nullptr, buildGeneral, actGeneral, keyGeneral };
    const Page kEditorPage{ "editor", "Editor", "How the code editor works.", nullptr, buildEditor, actEditor, keyEditor };
    const Page kProjectsPage{ "projects", "Projects", "Where the projects are kept.", nullptr, buildProjects, actProjects, keyProjects };
    const Page kAiPage{ "ai", "AI", "The model the chat talks to.", nullptr, buildAi, actAi, keyAi };
    const Page kAboutPage{ "about", "About", "This plugin, and what it is made with.", nullptr, buildAbout, nullptr, nullptr };

    std::span<const Page> pages()
    {
        // The rail's order. Network (package 08) takes its row here, before Debug.
        static const std::array<Page, 8> kPages{ kGeneralPage, kEditorPage, kAudioPage, kProjectsPage, kAiPage, kUsagePage, kDebugPage,
                                                 kAboutPage };
        return kPages;
    }
}
