// Source/editor/UsagePage.h: the settings card's Usage page (SettingsPages.h: settings::kUsagePage), what the chat's
// requests used and cost, from the usage ledger (plugin/Usage.h) through ChatPort::usage, which never waits (the page
// shows what was last read; the chat's job thread reads the file). Counts and model names only: no word of a chat.
//
//   Range, Show   two selects: Last 7 days, Last 30 days, Last 12 months, All time; Tokens or Cost
//   the chart     a bar a day (a month in Last 12 months and All time), stacked by series: the largest by tokens in
//                 the range a series each (the product's accent first, then ChatStyle.h's seriesInk), the rest Other,
//                 the largest at the bottom; a legend over it, the y axis in round steps ("2M", "$5"). A custom control
//                 of the card's (SettingsCard.h, PageHooks): the page draws it, the card focuses, hits and tabs to it.
//                 The pointer over a bar shows that bar's rows in the readout under it; a click picks the bar under the
//                 click's own point; Left, Right, Home and End move along the bars while it has the focus, and a bar a
//                 key picked stays shown until the pointer next moves; with no bar shown the readout is the range's
//   the readout   a title line and a line a model (at most five, then "and N more"), each its tokens and its cost
//   BY MODEL      a row a model (provider, requests, tokens in, cached, out, cost, a bar of its share of the range's
//                 tokens), a total row. Fast mode is a row of its own ("claude-opus-5-5, fast"), priced at fast
//                 mode's prices: the ledger keeps it apart, and it costs several times its model's standard rate
//   the footnote  that ~ marks an estimate at list prices as of the price table's date (usage::pricesInUse, so a
//                 newer table offered through usage::offerPrices shows at the next read), OpenRouter's as reported,
//                 the provider's invoice the record; Reset... asks first in the page (Cancel and Reset in its place)
//
// The view's state (the range, the measure, the bar picked, Reset's question) lives in the card's pageState("usage")
// while the card lives. The page's key() holds the view's serial and that state, so the card draws again when a read
// lands; a read that finds nothing new moves nothing (ChatPort::usage).
#pragma once

#include "plugin/ChatPort.h"

#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Geometry.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace playground::ui
{
    class SettingsCard;
}

namespace playground::ui::usagepage
{
    inline constexpr funkgui::Tag kBarTag = funkgui::tags::firstProduct + 41;   // a bar's piece of a stack

    // The page's codes (SettingsCard::Act::item). Reset's question puts Cancel under kReset's code, so the focus that
    // was on Reset... is on Cancel.
    constexpr int kRange = 0, kMeasure = 1, kChart = 2, kReset = 3, kResetConfirm = 4;

    constexpr int kNamedSeries = 5;              // series a colour each; the rest are Other
    constexpr int kReadoutRows = 5;              // the readout's model lines, then "and N more"

    enum class Measure : uint8_t { tokens, cost };

    struct State
    {
        usage::Range range = usage::Range::week;
        Measure measure = Measure::tokens;
        int picked = -1;                         // the bar a click or a key picked (-1 none)
        uint64_t pickedAt = 0;                   // SettingsCard::pointerMoves() when it was picked
        bool asking = false;                     // Reset's question shows
    };
    State& stateOf(SettingsCard& card);

    // A model's tokens and cost summed over rows (a series, a bar's part, a table row).
    struct Sum
    {
        usage::Counts counts;
        int64_t microUsd = 0;                    // the rows priced
        bool estimate = false;                   // one of them is an estimate
        bool unpriced = false;                   // one of them has no price known
        int64_t tokens() const noexcept { return counts.tokensIn() + counts.output; }
        void add(const UsageRow& r);
        void add(const Sum& s);
    };

    struct Model                                 // a table row: one provider's model, fast or not
    {
        std::string provider, model;
        bool fast = false;
        std::string name;                        // "claude-opus-5-5", "claude-opus-5-5, fast"
        int series = 0;                          // its series (kNamedSeries: Other)
        Sum sum;
    };

    struct Bar
    {
        std::string first;                       // its first day ("2026-11-12"), or its month ("2026-11")
        std::string label;                       // under it ("Nov 12", "Nov"), "" where labels are thinned
        std::string title;                       // the readout's ("Thu, Nov 12, 2026", "November 2026")
        std::vector<Sum> bySeries;               // kNamedSeries + 1 (Other last)
        std::vector<Sum> byModel;                // the table's models, in the table's order
        Sum total;
    };

    struct Chart
    {
        bool read = false;
        std::vector<Model> models;               // the table, the most tokens first
        std::vector<std::string> seriesNames;    // the named series' names, then "Other" when any model is Other
        std::vector<Bar> bars;
        Sum total;
        std::string rangeTitle;                  // "Last 7 days"
        bool empty() const noexcept { return total.counts.requests == 0; }
    };
    Chart chartOf(const UsageView& view);

    // Where the chart's parts are in its control's rect.
    struct Geometry
    {
        funkgui::Rect legend;                    // the series' chips and names
        funkgui::Rect plot;                      // the bars' area, the y axis at its left
        std::vector<funkgui::Rect> slots;        // each bar's column (a pointer in it is over the bar)
        std::vector<funkgui::Rect> bars;         // each bar's stack, as tall as the plot (its pieces inside)
        double top = 1.0;                        // the plot's top value (tokens, or millionths of a dollar)
        double step = 1.0;                       // the y axis' step
    };
    Geometry geometryOf(const funkgui::Rect& control, const Chart& chart, Measure measure);
    int barAt(const Geometry& g, funkgui::Point p);                 // the bar whose column holds p (-1 none)
    // The bar the readout shows: the pointer's while it is over the chart and moved since a pick, else the one picked,
    // else none (-1: the range's own).
    int shownBar(const SettingsCard& card, const State& s, const Geometry& g);

    // A measure of a sum, and its words.
    double valueOf(const Sum& s, Measure m);
    std::string tokensText(int64_t tokens);                         // "850", "12.3K", "1.25M"
    std::string costWords(const Sum& s);                            // "$0.12", "~$0.12", "no price known"
    std::string axisText(double value, Measure m);                  // "2M", "$5", "$0.50"
    // A bar's words (its accessibility title): its title, tokens and cost.
    std::string barWords(const Bar& b);
    // The readout's lines for a bar (-1: the range's): its title and totals, then a line a model (its series' chip
    // before it), then "and N more". None while the chart is empty.
    struct ReadoutLine
    {
        std::string text;
        int series = -1;                         // the chip's series (-1: none)
    };
    std::vector<ReadoutLine> readout(const Chart& chart, int bar);
    std::string readoutWords(const Chart& chart, int bar);         // the lines in one, for a screen reader

    // Where the page's parts are, from the body's top (the build's y): the selects, the chart, the readout, the table.
    struct Layout
    {
        float chartY = 0, chartH = 0, readoutY = 0, readoutH = 0;
        float tableY = 0;                        // the BY MODEL heading's row (after its gap)
        float headerY = 0, firstRowY = 0, rowH = 0, totalY = 0, after = 0;
    };
    Layout layoutOf(const Chart& chart);
    // The table's columns at the body's width: their x and width, Model to Share.
    struct Column
    {
        float x = 0, w = 0;
    };
    enum Cell : int { cModel, cProvider, cRequests, cIn, cCached, cOut, cCost, cShare, cCount };
    std::vector<Column> columnsOf(float inner);
}
