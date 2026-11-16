// PROBE name=gui.factory timeout=300
//
// gui.factory: the FACTORY group in the editor (editor/ProjectGroups.h's note, ProjectBrowser.h, Explorer.h) over a
// processor whose factory store is this probe's own: a folder of its own, the website's signed fixtures served by a
// scripted site (Tools/probes/fixtures/factory/good-1, good-2), the fixtures' test keys (factorystore::useConfigForTests,
// probes only) and <ENV_PREFIX>FACTORY=off turned off for it alone. Nothing reaches the network.
//   GATE     with no editor nothing is asked of the site: unanswered, a yes given through the port (getFactoryProjects),
//            or the switch off (<ENV_PREFIX>FACTORY=off, the config's); the port's note says each state
//   STATES   each state of the table, in PROJECTS and in the explorer's FACTORY (folded at first, opened here), both
//            themes, a picture each: not on this machine (Get them), getting, could not fetch (no connection: Try
//            again), could not be checked (none used), off (the way to turn them on), and ready (the projects, no note).
//            The note is one row of the group, never a project: PROJECTS' FACTORY counts none, its rows are text and an
//            action, nothing is chosen; the explorer's FACTORY has the one row, uncounted
//   ACTION   PROJECTS' Get them is a yes and fetches: the answer kept, the site asked, FACTORY fills; the explorer's Get
//            them does the same; Try again after a failed fetch fetches again and fills it; the keys reach it (Down,
//            then Return); a screen reader has a button of its name in both
//   NOTPROJ  the note's rows: no menu on a right-click, nothing chosen by a click on its text, no FACTORY at all while a
//            search filters (the note is no result)
//   OFFLINE  offline with a set: FACTORY unchanged, no note, no error shown there
//   UPDATE   a set that adds, changes and withdraws a project (good-1, then good-2): FACTORY made again when the
//            generation moves (PROJECTS' and the explorer's rows the new set's); a factory project open keeps the text
//            it was opened with; one chosen in PROJECTS and withdrawn is chosen no more
//   LEAST    the note in the least window, in PROJECTS
// PNGs (probe-out/<key>/): gui.factory.<state>.png and .<state>.paper.png (PROJECTS), gui.factory.explorer.<state>.png
// and .explorer.<state>.paper.png, for nothere, getting, offline, unchecked, off, ready; gui.factory.updated.png,
// gui.factory.explorer.updated.png, gui.factory.least.png
#include "Probe.h"

#include "editor/Explorer.h"
#include "editor/Panel.h"
#include "editor/ProjectBrowser.h"
#include "editor/ProjectGroups.h"
#include "plugin/ClaudeApi.h"
#include "plugin/EditorPort.h"
#include "plugin/Factory.h"
#include "plugin/FactoryStore.h"
#include "plugin/ProjectStore.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace pg = playground;
namespace fsx = pg::factorystore;
using S = pg::ui::ExplorerItem::Section;
using L = pg::ui::ProjectBrowser::List;
using Kind = pg::FactoryNote::Kind;

namespace
{
    juce::File fixtures() { return juce::File(PROBE_SOURCE_DIR).getChildFile("Tools/probes/fixtures/factory"); }

    // The site: a fixture's folder served as the website would, offline or holding the index at the probe's word.
    class Site final : public pg::claude::Transport
    {
    public:
        explicit Site(juce::File root) : root_(std::move(root)) {}
        void serve(const juce::File& root)
        {
            const std::lock_guard<std::mutex> lock(m_);
            root_ = root;
        }
        void setOffline(bool on) { offline_.store(on); }
        void setHold(bool on) { hold_.store(on); }
        int count() const { return count_.load(); }

        pg::claude::HttpResult post(const std::string&, const std::string&, const std::string&, const pg::claude::Headers&,
                                    const std::function<bool(std::string_view)>&, const std::atomic<bool>&) override
        {
            return { 500, {}, "no POST here", -1, {} };
        }
        pg::claude::HttpResult get(const std::string&, const std::string&, const pg::claude::Headers&, const std::atomic<bool>&) override
        {
            return { 500, {}, "the store asks with getLimited", -1, {} };
        }
        pg::claude::HttpResult getLimited(const std::string&, const std::string& path, const pg::claude::Headers&,
                                          const std::function<bool(std::string_view)>& onChunk, std::size_t,
                                          const std::atomic<bool>& cancel) override
        {
            ++count_;
            while (hold_.load() && !cancel.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (cancel.load())
                return { 0, "stopped", {}, -1, {} };
            if (offline_.load())
                return { 0, "no connection to fixture.test", {}, -1, {} };
            juce::File root;
            {
                const std::lock_guard<std::mutex> lock(m_);
                root = root_;
            }
            const juce::File file = root.getChildFile(juce::String(path).substring(1));
            if (!file.existsAsFile())
                return { 404, {}, "not found", -1, {} };
            juce::MemoryBlock data;
            (void)file.loadFileAsData(data);
            (void)onChunk(std::string_view(static_cast<const char*>(data.getData()), data.getSize()));
            return { 200, {}, {}, -1, {} };
        }

    private:
        std::mutex m_;
        juce::File root_;
        std::atomic<bool> offline_{ false }, hold_{ false };
        std::atomic<int> count_{ 0 };
    };

    std::vector<fsx::Config::Key> testKeys()
    {
        std::vector<fsx::Config::Key> out;
        const juce::var keys = juce::JSON::parse(fixtures().getChildFile("keys.json").loadFileAsString());
        if (const juce::DynamicObject* o = keys.getDynamicObject())
            for (const auto& k : o->getProperties())
            {
                juce::MemoryOutputStream bytes;
                if (!juce::Base64::convertFromBase64(bytes, k.value.toString()) || bytes.getDataSize() != 65)
                    continue;
                fsx::Config::Key key;
                key.id = k.name.toString().toStdString();
                std::copy_n(static_cast<const uint8_t*>(bytes.getData()), 65, key.point.begin());
                out.push_back(std::move(key));
            }
        return out;
    }

    // A home of the probe's own (the store's folders, the projects' folder) and the process's store made from it when
    // the next processor is: the answer given (or none), the switch, the keys (none: every index refused at its check).
    struct Home
    {
        juce::File dir;
        std::shared_ptr<Site> site;
        fsx::Config config;

        Home(const juce::File& d, const char* answer, bool off = false, bool keys = true)
            : dir(d), site(std::make_shared<Site>(fixtures().getChildFile("good-1")))
        {
            dir.deleteRecursively();
            (void)dir.createDirectory();
            if (answer != nullptr)
                dir.getChildFile("factory.json").replaceWithText(juce::String("{\"answer\":\"") + answer + "\"}\n");
            fsx::setDirectoryForTests(dir);
            pg::projectstore::setDirectoryForTests(dir.getChildFile("projects"));
            config = fsx::processConfig();
            config.site = "https://fixture.test";
            config.transport = site;
            config.off = off;
            config.keys = keys ? testKeys() : std::vector<fsx::Config::Key>{};
            config.now = [] { return int64_t{ 1'800'000'000'000 }; };
            fsx::useConfigForTests(&config);
        }
        ~Home()
        {
            fsx::useConfigForTests(nullptr);
            fsx::setDirectoryForTests(juce::File());
            pg::projectstore::setDirectoryForTests(juce::File());
        }
        Home(const Home&) = delete;
        Home& operator=(const Home&) = delete;
    };

    void writePng(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const std::string& name)
    {
        const std::string path = probe::outPath(name);
        host.draw();
        P.eq(name + ".written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
    }

    // Ticks the host until `done`, kWorkWait at most (the store's thread's work).
    template <typename F>
    bool tickUntil(funkgui::HeadlessHost& host, F done)
    {
        const auto until = std::chrono::steady_clock::now() + probe::kWorkWait;
        while (std::chrono::steady_clock::now() < until)
        {
            host.tick(1);
            if (done())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return done();
    }

    // The same, with no host (no editor).
    template <typename F>
    bool waitFor(F done)
    {
        const auto until = std::chrono::steady_clock::now() + probe::kWorkWait;
        while (std::chrono::steady_clock::now() < until)
        {
            if (done())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return done();
    }

    bool storeIdle()
    {
        const std::shared_ptr<fsx::Store> s = fsx::process();
        return s != nullptr && s->idle() && s->state().loaded;
    }

    const pg::ui::projectgroups::Group* factoryGroup(const pg::ui::ProjectBrowser& b)
    {
        for (const pg::ui::projectgroups::Group& g : b.groups())
            if (g.kind == pg::ui::projectgroups::Kind::factory)
                return &g;
        return nullptr;
    }

    // The explorer scrolled from the top until its FACTORY section's first row is in view (the sections above it differ
    // by machine: CLAUDE DESKTOP shows while the MCP link listens); its centre.
    std::optional<funkgui::Point> showFactoryRow(funkgui::HeadlessHost& host, pg::ui::ExplorerItem& ex)
    {
        ex.scrollTo(0.0f);
        host.settle();
        if (!ex.sectionOpen(S::factory))
        {
            for (float y = 0.0f; !ex.centreOfHeader(S::factory) && y <= ex.contentHeight(); y += 40.0f)
                ex.scrollTo(y);
            if (const auto h = ex.centreOfHeader(S::factory))
                host.click(h->x, h->y);
            host.settle();
        }
        for (float y = 0.0f; !ex.centreOfRow(S::factory, 0) && y <= ex.contentHeight(); y += 40.0f)
            ex.scrollTo(y);
        host.settle();
        return ex.centreOfRow(S::factory, 0);
    }

    bool a11yButton(const funkgui::HeadlessHost& host, const std::string& title)
    {
        for (const funkgui::A11yItem& a : host.accessibility())
            if (a.title == title && a.role == funkgui::A11yRole::button)
                return true;
        return false;
    }

    std::vector<std::string> ownNames(const std::vector<pg::ProjectEntry>& list)
    {
        std::vector<std::string> out;
        for (const pg::ProjectEntry& e : list)
            out.push_back(pg::ownName(e.name));
        std::sort(out.begin(), out.end());
        return out;
    }

    std::vector<std::string> fixtureNames(const char* set)
    {
        std::vector<std::string> out;
        const juce::var index = juce::JSON::parse(fixtures().getChildFile(juce::String(set) + "/factory/v1/" + pg::product::kKey + "/index.json")
                                                      .loadFileAsString());
        const juce::var body = juce::JSON::parse(index["body"].toString());
        if (const juce::Array<juce::var>* ps = body["projects"].getArray())
            for (const juce::var& p : *ps)
                out.push_back(p["name"].toString().toStdString());
        std::sort(out.begin(), out.end());
        return out;
    }
}

PROBE(gui, factory)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);
    const juce::File base = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getChildFile(juce::String(pg::product::kEnvPrefix) + "gui-factory-" + juce::String(juce::Time::currentTimeMillis()));
    base.deleteRecursively();
    const std::vector<std::string> good1 = fixtureNames("good-1"), good2 = fixtureNames("good-2");
    P.ge("fixtures.good1", static_cast<double>(good1.size()), 1.0);

    // ---- GATE: no editor, nothing asked -----------------------------------------------------------------------------------
    {
        Home home(base.getChildFile("gate"), nullptr);
        pg::Processor processor;
        pg::EditorPort& port = processor.editorPort();
        P.eq("gate.loaded", waitFor(storeIdle) ? 1 : 0, 1);
        P.eq("gate.unanswered_note", port.factoryNote().kind == Kind::notHere ? 1 : 0, 1);
        port.getFactoryProjects();                       // a yes, with no editor: kept, nothing fetched
        (void)waitFor(storeIdle);
        P.eq("gate.yes_kept", fsx::process() != nullptr && fsx::process()->state().answer == fsx::Answer::yes ? 1 : 0, 1);
        port.fetchFactoryNow();
        (void)waitFor(storeIdle);
        P.eq("gate.nothing_asked", home.site->count(), 0);
        P.eq("gate.yes_note", port.factoryNote().kind == Kind::getting ? 1 : 0, 1);   // a yes, its fetch not started
    }
    {
        Home home(base.getChildFile("gate-off"), "yes", true);
        pg::Processor processor;
        pg::EditorPort& port = processor.editorPort();
        P.eq("gate.off_loaded", waitFor(storeIdle) ? 1 : 0, 1);
        port.editorOpened();
        port.fetchFactoryNow();
        (void)waitFor(storeIdle);
        P.eq("gate.off_note", port.factoryNote().kind == Kind::off ? 1 : 0, 1);
        P.eq("gate.off_nothing_asked", home.site->count(), 0);
    }

    // ---- STATES ---------------------------------------------------------------------------------------------------------
    struct State
    {
        const char* key;
        Kind kind;
        const char* answer;
        bool off, keys, offline, hold;
        const char* words;                               // a piece of what the note says
        const char* action;                              // its action ("" none)
    };
    const std::string machine = pg::kThisMachine;
    const State states[] = {
        { "nothere", Kind::notHere, nullptr, false, true, false, false, "are not on ", "Get them" },
        { "getting", Kind::getting, "yes", false, true, false, true, "Getting the factory projects...", "" },
        { "offline", Kind::offline, "yes", false, true, true, false, "could not be fetched (no connection)", "Try again" },
        { "unchecked", Kind::unchecked, "yes", false, false, false, false, "could not be checked, and were not used", "" },
        { "off", Kind::off, "yes", true, true, false, false, "FACTORY=off", "" },
        { "ready", Kind::none, "yes", false, true, false, false, "", "" },
    };
    for (const int theme : { 0, 1 })
        for (const State& s : states)
        {
            funkgui::UiPreferences::get().setTheme(theme);
            const std::string k = s.key, suffix = theme == 1 ? ".paper" : "";
            const std::string row = k + (theme == 1 ? ".paper" : "");
            Home home(base.getChildFile(k + suffix), s.answer, s.off, s.keys);
            home.site->setOffline(s.offline);
            home.site->setHold(s.hold);
            pg::Processor processor;
            pg::EditorPort& port = processor.editorPort();
            pg::ui::Panel panel(processor.ports(), port);
            funkgui::HeadlessHost host(panel, theme, 2.0f);
            host.setZoom({ 100, 125, 150, 200 }, 100);
            host.settle();
            const bool reached = tickUntil(host, [&] {
                return port.factoryNote().kind == s.kind
                    && (s.kind != Kind::none || !port.factoryProjects().empty())
                    && (s.hold || storeIdle());
            });
            P.eq(row + ".state", reached ? 1 : 0, 1);
            host.tick(4);
            host.settle();
            const pg::ui::projectgroups::NoteRow words = pg::ui::projectgroups::noteRow(port.factoryNote());
            if (s.kind != Kind::none && theme == 0)
            {
                std::printf("%s: \"%s\" [%s]\n", k.c_str(), words.text.c_str(), words.action.c_str());
                P.eq(k + ".words", words.text.find(s.words) != std::string::npos ? 1 : 0, 1);
                P.eq(k + ".action", words.action == s.action ? 1 : 0, 1);
                if (s.kind == Kind::notHere)
                    P.eq(k + ".this_machine", words.text.find(machine) != std::string::npos ? 1 : 0, 1);
            }

            // PROJECTS
            panel.openProjects();
            host.tick(2);
            host.settle();
            pg::ui::ProjectBrowser& b = panel.projectBrowser();
            const pg::ui::projectgroups::Group* g = factoryGroup(b);
            if (s.kind == Kind::none)
            {
                P.eq(row + ".browser_projects", g != nullptr && !g->note && static_cast<int>(g->members.size()) == static_cast<int>(good1.size())
                                                    && b.noteRowOf(false) < 0 ? 1 : 0, 1);
            }
            else
            {
                P.eq(row + ".browser_note", g != nullptr && g->note && g->members.empty() && g->total == 0 && b.noteRowOf(false) >= 0 ? 1 : 0, 1);
                P.eq(row + ".browser_action", (b.noteRowOf(true) >= 0) == (s.action[0] != '\0') ? 1 : 0, 1);
                P.eq(row + ".browser_none_chosen", b.chosen(L::projects) < 0 && b.selection().empty() ? 1 : 0, 1);
                P.eq(row + ".browser_no_project", ownNames(port.factoryProjects()).empty() ? 1 : 0, 1);
                if (theme == 0 && s.action[0] != '\0')
                    P.eq(k + ".browser_a11y", a11yButton(host, s.action) ? 1 : 0, 1);
            }
            writePng(P, host, "gui.factory." + k + suffix + ".png");
            host.keys("escape");
            host.settle();

            // The explorer's FACTORY: folded at first, opened here.
            pg::ui::ExplorerItem& ex = panel.explorer();
            P.eq(row + ".explorer_folded_at_first", !ex.sectionOpen(S::factory) ? 1 : 0, 1);
            const std::optional<funkgui::Point> at = showFactoryRow(host, ex);
            P.eq(row + ".explorer_row_shown", at.has_value() ? 1 : 0, 1);
            if (s.kind == Kind::none)
                P.eq(row + ".explorer_projects", ex.rows(S::factory) == static_cast<int>(good1.size()) && ex.row(S::factory, 0)->note == 0 ? 1 : 0, 1);
            else
            {
                const pg::ui::ExplorerItem::Row* r = ex.row(S::factory, 0);
                P.eq(row + ".explorer_note", ex.rows(S::factory) == 1 && r != nullptr && r->note == (s.action[0] != '\0' ? 2 : 1)
                                                 && r->label == words.text ? 1 : 0, 1);
                if (theme == 0 && s.action[0] != '\0')
                    P.eq(k + ".explorer_a11y", a11yButton(host, s.action) ? 1 : 0, 1);
            }
            writePng(P, host, "gui.factory.explorer." + k + suffix + ".png");
            home.site->setHold(false);
        }
    funkgui::UiPreferences::get().setTheme(0);

    // ---- ACTION: Get them in PROJECTS, by a click and by the keys; in the explorer; Try again --------------------------------
    {
        Home home(base.getChildFile("get"), nullptr);
        pg::Processor processor;
        pg::EditorPort& port = processor.editorPort();
        pg::ui::Panel panel(processor.ports(), port);
        funkgui::HeadlessHost host(panel, 0, 2.0f);
        host.setZoom({ 100, 125, 150, 200 }, 100);
        P.eq("action.unanswered", tickUntil(host, [&] { return port.factoryNote().kind == Kind::notHere && storeIdle(); }) ? 1 : 0, 1);
        P.eq("action.nothing_before", home.site->count(), 0);
        panel.openProjects();
        host.tick(2);
        host.settle();
        pg::ui::ProjectBrowser& b = panel.projectBrowser();
        const float W = static_cast<float>(panel.width()), H = static_cast<float>(panel.height());
        // NOTPROJ: its text's row chooses nothing; a right-click on its rows shows no menu.
        const funkgui::Point text = b.centreOfRow(L::projects, b.noteRowOf(false), W, H);
        host.click(text.x, text.y);
        host.settle();
        P.eq("notproj.text_click_nothing", b.chosen(L::projects) < 0 && b.chosenProject().empty() && home.site->count() == 0 ? 1 : 0, 1);
        host.click(text.x, text.y, funkgui::Button::right);
        host.settle();
        const funkgui::Point act = b.centreOfRow(L::projects, b.noteRowOf(true), W, H);
        host.click(act.x, act.y, funkgui::Button::right);
        host.settle();
        P.eq("notproj.no_menu", host.pendingMenu() == nullptr && home.site->count() == 0 ? 1 : 0, 1);
        // While a search filters, no FACTORY at all (the note is no result).
        host.keys("cmd+f");
        host.keys("z,z,z");
        host.settle();
        P.eq("notproj.search_hides", factoryGroup(b) == nullptr && b.noteRowOf(false) < 0 ? 1 : 0, 1);
        host.keys("escape");
        host.keys("escape");
        host.settle();
        P.eq("notproj.search_cleared", factoryGroup(b) != nullptr && b.noteRowOf(true) >= 0 ? 1 : 0, 1);
        // The keys: Down reaches the action (no project chosen), Return presses it.
        host.keys("down");
        host.settle();
        P.eq("action.keys_reach", b.noteFocused() && b.chosenProject().empty() ? 1 : 0, 1);
        host.keys("return");
        host.settle();
        P.eq("action.yes", tickUntil(host, [&] { return fsx::process() != nullptr && fsx::process()->state().answer == fsx::Answer::yes; }) ? 1 : 0, 1);
        P.eq("action.fetches", tickUntil(host, [&] { return home.site->count() > 0; }) ? 1 : 0, 1);
        P.eq("action.fills", tickUntil(host, [&] {
            const pg::ui::projectgroups::Group* g = factoryGroup(b);
            return g != nullptr && !g->note && static_cast<int>(g->members.size()) == static_cast<int>(good1.size());
        }) ? 1 : 0, 1);
        P.eq("action.port_lists", ownNames(port.factoryProjects()) == good1 ? 1 : 0, 1);
        P.eq("action.answer_on_disk", juce::JSON::parse(home.dir.getChildFile("factory.json").loadFileAsString())["answer"].toString() == "yes"
                                          ? 1 : 0, 1);
    }
    {
        Home home(base.getChildFile("get-explorer"), "no");   // "Not now", or the switch turned off: the same row
        pg::Processor processor;
        pg::EditorPort& port = processor.editorPort();
        pg::ui::Panel panel(processor.ports(), port);
        funkgui::HeadlessHost host(panel, 0, 2.0f);
        host.setZoom({ 100, 125, 150, 200 }, 100);
        P.eq("action.declined_note", tickUntil(host, [&] { return port.factoryNote().kind == Kind::notHere && storeIdle(); }) ? 1 : 0, 1);
        host.tick(4);
        host.settle();
        pg::ui::ExplorerItem& ex = panel.explorer();
        const std::optional<funkgui::Point> at = showFactoryRow(host, ex);
        P.eq("action.explorer_nothing_before", home.site->count(), 0);
        if (at)
            host.click(at->x, at->y);
        host.settle();
        P.eq("action.explorer_fills", tickUntil(host, [&] {
            host.tick(1);
            return ex.rows(S::factory) == static_cast<int>(good1.size()) && ex.row(S::factory, 0)->note == 0;
        }) ? 1 : 0, 1);
        P.eq("action.explorer_yes", fsx::process() != nullptr && fsx::process()->state().answer == fsx::Answer::yes ? 1 : 0, 1);
    }
    {
        Home home(base.getChildFile("retry"), "yes");
        home.site->setOffline(true);
        pg::Processor processor;
        pg::EditorPort& port = processor.editorPort();
        pg::ui::Panel panel(processor.ports(), port);
        funkgui::HeadlessHost host(panel, 0, 2.0f);
        host.setZoom({ 100, 125, 150, 200 }, 100);
        P.eq("retry.offline", tickUntil(host, [&] { return port.factoryNote().kind == Kind::offline && storeIdle(); }) ? 1 : 0, 1);
        const int asked = home.site->count();
        home.site->setOffline(false);
        panel.openProjects();
        host.tick(2);
        host.settle();
        pg::ui::ProjectBrowser& b = panel.projectBrowser();
        const float W = static_cast<float>(panel.width()), H = static_cast<float>(panel.height());
        P.eq("retry.nothing_by_itself", home.site->count(), asked);
        const funkgui::Point act = b.centreOfRow(L::projects, b.noteRowOf(true), W, H);
        host.click(act.x, act.y);
        host.settle();
        P.eq("retry.fills", tickUntil(host, [&] {
            const pg::ui::projectgroups::Group* g = factoryGroup(b);
            return g != nullptr && !g->note && static_cast<int>(g->members.size()) == static_cast<int>(good1.size());
        }) ? 1 : 0, 1);
        P.ge("retry.asked_again", static_cast<double>(home.site->count()), static_cast<double>(asked + 1));

        // ---- OFFLINE with a set: unchanged, no note, no error there.
        home.site->setOffline(true);
        port.fetchFactoryNow();
        (void)tickUntil(host, [&] { return storeIdle() && fsx::process()->state().failure != fsx::Failure::none; });
        host.tick(4);
        host.settle();
        const pg::ui::projectgroups::Group* g = factoryGroup(b);
        P.eq("offline.with_set_unchanged", port.factoryNote().kind == Kind::none && g != nullptr && !g->note
                                               && static_cast<int>(g->members.size()) == static_cast<int>(good1.size()) && b.noteRowOf(false) < 0
                                               ? 1 : 0, 1);
        home.site->setOffline(false);

        // ---- UPDATE: good-2 adds, changes and withdraws one. ----
        // A factory project open (unchanged), and the one good-2 withdraws chosen in PROJECTS.
        std::string withdrawn;
        for (const std::string& n : good1)
            if (std::find(good2.begin(), good2.end(), n) == good2.end())
                withdrawn = n;
        std::string why;
        const std::string opened = pg::factoryName(good1.front());
        host.keys("escape");
        host.settle();
        P.eq("update.opened", port.openProject(opened, 0, why) ? 1 : 0, 1);
        processor.settle();
        host.tick(4);
        host.settle();
        const pg::Sources openedText = port.draft().files;
        panel.openProjects();
        host.tick(2);
        host.settle();
        b.chooseProjectNamed(pg::factoryName(withdrawn), W, H);
        host.settle();
        P.eq("update.withdrawn_chosen", !withdrawn.empty() && b.chosenProject() == pg::factoryName(withdrawn) ? 1 : 0, 1);
        const uint64_t g0 = port.factoryGeneration();
        home.site->serve(fixtures().getChildFile("good-2"));
        port.fetchFactoryNow();
        P.eq("update.generation_moved", tickUntil(host, [&] { return port.factoryGeneration() != g0 && storeIdle(); }) ? 1 : 0, 1);
        P.eq("update.browser_rebuilt", tickUntil(host, [&] {
            std::vector<std::string> shown;
            for (int i = 0; i < static_cast<int>(b.rowList().size()); ++i)
                if (const pg::ProjectEntry* e = b.rowProject(i); e != nullptr && e->factory)
                    shown.push_back(pg::ownName(e->name));
            std::sort(shown.begin(), shown.end());
            shown.erase(std::unique(shown.begin(), shown.end()), shown.end());
            return shown == good2;
        }) ? 1 : 0, 1);
        P.eq("update.withdrawn_not_chosen", b.chosenProject().empty() && b.why().empty() ? 1 : 0, 1);
        writePng(P, host, "gui.factory.updated.png");
        host.keys("escape");
        host.settle();
        P.eq("update.open_keeps_text", port.draft().files == openedText && port.project().name == opened ? 1 : 0, 1);
        pg::ui::ExplorerItem& ex = panel.explorer();
        (void)showFactoryRow(host, ex);
        host.tick(4);
        host.settle();
        std::vector<std::string> rows;
        for (int i = 0; i < ex.rows(S::factory); ++i)
            rows.push_back(ex.row(S::factory, i)->label);
        std::sort(rows.begin(), rows.end());
        P.eq("update.explorer_rebuilt", rows == good2 ? 1 : 0, 1);
        writePng(P, host, "gui.factory.explorer.updated.png");
    }

    // ---- LEAST ------------------------------------------------------------------------------------------------------------
    {
        Home home(base.getChildFile("least"), nullptr);
        pg::Processor processor;
        pg::EditorPort& port = processor.editorPort();
        pg::ui::Panel panel(processor.ports(), port);
        funkgui::HeadlessHost host(panel, 0, 2.0f);
        host.setZoom({ 100, 125, 150, 200 }, 100);
        (void)tickUntil(host, [&] { return port.factoryNote().kind == Kind::notHere && storeIdle(); });
        P.eq("least.resized", host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight) ? 1 : 0, 1);
        panel.openProjects();
        host.tick(2);
        host.settle();
        pg::ui::ProjectBrowser& b = panel.projectBrowser();
        const float W = static_cast<float>(panel.width()), H = static_cast<float>(panel.height());
        const int act = b.noteRowOf(true);
        const funkgui::Point at = act >= 0 ? b.centreOfRow(L::projects, act, W, H) : funkgui::Point{ -1.0e4f, -1.0e4f };
        P.eq("least.action_shown", act >= 0 && at.x >= 0.0f && at.x <= W && at.y >= 0.0f && at.y <= H ? 1 : 0, 1);
        writePng(P, host, "gui.factory.least.png");
    }

    base.deleteRecursively();
    return P.finish();
}
