// Source/editor/SettingsCard.h: SETTINGS, a card of pages over the panel, in the chat's look (ChatStyle.h). A rail of
// the pages' names at its left, the page at its right (its title, a line on what it holds, its sections), Done at the
// bottom. It opens in every host, from the top bar's gear (Panel), and in the Standalone also from the app's
// Settings... (Cmd-,: EditorPort::settingsAsked, at EditorPort::settingsPage, the Audio and MIDI page) and the effect's
// muted-input notice.
//
// The pages are data (SettingsPages.h): a row each in one table (settings::pages()), each its id, title, a line, when it
// is listed, what it builds (Builder: its sections' controls and texts) and what its controls do. A later page is a file
// and a row there, never a change inside the card. The card keeps no copy of a setting: a page reads where each lives
// (UiPreferences, the host, the port) whenever the card lays out, and the card follows what can change with no input
// (a page's key(): UiPreferences::revision, the zoom, the chat's provider; the devices' serial; the Debug page's
// once-a-second read) through revision(), which the Panel's draw key mixes.
//
//   General         the theme and the zoom (written where their buttons write), the version
//   Editor          Format on Run (UiPreferences, as the code's status line writes it)
//   Audio and MIDI  the Standalone's devices (EditorPort::devices, DevicePort.h), only where there are some:
//                   OUTPUT (the device, Test, its pairs), INPUT (an effect's: the device, its pairs, Mute input and why),
//                   AUDIO (the type, the rate, the buffer, the latency), MIDI (an instrument's: each input a switch, a
//                   racing wheel's status under its name). A device's error under the section whose choice it was. The
//                   numbers follow the devices while it shows: read again whenever DevicePort::serial moves
//   Projects        the projects folder, with Show in Finder (Explorer, File Manager: EditorPort::showPath)
//   AI              the chat's provider, model and key in words; Provider and key... opens the chat's key dialog
//   Usage           the chat's tokens and costs by day and model (UsagePage.h): a range, Tokens or Cost, a chart of
//                   stacked bars, a table a model, the price table's date, Reset (asked first); read off the message
//                   thread (ChatPort::usage) while it shows
//   Debug           sections of `name  value` rows (EditorPort::debugRows, and the Panel's graphics rows), read once a
//                   second while the page shows and only then (EditorPort::addDebugViewer); Copy all
//   About           the name, the version, the company, the website, where the licences are
//
// The page stays as it is while the card is open; the card opens at the page asked, else the one it showed last. Done,
// Escape and a click outside the card close it. Tab and Shift-Tab (or Down and Up) move the focus over the rail's
// pages, the page's controls and Done; Return or Space acts on the focused one (a rail's entry shows its page). When
// the window is too short for a page, the page scrolls as the code does (Scroll.h: the wheel, a thumb that drags).
// While it is open it takes every pointer event and key, and the rest of the panel is dimmed. Its selects are
// dropdowns (Dropdown.h): a field's trigger, open while their menu is (setMenuOpen: the Panel's menuOpenAt), their menu
// a radio set as wide as the select, the current choice highlighted when it opens.
#pragma once

#include "editor/ChatStyle.h"
#include "editor/Scroll.h"
#include "plugin/EditorPort.h"

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace playground::ui
{
    class SettingsCard
    {
    public:
        static constexpr uint32_t kA11yBase = 0x7FFC0000u;   // its accessibility ids, past every item's

        // What a control does: the devices' own (the Audio and MIDI page), a rail's page, a page's own control (`item`,
        // its index the page's code for it), Done.
        enum class Act : uint8_t { type, output, input, rate, buffer, test, outputPair, inputPair, mute, midi, done, page, item };

        struct Control
        {
            Act act = Act::done;
            int index = 0;                       // a pair's, a MIDI input's, a rail's page's, a page's own code
            funkgui::Rect r;                     // its place (the page's body scrolled; the rail's and Done unscrolled)
            std::string label;                   // what it shows
            std::string title;                   // its accessibility title
            bool menu = false;                   // a pill that opens a menu
            bool toggle = false;                 // a switch
            bool on = false;                     // a switch's state; a rail's entry: its page shows
            bool enabled = true;
            chat::Icon icon = chat::Icon::none;  // a button's icon before its label (Test's play)
            bool own = false;                    // drawn and named by its page (SettingsPages.h: paint, a11y)
            uint32_t id = 0;                     // its accessibility id
        };

        // A text on the card: a section's heading, a row's label, a value, a note, an error line.
        struct Line
        {
            enum class Kind : uint8_t { heading, label, value, note, error };
            Kind kind = Kind::label;
            std::string text;
            funkgui::Rect r;                     // drawn from r.x, centred on r's middle, fitted to r.w
        };

        struct Places
        {
            funkgui::Rect box;                   // the card
            funkgui::Rect rail;                  // the pages' names, at its left
            funkgui::Rect body;                  // the page's sections, which scroll when the window is too short
            int page = 0;                        // the page shown (its row in settings::pages())
            float contentH = 0.0f, scroll = 0.0f, maxScroll = 0.0f;
            std::vector<Control> controls;       // in the Tab order: the rail's pages, the page's, Done last
            std::vector<Line> lines;             // the page's
        };

        // What the card reads from the Panel: the chat's provider as the Panel holds it (the AI page: never the chat's
        // words), the key dialog (Provider and key...), and the graphics rows (the Debug page: the host's, through
        // Panel::setHostDebug, and the editor's own).
        struct Ai
        {
            std::string provider, model;         // "Anthropic", "Claude Opus 5.5" ("" none chosen)
            bool hasKey = false, keyless = false;
            std::string keyProblem;
        };
        struct Hooks
        {
            std::function<Ai()> ai;
            std::function<void()> keyDialog;     // after the card has closed
            std::function<DebugSection()> graphics;
        };

        explicit SettingsCard(EditorPort& port) : port_(port) {}
        ~SettingsCard();
        SettingsCard(const SettingsCard&) = delete;
        SettingsCard& operator=(const SettingsCard&) = delete;

        // Whether a menu opened from this rect is open (the Panel's menuOpenAt): a select draws open while it is.
        void setMenuOpen(std::function<bool(const funkgui::Rect&)> menuOpen) { menuOpen_ = std::move(menuOpen); }
        void setHooks(Hooks hooks) { hooks_ = std::move(hooks); }
        const Hooks& hooks() const noexcept { return hooks_; }

        bool isOpen() const noexcept { return open_; }
        // Opens at the page `id` names (SettingsPages.h; "" or a page not listed: the one it showed last). Always opens.
        bool open(std::string_view id = {});
        void close();
        // Shows the page of `id` while open (false when no listed page has it).
        bool showPage(std::string_view id);
        std::string pageId() const;              // the page shown (or shown last)
        // Each frame, open or not (the muted notice reads the devices): the devices read again when their serial moved,
        // or forgotten when they went (the card closes when it showed their page); the page's key followed; the Debug
        // page read again once a second. True when what it holds changed.
        bool sync(funkgui::HostServices* host = nullptr);
        bool hasDevices() const noexcept { return has_; }
        const DeviceSetup& setup() const noexcept { return setup_; }
        // An effect's input is muted (JUCE's guard against a feedback loop): the Panel's notice says so.
        bool inputMuted() const noexcept { return has_ && setup_.muteApplies && setup_.muted; }

        // The Debug page's sections as last read (none while it does not show), and Copy all's text: a line a row,
        // "name: value", under each section's title.
        const std::vector<DebugSection>& debug() const noexcept { return debug_; }
        std::string debugText() const;

        Places places(float width, float height) const;

        void pointerDown(funkgui::Point p, float width, float height, funkgui::HostServices* host);
        void pointerDrag(funkgui::Point p, float width, float height);   // the thumb's drag, while pressed
        void pointerUp();
        Scroll scrollOf(const Places& at) const;         // the body's scroll: content and view as the places have them
        funkgui::Rect barArea(const Places& at) const;   // where its bar is (Scroll's area)
        void wheel(const funkgui::WheelEvent& e, float width, float height);
        void key(const funkgui::KeyEvent& e, funkgui::HostServices* host, float width, float height);
        funkgui::Cursor cursor(funkgui::Point p, float width, float height) const;
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, float width, float height, funkgui::Point pointer,
                  bool hasPointer) const;
        void accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const;
        bool a11yAction(uint32_t id, funkgui::A11yAction a, float width, float height, funkgui::HostServices* host);
        uint32_t revision() const noexcept { return revision_; }   // bumps when what it shows changes

        // For probes: a control's centre (nullopt when it is not on the card, or scrolled out of its body), the
        // focused control (nullopt for none).
        std::optional<funkgui::Point> centreOf(Act act, int index, float width, float height) const;
        std::optional<Control> focused(float width, float height) const;

        // For the pages (SettingsPages.cpp): after an action, what changed read at once; whether a select's menu is open.
        void changed();
        // A page's view state (a choice of its own that is no setting: a range, the bar picked), kept while the card
        // lives, made on first use.
        template <typename T>
        T& viewState(const char* pageId) const
        {
            std::shared_ptr<void>& s = states_[pageId];
            if (!s)
                s = std::make_shared<T>();
            return *static_cast<T*>(s.get());
        }
        bool menuOpenAt(const funkgui::Rect& r) const { return menuOpen_ && menuOpen_(r); }
        EditorPort& port() noexcept { return port_; }

    private:
        struct Key
        {
            Act act = Act::done;
            int index = -1;                      // -1: none focused
        };
        int focusIndex(const Places& at) const;
        int shownPage() const;                   // page_, else the first listed
        void act(const Control& c, funkgui::HostServices* host);
        void pageChanged();                      // the scroll and focus reset, the Debug viewer counted or not
        void countViewer();
        void gatherDebug();

        EditorPort& port_;
        funkgui::HostServices* host_ = nullptr;          // the Panel's host, as sync() and the input last gave it: what a
                                                         // page lays out with (the theme, the zoom steps)
        Hooks hooks_;
        std::function<bool(const funkgui::Rect&)> menuOpen_;
        bool open_ = false;
        bool has_ = false;
        uint64_t seen_ = 0;                      // the serial setup_ was read at
        DeviceSetup setup_;
        int page_ = 0;                           // the page shown (or last), its row in settings::pages()
        uint64_t pageKey_ = 0;                   // the page's key() when last looked at
        Key focus_;
        bool ring_ = false;                      // the focus shown (the keys moved it)
        Scroll scroll_;                          // its offset kept (scrollOf)
        uint32_t revision_ = 0;
        // The Debug page: its viewer counted (EditorPort::addDebugViewer), its sections, when they were read.
        bool viewer_ = false;
        mutable std::map<std::string, std::shared_ptr<void>> states_;   // the pages' view states (viewState)
        std::vector<DebugSection> debug_;
        double debugAt_ = -1.0;                  // the host's clock at the last read (-1: read, the clock not taken)
    };
}
