// PROBE name=gui.usage timeout=240
//
// gui.usage: the settings card's Usage page (editor/UsagePage.h) over a real Processor and its chat
// (ChatPort::usage, resetUsage), headless, against a ledger the probe seeds in a chat store folder of its own
// (usage.json written as another process would), to <build>/probe-out/:
//   gui.usage.empty.png   no requests yet: the chart's frame and its words
//   gui.usage.week.png    the last 7 days, tokens
//   gui.usage.cost.png    the same in cost
//   gui.usage.hover.png   the pointer over a bar, its rows in the readout
//   gui.usage.year.png    the last 12 months, the chart focused (its ring) and a bar picked by the keys
//   gui.usage.least.png   at the least window (kMinWidth x kMinHeight)
//   gui.usage.reset.png   Reset's question
//   gui.usage.paper.png   the week in PAPER
// and checks:
//   EMPTY    before any request: "No requests yet." in the chart's frame, its words to a screen reader; no table
//   READ     usage() never waits (a read held on the job thread: it returns at once with the view it had); a read
//            that finds what the view holds moves neither its serial nor the card's revision; another process's
//            write shows after the reread period
//   RANGES   7 days, 30 days, 12 months and All against the seeded ledger's known totals (requests and tokens), the
//            bars' count (7, 30, 12, the months since the oldest row)
//   STACK    the frame's own bar primitives: each bar's pieces bottom up in series order (the most tokens first), in
//            the series' inks; a day's stacks as seeded (Other on top)
//   SERIES   the inks: the accent first; the four others neither accent nor the errors' colour (CIE76 30 or more apart
//            from both), each 3:1 or more
//            against the card and the chart's ground in both themes, every pair apart under protanopia,
//            deuteranopia and tritanopia (Machado 2009, CIE76 10 or more)
//   TABLE    a row a model (fast apart), a total row; an estimate marked ~, OpenRouter's reported figure, "no price"
//            for a model with none; the fast row priced at fast mode's prices; the footnote's date the table in use
//   COST     Tokens and Cost: a model with no price has no height in Cost, its readout says "no price known"
//   READOUT  the readout's words against each bar's own: under the pointer, after a click (the bar under the press's
//            own point, not the last drawn pointer's), each key (Left, Right, Home, End), a key over a resting
//            pointer, the pointer moving again; a screen reader's press and Return pick nothing
//   A11Y     each bar an item with its day and total; the readout an item with its words
//   PRICES   a later price table offered (usage::offerPrices): its date and its prices at the next read
//   SCROLL   the page is taller than the card's body: the rail and Done inside the card, the body inside the card,
//            every control within the body at some wheel position and when Tab reaches it
//   RESET    asked first (pointer and keyboard), the file untouched while asked and after Cancel, empty after Reset;
//            with a read in flight (a flush meanwhile waits for it, a count after the confirmation kept); with the card
//            closed at once; queued when the agent goes (its destructor runs it, then its flush: a count after the
//            confirmation kept); with the folder's lock held from outside, and with the write refused: the file and
//            the counts not written yet kept, said until the next Reset; one confirmed in one instance and run by
//            another's thread failing: both pages say so, and the next Reset clears both
#include "Probe.h"

#include "editor/ChatStyle.h"
#include "editor/Panel.h"
#include "editor/SettingsCard.h"
#include "editor/SettingsPages.h"
#include "editor/Theme.h"
#include "editor/UsagePage.h"
#include "plugin/ChatAgent.h"
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
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace pg = playground;
namespace usage = playground::usage;
namespace up = playground::ui::usagepage;
namespace fs = std::filesystem;
using Act = pg::ui::SettingsCard::Act;

namespace
{
    std::string fileText(const fs::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        std::ostringstream s;
        s << in.rdbuf();
        return s.str();
    }

    void writeText(const fs::path& p, const std::string& s)
    {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << s;
    }

    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    bool inside(const funkgui::Rect& r, const funkgui::Rect& in)
    {
        return r.x >= in.x - 0.01f && r.y >= in.y - 0.01f && r.right() <= in.right() + 0.01f && r.bottom() <= in.bottom() + 0.01f;
    }

    uint32_t packed(funkgui::Col c)
    {
        return static_cast<uint32_t>(c.r) | static_cast<uint32_t>(c.g) << 8 | static_cast<uint32_t>(c.b) << 16 | static_cast<uint32_t>(c.a) << 24;
    }

    double luminance(funkgui::Col c)
    {
        const auto lin = [](uint8_t v) {
            const double s = v / 255.0;
            return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b);
    }

    double contrast(funkgui::Col a, funkgui::Col b)
    {
        const double x = luminance(a), y = luminance(b);
        return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
    }

    // A colour as one with a colour vision deficiency sees it (Machado, Oliveira and Fernandes 2009, severity 1, on
    // linear RGB), in CIELAB (D65), and two colours' distance there (CIE76).
    struct Lab
    {
        double l, a, b;
    };
    Lab seen(funkgui::Col c, int kind)          // 0 none, 1 protanopia, 2 deuteranopia, 3 tritanopia
    {
        static constexpr double m[4][3][3] = {
            { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } },
            { { 0.152286, 1.052583, -0.204868 }, { 0.114503, 0.786281, 0.099216 }, { -0.003882, -0.048116, 1.051998 } },
            { { 0.367322, 0.860646, -0.227968 }, { 0.280085, 0.672501, 0.047413 }, { -0.011820, 0.042940, 0.968881 } },
            { { 1.255528, -0.076749, -0.178779 }, { -0.078411, 0.930809, 0.147602 }, { 0.004733, 0.691367, 0.303900 } },
        };
        const auto lin = [](uint8_t v) {
            const double s = v / 255.0;
            return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
        };
        const double in[3] = { lin(c.r), lin(c.g), lin(c.b) };
        double rgb[3];
        for (int i = 0; i < 3; ++i)
            rgb[i] = std::clamp(m[kind][i][0] * in[0] + m[kind][i][1] * in[1] + m[kind][i][2] * in[2], 0.0, 1.0);
        const double x = (0.4124 * rgb[0] + 0.3576 * rgb[1] + 0.1805 * rgb[2]) / 0.95047;
        const double y = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
        const double z = (0.0193 * rgb[0] + 0.1192 * rgb[1] + 0.9505 * rgb[2]) / 1.08883;
        const auto f = [](double t) { return t > 0.008856 ? std::cbrt(t) : 7.787 * t + 16.0 / 116.0; };
        return { 116.0 * f(y) - 16.0, 500.0 * (f(x) - f(y)), 200.0 * (f(y) - f(z)) };
    }
    double deltaE(const Lab& p, const Lab& q)
    {
        return std::sqrt((p.l - q.l) * (p.l - q.l) + (p.a - q.a) * (p.a - q.a) + (p.b - q.b) * (p.b - q.b));
    }

    // The seeded ledger: rows on days counted back from today, and what each range must total.
    struct Seed
    {
        int back;                                // days before today (-1: a folded month, 500 days back)
        usage::Key key;
        usage::Counts counts;
    };

    usage::Counts counts(int64_t requests, int64_t input, int64_t output, int64_t cacheRead = 0, int64_t w5 = 0, int64_t w1 = 0)
    {
        usage::Counts c;
        c.requests = requests;
        c.input = input;
        c.output = output;
        c.cacheRead = cacheRead;
        c.cacheWrite5m = w5;
        c.cacheWrite1h = w1;
        return c;
    }

    std::vector<Seed> seeds()
    {
        std::vector<Seed> s;
        const auto key = [](const char* provider, const char* model, bool fast = false) { return usage::Key{ "", provider, model, fast }; };
        s.push_back({ 0, key("anthropic", "claude-opus-5-5"), counts(3, 20000, 5000, 100000, 4000, 2000) });
        usage::Counts reported = counts(2, 30000, 3000);
        reported.reportedMicroUsd = 45000;       // OpenRouter reports every request's cost
        reported.reportedRequests = 2;
        s.push_back({ 1, key("openrouter", "anthropic/claude-sonnet-4.6"), reported });
        s.push_back({ 2, key("local", "llama3.1"), counts(4, 6000, 2000) });
        s.push_back({ 0, key("anthropic", "claude-opus-5-5", true), counts(1, 10000, 1000) });
        s.push_back({ 3, key("gemini", "gemini-2.5-flash"), counts(1, 2000, 500) });
        s.push_back({ 1, key("anthropic", "claude-haiku-4-5"), counts(1, 5000, 100) });
        s.push_back({ 1, key("openai", "gpt-5"), counts(1, 900, 100) });
        s.push_back({ 10, key("anthropic", "claude-opus-5-5"), counts(1, 5000, 0) });
        s.push_back({ 100, key("anthropic", "claude-opus-5-5"), counts(1, 7000, 0) });
        s.push_back({ 200, key("anthropic", "claude-sonnet-4-6"), counts(1, 9000, 0) });
        s.push_back({ -1, key("anthropic", "claude-opus-5-5"), counts(1, 11000, 0) });
        return s;
    }

    std::string dayBack(int back)
    {
        const int64_t now = usage::dayNumber(usage::today());
        if (back >= 0)
            return usage::dayOf(now - back);
        return usage::dayOf(now - 500).substr(0, 7);   // a month folded long ago
    }

    std::string seedText()
    {
        std::vector<usage::Row> rows;
        for (const Seed& s : seeds())
        {
            usage::Row r{ s.key, s.counts };
            r.key.day = dayBack(s.back);
            rows.push_back(r);
        }
        return usage::write(rows);
    }

    // What a range must total: requests and tokens.
    std::pair<int64_t, int64_t> expected(usage::Range range)
    {
        const std::string today = usage::today();
        int64_t requests = 0, tokens = 0;
        int y = std::stoi(today.substr(0, 4)), m = std::stoi(today.substr(5, 2)) - 11;
        if (m < 1)
        {
            m += 12;
            --y;
        }
        char first[16];
        std::snprintf(first, sizeof first, "%04d-%02d-01", y, m);
        for (const Seed& s : seeds())
        {
            const std::string day = dayBack(s.back);
            bool in = true;
            if (range == usage::Range::week)
                in = s.back >= 0 && s.back <= 6;
            else if (range == usage::Range::month)
                in = s.back >= 0 && s.back <= 29;
            else if (range == usage::Range::year)
                in = s.back >= 0 && day >= first;
            if (in)
            {
                requests += s.counts.requests;
                tokens += s.counts.tokensIn() + s.counts.output;
            }
        }
        return { requests, tokens };
    }
}

PROBE(gui, usage)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);
    const juce::String prefix(pg::product::kEnvPrefix);
    const juce::File projects = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                    .getChildFile(prefix + "gui-usage-" + juce::String(juce::Time::currentTimeMillis()));
    projects.deleteRecursively();
    pg::projectstore::setDirectoryForTests(projects);
    const juce::File store = juce::File(juce::String(probe::outPath(prefix.toStdString() + "usage-store")));
    store.deleteRecursively();
    store.createDirectory();
    pg::chatstore::setDirectoryForTests(store);
    usage::resetPricesForTests();
    const fs::path file = usage::processFile();
    std::printf("ledger: %s\n", file.string().c_str());

    pg::Processor processor;
    pg::EditorPort& port = processor.editorPort();
    processor.settle();
    pg::ChatAgent& agent = processor.chatAgent();
    agent.setUsageRereadSecondsForTests(0.25);
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
    // Waits (generously, returning as soon as it holds) for `done`, ticking the Panel: the card's sync asks for reads.
    const auto waitFor = [&](auto done) {
        const auto until = std::chrono::steady_clock::now() + probe::kWorkWait;
        while (std::chrono::steady_clock::now() < until)
        {
            host.tick(1);
            if (done())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return done();
    };
    const auto state = [&]() -> up::State& { return up::stateOf(card); };
    const auto view = [&] { return port.chat().usage(state().range); };
    // A view of the range shown, read after `after` (a serial), holding `rows` rows (-1: any).
    const auto waitView = [&](uint64_t after, int rows = -1) {
        return waitFor([&] {
            const auto v = view();
            return v->read && v->range == state().range && v->serial > after && !v->resetting
                && (rows < 0 || static_cast<int>(v->rows.size()) == rows);
        });
    };
    const auto chartControl = [&]() -> std::optional<pg::ui::SettingsCard::Control> {
        for (const pg::ui::SettingsCard::Control& c : card.places(W(), H()).controls)
            if (c.custom)
                return c;
        return std::nullopt;
    };
    const auto chart = [&] { return up::chartOf(*view()); };
    const auto geometry = [&] {
        const auto c = chartControl();
        return up::geometryOf(c ? c->r : funkgui::Rect{}, chart(), state().measure);
    };
    // The chart's items to a screen reader: its bars, then its readout (last).
    const auto chartItems = [&] {
        std::vector<funkgui::A11yItem> out;
        const auto c = chartControl();
        if (!c)
            return out;
        for (const funkgui::A11yItem& it : host.accessibility())
            if (it.parent == c->id)
                out.push_back(it);
        return out;
    };
    const auto readoutText = [&] {
        host.draw();                             // the card notes the pointer it drew with
        const auto items = chartItems();
        return items.empty() ? std::string() : items.back().title;
    };
    const auto shows = [&](int bar) { return has(readoutText(), up::barWords(chart().bars[static_cast<std::size_t>(bar)])); };
    const auto lines = [&] {
        std::vector<std::string> out;
        for (const pg::ui::SettingsCard::Line& l : card.places(W(), H()).lines)
            out.push_back(l.text);
        return out;
    };
    // The page's texts in one (a note's words wrap across its lines).
    const auto lineWith = [&](const std::string& part) {
        std::string all;
        for (const std::string& l : lines())
            all += l + " ";
        return has(all, part);
    };
    const auto choose = [&](int code, const char* label) {
        const auto at = card.centreOf(Act::item, code, W(), H());
        if (!at)
            return false;
        host.click(at->x, at->y);
        const bool chosen = host.pendingMenu() != nullptr && host.chooseMenuItem(label);
        settle();
        return chosen;
    };
    const auto writePng = [&](funkgui::HeadlessHost& h, const char* name) {
        const std::string path = probe::outPath(name);
        h.draw();
        P.eq(std::string(name) + ".written", h.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
    };
    // Holds `who`'s job thread in a usage read, and returns once one is waiting at the hold (a fact, not a hope): a
    // read that began before the hold runs past it (and ends by running any reset owed), so this asks for reads,
    // alternating the range so that each call asks for one, until one waits there. The job thread is serial: one at most.
    const auto holdARead = [&](pg::ChatAgent& who, usage::Range r) {
        who.holdUsageReadsForTests(true);
        const auto until = std::chrono::steady_clock::now() + probe::kWorkWait;
        bool other = false;
        while (who.usageReadsHeldForTests() == 0 && std::chrono::steady_clock::now() < until)
        {
            (void) who.usage(other ? usage::Range::all : r);
            other = !other;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return who.usageReadsHeldForTests() == 1;
    };
    const auto focusOn = [&](Act act, int code) {
        for (int i = 0; i < 80; ++i)
        {
            const auto f = card.focused(W(), H());
            if (f && f->act == act && f->index == code)
                return true;
            host.keys("tab");
            settle();
        }
        return false;
    };

    // ---- EMPTY ------------------------------------------------------------------------------------------------------------
    P.eq("empty.no_file", fs::exists(file) ? 1 : 0, 0);
    P.eq("empty.opens", panel.openSettings("usage") && card.pageId() == "usage" ? 1 : 0, 1);
    P.eq("empty.read", waitView(0) ? 1 : 0, 1);
    settle();
    {
        const auto c = chartControl();
        P.eq("empty.chart_control", c ? 1 : 0, 1);
        P.eq("empty.words_titled", c && has(c->title, "No requests yet.") ? 1 : 0, 1);
        P.eq("empty.readout_words", readoutText() == "No requests yet." ? 1 : 0, 1);
        P.eq("empty.no_table", lineWith("BY MODEL") ? 0 : 1, 1);
        P.eq("empty.footnote", lineWith("Your provider's invoice is the record.") ? 1 : 0, 1);
        P.eq("empty.missing_glyphs", static_cast<int64_t>(host.draw().missingGlyphs), 0);
    }
    writePng(host, "gui.usage.empty.png");

    // ---- READ: never waits; the same read moves nothing; another process's write shows ------------------------------------
    writeText(file, seedText());                 // as another process would write it
    {
        const uint64_t before = view()->serial;
        P.eq("read.other_process_shows", waitView(before, -1) && !view()->rows.empty() ? 1 : 0, 1);
        settle();
    }
    {
        // A read held on the job thread: usage() answers at once with the view it has.
        agent.setUsageRereadSecondsForTests(0.0);
        P.eq("read.held", holdARead(agent, state().range) ? 1 : 0, 1);
        const auto held = view();                // no read can publish while the one read waits at the hold
        const double t0 = probe::threadMs();
        std::shared_ptr<const pg::UsageView> got;
        for (int i = 0; i < 100; ++i)
            got = port.chat().usage(state().range);
        const double spent = probe::threadMs() - t0;
        std::printf("100 usage() calls with a read held: %.3f ms of this thread\n", spent);
        P.le("read.never_waits_ms", spent, 50.0);
        P.eq("read.held_view_kept", got == held ? 1 : 0, 1);
        agent.holdUsageReadsForTests(false);
        // Reads that find what the view holds: no serial moved, no card revision moved.
        const uint64_t reads = agent.usageReadsForTests();
        const uint64_t serial = view()->serial;
        settle();
        const uint32_t revision = card.revision();
        P.eq("read.again", waitFor([&] { return agent.usageReadsForTests() >= reads + 3; }) ? 1 : 0, 1);
        P.eq("read.same_serial", view()->serial == serial ? 1 : 0, 1);
        P.eq("read.same_revision", card.revision() == revision ? 1 : 0, 1);
        agent.setUsageRereadSecondsForTests(0.25);
    }

    // ---- RANGES -----------------------------------------------------------------------------------------------------------
    {
        const struct
        {
            usage::Range range;
            const char* label;
            const char* key;
        } kRanges[] = { { usage::Range::month, "Last 30 days", "month" },
                        { usage::Range::year, "Last 12 months", "year" },
                        { usage::Range::all, "All time", "all" },
                        { usage::Range::week, "Last 7 days", "week" } };
        for (const auto& r : kRanges)
        {
            const uint64_t before = view()->serial;
            P.eq(std::string("ranges.") + r.key + ".chosen", choose(up::kRange, r.label) && state().range == r.range ? 1 : 0, 1);
            P.eq(std::string("ranges.") + r.key + ".read", waitView(before) ? 1 : 0, 1);
            settle();
            const up::Chart c = chart();
            const auto want = expected(r.range);
            std::printf("%s: %lld requests, %lld tokens (want %lld, %lld), %zu bars\n", r.key, static_cast<long long>(c.total.counts.requests),
                        static_cast<long long>(c.total.tokens()), static_cast<long long>(want.first), static_cast<long long>(want.second),
                        c.bars.size());
            P.eq(std::string("ranges.") + r.key + ".requests", c.total.counts.requests, want.first);
            P.eq(std::string("ranges.") + r.key + ".tokens", c.total.tokens(), want.second);
            int64_t bars = r.range == usage::Range::week ? 7 : r.range == usage::Range::month ? 30 : 12;
            if (r.range == usage::Range::all)
            {
                const std::string today = usage::today(), old = dayBack(-1);
                const auto months = [](const std::string& d) { return std::stoi(d.substr(0, 4)) * 12 + std::stoi(d.substr(5, 2)); };
                bars = months(today) - months(old) + 1;
            }
            P.eq(std::string("ranges.") + r.key + ".bars", static_cast<int64_t>(c.bars.size()), bars);
            int64_t barSum = 0;
            for (const up::Bar& b : c.bars)
                barSum += b.total.tokens();
            P.eq(std::string("ranges.") + r.key + ".bars_add_up", barSum, want.second);
            const auto ctl = chartControl();
            P.eq(std::string("ranges.") + r.key + ".titled_total", ctl && has(ctl->title, up::tokensText(want.second) + " tokens") ? 1 : 0, 1);
        }
    }
    settle();
    writePng(host, "gui.usage.week.png");

    // ---- STACK: the bars' pieces from the frame's own primitives -------------------------------------------------------------
    const funkgui::Theme graphite = pg::ui::productTheme(funkgui::Theme::byIndex(0));
    {
        const up::Chart c = chart();
        std::printf("series:");
        for (const up::Model& m : c.models)
            std::printf(" %s=%d", m.name.c_str(), m.series);
        std::printf("\n");
        P.eq("stack.models", static_cast<int64_t>(c.models.size()), 7);
        const bool firstMost = c.models.size() >= 2 && c.models[0].name == "claude-opus-5-5"
                            && c.models[1].name == "anthropic/claude-sonnet-4.6";
        P.eq("stack.first_most", firstMost ? 1 : 0, 1);
        P.eq("stack.other_named", !c.seriesNames.empty() && c.seriesNames.back() == "Other" ? 1 : 0, 1);
        const up::Geometry g = geometry();
        host.move(5.0f, 5.0f);                   // the pointer off the chart
        const funkgui::PrimList& frame = host.draw();
        std::map<uint32_t, int> inkOf;
        for (int k = 0; k <= up::kNamedSeries; ++k)
            inkOf[packed(pg::ui::chat::seriesInk(graphite, k))] = k;
        std::vector<std::vector<std::pair<float, int>>> pieces(g.bars.size());   // per bar: (top y, series)
        int unknownInk = 0, outside = 0;
        for (const funkgui::Prim& p : frame.prims)
        {
            if (p.tag != up::kBarTag)
                continue;
            const float cx = 0.5f * (p.x0 + p.x1);
            const int bar = up::barAt(g, { cx, g.plot.centreY() });
            const auto ink = inkOf.find(p.c0);
            if (bar < 0)
                ++outside;
            else if (ink == inkOf.end())
                ++unknownInk;
            else
                pieces[static_cast<std::size_t>(bar)].push_back({ p.y0, ink->second });
        }
        P.eq("stack.pieces_in_bars", outside, 0);
        P.eq("stack.pieces_in_series_inks", unknownInk, 0);
        bool ordered = true;
        std::vector<std::vector<int>> orders(g.bars.size());
        for (std::size_t i = 0; i < pieces.size(); ++i)
        {
            auto& v = pieces[i];
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });   // bottom first
            for (const auto& piece : v)
                orders[i].push_back(piece.second);
            for (std::size_t k = 1; k < orders[i].size(); ++k)
                ordered = ordered && orders[i][k] > orders[i][k - 1];
        }
        const auto text = [](const std::vector<int>& v) {
            std::string s;
            for (const int k : v)
                s += std::to_string(k) + " ";
            return s;
        };
        std::printf("today's stack: %s; yesterday's: %s\n", text(orders.back()).c_str(), text(orders[orders.size() - 2]).c_str());
        P.eq("stack.bottom_up_in_series_order", ordered ? 1 : 0, 1);
        // Today: opus (0) under opus fast (2); yesterday: sonnet (1), haiku (4), Other (5: gpt-5).
        P.eq("stack.today", text(orders.back()) == "0 2 " ? 1 : 0, 1);
        P.eq("stack.yesterday", text(orders[orders.size() - 2]) == "1 4 5 " ? 1 : 0, 1);
        P.eq("stack.empty_days_none", orders[0].empty() && orders[1].empty() ? 1 : 0, 1);
    }

    // ---- SERIES: the inks against both grounds ------------------------------------------------------------------------------
    for (const int themeIdx : { 0, 1 })
    {
        const funkgui::Theme th = pg::ui::productTheme(funkgui::Theme::byIndex(themeIdx));
        const pg::ui::chat::Palette p = pg::ui::chat::palette(th);
        const std::string t = themeIdx == 0 ? "graphite" : "paper";
        P.eq("series." + t + ".accent_first", packed(pg::ui::chat::seriesInk(th, 0)) == packed(th.accent) ? 1 : 0, 1);
        double least = 100.0, nearest = 1000.0;
        for (int k = 0; k < pg::ui::chat::kSeriesInks; ++k)
        {
            const funkgui::Col ink = pg::ui::chat::seriesInk(th, k);
            least = std::min({ least, contrast(ink, p.card), contrast(ink, p.raised) });
            if (k > 0)                          // as seen with normal colour vision, CIE76
                nearest = std::min({ nearest, deltaE(seen(ink, 0), seen(th.accent, 0)), deltaE(seen(ink, 0), seen(th.signal, 0)) });
        }
        std::printf("%s: least contrast %.2f, nearest to the accent or the errors' colour %.1f (CIE76)\n", t.c_str(), least, nearest);
        P.ge("series." + t + ".contrast", least, 3.0);
        P.ge("series." + t + ".apart_from_accent_and_signal", nearest, 30.0);
        // Every pair of the five (this product's accent and the four) apart for every kind of colour vision.
        double leastSeen = 1000.0;
        for (int kind = 0; kind < 4; ++kind)
            for (int i = 0; i < pg::ui::chat::kSeriesInks; ++i)
                for (int j = i + 1; j < pg::ui::chat::kSeriesInks; ++j)
                {
                    const Lab a = seen(pg::ui::chat::seriesInk(th, i), kind), b = seen(pg::ui::chat::seriesInk(th, j), kind);
                    leastSeen = std::min(leastSeen, deltaE(a, b));
                }
        std::printf("%s: least distance between two series under any of the four visions: %.1f (CIE76)\n", t.c_str(), leastSeen);
        P.ge("series." + t + ".apart_for_every_vision", leastSeen, 10.0);
    }

    // ---- TABLE ------------------------------------------------------------------------------------------------------------
    {
        const auto rowOf = [&](const std::string& name) {   // the texts of a table row, from its model's cell on
            std::vector<std::string> out;
            const auto at = card.places(W(), H()).lines;
            for (std::size_t i = 0; i < at.size(); ++i)
                if (at[i].text == name)
                {
                    for (std::size_t j = i; j < at.size() && at[j].r.y == at[i].r.y; ++j)
                        out.push_back(at[j].text);
                    break;
                }
            return out;
        };
        const auto opus = rowOf("claude-opus-5-5");
        const auto fast = rowOf("claude-opus-5-5, fast");
        const auto router = rowOf("anthropic/claude-sonnet-4.6");
        const auto local = rowOf("llama3.1");
        const auto total = rowOf("Total");
        P.eq("table.heading", lineWith("BY MODEL") ? 1 : 0, 1);
        P.eq("table.rows", !opus.empty() && !fast.empty() && !router.empty() && !local.empty() && !total.empty() ? 1 : 0, 1);
        if (opus.size() >= 7 && fast.size() >= 7 && router.size() >= 7 && local.size() >= 7)
        {
            const usage::Counts oc = seeds()[0].counts;
            const usage::Cost want = usage::costOf({ usage::today(), "anthropic", "claude-opus-5-5", false }, oc);
            const usage::Cost fastWant = usage::costOf({ usage::today(), "anthropic", "claude-opus-5-5", true }, seeds()[3].counts);
            std::printf("opus: %s; fast: %s; router: %s; local: %s\n", opus[6].c_str(), fast[6].c_str(), router[6].c_str(), local[6].c_str());
            P.eq("table.estimate_marked", opus[6] == usage::costText(want) && has(opus[6], "~") ? 1 : 0, 1);
            P.eq("table.fast_row_fast_prices", fastWant.kind == usage::Cost::Kind::estimate && fast[6] == usage::costText(fastWant) ? 1 : 0, 1);
            usage::Cost reported;
            reported.kind = usage::Cost::Kind::reported;
            reported.microUsd = 45000;
            P.eq("table.reported_as_is", router[6] == usage::costText(reported) && !has(router[6], "~") ? 1 : 0, 1);
            P.eq("table.no_price", local[6] == "no price" ? 1 : 0, 1);
            P.eq("table.requests", opus[2] == "3" && router[2] == "2" ? 1 : 0, 1);
            const bool cells = opus[3] == up::tokensText(26000) && opus[4] == up::tokensText(100000)
                            && opus[5] == up::tokensText(5000);
            P.eq("table.in_cached_out", cells ? 1 : 0, 1);
            P.eq("table.provider", router[1] == "OpenRouter" && local[1] == "Local" ? 1 : 0, 1);
        }
        P.eq("table.unpriced_note", lineWith("no price known shows its tokens alone") ? 1 : 0, 1);
        P.eq("table.footnote_date", lineWith("as of " + usage::builtInPrices().asOf) ? 1 : 0, 1);
        P.eq("table.footnote_reported", lineWith("OpenRouter's are as reported.") ? 1 : 0, 1);
        P.eq("table.footnote_invoice", lineWith("Your provider's invoice is the record.") ? 1 : 0, 1);
    }

    // ---- READOUT ----------------------------------------------------------------------------------------------------------
    {
        const up::Geometry g = geometry();
        const int n = static_cast<int>(g.slots.size());
        const auto at = [&](int bar, float dx = 0.0f) {
            const funkgui::Rect& s = g.slots[static_cast<std::size_t>(bar)];
            return funkgui::Point{ s.centreX() + dx, s.centreY() };
        };
        host.move(5.0f, 5.0f);
        P.eq("readout.none_is_range", has(readoutText(), "Last 7 days: ") ? 1 : 0, 1);
        P.eq("readout.partly_priced_total", has(readoutText(), " + no price for some, ") ? 1 : 0, 1);
        bool pointer = true;
        for (int i = 0; i < n; ++i)
        {
            host.move(at(i).x, at(i).y);
            pointer = pointer && shows(i);
        }
        P.eq("readout.pointer_each_bar", pointer ? 1 : 0, 1);
        host.move(at(5).x, at(5).y);
        writePng(host, "gui.usage.hover.png");
        // A click: the bar under the press's own point, though the frame last drawn had the pointer over another.
        host.move(at(1).x, at(1).y);
        host.draw();
        host.click(at(4).x, at(4).y);            // no frame between the move and the press
        P.eq("readout.click_own_point", state().picked == 4 ? 1 : 0, 1);
        host.move(5.0f, 5.0f);                   // off the chart: the bar clicked stays
        P.eq("readout.click_stays", shows(4) ? 1 : 0, 1);
        // The keys, the chart focused.
        P.eq("readout.focus_by_tab", focusOn(Act::item, up::kChart) ? 1 : 0, 1);
        host.keys("home");
        settle();
        P.eq("readout.home", shows(0) && state().picked == 0 ? 1 : 0, 1);
        host.keys("right");
        settle();
        P.eq("readout.right", shows(1) ? 1 : 0, 1);
        host.keys("end");
        settle();
        P.eq("readout.end", shows(n - 1) ? 1 : 0, 1);
        host.keys("left");
        settle();
        P.eq("readout.left", shows(n - 2) ? 1 : 0, 1);
        P.eq("readout.focus_kept", card.focused(W(), H()) && card.focused(W(), H())->custom ? 1 : 0, 1);
        // A key over a resting pointer: the key's bar shows until the pointer moves.
        host.move(at(2).x, at(2).y);
        P.eq("readout.pointer_before_key", shows(2) ? 1 : 0, 1);
        host.keys("left");
        settle();
        P.eq("readout.key_over_pointer", shows(1) ? 1 : 0, 1);
        host.tick(5);
        P.eq("readout.key_stays_at_rest", shows(1) ? 1 : 0, 1);
        host.move(at(2, 2.0f).x, at(2).y);       // the pointer moves again, over the bar it rested on
        P.eq("readout.pointer_moves_back", shows(2) ? 1 : 0, 1);
        // Return and a screen reader's press pick nothing by position.
        const int before = state().picked;
        host.keys("return");
        settle();
        P.eq("readout.return_picks_nothing", state().picked == before ? 1 : 0, 1);
        const auto c = chartControl();
        if (c)
            card.a11yAction(c->id, funkgui::A11yAction::press, W(), H(), &host);
        P.eq("readout.a11y_press_picks_nothing", state().picked == before ? 1 : 0, 1);

        // A11Y: a bar each, its day and its total; the readout's words.
        const auto items = chartItems();
        const up::Chart ch = chart();
        bool titled = static_cast<int>(items.size()) == n + 1;
        for (int i = 0; titled && i < n; ++i)
            titled = items[static_cast<std::size_t>(i)].title == up::barWords(ch.bars[static_cast<std::size_t>(i)])
                  && has(items[static_cast<std::size_t>(i)].title, ch.bars[static_cast<std::size_t>(i)].title);
        P.eq("a11y.bars", titled ? 1 : 0, 1);
        const std::string lastTotal = up::tokensText(ch.bars.back().total.tokens()) + " tokens";
        P.eq("a11y.bar_total", n > 0 && has(items[static_cast<std::size_t>(n - 1)].title, lastTotal) ? 1 : 0, 1);
        P.eq("a11y.readout_words", !items.empty() && items.back().title == up::readoutWords(ch, 2) ? 1 : 0, 1);
        P.eq("a11y.chart_group", c && has(c->title, "Usage chart") ? 1 : 0, 1);
    }

    // ---- COST ---------------------------------------------------------------------------------------------------------------
    {
        P.eq("cost.chosen", choose(up::kMeasure, "Cost") && state().measure == up::Measure::cost ? 1 : 0, 1);
        const up::Geometry g = geometry();
        host.move(5.0f, 5.0f);
        const funkgui::PrimList& frame = host.draw();
        int localDay = 0, today = 0;            // the local model's day (2 back) has no price: no height
        for (const funkgui::Prim& p : frame.prims)
            if (p.tag == up::kBarTag)
            {
                const int bar = up::barAt(g, { 0.5f * (p.x0 + p.x1), g.plot.centreY() });
                localDay += bar == 4 ? 1 : 0;
                today += bar == 6 ? 1 : 0;
            }
        P.eq("cost.unpriced_no_height", localDay, 0);
        P.eq("cost.priced_has_height", today >= 1 ? 1 : 0, 1);
        const auto pt = g.slots[4];
        host.move(pt.centreX(), pt.centreY());
        P.eq("cost.readout_no_price", has(readoutText(), "no price known") ? 1 : 0, 1);
        host.move(5.0f, 5.0f);
        host.draw();
        writePng(host, "gui.usage.cost.png");
        choose(up::kMeasure, "Tokens");
    }

    // ---- PRICES: a later table shows at the next read ---------------------------------------------------------------------
    {
        usage::PriceTable later = usage::builtInPrices();
        later.asOf = "2099-01-01";
        for (usage::PriceRow& r : later.rows)
            if (r.model == "claude-opus-5-5")
                r.price.in *= 2.0;
        const uint64_t before = view()->serial;
        P.eq("prices.offered", usage::offerPrices(later) ? 1 : 0, 1);
        P.eq("prices.read", waitView(before) ? 1 : 0, 1);
        settle();
        P.eq("prices.date_shown", lineWith("as of 2099-01-01") ? 1 : 0, 1);
        const usage::Cost want = usage::costOf({ usage::today(), "anthropic", "claude-opus-5-5", false }, seeds()[0].counts);
        bool shown = false;
        for (const std::string& l : lines())
            shown = shown || l == usage::costText(want);
        P.eq("prices.new_price", shown ? 1 : 0, 1);
        usage::resetPricesForTests();
        const uint64_t again = view()->serial;
        waitView(again);
        settle();
    }

    // ---- SCROLL ------------------------------------------------------------------------------------------------------------
    {
        const auto at = card.places(W(), H());
        std::printf("page: %.0f px in a body of %.0f\n", static_cast<double>(at.contentH), static_cast<double>(at.body.h));
        P.ge("scroll.taller_than_body", static_cast<double>(at.maxScroll), 1.0);
        bool fixedIn = true;
        for (const pg::ui::SettingsCard::Control& c : at.controls)
            if (c.act == Act::page || c.act == Act::done)
                fixedIn = fixedIn && inside(c.r, at.box);
        P.eq("scroll.rail_and_done_in_card", fixedIn ? 1 : 0, 1);
        P.eq("scroll.body_in_card", inside(at.body, at.box) ? 1 : 0, 1);
        // By the wheel: each control wholly in the body at some position, from the top to the bottom.
        std::map<std::pair<int, int>, bool> seen;
        for (const pg::ui::SettingsCard::Control& c : at.controls)
            if (c.act == Act::item)
                seen[{ static_cast<int>(c.act), c.index }] = false;
        const funkgui::Point mid{ at.body.centreX(), at.body.centreY() };
        for (int i = 0; i < 40; ++i)
        {
            const auto now = card.places(W(), H());
            for (const pg::ui::SettingsCard::Control& c : now.controls)
                if (c.act == Act::item && inside(c.r, now.body))
                    seen[{ static_cast<int>(c.act), c.index }] = true;
            host.wheel(mid.x, mid.y, -1.0f);
            settle();
        }
        bool wheel = true;
        for (const auto& [k, ok] : seen)
            wheel = wheel && ok;
        P.eq("scroll.wheel_reaches_every_control", wheel ? 1 : 0, 1);
        P.eq("scroll.wheel_reached_bottom", card.places(W(), H()).scroll >= card.places(W(), H()).maxScroll - 0.5f ? 1 : 0, 1);
        for (int i = 0; i < 40; ++i)
            host.wheel(mid.x, mid.y, 1.0f);
        settle();
        // By Tab alone: the body scrolls to the control focused.
        bool tab = true;
        int reached = 0;
        for (int i = 0; i < 30; ++i)
        {
            host.keys("tab");
            settle();
            const auto now = card.places(W(), H());
            const auto f = card.focused(W(), H());
            if (f && f->act == Act::item)
            {
                tab = tab && inside(f->r, now.body);
                ++reached;
            }
        }
        P.eq("scroll.tab_shows_focused", tab && reached > 0 ? 1 : 0, 1);
    }

    // ---- the year, focused; the least window ---------------------------------------------------------------------------
    {
        for (int i = 0; i < 60 && card.places(W(), H()).scroll > 0.0f; ++i)
        {
            host.wheel(10.0f + card.places(W(), H()).body.x, card.places(W(), H()).body.centreY(), 1.0f);
            settle();
        }
        P.eq("year.scrolled_to_top", card.places(W(), H()).scroll <= 0.0f ? 1 : 0, 1);
        const uint64_t before = view()->serial;
        P.eq("year.chosen", choose(up::kRange, "Last 12 months") && state().range == usage::Range::year ? 1 : 0, 1);
        P.eq("year.read", waitView(before) ? 1 : 0, 1);
        settle();
        focusOn(Act::item, up::kChart);
        host.keys("end");
        settle();
        host.move(5.0f, 5.0f);
        P.eq("year.ring", card.focused(W(), H()) && card.focused(W(), H())->custom ? 1 : 0, 1);
        P.eq("year.missing_glyphs", static_cast<int64_t>(host.draw().missingGlyphs), 0);
        writePng(host, "gui.usage.year.png");
        host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight);
        settle();
        const auto at = card.places(W(), H());
        P.eq("least.in_window", at.box.x >= 0.0f && at.box.y >= 0.0f && at.box.right() <= W() && at.box.bottom() <= H() ? 1 : 0, 1);
        const auto c = chartControl();
        P.eq("least.chart_in_body_width", c && c->r.x >= at.body.x && c->r.right() <= at.body.right() + 0.5f ? 1 : 0, 1);
        writePng(host, "gui.usage.least.png");
        host.resize(pg::ui::kDefaultWidth, pg::ui::kDefaultHeight);
        settle();
        const uint64_t again = view()->serial;
        P.eq("least.back_to_week", choose(up::kRange, "Last 7 days") && waitView(again) ? 1 : 0, 1);
        settle();
    }

    // ---- PAPER ------------------------------------------------------------------------------------------------------------
    {
        funkgui::UiPreferences::get().setTheme(1);
        pg::ui::Panel paper(processor.ports(), port);
        funkgui::HeadlessHost paperHost(paper, 1, 2.0f);
        paperHost.setZoom({ 100, 125, 150, 200 }, 100);
        paperHost.settle();
        paper.openSettings("usage");
        P.eq("paper.read", [&] {
            const auto until = std::chrono::steady_clock::now() + probe::kWorkWait;
            while (std::chrono::steady_clock::now() < until)
            {
                paperHost.tick(1);
                const auto v = port.chat().usage(usage::Range::week);
                if (v->read && v->range == usage::Range::week && !v->rows.empty())
                    return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return false;
        }() ? 1 : 0, 1);
        paperHost.tick(2);
        paperHost.settle();
        P.eq("paper.missing_glyphs", static_cast<int64_t>(paperHost.draw().missingGlyphs), 0);
        writePng(paperHost, "gui.usage.paper.png");
        paperHost.keys("escape");
        paperHost.tick(2);
        funkgui::UiPreferences::get().setTheme(0);
    }
    settle();

    // ---- RESET by pointer: asked first, the file untouched while asked and after Cancel --------------------------------------
    const auto scrollTo = [&](int code) {
        for (int i = 0; i < 80 && !card.centreOf(Act::item, code, W(), H()); ++i)
        {
            const auto at = card.places(W(), H());
            host.wheel(at.body.centreX(), at.body.centreY(), i < 40 ? -1.0f : 1.0f);   // down, then back up
            settle();
        }
        return card.centreOf(Act::item, code, W(), H());
    };
    const auto clickCode = [&](int code) {
        const auto at = scrollTo(code);
        if (at)
            host.click(at->x, at->y);
        settle();
        return at.has_value();
    };
    const auto emptied = [&] { return fs::exists(file) && usage::read(file).empty(); };
    {
        const std::string seeded = fileText(file);
        P.eq("reset.ask", clickCode(up::kReset) && state().asking ? 1 : 0, 1);
        P.eq("reset.question", lineWith("Reset the usage record?") ? 1 : 0, 1);
        P.eq("reset.untouched_asked", fileText(file) == seeded ? 1 : 0, 1);
        scrollTo(up::kResetConfirm);
        writePng(host, "gui.usage.reset.png");
        P.eq("reset.cancel", clickCode(up::kReset) && !state().asking ? 1 : 0, 1);
        host.tick(10);
        P.eq("reset.untouched_cancelled", fileText(file) == seeded ? 1 : 0, 1);
        P.eq("reset.ask_again", clickCode(up::kReset) && state().asking ? 1 : 0, 1);
        P.eq("reset.confirm", clickCode(up::kResetConfirm) && !state().asking ? 1 : 0, 1);
        P.eq("reset.emptied", waitFor(emptied) ? 1 : 0, 1);
        P.eq("reset.view_empty", waitFor([&] { return view()->read && view()->rows.empty() && !view()->resetting; }) ? 1 : 0, 1);
        settle();
        P.eq("reset.chart_empty", has(readoutText(), "No requests yet.") ? 1 : 0, 1);
    }

    // ---- RESET by the keyboard ---------------------------------------------------------------------------------------------
    {
        writeText(file, seedText());
        P.eq("keys.reseeded", waitFor([&] { return !view()->rows.empty(); }) ? 1 : 0, 1);
        settle();
        const std::string seeded = fileText(file);
        P.eq("keys.focus_reset", focusOn(Act::item, up::kReset) ? 1 : 0, 1);
        host.keys("return");
        settle();
        const auto f = card.focused(W(), H());
        P.eq("keys.asked_focus_on_cancel", state().asking && f && f->label == "Cancel" ? 1 : 0, 1);
        host.keys("return");
        settle();
        P.eq("keys.cancelled", !state().asking && fileText(file) == seeded ? 1 : 0, 1);
        host.keys("return");
        settle();
        host.keys("tab");
        settle();
        const auto g = card.focused(W(), H());
        P.eq("keys.tab_to_reset", state().asking && g && g->index == up::kResetConfirm ? 1 : 0, 1);
        P.eq("keys.untouched_asked", fileText(file) == seeded ? 1 : 0, 1);
        host.keys("return");
        settle();
        P.eq("keys.emptied", waitFor(emptied) ? 1 : 0, 1);
    }

    // ---- RESET with a read in flight, and with the card closed at once -----------------------------------------------------
    {
        writeText(file, seedText());
        waitFor([&] { return !view()->rows.empty(); });
        settle();
        usage::add({ usage::today(), "anthropic", "claude-haiku-4-5", false }, counts(1, 111, 0));   // before the cut
        P.eq("inflight.held", holdARead(agent, usage::Range::all) ? 1 : 0, 1);   // the reset's job queues behind it
        port.chat().resetUsage();
        usage::add({ usage::today(), "anthropic", "claude-haiku-4-5", false }, counts(1, 222, 0));   // after the cut
        P.eq("inflight.resetting_said", view()->resetting ? 1 : 0, 1);
        host.tick(5);
        P.eq("inflight.waits_for_read", usage::read(file).empty() ? 0 : 1, 1);
        P.eq("inflight.flush_waits_for_reset", usage::flush() ? 0 : 1, 1);
        agent.holdUsageReadsForTests(false);
        P.eq("inflight.emptied", waitFor(emptied) ? 1 : 0, 1);
        waitFor([&] { return !view()->resetting; });
        usage::flush();
        const std::vector<usage::Row> after = usage::read(file);
        P.eq("inflight.after_cut_kept", after.size() == 1 && after[0].counts.input == 222 ? 1 : 0, 1);
        writeText(file, usage::write({}));

        writeText(file, seedText());
        waitFor([&] { return !view()->rows.empty(); });
        settle();
        clickCode(up::kReset);
        clickCode(up::kResetConfirm);
        host.keys("escape");                     // the card closed at once; nobody asks for a view again
        settle();
        P.eq("closed.card_closed", card.isOpen() ? 0 : 1, 1);
        const auto until = std::chrono::steady_clock::now() + probe::kWorkWait;
        while (!emptied() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        P.eq("closed.emptied", emptied() ? 1 : 0, 1);
    }

    // ---- RESET queued when the agent goes: its destructor runs it -----------------------------------------------------------
    {
        writeText(file, seedText());
        {
            auto other = std::make_unique<pg::Processor>();
            pg::ChatAgent& a = other->chatAgent();
            P.eq("destructor.held", holdARead(a, usage::Range::week) ? 1 : 0, 1);   // its job thread held in a read
            a.resetUsage();                      // queued behind it: the job thread will never reach it
            usage::add({ usage::today(), "anthropic", "claude-haiku-4-5", false }, counts(1, 333, 0));   // after the cut
            P.eq("destructor.queued", usage::read(file).empty() ? 0 : 1, 1);
            other.reset();                       // the agent goes: its queued jobs dropped, the reset run, then its flush
        }
        const std::vector<usage::Row> left = usage::read(file);
        P.eq("destructor.emptied_then_flushed", left.size() == 1 && left[0].counts.input == 333 ? 1 : 0, 1);
        writeText(file, usage::write({}));
        waitFor([&] { return !usage::resetOwed() && !view()->resetting; });
    }

    // ---- RESET with the folder's lock held from outside: it fails, nothing lost ---------------------------------------------
#if !defined(_WIN32)
    {
        panel.openSettings("usage");
        settle();
        writeText(file, seedText());
        waitFor([&] { return !view()->rows.empty(); });
        const std::string seeded = fileText(file);
        const int fd = ::open((file.parent_path() / ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        P.eq("locked.held", fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) == 0 ? 1 : 0, 1);
        port.chat().resetUsage();
        P.eq("locked.said", waitFor([&] { return !view()->resetting && !view()->resetError.empty(); }) ? 1 : 0, 1);
        P.eq("locked.file_kept", fileText(file) == seeded ? 1 : 0, 1);
        if (fd >= 0)
        {
            ::flock(fd, LOCK_UN);
            ::close(fd);
        }
        port.chat().resetUsage();
        P.eq("locked.next_reset", waitFor([&] { return !view()->resetting && view()->resetError.empty() && emptied(); }) ? 1 : 0, 1);
    }
#else
    probe::skip(P, "locked", "flock: the folder's lock is held from outside on POSIX; the refused write below covers Windows");
#endif

    // ---- RESET that fails: nothing lost, said until the next Reset -----------------------------------------------------------
    {
        panel.openSettings("usage");
        settle();
        writeText(file, seedText());
        waitFor([&] { return !view()->rows.empty(); });
        usage::Counts waiting = counts(1, 777, 0);
        usage::add({ usage::today(), "anthropic", "claude-haiku-4-5", false }, waiting);   // counted, not written yet
        settle();
        const std::string seeded = fileText(file);
        usage::setWritesFailForTests(true);
        port.chat().resetUsage();
        P.eq("fail.said", waitFor([&] { return !view()->resetting && !view()->resetError.empty(); }) ? 1 : 0, 1);
        P.eq("fail.file_kept", fileText(file) == seeded ? 1 : 0, 1);
        int64_t haikuInput = 0;
        for (const pg::UsageRow& r : view()->rows)
            if (r.key.model == "claude-haiku-4-5" && r.key.day == usage::today())
                haikuInput += r.counts.input;
        P.eq("fail.waiting_kept", haikuInput, 777);
        settle();
        P.eq("fail.on_page", lineWith("could not be emptied") ? 1 : 0, 1);
        const uint64_t reads = agent.usageReadsForTests();
        agent.setUsageRereadSecondsForTests(0.0);
        waitFor([&] { return agent.usageReadsForTests() >= reads + 3; });
        settle();
        P.eq("fail.said_after_reads", lineWith("could not be emptied") ? 1 : 0, 1);
        agent.setUsageRereadSecondsForTests(0.25);
        usage::setWritesFailForTests(false);
        port.chat().resetUsage();
        const bool cleared = waitFor([&] { return !view()->resetting && view()->resetError.empty() && view()->rows.empty(); });
        P.eq("fail.next_reset_clears", cleared ? 1 : 0, 1);
        P.eq("fail.next_reset_emptied", emptied() ? 1 : 0, 1);
        usage::flush();
        P.eq("fail.waiting_gone_with_it", usage::read(file).empty() ? 1 : 0, 1);
        settle();
        P.eq("fail.unsaid", lineWith("could not be emptied") ? 0 : 1, 1);
    }

    // ---- RESET failing when another instance's thread runs it: every page says so ------------------------------------------
    {
        writeText(file, seedText());
        waitFor([&] { return !view()->rows.empty(); });
        auto other = std::make_unique<pg::Processor>();
        pg::ChatAgent& b = other->chatAgent();
        b.setUsageRereadSecondsForTests(0.0);
        const auto bView = [&] { return b.usage(usage::Range::month); };
        P.eq("two.b_read", waitFor([&] { return bView()->read; }) ? 1 : 0, 1);
        agent.setUsageRereadSecondsForTests(0.0);
        // A's job thread held in a read, waited for: A can run no reset (its reads and its reset job queue behind it).
        P.eq("two.a_held", holdARead(agent, usage::Range::all) ? 1 : 0, 1);
        usage::setWritesFailForTests(true);
        const uint64_t bReads = b.usageReadsForTests();  // before the confirmation: a B read counted after it ran after it
        port.chat().resetUsage();                        // A confirms: the reset is owed by the ledger
        // B's page asks for its view, as a page does, so that B reads; a B read that ends after the confirmation runs the
        // owed reset before it lets go. With A still held and no other agent, "no longer owed" means B's thread ran it.
        const bool ran = waitFor([&] {
            (void) bView();
            return agent.usageReadsHeldForTests() == 1 && b.usageReadsForTests() > bReads && !usage::resetOwed();
        });
        P.eq("two.b_ran_it", ran ? 1 : 0, 1);
        P.eq("two.a_still_held", agent.usageReadsHeldForTests(), 1);
        P.eq("two.b_says", waitFor([&] { return !bView()->resetError.empty() && !bView()->resetting; }) ? 1 : 0, 1);
        agent.holdUsageReadsForTests(false);
        P.eq("two.a_says", waitFor([&] { return !view()->resetError.empty() && !view()->resetting; }) ? 1 : 0, 1);
        P.eq("two.file_kept", usage::read(file).empty() ? 0 : 1, 1);
        usage::setWritesFailForTests(false);
        port.chat().resetUsage();
        P.eq("two.a_cleared", waitFor([&] { return view()->resetError.empty() && !view()->resetting && emptied(); }) ? 1 : 0, 1);
        P.eq("two.b_cleared", waitFor([&] { return bView()->resetError.empty() && !bView()->resetting; }) ? 1 : 0, 1);
        agent.setUsageRereadSecondsForTests(0.25);
        other.reset();
    }

    host.keys("escape");
    settle();
    store.deleteRecursively();
    projects.deleteRecursively();
    return P.finish();
}
