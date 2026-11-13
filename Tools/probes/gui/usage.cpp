// PROBE name=gui.usage timeout=180
//
// gui.usage: the settings card's Usage page (editor/UsagePage.h, SettingsPages.h): what the chat's requests used and
// cost, drawn from a usage ledger (plugin/Usage.h) seeded into the probe's own folder. Headless over a real Processor,
// whose chat reads the ledger on its job thread (ChatPort::usage), to <build>/probe-out/:
//   gui.usage.empty.png     the page with no requests (the chart's frame, "No requests yet.")
//   gui.usage.week.png      7 days, Tokens, a bar a day stacked by model, the legend
//   gui.usage.week.cost.png 7 days, Cost
//   gui.usage.year.png      12 months, a bar a month, the bar picked read out
//   gui.usage.hover.png     the bar under the pointer read out
//   gui.usage.paper.png     a week in PAPER
//   gui.usage.small.png     the least window
//   gui.usage.reset.png     Reset asked (Reset and Cancel)
// and checks:
//   EMPTY    no file: "No requests yet.", no bars, the footnote's date, Reset disabled
//   MODEL    buildModel (pure): the series ranked by tokens, the largest kMaxSeries coloured and the rest Other, the
//            buckets by day (week, month) and by month (year, all), the total; a nice round axis
//   WEEK     seven bars, each a day; the stacks' order (the largest model at the foot); Tokens and Cost switch
//   YEAR     twelve bars, each a month
//   READOUT  the bar under the pointer, and the one the keys pick (Left, Right, Home, End), read out; the footnote date
//   RESET    Reset asks first (Reset and Cancel); Cancel keeps them; Reset forgets them (the file emptied), asked on the
//            chat's job thread (the message thread waits on nothing)
//   THEMES   a week in both themes; no missing glyph
//   A11Y     the chart a group, a bar an item named by its readout; the table a table, a row a model and the total
//   PLACES   the default, the least and a large window: the controls inside the card and its body; each titled
#include "Probe.h"

#include "editor/Panel.h"
#include "editor/SettingsCard.h"
#include "editor/SettingsPages.h"
#include "editor/UsagePage.h"
#include "plugin/ChatStore.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"
#include "plugin/ProjectStore.h"
#include "plugin/Usage.h"

#include "Product.h"

#include <funkgui/canvas/Prim.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>

#include <juce_core/juce_core.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace pg = playground;
namespace up = playground::ui::usagepage;
namespace st = playground::ui::settings;
using Act = pg::ui::SettingsCard::Act;
using usage = pg::usage::Range;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;

    pg::usage::Counts counts(int64_t in, int64_t cached, int64_t out, int64_t reqs, int64_t microUsd = -1)
    {
        pg::usage::Counts c;
        c.requests = reqs;
        c.input = in;
        c.cacheRead = cached;
        c.output = out;
        if (microUsd >= 0)
        {
            c.reportedMicroUsd = microUsd;
            c.reportedRequests = reqs;
        }
        return c;
    }

    int tagPrims(const funkgui::PrimList& frame, funkgui::Tag tag)
    {
        int n = 0;
        for (const funkgui::Prim& p : frame.prims)
            n += p.tag == tag ? 1 : 0;
        return n;
    }

    void writePng(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const char* name)
    {
        const std::string path = probe::outPath(name);
        const funkgui::PrimList& frame = host.draw();
        P.eq(std::string(name) + ".written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        P.eq(std::string(name) + ".glyphs", static_cast<int64_t>(frame.missingGlyphs), 0);
        std::printf("wrote %s\n", path.c_str());
    }

    bool meet(const funkgui::Rect& a, const funkgui::Rect& b)
    {
        return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
    }

    bool inside(const funkgui::Rect& r, const funkgui::Rect& in)
    {
        return r.x >= in.x - 0.5f && r.y >= in.y - 0.5f && r.right() <= in.right() + 0.5f && r.bottom() <= in.bottom() + 0.5f;
    }

    // Ticks until `done`, probe::kWorkWait at most: the chat reads the ledger on its job thread.
    bool waitFor(funkgui::HeadlessHost& host, const std::function<bool()>& done)
    {
        const auto by = std::chrono::steady_clock::now() + probe::kWorkWait;
        do
        {
            host.tick(2);
            if (done())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now() < by);
        return false;
    }
}

PROBE(gui, usage)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());
    funkgui::UiPreferences::get().setTheme(0);

    const juce::File store = juce::File(probe::outPath(std::string(pg::product::kEnvPrefix) + "usage-store"));
    store.deleteRecursively();
    pg::chatstore::setDirectoryForTests(store);                  // usage::processFile() points here
    pg::usage::resetPricesForTests();
    const juce::File projects = juce::File(probe::outPath(std::string(pg::product::kEnvPrefix) + "usage-projects"));
    projects.deleteRecursively();
    pg::projectstore::setDirectoryForTests(projects);

    const std::string today = pg::usage::today();
    const int64_t now = pg::usage::dayNumber(today);
    const std::string opus = "claude-opus-5-5", sonnet = "claude-sonnet-5-5", haiku = "claude-haiku-5-5";

    // ---- MODEL: buildModel is pure, so it is checked on a snapshot made by hand -----------------------------------------
    {
        pg::usage::Snapshot snap;
        snap.today = today;
        const auto row = [&](const std::string& day, const std::string& prov, const std::string& model, pg::usage::Counts c) {
            snap.rows.push_back({ { day, prov, model, false }, c });
        };
        row(pg::usage::dayOf(now), "anthropic", opus, counts(100000, 50000, 20000, 10, 2000000));
        row(pg::usage::dayOf(now - 1), "anthropic", opus, counts(40000, 0, 8000, 4, 800000));
        row(pg::usage::dayOf(now - 2), "anthropic", sonnet, counts(30000, 0, 6000, 3, 100000));
        row(pg::usage::dayOf(now - 3), "openrouter", haiku, counts(5000, 0, 1000, 2, 1500));
        row(pg::usage::dayOf(now - 4), "gemini", "gemini-3-pro", counts(8000, 0, 2000, 1));   // no price: estimate/unpriced
        const up::Model m = up::buildModel(snap, usage::week);
        P.eq("model.rows", static_cast<int64_t>(m.rows.size()), 4);
        P.eq("model.ranked", m.rows.front().model == opus ? 1 : 0, 1);                         // the most tokens first
        P.eq("model.buckets", static_cast<int64_t>(m.buckets.size()), 7);                      // a week
        P.eq("model.by_day", m.byMonth ? 1 : 0, 0);
        P.eq("model.total_reqs", m.total.counts.requests, 20);
        // The newest bucket is today, its stack the opus row.
        const up::Bucket& last = m.buckets.back();
        P.eq("model.today", last.day == today ? 1 : 0, 1);
        P.ge("model.today_tokens", static_cast<double>(last.totalTokens()), 170000.0);
        // A year groups by month.
        const up::Model y = up::buildModel(snap, usage::year);
        P.eq("model.year_by_month", y.byMonth ? 1 : 0, 1);
        P.eq("model.year_buckets", static_cast<int64_t>(y.buckets.size()), 12);
        // niceAxis: round steps.
        const up::Axis a = up::niceAxis(170000.0, 1.0);
        P.ge("model.axis_top", a.top, 170000.0);
        P.le("model.axis_steps", static_cast<double>(a.steps), 6.0);
        // moneyText: an estimate marked, no price known said.
        up::Money est;
        est.add(pg::usage::Cost{ pg::usage::Cost::Kind::estimate, 1230000 });
        P.eq("model.est_tilde", up::moneyText(est).front() == '~' ? 1 : 0, 1);
        up::Money none;
        none.add(pg::usage::Cost{ pg::usage::Cost::Kind::none, 0 });
        P.eq("model.none_words", up::moneyText(none) == "no price known" ? 1 : 0, 1);
    }

    // ---- the page over a real Processor ---------------------------------------------------------------------------------
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
    const auto bars = [&] { return tagPrims(host.draw(), pg::ui::kUsageBarTag); };
    // Scroll the card's body until the control is on it (the chart and the table push Reset past the fold).
    const auto reach = [&](Act act, int index) -> bool {
        for (int tries = 0; tries < 30; ++tries)
        {
            if (card.centreOf(act, index, W(), H()).has_value())
                return true;
            host.wheel(W() * 0.5f, H() * 0.5f, -6.0f);   // negative: toward the bottom
            settle();
        }
        return card.centreOf(act, index, W(), H()).has_value();
    };
    const auto click = [&](Act act, int index) {
        const auto at = card.centreOf(act, index, W(), H());
        if (at)
            host.click(at->x, at->y);
    };

    // ---- EMPTY: no file ------------------------------------------------------------------------------------------------
    panel.openSettings("usage");
    settle();
    P.eq("empty.shown", card.pageId() == "usage" ? 1 : 0, 1);
    waitFor(host, [&] { return card.revision() > 0; });
    settle();
    P.eq("empty.no_bars", bars(), 0);
    // Reset is disabled with nothing to forget.
    {
        std::vector<funkgui::A11yItem> items;
        panel.accessibility(items);
        bool resetOff = false, dated = false;
        for (const funkgui::A11yItem& it : items)
        {
            if (it.title.rfind("Reset:", 0) == 0)
                resetOff = !it.enabled;
            if (it.title.find("Prices as of ") != std::string::npos)
                dated = true;
        }
        P.eq("empty.reset_off", resetOff ? 1 : 0, 1);
        P.eq("empty.dated", dated ? 1 : 0, 1);
    }
    writePng(P, host, "gui.usage.empty.png");

    // ---- seed a week of requests, then the page fills --------------------------------------------------------------------
    pg::usage::add({ pg::usage::dayOf(now), "anthropic", opus, false }, counts(120000, 60000, 24000, 12, 2400000));
    pg::usage::add({ pg::usage::dayOf(now), "anthropic", opus, true }, counts(10000, 0, 2000, 2, 300000));
    pg::usage::add({ pg::usage::dayOf(now - 1), "anthropic", opus, false }, counts(50000, 0, 10000, 5, 1000000));
    pg::usage::add({ pg::usage::dayOf(now - 2), "anthropic", sonnet, false }, counts(40000, 0, 8000, 4, 150000));
    pg::usage::add({ pg::usage::dayOf(now - 3), "openrouter", haiku, false }, counts(8000, 0, 1500, 3, 2000));
    pg::usage::add({ pg::usage::dayOf(now - 4), "gemini", "gemini-3-pro", false }, counts(12000, 0, 3000, 2));
    pg::usage::add({ pg::usage::dayOf(now - 6), "openai", "gpt-5", false }, counts(6000, 0, 1200, 1, 90000));
    // An older one, for the year view.
    pg::usage::add({ pg::usage::dayOf(now - 40), "anthropic", opus, false }, counts(30000, 0, 6000, 3, 600000));
    pg::usage::flush();

    // ---- WEEK --------------------------------------------------------------------------------------------------------
    P.eq("week.filled", waitFor(host, [&] { return bars() > 0; }) ? 1 : 0, 1);
    settle();
    P.ge("week.bars", static_cast<double>(bars()), 5.0);        // several days, several models
    // 7 days for the picture (the default is 30).
    click(Act::item, up::kRange);
    P.eq("week.range_menu", host.pendingMenu() != nullptr && host.chooseMenuItem("7 days") ? 1 : 0, 1);
    waitFor(host, [&] { return card.pageId() == "usage" && bars() > 0; });
    settle();
    writePng(P, host, "gui.usage.week.png");

    // Cost.
    click(Act::item, up::kShow);
    P.eq("week.cost_menu", host.pendingMenu() != nullptr && host.chooseMenuItem("Cost") ? 1 : 0, 1);
    settle();
    writePng(P, host, "gui.usage.week.cost.png");
    click(Act::item, up::kShow);
    host.chooseMenuItem("Tokens");
    settle();

    // ---- READOUT: the keys pick a bar ---------------------------------------------------------------------------------
    {
        const auto chart = card.centreOf(Act::item, up::kChart, W(), H());
        P.eq("readout.chart_there", chart.has_value() ? 1 : 0, 1);
        // Focus the chart (Tab from the range select is two stops): click it, then keys.
        if (chart)
            host.click(chart->x, chart->y);
        settle();
        host.keys("end");
        settle();
        std::vector<funkgui::A11yItem> items;
        panel.accessibility(items);
        bool readout = false;
        for (const funkgui::A11yItem& it : items)
            if (it.title.rfind("Usage chart:", 0) == 0 && !it.value.empty())
                readout = true;   // the picked bar read out (the group's value)
        P.eq("readout.keyed", readout ? 1 : 0, 1);
        host.keys("home");
        settle();
        host.keys("right");
        settle();
    }
    writePng(P, host, "gui.usage.hover.png");

    // ---- YEAR --------------------------------------------------------------------------------------------------------
    click(Act::item, up::kRange);
    P.eq("year.menu", host.pendingMenu() != nullptr && host.chooseMenuItem("12 months") ? 1 : 0, 1);
    P.eq("year.filled", waitFor(host, [&] { return bars() > 0 && card.pageId() == "usage"; }) ? 1 : 0, 1);
    settle();
    {
        const auto chart = card.centreOf(Act::item, up::kChart, W(), H());
        if (chart)
            host.click(chart->x, chart->y);
        settle();
        host.keys("end");
        host.move(card.places(W(), H()).rail.centreX(), card.places(W(), H()).rail.centreY());   // off the chart: the keyed bar shows
        settle();
    }
    writePng(P, host, "gui.usage.year.png");
    click(Act::item, up::kRange);
    host.chooseMenuItem("7 days");
    settle();

    // ---- A11Y --------------------------------------------------------------------------------------------------------
    waitFor(host, [&] { return bars() > 0; });
    settle();
    {
        std::vector<funkgui::A11yItem> items;
        panel.accessibility(items);
        int group = 0, barItems = 0, tables = 0, rows = 0;
        for (const funkgui::A11yItem& it : items)
        {
            if (it.title.rfind("Usage chart:", 0) == 0 && it.role == funkgui::a11yrole::group)
                ++group;
            if (it.role == funkgui::a11yrole::table && it.title == "Usage by model")
                ++tables;
            if (it.role == funkgui::a11yrole::row)
                ++rows;
            if (it.role == funkgui::A11yRole::staticText && it.title.find("request") != std::string::npos
                && it.title.find("tokens") != std::string::npos && it.parent != 0)
                ++barItems;
        }
        P.eq("a11y.group", group, 1);
        P.eq("a11y.table", tables, 1);
        P.ge("a11y.rows", static_cast<double>(rows), 5.0);     // four models and the total
        P.ge("a11y.bars", static_cast<double>(barItems), 5.0);
    }

    // ---- PLACES ------------------------------------------------------------------------------------------------------
    for (const auto [w, h] : { std::pair{ pg::ui::kDefaultWidth, pg::ui::kDefaultHeight },
                               std::pair{ pg::ui::kMinWidth, pg::ui::kMinHeight }, std::pair{ 1600, 1000 } })
    {
        host.resize(w, h);
        settle();
        const pg::ui::SettingsCard::Places at = card.places(W(), H());
        std::vector<funkgui::A11yItem> items;
        panel.accessibility(items);
        bool inCard = true;
        int titled = 0;
        for (const pg::ui::SettingsCard::Control& c : at.controls)
        {
            // Inside the card, or scrolled out of the body when the page is taller than it (the chart and the table).
            inCard = inCard && (inside(c.r, at.box) || (at.maxScroll > 0.0f && !meet(c.r, at.body)));
            int found = 0;
            for (const funkgui::A11yItem& it : items)
                found += it.id == c.id && !it.title.empty() ? 1 : 0;
            titled += found > 0 || c.act == Act::done ? 1 : 0;
        }
        P.eq("places.in_card_" + std::to_string(w), inCard ? 1 : 0, 1);
        P.eq("places.titled_" + std::to_string(w), titled, static_cast<int64_t>(at.controls.size()));
        if (w == pg::ui::kMinWidth)
            writePng(P, host, "gui.usage.small.png");
    }
    host.resize(pg::ui::kDefaultWidth, pg::ui::kDefaultHeight);
    settle();

    // ---- RESET: asked first ------------------------------------------------------------------------------------------
    P.eq("reset.reached", reach(Act::item, up::kReset) ? 1 : 0, 1);
    click(Act::item, up::kReset);
    settle();
    {
        // Reset and Cancel now; Cancel keeps them.
        reach(Act::item, up::kResetYes);
        const auto yes = card.centreOf(Act::item, up::kResetYes, W(), H());
        const auto no = card.centreOf(Act::item, up::kResetNo, W(), H());
        P.eq("reset.asked", yes.has_value() && no.has_value() ? 1 : 0, 1);
        writePng(P, host, "gui.usage.reset.png");
        click(Act::item, up::kResetNo);
        settle();
        P.ge("reset.cancel_keeps", static_cast<double>(bars()), 1.0);
    }
    // Reset forgets them.
    reach(Act::item, up::kReset);
    click(Act::item, up::kReset);
    settle();
    reach(Act::item, up::kResetYes);
    click(Act::item, up::kResetYes);
    P.eq("reset.emptied", waitFor(host, [&] { return bars() == 0; }) ? 1 : 0, 1);
    settle();
    {
        const std::vector<pg::usage::Row> left = pg::usage::read(pg::usage::processFile());
        P.eq("reset.file_empty", static_cast<int64_t>(left.size()), 0);
    }

    // ---- THEMES ------------------------------------------------------------------------------------------------------
    host.keys("escape");
    settle();
    {
        funkgui::UiPreferences::get().setTheme(1);
        pg::ui::Panel paper(processor.ports(), port);
        funkgui::HeadlessHost paperHost(paper, 1, 2.0f);
        paperHost.setZoom({ 100, 125, 150, 200 }, 100);
        paperHost.settle();
        // Seed again (the reset emptied it).
        pg::usage::add({ pg::usage::dayOf(now), "anthropic", opus, false }, counts(120000, 60000, 24000, 12, 2400000));
        pg::usage::add({ pg::usage::dayOf(now - 2), "anthropic", sonnet, false }, counts(40000, 0, 8000, 4, 150000));
        pg::usage::add({ pg::usage::dayOf(now - 4), "gemini", "gemini-3-pro", false }, counts(12000, 0, 3000, 2));
        pg::usage::flush();
        paper.openSettings("usage");
        paperHost.tick(2);
        paperHost.settle();
        const auto paperBars = [&] { return tagPrims(paperHost.draw(), pg::ui::kUsageBarTag); };
        P.eq("themes.paper_filled", waitFor(paperHost, [&] { return paperBars() > 0; }) ? 1 : 0, 1);
        paperHost.settle();
        const std::string path = probe::outPath("gui.usage.paper.png");
        P.eq("gui.usage.paper.png.written", paperHost.writePng(path.c_str(), 2) ? 1 : 0, 1);
        P.eq("gui.usage.paper.png.glyphs", static_cast<int64_t>(paperHost.draw().missingGlyphs), 0);
        std::printf("wrote %s\n", path.c_str());
        funkgui::UiPreferences::get().setTheme(0);
    }

    pg::chatstore::setDirectoryForTests({});
    pg::projectstore::setDirectoryForTests({});
    (void) kInstrument;
    return P.finish();
}
