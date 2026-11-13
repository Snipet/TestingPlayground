// PROBE name=gui.settings timeout=180
//
// gui.settings: SETTINGS, the editor's card of pages (editor/SettingsCard.h, SettingsPages.h): in a host (no devices:
// the top bar's gear) and in the Standalone (the Audio and MIDI page, the devices in the editor's own card), and the
// effect's muted-input notice, headless over a real Processor whose window hooks the probe sets as the app's window
// does (Processor::setWindowHooks), with a DevicePort of its own (FakeDevices: a few devices, made up; every action
// recorded and applied as the app's would), to <build>/probe-out/:
//   gui.settings.png        the Audio and MIDI page in GRAPHITE, as the app opens it
//   gui.settings.paper.png  in PAPER
//   gui.settings.small.png  at the least size (kMinWidth x kMinHeight)
//   gui.settings.error.png  a device's error under its section
//   gui.settings.muted.png  the effect's notice in the status bar (the synth has none)
//   gui.settings.host.png   the card in a host (the gear's): General, no Audio and MIDI page
//   gui.settings.<page>.png, .<page>.paper.png   each page (general, editor, projects, ai, network, debug, about) in
//                                                both themes
//   gui.settings.debug.small.png                 the Debug page at the least size
// and checks:
//   HOST     no devices (a host's window): the gear in the top bar opens the card at General, its rail without Audio
//            and MIDI (which openSettings("audio") does not show either); no notice
//   PAGES    the rail lists the pages in order; a click and Return on a rail's entry show its page; the page is kept
//            while the card is open and when the gear opens it again; the controls' ids are unique
//   GENERAL  the theme's select writes UiPreferences' theme (another Panel's card follows: its revision moves), the
//            zoom's the host's zoom; the version shown
//   EDITOR   Format on Run's switch writes UiPreferences' codeFormatOnRun, both ways
//   PROJECTS the projects folder shown; Show in File Manager (Finder, Explorer) asks the processor's launcher for it
//   AI       the provider, model and key in words; Provider and key... closes the card and opens the key dialog
//   DEBUG    nothing gathered before the page shows; its sections (Versions, Graphics, Program, Audio, Shaders, Stores,
//            Claude Desktop, Environment) and the rows that must be right (the version, the shader target, the
//            program's id); read again once a second while it shows, never once it does not; Copy all's text on the
//            host's clipboard, every section in it, nothing of the program's text
//   A11Y     the rail's entries are buttons named for their pages, the page shown checked; every control titled
//   LEAST    every page's places at the least size
//   NOTICE   the effect: with the devices handed over, the notice and its Settings... button in the status bar, clear of
//            the zoom; the button opens the card. The synth: none
//   OPEN     the app's request (Processor::showSettings) opens the card, over the panel (the rest takes no click)
//   PLACES   at the default size, the least and a large one: every control inside the card (and in its body, unscrolled)
//            and none over another or over a text; each with an accessibility title
//   SECTIONS the effect's Input (its device, its pairs, Mute input) and no MIDI; the synth's MIDI (an input a switch)
//            and no Input
//   KEYS     Tab reaches every control once, in order, the focus ring on it; Return on Test plays the tone; a Cmd
//            key is given back (to the app's menu: Cmd-Q)
//   ACTIONS  a channel pair's switch, a MIDI input's switch, the mute's switch (the notice gone once unmuted) and Test
//            call the port; the output device, the sample rate and the buffer size chosen through their menus (the
//            host's, the current one ticked) call it with those values, and the card shows them
//   WHEEL    (the synth) the racing wheel among the MIDI inputs (standalone/RacingWheel.h's entry), its status under its
//            name and in its accessibility title; its switch calls the port with its identifier; unplugged, it goes
//   SERIAL   a device plugged in while the card is open: the output's menu lists it
//   ERROR    the port's error shown under its section (Audio), in words to a screen reader too; JUCE's error with
//            line breaks in lines of its own, no break drawn
//   SCROLL   at the least size, more than the card holds: it scrolls, Done stays in the card, Tab scrolls the focused
//            control into view, the wheel back; as the code scrolls (editor/Scroll.h): a notch three line pitches,
//            reversed set (a built WheelEvent: HeadlessHost cannot set it) the same way, its thumb dragged in proportion,
//            a press on its track jumps
//   CLOSE    Done, Escape and a click outside the card close it (the click reaching nothing under it: RUN unpressed);
//            a double-click outside it over a dial closes it and leaves the dial's value (the next one resets it)
//   GONE     the hooks let go of the devices (the window closing): the card and the notice go
#include "Probe.h"

#include "editor/Panel.h"
#include "editor/SettingsCard.h"
#include "editor/SettingsPages.h"
#include "plugin/DevicePort.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"
#include "plugin/ProjectStore.h"
#include "plugin/WheelMap.h"

#include "Product.h"

#include <funkgui/canvas/Prim.h>
#include <funkgui/gpu/ShaderLibrary.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace pg = playground;
using Act = pg::ui::SettingsCard::Act;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;

    // The app's devices, made up: three outputs (the interface with two pairs out and in), its rates and buffers, two
    // MIDI inputs (the synth's), the input muted (the effect's). Every call is recorded ("rate:96000") and applied.
    class FakeDevices final : public pg::DevicePort
    {
    public:
        FakeDevices()
        {
            s_.types = { "CoreAudio" };
            s_.type = "CoreAudio";
            s_.outputs = { "MacBook Pro Speakers", "External Headphones", "Studio Interface" };
            s_.output = "Studio Interface";
            s_.hasInput = !kInstrument;
            if (s_.hasInput)
            {
                s_.inputs = { "MacBook Pro Microphone", "Studio Interface" };
                s_.input = "Studio Interface";
            }
            s_.open = true;
            s_.rates = { 44100.0, 48000.0, 88200.0, 96000.0 };
            s_.rate = 48000.0;
            s_.buffer = 256;
            s_.takesMidi = kInstrument;
            if (s_.takesMidi)
                s_.midi = { { "IAC Driver Bus 1", "iac-1", false, {} }, { "KeyStep 37", "keystep-37", true, {} },
                            { pg::wheel::kName, pg::wheel::kIdentifier, true, "switching to its full mode" } };
            s_.muteApplies = !kInstrument;
            s_.muted = !kInstrument;
            refit();
        }

        pg::DeviceSetup setup() const override
        {
            pg::DeviceSetup s = s_;
            s.serial = serial_;
            return s;
        }
        uint64_t serial() const override { return serial_; }

        void chooseType(const std::string& type) override { did("type:" + type, [&] { s_.type = type; }); }
        void chooseOutput(const std::string& device) override { did("output:" + device, [&] { s_.output = device; }); }
        void chooseInput(const std::string& device) override { did("input:" + device, [&] { s_.input = device; }); }
        void chooseRate(double rate) override
        {
            did("rate:" + std::to_string(std::lround(rate)), [&] { s_.rate = rate; });
        }
        void chooseBuffer(int samples) override { did("buffer:" + std::to_string(samples), [&] { s_.buffer = samples; }); }
        void switchPair(bool input, int pair, bool on) override
        {
            did(std::string("pair:") + (input ? "in:" : "out:") + std::to_string(pair) + (on ? ":on" : ":off"), [&] {
                std::vector<pg::ChannelPair>& pairs = input ? s_.inputPairs : s_.outputPairs;
                for (std::size_t i = 0; i < pairs.size(); ++i)             // the product's two channels: one pair at most
                    pairs[i].on = static_cast<int>(i) == pair ? on : (on ? false : pairs[i].on);
            });
        }
        void switchMidi(const std::string& identifier, bool on) override
        {
            did("midi:" + identifier + (on ? ":on" : ":off"), [&] {
                for (pg::MidiInputDevice& m : s_.midi)
                    if (m.identifier == identifier)
                        m.on = on;
            });
        }
        void setMuted(bool muted) override { did(std::string("mute:") + (muted ? "on" : "off"), [&] { s_.muted = muted; }); }
        void playTestTone() override { calls.push_back("tone"); }

        // The world changing: a device plugged in, an error, the racing wheel unplugged.
        void plugIn(const std::string& name)
        {
            s_.outputs.push_back(name);
            ++serial_;
        }
        void unplugWheel()
        {
            std::erase_if(s_.midi, [](const pg::MidiInputDevice& m) { return m.identifier == pg::wheel::kIdentifier; });
            ++serial_;
        }
        void fail(const std::string& error, pg::DeviceSection where)
        {
            s_.error = error;
            s_.errorOf = where;
            ++serial_;
        }
        // A device of many outputs: `pairs` pairs on the output (0: the device's own again).
        void crowd(int pairs)
        {
            crowded_ = pairs;
            s_.outputPairs.clear();
            refit();
            ++serial_;
        }

        std::string last() const { return calls.empty() ? std::string("(none)") : calls.back(); }
        std::vector<std::string> calls;

    private:
        template <typename F>
        void did(std::string call, F apply)
        {
            calls.push_back(std::move(call));
            apply();
            refit();
            ++serial_;
        }

        // The output's and input's pairs (the interface has two each, the rest one), the buffers' ms and the latency at
        // the rate.
        void refit()
        {
            const int crowded = crowded_;
            const auto pairs = [crowded](const std::string& device, const char* prefix, std::vector<pg::ChannelPair>& out) {
                const std::size_t n = device == "Studio Interface" ? 2 : device.empty() ? 0 : 1;
                if (std::string(prefix) == "Output" && crowded > 0)
                {
                    if (out.size() != static_cast<std::size_t>(crowded))
                    {
                        out.clear();
                        for (int i = 0; i < crowded; ++i)
                            out.push_back({ "Output " + std::to_string(2 * i + 1) + " + " + std::to_string(2 * i + 2), i == 0 });
                    }
                    return;
                }
                if (out.size() == n)
                    return;
                out.clear();
                for (std::size_t i = 0; i < n; ++i)
                    out.push_back({ std::string(prefix) + " " + std::to_string(2 * i + 1) + " + " + std::to_string(2 * i + 2), i == 0 });
            };
            pairs(s_.output, "Output", s_.outputPairs);
            if (s_.hasInput)
                pairs(s_.input, "Input", s_.inputPairs);
            s_.buffers.clear();
            for (const int b : { 32, 64, 128, 256, 512, 1024 })
                s_.buffers.push_back({ b, b * 1000.0 / s_.rate });
            s_.bufferMs = s_.buffer * 1000.0 / s_.rate;
            s_.outputLatency = s_.buffer + 37;
            s_.inputLatency = s_.hasInput ? s_.buffer + 29 : 0;
            s_.outputLatencyMs = s_.outputLatency * 1000.0 / s_.rate;
            s_.inputLatencyMs = s_.inputLatency * 1000.0 / s_.rate;
        }

        pg::DeviceSetup s_;
        uint64_t serial_ = 1;
        int crowded_ = 0;
    };

    int countTag(const funkgui::PrimList& frame, funkgui::Tag tag)
    {
        int n = 0;
        for (const funkgui::Prim& p : frame.prims)
            n += p.tag == tag ? 1 : 0;
        return n;
    }

    void writePng(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const char* name)
    {
        const std::string path = probe::outPath(name);
        host.draw();
        P.eq(std::string(name) + ".written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
    }

    bool meet(const funkgui::Rect& a, const funkgui::Rect& b)
    {
        return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
    }

    bool inside(const funkgui::Rect& r, const funkgui::Rect& in)
    {
        return r.x >= in.x && r.y >= in.y && r.right() <= in.right() && r.bottom() <= in.bottom();
    }

    // The card's places at the panel's size: every control inside the card (and its body: nothing scrolled away),
    // none over another or over a text, each with an accessibility title.
    void checkPlaces(funkgui::test::Probe& P, const std::string& key, const pg::ui::Panel& panel, pg::ui::SettingsCard& card)
    {
        const float W = static_cast<float>(panel.width()), H = static_cast<float>(panel.height());
        const pg::ui::SettingsCard::Places at = card.places(W, H);
        bool in = true, apart = true, clear = true, body = true;
        for (std::size_t i = 0; i < at.controls.size(); ++i)
        {
            const pg::ui::SettingsCard::Control& c = at.controls[i];
            in = in && (inside(c.r, at.box) || (at.maxScroll > 0.0f && !meet(c.r, at.body)));   // scrolled out of view
            body = body && (c.act == Act::done || c.act == Act::page || inside(c.r, at.body) || at.maxScroll > 0.0f);
            for (std::size_t j = 0; j < i; ++j)
                apart = apart && !meet(c.r, at.controls[j].r);
            for (const pg::ui::SettingsCard::Line& l : at.lines)
            {
                // What is drawn: a page's text is clipped to its body (one scrolled half out of it is cut at its edge).
                const float top = std::max(l.r.y, at.body.y), bottom = std::min(l.r.bottom(), at.body.bottom());
                const funkgui::Rect drawn{ l.r.x, top, l.r.w, bottom - top };
                clear = clear && (bottom <= top || !meet(c.r, drawn));
            }
        }
        bool linesIn = true;
        for (const pg::ui::SettingsCard::Line& l : at.lines)
            linesIn = linesIn && (inside(l.r, at.body) || at.maxScroll > 0.0f);   // a page that scrolls: below its body
        std::printf("%s: card %.0f x %.0f at %.0f, %.0f; %zu controls, %zu texts; content %.0f px, scroll %.0f\n", key.c_str(),
                    static_cast<double>(at.box.w), static_cast<double>(at.box.h), static_cast<double>(at.box.x),
                    static_cast<double>(at.box.y), at.controls.size(), at.lines.size(), static_cast<double>(at.contentH),
                    static_cast<double>(at.maxScroll));
        P.eq(key + ".in_window", at.box.x >= 0.0f && at.box.y >= 0.0f && at.box.right() <= W && at.box.bottom() <= H ? 1 : 0, 1);
        P.eq(key + ".controls_in_card", in ? 1 : 0, 1);
        P.eq(key + ".controls_in_body", body ? 1 : 0, 1);
        P.eq(key + ".texts_in_body", linesIn ? 1 : 0, 1);
        P.eq(key + ".controls_apart", apart ? 1 : 0, 1);
        P.eq(key + ".controls_clear_of_texts", clear ? 1 : 0, 1);
        std::vector<funkgui::A11yItem> items;
        panel.accessibility(items);
        int titled = 0;
        for (const pg::ui::SettingsCard::Control& c : at.controls)
            for (const funkgui::A11yItem& it : items)
                titled += it.id == c.id && !it.title.empty() ? 1 : 0;
        P.eq(key + ".a11y_titles", titled, static_cast<int64_t>(at.controls.size()));
    }

    bool has(const pg::ui::SettingsCard& card, const pg::ui::Panel& panel, Act act)
    {
        for (const pg::ui::SettingsCard::Control& c : card.places(static_cast<float>(panel.width()), static_cast<float>(panel.height())).controls)
            if (c.act == act)
                return true;
        return false;
    }

    // The control of `act` and `index` on the page shown, if any.
    std::optional<pg::ui::SettingsCard::Control> controlOf(const pg::ui::SettingsCard& card, const pg::ui::Panel& panel, Act act, int index)
    {
        for (const pg::ui::SettingsCard::Control& c : card.places(static_cast<float>(panel.width()), static_cast<float>(panel.height())).controls)
            if (c.act == act && c.index == index)
                return c;
        return std::nullopt;
    }

    bool lineHas(const pg::ui::SettingsCard& card, const pg::ui::Panel& panel, const std::string& text)
    {
        for (const pg::ui::SettingsCard::Line& l : card.places(static_cast<float>(panel.width()), static_cast<float>(panel.height())).lines)
            if (l.text.find(text) != std::string::npos)
                return true;
        return false;
    }

    // PAGES, GENERAL, EDITOR, PROJECTS, AI, DEBUG, A11Y, LEAST and each page's PNGs, in a host's window (no devices).
    void pagesChecks(funkgui::test::Probe& P, pg::Processor& processor, pg::ui::Panel& panel, funkgui::HeadlessHost& host,
                     pg::ui::SettingsCard& card)
    {
        namespace st = pg::ui::settings;
        pg::EditorPort& port = processor.editorPort();
        const auto W = [&] { return static_cast<float>(panel.width()); };
        const auto H = [&] { return static_cast<float>(panel.height()); };
        const auto settle = [&] {
            host.tick(2);
            host.settle();
        };
        const auto click = [&](Act act, int index) {
            const auto at = card.centreOf(act, index, W(), H());
            if (at)
                host.click(at->x, at->y);
            settle();
            return at.has_value();
        };
        const auto pageIndex = [](const char* id) {
            const auto all = st::pages();
            for (std::size_t i = 0; i < all.size(); ++i)
                if (all[i].id == std::string(id))
                    return static_cast<int>(i);
            return -1;
        };

        // ---- PAGES -------------------------------------------------------------------------------------------------------
        P.eq("pages.debug_none_before", static_cast<int64_t>(port.debugGathers()), 0);
        const funkgui::Point gear = panel.centreOfSettings();
        host.click(gear.x, gear.y);
        settle();
        P.eq("pages.gear_opens", card.isOpen() ? 1 : 0, 1);
        {
            std::vector<std::string> order;
            for (const pg::ui::SettingsCard::Control& c : card.places(W(), H()).controls)
                if (c.act == Act::page)
                    order.push_back(st::pages()[static_cast<std::size_t>(c.index)].id);
            std::string joined;
            for (const std::string& o : order)
                joined += o + " ";
            std::printf("rail: %s\n", joined.c_str());
            P.eq("pages.rail_order", joined == "general editor projects ai network debug about " ? 1 : 0, 1);
        }
        P.eq("pages.click_editor", click(Act::page, pageIndex("editor")) && card.pageId() == "editor" ? 1 : 0, 1);
        {
            // Tab to the rail's Projects, Return.
            const int projects = pageIndex("projects");
            for (int k = 0; k < 20; ++k)
            {
                const auto f = card.focused(W(), H());
                if (f && f->act == Act::page && f->index == projects)
                    break;
                host.keys("tab");
            }
            host.keys("return");
            settle();
            P.eq("pages.return_projects", card.pageId() == "projects" ? 1 : 0, 1);
        }
        {
            std::set<uint32_t> ids;
            const auto at = card.places(W(), H());
            for (const pg::ui::SettingsCard::Control& c : at.controls)
                ids.insert(c.id);
            P.eq("pages.ids_unique", static_cast<int64_t>(ids.size()), static_cast<int64_t>(at.controls.size()));
        }
        host.keys("escape");
        settle();
        host.click(gear.x, gear.y);
        settle();
        P.eq("pages.kept_on_reopen", card.pageId() == "projects" ? 1 : 0, 1);

        // ---- GENERAL: the theme and the zoom, written where they live -------------------------------------------------------
        card.showPage("general");
        settle();
        {
            pg::ui::Panel other(processor.ports(), port);
            funkgui::HeadlessHost otherHost(other, 0, 2.0f);
            otherHost.settle();
            other.openSettings("general");
            otherHost.tick(2);
            const uint32_t otherRev = other.settingsCard().revision();
            const uint32_t prefsRev = funkgui::UiPreferences::get().revision();
            click(Act::item, st::kTheme);
            const funkgui::MenuRequest* menu = host.pendingMenu();
            bool listed = menu != nullptr && menu->items.size() == 2;
            bool ticked = false;
            if (menu != nullptr)
                for (const funkgui::MenuItem& it : menu->items)
                    ticked = ticked || (it.checked && it.label == "Graphite");
            const bool chosen = menu != nullptr && host.chooseMenuItem("Paper");
            settle();
            P.eq("general.theme_menu", listed && ticked && chosen ? 1 : 0, 1);
            P.eq("general.theme_written", funkgui::UiPreferences::get().theme(), 1);
            P.ge("general.prefs_revision", static_cast<double>(funkgui::UiPreferences::get().revision()), static_cast<double>(prefsRev) + 1.0);
            otherHost.tick(2);
            P.ge("general.other_follows", static_cast<double>(other.settingsCard().revision()), static_cast<double>(otherRev) + 1.0);
            other.settingsCard().close();
            funkgui::UiPreferences::get().setTheme(0);
            settle();
            click(Act::item, st::kZoom);
            const bool zoomed = host.pendingMenu() != nullptr && host.chooseMenuItem("150%");
            settle();
            P.eq("general.zoom_menu", zoomed ? 1 : 0, 1);
            P.eq("general.zoom_written", host.zoomPercent(), 150);
            const auto z = controlOf(card, panel, Act::item, st::kZoom);
            P.eq("general.zoom_shown", z && z->label == "150%" ? 1 : 0, 1);
            host.setZoom({ 100, 125, 150, 200 }, 100);
            P.eq("general.version", lineHas(card, panel, pg::product::kVersion) ? 1 : 0, 1);
        }

        // ---- EDITOR: Format on Run --------------------------------------------------------------------------------------
        card.showPage("editor");
        settle();
        {
            const int before = funkgui::UiPreferences::get().getInt("codeFormatOnRun", 0, 0, 1);
            click(Act::item, st::kFormatOnRun);
            P.eq("editor.format_on_run", funkgui::UiPreferences::get().getInt("codeFormatOnRun", 0, 0, 1), before == 0 ? 1 : 0);
            const auto t = controlOf(card, panel, Act::item, st::kFormatOnRun);
            P.eq("editor.switch_shows", t && t->toggle && t->on == (before == 0) ? 1 : 0, 1);
            click(Act::item, st::kFormatOnRun);
            P.eq("editor.format_back", funkgui::UiPreferences::get().getInt("codeFormatOnRun", 0, 0, 1), before);
        }

        // ---- PROJECTS: the folder, shown through the processor's launcher ----------------------------------------------
        card.showPage("projects");
        settle();
        {
            std::string asked;
            processor.setFolderLauncher([&asked](pg::FolderApp app, const std::string& path, std::string&) {
                if (app == pg::FolderApp::files)
                    asked = path;
                return true;
            });
            P.eq("projects.folder_shown", lineHas(card, panel, port.projectFolder()) ? 1 : 0, 1);
            const auto b = controlOf(card, panel, Act::item, st::kShowProjects);
            P.eq("projects.button_words", b && b->label == st::showFolderWord() ? 1 : 0, 1);
            click(Act::item, st::kShowProjects);
            std::printf("projects: shown %s\n", asked.c_str());
            P.eq("projects.show_asked", !asked.empty() && asked.rfind("~", 0) != 0
                                            && asked == pg::projectstore::directory().getFullPathName().toStdString() ? 1 : 0, 1);
            processor.setFolderLauncher({});
        }

        // ---- AI: the provider in words; the key dialog -------------------------------------------------------------------
        card.showPage("ai");
        settle();
        P.eq("ai.provider", lineHas(card, panel, "Anthropic") ? 1 : 0, 1);
        P.eq("ai.key_words", lineHas(card, panel, "none stored") ? 1 : 0, 1);
        click(Act::item, st::kProviderKey);
        P.eq("ai.card_closed", card.isOpen() ? 0 : 1, 1);
        P.eq("ai.key_dialog", panel.keyDialogOpen() ? 1 : 0, 1);
        host.keys("escape");
        settle();
        P.eq("ai.key_dialog_closes", panel.keyDialogOpen() ? 0 : 1, 1);

        // ---- DEBUG ------------------------------------------------------------------------------------------------------
        P.eq("debug.none_gathered_closed", static_cast<int64_t>(port.debugGathers()), 0);
        P.eq("debug.no_viewer_closed", port.debugViewers(), 0);
        panel.openSettings("debug");
        settle();
        P.eq("debug.viewer", port.debugViewers(), 1);
        P.ge("debug.gathered", static_cast<double>(port.debugGathers()), 1.0);
        {
            std::string titles;
            for (const pg::DebugSection& sec : card.debug())
                titles += sec.title + "; ";
            std::printf("debug: %s\n", titles.c_str());
            P.eq("debug.sections", titles == "Versions; Graphics (FunkGui); Program (FMinus); Audio; Shaders; Stores; Claude Desktop; Environment; "
                                       ? 1 : 0, 1);
            const auto rowOf = [&](const std::string& section, const std::string& name) -> std::string {
                for (const pg::DebugSection& sec : card.debug())
                    if (sec.title == section)
                        for (const pg::DebugRow& r : sec.rows)
                            if (r.name == name)
                                return r.value;
                return "(none)";
            };
            const std::string product = rowOf("Versions", "Product");
            std::printf("debug: product %s; build %s; fminus %s; shaders %s; program %s\n", product.c_str(), rowOf("Versions", "Build").c_str(),
                        rowOf("Versions", "FMinus").c_str(), rowOf("Graphics (FunkGui)", "Shader target").c_str(),
                        rowOf("Program (FMinus)", "Program").c_str());
            P.eq("debug.version", product.find(pg::product::kVersion) != std::string::npos ? 1 : 0, 1);
            P.eq("debug.build_commit", rowOf("Versions", "Build").rfind("commit ", 0) == 0 ? 1 : 0, 1);
            P.eq("debug.fminus_commit", rowOf("Versions", "FMinus").find(std::string(pg::product::kFMinusCommit).substr(0, 12)) != std::string::npos ? 1 : 0, 1);
            const funkgui::shaders::Target t = funkgui::shaders::target();
            const std::string target = rowOf("Graphics (FunkGui)", "Shader target");
            P.eq("debug.shader_target", t.platform.empty() ? (target.rfind("none yet", 0) == 0 ? 1 : 0)
                                                           : (target == t.platform + ", " + t.profile ? 1 : 0), 1);
            P.eq("debug.program_id", rowOf("Program (FMinus)", "Program") == "#" + std::to_string(port.status().program) ? 1 : 0, 1);
            P.eq("debug.environment_listed", rowOf("Environment", std::string(pg::product::kEnvPrefix) + "PREFS_DIR") != "(none)" ? 1 : 0, 1);
            P.eq("debug.missing_glyphs", static_cast<int64_t>(host.draw().missingGlyphs), 0);
        }
        {
            // Read again once a second while it shows.
            const uint64_t g = port.debugGathers();
            host.tick(30);
            P.eq("debug.not_before_a_second", static_cast<int64_t>(port.debugGathers()), static_cast<int64_t>(g));
            host.tick(45);
            P.ge("debug.again_after_a_second", static_cast<double>(port.debugGathers()), static_cast<double>(g) + 1.0);
        }
        {
            // Copy all: plain text on the clipboard, every section, nothing of the program's text.
            click(Act::item, st::kCopyAll);
            const std::string text = host.clipboardText();
            bool sections = !text.empty();
            for (const pg::DebugSection& sec : card.debug())
                sections = sections && text.find("\n" + sec.title + "\n") != std::string::npos;
            P.eq("debug.copy_all", sections ? 1 : 0, 1);
            const pg::Sources files = port.running();
            const std::string first = files.empty() ? std::string() : files.front().text.substr(0, files.front().text.find('\n'));
            P.eq("debug.copy_no_program_text", !first.empty() && text.find(first) == std::string::npos ? 1 : 0, 1);
        }
        writePng(P, host, "gui.settings.debug.png");
        host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight);
        settle();
        checkPlaces(P, "debug_small", panel, card);
        P.ge("debug.scrolls_small", static_cast<double>(card.places(W(), H()).maxScroll), 1.0);
        writePng(P, host, "gui.settings.debug.small.png");
        host.resize(pg::ui::kDefaultWidth, pg::ui::kDefaultHeight);
        settle();
        card.showPage("about");
        settle();
        P.eq("debug.viewer_gone", port.debugViewers(), 0);
        {
            const uint64_t g = port.debugGathers();
            host.tick(150);
            P.eq("debug.none_once_gone", static_cast<int64_t>(port.debugGathers()), static_cast<int64_t>(g));
        }

        // ---- A11Y: the rail's entries ------------------------------------------------------------------------------------
        {
            std::vector<funkgui::A11yItem> items;
            panel.accessibility(items);
            int rail = 0, checked = 0;
            for (const funkgui::A11yItem& it : items)
                if (it.title.rfind("Settings page: ", 0) == 0)
                {
                    ++rail;
                    checked += it.checked ? 1 : 0;
                    std::string id = "?";
                    for (const st::Page& pg2 : st::pages())
                        if (it.title == std::string("Settings page: ") + pg2.title)
                            id = pg2.id;
                    P.eq("a11y.rail_" + id, it.role == funkgui::A11yRole::button ? 1 : 0, 1);
                }
            P.eq("a11y.rail_entries", rail, static_cast<int64_t>(st::pages().size()) - 1);
            P.eq("a11y.one_checked", checked, 1);
        }

        // ---- each page in both themes, and at the least size -----------------------------------------------------------
        for (const char* id : { "general", "editor", "projects", "ai", "network", "debug", "about" })
        {
            card.showPage(id);
            settle();
            checkPlaces(P, std::string("page_") + id, panel, card);
            P.eq(std::string("page_") + id + ".missing_glyphs", static_cast<int64_t>(host.draw().missingGlyphs), 0);
            if (std::string(id) != "debug")
                writePng(P, host, (std::string("gui.settings.") + id + ".png").c_str());
            host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight);
            settle();
            checkPlaces(P, std::string("least_") + id, panel, card);
            host.resize(pg::ui::kDefaultWidth, pg::ui::kDefaultHeight);
            settle();
        }
        host.keys("escape");
        settle();
        {
            funkgui::UiPreferences::get().setTheme(1);
            pg::ui::Panel paper(processor.ports(), port);
            funkgui::HeadlessHost paperHost(paper, 1, 2.0f);
            paperHost.setZoom({ 100, 125, 150, 200 }, 100);
            paperHost.settle();
            for (const char* id : { "general", "editor", "projects", "ai", "network", "debug", "about" })
            {
                paper.openSettings(id);
                paperHost.tick(2);
                paperHost.settle();
                P.eq(std::string("paper_") + id + ".shown", paper.settingsCard().pageId() == id ? 1 : 0, 1);
                writePng(P, paperHost, (std::string("gui.settings.") + id + ".paper.png").c_str());
                paperHost.keys("escape");
                paperHost.tick(2);
            }
            funkgui::UiPreferences::get().setTheme(0);
        }
        settle();
    }
}

PROBE(gui, settings)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);
    const juce::File folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                  .getChildFile(juce::String(pg::product::kEnvPrefix) + "gui-settings-" + juce::String(juce::Time::currentTimeMillis()));
    folder.deleteRecursively();
    pg::projectstore::setDirectoryForTests(folder);

    pg::Processor processor;
    pg::EditorPort& port = processor.editorPort();
    processor.settle();
    pg::ui::Panel panel(processor.ports(), port);
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.setZoom({ 100, 125, 150, 200 }, 100);
    host.settle();
    pg::ui::SettingsCard& card = panel.settingsCard();
    const auto W = [&] { return static_cast<float>(panel.width()); };
    const auto H = [&] { return static_cast<float>(panel.height()); };
    const auto settle = [&] {
        host.tick(2);
        host.settle();
    };
    const auto click = [&](Act act, int index) {
        const auto at = card.centreOf(act, index, W(), H());
        if (at)
            host.click(at->x, at->y);
        settle();
        return at.has_value();
    };

    // ---- HOST: no devices: the gear opens the card, without the Audio and MIDI page -----------------------------------
    P.eq("host.no_devices", port.devices() == nullptr ? 1 : 0, 1);
    P.eq("host.no_card_at_first", card.isOpen() ? 0 : 1, 1);
    {
        const funkgui::Point gear = panel.centreOfSettings();
        P.le("host.gear_in_top_bar", static_cast<double>(gear.y), 60.0);
        std::vector<funkgui::A11yItem> items;
        panel.accessibility(items);
        bool titled = false, clear = true;
        funkgui::Rect gearRect{};
        for (const funkgui::A11yItem& it : items)
            if (it.title == "Settings" && it.bounds.contains(gear))
            {
                titled = true;
                gearRect = it.bounds;
            }
        for (const funkgui::A11yItem& it : items)   // nothing else of the top bar under it (the theme, the switch)
            if (it.title != "Settings" && it.bounds.y < 60.0f && it.bounds.w < 400.0f && meet(it.bounds, gearRect))
                clear = false;
        P.eq("host.gear_titled", titled ? 1 : 0, 1);
        P.eq("host.gear_clear", clear ? 1 : 0, 1);
        host.click(gear.x, gear.y);
        settle();
    }
    P.eq("host.gear_opens", card.isOpen() ? 1 : 0, 1);
    P.eq("host.at_general", card.pageId() == "general" ? 1 : 0, 1);
    {
        const auto at = card.places(W(), H());
        int rail = 0;
        bool audio = false;
        for (const pg::ui::SettingsCard::Control& c : at.controls)
            if (c.act == Act::page)
            {
                ++rail;
                audio = audio || pg::ui::settings::pages()[static_cast<std::size_t>(c.index)].id == std::string("audio");
            }
        P.eq("host.rail_without_audio", audio ? 0 : 1, 1);
        P.eq("host.rail_pages", rail, static_cast<int64_t>(pg::ui::settings::pages().size()) - 1);
        P.eq("host.missing_glyphs", static_cast<int64_t>(host.draw().missingGlyphs), 0);
    }
    writePng(P, host, "gui.settings.host.png");
    P.eq("host.no_audio_page", card.showPage("audio") ? 0 : 1, 1);
    host.keys("escape");
    settle();
    P.eq("host.escape_closes", card.isOpen() ? 0 : 1, 1);
    P.eq("host.open_asks_audio", panel.openSettings("audio") && card.pageId() != "audio" ? 1 : 0, 1);
    host.keys("escape");
    settle();
    P.eq("host.no_notice", panel.mutedNoticeShown() ? 0 : 1, 1);
    P.eq("host.notice_not_drawn", countTag(host.draw(), pg::ui::kMutedTag), 0);

    // The app's window hands its devices over (its request made in the host is not taken now).
    FakeDevices fake;
    pg::Processor::WindowHooks hooks;
    hooks.devices = &fake;
    processor.setWindowHooks(hooks);
    settle();
    P.eq("hooks.devices", port.devices() == &fake ? 1 : 0, 1);
    P.eq("hooks.no_card_yet", card.isOpen() ? 0 : 1, 1);

    // ---- NOTICE: the effect's muted input ------------------------------------------------------------------------------
    if constexpr (!kInstrument)
    {
        P.eq("notice.shown", panel.mutedNoticeShown() ? 1 : 0, 1);
        const funkgui::PrimList& frame = host.draw();
        P.ge("notice.drawn", countTag(frame, pg::ui::kMutedTag), 2.0);
        P.eq("notice.missing_glyphs", static_cast<int64_t>(frame.missingGlyphs), 0);
        const funkgui::Point button = panel.centreOfMutedSettings();
        P.ge("notice.in_status_bar", static_cast<double>(button.y), static_cast<double>(H()) - 30.0);
        std::vector<funkgui::A11yItem> items;
        panel.accessibility(items);
        bool buttonTitled = false, zoomClear = true;
        funkgui::Rect buttonRect{};
        for (const funkgui::A11yItem& it : items)
            if (it.bounds.contains(button) && it.role == funkgui::A11yRole::button)
            {
                buttonTitled = it.title.find("muted") != std::string::npos;
                buttonRect = it.bounds;
            }
        for (const funkgui::A11yItem& it : items)
            if (it.title == "Zoom")
                zoomClear = buttonRect.right() <= it.bounds.x;
        P.eq("notice.button_titled", buttonTitled ? 1 : 0, 1);
        P.eq("notice.clear_of_zoom", zoomClear ? 1 : 0, 1);
        writePng(P, host, "gui.settings.muted.png");
        host.click(button.x, button.y);
        settle();
        P.eq("notice.opens_card", card.isOpen() ? 1 : 0, 1);
        host.keys("escape");
        settle();
        P.eq("notice.escape_closes", card.isOpen() ? 0 : 1, 1);
    }
    else
        P.eq("notice.none", panel.mutedNoticeShown() ? 0 : 1, 1);

    // ---- OPEN: the app's request ---------------------------------------------------------------------------------------
    processor.showSettings();
    settle();
    P.eq("open.app_request", card.isOpen() ? 1 : 0, 1);
    {
        const funkgui::PrimList& frame = host.draw();
        P.ge("open.drawn", countTag(frame, pg::ui::kSettingsTag), 20.0);
        P.eq("open.missing_glyphs", static_cast<int64_t>(frame.missingGlyphs), 0);
        std::vector<funkgui::A11yItem> items;
        panel.accessibility(items);
        P.eq("open.modal", !items.empty() && items.front().title == "Settings" ? 1 : 0, 1);   // the card's items alone
    }
    writePng(P, host, "gui.settings.png");
    checkPlaces(P, "default", panel, card);
    // The least size and a large one, the card open as the window changes.
    host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight);
    settle();
    P.eq("small.still_open", card.isOpen() ? 1 : 0, 1);
    checkPlaces(P, "small", panel, card);
    writePng(P, host, "gui.settings.small.png");
    host.resize(1800, 1100);
    settle();
    checkPlaces(P, "large", panel, card);
    host.resize(pg::ui::kDefaultWidth, pg::ui::kDefaultHeight);
    settle();

    // ---- PAPER: another editor on the same processor, in PAPER (this one's card stays open) ------------------------------
    {
        funkgui::UiPreferences::get().setTheme(1);
        pg::ui::Panel paper(processor.ports(), port);
        funkgui::HeadlessHost paperHost(paper, 1, 2.0f);
        paperHost.setZoom({ 100, 125, 150, 200 }, 100);
        paperHost.settle();
        P.eq("paper.no_card_at_first", paper.settingsCard().isOpen() ? 0 : 1, 1);   // a new editor: only a new request
        processor.showSettings();
        paperHost.tick(2);
        paperHost.settle();
        P.eq("paper.open", paper.settingsCard().isOpen() ? 1 : 0, 1);
        P.eq("paper.missing_glyphs", static_cast<int64_t>(paperHost.draw().missingGlyphs), 0);
        writePng(P, paperHost, "gui.settings.paper.png");
        paperHost.keys("escape");
        funkgui::UiPreferences::get().setTheme(0);
    }
    settle();
    P.eq("paper.this_one_open", card.isOpen() ? 1 : 0, 1);

    // ---- SECTIONS ------------------------------------------------------------------------------------------------------
    P.eq("sections.output", has(card, panel, Act::output) && has(card, panel, Act::test) && has(card, panel, Act::outputPair) ? 1 : 0, 1);
    P.eq("sections.audio", has(card, panel, Act::rate) && has(card, panel, Act::buffer) ? 1 : 0, 1);
    P.eq("sections.type_hidden", has(card, panel, Act::type) ? 0 : 1, 1);                 // one type: none shown
    P.eq("sections.input", has(card, panel, Act::input) && has(card, panel, Act::inputPair) && has(card, panel, Act::mute) ? 1 : 0,
         kInstrument ? 0 : 1);
    P.eq("sections.midi", has(card, panel, Act::midi) ? 1 : 0, kInstrument ? 1 : 0);

    // ---- KEYS: Tab over every control, Return acts -----------------------------------------------------------------------
    {
        const std::size_t n = card.places(W(), H()).controls.size();
        std::set<std::pair<int, int>> seen;
        bool ordered = true;
        const auto places = card.places(W(), H());
        for (std::size_t k = 0; k < n; ++k)
        {
            host.keys("tab");
            const auto f = card.focused(W(), H());
            if (!f)
            {
                ordered = false;
                continue;
            }
            seen.insert({ static_cast<int>(f->act), f->index });
            ordered = ordered && f->act == places.controls[k].act && f->index == places.controls[k].index;
        }
        P.eq("keys.every_control", static_cast<int64_t>(seen.size()), static_cast<int64_t>(n));
        P.eq("keys.in_order", ordered ? 1 : 0, 1);
        // A Cmd key is none of the card's: the editor gives it back (JUCE then offers it to the app's menu: Cmd-Q).
        funkgui::KeyEvent quit;
        quit.ch = U'q';
        quit.mods.cmd = true;
        P.eq("keys.cmd_to_the_menu", panel.key(quit) ? 0 : 1, 1);
        P.eq("keys.cmd_card_stays", card.isOpen() ? 1 : 0, 1);
        // On to Test, then Return.
        for (std::size_t k = 0; k < n + 1; ++k)
        {
            const auto f = card.focused(W(), H());
            if (f && f->act == Act::test)
                break;
            host.keys("tab");
        }
        host.keys("return");
        settle();
        P.eq("keys.return_on_test", fake.last() == "tone" ? 1 : 0, 1);
    }

    // ---- ACTIONS -------------------------------------------------------------------------------------------------------
    P.eq("actions.output_pair_shown", click(Act::outputPair, 1) ? 1 : 0, 1);
    P.eq("actions.output_pair", fake.last() == "pair:out:1:on" ? 1 : 0, 1);
    P.eq("actions.output_pair_shows", card.setup().outputPairs.size() == 2 && card.setup().outputPairs[1].on ? 1 : 0, 1);
    if constexpr (kInstrument)
    {
        click(Act::midi, 0);
        P.eq("actions.midi", fake.last() == "midi:iac-1:on" ? 1 : 0, 1);
        // The racing wheel (standalone/RacingWheel.h), a MIDI input among them, its status under its name.
        const auto places = card.places(W(), H());
        bool titled = false, status = false;
        for (const pg::ui::SettingsCard::Control& c : places.controls)
            titled = titled || (c.act == Act::midi && c.index == 2 && c.title.find("Logitech G29 Racing Wheel: switching") != std::string::npos);
        for (const pg::ui::SettingsCard::Line& l : places.lines)
            status = status || (l.kind == pg::ui::SettingsCard::Line::Kind::note && l.text == "switching to its full mode");
        P.eq("wheel.listed_titled", titled ? 1 : 0, 1);
        P.eq("wheel.status_shown", status ? 1 : 0, 1);
        click(Act::midi, 2);
        P.eq("wheel.switch", fake.last() == std::string("midi:") + pg::wheel::kIdentifier + ":off" ? 1 : 0, 1);
        fake.unplugWheel();
        settle();
        P.eq("wheel.gone_with_it", card.centreOf(Act::midi, 2, W(), H()).has_value() ? 0 : 1, 1);
    }
    else
    {
        click(Act::inputPair, 1);
        P.eq("actions.input_pair", fake.last() == "pair:in:1:on" ? 1 : 0, 1);
        click(Act::mute, 0);
        P.eq("actions.unmute", fake.last() == "mute:off" ? 1 : 0, 1);
        P.eq("actions.notice_gone", panel.mutedNoticeShown() ? 0 : 1, 1);
        click(Act::mute, 0);
        P.eq("actions.mute", fake.last() == "mute:on" ? 1 : 0, 1);
    }
    click(Act::test, 0);
    P.eq("actions.test", fake.last() == "tone" ? 1 : 0, 1);
    // The menus: the host's, the current choice ticked; a choice reaches the port.
    const auto choose = [&](Act act, const std::string& label, const std::string& current) {
        click(act, 0);
        const funkgui::MenuRequest* menu = host.pendingMenu();
        bool ticked = false;
        if (menu != nullptr)
            for (const funkgui::MenuItem& it : menu->items)
                ticked = ticked || (it.checked && it.label == current);
        const bool chosen = menu != nullptr && host.chooseMenuItem(label);
        settle();
        return menu != nullptr && ticked && chosen;
    };
    P.eq("menus.output", choose(Act::output, "External Headphones", "Studio Interface") ? 1 : 0, 1);
    P.eq("menus.output_called", fake.last() == "output:External Headphones" ? 1 : 0, 1);
    P.eq("menus.output_shown", card.setup().output == "External Headphones" && !has(card, panel, Act::outputPair) ? 1 : 0, 1);
    P.eq("menus.rate", choose(Act::rate, "96000 Hz", "48000 Hz") ? 1 : 0, 1);
    P.eq("menus.rate_called", fake.last() == "rate:96000" ? 1 : 0, 1);
    P.eq("menus.buffer", choose(Act::buffer, "512 samples (5.3 ms)", "256 samples (2.7 ms)") ? 1 : 0, 1);
    P.eq("menus.buffer_called", fake.last() == "buffer:512" ? 1 : 0, 1);
    {
        const auto buffer = card.places(W(), H());
        bool shown = false;
        for (const pg::ui::SettingsCard::Control& c : buffer.controls)
            shown = shown || (c.act == Act::buffer && c.label == "512 samples (5.3 ms)");
        P.eq("menus.buffer_shown", shown ? 1 : 0, 1);
    }
    if constexpr (!kInstrument)
    {
        P.eq("menus.input_none", choose(Act::input, "None", "Studio Interface") ? 1 : 0, 1);
        P.eq("menus.input_called", fake.last() == "input:" ? 1 : 0, 1);
    }
    std::printf("calls:");
    for (const std::string& c : fake.calls)
        std::printf(" %s", c.c_str());
    std::printf("\n");

    // ---- SERIAL: a device plugged in while it is open ------------------------------------------------------------------
    fake.plugIn("USB Audio CODEC");
    settle();
    {
        click(Act::output, 0);
        const funkgui::MenuRequest* menu = host.pendingMenu();
        bool listed = false;
        if (menu != nullptr)
            for (const funkgui::MenuItem& it : menu->items)
                listed = listed || it.label == "USB Audio CODEC";
        P.eq("serial.device_listed", listed ? 1 : 0, 1);
        host.cancelMenu();
        settle();
    }

    // ---- ERROR ---------------------------------------------------------------------------------------------------------
    fake.fail("The device would not open at 96000 Hz: the format is not supported by this device.", pg::DeviceSection::audio);
    settle();
    {
        const auto at = card.places(W(), H());
        float audio = -1.0f, midi = 1.0e9f, error = -1.0f;
        for (const pg::ui::SettingsCard::Line& l : at.lines)
        {
            if (l.kind == pg::ui::SettingsCard::Line::Kind::heading && l.text == "AUDIO")
                audio = l.r.y;
            if (l.kind == pg::ui::SettingsCard::Line::Kind::heading && l.text == "MIDI")
                midi = l.r.y;
            if (l.kind == pg::ui::SettingsCard::Line::Kind::error && error < 0.0f)
                error = l.r.y;
        }
        P.eq("error.shown", error >= 0.0f ? 1 : 0, 1);
        P.eq("error.under_audio", error > audio && error < midi ? 1 : 0, 1);
        std::vector<funkgui::A11yItem> items;
        panel.accessibility(items);
        bool spoken = false;
        for (const funkgui::A11yItem& it : items)
            spoken = spoken || it.title.rfind("Audio error: The device would not open", 0) == 0;
        P.eq("error.spoken", spoken ? 1 : 0, 1);
        checkPlaces(P, "error", panel, card);
    }
    writePng(P, host, "gui.settings.error.png");
    // JUCE's own error when a device will not open has line breaks: each starts a line, none is drawn as nothing.
    fake.fail("Can't open the audio device!\n\nThis may be because another application is currently using the same device - "
              "if so, you should close any other applications and try again!",
              pg::DeviceSection::output);
    settle();
    {
        std::vector<std::string> lines;
        for (const pg::ui::SettingsCard::Line& l : card.places(W(), H()).lines)
            if (l.kind == pg::ui::SettingsCard::Line::Kind::error)
                lines.push_back(l.text);
        bool plain = true;
        for (const std::string& l : lines)
            plain = plain && !l.empty() && l.find_first_of("\t\r\n") == std::string::npos && l.find("  ") == std::string::npos;
        P.eq("error.breaks_a_line", !lines.empty() && lines.front() == "Can't open the audio device!" ? 1 : 0, 1);
        P.ge("error.breaks_lines", static_cast<double>(lines.size()), 2.0);
        P.eq("error.breaks_plain", plain ? 1 : 0, 1);
        P.eq("error.breaks_missing_glyphs", static_cast<int64_t>(host.draw().missingGlyphs), 0);
    }
    fake.fail("", pg::DeviceSection::output);
    settle();

    // ---- SCROLL: at the least size, more than the card holds (twelve output pairs, an error) ----------------------------
    host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight);
    fake.crowd(12);
    fake.fail("The device would not open with these channels: the interface was unplugged, or another app holds it.",
              pg::DeviceSection::output);
    settle();
    {
        const auto at = card.places(W(), H());
        std::printf("scroll: content %.0f px in a body of %.0f\n", static_cast<double>(at.contentH), static_cast<double>(at.body.h));
        P.ge("scroll.needed", static_cast<double>(at.maxScroll), 1.0);
        P.eq("scroll.card_in_window", at.box.y >= 0.0f && at.box.bottom() <= H() ? 1 : 0, 1);
        P.eq("scroll.done_in_card", inside(at.controls.back().r, at.box) ? 1 : 0, 1);
        const pg::ui::SettingsCard::Control last = at.controls[at.controls.size() - 2];   // the last before Done
        P.eq("scroll.last_hidden_at_first", card.centreOf(last.act, last.index, W(), H()).has_value() ? 0 : 1, 1);
        for (std::size_t k = 0; k <= at.controls.size(); ++k)                         // back to it with Shift-Tab
        {
            const auto now = card.focused(W(), H());
            if (now && now->act == last.act && now->index == last.index)
                break;
            host.keys("shift+tab");
        }
        const auto f = card.focused(W(), H());
        P.eq("scroll.tab_focuses_it", f && f->act == last.act && f->index == last.index ? 1 : 0, 1);
        P.eq("scroll.tab_shows_it", card.centreOf(last.act, last.index, W(), H()).has_value() ? 1 : 0, 1);
        const float before = card.places(W(), H()).scroll;
        host.wheel(at.body.centreX(), at.body.centreY(), 0.5f);                            // up, to the top
        P.ge("scroll.was_scrolled", static_cast<double>(before), 1.0);
        P.le("scroll.wheel_back", static_cast<double>(card.places(W(), H()).scroll), 0.0);
        P.eq("scroll.first_shown", card.centreOf(Act::output, 0, W(), H()).has_value() ? 1 : 0, 1);
        // As the code scrolls (editor/Scroll.h): a notch three line pitches, natural scrolling's reversed set the same way
        // (a built WheelEvent: HeadlessHost cannot set it), the thumb dragged in proportion, a press on its track jumps.
        const auto offset = [&] { return static_cast<double>(card.places(W(), H()).scroll); };
        const pg::ui::Scroll sc = card.scrollOf(card.places(W(), H()));
        const double notch = std::min(3.0 * sc.row, static_cast<double>(sc.max()));
        const float notchDy = pg::ui::Scroll::kWheelNotch;
        host.wheel(at.body.centreX(), at.body.centreY(), -notchDy);
        P.near("scroll.notch_three_rows", offset(), notch, 0.01);
        host.wheel(at.body.centreX(), at.body.centreY(), 1000.0f);
        funkgui::WheelEvent w;
        w.x = at.body.centreX();
        w.y = at.body.centreY();
        w.dy = -notchDy;
        w.reversed = true;
        panel.wheel(w);
        P.near("scroll.reversed_as_code", offset(), notch, 0.01);
        host.wheel(at.body.centreX(), at.body.centreY(), 1000.0f);
        const auto now = card.places(W(), H());
        const funkgui::Rect area = card.barArea(now), t = card.scrollOf(now).thumb(area);
        host.drag(t.centreX(), t.centreY(), t.centreX(), t.centreY() + 10.0f, 4);
        host.settle();
        P.near("scroll.thumb_drags", offset(), 10.0 * static_cast<double>(sc.max()) / static_cast<double>(area.h - t.h), 0.05);
        P.eq("scroll.thumb_kept_open", card.isOpen() ? 1 : 0, 1);
        host.click(t.centreX(), area.bottom() - 1.0f);
        host.settle();
        P.near("scroll.track_jumps", offset(), static_cast<double>(sc.max()), 0.01);
        host.wheel(at.body.centreX(), at.body.centreY(), 1000.0f);
    }
    fake.crowd(0);
    fake.fail("", pg::DeviceSection::output);
    host.resize(pg::ui::kDefaultWidth, pg::ui::kDefaultHeight);
    settle();

    // ---- CLOSE ---------------------------------------------------------------------------------------------------------
    click(Act::done, 0);
    P.eq("close.done", card.isOpen() ? 0 : 1, 1);
    processor.showSettings();
    settle();
    P.eq("close.reopens", card.isOpen() ? 1 : 0, 1);
    host.keys("escape");
    settle();
    P.eq("close.escape", card.isOpen() ? 0 : 1, 1);
    processor.showSettings();
    settle();
    {
        // A click outside the card closes it, and reaches nothing under it (RUN stays unpressed).
        const uint64_t runs = port.statusRevision();
        const funkgui::Point run = panel.centreOfRun();
        P.eq("close.run_outside_card", card.places(W(), H()).box.contains(run) ? 0 : 1, 1);
        host.click(run.x, run.y);
        processor.settle();
        settle();
        P.eq("close.click_outside", card.isOpen() ? 0 : 1, 1);
        P.eq("close.nothing_under_it", port.statusRevision() == runs && !port.compiling() ? 1 : 0, 1);
    }
    {
        // A double-click outside the card closes it, and its second press and doubleClick reach nothing either: the
        // dial under it keeps its value (a double-click there would set it back to its default). The next one does. A
        // knob's (a real one's: the synth's first dial, its wave, is buttons, which a click chooses).
        processor.showSettings();
        settle();
        const funkgui::Rect box = card.places(W(), H()).box;
        int slot = -1;
        funkgui::Point dial{};
        for (int c = 0; c < pg::kMaxControls && slot < 0; ++c)
            if (const auto at = panel.centreOfDial(c);
                at && !box.contains(*at) && processor.control(c).type == pg::ControlType::real)
            {
                slot = c;
                dial = *at;
            }
        P.eq("close.dial_outside_card", slot >= 0 ? 1 : 0, 1);
        if (slot >= 0)
        {
            juce::AudioProcessorParameter& param = *processor.getParameters()[slot];
            const float start = param.getValue();          // the program's default: nothing turned it yet
            const float off = start > 0.5f ? 0.1f : 0.9f;
            param.setValueNotifyingHost(off);
            settle();
            host.doubleClick(dial.x, dial.y);
            settle();
            P.eq("close.double_click_outside", card.isOpen() ? 0 : 1, 1);
            P.near("close.double_click_nothing_under_it", static_cast<double>(param.getValue()), static_cast<double>(off), 1.0e-4);
            host.doubleClick(dial.x, dial.y);
            settle();
            P.near("close.next_double_click_works", static_cast<double>(param.getValue()), static_cast<double>(start), 1.0e-4);
        }
    }

    // ---- GONE: the window lets go of the devices -------------------------------------------------------------------------
    processor.showSettings();
    settle();
    P.eq("gone.open_before", card.isOpen() ? 1 : 0, 1);
    processor.setWindowHooks({});
    settle();
    P.eq("gone.card_closed", card.isOpen() ? 0 : 1, 1);
    P.eq("gone.no_notice", panel.mutedNoticeShown() ? 0 : 1, 1);
    processor.showSettings();
    settle();
    P.eq("gone.request_opens_without_audio", card.isOpen() && card.pageId() != "audio" ? 1 : 0, 1);
    host.keys("escape");
    settle();

    pagesChecks(P, processor, panel, host, card);

    pg::projectstore::setDirectoryForTests({});
    folder.deleteRecursively();
    return P.finish();
}
