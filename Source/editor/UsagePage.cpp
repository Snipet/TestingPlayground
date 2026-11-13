// Source/editor/UsagePage.cpp: the settings card's Usage page (UsagePage.h): its chart's model and geometry, its
// build (SettingsPages.h's Builder), its controls' acts, its keys, its drawing and its accessibility items (the card's
// PageHooks), and its row in the page table (settings::kUsagePage).
#include "editor/UsagePage.h"

#include "editor/ChatStyle.h"
#include "editor/Dropdown.h"
#include "editor/Items.h"
#include "editor/SettingsCard.h"
#include "editor/SettingsPages.h"
#include "editor/Theme.h"

#include <funkgui/canvas/Canvas.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <tuple>

namespace playground::ui::usagepage
{
    namespace
    {
        constexpr const char* kMonths[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
        constexpr const char* kMonthNames[] = { "January", "February", "March",     "April",   "May",      "June",
                                                "July",    "August",   "September", "October", "November", "December" };
        constexpr const char* kWeekdays[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
        constexpr const char* kRanges[] = { "Last 7 days", "Last 30 days", "Last 12 months", "All time" };
        constexpr const char* kMeasures[] = { "Tokens", "Cost" };

        constexpr float kChartH = 196.0f;
        constexpr float kReadoutLine = 16.0f;
        constexpr float kTableRow = 22.0f;
        constexpr float kInner = 556.0f;         // the body's width at the card's full size: the columns' plan

        int number(const std::string& s, std::size_t at, std::size_t n)
        {
            int v = 0;
            for (std::size_t i = at; i < at + n && i < s.size(); ++i)
                v = v * 10 + (s[i] - '0');
            return v;
        }

        // A month as a count (year * 12 + month - 1), and back.
        int monthIndex(const std::string& day) { return number(day, 0, 4) * 12 + number(day, 5, 2) - 1; }
        std::string monthOf(int index)
        {
            char b[16];
            std::snprintf(b, sizeof b, "%04d-%02d", index / 12, index % 12 + 1);
            return b;
        }

        // 3 significant digits, the trailing zeros dropped: 1.25, 12.5, 125, 1.2.
        std::string sig3(double v)
        {
            char b[32];
            std::snprintf(b, sizeof b, v < 10.0 ? "%.2f" : v < 100.0 ? "%.1f" : "%.0f", v);
            std::string s = b;
            if (s.find('.') != std::string::npos)
            {
                while (!s.empty() && s.back() == '0')
                    s.pop_back();
                if (!s.empty() && s.back() == '.')
                    s.pop_back();
            }
            return s;
        }

        uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001B3ull; }

        std::shared_ptr<const UsageView> viewOf(settings::Context& ctx, const State& s) { return ctx.port.chat().usage(s.range); }

        const SettingsCard::Control* chartControl(const SettingsCard::Places& at)
        {
            for (const SettingsCard::Control& c : at.controls)
                if (c.custom && c.act == SettingsCard::Act::item && c.index == kChart)
                    return &c;
            return nullptr;
        }

        std::string requestsText(int64_t n) { return std::to_string(n) + (n == 1 ? " request" : " requests"); }
    }

    // ---- the model ---------------------------------------------------------------------------------------------------
    State& stateOf(SettingsCard& card)
    {
        std::shared_ptr<void>& p = card.pageState("usage");
        if (!p)
            p = std::make_shared<State>();
        return *static_cast<State*>(p.get());
    }

    void Sum::add(const UsageRow& r)
    {
        counts += r.counts;
        switch (r.cost.kind)
        {
            case usage::Cost::Kind::none: unpriced = unpriced || r.counts.requests > 0; break;
            case usage::Cost::Kind::estimate:
                microUsd += r.cost.microUsd;
                estimate = true;
                priced = true;
                break;
            case usage::Cost::Kind::reported:
                microUsd += r.cost.microUsd;
                priced = true;
                break;
        }
    }

    void Sum::add(const Sum& s)
    {
        counts += s.counts;
        microUsd += s.microUsd;
        estimate = estimate || s.estimate;
        unpriced = unpriced || s.unpriced;
        priced = priced || s.priced;
    }

    Chart chartOf(const UsageView& view)
    {
        Chart chart;
        chart.read = view.read;
        chart.rangeTitle = kRanges[static_cast<int>(view.range)];
        const std::string today = view.today.empty() ? usage::today() : view.today;

        // The table: a row a provider's model, fast apart; the most tokens first.
        std::map<std::tuple<std::string, std::string, bool>, std::size_t> modelOf;
        for (const UsageRow& r : view.rows)
        {
            const auto key = std::make_tuple(r.key.provider, r.key.model, r.key.fast);
            auto it = modelOf.find(key);
            if (it == modelOf.end())
            {
                Model m;
                m.provider = r.key.provider;
                m.model = r.key.model;
                m.fast = r.key.fast;
                m.name = r.key.model + (r.key.fast ? ", fast" : "");
                it = modelOf.emplace(key, chart.models.size()).first;
                chart.models.push_back(std::move(m));
            }
            chart.models[it->second].sum.add(r);
            chart.total.add(r);
        }
        std::vector<std::size_t> order(chart.models.size());
        for (std::size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            const Model& x = chart.models[a];
            const Model& y = chart.models[b];
            if (x.sum.tokens() != y.sum.tokens())
                return x.sum.tokens() > y.sum.tokens();
            return std::tie(x.name, x.provider) < std::tie(y.name, y.provider);
        });
        {
            std::vector<Model> sorted;
            sorted.reserve(order.size());
            for (const std::size_t i : order)
                sorted.push_back(std::move(chart.models[i]));
            chart.models = std::move(sorted);
        }
        modelOf.clear();
        for (std::size_t i = 0; i < chart.models.size(); ++i)
        {
            Model& m = chart.models[i];
            m.series = std::min(static_cast<int>(i), kNamedSeries);
            modelOf[std::make_tuple(m.provider, m.model, m.fast)] = i;
            if (static_cast<int>(i) < kNamedSeries)
                chart.seriesNames.push_back(m.name);
        }
        if (static_cast<int>(chart.models.size()) > kNamedSeries)
            chart.seriesNames.push_back("Other");

        // The bars: a day each in the last 7 or 30 days, a month each in the last 12 months and all time.
        const bool byDay = view.range == usage::Range::week || view.range == usage::Range::month;
        const int64_t now = usage::dayNumber(today);
        const int thisMonth = monthIndex(today);
        int n = 0, firstMonth = thisMonth - 11;
        int64_t firstDay = 0;
        if (byDay)
        {
            n = view.range == usage::Range::week ? 7 : 30;
            firstDay = now - (n - 1);
        }
        else
        {
            if (view.range == usage::Range::all)
                for (const UsageRow& r : view.rows)
                    firstMonth = std::min(firstMonth, monthIndex(r.key.day));
            n = thisMonth - firstMonth + 1;
        }
        const int labelEvery = byDay ? (n <= 7 ? 1 : 5) : std::max(1, (n + 11) / 12);
        chart.bars.resize(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
        {
            Bar& b = chart.bars[static_cast<std::size_t>(i)];
            b.bySeries.resize(kNamedSeries + 1);
            b.byModel.resize(chart.models.size());
            const bool labelled = (n - 1 - i) % labelEvery == 0;
            if (byDay)
            {
                b.first = usage::dayOf(firstDay + i);
                const int m = number(b.first, 5, 2) - 1, d = number(b.first, 8, 2);
                const int weekday = static_cast<int>(((firstDay + i) % 7 + 11) % 7);   // 1970-01-01 was a Thursday
                char t[48];
                std::snprintf(t, sizeof t, "%s %d", kMonths[m], d);
                b.label = labelled ? t : "";
                std::snprintf(t, sizeof t, "%s, %s %d, %d", kWeekdays[weekday], kMonths[m], d, number(b.first, 0, 4));
                b.title = t;
            }
            else
            {
                const int month = firstMonth + i;
                b.first = monthOf(month);
                char t[48];
                if (month % 12 == 0 || i == 0)
                    std::snprintf(t, sizeof t, "%s %02d", kMonths[month % 12], (month / 12) % 100);
                else
                    std::snprintf(t, sizeof t, "%s", kMonths[month % 12]);
                b.label = labelled ? t : "";
                std::snprintf(t, sizeof t, "%s %d", kMonthNames[month % 12], month / 12);
                b.title = t;
            }
        }
        for (const UsageRow& r : view.rows)
        {
            int64_t i = -1;
            if (byDay)
            {
                if (usage::validDay(r.key.day))
                    i = usage::dayNumber(r.key.day) - firstDay;
            }
            else
                i = monthIndex(r.key.day) - firstMonth;
            if (i < 0 || i >= n)
                continue;                        // outside the range (a month folded from before it)
            Bar& b = chart.bars[static_cast<std::size_t>(i)];
            const std::size_t m = modelOf[std::make_tuple(r.key.provider, r.key.model, r.key.fast)];
            b.byModel[m].add(r);
            b.bySeries[static_cast<std::size_t>(chart.models[m].series)].add(r);
            b.total.add(r);
        }
        return chart;
    }

    // ---- geometry ----------------------------------------------------------------------------------------------------
    double valueOf(const Sum& s, Measure m)
    {
        return m == Measure::tokens ? static_cast<double>(s.tokens()) : static_cast<double>(s.microUsd);
    }

    Geometry geometryOf(const funkgui::Rect& r, const Chart& chart, Measure m)
    {
        Geometry g;
        g.legend = { r.x + 14.0f, r.y + 8.0f, r.w - 28.0f, 32.0f };
        const float left = r.x + 56.0f, top = r.y + 56.0f;
        g.plot = { left, top, r.right() - 14.0f - left, r.bottom() - 26.0f - top };
        double most = 0.0;
        for (const Bar& b : chart.bars)
            most = std::max(most, valueOf(b.total, m));
        if (most > 0.0)
        {
            // A round step: 1, 2 or 5 times a power of ten, about four of them to the top.
            const double raw = most / 4.0;
            const double mag = std::pow(10.0, std::floor(std::log10(raw)));
            g.step = mag * 10.0;
            for (const double f : { 1.0, 2.0, 5.0 })
                if (f * mag >= raw)
                {
                    g.step = f * mag;
                    break;
                }
            g.step = std::max(1.0, g.step);
            g.top = g.step * std::ceil(most / g.step - 1e-9);
        }
        else
        {
            g.step = 0.0;                        // nothing to scale: the frame and the baseline alone
            g.top = 0.0;
        }
        const std::size_t n = chart.bars.size();
        const float slot = n > 0 ? g.plot.w / static_cast<float>(n) : g.plot.w;
        const float barW = std::clamp(std::floor(slot * 0.66f), 1.0f, 30.0f);
        for (std::size_t i = 0; i < n; ++i)
        {
            const float x = g.plot.x + slot * static_cast<float>(i);
            g.slots.push_back({ x, g.plot.y, slot, g.plot.h });
            g.bars.push_back({ std::round(x + 0.5f * slot - 0.5f * barW), g.plot.y, barW, g.plot.h });
        }
        return g;
    }

    int barAt(const Geometry& g, funkgui::Point p)
    {
        for (std::size_t i = 0; i < g.slots.size(); ++i)
            if (g.slots[i].contains(p))
                return static_cast<int>(i);
        return -1;
    }

    int shownBar(const SettingsCard& card, const State& s, const Geometry& g)
    {
        const auto pointer = card.lastPointer();
        if (pointer && card.pointerMoves() != s.pickedAt)
            if (const int b = barAt(g, *pointer); b >= 0)
                return b;                        // the pointer's, once it moved since the pick
        return s.picked >= 0 && s.picked < static_cast<int>(g.slots.size()) ? s.picked : -1;
    }

    // ---- words -------------------------------------------------------------------------------------------------------
    std::string tokensText(int64_t t)
    {
        if (t < 1000)
            return std::to_string(std::max<int64_t>(0, t));
        const double v = static_cast<double>(t);
        if (t < 1000000)
            return sig3(v / 1e3) + "K";
        if (t < 1000000000)
            return sig3(v / 1e6) + "M";
        return sig3(v / 1e9) + "B";
    }

    std::string costWords(const Sum& s)
    {
        if (s.counts.requests == 0)
            return "$0";
        if (s.unpriced && !s.priced)
            return "no price known";
        usage::Cost c;
        c.kind = s.estimate ? usage::Cost::Kind::estimate : usage::Cost::Kind::reported;
        c.microUsd = s.microUsd;
        return usage::costText(c) + (s.unpriced ? " + no price for some" : "");
    }

    std::string axisText(double v, Measure m)
    {
        if (m == Measure::tokens)
            return v <= 0.0 ? std::string("0") : tokensText(static_cast<int64_t>(std::llround(v)));
        if (v <= 0.0)
            return "$0";
        const double dollars = v / 1e6;
        char b[32];
        if (dollars >= 1.0 && std::abs(dollars - std::round(dollars)) < 1e-9)
            std::snprintf(b, sizeof b, "$%.0f", dollars);
        else if (dollars >= 0.01)
            std::snprintf(b, sizeof b, "$%.2f", dollars);
        else
        {
            std::snprintf(b, sizeof b, "$%.6f", dollars);   // the digits a small step needs, no more
            std::string s = b;
            while (s.back() == '0')
                s.pop_back();
            return s;
        }
        return b;
    }

    std::string barWords(const Bar& b)
    {
        if (b.total.counts.requests == 0)
            return b.title + ": no requests";
        return b.title + ": " + tokensText(b.total.tokens()) + " tokens, " + costWords(b.total) + ", "
             + requestsText(b.total.counts.requests);
    }

    std::vector<ReadoutLine> readout(const Chart& chart, int bar)
    {
        std::vector<ReadoutLine> out;
        if (chart.empty())
            return out;
        const bool one = bar >= 0 && bar < static_cast<int>(chart.bars.size());
        const Sum& total = one ? chart.bars[static_cast<std::size_t>(bar)].total : chart.total;
        const std::string title = one ? chart.bars[static_cast<std::size_t>(bar)].title : chart.rangeTitle;
        if (total.counts.requests == 0)
        {
            out.push_back({ title + ": no requests", -1 });
            return out;
        }
        out.push_back({ title + ": " + tokensText(total.tokens()) + " tokens, " + costWords(total) + ", "
                            + requestsText(total.counts.requests),
                        -1 });
        int shown = 0;
        Sum rest;
        int more = 0;
        for (std::size_t m = 0; m < chart.models.size(); ++m)
        {
            const Sum& s = one ? chart.bars[static_cast<std::size_t>(bar)].byModel[m] : chart.models[m].sum;
            if (s.counts.requests == 0)
                continue;
            if (shown < kReadoutRows)
            {
                const usage::Counts& c = s.counts;
                out.push_back({ chart.models[m].name + ": " + tokensText(s.tokens()) + " tokens (" + tokensText(c.input + c.cacheWrite5m + c.cacheWrite1h)
                                    + " in, " + tokensText(c.cacheRead) + " cached, " + tokensText(c.output) + " out), " + costWords(s),
                                chart.models[m].series });
                ++shown;
            }
            else
            {
                rest.add(s);
                ++more;
            }
        }
        if (more > 0)
            out.push_back({ "and " + std::to_string(more) + (more == 1 ? " more model: " : " more models: ") + tokensText(rest.tokens())
                                + " tokens, " + costWords(rest),
                            kNamedSeries });
        return out;
    }

    std::string readoutWords(const Chart& chart, int bar)
    {
        std::string s;
        for (const ReadoutLine& l : readout(chart, bar))
            s += (s.empty() ? "" : "; ") + l.text;
        return s;
    }

    // ---- layout ------------------------------------------------------------------------------------------------------
    Layout layoutOf(const Chart& chart)
    {
        Layout l;
        l.chartY = settings::kRowStep + 4.0f;
        l.chartH = kChartH;
        l.readoutY = l.chartY + l.chartH + 10.0f;
        l.readoutH = chart.empty() ? 0.0f : kReadoutLine * static_cast<float>(kReadoutRows + 2);   // none to read out
        l.tableY = l.readoutY + l.readoutH + settings::kSectionGap;      // the heading's (its gap before it)
        l.headerY = l.tableY + settings::kHeadingH;
        l.rowH = kTableRow;
        l.firstRowY = l.headerY + kTableRow;
        l.totalY = l.firstRowY + kTableRow * static_cast<float>(chart.models.size()) + 4.0f;
        l.after = chart.empty() ? l.tableY : l.totalY + kTableRow + 6.0f;
        return l;
    }

    std::vector<Column> columnsOf(float inner)
    {
        // At the card's full width; scaled to a narrower body.
        constexpr std::array<float, cCount> kW{ 150.0f, 74.0f, 60.0f, 52.0f, 56.0f, 52.0f, 62.0f, 50.0f };
        const float k = inner / kInner;
        std::vector<Column> out;
        float x = 0.0f;
        for (const float w : kW)
        {
            out.push_back({ x, w * k - 6.0f });
            x += w * k;
        }
        return out;
    }
}

// ---- the page ------------------------------------------------------------------------------------------------------------
namespace playground::ui::settings
{
    namespace
    {
        using namespace usagepage;

        std::string providerWords(const std::string& provider)
        {
            if (provider == "anthropic")
                return "Anthropic";
            if (provider == "openrouter")
                return "OpenRouter";
            if (provider == "openai")
                return "OpenAI";
            if (provider == "gemini")
                return "Gemini";
            if (provider == "local")
                return "Local";
            return "Other";
        }

        // A select's menu (SettingsPages.cpp's, for this page): the choices, the current one ticked.
        void choose(const Control& c, Context& ctx, const std::vector<std::string>& labels, int current, std::function<void(int)> pick)
        {
            funkgui::HostServices* host = ctx.host;
            if (host == nullptr)
                return;
            funkgui::MenuRequest request;
            for (std::size_t i = 0; i < labels.size(); ++i)
                request.items.push_back(dropdown::choice(static_cast<int>(i) + 1, labels[i], static_cast<int>(i) == current, true));
            dropdown::dropdownMenu(request, c.r, current + 1);
            request.theme = productTheme(funkgui::Theme::byIndex(host->themeIndex()));
            SettingsCard* card = &ctx.card;
            const int n = static_cast<int>(labels.size());
            host->showMenu(request, [card, n, then = std::move(pick)](int chosen) {
                if (!card->isOpen() || chosen < 1 || chosen > n)
                    return;
                then(chosen - 1);
                card->changed();
            });
        }

        bool keyed(const Control& c, const funkgui::KeyEvent& e, Context& ctx);
        void draw(const SettingsCard::Drawing& d, Context& ctx);
        void a11y(const Control& c, const SettingsCard::Places& at, std::vector<funkgui::A11yItem>& out, uint32_t firstId, Context& ctx);

        void build(Builder& b)
        {
            State& s = stateOf(b.ctx.card);
            const std::shared_ptr<const UsageView> view = viewOf(b.ctx, s);
            const Chart chart = chartOf(*view);
            const Layout l = layoutOf(chart);
            const int range = static_cast<int>(s.range), measure = static_cast<int>(s.measure);
            b.select(kRange, { 0.0f, b.y, 150.0f, kRowH }, kRanges[range], std::string("Range: ") + kRanges[range]);
            b.select(kMeasure, { 160.0f, b.y, 110.0f, kRowH }, kMeasures[measure], std::string("Show: ") + kMeasures[measure]);

            // The chart: the page draws it (PageHooks::draw), the card focuses and hits it.
            const std::string what = !chart.read ? std::string("reading") : chart.empty() ? std::string("No requests yet.") : barWords(Bar{ "", "", chart.rangeTitle, {}, {}, chart.total });
            Control& c = b.control(Act::item, kChart, { 0.0f, l.chartY, b.inner, l.chartH }, "",
                                   "Usage chart, " + std::string(kMeasures[measure]) + ": " + what
                                       + ". Left and Right move along the bars, Home and End to the first and the last.");
            c.custom = true;
            b.y = l.tableY;
            if (chart.read && !chart.empty())
            {
                b.y = l.tableY - kSectionGap;    // heading() adds its gap
                b.heading("BY MODEL");
                const std::vector<Column> col = columnsOf(b.inner);
                const auto cell = [&](Cell k, float y, std::string text, Line::Kind kind) {
                    b.line(kind, std::move(text), { col[static_cast<std::size_t>(k)].x, y, col[static_cast<std::size_t>(k)].w, kTableRow });
                };
                const char* const heads[] = { "Model", "Provider", "Requests", "In", "Cached", "Out", "Cost", "Share" };
                for (int k = 0; k < cCount; ++k)
                    cell(static_cast<Cell>(k), l.headerY, heads[k], Line::Kind::label);
                const double all = std::max<double>(1.0, static_cast<double>(chart.total.tokens()));
                const auto row = [&](float y, const std::string& name, const std::string& provider, const Sum& sum) {
                    const usage::Counts& n = sum.counts;
                    cell(cModel, y, name, Line::Kind::value);
                    cell(cProvider, y, provider, Line::Kind::value);
                    cell(cRequests, y, std::to_string(n.requests), Line::Kind::value);
                    cell(cIn, y, tokensText(n.input + n.cacheWrite5m + n.cacheWrite1h), Line::Kind::value);
                    cell(cCached, y, tokensText(n.cacheRead), Line::Kind::value);
                    cell(cOut, y, tokensText(n.output), Line::Kind::value);
                    cell(cCost, y, sum.unpriced ? std::string("no price") : costWords(sum), sum.unpriced ? Line::Kind::note : Line::Kind::value);
                    char pct[16];
                    std::snprintf(pct, sizeof pct, "%.0f%%", 100.0 * static_cast<double>(sum.tokens()) / all);
                    const Column& sc = col[cShare];
                    b.line(Line::Kind::note, pct, { sc.x + 0.5f * sc.w + 2.0f, y, 0.5f * sc.w - 2.0f, kTableRow });
                };
                for (std::size_t i = 0; i < chart.models.size(); ++i)
                    row(l.firstRowY + kTableRow * static_cast<float>(i), chart.models[i].name, providerWords(chart.models[i].provider),
                        chart.models[i].sum);
                Sum priced = chart.total;
                priced.unpriced = false;         // the total is of the rows priced, said below when some are not
                row(l.totalY, "Total", "", priced);
                b.y = l.after;
                if (chart.total.unpriced)
                    b.notes("A model with no price known shows its tokens alone, and is left out of the cost total.", 0.0f, b.inner);
            }
            else
                b.y = l.tableY;

            // The footnote, from the price table in use (usage::pricesInUse, read with the view).
            b.y += 4.0f;
            const std::string asOf = view->pricesAsOf.empty() ? usage::pricesInUse().asOf : view->pricesAsOf;
            b.notes("Costs marked ~ are estimates at list prices as of " + asOf
                        + " (Claude Haiku 5.5's at its rate for prompts up to 100K tokens). OpenRouter's are as reported. "
                          "Your provider's invoice is the record.",
                    0.0f, b.inner);
            b.y += 8.0f;
            if (s.asking)
            {
                b.notes(std::string("Reset the usage record? Every product's counts on ") + kThisMachine
                            + " go, from every provider. Your provider's own record does not change.",
                        0.0f, b.inner, Line::Kind::value);
                b.y += 6.0f;
                const std::string cancel = "Cancel", reset = "Reset";
                const float cw = Builder::pillW(cancel, false, false);
                b.button(kReset, { 0.0f, b.y, cw, kRowH }, cancel, "Cancel: keep the usage record");
                b.button(kResetConfirm, { cw + 10.0f, b.y, Builder::pillW(reset, false, false), kRowH }, reset,
                         "Reset: empty the usage record");
                b.y += kRowStep;
            }
            else
            {
                const std::string word = view->resetting ? "Resetting..." : "Reset...";
                b.button(kReset, { 0.0f, b.y, Builder::pillW(word, false, false), kRowH }, word,
                         "Reset: empty the usage record (asked first)", !view->resetting);
                b.y += kRowStep;
            }
            if (!view->resetError.empty())
                b.notes(view->resetError, 0.0f, b.inner, Line::Kind::error);
            b.hooks({ draw, keyed, a11y });
        }

        void act(const Control& c, Context& ctx)
        {
            if (c.act != Act::item)
                return;
            State& s = stateOf(ctx.card);
            switch (c.index)
            {
                case kRange:
                    choose(c, ctx, { kRanges[0], kRanges[1], kRanges[2], kRanges[3] }, static_cast<int>(s.range), [card = &ctx.card](int i) {
                        State& st = stateOf(*card);
                        st.range = static_cast<usage::Range>(i);
                        st.picked = -1;
                    });
                    break;
                case kMeasure:
                    choose(c, ctx, { kMeasures[0], kMeasures[1] }, static_cast<int>(s.measure),
                           [card = &ctx.card](int i) { stateOf(*card).measure = static_cast<Measure>(i); });
                    break;
                case kChart:
                    if (c.press)                 // a pointer's press: the bar under its own point; a key's: none
                    {
                        const Chart chart = chartOf(*viewOf(ctx, s));
                        s.picked = barAt(geometryOf(c.r, chart, s.measure), *c.press);
                        s.pickedAt = ctx.card.pointerMoves();
                    }
                    break;
                case kReset: s.asking = !s.asking; break;     // Reset..., or the question's Cancel
                case kResetConfirm:
                    s.asking = false;
                    ctx.port.chat().resetUsage();
                    break;
                default: break;
            }
        }

        uint64_t key(const Context& ctx)
        {
            Context& c = const_cast<Context&>(ctx);
            const State& s = stateOf(c.card);
            const std::shared_ptr<const UsageView> view = viewOf(c, s);   // asks for a read when one is due
            uint64_t h = mix(0x9E3779B97F4A7C15ull, view->serial);
            h = mix(h, static_cast<uint64_t>(s.range));
            h = mix(h, static_cast<uint64_t>(s.measure));
            h = mix(h, static_cast<uint64_t>(static_cast<int64_t>(s.picked) + 1));
            h = mix(h, s.pickedAt);
            return mix(h, s.asking ? 1u : 0u);
        }

        bool keyed(const Control& c, const funkgui::KeyEvent& e, Context& ctx)
        {
            if (c.index != kChart)
                return false;
            using funkgui::Key;
            if (e.key != Key::left && e.key != Key::right && e.key != Key::home && e.key != Key::end)
                return false;
            State& s = stateOf(ctx.card);
            const Chart chart = chartOf(*viewOf(ctx, s));
            const Geometry g = geometryOf(c.r, chart, s.measure);
            const int n = static_cast<int>(g.slots.size());
            if (n == 0)
                return false;
            const int now = shownBar(ctx.card, s, g);
            int next = now;
            if (e.key == Key::home)
                next = 0;
            else if (e.key == Key::end)
                next = n - 1;
            else if (e.key == Key::left)
                next = now < 0 ? n - 1 : std::max(0, now - 1);
            else
                next = now < 0 ? 0 : std::min(n - 1, now + 1);
            s.picked = next;
            s.pickedAt = ctx.card.pointerMoves();   // shown until the pointer next moves
            return true;
        }

        void draw(const SettingsCard::Drawing& d, Context& ctx)
        {
            const SettingsCard::Control* control = chartControl(d.at);
            if (control == nullptr)
                return;
            funkgui::Canvas& c = d.c;
            const chat::Palette p = chat::palette(d.th);
            const State& s = stateOf(ctx.card);
            const std::shared_ptr<const UsageView> view = viewOf(ctx, s);
            const Chart chart = chartOf(*view);
            const Geometry g = geometryOf(control->r, chart, s.measure);
            const funkgui::Rect& r = control->r;
            const int index = static_cast<int>(control - d.at.controls.data());
            const bool focused = d.focus == index;

            // The frame, its legend, the axis.
            c.rrect(r.x, r.y, r.w, r.h, 12.0f, p.raised, 1.0f, p.cardEdge);
            if (focused)
                c.rrect(r.x - 2.0f, r.y - 2.0f, r.w + 4.0f, r.h + 4.0f, 14.0f, p.card.withAlpha(0.0f), 1.5f, p.accent);
            {
                // Every series named, on two lines: a name cut to what is left of the second rather than left out.
                float x = g.legend.x, mid = g.legend.y + 8.0f;
                for (std::size_t i = 0; i < chart.seriesNames.size(); ++i)
                {
                    const int k = std::min(static_cast<int>(i), kNamedSeries);   // Other's index is kNamedSeries
                    std::string name = fitText(c, chart.seriesNames[i], chat::kSmall, 200.0f);
                    float w = 14.0f + c.textWidth(name.c_str(), chat::kSmall);
                    if (x + w > g.legend.right() && x > g.legend.x && mid < g.legend.y + 16.0f)
                    {
                        x = g.legend.x;                  // the second line
                        mid += 16.0f;
                    }
                    if (x + w > g.legend.right())
                    {
                        name = fitText(c, name, chat::kSmall, std::max(10.0f, g.legend.right() - x - 14.0f));
                        w = 14.0f + c.textWidth(name.c_str(), chat::kSmall);
                    }
                    c.rrect(x, mid - 4.0f, 8.0f, 8.0f, 2.0f, chat::seriesInk(d.th, k));
                    c.text(name.c_str(), x + 13.0f, c.capCentreTop(mid, chat::kSmall), chat::kSmall, p.soft);
                    x += w + 16.0f;
                }
            }
            const float base = g.plot.bottom();
            if (g.top > 0.0)
            {
                for (double v = 0.0; v <= g.top + 0.5 * g.step; v += g.step)
                {
                    const float y = std::round(base - static_cast<float>(v / g.top) * g.plot.h);
                    if (v > 0.0)
                        c.hairlineH(g.plot.x, y, g.plot.w, p.cardEdge);
                    c.text(axisText(v, s.measure).c_str(), g.plot.x - 8.0f, c.capCentreTop(static_cast<float>(y), chat::kSmall), chat::kSmall,
                           p.faint, funkgui::Align::right);
                }
            }
            c.hairlineH(g.plot.x, base, g.plot.w, p.composerEdge);
            if (!chart.read || chart.empty())
            {
                const char* words = !chart.read ? "Reading..." : "No requests yet.";
                c.text(words, g.plot.centreX(), c.capCentreTop(g.plot.centreY(), chat::kSmall), chat::kSmall, p.dim, funkgui::Align::centre);
            }

            // The bar shown, its column lit; the bars, each a stack, the largest series at the bottom.
            const int shown = chart.empty() ? -1 : shownBar(ctx.card, s, g);
            if (shown >= 0)
            {
                const funkgui::Rect& sl = g.slots[static_cast<std::size_t>(shown)];
                c.rrect(sl.x + 1.0f, sl.y - 4.0f, sl.w - 2.0f, sl.h + 4.0f, 4.0f, p.hover);
            }
            if (g.top > 0.0)
            {
                const funkgui::Canvas::Scope tag(c, kBarTag, false);
                for (std::size_t i = 0; i < chart.bars.size(); ++i)
                {
                    const funkgui::Rect& br = g.bars[i];
                    float y = base;
                    for (int k = 0; k <= kNamedSeries; ++k)
                    {
                        const double v = valueOf(chart.bars[i].bySeries[static_cast<std::size_t>(k)], s.measure);
                        if (v <= 0.0)
                            continue;
                        const float h = std::max(1.0f, static_cast<float>(v / g.top) * g.plot.h);
                        const float gap = y < base && h >= 3.0f ? 1.0f : 0.0f;   // apart from the piece under it
                        c.rrect(br.x, y - h, br.w, h - gap, std::min(2.0f, 0.5f * br.w), chat::seriesInk(d.th, k));
                        y -= h;
                    }
                }
            }
            if (focused && shown >= 0)
            {
                const funkgui::Rect& sl = g.slots[static_cast<std::size_t>(shown)];
                c.rrect(sl.x + 1.0f, sl.y - 4.0f, sl.w - 2.0f, sl.h + 4.0f, 4.0f, p.card.withAlpha(0.0f), 1.5f, p.accent);
            }
            for (std::size_t i = 0; i < chart.bars.size(); ++i)
                if (!chart.bars[i].label.empty())
                {
                    const funkgui::Rect& sl = g.slots[i];
                    c.text(chart.bars[i].label.c_str(), sl.centreX(), c.capCentreTop(base + 13.0f, chat::kSmall), chat::kSmall,
                           static_cast<int>(i) == shown ? p.text : p.faint, funkgui::Align::centre);
                }

            // The readout under it.
            const Layout l = layoutOf(chart);
            float y = r.y + (l.readoutY - l.chartY);
            for (const ReadoutLine& line : readout(chart, shown))
            {
                float x = r.x;
                if (line.series >= 0)
                {
                    c.rrect(x + 2.0f, y + 4.0f, 8.0f, 8.0f, 2.0f, chat::seriesInk(d.th, line.series));
                    x += 16.0f;
                }
                const bool head = line.series < 0;
                c.text(fitText(c, line.text, head ? chat::kSmallStrong : chat::kSmall, r.right() - x).c_str(), x,
                       c.capCentreTop(y + 8.0f, chat::kSmall), head ? chat::kSmallStrong : chat::kSmall, head ? p.text : p.soft);
                y += 16.0f;
            }

            // The table's share bars, in their models' series' colours.
            if (chart.read && !chart.empty())
            {
                const std::vector<Column> col = columnsOf(r.w);
                const Column& sc = col[cShare];
                const double all = std::max<double>(1.0, static_cast<double>(chart.total.tokens()));
                const float top = r.y - l.chartY;
                for (std::size_t i = 0; i < chart.models.size(); ++i)
                {
                    const float ry = top + l.firstRowY + 22.0f * static_cast<float>(i) + 0.5f * 22.0f - 3.0f;
                    const float w = 0.5f * sc.w;
                    c.rrect(r.x + sc.x, ry, w, 6.0f, 3.0f, p.hover);
                    const float f = static_cast<float>(static_cast<double>(chart.models[i].sum.tokens()) / all);
                    if (f > 0.0f)
                        c.rrect(r.x + sc.x, ry, std::max(2.0f, w * f), 6.0f, 3.0f, chat::seriesInk(d.th, chart.models[i].series));
                }
                c.hairlineH(r.x, std::round(top + l.totalY - 2.0f), r.w, p.cardEdge);
            }
        }

        bool meet(const funkgui::Rect& a, const funkgui::Rect& b)
        {
            return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
        }

        void a11y(const Control& c, const SettingsCard::Places& at, std::vector<funkgui::A11yItem>& out, uint32_t firstId, Context& ctx)
        {
            if (c.index != kChart)
                return;
            const State& s = stateOf(ctx.card);
            const Chart chart = chartOf(*viewOf(ctx, s));
            const Geometry g = geometryOf(c.r, chart, s.measure);
            uint32_t id = firstId;
            for (std::size_t i = 0; i < chart.bars.size(); ++i)
            {
                funkgui::A11yItem it;
                it.id = id++;
                it.parent = c.id;
                it.role = funkgui::A11yRole::staticText;
                it.bounds = g.slots[i];
                it.title = barWords(chart.bars[i]);
                it.readOnly = true;
                it.visible = meet(at.body, g.slots[i]);
                out.push_back(std::move(it));
            }
            const Layout l = layoutOf(chart);
            funkgui::A11yItem it;
            it.id = id++;
            it.parent = c.id;
            it.role = funkgui::A11yRole::staticText;
            it.bounds = { c.r.x, c.r.y + (l.readoutY - l.chartY), c.r.w, l.readoutH };
            it.title = chart.empty() ? std::string(chart.read ? "No requests yet." : "Reading...")
                                     : readoutWords(chart, shownBar(ctx.card, s, g));
            it.readOnly = true;
            it.visible = meet(at.body, it.bounds);
            out.push_back(std::move(it));
        }
    }

    const Page kUsagePage{ "usage", "Usage", "What the chat's requests used, and what they cost.", nullptr, build, act, key };
}
