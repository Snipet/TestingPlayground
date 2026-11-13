// Source/editor/SettingsPages.h: the settings card's pages (SettingsCard.h), as data. A page is a row in pages()'s
// table (SettingsPages.cpp) and, when it is more than a few lines, a file of its own (SettingsDevices.cpp: Audio and
// MIDI; SettingsDebug.cpp: Debug; UsagePage.cpp: Usage). A later package adds its page as a file and a row: the card
// does not change (a page that draws sets the card's optional PageHooks from its build: Builder::hooks).
//
// A page builds its sections with a Builder, top down from the body's top-left (y 0), in the body's width: headings,
// a row's label, values, notes, errors and controls, each a Control the card draws, focuses, speaks and hits as any
// other (a select, a switch, a button). Its act() is called for its controls (Act::item, its index the page's own code;
// the Audio and MIDI page's devices' acts) when they are pressed, with the card open and the host there. A setting is
// read where it lives each time the card lays out; key() returns what the page reads that can change with no input of
// the card's (a revision, a hash), so the card draws again when it moves.
#pragma once

#include "editor/SettingsCard.h"

#include <funkgui/text/TextStyle.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace playground::ui::settings
{
    using Act = SettingsCard::Act;
    using Control = SettingsCard::Control;
    using Line = SettingsCard::Line;

    constexpr float kLabelW = 108.0f;            // a row's label, before its controls
    constexpr float kRowH = 28.0f;               // a pill's, a switch's
    constexpr float kRowStep = 36.0f;
    constexpr float kFlowStep = 34.0f;           // switches that wrap: the next line
    constexpr float kTextRow = 22.0f;            // a value's or a note's row
    constexpr float kHeadingH = 26.0f;           // a section's heading and the gap under it
    constexpr float kSectionGap = 14.0f;
    constexpr float kSwitchW = 30.0f, kSwitchH = 17.0f;
    // Why an effect's input starts muted (JUCE's guard), under its switch (and its accessibility help).
    inline constexpr const char* kMuteWhy = "A loop through the speakers would feed back: unmute with headphones on.";

    struct Context
    {
        EditorPort& port;
        funkgui::HostServices* host;             // null while there is none (a headless Panel before its host)
        SettingsCard& card;
    };

    class Builder
    {
    public:
        Builder(SettingsCard::Places& at, float width, Context& context) : ctx(context), inner(width), ctrlW(width - kLabelW), at_(at) {}

        Context& ctx;
        const float inner;                       // the body's width
        const float ctrlX = kLabelW;             // the controls' column
        const float ctrlW;
        float y = 0.0f;                          // the next row's top

        void heading(const std::string& title);                         // a section's (in capitals), a gap before it
        void label(const std::string& text, float w = kLabelW - 10.0f); // a row's, at y
        void line(Line::Kind kind, std::string text, funkgui::Rect r);
        // A row of a label and a value (the value fitted to the controls' column), y moved past it.
        void row(const std::string& name, std::string value);
        // `text` wrapped to `w` from x, a note's lines (or an error's), y moved past them.
        void notes(const std::string& text, float x, float w, Line::Kind kind = Line::Kind::note);
        Control& control(Act act, int index, funkgui::Rect r, std::string shown, std::string title, bool enabled = true);
        Control& select(int code, funkgui::Rect r, std::string shown, std::string title, bool enabled = true);
        Control& button(int code, funkgui::Rect r, std::string shown, std::string title, bool enabled = true);
        Control& toggle(int code, funkgui::Rect r, std::string shown, std::string title, bool on);
        // A page that draws (SettingsCard.h, PageHooks): its hooks, for these places.
        void hooks(const SettingsCard::PageHooks& h) { at_.hooks = h; }

        // A pill's width for its label (a chevron when a menu opens, an icon before it); a switch's with its name.
        static float pillW(const std::string& label, bool menu, bool icon);
        static float switchW(const std::string& name);
        static float textW(const std::string& s, const funkgui::TextStyle& st);
        // `s` in lines of at most `maxW` px of `st`, words kept whole while they fit; a line break starts a line.
        static std::vector<std::string> wrapped(const std::string& s, const funkgui::TextStyle& st, float maxW);

    private:
        SettingsCard::Places& at_;
    };

    struct Page
    {
        const char* id;                          // "general", "audio": what EditorPort::settingsPage and open() name
        const char* title;                       // the rail's name and the page's heading
        const char* blurb;                       // the line under its heading
        bool (*shown)(const Context&);           // listed in the rail (null: always)
        void (*build)(Builder&);
        void (*act)(const Control&, Context&);   // its controls pressed (null: none)
        uint64_t (*key)(const Context&);         // what it reads that changes with no input (null: nothing)
    };

    // Every page, in the rail's order: General, Editor, Audio and MIDI, Projects, AI, Usage, Debug, About.
    std::span<const Page> pages();

    // The pages in files of their own (the table names them).
    extern const Page kAudioPage;                // SettingsDevices.cpp
    extern const Page kDebugPage;                // SettingsDebug.cpp
    extern const Page kUsagePage;                // UsagePage.cpp

    // The Debug page's own codes (Act::item): Copy all; a folder's row's Show button (kShowPath + its row's number).
    constexpr int kCopyAll = 0, kShowPath = 100;
    // The other pages' codes (Act::item), for the probes.
    constexpr int kTheme = 0, kZoom = 1;                     // General
    constexpr int kFormatOnRun = 0;                          // Editor
    constexpr int kShowProjects = 0;                         // Projects
    constexpr int kLookForGit = 10, kChooseGit = 11, kDefaultGit = 12, kShowGitFolder = 13;   // ... its Git rows
    constexpr int kProviderKey = 0;                          // AI

    // The Projects page's Git rows (SettingsGit.cpp, package 05): built after its folder, its codes acted on (true: one
    // of theirs), and what they read that changes with no input.
    void gitRows(Builder& b);
    bool gitAct(const Control& c, Context& ctx);
    uint64_t gitKey(const Context& ctx);

    // What a folder's button says on this platform: Show in Finder, Explorer, File Manager.
    const char* showFolderWord();
}
