// PROBE name=gui.usage timeout=180
//
// gui.usage: the settings card's Usage page (editor/UsagePage.h, SettingsPages.h): what the chat's requests used and
// cost, drawn from a usage ledger (plugin/Usage.h) seeded into the probe's own folder. Headless over a real Processor,
// whose chat reads the ledger on its job thread (ChatPort::usage), to <build>/probe-out/:
//   gui.usage.empty.png       the page with no requests (the chart's frame, "No requests yet.")
//   gui.usage.week.png        7 days, Tokens, a bar a day stacked by model, the legend
//   gui.usage.week.cost.png   7 days, Cost
//   gui.usage.year.png        12 months, a bar a month, the bar the keys picked read out
//   gui.usage.hover.png       the bar under the pointer read out
//   gui.usage.small.png       the least window
//   gui.usage.reset.png       Reset asked (Reset and Cancel)
//   gui.usage.paper.png, gui.usage.paper.small.png   a week in PAPER, and at the least window
// and checks:
//   MODEL    buildModel (pure): the series ranked by tokens, the buckets by day (week) and by month (year), the total;
//            a round axis; an estimate marked ~, none said "no price known"
//   EMPTY    the ledger read (not merely the card open): "No requests yet.", no bar, the footnote's date, Reset disabled
//   RANGES   7 days 7 bars, 30 days 30, 12 months 12 (the newest this month), All back to the oldest month kept
//   STACKS   in the frame, each bar's segments bottom up in the chart's order: the model with the most tokens at the foot
//   SHOW     Tokens and Cost: the chart's name and the bars' readouts in tokens, then in dollars; the table's costs: an
//            estimate with ~, the provider's own figure without, "no price known", the total ~ and + when partly priced
//   READOUT  the bar under the pointer; a click picks a bar, kept once the pointer leaves the chart; End, Home and Right
//            pick by the keys, and a key's pick holds over the pointer's bar until the pointer moves
//   A11Y     the chart a group named and valued by its readout, a bar an item each; the table a table, a row a model and
//            the total
//   PLACES   the default, the least and a large window: the rail and Done inside the card; the body inside the card (its
//            clip never crosses the card's edge); every page control that shows within the body's width; every one
//            reachable by scrolling (inside the content, no taller than the body) and by Tab (the body scrolls to it);
//            every control with an accessible title
//   RESET    asked first: the file untouched while asked and after Cancel; reached by the wheel and by Tab alone, Return
//            asks and Return on Reset empties the file, on the chat's job thread (the message thread waits on nothing)
//   THEMES   a week in both themes and the least window in PAPER; no missing glyph
#include "Probe.h"

#include "editor/Panel.h"
#include "editor/SettingsCard.h"
#include "editor/SettingsPages.h"
#include "editor/Theme.h"
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

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
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

    // What a screen reader is given: the chart's group, its bars in order, the table's rows.
    struct Seen
    {
        funkgui::A11yItem chart;
        std::vector<funkgui::A11yItem> bars, rows;
        bool table = false;
    };
    const auto seen = [&] {
        Seen out;
        std::vector<funkgui::A11yItem> items;
        panel.accessibility(items);
        for (const funkgui::A11yItem& it : items)
            if (it.role == funkgui::a11yrole::group && it.title.rfind("Usage chart:", 0) == 0)
                out.chart = it;
        for (const funkgui::A11yItem& it : items)
        {
            if (out.chart.id != 0 && it.parent == out.chart.id)
                out.bars.push_back(it);
            if (it.role == funkgui::a11yrole::table && it.title == "Usage by model")
                out.table = true;
            if (it.role == funkgui::a11yrole::row)
                out.rows.push_back(it);
        }
        return out;
    };
    const auto fileRows = [&] { return static_cast<int64_t>(pg::usage::read(pg::usage::processFile()).size()); };
    const auto rowOf = [&](const Seen& w, const std::string& start) {
        for (const funkgui::A11yItem& it : w.rows)
            if (it.title.rfind(start, 0) == 0)
                return it.title;
        return std::string();
    };
    // The range chosen through its menu, and the ledger read for it (its bars named, as many as the range has).
    const auto chooseRange = [&](const char* label, std::size_t least) {
        click(Act::item, up::kRange);
        const bool chosen = host.pendingMenu() != nullptr && host.chooseMenuItem(label);
        settle();
        return chosen && waitFor(host, [&] { return seen().bars.size() >= least; });
    };
    // The bars' segments as drawn: by column (its left x), each column's colours from its foot up.
    const auto stacks = [&] {
        std::map<int, std::vector<std::pair<float, uint32_t>>> cols;
        for (const funkgui::Prim& p : host.draw().prims)
            if (p.tag == pg::ui::kUsageBarTag)
                cols[static_cast<int>(std::lround(p.x0))].push_back({ p.y1, p.c0 });
        std::vector<std::vector<uint32_t>> out;
        for (auto& [x, segs] : cols)
        {
            std::sort(segs.begin(), segs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            std::vector<uint32_t> c;
            for (const auto& sg : segs)
                c.push_back(sg.second);
            out.push_back(std::move(c));
        }
        return out;
    };
    const auto packed = [](funkgui::Col c) {
        return static_cast<uint32_t>(c.r) | static_cast<uint32_t>(c.g) << 8 | static_cast<uint32_t>(c.b) << 16 | static_cast<uint32_t>(c.a) << 24;
    };

    // ---- EMPTY: the ledger read, and nothing in it ----------------------------------------------------------------------
    panel.openSettings("usage");
    settle();
    P.eq("empty.shown", card.pageId() == "usage" ? 1 : 0, 1);
    P.eq("empty.read", waitFor(host, [&] { return seen().chart.description == "No requests yet."; }) ? 1 : 0, 1);
    settle();
    P.eq("empty.no_bars", bars(), 0);
    P.eq("empty.no_bar_items", static_cast<int64_t>(seen().bars.size()), 0);
    {
        std::vector<funkgui::A11yItem> items;
        panel.accessibility(items);
        bool resetOff = false, dated = false;
        for (const funkgui::A11yItem& it : items)
        {
            if (it.title.rfind("Reset:", 0) == 0)
                resetOff = !it.enabled;
            if (it.title.find("Prices as of " + pg::usage::pricesInUse().asOf) != std::string::npos)
                dated = true;
        }
        P.eq("empty.reset_off", resetOff ? 1 : 0, 1);
        P.eq("empty.dated", dated ? 1 : 0, 1);
    }
    writePng(P, host, "gui.usage.empty.png");

    // ---- seed: Anthropic's estimated (no reported figure), OpenRouter's reported, Gemini's and OpenAI's unpriced ------
    pg::usage::add({ pg::usage::dayOf(now), "anthropic", opus, false }, counts(120000, 60000, 24000, 12));
    pg::usage::add({ pg::usage::dayOf(now), "anthropic", opus, true }, counts(10000, 0, 2000, 2));
    pg::usage::add({ pg::usage::dayOf(now), "anthropic", sonnet, false }, counts(20000, 0, 4000, 2));
    pg::usage::add({ pg::usage::dayOf(now - 1), "anthropic", opus, false }, counts(50000, 0, 10000, 5));
    pg::usage::add({ pg::usage::dayOf(now - 2), "anthropic", sonnet, false }, counts(40000, 0, 8000, 4));
    pg::usage::add({ pg::usage::dayOf(now - 3), "openrouter", haiku, false }, counts(8000, 0, 1500, 3, 2000));
    pg::usage::add({ pg::usage::dayOf(now - 4), "gemini", "gemini-3-pro", false }, counts(12000, 0, 3000, 2));
    pg::usage::add({ pg::usage::dayOf(now - 6), "openai", "gpt-5", false }, counts(6000, 0, 1200, 1));
    pg::usage::add({ pg::usage::dayOf(now - 40), "anthropic", opus, false }, counts(30000, 0, 6000, 3));   // the year's
    pg::usage::add({ pg::usage::dayOf(now - 500), "anthropic", sonnet, false }, counts(9000, 0, 900, 1));  // All's (folded)
    pg::usage::flush();
    const auto monthOf = [](const std::string& d) { return std::stoi(d.substr(0, 4)) * 12 + std::stoi(d.substr(5, 2)) - 1; };
    const int64_t allMonths = monthOf(today) - monthOf(pg::usage::dayOf(now - 500)) + 1;
    const int64_t seeded = fileRows();

    // ---- RANGES ----------------------------------------------------------------------------------------------------------
    P.eq("ranges.month_filled", waitFor(host, [&] { return bars() > 0 && seen().bars.size() == 30; }) ? 1 : 0, 1);
    P.eq("ranges.month_bars", static_cast<int64_t>(seen().bars.size()), 30);
    P.eq("ranges.year", chooseRange("12 months", 12) ? 1 : 0, 1);
    {
        const Seen w = seen();
        P.eq("ranges.year_bars", static_cast<int64_t>(w.bars.size()), 12);
        static const char* const kNames[] = { "January", "February", "March", "April", "May", "June",
                                              "July", "August", "September", "October", "November", "December" };
        const std::string month = std::string(kNames[std::stoi(today.substr(5, 2)) - 1]) + " " + today.substr(0, 4);
        P.eq("ranges.year_newest_this_month", !w.bars.empty() && w.bars.back().title.rfind(month, 0) == 0 ? 1 : 0, 1);
    }
    P.eq("ranges.all", chooseRange("All", static_cast<std::size_t>(allMonths)) ? 1 : 0, 1);
    P.eq("ranges.all_bars", static_cast<int64_t>(seen().bars.size()), allMonths);   // back to the oldest month kept
    P.eq("ranges.week", chooseRange("7 days", 7) ? 1 : 0, 1);
    waitFor(host, [&] { return seen().bars.size() == 7; });
    P.eq("ranges.week_bars", static_cast<int64_t>(seen().bars.size()), 7);
    settle();
    writePng(P, host, "gui.usage.week.png");

    // ---- STACKS: the largest model at each bar's foot, as the chart orders them -------------------------------------------
    {
        const up::Model m = up::buildModel(pg::usage::snapshot(usage::week), usage::week);
        const funkgui::Theme th = pg::ui::productTheme(funkgui::Theme::byIndex(0));
        std::vector<uint32_t> order;
        for (const int c : m.chartColours)
            order.push_back(packed(up::colourOf(c, th)));
        P.eq("stacks.opus_first", !m.rows.empty() && m.rows.front().model == opus ? 1 : 0, 1);
        bool inOrder = true;
        std::size_t stacked = 0;
        for (const std::vector<uint32_t>& col : stacks())
        {
            // Each colour found in the chart's order, each one after the one under it.
            std::size_t at = 0;
            for (const uint32_t c : col)
            {
                const auto it = std::find(order.begin() + static_cast<std::ptrdiff_t>(at), order.end(), c);
                inOrder = inOrder && it != order.end();
                if (it != order.end())
                    at = static_cast<std::size_t>(it - order.begin()) + 1;
            }
            stacked += col.size() > 1 ? 1 : 0;
        }
        P.eq("stacks.foot_up_in_order", inOrder ? 1 : 0, 1);
        P.ge("stacks.today_stacked", static_cast<double>(stacked), 1.0);   // today: opus under sonnet
    }

    // ---- SHOW: Tokens, then Cost ------------------------------------------------------------------------------------------
    {
        const Seen w = seen();
        P.eq("show.tokens_named", w.chart.title.find("tokens") != std::string::npos ? 1 : 0, 1);
        P.eq("show.tokens_bar", !w.bars.empty() && w.bars.back().title.find(" tokens") != std::string::npos ? 1 : 0, 1);
        P.eq("show.estimate_marked", rowOf(w, opus + ",").find("~$") != std::string::npos ? 1 : 0, 1);
        const std::string hk = rowOf(w, haiku + ",");
        P.eq("show.reported_plain", hk.find("$") != std::string::npos && hk.find("~") == std::string::npos ? 1 : 0, 1);
        P.eq("show.no_price", rowOf(w, "gemini-3-pro,").find("no price known") != std::string::npos ? 1 : 0, 1);
        const std::string total = rowOf(w, "Total");
        P.eq("show.total_partial", total.find("~$") != std::string::npos && total.find("+") != std::string::npos ? 1 : 0, 1);
    }
    click(Act::item, up::kShow);
    P.eq("show.cost_menu", host.pendingMenu() != nullptr && host.chooseMenuItem("Cost") ? 1 : 0, 1);
    settle();
    {
        const Seen w = seen();
        P.eq("show.cost_named", w.chart.title.find("cost") != std::string::npos ? 1 : 0, 1);
        P.eq("show.cost_bar", !w.bars.empty() && w.bars.back().title.find("$") != std::string::npos ? 1 : 0, 1);
    }
    writePng(P, host, "gui.usage.week.cost.png");
    click(Act::item, up::kShow);
    host.chooseMenuItem("Tokens");
    settle();

    // ---- READOUT: the pointer, a click, the keys ----------------------------------------------------------------------
    {
        const funkgui::Rect rail = card.places(W(), H()).rail;
        const auto valueAfter = [&] {
            host.draw();                                         // what the readout shows is what was drawn
            return seen().chart.value;
        };
        Seen w = seen();
        P.eq("readout.bars", static_cast<int64_t>(w.bars.size()), 7);
        if (w.bars.size() == 7)
        {
            const auto centre = [&](int i) {
                const funkgui::Rect r = seen().bars[static_cast<std::size_t>(i)].bounds;
                return funkgui::Point{ r.centreX(), r.centreY() };
            };
            const auto title = [&](int i) { return seen().bars[static_cast<std::size_t>(i)].title; };
            host.move(centre(2).x, centre(2).y);
            P.eq("readout.pointer", valueAfter() == title(2) ? 1 : 0, 1);
            writePng(P, host, "gui.usage.hover.png");
            host.click(centre(1).x, centre(1).y);                // picks bar 1, and focuses the chart
            settle();
            host.move(rail.centreX(), rail.centreY());
            P.eq("readout.click_picks", valueAfter() == title(1) ? 1 : 0, 1);
            host.keys("end");
            P.eq("readout.key_end", valueAfter() == title(6) ? 1 : 0, 1);
            host.move(centre(3).x, centre(3).y);
            P.eq("readout.pointer_again", valueAfter() == title(3) ? 1 : 0, 1);
            host.keys("home");
            P.eq("readout.key_over_pointer", valueAfter() == title(0) ? 1 : 0, 1);   // the pointer still over bar 3
            host.keys("right");
            P.eq("readout.key_right", valueAfter() == title(1) ? 1 : 0, 1);
            host.move(centre(4).x, centre(4).y);
            P.eq("readout.pointer_moves_back", valueAfter() == title(4) ? 1 : 0, 1);
        }
        host.move(rail.centreX(), rail.centreY());
        settle();
    }

    // ---- YEAR's picture: the newest month picked by the keys --------------------------------------------------------------
    chooseRange("12 months", 12);
    {
        const auto chart = card.centreOf(Act::item, up::kChart, W(), H());
        if (chart)
            host.click(chart->x, chart->y);
        settle();
        host.keys("end");
        host.move(card.places(W(), H()).rail.centreX(), card.places(W(), H()).rail.centreY());
        settle();
    }
    writePng(P, host, "gui.usage.year.png");
    chooseRange("7 days", 7);
    waitFor(host, [&] { return seen().bars.size() == 7; });
    settle();

    // ---- A11Y ---------------------------------------------------------------------------------------------------------
    {
        const Seen w = seen();
        P.eq("a11y.chart_group", w.chart.id != 0 ? 1 : 0, 1);
        P.eq("a11y.bars", static_cast<int64_t>(w.bars.size()), 7);
        bool barsNamed = true;
        for (const funkgui::A11yItem& b : w.bars)
            barsNamed = barsNamed && b.role == funkgui::A11yRole::staticText && !b.title.empty();
        P.eq("a11y.bars_named", barsNamed ? 1 : 0, 1);
        P.eq("a11y.table", w.table ? 1 : 0, 1);
        P.eq("a11y.rows", static_cast<int64_t>(w.rows.size()), 6);   // five models in the week and the total
    }

    // ---- PLACES -------------------------------------------------------------------------------------------------------
    for (const auto& [w, h] : { std::pair{ pg::ui::kDefaultWidth, pg::ui::kDefaultHeight },
                                std::pair{ pg::ui::kMinWidth, pg::ui::kMinHeight }, std::pair{ 1600, 1000 } })
    {
        host.resize(w, h);
        settle();
        host.wheel(W() * 0.5f, H() * 0.5f, 1000.0f);            // the body at its top
        settle();
        const std::string k = std::to_string(w);
        const pg::ui::SettingsCard::Places at = card.places(W(), H());
        std::vector<funkgui::A11yItem> items;
        panel.accessibility(items);
        bool fixedIn = true, shownIn = true, reachable = true;
        int titled = 0, pageControls = 0;
        for (const pg::ui::SettingsCard::Control& c : at.controls)
        {
            int named = 0;
            for (const funkgui::A11yItem& it : items)
                named += it.id == c.id && !it.title.empty() ? 1 : 0;
            titled += named > 0 ? 1 : 0;
            if (c.act == Act::done || c.act == Act::page)
            {
                fixedIn = fixedIn && inside(c.r, at.box);
                continue;
            }
            ++pageControls;
            if (meet(c.r, at.body))
                shownIn = shownIn && c.r.x >= at.body.x - 0.5f && c.r.right() <= at.body.right() + 0.5f;
            const float top = c.r.y - at.body.y + at.scroll;   // its place in the content, unscrolled
            reachable = reachable && top >= -0.5f && top + c.r.h <= at.contentH + 0.5f && c.r.h <= at.body.h;
        }
        P.eq("places.fixed_in_card_" + k, fixedIn ? 1 : 0, 1);
        P.eq("places.body_in_card_" + k, inside(at.body, at.box) ? 1 : 0, 1);
        P.eq("places.shown_in_body_" + k, shownIn ? 1 : 0, 1);
        P.eq("places.scroll_reaches_" + k, reachable ? 1 : 0, 1);
        P.eq("places.titled_" + k, titled, static_cast<int64_t>(at.controls.size()));
        // Tab alone: each page control focused in turn lies wholly in the body (the body scrolled to it).
        std::set<std::pair<int, int>> reached;
        for (std::size_t i = 0; i < at.controls.size() + 1; ++i)
        {
            host.keys("tab");
            settle();
            const auto f = card.focused(W(), H());
            const auto placed = card.places(W(), H());
            if (f && f->act != Act::done && f->act != Act::page && inside(f->r, placed.body))
                reached.insert({ static_cast<int>(f->act), f->index });
        }
        P.eq("places.tab_reaches_" + k, static_cast<int64_t>(reached.size()), pageControls);
        if (w == pg::ui::kMinWidth)
        {
            host.wheel(W() * 0.5f, H() * 0.5f, 1000.0f);
            settle();
            writePng(P, host, "gui.usage.small.png");
        }
    }
    host.resize(pg::ui::kDefaultWidth, pg::ui::kDefaultHeight);
    settle();

    // ---- RESET: asked first, by the wheel and the pointer, then by the keys alone -----------------------------------------
    P.eq("reset.reached_wheel", reach(Act::item, up::kReset) ? 1 : 0, 1);
    click(Act::item, up::kReset);
    settle();
    reach(Act::item, up::kResetYes);
    P.eq("reset.asked", card.centreOf(Act::item, up::kResetYes, W(), H()).has_value()
                            && card.centreOf(Act::item, up::kResetNo, W(), H()).has_value() ? 1 : 0, 1);
    P.eq("reset.not_while_asked", fileRows(), seeded);
    writePng(P, host, "gui.usage.reset.png");
    click(Act::item, up::kResetNo);
    settle();
    P.eq("reset.cancel_keeps", fileRows(), seeded);
    P.eq("reset.cancel_back", card.centreOf(Act::item, up::kReset, W(), H()).has_value() ? 1 : 0, 1);
    {
        host.wheel(W() * 0.5f, H() * 0.5f, 1000.0f);            // back to the top: the keys alone from here
        settle();
        const auto tabTo = [&](int index) {
            for (std::size_t i = 0; i < card.places(W(), H()).controls.size() + 1; ++i)
            {
                const auto f = card.focused(W(), H());
                if (f && f->act == Act::item && f->index == index)
                    return inside(f->r, card.places(W(), H()).body);
                host.keys("tab");
                settle();
            }
            return false;
        };
        P.eq("reset.tab_reaches", tabTo(up::kReset) ? 1 : 0, 1);
        host.keys("return");
        settle();
        P.eq("reset.return_asks", tabTo(up::kResetYes) ? 1 : 0, 1);
        P.eq("reset.still_kept", fileRows(), seeded);
        host.keys("return");
    }
    P.eq("reset.emptied", waitFor(host, [&] { return seen().chart.description == "No requests yet."; }) ? 1 : 0, 1);
    P.eq("reset.file_empty", fileRows(), 0);
    settle();

    // ---- THEMES ------------------------------------------------------------------------------------------------------
    host.keys("escape");
    settle();
    {
        funkgui::UiPreferences::get().setTheme(1);
        pg::ui::Panel paper(processor.ports(), port);
        funkgui::HeadlessHost paperHost(paper, 1, 2.0f);
        paperHost.setZoom({ 100, 125, 150, 200 }, 100);
        paperHost.settle();
        pg::usage::add({ pg::usage::dayOf(now), "anthropic", opus, false }, counts(120000, 60000, 24000, 12));
        pg::usage::add({ pg::usage::dayOf(now), "anthropic", sonnet, false }, counts(20000, 0, 4000, 2));
        pg::usage::add({ pg::usage::dayOf(now - 2), "anthropic", sonnet, false }, counts(40000, 0, 8000, 4));
        pg::usage::add({ pg::usage::dayOf(now - 3), "openrouter", haiku, false }, counts(8000, 0, 1500, 3, 2000));
        pg::usage::add({ pg::usage::dayOf(now - 4), "gemini", "gemini-3-pro", false }, counts(12000, 0, 3000, 2));
        pg::usage::flush();
        paper.openSettings("usage");
        paperHost.tick(2);
        paperHost.settle();
        const auto paperBars = [&] { return tagPrims(paperHost.draw(), pg::ui::kUsageBarTag); };
        P.eq("themes.paper_filled", waitFor(paperHost, [&] { return paperBars() > 0; }) ? 1 : 0, 1);
        paperHost.settle();
        writePng(P, paperHost, "gui.usage.paper.png");
        paperHost.resize(pg::ui::kMinWidth, pg::ui::kMinHeight);
        paperHost.tick(2);
        paperHost.settle();
        writePng(P, paperHost, "gui.usage.paper.small.png");
        funkgui::UiPreferences::get().setTheme(0);
    }

    pg::chatstore::setDirectoryForTests({});
    pg::projectstore::setDirectoryForTests({});
    (void) kInstrument;
    return P.finish();
}
