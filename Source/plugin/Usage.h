#pragma once

// The chat's usage: how many tokens each model used, day by day, and what that cost (docs/plan/09-usage.md).
//
//   Key, Counts   a row of the ledger: a day (the user's local day), a provider (its kind, never a server's address),
//                 the model that served and whether it ran fast; what its requests used
//   add           one request's counts, from any thread (ChatAgent::countUsage: every request that reached an API and
//                 reported usage, whatever became of it: a turn's rounds, retries, broken streams, the planner's, a
//                 provider's work-around request)
//   flush         this process's counts since its last write added to usage.json, under the folder's .lock (two
//                 instances, two products and two hosts add up instead of overwriting); on the agent's worker, never
//                 under its mutex
//   snapshot      a copy of the ledger (the file and what is not written yet) for a range of days
//   Price, costOf the price of a model (UsagePrices.cpp's table, a provider's listed price) and a row's cost: the
//                 provider's reported figure where every request reported one, else an estimate at list prices, else
//                 none
//
// usage.json (the chat store's folder, chatstore::directory(), which both products share):
//   {"format": "funk-playground-usage", "version": 1, "days": [{"day", "provider", "model", "fast", <counts>}]}
// Counts and names only: no word of a chat, no key, no server address, no project. Kept 400 days by day; older days
// folded into one row a month ("2025-08"). Plain JSON, 0600.
//
// The header is JUCE-free (the editor's page will take a Snapshot); Usage.cpp reads and writes the file with juce_core.

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playground::usage
{
    inline constexpr const char* kFormat = "funk-playground-usage";
    inline constexpr int kVersion = 1;
    inline constexpr int kDaysKept = 400;      // days kept by day; older ones folded into months

    struct Key
    {
        std::string day;        // the user's local day, "2026-11-12" (a folded month: "2025-08")
        std::string provider;   // "anthropic", "openrouter", "openai", "gemini", "local", "other"
        std::string model;      // the model that served the request (the response's), "unknown" when none said
        bool fast = false;      // it ran in fast mode (the response's usage.speed)
        friend auto operator<=>(const Key&, const Key&) = default;
    };

    struct Counts
    {
        int64_t requests = 0, input = 0, output = 0, cacheRead = 0, cacheWrite5m = 0, cacheWrite1h = 0;
        int64_t webSearches = 0, webFetches = 0;
        int64_t reportedMicroUsd = 0;          // the provider's own figure (OpenRouter's usage.cost), summed
        int64_t reportedRequests = 0;          // the requests that reported one
        Counts& operator+=(const Counts& o);
        int64_t tokensIn() const noexcept { return input + cacheRead + cacheWrite5m + cacheWrite1h; }
        friend bool operator==(const Counts&, const Counts&) = default;
    };

    struct Row
    {
        Key key;
        Counts counts;
    };

    // ---- the provider -------------------------------------------------------------------------------------------------
    // A provider's kind as the ledger names it: "anthropic", "gemini", and of an OpenAI-compatible server "openrouter"
    // (openrouter.ai), "openai" (api.openai.com), "local" (on this machine or the local network), else "other". Never
    // the server's address: a private server's name is the user's own business.
    std::string providerName(std::string_view kind, std::string_view host, bool local);
    // A model's name as kept: printable ASCII with no space, at most 200 characters (an id), else "unknown".
    std::string modelName(std::string_view model);

    // ---- days -----------------------------------------------------------------------------------------------------------
    std::string today();                                    // the user's local day, "2026-11-12"
    bool validDay(std::string_view day);                    // "YYYY-MM-DD"
    bool validMonth(std::string_view month);                // "YYYY-MM"
    int64_t dayNumber(std::string_view day);                // days since 1970-01-01 (a month: its first day)
    std::string dayOf(int64_t number);                      // its inverse
    // The rows as kept on `today`: summed by key, the days 400 or more before it folded into their month, in order.
    std::vector<Row> tidy(std::vector<Row> rows, std::string_view today);

    // ---- prices -------------------------------------------------------------------------------------------------------
    // USD a million tokens. A cache read and the two cache writes are the row's own (a tenth, 1.25 times and twice the
    // input price unless a row says otherwise); fast mode's input and output 0 where the model has none, and its cache
    // reads and writes take fast mode's multiplier too (the fast-mode docs: caching's multipliers apply on top of it).
    struct Price
    {
        double in = 0, out = 0, cacheRead = 0, write5m = 0, write1h = 0;
        double fastIn = 0, fastOut = 0;
        double webSearch = 0;                  // USD a search (Anthropic's: $10 a thousand)
    };
    struct PriceRow
    {
        std::string provider, model;           // "anthropic", "claude-opus-5-5"
        Price price;
    };
    struct PriceTable
    {
        std::string asOf;                      // the day its prices were checked, "2026-11-12"
        std::vector<PriceRow> rows;
    };
    // The table compiled into the plugin (UsagePrices.cpp).
    const PriceTable& builtInPrices();
    // A table from elsewhere (the website's, package 08): used in place of the one in use when its date is later, and
    // then true. The process's; the built-in one is in use until then.
    bool offerPrices(PriceTable table);
    PriceTable pricesInUse();
    void resetPricesForTests();
    // A provider's own list price for a model (OpenRouter's models list's `pricing`), kept for the process: it prices
    // that model's requests whose cost was not reported. Before the table's.
    void setListedPrice(const std::string& provider, const std::string& model, const Price& price);
    // The price of `model` from `provider`: its listed price, else the table's row of that id (or of that id with a
    // date after it, "claude-opus-5-5-20261001"); none when neither knows it.
    std::optional<Price> priceOf(const std::string& provider, const std::string& model);
    std::optional<Price> priceOf(const PriceTable& table, const std::string& provider, const std::string& model);

    struct Cost
    {
        enum class Kind : uint8_t { none, reported, estimate };
        Kind kind = Kind::none;
        int64_t microUsd = 0;
    };
    // What `counts` of `key` cost at `price`, in millionths of a dollar: a fast row at fast mode's prices (none when the
    // price has no fast mode), web searches at their price. None when `price` is.
    std::optional<int64_t> estimate(const Key& key, const Counts& counts, const std::optional<Price>& price);
    // A row's cost: the reported figure where every request reported one, else the estimate, else none.
    Cost costOf(const Key& key, const Counts& counts, const std::optional<Price>& price);
    Cost costOf(const Key& key, const Counts& counts);      // at priceOf's price
    // "$0.12", "$1.23", "$0.004", "<$0.001"; an estimate with "~" before it.
    std::string costText(const Cost& cost);

    // ---- the ledger -------------------------------------------------------------------------------------------------------
    enum class Range : uint8_t { week, month, year, all };
    struct Snapshot
    {
        std::string today;
        std::vector<Row> rows;                 // in the range, summed by key, in order
    };

    class Ledger
    {
    public:
        explicit Ledger(std::filesystem::path file) : file_(std::move(file)) {}
        Ledger() = default;

        void add(const Key& key, const Counts& counts);
        // The counts not written yet added to the file (read, added, written beside and renamed, under the folder's
        // .lock), on `today`. True when nothing waits any more; false (the counts kept for the next) when the lock or
        // the write failed. A file that is not a ledger is kept beside as usage.json.bad, never added to.
        bool flush(std::string_view today);
        bool flush() { return flush(usage::today()); }
        // Every count gone: the file emptied (written beside and renamed, under the folder's .lock) and what was not
        // written yet dropped. False when the lock or the write failed (the file as it was).
        bool reset(std::string_view today);
        Snapshot snapshot(Range range, std::string_view today) const;
        Snapshot snapshot(Range range) const { return snapshot(range, usage::today()); }
        bool pending() const;
        void setFile(std::filesystem::path file);
        std::filesystem::path file() const;

    private:
        mutable std::mutex mutex_;             // the pending counts and the file's name (never held across the file's I/O)
        std::mutex flushMutex_;                // one flush at a time in this ledger
        std::filesystem::path file_;
        std::vector<Row> pending_;
    };

    // The process's ledger, on usage.json in the chat store's folder (chatstore::directory(), looked up at each flush and
    // snapshot, so a probe's own folder takes it).
    void add(const Key& key, const Counts& counts);
    bool flush();
    Snapshot snapshot(Range range);
    bool reset();                                // the usage page's Reset (asked first): Ledger::reset
    std::filesystem::path processFile();
    // Moves when this process's ledger changes (a count added, a flush, a reset): what a reader of snapshot() follows,
    // besides the file another process may write (read again now and then).
    uint64_t revision();

    // The file's rows (none when it is missing or no ledger), and the JSON a set of rows is written as.
    std::vector<Row> read(const std::filesystem::path& file, bool* damaged = nullptr);
    std::string write(const std::vector<Row>& rows);
}
