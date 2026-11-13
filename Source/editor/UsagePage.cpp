// Source/editor/UsagePage.cpp: the settings card's Usage page (UsagePage.h). Its snapshot comes from ChatPort::usage,
// read on the chat's job thread; the page builds its model from it only when it changes, so a frame does no more than
// lay out and draw.
#include "editor/UsagePage.h"

#include "editor/ChatStyle.h"
#include "editor/Dropdown.h"
#include "editor/Items.h"
#include "editor/SettingsPages.h"
#include "editor/Theme.h"
#include "plugin/ChatPort.h"

#include "Product.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <utility>

namespace playground::ui::usagepage
{
    // ---- the model --------------------------------------------------------------------------------------------------------
    void Money::add(const usage::Cost& c)
    {
        if (c.kind == usage::Cost::Kind::none)
        {
            unpriced = true;
            return;
        }
        priced = true;
        microUsd += c.microUsd;
        estimate = estimate || c.kind == usage::Cost::Kind::estimate;
    }

    void Money::add(const Money& m)
    {
        microUsd += m.microUsd;
        estimate = estimate || m.estimate;
        unpriced = unpriced || m.unpriced;
        priced = priced || m.priced;
    }

    std::string moneyText(const Money& m)
    {
        if (!m.priced)
            return m.unpriced ? "no price known" : "$0.00";
        usage::Cost c;
        c.kind = m.estimate || m.unpriced ? usage::Cost::Kind::estimate : usage::Cost::Kind::reported;
        c.microUsd = m.microUsd;
        return usage::costText(c) + (m.unpriced ? "+" : "");
    }

    int64_t Bucket::totalTokens() const
    {
        int64_t t = 0;
        for (const int64_t v : tokens)
            t += v;
        return t;
    }

    Money Bucket::totalMoney() const
    {
        Money t;
        for (const Money& v : money)
            t.add(v);
        return t;
    }

    namespace
    {
        constexpr const char* kWeekdays[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
        constexpr const char* kMonths[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
        constexpr const char* kMonthNames[] = { "January", "February", "March", "April", "May", "June",
                                                "July", "August", "September", "October", "November", "December" };

        int numberAt(const std::string& s, std::size_t at, std::size_t n)
        {
            int v = 0;
            for (std::size_t i = at; i < at + n && i < s.size(); ++i)
                v = v * 10 + (s[i] - '0');
            return v;
        }

        // A month as a number (year * 12 + month - 1), from "YYYY-MM" or "YYYY-MM-DD".
        int monthIndex(const std::string& day) { return numberAt(day, 0, 4) * 12 + numberAt(day, 5, 2) - 1; }

        std::string monthDay(int index)
        {
            char b[16];
            std::snprintf(b, sizeof b, "%04d-%02d-01", index / 12, index % 12 + 1);
            return b;
        }

        int64_t tokensOf(const usage::Counts& c) { return c.tokensIn() + c.output; }

        bool sameSeries(const Series& s, const usage::Key& k) { return s.provider == k.provider && s.model == k.model; }
    }

    Model buildModel(const usage::Snapshot& snapshot, usage::Range range)
    {
        Model m;
        m.range = range;
        m.byMonth = range == usage::Range::year || range == usage::Range::all;
        const std::string today = usage::validDay(snapshot.today) ? snapshot.today : usage::today();

        // The rows by model (provider and model; fast mode's requests with the model's own), each row priced.
        std::vector<usage::Cost> costs;
        costs.reserve(snapshot.rows.size());
        for (const usage::Row& r : snapshot.rows)
        {
            const usage::Cost cost = usage::costOf(r.key, r.counts);
            costs.push_back(cost);
            auto it = std::find_if(m.rows.begin(), m.rows.end(), [&](const Series& s) { return sameSeries(s, r.key); });
            if (it == m.rows.end())
            {
                Series s;
                s.provider = r.key.provider;
                s.model = r.key.model;
                m.rows.push_back(std::move(s));
                it = m.rows.end() - 1;
            }
            it->counts += r.counts;
            it->money.add(cost);
            m.total.counts += r.counts;
            m.total.money.add(cost);
        }
        std::stable_sort(m.rows.begin(), m.rows.end(), [](const Series& a, const Series& b) {
            const int64_t ta = tokensOf(a.counts), tb = tokensOf(b.counts);
            if (ta != tb)
                return ta > tb;
            return a.model != b.model ? a.model < b.model : a.provider < b.provider;
        });
        const int coloured = std::min<int>(kMaxSeries, static_cast<int>(m.rows.size()));
        for (int i = 0; i < coloured; ++i)
        {
            m.rows[static_cast<std::size_t>(i)].colour = i;
            m.chartNames.push_back(m.rows[static_cast<std::size_t>(i)].model);
            m.chartColours.push_back(i);
        }
        const bool other = static_cast<int>(m.rows.size()) > coloured;
        if (other)
        {
            m.chartNames.push_back("Other");
            m.chartColours.push_back(-1);
        }

        // The buckets, oldest first.
        const int64_t now = usage::dayNumber(today);
        int64_t firstDay = 0;
        int firstMonth = 0, months = 0;
        int days = 0;
        if (range == usage::Range::week || range == usage::Range::month)
        {
            days = range == usage::Range::week ? 7 : 30;
            firstDay = now - days + 1;
        }
        else
        {
            const int thisMonth = monthIndex(today);
            firstMonth = thisMonth - 11;
            if (range == usage::Range::all)
                for (const usage::Row& r : snapshot.rows)
                    firstMonth = std::min(firstMonth, monthIndex(r.key.day));
            months = thisMonth - firstMonth + 1;
        }
        const std::size_t width = m.chartNames.size();
        const int n = m.byMonth ? months : days;
        m.buckets.resize(static_cast<std::size_t>(std::max(0, n)));
        for (int i = 0; i < n; ++i)
        {
            Bucket& b = m.buckets[static_cast<std::size_t>(i)];
            b.tokens.assign(width, 0);
            b.money.assign(width, Money{});
            if (m.byMonth)
            {
                const int idx = firstMonth + i;
                b.day = monthDay(idx);
                b.label = kMonths[idx % 12];
                b.longLabel = std::string(kMonthNames[idx % 12]) + " " + std::to_string(idx / 12);
            }
            else
            {
                const int64_t d = firstDay + i;
                b.day = usage::dayOf(d);
                const int weekday = static_cast<int>(((d + 3) % 7 + 7) % 7);   // day 0, 1970-01-01, was a Thursday
                const int dom = numberAt(b.day, 8, 2);
                b.label = range == usage::Range::week ? std::string(kWeekdays[weekday]) : std::to_string(dom);
                b.longLabel = std::string(kWeekdays[weekday]) + " " + std::to_string(dom) + " " + kMonths[numberAt(b.day, 5, 2) - 1];
            }
        }
        for (std::size_t i = 0; i < snapshot.rows.size(); ++i)
        {
            const usage::Row& r = snapshot.rows[i];
            const int64_t at = m.byMonth ? monthIndex(r.key.day) - firstMonth
                                         : (r.key.day.size() == 10 ? usage::dayNumber(r.key.day) - firstDay : -1);
            if (at < 0 || at >= n)
                continue;
            std::size_t s = width - 1;           // Other, unless it is one of the largest
            for (std::size_t k = 0; k < static_cast<std::size_t>(coloured); ++k)
                if (sameSeries(m.rows[k], r.key))
                    s = k;
            Bucket& b = m.buckets[static_cast<std::size_t>(at)];
            b.tokens[s] += tokensOf(r.counts);
            b.money[s].add(costs[i]);
            b.requests += r.counts.requests;
        }
        return m;
    }

    Axis niceAxis(double max, double least)
    {
        Axis a;
        const double rough = std::max(max / 4.0, least);
        const double p = std::pow(10.0, std::floor(std::log10(rough)));
        double step = 10.0 * p;
        for (const double f : { 1.0, 2.0, 2.5, 5.0, 10.0 })
            if (f * p >= rough - 1e-9 * rough)
            {
                step = f * p;
                break;
            }
        step = std::max(step, least);
        a.step = step;
        a.steps = std::max(1, static_cast<int>(std::ceil(max / step - 1e-9)));
        a.top = a.step * a.steps;
        return a;
    }

    std::string tokensText(int64_t n)
    {
        char b[32];
        if (n < 1000)
        {
            std::snprintf(b, sizeof b, "%lld", static_cast<long long>(n));
            return b;
        }
        const char* unit = "K";
        double v = static_cast<double>(n) / 1e3;
        if (n >= 1000000000)
        {
            unit = "B";
            v = static_cast<double>(n) / 1e9;
        }
        else if (n >= 1000000)
        {
            unit = "M";
            v = static_cast<double>(n) / 1e6;
        }
        if (v < 10.0 && std::abs(v - std::round(v)) >= 0.05)
            std::snprintf(b, sizeof b, "%.1f%s", v, unit);
        else
            std::snprintf(b, sizeof b, "%.0f%s", v, unit);
        return b;
    }

    std::string axisMoney(int64_t microUsd)
    {
        char b[32];
        const double usd = static_cast<double>(microUsd) / 1e6;
        if (microUsd % 1000000 == 0)
            std::snprintf(b, sizeof b, "$%.0f", usd);
        else if (microUsd % 10000 == 0)
            std::snprintf(b, sizeof b, "$%.2f", usd);
        else
            std::snprintf(b, sizeof b, "$%.3f", usd);
        return b;
    }

    std::string grouped(int64_t n)
    {
        std::string s = std::to_string(n);
        for (int i = static_cast<int>(s.size()) - 3; i > (s[0] == '-' ? 1 : 0); i -= 3)
            s.insert(static_cast<std::size_t>(i), ",");
        return s;
    }

    std::string readoutText(const Model& m, int bucket, bool cost)
    {
        if (bucket < 0 || bucket >= static_cast<int>(m.buckets.size()))
            return {};
        const Bucket& b = m.buckets[static_cast<std::size_t>(bucket)];
        if (b.requests == 0)
            return b.longLabel + ": no requests";
        std::string out = b.longLabel + ": " + (cost ? moneyText(b.totalMoney()) : tokensText(b.totalTokens()) + " tokens");
        out += b.requests == 1 ? ", 1 request" : ", " + grouped(b.requests) + " requests";
        // Its series, the most first.
        std::vector<std::size_t> order;
        for (std::size_t s = 0; s < m.chartNames.size(); ++s)
            if (b.tokens[s] > 0 || b.money[s].priced || b.money[s].unpriced)
                order.push_back(s);
        std::stable_sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) {
            return cost ? b.money[x].microUsd > b.money[y].microUsd : b.tokens[x] > b.tokens[y];
        });
        std::string parts;
        for (const std::size_t s : order)
            parts += (parts.empty() ? "" : ", ") + m.chartNames[s] + " "
                   + (cost ? moneyText(b.money[s]) : tokensText(b.tokens[s]));
        if (!parts.empty())
            out += "; " + parts;
        return out;
    }

    funkgui::Col colourOf(int index, const funkgui::Theme& th)
    {
        const chat::Palette p = chat::palette(th);
        if (index < 0)
            return p.dim;
        if (index == 0)
            return p.accent;
        // Neither accent nor the errors' red: the effect's accent is orange and its errors cyan; the instrument's accent
        // is blue and its errors orange.
        constexpr funkgui::Col violet{ 155, 123, 224, 255 }, green{ 93, 187, 122, 255 }, gold{ 214, 178, 74, 255 };
        constexpr funkgui::Col blue{ 91, 155, 230, 255 }, pink{ 217, 115, 155, 255 };
        constexpr std::array<funkgui::Col, 4> effect{ violet, green, blue, gold };
        constexpr std::array<funkgui::Col, 4> instrument{ violet, green, gold, pink };
        const auto& set = product::kIsInstrument ? instrument : effect;
        const funkgui::Col c = set[static_cast<std::size_t>(index - 1) % set.size()];
        const bool dark = th.ground.r < 128;
        return dark ? c : funkgui::mix(c, funkgui::Col{ 0, 0, 0, 255 }, 0.22f);
    }
}

// ---- the page -----------------------------------------------------------------------------------------------------------

namespace playground::ui::settings
{
    namespace
    {
        using namespace usagepage;

        constexpr float kChartH = 168.0f;
        constexpr float kAxisW = 52.0f;          // the y axis's labels
        constexpr float kPlotTop = 8.0f, kLabelsH = 18.0f;
        constexpr float kReadoutH = 26.0f;
        constexpr float kHeadH = 22.0f, kTableRowH = 22.0f, kTotalH = 26.0f;
        constexpr uint32_t kBarIds = SettingsCard::kA11yBase + 0x38000u;
        constexpr uint32_t kTableIds = SettingsCard::kA11yBase + 0x3C000u;

        const char* const kRanges[] = { "7 days", "30 days", "12 months", "All" };
        const char* const kShows[] = { "Tokens", "Cost" };

        struct View
        {
            usage::Range range = usage::Range::month;
            bool cost = false;
            int picked = -1;                     // the bar the keys picked (-1 none)
            int hover = -1;                      // the bar under the pointer, as the last paint found it
            bool asking = false;                 // Reset asked, not yet answered
            std::shared_ptr<const UsageView> data;
            uint64_t modelRev = 0;
            bool modelMade = false;
            Model model;
        };

        uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001B3ull; }

        View& viewOf(Context& ctx)
        {
            View& v = ctx.card.viewState<View>(kUsagePage.id);
            if (auto d = ctx.port.chat().usage(v.range))
                v.data = std::move(d);
            if (v.data != nullptr && v.data->range == v.range && (!v.modelMade || v.modelRev != v.data->revision))
            {
                v.model = buildModel(v.data->snapshot, v.range);
                v.modelRev = v.data->revision;
                v.modelMade = true;
                if (v.picked >= static_cast<int>(v.model.buckets.size()))
                    v.picked = static_cast<int>(v.model.buckets.size()) - 1;
            }
            return v;
        }

        // The snapshot shown is of the range chosen (one of another is on its way).
        bool ready(const View& v) { return v.data != nullptr && v.data->range == v.range && v.modelMade; }

        std::string providerText(const std::string& kind)
        {
            if (kind == "anthropic")
                return "Anthropic";
            if (kind == "openrouter")
                return "OpenRouter";
            if (kind == "openai")
                return "OpenAI";
            if (kind == "gemini")
                return "Gemini";
            if (kind == "local")
                return "Local server";
            return "Other server";
        }

        std::string rangeWords(usage::Range r)
        {
            switch (r)
            {
                case usage::Range::week: return "a bar a day, the last 7 days";
                case usage::Range::month: return "a bar a day, the last 30 days";
                case usage::Range::year: return "a bar a month, the last 12 months";
                case usage::Range::all: return "a bar a month, every month kept";
            }
            return {};
        }

        // The table's columns, from the right: share, cost, out, cached, in, requests, provider; the model has the rest.
        struct Columns
        {
            float model, provider, requests, in, cached, out, cost, share;   // each one's right edge (the model's left)
        };
        Columns columns(float x, float w)
        {
            Columns c{};
            c.share = x + w;                          // right edges, from the right
            c.cost = c.share - 54.0f;
            c.out = c.cost - 86.0f;
            c.cached = c.out - 50.0f;
            c.in = c.cached - 54.0f;
            c.requests = c.in - 50.0f;
            c.provider = x + 146.0f;                  // the provider's left (the model has the rest)
            c.model = x;
            return c;
        }

        // The chart's own control (its place as the card put it, scrolled), or null.
        const Control* chartOf(const SettingsCard::Places& at)
        {
            for (const Control& c : at.controls)
                if (c.own && c.index == kChart)
                    return &c;
            return nullptr;
        }

        funkgui::Rect plotOf(const funkgui::Rect& r)
        {
            return { r.x + kAxisW, r.y + kPlotTop, r.w - kAxisW - 4.0f, r.h - kPlotTop - kLabelsH };
        }

        int bucketAt(const funkgui::Rect& plot, int n, funkgui::Point p)
        {
            if (n <= 0 || p.x < plot.x || p.x >= plot.right() || p.y < plot.y || p.y >= plot.bottom() + kLabelsH)
                return -1;
            return std::clamp(static_cast<int>((p.x - plot.x) / (plot.w / static_cast<float>(n))), 0, n - 1);
        }

        double valueOf(const Bucket& b, std::size_t s, bool cost)
        {
            return cost ? static_cast<double>(b.money[s].microUsd) : static_cast<double>(b.tokens[s]);
        }

        Axis axisOf(const Model& m, bool cost)
        {
            double max = 0.0;
            for (const Bucket& b : m.buckets)
                max = std::max(max, cost ? static_cast<double>(b.totalMoney().microUsd) : static_cast<double>(b.totalTokens()));
            return niceAxis(max, cost ? 1000.0 : 1.0);   // a tenth of a cent, a token
        }

        std::string axisText(double v, bool cost)
        {
            return cost ? axisMoney(static_cast<int64_t>(std::llround(v))) : tokensText(static_cast<int64_t>(std::llround(v)));
        }

        std::string shareText(int64_t part, int64_t whole)
        {
            if (whole <= 0 || part <= 0)
                return "0%";
            const double pc = 100.0 * static_cast<double>(part) / static_cast<double>(whole);
            if (pc < 1.0)
                return "<1%";
            char b[16];
            std::snprintf(b, sizeof b, "%.0f%%", pc);
            return b;
        }

        int64_t shareOf(const Series& s, bool cost) { return cost ? s.money.microUsd : s.counts.tokensIn() + s.counts.output; }

        // A row of the table in words (its accessibility title).
        std::string rowWords(const Series& s, const Series& total, bool cost, bool isTotal)
        {
            std::string w = isTotal ? std::string("Total") : s.model + ", " + providerText(s.provider);
            w += ": " + grouped(s.counts.requests) + (s.counts.requests == 1 ? " request, " : " requests, ");
            w += tokensText(s.counts.tokensIn()) + " tokens in (" + tokensText(s.counts.cacheRead + s.counts.cacheWrite5m + s.counts.cacheWrite1h)
               + " cached), " + tokensText(s.counts.output) + " out, " + moneyText(s.money);
            if (!isTotal)
                w += ", " + shareText(shareOf(s, cost), shareOf(total, cost)) + (cost ? " of the cost" : " of the tokens");
            return w;
        }

        // ---- build ----------------------------------------------------------------------------------------------------------
        void build(Builder& b)
        {
            View& v = viewOf(b.ctx);
            b.label("Range");
            b.select(kRange, { b.ctrlX, b.y, 128.0f, kRowH }, kRanges[static_cast<int>(v.range)], "Range");
            const float showX = b.ctrlX + 144.0f;
            b.line(Line::Kind::label, "Show", { showX, b.y, 44.0f, kRowH });
            b.select(kShow, { showX + 48.0f, b.y, 112.0f, kRowH }, kShows[v.cost ? 1 : 0], "Show");
            b.y += kRowStep + 4.0f;

            const bool has = ready(v);
            Control& chart = b.control(Act::item, kChart, { 0.0f, b.y, b.inner, kChartH }, "",
                                       std::string("Usage chart: ") + (v.cost ? "cost, " : "tokens, ") + rangeWords(v.range));
            chart.own = true;
            b.y += kChartH + kReadoutH;

            if (has && !v.model.empty())
                b.y += 8.0f + kHeadH + kTableRowH * static_cast<float>(v.model.rows.size()) + kTotalH + 14.0f;
            else
                b.y += 6.0f;

            // Reset, asked first.
            const bool any = has && !v.model.empty();
            if (!v.asking)
            {
                const std::string word = "Reset...";
                b.button(kReset, { 0.0f, b.y, Builder::pillW(word, false, false), kRowH }, word,
                         "Reset: forget every request counted", any || (v.data != nullptr && !v.data->snapshot.rows.empty()));
                b.y += kRowStep;
            }
            else
            {
                b.notes(std::string("Forget every request counted on ") + kThisMachine
                            + ", by every range and both playgrounds? This cannot be undone.",
                        0.0f, b.inner, Line::Kind::value);
                b.y += 6.0f;
                const std::string yes = "Reset", no = "Cancel";
                const float yesW = Builder::pillW(yes, false, false);
                b.button(kResetYes, { 0.0f, b.y, yesW, kRowH }, yes, "Reset: forget every request counted");
                b.button(kResetNo, { yesW + 8.0f, b.y, Builder::pillW(no, false, false), kRowH }, no, "Cancel: keep them");
                b.y += kRowStep;
            }
            if (v.data != nullptr && v.data->resetFailed)
                b.notes("The usage file could not be written: nothing was reset.", 0.0f, b.inner, Line::Kind::error);

            // The footnote: the price table's date, always.
            const std::string asOf = v.data != nullptr && !v.data->pricesAsOf.empty() ? v.data->pricesAsOf : usage::pricesInUse().asOf;
            b.y += 4.0f;
            b.notes("Prices as of " + asOf + ", the table in use. A cost with ~ is an estimate at those prices; one without "
                        "is the provider's own figure; + after it: a part has no price known.",
                    0.0f, b.inner);
            b.notes(std::string("Every request the chat sent from ") + kThisMachine + ", by both playgrounds: names and numbers, "
                        "never a word of a chat.",
                    0.0f, b.inner);
        }

        void act(const Control& c, Context& ctx)
        {
            if (c.act != Act::item)
                return;
            View& v = viewOf(ctx);
            funkgui::HostServices* host = ctx.host;
            SettingsCard* card = &ctx.card;
            const auto menu = [&](const std::vector<std::string>& labels, int current, std::function<void(int)> pick) {
                if (host == nullptr)
                    return;
                funkgui::MenuRequest request;
                for (std::size_t i = 0; i < labels.size(); ++i)
                    request.items.push_back(dropdown::choice(static_cast<int>(i) + 1, labels[i], static_cast<int>(i) == current));
                dropdown::dropdownMenu(request, c.r, current + 1);
                request.theme = productTheme(funkgui::Theme::byIndex(host->themeIndex()));
                const int n = static_cast<int>(labels.size());
                host->showMenu(request, [card, n, then = std::move(pick)](int chosen) {
                    if (!card->isOpen() || chosen < 1 || chosen > n)
                        return;
                    then(chosen - 1);
                    card->changed();
                });
            };
            switch (c.index)
            {
                case kRange:
                    menu({ kRanges[0], kRanges[1], kRanges[2], kRanges[3] }, static_cast<int>(v.range), [card](int i) {
                        View& w = card->viewState<View>(kUsagePage.id);
                        if (w.range != static_cast<usage::Range>(i))
                        {
                            w.range = static_cast<usage::Range>(i);
                            w.picked = -1;
                            w.modelMade = false;          // the bars are of another range
                        }
                    });
                    break;
                case kShow:
                    menu({ kShows[0], kShows[1] }, v.cost ? 1 : 0, [card](int i) { card->viewState<View>(kUsagePage.id).cost = i == 1; });
                    break;
                case kChart:
                    if (v.hover >= 0)
                        v.picked = v.hover;              // a click picks the bar under the pointer
                    break;
                case kReset:
                    v.asking = true;
                    break;
                case kResetYes:
                    v.asking = false;
                    ctx.port.chat().resetUsage();
                    break;
                case kResetNo:
                    v.asking = false;
                    break;
                default:
                    break;
            }
        }

        uint64_t key(const Context& constCtx)
        {
            Context& ctx = const_cast<Context&>(constCtx);
            const View& v = viewOf(ctx);
            uint64_t h = mix(0, v.data != nullptr ? v.data->revision : 0);
            h = mix(h, static_cast<uint64_t>(v.range));
            h = mix(h, v.cost ? 1u : 0u);
            h = mix(h, static_cast<uint64_t>(v.picked + 1));
            h = mix(h, v.asking ? 1u : 0u);
            return mix(h, v.modelMade ? 1u : 0u);
        }

        // ---- paint ----------------------------------------------------------------------------------------------------------
        void paint(funkgui::Canvas& c, const funkgui::Theme& th, const SettingsCard::Places& at, Context& ctx, funkgui::Point pointer,
                   bool hasPointer)
        {
            View& v = viewOf(ctx);
            const Control* chart = chartOf(at);
            if (chart == nullptr)
                return;
            const chat::Palette p = chat::palette(th);
            const funkgui::Rect r = chart->r;
            const funkgui::Rect plot = plotOf(r);
            const bool has = ready(v);
            const Model& m = v.model;
            const int n = has ? static_cast<int>(m.buckets.size()) : 0;
            v.hover = hasPointer && at.body.contains(pointer) ? bucketAt(plot, n, pointer) : -1;

            // The frame: the baseline, and the gridlines with their labels at round steps.
            const bool empty = !has || m.empty();
            const Axis axis = empty ? Axis{} : axisOf(m, v.cost);
            if (!empty)
                for (int s = 1; s <= axis.steps; ++s)
                {
                    const float y = std::round(plot.bottom() - plot.h * static_cast<float>(s) / static_cast<float>(axis.steps));
                    c.hairlineH(plot.x, y, plot.w, p.cardEdge);
                    c.text(axisText(axis.step * s, v.cost).c_str(), plot.x - 8.0f, c.capCentreTop(y, chat::kSmall), chat::kSmall, p.faint,
                           funkgui::Align::right);
                }
            c.hairlineH(plot.x, std::round(plot.bottom()), plot.w, p.composerEdge);
            c.text(v.cost ? "$0" : "0", plot.x - 8.0f, c.capCentreTop(plot.bottom(), chat::kSmall), chat::kSmall, p.faint, funkgui::Align::right);
            if (empty)
            {
                c.text(!has ? "Reading..." : "No requests yet.", plot.centreX(), c.capCentreTop(plot.centreY(), chat::kSmall), chat::kSmall,
                       p.dim, funkgui::Align::centre);
            }
            else
            {
                const float slot = plot.w / static_cast<float>(n);
                const float barW = std::clamp(std::floor(slot * 0.66f), 2.0f, 26.0f);
                const int shown = v.hover >= 0 ? v.hover : v.picked;
                if (shown >= 0)
                    c.rrect(plot.x + slot * static_cast<float>(shown), plot.y - 4.0f, slot, plot.h + 4.0f + kLabelsH, 4.0f, p.hover);
                // The bars, the largest model at the bottom of each stack.
                {
                    const funkgui::Canvas::Scope tag(c, kUsageBarTag, false);
                    for (int i = 0; i < n; ++i)
                    {
                        const Bucket& b = m.buckets[static_cast<std::size_t>(i)];
                        const float x = std::round(plot.x + slot * static_cast<float>(i) + 0.5f * (slot - barW));
                        float y = plot.bottom();
                        for (std::size_t s = 0; s < m.chartNames.size(); ++s)
                        {
                            const double val = valueOf(b, s, v.cost);
                            if (val <= 0.0)
                                continue;
                            const float h = static_cast<float>(val / axis.top) * plot.h;
                            const float top = std::round(y - h);
                            if (std::round(y) - top >= 1.0f)
                                c.rrect(x, top, barW, std::round(y) - top, 0.0f, colourOf(m.chartColours[s], th));
                            else
                                c.rrect(x, std::round(y) - 1.0f, barW, 1.0f, 0.0f, colourOf(m.chartColours[s], th));
                            y -= h;
                        }
                    }
                }
                // The labels under them, as many as fit.
                const float labelW = m.byMonth ? 30.0f : (m.range == usage::Range::week ? 32.0f : 22.0f);
                const int every = std::max(1, static_cast<int>(std::ceil(labelW / slot)));
                for (int i = n - 1; i >= 0; i -= every)
                {
                    const Bucket& b = m.buckets[static_cast<std::size_t>(i)];
                    c.text(b.label.c_str(), plot.x + slot * (static_cast<float>(i) + 0.5f), c.capCentreTop(plot.bottom() + 10.0f, chat::kSmall),
                           chat::kSmall, i == shown ? p.text : p.faint, funkgui::Align::centre);
                }
            }

            // The readout: the bar under the pointer, else the one picked, else the legend.
            const float ry = r.bottom() + 0.5f * kReadoutH;
            const int shown = v.hover >= 0 ? v.hover : v.picked;
            if (has && !m.empty() && shown >= 0)
                c.text(fitText(c, readoutText(m, shown, v.cost), chat::kSmall, r.w - 8.0f).c_str(), r.x + 4.0f, c.capCentreTop(ry, chat::kSmall),
                       chat::kSmall, p.text);
            else if (has && !m.empty())
            {
                float x = plot.x;
                for (std::size_t s = 0; s < m.chartNames.size(); ++s)
                {
                    const std::string& name = m.chartNames[s];
                    const float w = c.textWidth(name.c_str(), chat::kSmall);
                    if (x + 14.0f + w > r.right())
                        break;
                    c.rrect(x, std::round(ry - 4.0f), 8.0f, 8.0f, 2.0f, colourOf(m.chartColours[s], th));
                    c.text(name.c_str(), x + 13.0f, c.capCentreTop(ry, chat::kSmall), chat::kSmall, p.soft);
                    x += 13.0f + w + 18.0f;
                }
            }

            // The table.
            if (!has || m.empty())
                return;
            float y = r.bottom() + kReadoutH + 8.0f;
            const Columns col = columns(r.x, r.w);
            const auto right = [&](const std::string& s, float edge, float cy, const funkgui::TextStyle& st, funkgui::Col ink) {
                c.text(s.c_str(), edge, c.capCentreTop(cy, st), st, ink, funkgui::Align::right);
            };
            {
                const float cy = y + 0.5f * kHeadH;
                c.text("Model", col.model, c.capCentreTop(cy, chat::kSmall), chat::kSmall, p.dim);
                c.text("Provider", col.provider, c.capCentreTop(cy, chat::kSmall), chat::kSmall, p.dim);
                right("Reqs", col.requests, cy, chat::kSmall, p.dim);
                right("In", col.in, cy, chat::kSmall, p.dim);
                right("Cached", col.cached, cy, chat::kSmall, p.dim);
                right("Out", col.out, cy, chat::kSmall, p.dim);
                right("Cost", col.cost, cy, chat::kSmall, p.dim);
                right("Share", col.share, cy, chat::kSmall, p.dim);
                y += kHeadH;
                c.hairlineH(r.x, std::round(y), r.w, p.cardEdge);
            }
            const int64_t whole = shareOf(m.total, v.cost);
            const auto row = [&](const Series& s, bool isTotal, float h) {
                const float cy = y + 0.5f * h;
                const funkgui::TextStyle& st = isTotal ? chat::kSmallStrong : chat::kSmall;
                const funkgui::Col ink = p.text;
                float nameX = col.model;
                if (!isTotal)
                {
                    c.rrect(col.model, std::round(cy - 4.0f), 8.0f, 8.0f, 2.0f, colourOf(s.colour, th));
                    nameX += 14.0f;
                    c.text(fitText(c, providerText(s.provider), st, col.requests - 28.0f - col.provider).c_str(), col.provider,
                           c.capCentreTop(cy, st), st, p.soft);
                }
                c.text(fitText(c, isTotal ? std::string("Total") : s.model, st, col.provider - 8.0f - nameX).c_str(), nameX,
                       c.capCentreTop(cy, st), st, ink);
                right(grouped(s.counts.requests), col.requests, cy, st, ink);
                right(tokensText(s.counts.tokensIn()), col.in, cy, st, ink);
                right(tokensText(s.counts.cacheRead + s.counts.cacheWrite5m + s.counts.cacheWrite1h), col.cached, cy, st, p.soft);
                right(tokensText(s.counts.output), col.out, cy, st, ink);
                const std::string cost = moneyText(s.money);
                right(fitText(c, cost, st, 84.0f), col.cost, cy, st, s.money.priced ? ink : p.dim);
                right(isTotal ? std::string("100%") : shareText(shareOf(s, v.cost), whole), col.share, cy, st, isTotal ? ink : p.soft);
                y += h;
            };
            for (const Series& s : m.rows)
                row(s, false, kTableRowH);
            c.hairlineH(r.x, std::round(y + 1.0f), r.w, p.cardEdge);
            y += 2.0f;
            row(m.total, true, kTotalH);
        }

        bool keyed(const funkgui::KeyEvent& e, const Control& focused, Context& ctx)
        {
            if (focused.index != kChart)
                return false;
            View& v = viewOf(ctx);
            const int n = ready(v) && !v.model.empty() ? static_cast<int>(v.model.buckets.size()) : 0;
            if (n == 0)
                return false;
            using funkgui::Key;
            int at = v.picked;
            if (e.key == Key::left)
                at = at < 0 ? n - 1 : std::max(0, at - 1);
            else if (e.key == Key::right)
                at = at < 0 ? n - 1 : std::min(n - 1, at + 1);
            else if (e.key == Key::home)
                at = 0;
            else if (e.key == Key::end)
                at = n - 1;
            else
                return false;
            v.picked = at;
            return true;
        }

        bool meet(const funkgui::Rect& a, const funkgui::Rect& b)
        {
            return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
        }

        void a11y(std::vector<funkgui::A11yItem>& out, const SettingsCard::Places& at, Context& ctx)
        {
            View& v = viewOf(ctx);
            const Control* chart = chartOf(at);
            if (chart == nullptr)
                return;
            const bool has = ready(v) && !v.model.empty();
            const Model& m = v.model;
            const int shown = v.hover >= 0 ? v.hover : v.picked;
            funkgui::A11yItem group;
            group.id = chart->id;
            group.parent = SettingsCard::kA11yBase;
            group.role = funkgui::a11yrole::group;
            group.bounds = chart->r;
            group.title = chart->title;
            group.description = has ? "Left and Right move along the bars; Home and End go to the first and the last."
                                    : (ready(v) ? "No requests yet." : "Reading...");
            group.value = has && shown >= 0 ? readoutText(m, shown, v.cost) : "";
            group.visible = meet(at.body, chart->r);
            out.push_back(group);
            if (!has)
                return;
            const funkgui::Rect plot = plotOf(chart->r);
            const float slot = plot.w / static_cast<float>(m.buckets.size());
            for (std::size_t i = 0; i < m.buckets.size(); ++i)
            {
                funkgui::A11yItem it;
                it.id = kBarIds + static_cast<uint32_t>(i);
                it.parent = chart->id;
                it.role = funkgui::A11yRole::staticText;
                it.bounds = { plot.x + slot * static_cast<float>(i), plot.y, slot, plot.h };
                it.title = readoutText(m, static_cast<int>(i), v.cost);
                it.readOnly = true;
                it.visible = meet(at.body, it.bounds);
                out.push_back(std::move(it));
            }
            // The table: a row a model, then the total.
            const float top = chart->r.bottom() + kReadoutH + 8.0f;
            funkgui::A11yItem table;
            table.id = kTableIds;
            table.parent = SettingsCard::kA11yBase;
            table.role = funkgui::a11yrole::table;
            table.title = "Usage by model";
            table.rows = static_cast<int>(m.rows.size()) + 1;
            table.cols = 1;
            table.bounds = { chart->r.x, top, chart->r.w, kHeadH + kTableRowH * static_cast<float>(m.rows.size()) + kTotalH + 2.0f };
            table.visible = meet(at.body, table.bounds);
            out.push_back(table);
            float y = top + kHeadH;
            for (std::size_t i = 0; i <= m.rows.size(); ++i)
            {
                const bool isTotal = i == m.rows.size();
                funkgui::A11yItem it;
                it.id = kTableIds + 1u + static_cast<uint32_t>(i);
                it.parent = kTableIds;
                it.role = funkgui::a11yrole::row;
                it.row = static_cast<int>(i);
                it.bounds = { chart->r.x, y + (isTotal ? 2.0f : 0.0f), chart->r.w, isTotal ? kTotalH : kTableRowH };
                it.title = rowWords(isTotal ? m.total : m.rows[i], m.total, v.cost, isTotal);
                it.readOnly = true;
                it.visible = meet(at.body, it.bounds);
                out.push_back(std::move(it));
                y += kTableRowH;
            }
        }
    }

    const Page kUsagePage{
        "usage",
        "Usage",
        "What the chat's requests used, and what they cost.",
        nullptr,
        build,
        act,
        key,
        paint,
        keyed,
        a11y,
    };
}
