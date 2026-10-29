// PROBE name=gui.settings timeout=180
//
// gui.settings: SETTINGS, the Standalone's audio and MIDI devices in the editor's own card (editor/SettingsCard.h), and
// the effect's muted-input notice, headless over a real Processor whose window hooks the probe sets as the app's
// window does (Processor::setWindowHooks), with a DevicePort of its own (FakeDevices: a few devices, made up; every
// action recorded and applied as the app's would), to <build>/probe-out/:
//   gui.settings.png        the card in GRAPHITE, as the app opens it
//   gui.settings.paper.png  in PAPER
//   gui.settings.small.png  at the least size (kMinWidth x kMinHeight)
//   gui.settings.error.png  a device's error under its section
//   gui.settings.muted.png  the effect's notice in the status bar (the synth has none)
// and checks:
//   HOST     no devices (a host's window): no card and no notice; the app's request and openSettings do nothing
//   NOTICE   the effect: with the devices handed over, the notice and its Settings... button in the status bar, clear of
//            the zoom; the button opens the card. The synth: none
//   OPEN     the app's request (Processor::showSettings) opens the card, over the panel (the rest takes no click)
//   PLACES   at the default size, the least and a large one: every control inside the card (and in its body, unscrolled)
//            and none over another or over a text; each with an accessibility title
//   SECTIONS the effect's Input (its device, its pairs, Mute input) and no MIDI; the synth's MIDI (an input a switch)
//            and no Input
//   KEYS     Tab reaches every control once, in order, the focus ring on it; Return on Test plays the tone
//   ACTIONS  a channel pair's switch, a MIDI input's switch, the mute's switch (the notice gone once unmuted) and Test
//            call the port; the output device, the sample rate and the buffer size chosen through their menus (the
//            host's, the current one ticked) call it with those values, and the card shows them
//   SERIAL   a device plugged in while the card is open: the output's menu lists it
//   ERROR    the port's error shown under its section (Audio), in words to a screen reader too
//   SCROLL   at the least size, more than the card holds: it scrolls, Done stays in the card, Tab scrolls the focused
//            control into view, the wheel back
//   CLOSE    Done, Escape and a click outside the card close it (the click reaching nothing under it: RUN unpressed)
//   GONE     the hooks let go of the devices (the window closing): the card and the notice go
#include "Probe.h"

#include "editor/Panel.h"
#include "editor/SettingsCard.h"
#include "plugin/DevicePort.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"
#include "plugin/ProjectStore.h"

#include "Product.h"

#include <funkgui/canvas/Prim.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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
                s_.midi = { { "IAC Driver Bus 1", "iac-1", false }, { "KeyStep 37", "keystep-37", true } };
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

        // The world changing: a device plugged in, an error.
        void plugIn(const std::string& name)
        {
            s_.outputs.push_back(name);
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
            in = in && inside(c.r, at.box);
            body = body && (c.act == Act::done || inside(c.r, at.body));
            for (std::size_t j = 0; j < i; ++j)
                apart = apart && !meet(c.r, at.controls[j].r);
            for (const pg::ui::SettingsCard::Line& l : at.lines)
                clear = clear && !meet(c.r, l.r);
        }
        bool linesIn = true;
        for (const pg::ui::SettingsCard::Line& l : at.lines)
            linesIn = linesIn && inside(l.r, at.body);
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

    // ---- HOST: no devices -------------------------------------------------------------------------------------------
    P.eq("host.no_devices", port.devices() == nullptr ? 1 : 0, 1);
    processor.showSettings();
    settle();
    P.eq("host.request_no_card", card.isOpen() ? 0 : 1, 1);
    P.eq("host.open_refused", panel.openSettings() ? 0 : 1, 1);
    P.eq("host.no_notice", panel.mutedNoticeShown() ? 0 : 1, 1);
    P.eq("host.nothing_drawn", countTag(host.draw(), pg::ui::kMutedTag) + countTag(host.draw(), pg::ui::kSettingsTag), 0);

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
    P.eq("gone.request_nothing", card.isOpen() ? 0 : 1, 1);

    pg::projectstore::setDirectoryForTests({});
    folder.deleteRecursively();
    return P.finish();
}
