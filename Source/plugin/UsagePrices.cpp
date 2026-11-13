// The built-in price table (Usage.h's builtInPrices): Anthropic's list prices, USD a million tokens.
//
// Its twin is the website's src/lib/prices.json (Snipet/TestingPlaygroundBackend): the two must agree, row for row and
// in their date. A change here is a change there (and a new kAsOf), in the same step.
//
// Every row checked against Anthropic's published pricing page on 2026-11-12. A cache write is 1.25 times the input
// price for five minutes and twice it for an hour; a cache read a tenth of it unless the row says otherwise; a web
// search $10 a thousand. Fast mode's cache reads and writes take its multiplier too (Usage.cpp's estimate).
#include "plugin/Usage.h"

namespace playground::usage
{
    namespace
    {
        constexpr const char* kAsOf = "2026-11-12";
        constexpr double kSearch = 10.0 / 1000.0;   // USD a web search

        //                       in     out    read   5m      1h     fast in / out
        Price p(double in, double out, double read, double w5, double w1, double fastIn = 0, double fastOut = 0)
        {
            return { in, out, read, w5, w1, fastIn, fastOut, kSearch };
        }

        PriceTable make()
        {
            PriceTable t;
            t.asOf = kAsOf;
            auto row = [&](const char* model, const Price& price) { t.rows.push_back({ "anthropic", model, price }); };
            row("claude-fable-5-2", p(8, 40, 0.20, 10, 16, 16, 80));
            row("claude-fable-5-1", p(10, 50, 0.25, 12.50, 20));
            row("claude-fable-5", p(10, 50, 1.00, 12.50, 20));
            row("claude-opus-5-5", p(4, 20, 0.20, 5, 8, 8, 40));
            row("claude-opus-5", p(5, 25, 0.50, 6.25, 10, 10, 50));
            // Fast mode on this model is removed on 2026-11-30 (the API refuses it from then): its fast prices stay for
            // the requests made before, which the ledger keeps.
            row("claude-opus-4-8", p(5, 25, 0.50, 6.25, 10, 10, 50));
            row("claude-opus-4-7", p(5, 25, 0.50, 6.25, 10));
            row("claude-opus-4-6", p(5, 25, 0.50, 6.25, 10));
            row("claude-sonnet-5-5", p(2, 10, 0.10, 2.50, 4));
            row("claude-sonnet-5", p(2, 10, 0.20, 2.50, 4));
            row("claude-sonnet-4-6", p(3, 15, 0.30, 3.75, 6));
            // Prompts up to 100K tokens; past that it costs five times as much, which the estimate does not follow (the
            // usage page's footnote says so).
            row("claude-haiku-5-5", p(0.10, 0.50, 0.01, 0.125, 0.20));
            row("claude-haiku-4-5", p(1, 5, 0.10, 1.25, 2));
            return t;
        }
    }

    const PriceTable& builtInPrices()
    {
        static const PriceTable table = make();
        return table;
    }
}
