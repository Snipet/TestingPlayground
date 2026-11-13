// PROBE name=gui.updates timeout=240
//
// gui.updates: the website in the editor (editor/UpdateCards.h, the top bar's pill, the status line, the Settings card's
// Network page: SettingsNetwork.cpp), over a processor whose update check is this probe's own (a scripted site and a
// folder of the probe's: updates::useConfigForTests, factorystore::setDirectoryForTests). Nothing reaches the network
// and nothing opens a browser: Download goes to the processor's hook.
//   OFFER    a new editor with neither question answered shows the offer, both lines ticked; a click unticks one; it
//            stops nothing (Cmd-J reaches the panel behind it, a click outside it reaches the gear); while a modal card
//            (Settings) shows it is not drawn; it fits the least window; Allow stores a yes for the ticked line and a no
//            for the other, closes and says so; a second editor shows none
//   NOTNOW   (Paper) Not now: the update check "later", the factory projects unasked, said in the status; no offer in
//            the next editor; the Network page's switches make either a yes again
//   PILL     once the check answers with a newer version: the pill in the top bar ("0.2.0 is out"), clear of the
//            project's pill, and the status line's "VERSION 0.2.0 IS OUT" once a process (not again in a second
//            editor); nothing opens by itself
//   CARD     the pill opens the update card (modal); Escape and a click outside close it (Later); Download hands the
//            site's /download to the processor's hook and closes; Skip this version takes the pill away
//   NETWORK  the Network page: unanswered, on with a newer version, failed (the reason in words), off; Check now asks the
//            site again; its words say what is sent
// PNGs (probe-out/<key>/): gui.updates.offer.png, .offer.unticked.png, .offer.least.png, .offer.paper.png, .pill.png,
// .pill.paper.png, .card.png, .card.paper.png, .card.least.png, .network.unanswered.png, .network.on.png,
// .network.failed.png, .network.off.png, .network.least.png, .network.later.paper.png
#include "Probe.h"

#include "editor/Panel.h"
#include "editor/SettingsCard.h"
#include "editor/SettingsPages.h"
#include "editor/UpdateCards.h"
#include "plugin/ClaudeApi.h"
#include "plugin/EditorPort.h"
#include "plugin/FactoryStore.h"
#include "plugin/Processor.h"
#include "plugin/UpdateCheck.h"

#include "Product.h"

#include <funkgui/canvas/PrimList.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>

#include <juce_core/juce_core.h>

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
namespace up = pg::updates;
namespace st = pg::ui::settings;
using Act = pg::ui::SettingsCard::Act;

namespace
{
    const std::string kRelease =
        "{\"format\":\"funk-playground-release\",\"version\":1,\"latest\":{\"version\":\"0.2.0\",\"tag\":\"v0.2.0\","
        "\"date\":\"2026-11-20\",\"summary\":\"Samples in the panel, a faster schematic, and the chat remembers which model "
        "each project used.\"}}";

    // The site, held (a request waits until let go, or its cancel) or offline at the probe's word.
    class Site final : public pg::claude::Transport
    {
    public:
        void setHold(bool on) { hold_.store(on); }
        void setOffline(bool on) { offline_.store(on); }
        int count() const { return count_.load(); }

        pg::claude::HttpResult post(const std::string&, const std::string&, const std::string&, const pg::claude::Headers&,
                                    const std::function<bool(std::string_view)>&, const std::atomic<bool>&) override
        {
            return { 500, {}, "no POST here", -1, {} };
        }
        pg::claude::HttpResult get(const std::string&, const std::string&, const pg::claude::Headers&, const std::atomic<bool>&) override
        {
            return { 500, {}, "no", -1, {} };
        }
        pg::claude::HttpResult getLimited(const std::string&, const std::string&, const pg::claude::Headers&,
                                          const std::function<bool(std::string_view)>& onChunk, std::size_t,
                                          const std::atomic<bool>& cancel) override
        {
            ++count_;
            while (hold_.load() && !cancel.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (cancel.load())
                return { 0, "stopped", {}, -1, {} };
            if (offline_.load())
                return { 0, "no connection", {}, -1, {} };
            (void)onChunk(kRelease);
            return { 200, {}, {}, -1, {} };
        }

    private:
        std::atomic<bool> hold_{ false }, offline_{ false };
        std::atomic<int> count_{ 0 };
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

    bool inside(const funkgui::Rect& r, float w, float h) { return r.x >= 0.0f && r.y >= 0.0f && r.right() <= w && r.bottom() <= h; }
    bool meet(const funkgui::Rect& a, const funkgui::Rect& b)
    {
        return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
    }

    // The factory projects' site: nothing there (every request a 404), counted.
    class NoFactory final : public pg::claude::Transport
    {
    public:
        int count() const { return count_.load(); }
        pg::claude::HttpResult post(const std::string&, const std::string&, const std::string&, const pg::claude::Headers&,
                                    const std::function<bool(std::string_view)>&, const std::atomic<bool>&) override
        {
            return { 500, {}, "no POST here", -1, {} };
        }
        pg::claude::HttpResult get(const std::string&, const std::string&, const pg::claude::Headers&, const std::atomic<bool>&) override
        {
            ++count_;
            return { 404, {}, "not found", -1, {} };
        }
        pg::claude::HttpResult getLimited(const std::string&, const std::string&, const pg::claude::Headers&,
                                          const std::function<bool(std::string_view)>&, std::size_t, const std::atomic<bool>&) override
        {
            ++count_;
            return { 404, {}, "not found", -1, {} };
        }

    private:
        std::atomic<int> count_{ 0 };
    };

    // A home folder of the probe's own, a check over it and a factory store over a site of nothing (the environment's
    // <PREFIX>FACTORY=off turned off for it alone, its transport scripted): the process's are made from them when the
    // next processor is.
    struct Home
    {
        juce::File dir;
        std::shared_ptr<Site> site = std::make_shared<Site>();
        std::shared_ptr<NoFactory> factorySite = std::make_shared<NoFactory>();
        up::Config config;
        pg::factorystore::Config factory;

        explicit Home(const juce::File& d) : dir(d)
        {
            (void)dir.createDirectory();
            config.file = dir.getChildFile("app.json").getFullPathName().toStdString();
            config.version = pg::product::kVersion;
            config.site = "https://fixture.test";
            config.transport = site;
            config.now = [] { return int64_t{ 1'800'000'000'000 }; };
            config.userAgent = std::string(pg::product::kName) + "/" + pg::product::kVersion;
            config.offersPrices = false;
            up::useConfigForTests(&config);
            pg::factorystore::setDirectoryForTests(dir);
            factory = pg::factorystore::processConfig();
            factory.transport = factorySite;
            factory.off = false;
            factory.site = "https://fixture.test";
            pg::factorystore::useConfigForTests(&factory);
        }
        ~Home()
        {
            up::useConfigForTests(nullptr);
            pg::factorystore::useConfigForTests(nullptr);
            pg::factorystore::setDirectoryForTests(juce::File());
        }
    };

    // Ticks the host until `done`, kWorkWait at most (another thread's work: the check's).
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
}

PROBE(gui, updates)
{
    const funkgui::HeadlessGuiScope gui;
    const juce::File base = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getChildFile(juce::String(pg::product::kEnvPrefix) + "gui-updates-"
                                              + juce::String(juce::Time::currentTimeMillis()));
    base.deleteRecursively();
    funkgui::UiPreferences::get().setTheme(0);
    const std::string newer = "0.2.0";

    // ---- GRAPHITE: OFFER (Allow), PILL, CARD, NETWORK ------------------------------------------------------------------
    {
        Home home(base.getChildFile("graphite"));
        pg::Processor processor;
        pg::EditorPort& port = processor.editorPort();
        std::vector<std::string> opened;
        processor.setPageLauncher([&](const std::string& url, std::string&) {
            opened.push_back(url);
            return true;
        });
        pg::ui::Panel panel(processor.ports(), port);
        funkgui::HeadlessHost host(panel, 0, 2.0f);
        host.setZoom({ 100, 125, 150, 200 }, 100);
        const auto W = [&] { return static_cast<float>(panel.width()); };
        const auto H = [&] { return static_cast<float>(panel.height()); };
        const auto settle = [&] {
            host.tick(2);
            host.settle();
        };
        pg::ui::OfferCard& offer = panel.offerCard();
        pg::ui::UpdateCard& card = panel.updateCard();
        pg::ui::SettingsCard& settings = panel.settingsCard();

        // ---- OFFER ----
        P.eq("offer.shown", tickUntil(host, [&] { return offer.isOpen(); }) ? 1 : 0, 1);
        settle();
        P.eq("offer.both_ticked", offer.factoryTicked() && offer.updatesTicked() ? 1 : 0, 1);
        P.eq("offer.nothing_asked", home.site->count() + home.factorySite->count(), 0);
        P.eq("offer.drawn", countTag(host.draw(), pg::ui::kOfferTag) > 0 ? 1 : 0, 1);
        {
            bool group = false;
            for (const funkgui::A11yItem& a : host.accessibility())
                group = group || a.title == "Use the website?";
            P.eq("offer.a11y", group ? 1 : 0, 1);
        }
        const auto at = [&] { return offer.places(W(), H()); };
        P.eq("offer.inside", inside(at().box, W(), H()) ? 1 : 0, 1);
        writePng(P, host, "gui.updates.offer.png");
        host.click(at().factory.centreX(), at().factory.centreY());
        settle();
        P.eq("offer.untick", !offer.factoryTicked() && offer.updatesTicked() && offer.isOpen() ? 1 : 0, 1);
        writePng(P, host, "gui.updates.offer.unticked.png");

        // It stops nothing: a key reaches the panel behind it, and a click outside it reaches the gear.
        const bool panelShown = panel.codePanelShown();
        host.keys("cmd+j");
        settle();
        P.eq("offer.keys_behind", panel.codePanelShown() != panelShown && offer.isOpen() ? 1 : 0, 1);
        host.keys("cmd+j");
        settle();
        const funkgui::Point gear = panel.centreOfSettings();
        host.click(gear.x, gear.y);
        settle();
        P.eq("offer.click_behind", settings.isOpen() && offer.isOpen() ? 1 : 0, 1);
        P.eq("offer.not_under_a_card", countTag(host.draw(), pg::ui::kOfferTag), 0);
        // The Network page while neither is answered.
        settings.showPage("network");
        settle();
        P.eq("network.unanswered.shown", settings.pageId() == "network" ? 1 : 0, 1);
        P.eq("network.unanswered.check_now_off", [&] {
            for (const pg::ui::SettingsCard::Control& c : settings.places(W(), H()).controls)
                if (c.act == Act::item && c.index == st::kCheckNow)
                    return c.enabled ? 0 : 1;
            return 0;
        }(), 1);
        writePng(P, host, "gui.updates.network.unanswered.png");
        host.keys("escape");
        settle();
        P.eq("offer.back_after_the_card", !settings.isOpen() && countTag(host.draw(), pg::ui::kOfferTag) > 0 ? 1 : 0, 1);

        host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight);
        settle();
        P.eq("offer.least.inside", inside(at().box, W(), H()) && at().allow.right() <= at().box.right() ? 1 : 0, 1);
        writePng(P, host, "gui.updates.offer.least.png");
        host.resize(pg::ui::kDefaultWidth, pg::ui::kDefaultHeight);
        settle();

        // Allow, the site held so the answer's notice shows before the check's.
        home.site->setHold(true);
        host.click(at().allow.centreX(), at().allow.centreY());
        host.tick(2);
        P.eq("offer.allow.closed", offer.isOpen() ? 1 : 0, 0);
        P.eq("offer.allow.said", panel.statusText().find("UPDATE NOTES ARE ON") != std::string::npos ? 1 : 0, 1);
        {
            const pg::UpdateState u = port.updateState();
            P.eq("offer.allow.answers", u.updatesOn && u.factory == 2 ? 1 : 0, 1);
        }
        P.eq("offer.allow.no_pill_yet", panel.centreOfUpdatePill().has_value() ? 1 : 0, 0);
        home.site->setHold(false);

        // ---- PILL ----
        P.eq("pill.shown", tickUntil(host, [&] { return panel.centreOfUpdatePill().has_value(); }) ? 1 : 0, 1);
        settle();
        P.eq("pill.one_request", home.site->count(), 1);
        P.eq("pill.status_once", panel.statusText() == "VERSION " + newer + " IS OUT" ? 1 : 0, 1);
        P.eq("pill.nothing_opens", card.isOpen() || offer.isOpen() || settings.isOpen() ? 1 : 0, 0);
        {
            bool named = false;
            for (const funkgui::A11yItem& a : host.accessibility())
                named = named || a.title == "A new version is out";
            P.eq("pill.a11y", named ? 1 : 0, 1);
            // Clear of the project's pill (its accessibility item's bounds) and inside the top bar.
            std::optional<funkgui::Rect> pill, project;
            for (const funkgui::A11yItem& a : host.accessibility())
            {
                if (a.title == "A new version is out")
                    pill = a.bounds;
                else if (a.title == "Projects")
                    project = a.bounds;
            }
            P.eq("pill.clear_of_project", pill && project && !meet(*pill, *project) && pill->y >= 0.0f && pill->bottom() < 60.0f ? 1 : 0, 1);
        }
        writePng(P, host, "gui.updates.pill.png");
        {
            // A second editor: the pill, no offer, no notice again.
            pg::ui::Panel second(processor.ports(), port);
            funkgui::HeadlessHost secondHost(second, 0, 2.0f);
            (void)tickUntil(secondHost, [&] { return second.centreOfUpdatePill().has_value(); });
            secondHost.settle();
            P.eq("pill.second_editor", second.centreOfUpdatePill().has_value() && !second.offerCard().isOpen() ? 1 : 0, 1);
            P.eq("pill.second_editor.no_notice", second.statusText().find("IS OUT") == std::string::npos ? 1 : 0, 1);
        }

        // ---- CARD ----
        const auto openCard = [&] {
            const std::optional<funkgui::Point> p = panel.centreOfUpdatePill();
            if (p)
                host.click(p->x, p->y);
            settle();
            return card.isOpen();
        };
        P.eq("card.opens", openCard() ? 1 : 0, 1);
        P.eq("card.version", card.version() == newer ? 1 : 0, 1);
        P.eq("card.inside", inside(card.places(W(), H()).box, W(), H()) ? 1 : 0, 1);
        writePng(P, host, "gui.updates.card.png");
        host.keys("escape");
        settle();
        P.eq("card.escape_later", card.isOpen() ? 1 : 0, 0);
        P.eq("card.outside_later", openCard() && [&] {
            host.click(20.0f, H() - 60.0f);
            settle();
            return !card.isOpen();
        }() ? 1 : 0, 1);
        P.eq("card.later_keeps_pill", panel.centreOfUpdatePill().has_value() ? 1 : 0, 1);
        host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight);
        settle();
        P.eq("card.least", openCard() && inside(card.places(W(), H()).box, W(), H()) ? 1 : 0, 1);
        writePng(P, host, "gui.updates.card.least.png");
        host.keys("escape");
        host.resize(pg::ui::kDefaultWidth, pg::ui::kDefaultHeight);
        settle();
        P.eq("card.download.open", openCard() ? 1 : 0, 1);
        {
            const pg::ui::UpdateCard::Places c = card.places(W(), H());
            host.click(c.download.centreX(), c.download.centreY());
            settle();
        }
        P.eq("card.download.hook", opened.size() == 1 && opened[0] == std::string(pg::factorystore::kSite) + "/download" ? 1 : 0, 1);
        P.eq("card.download.closed", card.isOpen() ? 1 : 0, 0);
        P.eq("card.download.said", panel.statusText().find("DOWNLOAD PAGE") != std::string::npos ? 1 : 0, 1);

        // ---- NETWORK ----
        const auto control = [&](int code) -> std::optional<funkgui::Point> {
            return settings.centreOf(Act::item, code, W(), H());
        };
        panel.openSettings("network");
        settle();
        writePng(P, host, "gui.updates.network.on.png");
        {
            bool said = false;
            for (const pg::ui::SettingsCard::Line& l : settings.places(W(), H()).lines)
                said = said || l.text.find("is out (you have") != std::string::npos;
            P.eq("network.on.says_newer", said ? 1 : 0, 1);
        }
        home.site->setOffline(true);
        const int before = home.site->count();
        if (const auto p = control(st::kCheckNow))
            host.click(p->x, p->y);
        P.eq("network.check_now.asks", tickUntil(host, [&] {
                 return home.site->count() > before && port.updateState().check == pg::UpdateState::Check::failed;
             }) ? 1 : 0, 1);
        settle();
        {
            bool said = false;
            for (const pg::ui::SettingsCard::Line& l : settings.places(W(), H()).lines)
                said = said || (l.kind == pg::ui::SettingsCard::Line::Kind::error && l.text.find("no connection") != std::string::npos);
            P.eq("network.failed.says_why", said ? 1 : 0, 1);
        }
        writePng(P, host, "gui.updates.network.failed.png");
        home.site->setOffline(false);
        host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight);
        settle();
        writePng(P, host, "gui.updates.network.least.png");
        host.resize(pg::ui::kDefaultWidth, pg::ui::kDefaultHeight);
        settle();
        if (const auto p = control(st::kCheckUpdates))
            host.click(p->x, p->y);
        settle();
        P.eq("network.switch_off", !port.updateState().updatesOn ? 1 : 0, 1);
        (void)tickUntil(host, [&] { return !panel.centreOfUpdatePill().has_value(); });
        P.eq("network.switch_off.no_pill", panel.centreOfUpdatePill().has_value() ? 1 : 0, 0);
        writePng(P, host, "gui.updates.network.off.png");
        {
            bool sends = false;
            for (const pg::ui::SettingsCard::Line& l : settings.places(W(), H()).lines)
                sends = sends || l.text.find("name and version") != std::string::npos;
            P.eq("network.says_what_is_sent", sends ? 1 : 0, 1);
        }
        if (const auto p = control(st::kCheckUpdates))
            host.click(p->x, p->y);
        settle();
        host.keys("escape");
        (void)tickUntil(host, [&] { return panel.centreOfUpdatePill().has_value(); });

        // Skip this version: the pill goes.
        P.eq("card.skip.open", openCard() ? 1 : 0, 1);
        {
            const pg::ui::UpdateCard::Places c = card.places(W(), H());
            host.click(c.skip.centreX(), c.skip.centreY());
            settle();
        }
        P.eq("card.skip.no_pill", tickUntil(host, [&] { return !panel.centreOfUpdatePill().has_value(); }) && !card.isOpen() ? 1 : 0, 1);
        P.eq("card.skip.kept", port.updateState().skipped == newer ? 1 : 0, 1);
        P.eq("card.no_browser_but_the_hook", static_cast<int64_t>(opened.size()), 1);
    }

    // ---- PAPER: OFFER (Not now), again from Settings, PILL, CARD ------------------------------------------------------
    {
        funkgui::UiPreferences::get().setTheme(1);
        Home home(base.getChildFile("paper"));
        pg::Processor processor;
        pg::EditorPort& port = processor.editorPort();
        processor.setPageLauncher([](const std::string&, std::string&) { return true; });
        {
            pg::ui::Panel panel(processor.ports(), port);
            funkgui::HeadlessHost host(panel, 1, 2.0f);
            const auto W = [&] { return static_cast<float>(panel.width()); };
            const auto H = [&] { return static_cast<float>(panel.height()); };
            const auto settle = [&] {
                host.tick(2);
                host.settle();
            };
            pg::ui::OfferCard& offer = panel.offerCard();
            P.eq("paper.offer.shown", tickUntil(host, [&] { return offer.isOpen(); }) ? 1 : 0, 1);
            settle();
            writePng(P, host, "gui.updates.offer.paper.png");
            const pg::ui::OfferCard::Places at = offer.places(W(), H());
            host.click(at.notNow.centreX(), at.notNow.centreY());
            settle();
            const pg::UpdateState u = port.updateState();
            P.eq("notnow.closed", offer.isOpen() ? 1 : 0, 0);
            P.eq("notnow.answers", u.updatesAnswered && !u.updatesOn && u.factory == 0 ? 1 : 0, 1);
            P.eq("notnow.said", panel.statusText().find("NOT NOW") != std::string::npos ? 1 : 0, 1);
            P.eq("notnow.nothing_asked", home.site->count(), 0);
            panel.openSettings("network");
            settle();
            writePng(P, host, "gui.updates.network.later.paper.png");
            pg::ui::SettingsCard& settings = panel.settingsCard();
            if (const auto p = settings.centreOf(Act::item, st::kFactoryProjects, W(), H()))
                host.click(p->x, p->y);
            settle();
            if (const auto p = settings.centreOf(Act::item, st::kCheckUpdates, W(), H()))
                host.click(p->x, p->y);
            settle();
            const pg::UpdateState v = port.updateState();
            P.eq("again_from_settings", v.updatesOn && v.factory == 1 ? 1 : 0, 1);
            P.eq("again_from_settings.factory_fetches", tickUntil(host, [&] { return home.factorySite->count() > 0; }) ? 1 : 0, 1);
            host.keys("escape");
            P.eq("paper.pill.shown", tickUntil(host, [&] { return panel.centreOfUpdatePill().has_value(); }) ? 1 : 0, 1);
            settle();
            writePng(P, host, "gui.updates.pill.paper.png");
            if (const std::optional<funkgui::Point> p = panel.centreOfUpdatePill())
                host.click(p->x, p->y);
            settle();
            P.eq("paper.card.open", panel.updateCard().isOpen() ? 1 : 0, 1);
            writePng(P, host, "gui.updates.card.paper.png");
            host.keys("escape");
            settle();
        }
        {
            // Not offered again by itself: the next editor shows none.
            pg::ui::Panel next(processor.ports(), port);
            funkgui::HeadlessHost host(next, 1, 2.0f);
            for (int i = 0; i < 30; ++i)
                host.tick(1);
            host.settle();
            P.eq("notnow.never_again", next.offerCard().isOpen() ? 1 : 0, 0);
        }
        funkgui::UiPreferences::get().setTheme(0);
    }

    // ---- A picture's Panel never makes the offer, nor tells the processor an editor opened ---------------------------
    {
        Home home(base.getChildFile("picture"));
        pg::Processor processor;
        pg::EditorPort& port = processor.editorPort();
        pg::ui::Panel picture(processor.ports(), port, true);
        funkgui::HeadlessHost host(picture, 0, 2.0f);
        for (int i = 0; i < 30; ++i)
            host.tick(1);
        (void)tickUntil(host, [&] { return port.updateState().loaded; });
        host.tick(5);
        P.eq("picture.no_offer", picture.offerCard().isOpen() ? 1 : 0, 0);
        P.eq("picture.offer_due_still", port.updateState().offer ? 1 : 0, 1);
    }

    base.deleteRecursively();
    return P.finish();
}
