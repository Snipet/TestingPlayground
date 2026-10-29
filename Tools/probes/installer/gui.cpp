// PROBE name=installer.gui timeout=240
//
// installer.gui: the installer's window (Source/installer/Panel.h), headless (funkgui::HeadlessHost: no window, no GPU),
// at its size (780 x 540), over an engine on a root of the probe's own (Fixture.h) where some parts are installed
// already: the effect's Audio Unit older than the payload's, its VST3 newer, its app the same. It writes each page,
// rasterised by FunkGui's SoftRaster at 2x, to <build>/probe-out/installer/, in GRAPHITE and PAPER:
//   gui.installer.welcome[.paper].png      the marks, the products and their versions
//   gui.installer.choose[.paper].png       the parts' switches and what is installed now; .allusers.png "All users"
//   gui.installer.installing[.paper].png   a run half done: two parts in place, the third copying, the rest waiting
//   gui.installer.done[.paper].png         what went where, Open app, Add to Claude Desktop; .noclaude.png without it
//   gui.installer.uninstall[.paper].png    every copy found, each a switch
//   gui.installer.error[.paper].png        a run that failed (a folder made unwritable): the words, what changed
// and checks, on every page in both themes:
//   LAYOUT    every control inside the window, none overlapping another, every text inside the window and clear of
//             the controls, every control and text with an accessibility title, every glyph in the font
// and on the way:
//   BUTTONS   the window's buttons are placed over the sidebar (20, 22), once
//   CHOOSE    a part whose installed copy is newer starts off, the others on; a blocked part cannot be switched on
//   KEYS      Return presses the default button (Continue, Install, Done), Escape goes back (Choose to Welcome,
//             Uninstall to Choose), Tab moves the focus to the first control and Space toggles a focused switch
//   RUN       Install runs on the Panel's thread: its progress shows while it copies, then the Done page with the report
//   DONE      Open app opens the installed app, Add to Claude Desktop opens the installed extension with Claude Desktop
//             (only while it is installed; without it, the button is off and a note says how), Done quits
//   UNINSTALL every copy listed, Move to Trash moves them (to the root's Trash), the Done page says so
#include "Probe.h"

#include "Fixture.h"

#include "installer/Engine.h"
#include "installer/Panel.h"

#include <funkgui/canvas/PrimList.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    namespace pi = playground::installer;
    using Act = pi::Panel::Act;
    using Page = pi::Panel::Page;

    class FakePort final : public pi::AppPort
    {
    public:
        int quits = 0, drags = 0, places = 0;
        std::vector<fs::path> opened, added;
        fs::path claude;
        float placedX = -1.0f, placedMiddle = -1.0f;

        void quit() override { ++quits; }
        bool openApp(const fs::path& app) override
        {
            opened.push_back(app);
            return true;
        }
        fs::path claudeDesktop() const override { return claude; }
        bool addToClaude(const fs::path& mcpb) override
        {
            added.push_back(mcpb);
            return true;
        }
        Buttons windowButtons() const override { return { 54.0f, 16.0f }; }
        void placeWindowButtons(float x, float middle) override
        {
            ++places;
            placedX = x;
            placedMiddle = middle;
        }
        void dragWindow(int) override { ++drags; }
    };

    bool meet(const funkgui::Rect& a, const funkgui::Rect& b)
    {
        return a.x < b.right() - 0.01f && b.x < a.right() - 0.01f && a.y < b.bottom() - 0.01f && b.y < a.bottom() - 0.01f;
    }

    bool inside(const funkgui::Rect& r)
    {
        return r.x >= 0.0f && r.y >= 0.0f && r.right() <= static_cast<float>(pi::Panel::kWidth)
               && r.bottom() <= static_cast<float>(pi::Panel::kHeight);
    }

    // LAYOUT, on the page as it stands.
    void checkPage(funkgui::test::Probe& P, const pi::Panel& panel, funkgui::HeadlessHost& host, const std::string& key)
    {
        const funkgui::PrimList& frame = host.draw();
        P.eq(key + ".missing_glyphs", static_cast<int64_t>(frame.missingGlyphs), 0);
        const std::vector<pi::Panel::Control> cs = panel.controls();
        const std::vector<pi::Panel::Line> ls = panel.lines();
        int outside = 0, overlaps = 0, untitled = 0, textsOut = 0, textsOver = 0;
        for (std::size_t i = 0; i < cs.size(); ++i)
        {
            outside += inside(cs[i].r) ? 0 : 1;
            untitled += cs[i].title.empty() ? 1 : 0;
            for (std::size_t j = i + 1; j < cs.size(); ++j)
                if (meet(cs[i].r, cs[j].r))
                {
                    ++overlaps;
                    std::printf("%s: %s overlaps %s\n", key.c_str(), cs[i].title.c_str(), cs[j].title.c_str());
                }
        }
        for (const pi::Panel::Line& l : ls)
        {
            textsOut += inside(l.r) ? 0 : 1;
            for (const pi::Panel::Control& c : cs)
                if (meet(l.r, c.r))
                {
                    ++textsOver;
                    std::printf("%s: \"%s\" over %s\n", key.c_str(), l.text.c_str(), c.title.c_str());
                }
        }
        std::vector<funkgui::A11yItem> items = host.accessibility();
        int a11yUntitled = 0;
        for (const funkgui::A11yItem& it : items)
            a11yUntitled += it.title.empty() ? 1 : 0;
        P.ge(key + ".controls", static_cast<double>(cs.size()), 1.0);
        P.eq(key + ".controls_inside", outside, 0);
        P.eq(key + ".controls_apart", overlaps, 0);
        P.eq(key + ".controls_titled", untitled, 0);
        P.eq(key + ".texts_inside", textsOut, 0);
        P.eq(key + ".texts_clear_of_controls", textsOver, 0);
        P.eq(key + ".a11y_titled", a11yUntitled, 0);
    }

    // The page in GRAPHITE (the probe's host) and PAPER (a host of its own for the frame), each checked.
    void shoot(funkgui::test::Probe& P, pi::Panel& panel, funkgui::HeadlessHost& host, const std::string& name, bool paper = true)
    {
        checkPage(P, panel, host, name);
        host.draw();
        const std::string path = probe::outPath(name + ".png");
        P.eq(name + ".png_written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
        if (!paper)
            return;
        {
            funkgui::HeadlessHost light(panel, 1, 2.0f);
            light.tick(1);
            checkPage(P, panel, light, name + ".paper");
            light.draw();
            const std::string lp = probe::outPath(name + ".paper.png");
            P.eq(name + ".paper.png_written", light.writePng(lp.c_str(), 2) ? 1 : 0, 1);
            std::printf("wrote %s\n", lp.c_str());
        }
        panel.attach(host);
    }

    bool has(const std::vector<pi::Panel::Control>& cs, Act a, int index = -1)
    {
        for (const pi::Panel::Control& c : cs)
            if (c.act == a && (index < 0 || c.index == index))
                return true;
        return false;
    }

    const pi::Panel::Control* findControl(const std::vector<pi::Panel::Control>& cs, Act a, int index)
    {
        for (const pi::Panel::Control& c : cs)
            if (c.act == a && c.index == index)
                return &c;
        return nullptr;
    }

    bool anyLine(const pi::Panel& panel, const std::string& what)
    {
        for (const pi::Panel::Line& l : panel.lines())
            if (l.text.find(what) != std::string::npos)
                return true;
        return false;
    }
}

PROBE(installer, gui)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);

    const fs::path root = fixture::root("installer.gui");
    const fs::path payloadDir = root / "payload";
    fixture::writePayload(payloadDir);
    const pi::Places places = pi::Places::under(root);
    std::error_code ec;
    fs::create_directories(places.root / "Applications", ec);
    fs::create_directories(places.home, ec);
    fixture::FakeSystem sys;
    pi::Engine engine(pi::Payload::read(payloadDir), places, sys);

    // Installed already: the effect's AU (to be made older), VST3 (newer) and app (the same).
    {
        const pi::Plan pre = engine.plan({ { "effect", pi::Part::au }, { "effect", pi::Part::vst3 }, { "effect", pi::Part::app } },
                                         pi::Scope::user);
        P.eq("setup.installed", engine.install(pre, {}).count(pi::State::done), 3);
        fixture::setVersion(places.home / "Library/Audio/Plug-Ins/Components/Tone.component", "1.1.0");
        fixture::setVersion(places.home / "Library/Audio/Plug-Ins/VST3/Tone.vst3", "2.0.0");
    }

    FakePort port;
    pi::Panel panel(engine, port, "Tone Installer");
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.tick(2);
    host.settle();

    // ---- BUTTONS, WELCOME --------------------------------------------------------------------------------------------
    P.eq("buttons.placed_once", port.places, 1);
    P.near("buttons.x", static_cast<double>(port.placedX), static_cast<double>(pi::Panel::kButtonsX), 1e-6);
    P.near("buttons.middle", static_cast<double>(port.placedMiddle), static_cast<double>(pi::Panel::kButtonsMiddle), 1e-6);
    P.eq("welcome.page", panel.page() == Page::welcome ? 1 : 0, 1);
    P.eq("welcome.versions_shown", anyLine(panel, "version 1.2.0") ? 1 : 0, 1);
    P.eq("welcome.both_products", anyLine(panel, "ToneSynth") && anyLine(panel, "Tone") ? 1 : 0, 1);
    shoot(P, panel, host, "gui.installer.welcome");

    // ---- KEYS, CHOOSE ------------------------------------------------------------------------------------------------
    host.keys("return");
    host.settle();
    P.eq("keys.return_continues", panel.page() == Page::choose ? 1 : 0, 1);
    host.keys("escape");
    host.settle();
    P.eq("keys.escape_goes_back", panel.page() == Page::welcome ? 1 : 0, 1);
    host.keys("return");
    host.settle();
    P.eq("choose.newer_starts_off", panel.chosen("effect", pi::Part::vst3) ? 1 : 0, 0);
    P.eq("choose.older_on", panel.chosen("effect", pi::Part::au) ? 1 : 0, 1);
    P.eq("choose.new_on", panel.chosen("synth", pi::Part::extension) ? 1 : 0, 1);
    P.eq("choose.says_upgrade", anyLine(panel, "1.1.0 installed: upgrades to 1.2.0") ? 1 : 0, 1);
    P.eq("choose.says_newer", anyLine(panel, "2.0.0 installed, newer than 1.2.0") ? 1 : 0, 1);
    P.eq("choose.says_same", anyLine(panel, "already up to date") ? 1 : 0, 1);
    shoot(P, panel, host, "gui.installer.choose");
    {
        // Tab to the first control (the first switch), Space toggles it, Space again back.
        host.keys("tab");
        const bool before = panel.chosen("effect", pi::Part::au);
        host.keys("space");
        P.eq("keys.space_toggles", panel.chosen("effect", pi::Part::au) != before ? 1 : 0, 1);
        host.keys("space");
        P.eq("keys.space_toggles_back", panel.chosen("effect", pi::Part::au) == before ? 1 : 0, 1);
        host.settle();
    }
    std::printf("choose's accessibility:\n%s", panel.a11yDump().c_str());
    panel.press(Act::scope, 1);
    host.settle();
    P.eq("choose.all_users", panel.scope() == pi::Scope::system ? 1 : 0, 1);
    P.eq("choose.all_users_says_password", anyLine(panel, "administrator's password") ? 1 : 0, 1);
    shoot(P, panel, host, "gui.installer.choose.allusers", false);
    panel.press(Act::scope, 0);
    host.settle();

    // ---- RUN: half done, then done -----------------------------------------------------------------------------------
    port.claude = root / "Claude.app";
    fs::create_directories(port.claude, ec);
    const auto waitFor = [&](const auto& cond) {
        for (int k = 0; k < 6000 && !cond(); ++k)
        {
            host.tick(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return cond();
    };
    const int copied = sys.copiesStarted.load();
    sys.holdFrom(copied + 3);
    host.keys("return");
    P.eq("run.started", panel.page() == Page::installing ? 1 : 0, 1);
    P.eq("run.third_copying", waitFor([&] { return sys.copiesStarted.load() - copied >= 3; }) ? 1 : 0, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    host.tick(3);
    P.eq("run.busy", panel.busy() ? 1 : 0, 1);
    P.eq("run.progress_shown", panel.a11yDump().find("Progress") != std::string::npos ? 1 : 0, 1);
    shoot(P, panel, host, "gui.installer.installing");
    sys.release();
    panel.waitForRun();
    host.tick(2);
    host.settle();
    P.eq("run.done_page", panel.page() == Page::done ? 1 : 0, 1);
    P.eq("run.report_ok", panel.report().ok() ? 1 : 0, 1);
    P.eq("run.parts_done", panel.report().count(pi::State::done), 6);     // 8 chosen of which: the app same, VST3 off

    // ---- DONE --------------------------------------------------------------------------------------------------------
    {
        const auto cs = panel.controls();
        P.eq("done.open_app_each", has(cs, Act::openApp, 0) && has(cs, Act::openApp, 1) ? 1 : 0, 1);
        P.eq("done.add_to_claude_each", has(cs, Act::addToClaude, 0) && has(cs, Act::addToClaude, 1) ? 1 : 0, 1);
        shoot(P, panel, host, "gui.installer.done");
        panel.press(Act::addToClaude, 1);
        P.eq("done.added_the_installed_extension",
             port.added.size() == 1
                 && port.added[0] == places.home / "Library/Application Support/Funk Test/ToneSynth/ToneSynth.mcpb" ? 1 : 0, 1);
        panel.press(Act::openApp, 0);
        P.eq("done.opened_the_installed_app", port.opened.size() == 1 && port.opened[0] == places.root / "Applications/Tone.app" ? 1 : 0, 1);
        port.claude.clear();
        host.tick(1);
        const auto off = panel.controls();
        const pi::Panel::Control* add = findControl(off, Act::addToClaude, 0);
        P.eq("done.no_claude_button_off", add != nullptr && !add->enabled ? 1 : 0, 1);
        P.eq("done.no_claude_says_how", anyLine(panel, "claude.ai/download") ? 1 : 0, 1);
        const std::size_t addedBefore = port.added.size();
        panel.press(Act::addToClaude, 0);
        P.eq("done.no_claude_opens_nothing", static_cast<int64_t>(port.added.size()), static_cast<int64_t>(addedBefore));
        shoot(P, panel, host, "gui.installer.done.noclaude", false);
        host.keys("return");
        P.eq("done.return_quits", port.quits, 1);
    }

    // ---- UNINSTALL ---------------------------------------------------------------------------------------------------
    {
        panel.show(Page::choose);
        panel.press(Act::toUninstall);
        host.settle();
        P.eq("uninstall.page", panel.page() == Page::uninstall ? 1 : 0, 1);
        int copies = 0;
        for (const auto& c : panel.controls())
            copies += c.act == Act::copy ? 1 : 0;
        P.eq("uninstall.every_copy_listed", copies, 8);
        shoot(P, panel, host, "gui.installer.uninstall");
        // A copy switched off stays off when the page looks again (every 3 s).
        int first = -1;
        for (const auto& c : panel.controls())
            if (c.act == Act::copy && first < 0)
                first = c.index;
        panel.press(Act::copy, first);
        host.tick(240);
        const pi::Panel::Control* off = findControl(panel.controls(), Act::copy, first);
        P.eq("uninstall.switch_kept_across_a_refresh", off != nullptr && !off->on ? 1 : 0, 1);
        panel.press(Act::copy, first);
        host.keys("escape");
        host.settle();
        P.eq("uninstall.escape_back", panel.page() == Page::choose ? 1 : 0, 1);
    }

    // ---- ERROR: the extension's folder made unwritable while its copy is made (as a run may meet it) ----------------
    {
        fixture::setVersion(places.home / "Library/Audio/Plug-Ins/Components/ToneSynth.component", "1.0.0");
        const fs::path dir = places.home / "Library/Application Support/Funk Test/ToneSynth";
        fixture::writeMcpb(dir / "ToneSynth.mcpb", "0.1.0", "old");
        panel.show(Page::choose);
        host.settle();
        const int base = sys.copiesStarted.load();
        sys.holdFrom(base + 2);                          // the second copy, the extension's, waits
        host.keys("return");
        P.eq("error.started", panel.page() == Page::installing ? 1 : 0, 1);
        P.eq("error.extension_copying", waitFor([&] { return sys.copiesStarted.load() - base >= 2; }) ? 1 : 0, 1);
        fs::permissions(dir, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write, fs::perm_options::remove, ec);
        sys.release();
        panel.waitForRun();
        host.tick(2);
        host.settle();
        fs::permissions(dir, fs::perms::owner_write, fs::perm_options::add, ec);
        P.eq("error.done_page", panel.page() == Page::done ? 1 : 0, 1);
        P.eq("error.reported", panel.report().ok() ? 1 : 0, 0);
        P.eq("error.says_what_was_installed",
             panel.report().error.find("Installed: ToneSynth's Audio Unit") != std::string::npos ? 1 : 0, 1);
        P.eq("error.shown", anyLine(panel, "ToneSynth's Claude Desktop extension was not installed") ? 1 : 0, 1);
        P.eq("error.old_extension_kept", pi::Engine::extensionVersion(dir / "ToneSynth.mcpb") == "0.1.0" ? 1 : 0, 1);
        P.eq("error.a11y_says_error", panel.a11yDump().find("Error: ") != std::string::npos ? 1 : 0, 1);
        shoot(P, panel, host, "gui.installer.error");
    }

    // ---- UNINSTALL, run ----------------------------------------------------------------------------------------------
    {
        panel.show(Page::uninstall);
        host.settle();
        host.keys("return");
        panel.waitForRun();
        host.tick(2);
        host.settle();
        P.eq("uninstalled.done_page", panel.page() == Page::done ? 1 : 0, 1);
        P.eq("uninstalled.ok", panel.report().ok() ? 1 : 0, 1);
        P.eq("uninstalled.none_left", static_cast<int64_t>(engine.installed().size()), 0);
        P.eq("uninstalled.says_trash", anyLine(panel, "Moved to the Trash") ? 1 : 0, 1);
        shoot(P, panel, host, "gui.installer.uninstalled", false);
    }
    return P.finish();
}
