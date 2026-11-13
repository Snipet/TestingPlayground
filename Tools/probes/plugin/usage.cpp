// PROBE name=plugin.usage timeout=180
//
// plugin.usage: the chat's usage ledger (plugin/Usage.h, UsagePrices.cpp, ChatUsage.cpp) in a folder of the probe's own,
// and the agent adding to it over scripted APIs (FakeClaude.h): nothing reaches the network.
//   TABLE     the built-in table: thirteen rows, its date, each cache write 1.25 and 2 times the input price; a dated id
//             takes its model's row; a model it does not know has none
//   PRICES    the arithmetic for each row: a million tokens of each kind costs the row's prices, fast mode's at its own
//             prices (its cache reads and writes by its multiplier), a web search $0.01; a fast row of a model with no
//             fast mode, and an unknown model, have tokens and no cost; a reported cost wins over the table; a row
//             partly reported is an estimate; the words ("$0.12", "~$0.004", "<$0.001")
//   REFRESH   a table with a later date replaces the built-in one, an earlier or the same one does not
//   DAYS      the day arithmetic both ways; 400 days kept by day, older days folded into their month; the ranges
//   FILE      two ledgers on one file add up (and a flush with nothing pending writes nothing); what does not parse is
//             kept beside as usage.json.bad and a new ledger begins; the file 0600 (POSIX)
//   TURN      a turn over the scripted API adds each request once, under the model that served it: the cache writes
//             by lifetime (the split, and a total without one as five minutes'); the footer keeps its words and adds
//             the turn's cost
//   RETRY     a refused request (529) adds nothing, its retry one; a stream that broke off after message_start adds
//             itself and the retry again
//   FALLBACK  a request a fallback model served is counted under that model
//   PLANNER   the planner's request under its model, the builder's under its own
//   WEB       web searches and fetches counted
//   FAST      a request that ran fast is a fast row at fast mode's price
//   COMPACT   a request that compacted counts its iterations' tokens
//   PRIVACY   after a chat with known words, the file holds names and numbers alone: none of the words, no key, no
//             server address
//   OTHERS    OpenRouter: its reported cost kept (the row "reported"), its list's prices taken, the request asking for its
//             usage accounting; a server on this machine is "local", with tokens and no price; OpenAI and Gemini with
//             tokens and no price
#include "Probe.h"

#include "FakeClaude.h"

#include "plugin/ChatAgent.h"
#include "plugin/ChatStore.h"
#include "plugin/ClaudeApi.h"
#include "plugin/Processor.h"
#include "plugin/Providers.h"
#include "plugin/Usage.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace pg = playground;
namespace claude = playground::claude;
namespace usage = playground::usage;
namespace fs = std::filesystem;

namespace
{
    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    fake::Response stream(const std::string& events)
    {
        fake::Response r;
        r.stream = events;
        return r;
    }

    std::string fileText(const fs::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        std::ostringstream s;
        s << in.rdbuf();
        return s.str();
    }

    // The process's ledger's counts, written: of `provider`'s `model` (any day), fast or not.
    usage::Counts counted(const std::string& provider, const std::string& model, bool fast = false)
    {
        usage::flush();
        usage::Counts c;
        for (const usage::Row& r : usage::read(usage::processFile()))
            if (r.key.provider == provider && r.key.model == model && r.key.fast == fast)
                c += r.counts;
        return c;
    }

    // A million of every kind of token.
    usage::Counts million()
    {
        usage::Counts c;
        c.requests = 1;
        c.input = c.output = c.cacheRead = c.cacheWrite5m = c.cacheWrite1h = 1000000;
        return c;
    }

    std::string lastText(const pg::ChatState& s, pg::ChatEntry::Kind kind)
    {
        for (auto it = s.entries.rbegin(); it != s.entries.rend(); ++it)
            if (it->kind == kind)
                return it->text;
        return {};
    }
}

PROBE(plugin, usage)
{
    const funkgui::HeadlessGuiScope gui;
    const juce::File keys = juce::File(juce::String(probe::outPath("usage-keys")));
    keys.deleteRecursively();
    pg::chatstore::setDirectoryForTests(keys);
    usage::resetPricesForTests();
    const std::string day = "2026-11-12";

    // ---- TABLE -----------------------------------------------------------------------------------------------------------
    {
        const usage::PriceTable& t = usage::builtInPrices();
        P.eq("table.rows", static_cast<int64_t>(t.rows.size()), 13);
        P.eq("table.date", t.asOf == "2026-11-12" ? 1 : 0, 1);
        bool writes = true, anthropic = true, positive = true;
        std::set<std::string> ids;
        for (const usage::PriceRow& r : t.rows)
        {
            writes = writes && std::abs(r.price.write5m - 1.25 * r.price.in) < 1e-9 && std::abs(r.price.write1h - 2 * r.price.in) < 1e-9;
            anthropic = anthropic && r.provider == "anthropic";
            positive = positive && r.price.in > 0 && r.price.out > 0 && r.price.cacheRead > 0 && std::abs(r.price.webSearch - 0.01) < 1e-12;
            ids.insert(r.model);
        }
        P.eq("table.cache_writes", writes ? 1 : 0, 1);
        P.eq("table.anthropic", anthropic ? 1 : 0, 1);
        P.eq("table.priced", positive ? 1 : 0, 1);
        P.eq("table.ids_unique", static_cast<int64_t>(ids.size()), 13);
        P.eq("table.dated_id", usage::priceOf("anthropic", "claude-opus-5-5-20261001").has_value() ? 1 : 0, 1);
        P.eq("table.unknown_none", usage::priceOf("anthropic", "claude-opus-9").has_value()
                                       || usage::priceOf("anthropic", "claude-opus-5-5-x").has_value()
                                       || usage::priceOf("openai", "claude-opus-5-5").has_value() ? 1 : 0, 0);
    }

    // ---- PRICES ----------------------------------------------------------------------------------------------------------
    {
        bool every = true, fastEvery = true;
        int fastModels = 0;
        for (const usage::PriceRow& r : usage::builtInPrices().rows)
        {
            const usage::Price& p = r.price;
            const usage::Key k{ day, "anthropic", r.model, false };
            const usage::Cost c = usage::costOf(k, million());
            const double want = (p.in + p.out + p.cacheRead + p.write5m + p.write1h) * 1e6;
            const bool ok = c.kind == usage::Cost::Kind::estimate && std::abs(static_cast<double>(c.microUsd) - want) <= 1;
            if (!ok)
                std::printf("price %s: %lld, want %.0f\n", r.model.c_str(), static_cast<long long>(c.microUsd), want);
            every = every && ok;
            const usage::Cost f = usage::costOf({ day, "anthropic", r.model, true }, million());
            if (p.fastIn > 0)
            {
                ++fastModels;
                const double m = p.fastIn / p.in;
                const double fastWant = (p.fastIn + p.fastOut + (p.cacheRead + p.write5m + p.write1h) * m) * 1e6;
                fastEvery = fastEvery && f.kind == usage::Cost::Kind::estimate && std::abs(static_cast<double>(f.microUsd) - fastWant) <= 1;
            }
            else
                fastEvery = fastEvery && f.kind == usage::Cost::Kind::none;
        }
        P.eq("prices.each_row", every ? 1 : 0, 1);
        P.eq("prices.fast_rows", fastModels, 4);
        P.eq("prices.fast_each", fastEvery ? 1 : 0, 1);
        // Claude Opus 5.5 by kind: 1M input $4, output $20, a cache read $0.20, writes $5 and $8.
        const usage::Key opus{ day, "anthropic", "claude-opus-5-5", false };
        const auto one = [&](int64_t usage::Counts::* m) {
            usage::Counts c;
            c.requests = 1;
            c.*m = 1000000;
            return usage::costOf(opus, c).microUsd;
        };
        P.eq("prices.opus_input", one(&usage::Counts::input), 4000000);
        P.eq("prices.opus_output", one(&usage::Counts::output), 20000000);
        P.eq("prices.opus_read", one(&usage::Counts::cacheRead), 200000);
        P.eq("prices.opus_write_5m", one(&usage::Counts::cacheWrite5m), 5000000);
        P.eq("prices.opus_write_1h", one(&usage::Counts::cacheWrite1h), 8000000);
        usage::Counts searches;
        searches.requests = 1;
        searches.webSearches = 3;
        searches.webFetches = 5;
        P.eq("prices.web_search", usage::costOf(opus, searches).microUsd, 30000);
        usage::Counts fastOut;
        fastOut.requests = 1;
        fastOut.output = 1000000;
        P.eq("prices.fast_output", usage::costOf({ day, "anthropic", "claude-opus-5-5", true }, fastOut).microUsd, 40000000);
        P.eq("prices.fable_fast_output", usage::costOf({ day, "anthropic", "claude-fable-5-2", true }, fastOut).microUsd, 80000000);
        // An unknown model: tokens, no cost, no words.
        const usage::Cost unknown = usage::costOf({ day, "openai", "gpt-probe-5", false }, million());
        P.eq("prices.unknown_none", unknown.kind == usage::Cost::Kind::none && usage::costText(unknown).empty() ? 1 : 0, 1);
        // Reported: wins over the table where every request reported; a row partly reported is an estimate.
        usage::Counts reported = million();
        reported.requests = 2;
        reported.reportedRequests = 2;
        reported.reportedMicroUsd = 123456;
        const usage::Cost r = usage::costOf(opus, reported);
        P.eq("prices.reported_wins", r.kind == usage::Cost::Kind::reported && r.microUsd == 123456 ? 1 : 0, 1);
        reported.reportedRequests = 1;
        const usage::Cost partly = usage::costOf(opus, reported);
        P.eq("prices.partly_estimate", partly.kind == usage::Cost::Kind::estimate && partly.microUsd != 123456 ? 1 : 0, 1);
        const usage::Cost partlyUnknown = usage::costOf({ day, "openrouter", "acme/unpriced", false }, reported);
        P.eq("prices.partly_unpriced_none", partlyUnknown.kind == usage::Cost::Kind::none ? 1 : 0, 1);
        P.eq("prices.words", usage::costText({ usage::Cost::Kind::reported, 120000 }) == "$0.12"
                                 && usage::costText({ usage::Cost::Kind::estimate, 1234567 }) == "~$1.23"
                                 && usage::costText({ usage::Cost::Kind::estimate, 4000 }) == "~$0.004"
                                 && usage::costText({ usage::Cost::Kind::reported, 300 }) == "<$0.001"
                                 && usage::costText({ usage::Cost::Kind::reported, 0 }) == "$0.000" ? 1 : 0, 1);
    }

    // ---- REFRESH ---------------------------------------------------------------------------------------------------------
    {
        usage::PriceTable earlier{ "2026-10-01", { { "anthropic", "claude-opus-5-5", { 1, 1, 1, 1, 1, 0, 0, 0 } } } };
        P.eq("refresh.earlier_refused", usage::offerPrices(earlier) ? 1 : 0, 0);
        usage::PriceTable same{ usage::builtInPrices().asOf, earlier.rows };
        P.eq("refresh.same_refused", usage::offerPrices(same) ? 1 : 0, 0);
        P.eq("refresh.builtin_in_use", usage::pricesInUse().asOf == usage::builtInPrices().asOf ? 1 : 0, 1);
        usage::PriceTable later{ "2026-12-01", { { "anthropic", "claude-opus-5-5", { 3, 15, 0.3, 3.75, 6, 0, 0, 0.01 } },
                                                 { "openai", "gpt-probe-5", { 1, 2, 0.1, 1, 1, 0, 0, 0 } } } };
        P.eq("refresh.later_taken", usage::offerPrices(later) ? 1 : 0, 1);
        P.eq("refresh.in_use", usage::pricesInUse().asOf == "2026-12-01" ? 1 : 0, 1);
        const auto opus = usage::priceOf("anthropic", "claude-opus-5-5");
        P.eq("refresh.price", opus && std::abs(opus->in - 3) < 1e-9 ? 1 : 0, 1);
        P.eq("refresh.other_provider", usage::priceOf("openai", "gpt-probe-5").has_value() ? 1 : 0, 1);
        P.eq("refresh.then_earlier_refused", usage::offerPrices(usage::builtInPrices()) ? 1 : 0, 0);
        usage::resetPricesForTests();
        P.eq("refresh.reset", usage::pricesInUse().asOf == usage::builtInPrices().asOf ? 1 : 0, 1);
    }

    // ---- DAYS ------------------------------------------------------------------------------------------------------------
    {
        P.eq("days.epoch", usage::dayNumber("1970-01-01"), 0);
        P.eq("days.leap", usage::dayNumber("2024-03-01") - usage::dayNumber("2024-02-28"), 2);
        bool both = true;
        for (int64_t n = usage::dayNumber("2020-01-01"); n < usage::dayNumber("2031-01-01"); n += 17)
            both = both && usage::dayNumber(usage::dayOf(n)) == n && usage::validDay(usage::dayOf(n));
        P.eq("days.round_trip", both ? 1 : 0, 1);
        P.eq("days.valid", usage::validDay("2026-11-12") && !usage::validDay("2026-02-30") && !usage::validDay("2026-13-01")
                               && !usage::validDay("26-11-12") && usage::validMonth("2025-08") && !usage::validMonth("2025-8") ? 1 : 0, 1);
        P.eq("days.today", usage::validDay(usage::today()) ? 1 : 0, 1);
        // 400 days by day, then months.
        std::vector<usage::Row> rows;
        const int64_t now = usage::dayNumber(day);
        for (int64_t back = 0; back < 800; ++back)
        {
            usage::Counts c;
            c.requests = 1;
            c.input = 10;
            rows.push_back({ { usage::dayOf(now - back), "anthropic", "claude-opus-5-5", false }, c });
        }
        const std::vector<usage::Row> kept = usage::tidy(rows, day);
        int days = 0, months = 0;
        int64_t requests = 0;
        for (const usage::Row& r : kept)
        {
            days += r.key.day.size() == 10 ? 1 : 0;
            months += r.key.day.size() == 7 ? 1 : 0;
            requests += r.counts.requests;
        }
        P.eq("days.by_day", days, usage::kDaysKept);
        P.ge("days.months", months, 13);
        P.le("days.months_at_most", months, 15);
        P.eq("days.nothing_lost", requests, 800);
        usage::Ledger l(fs::path(probe::outPath("usage-ranges.json")));
        std::error_code ec;
        fs::remove(l.file(), ec);
        for (const usage::Row& r : rows)
            l.add(r.key, r.counts);
        P.eq("days.flushed", l.flush(day) ? 1 : 0, 1);
        const auto inRange = [&](usage::Range range) {
            int64_t n = 0;
            for (const usage::Row& r : l.snapshot(range, day).rows)
                n += r.counts.requests;
            return n;
        };
        P.eq("days.week", inRange(usage::Range::week), 7);
        P.eq("days.month", inRange(usage::Range::month), 30);
        // Twelve months: this one's twelve days and the eleven before it whole.
        P.eq("days.year", inRange(usage::Range::year), usage::dayNumber(day) - usage::dayNumber("2025-12-01") + 1);
        P.eq("days.all", inRange(usage::Range::all), 800);
    }

    // ---- FILE ------------------------------------------------------------------------------------------------------------
    {
        const fs::path file = probe::outPath("usage-two.json");
        std::error_code ec;
        fs::remove(file, ec);
        usage::Ledger a(file), b(file);
        usage::Counts c = million();
        a.add({ day, "anthropic", "claude-opus-5-5", false }, c);
        b.add({ day, "anthropic", "claude-opus-5-5", false }, c);
        b.add({ day, "openrouter", "acme/model", false }, c);
        P.eq("file.a", a.flush(day) ? 1 : 0, 1);
        P.eq("file.b", b.flush(day) ? 1 : 0, 1);
        a.add({ day, "anthropic", "claude-opus-5-5", false }, c);
        P.eq("file.a_again", a.flush(day) ? 1 : 0, 1);
        const std::vector<usage::Row> rows = usage::read(file);
        int64_t opus = 0, other = 0;
        for (const usage::Row& r : rows)
            (r.key.provider == "anthropic" ? opus : other) += r.counts.requests;
        P.eq("file.added_up", opus, 3);
        P.eq("file.other_row", other, 1);
        P.eq("file.input_added", rows.size() == 2 && rows[0].counts.input + rows[1].counts.input == 4000000 ? 1 : 0, 1);
        const auto before = fs::last_write_time(file, ec);
        P.eq("file.nothing_pending", a.flush(day) && fs::last_write_time(file, ec) == before ? 1 : 0, 1);
        const std::string text = fileText(file);
        P.eq("file.format", has(text, "\"format\": \"funk-playground-usage\"") && has(text, "\"version\": 1") ? 1 : 0, 1);
        P.eq("file.no_temp_left", [&] {
            for (const auto& e : fs::directory_iterator(file.parent_path()))
                if (has(e.path().filename().string(), "usage-two.json.") && has(e.path().filename().string(), ".tmp"))
                    return 0;
            return 1;
        }(), 1);
#if !defined(_WIN32)
        const fs::perms perms = fs::status(file).permissions();
        P.eq("file.private", (perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none ? 1 : 0, 1);
#else
        probe::skip(P, "file.private", "POSIX permissions: Windows keeps the folder's ACL");
#endif
        // Not a ledger: kept beside, and a new one begins.
        { std::ofstream(file, std::ios::trunc) << "{\"something\": \"else\"}"; }
        bool damaged = false;
        P.eq("file.damaged_read_nothing", usage::read(file, &damaged).empty() && damaged ? 1 : 0, 1);
        a.add({ day, "anthropic", "claude-opus-5-5", false }, c);
        P.eq("file.damaged_flush", a.flush(day) ? 1 : 0, 1);
        fs::path bad = file;
        bad += ".bad";
        P.eq("file.damaged_kept", has(fileText(bad), "something") ? 1 : 0, 1);
        P.eq("file.new_ledger", usage::read(file).size() == 1 && usage::read(file)[0].counts.requests == 1 ? 1 : 0, 1);
    }

    // ---- the agent ---------------------------------------------------------------------------------------------------------
    pg::Processor processor;
    pg::ChatAgent& agent = processor.chatAgent();
    auto script = std::make_shared<fake::Script>();
    script->models = fake::modelsBody({ { "claude-opus-5-5", "Claude Opus 5.5", true, { "low", "medium", "high", "xhigh", "max" } },
                                        { "claude-sonnet-5-5", "Claude Sonnet 5.5", true, { "low", "medium", "high", "xhigh" } } });
    agent.setTransport(std::make_unique<fake::Transport>(script));
    std::string why;
    P.eq("agent.key", agent.saveKey(fake::fakeKey(), why) ? 1 : 0, 1);
    agent.setModel("claude-opus-5-5");

    // ---- TURN ------------------------------------------------------------------------------------------------------------
    {
        script->add(fake::Response::ok(fake::Sse().startSplit("claude-opus-5-5", 1000, 20000, 3000, 4000)
                                           .text("The zebra-quartz-marmalade answer.").end("end_turn", 600)));
        agent.send("Tell me about zebra-quartz-marmalade, please.");
        agent.waitIdle();
        const usage::Counts c = counted("anthropic", "claude-opus-5-5");
        P.eq("turn.requests", c.requests, 1);
        P.eq("turn.input", c.input, 1000);
        P.eq("turn.output", c.output, 600);
        P.eq("turn.cache_read", c.cacheRead, 20000);
        P.eq("turn.write_5m", c.cacheWrite5m, 3000);
        P.eq("turn.write_1h", c.cacheWrite1h, 4000);
        const std::string footer = agent.chatState().usage;
        std::printf("footer: %s\n", footer.c_str());
        // 1000 * 4 + 600 * 20 + 20000 * 0.2 + 3000 * 5 + 4000 * 8 millionths = $0.067, an estimate
        P.eq("turn.footer_words", has(footer, "Last reply: 28K tokens in (20K cached), 600 out") ? 1 : 0, 1);
        P.eq("turn.footer_cost", has(footer, ", ~$0.07") ? 1 : 0, 1);
        // A server that says a total and no split: all of it five minutes'.
        script->add(fake::Response::ok(fake::Sse().start("claude-opus-5-5", 100, 0, 2500).text("Hm.").end("end_turn", 10)));
        agent.send("again");
        agent.waitIdle();
        const usage::Counts d = counted("anthropic", "claude-opus-5-5");
        P.eq("turn.total_as_5m", d.cacheWrite5m - c.cacheWrite5m, 2500);
        P.eq("turn.total_no_1h", d.cacheWrite1h - c.cacheWrite1h, 0);
    }

    // ---- RETRY -----------------------------------------------------------------------------------------------------------
    {
        const usage::Counts before = counted("anthropic", "claude-opus-5-5");
        script->add(fake::Response::failure(529, "overloaded_error", "Overloaded", 1));
        script->add(fake::Response::ok(fake::Sse().start("claude-opus-5-5", 200).text("Back.").end("end_turn", 5)));
        agent.send("are you there?");
        agent.waitIdle();
        P.eq("retry.refused_not_counted", counted("anthropic", "claude-opus-5-5").requests - before.requests, 1);
        // A stream that broke off after message_start (no message_stop), asked again: both counted.
        const usage::Counts mid = counted("anthropic", "claude-opus-5-5");
        script->add(fake::Response::ok(fake::Sse().start("claude-opus-5-5", 300).text("Half an ans")));
        script->add(fake::Response::ok(fake::Sse().start("claude-opus-5-5", 300).text("A whole answer.").end("end_turn", 7)));
        agent.send("go on");
        agent.waitIdle();
        P.eq("retry.answered", lastText(agent.chatState(), pg::ChatEntry::Kind::assistant) == "A whole answer." ? 1 : 0, 1);
        const usage::Counts after = counted("anthropic", "claude-opus-5-5");
        P.eq("broken.counted_twice", after.requests - mid.requests, 2);
        P.eq("broken.input_twice", after.input - mid.input, 600);
    }

    // ---- FALLBACK --------------------------------------------------------------------------------------------------------
    {
        const int64_t opusBefore = counted("anthropic", "claude-opus-5-5").requests;
        script->add(fake::Response::ok(fake::Sse().start("claude-opus-4-8", 400).fallback("claude-opus-5-5", "claude-opus-4-8")
                                           .text("Carried on.").end("end_turn", 9)));
        agent.send("something");
        agent.waitIdle();
        P.eq("fallback.served_model", counted("anthropic", "claude-opus-4-8").requests, 1);
        P.eq("fallback.not_asked_model", counted("anthropic", "claude-opus-5-5").requests - opusBefore, 0);
    }

    // ---- PLANNER ---------------------------------------------------------------------------------------------------------
    {
        agent.setModel("claude-sonnet-5-5");
        agent.setPlanner("claude-opus-5-5");
        const int64_t opusBefore = counted("anthropic", "claude-opus-5-5").requests;
        const int64_t sonnetBefore = counted("anthropic", "claude-sonnet-5-5").requests;
        script->add(fake::Response::ok(fake::Sse().start("claude-opus-5-5", 700).text("1. A sine.").end("end_turn", 30)));
        script->add(fake::Response::ok(fake::Sse().start("claude-sonnet-5-5", 800).text("Built it.").end("end_turn", 40)));
        agent.send("a plan, please");
        agent.waitIdle();
        P.eq("planner.its_model", counted("anthropic", "claude-opus-5-5").requests - opusBefore, 1);
        P.eq("planner.builder_model", counted("anthropic", "claude-sonnet-5-5").requests - sonnetBefore, 1);
        P.eq("planner.footer_words", has(agent.chatState().usage, "planner ") ? 1 : 0, 1);
        agent.setPlanner("");
        agent.setModel("claude-opus-5-5");
    }

    // ---- WEB -------------------------------------------------------------------------------------------------------------
    {
        const usage::Counts before = counted("anthropic", "claude-opus-5-5");
        script->add(fake::Response::ok(fake::Sse().start("claude-opus-5-5", 900).text("Found it.").serverUse(2, 1).end("end_turn", 12)));
        agent.send("look it up");
        agent.waitIdle();
        const usage::Counts after = counted("anthropic", "claude-opus-5-5");
        P.eq("web.searches", after.webSearches - before.webSearches, 2);
        P.eq("web.fetches", after.webFetches - before.webFetches, 1);
    }

    // ---- FAST ------------------------------------------------------------------------------------------------------------
    {
        script->add(fake::Response::ok(fake::Sse().start("claude-opus-5-5", 1000).text("Quick.").end("end_turn", 1000, "fast")));
        agent.send("quickly");
        agent.waitIdle();
        const usage::Counts f = counted("anthropic", "claude-opus-5-5", true);
        P.eq("fast.row", f.requests, 1);
        // 1000 * 8 + 1000 * 40 millionths: $0.048, an estimate.
        P.eq("fast.footer", has(agent.chatState().usage, ", fast") && has(agent.chatState().usage, ", ~$0.05") ? 1 : 0, 1);
    }

    // ---- COMPACT ---------------------------------------------------------------------------------------------------------
    {
        const usage::Counts before = counted("anthropic", "claude-opus-5-5");
        script->add(fake::Response::ok(fake::Sse().start("claude-opus-5-5", 5000).compaction("The summary.").text("Carry on.")
                                           .endCompacted(160000, 3000, 5000, 200)));
        agent.send("long");
        agent.waitIdle();
        const usage::Counts after = counted("anthropic", "claude-opus-5-5");
        P.eq("compact.input", after.input - before.input, 165000);
        P.eq("compact.output", after.output - before.output, 3200);
    }

    // ---- OTHERS ----------------------------------------------------------------------------------------------------------
    {
        const std::string orKey = fake::fakeOpenRouterKey();
        fake::ListedModel priced{ "anthropic/claude-probe", "Claude via OpenRouter", 1762000000, 200000, true, true };
        priced.pricePrompt = "0.000003";
        priced.priceCompletion = "0.000015";
        priced.priceCacheRead = "0.0000003";
        fake::ListedModel free{ "meta/llama-probe", "Llama", 1761000000, 131072, true, true };
        free.pricePrompt = "-1";
        free.priceCompletion = "0";
        script->gets["/api/v1/models"] = { 200, fake::openAiModels({ priced, free }, true) };
        P.eq("openrouter.set", agent.setProvider({ "openai", "https://openrouter.ai/api/v1", "" }, orKey, why) ? 1 : 0, 1);
        agent.waitIdle();
        agent.setModel("anthropic/claude-probe");
        const auto listed = usage::priceOf("openrouter", "anthropic/claude-probe");
        P.eq("openrouter.list_price", listed && std::abs(listed->in - 3) < 1e-9 && std::abs(listed->out - 15) < 1e-9
                                          && std::abs(listed->cacheRead - 0.3) < 1e-9 ? 1 : 0, 1);
        P.eq("openrouter.list_negative_none", usage::priceOf("openrouter", "meta/llama-probe").has_value() ? 1 : 0, 0);
        const int before = script->postCount();
        script->add(stream(fake::OpenAiSse("anthropic/claude-probe").text("Hello.").finish("stop").usageCost(12000, 80, 3000, 0.0417).done().str()));
        agent.send("hi there");
        agent.waitIdle();
        const juce::var body = claude::parseJson(script->body(before));
        P.eq("openrouter.asks_usage", static_cast<bool>(body.getProperty("usage", {}).getProperty("include", false)) ? 1 : 0, 1);
        const usage::Counts c = counted("openrouter", "anthropic/claude-probe");
        P.eq("openrouter.tokens", c.requests == 1 && c.input == 9000 && c.cacheRead == 3000 && c.output == 80 ? 1 : 0, 1);
        P.eq("openrouter.reported", c.reportedRequests == 1 && c.reportedMicroUsd == 41700 ? 1 : 0, 1);
        const usage::Cost cost = usage::costOf({ day, "openrouter", "anthropic/claude-probe", false }, c);
        P.eq("openrouter.cost_reported", cost.kind == usage::Cost::Kind::reported && cost.microUsd == 41700 ? 1 : 0, 1);
        P.eq("openrouter.footer", has(agent.chatState().usage, ", $0.04") ? 1 : 0, 1);
        // Without a reported cost: the list's price, an estimate.
        script->add(stream(fake::OpenAiSse("anthropic/claude-probe").text("Again.").finish("stop").usage(1000, 200, 0).done().str()));
        agent.send("again");
        agent.waitIdle();
        // 1000 * 3 + 200 * 15 millionths: $0.006.
        P.eq("openrouter.estimate_footer", has(agent.chatState().usage, ", ~$0.006") ? 1 : 0, 1);

        // A server on this machine: "local", tokens, no price, no cost in the footer.
        script->gets["/v1/models"] = { 200, fake::openAiModels({ { "qwen-probe:32b", "", 1762400000 } }) };
        P.eq("local.set", agent.setProvider({ "openai", "http://localhost:11434/v1", "" }, "", why) ? 1 : 0, 1);
        agent.waitIdle();
        script->add(stream(fake::OpenAiSse("qwen-probe:32b").text("Local words.").finish("stop").usage(2048, 10).done().str()));
        agent.send("local hello");
        agent.waitIdle();
        const usage::Counts l = counted("local", "qwen-probe:32b");
        P.eq("local.counted", l.requests == 1 && l.input == 2048 && l.output == 10 ? 1 : 0, 1);
        P.eq("local.no_price", usage::costOf({ day, "local", "qwen-probe:32b", false }, l).kind == usage::Cost::Kind::none ? 1 : 0, 1);
        P.eq("local.footer_no_cost", has(agent.chatState().usage, "$") ? 1 : 0, 0);

        // OpenAI: tokens, no price until a table has one.
        script->gets["/v1/models"] = { 200, fake::openAiModels({ { "gpt-probe-5", "", 1762000000 } }) };
        P.eq("openai.set", agent.setProvider({ "openai", "https://api.openai.com/v1", "gpt-probe-5" }, fake::fakeOpenAiKey(), why) ? 1 : 0, 1);
        agent.waitIdle();
        script->add(stream(fake::OpenAiSse("gpt-probe-5").text("OpenAI words.").finish("stop").usage(500, 20).done().str()));
        agent.send("openai hello");
        agent.waitIdle();
        const usage::Counts o = counted("openai", "gpt-probe-5");
        P.eq("openai.counted", o.requests == 1 && o.input == 500 && o.output == 20 ? 1 : 0, 1);
        P.eq("openai.no_price", usage::costOf({ day, "openai", "gpt-probe-5", false }, o).kind == usage::Cost::Kind::none ? 1 : 0, 1);

        // Gemini: its own name.
        script->gets["/v1beta/models?pageSize=1000"] = { 200, fake::geminiModels({ { "gemini-3-pro-preview", "Gemini 3 Pro", 0, 1048576 } }) };
        P.eq("gemini.set", agent.setProvider({ "gemini", "", "gemini-3-pro-preview" }, fake::fakeGeminiKey(), why) ? 1 : 0, 1);
        agent.waitIdle();
        script->add(stream(fake::GeminiSse("gemini-3-pro-preview").text("Gemini words.").finish("STOP", 700, 30).str()));
        agent.send("gemini hello");
        agent.waitIdle();
        int64_t gemini = 0;
        usage::flush();
        for (const usage::Row& r : usage::read(usage::processFile()))
            gemini += r.key.provider == "gemini" ? r.counts.requests : 0;
        P.eq("gemini.counted", gemini, 1);
        P.eq("names.kinds", usage::providerName("anthropic", "", false) == "anthropic" && usage::providerName("gemini", "", false) == "gemini"
                                && usage::providerName("openai", "openrouter.ai", false) == "openrouter"
                                && usage::providerName("openai", "api.openai.com", false) == "openai"
                                && usage::providerName("openai", "localhost:1234", true) == "local"
                                && usage::providerName("openai", "llm.example.org", false) == "other" ? 1 : 0, 1);
        P.eq("names.model", usage::modelName("a model with spaces") == "unknown" && usage::modelName("") == "unknown"
                                && usage::modelName(std::string(201, 'a')) == "unknown" && usage::modelName("qwen:32b") == "qwen:32b" ? 1 : 0, 1);
        P.eq("back.set", agent.setProvider({}, "", why) ? 1 : 0, 1);
    }

    // ---- PRIVACY ---------------------------------------------------------------------------------------------------------
    {
        usage::flush();
        const std::string text = fileText(usage::processFile());
        std::printf("usage.json: %zu bytes\n", text.size());
        P.eq("privacy.no_words", has(text, "zebra") || has(text, "marmalade") || has(text, "hello") || has(text, "Hello")
                                         || has(text, "answer") || has(text, "words") ? 1 : 0, 0);
        P.eq("privacy.no_key", has(text, fake::fakeKey().substr(0, 12)) || has(text, fake::fakeOpenRouterKey().substr(0, 10))
                                       || has(text, "sk-") || has(text, "AIza") ? 1 : 0, 0);
        P.eq("privacy.no_address", has(text, "openrouter.ai") || has(text, "localhost") || has(text, "http") ? 1 : 0, 0);
        // Every string in it is a field's name, a day, a provider's name or a model's id; nothing else.
        const juce::var v = juce::JSON::parse(juce::String(text));
        const std::set<std::string> fields{ "day", "provider", "model", "fast", "requests", "input", "output", "cache_read",
                                            "cache_write_5m", "cache_write_1h", "web_searches", "web_fetches",
                                            "reported_micro_usd", "reported_requests" };
        const std::set<std::string> providers{ "anthropic", "openrouter", "openai", "gemini", "local", "other" };
        bool only = v.getProperty("format", "").toString() == "funk-playground-usage" && v.getDynamicObject() != nullptr
                 && v.getDynamicObject()->getProperties().size() == 3;
        int rows = 0;
        if (const auto* days = v.getProperty("days", {}).getArray())
            for (const juce::var& d : *days)
            {
                ++rows;
                for (const auto& nv : d.getDynamicObject()->getProperties())
                    only = only && fields.count(nv.name.toString().toStdString()) > 0;
                only = only && usage::validDay(d.getProperty("day", "").toString().toStdString())
                    && providers.count(d.getProperty("provider", "").toString().toStdString()) > 0
                    && usage::modelName(d.getProperty("model", "").toString().toStdString()) == d.getProperty("model", "").toString().toStdString();
            }
        P.ge("privacy.rows", rows, 6);
        P.eq("privacy.names_and_numbers", only ? 1 : 0, 1);
    }

    pg::chatstore::setDirectoryForTests(juce::File());
    return P.finish();
}
