// Source/editor/UsagePage.h: the settings card's Usage page (SettingsPages.h's kUsagePage): what the chat's requests
// cost, read from the usage ledger (plugin/Usage.h) through ChatPort::usage, which reads it on the chat's job thread:
// the message thread never waits on the disk or the ledger's lock.
//
//   a range (7 days, 30 days, 12 months, All) and Tokens or Cost;
//   a chart (the page's own control, drawn by its paint(): UsagePage.cpp): a bar a day (a month in 12 months and All),
//   stacked by model, the largest models a colour each (kMaxSeries), the rest Other, round steps up its axis; the bar
//   under the pointer, or the one a click or the keys picked (Left, Right, Home, End with the chart focused; a key's
//   pick holds until the pointer moves), read out under it, else the legend; each bar an accessibility item;
//   a table: a row a model (its provider, requests, tokens in, cached, out, cost, share) and the total;
//   the footnote: the date of the price table in use, `~` before an estimate, "no price known" where there is none;
//   Reset..., which asks first (ChatPort::resetUsage).
//
// An estimate is never called a cost: `~` before it; a figure without one is the provider's own. Nothing of a chat is
// read or shown: the ledger holds names and numbers alone. The model below is pure, for the probes (gui.usage).
#pragma once

#include "editor/Theme.h"
#include "plugin/Usage.h"

#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Theme.h>

#include <cstdint>
#include <string>
#include <vector>

namespace playground::ui
{
    inline constexpr funkgui::Tag kUsageBarTag = funkgui::tags::firstProduct + 41;   // a bar's stack (its segments)

    namespace usagepage
    {
        // The page's own codes (Act::item).
        constexpr int kRange = 0, kShow = 1, kChart = 2, kReset = 3, kResetYes = 4, kResetNo = 5;
        constexpr int kMaxSeries = 1 + kSeriesInks;   // a colour each (the accent, then Theme.h's); the rest Other

        // What a set of rows cost: what is priced summed, and whether any of it is an estimate, any has no price.
        struct Money
        {
            int64_t microUsd = 0;
            bool estimate = false;               // a part of it is an estimate at the table's price
            bool unpriced = false;               // a part has no price known
            bool priced = false;                 // a part has one
            void add(const usage::Cost& c);
            void add(const Money& m);
        };
        // "$1.23", "~$1.23" (an estimate in it), "~$1.23+" (a part has no price known), "no price known" (none has
        // one).
        std::string moneyText(const Money& m);

        // A model's row in the table (a series of the chart when it is among the largest).
        struct Series
        {
            std::string provider, model;         // the ledger's names ("anthropic", "claude-opus-5-5")
            usage::Counts counts;
            Money money;
            int colour = -1;                     // its colour's index (colourOf), -1: Other's
        };

        // A bar: a day or a month, its values by the chart's series (chartNames' order: the largest first, Other last).
        struct Bucket
        {
            std::string day;                     // its first day, "2026-11-12"
            std::string label;                   // under the bar: "Wed", "12", "Nov"
            std::string longLabel;               // in the readout: "Wed 12 Nov", "November 2026"
            std::vector<int64_t> tokens;         // by series: tokens in and out
            std::vector<Money> money;            // by series
            int64_t requests = 0;
            int64_t totalTokens() const;
            Money totalMoney() const;
        };

        struct Model
        {
            usage::Range range = usage::Range::month;
            bool byMonth = false;
            std::vector<Series> rows;            // every model, the most tokens first (ties by name)
            std::vector<std::string> chartNames; // the chart's series: the largest models, then "Other" when any is left
            std::vector<int> chartColours;       // each one's colour (colourOf; -1 Other)
            std::vector<Bucket> buckets;         // oldest first, today's (this month's) last
            Series total;                        // the table's total row
            bool empty() const { return rows.empty(); }
        };

        // The page's view of a snapshot: its rows summed by model, bucketed by day (week, month) or month (year, all),
        // each row's cost at the table in use (usage::costOf).
        Model buildModel(const usage::Snapshot& snapshot, usage::Range range);

        // A round axis for values up to `max`: steps of 1, 2, 2.5 or 5 times a power of ten (at least `least`), about
        // four of them.
        struct Axis
        {
            double top = 0.0, step = 0.0;
            int steps = 0;
        };
        Axis niceAxis(double max, double least);

        // 950, 1.2K, 12K, 1.5M; a cost axis's "$0", "$0.25", "$5"; 12,345.
        std::string tokensText(int64_t n);
        std::string axisMoney(int64_t microUsd);
        std::string grouped(int64_t n);

        // What the readout says of a bucket ("Wed 12 Nov: 12K tokens; claude-opus-5-5 8.1K, Other 3.9K").
        std::string readoutText(const Model& m, int bucket, bool cost);

        // A series' colour: 0 the accent, then Theme.h's seriesInk (one set for both kinds, a value for each theme,
        // never an accent or the errors' colour); Other's (-1) a neutral ink (ChatStyle.h's dim).
        funkgui::Col colourOf(int index, const funkgui::Theme& th);
    }
}
