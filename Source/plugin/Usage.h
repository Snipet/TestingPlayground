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
//   requestReset  the user confirmed a Reset (the Settings card's Usage page, asked first): the cut. What this
//                 process counted until now and has not written is set aside for the reset, which is owed from then
//                 until it has run; a count added after it is after the reset, kept for the flush after it. In memory
//                 only, under the ledger's own mutex (never the file or its folder's lock): the message thread's
//   reset         runs an owed reset (none owed: nothing, true): under the ledger's flush lock and the folder's
//                 .lock, as a flush, the empty file written beside and renamed in, then the counts set aside dropped.
//                 One that fails (the lock not had in its few seconds, the write refused) changes nothing: the file is
//                 as it was and the counts set aside go back with those added meanwhile; it is no longer owed (the
//                 user asks again). A flush while one is owed writes nothing (its counts are after the cut and would
//                 be emptied with the file): it waits for the reset, so the owner runs an owed reset before its last
//                 flush. Another process's counts not yet written are its own: they reach the emptied file later.
//                 Its outcome is the ledger's (lastResetFailed: set by the reset, cleared by the next requestReset),
//                 so every instance's page says a failure, whichever instance's thread ran the reset
//   revision      moves whenever this process's ledger changes (a count added, a flush or a reset done), so a reader
//                 of snapshots (ChatAgent::usage) knows when to read again; another process's write does not move it
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

#include <atomic>
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
        Snapshot snapshot(Range range, std::string_view today) const;
        Snapshot snapshot(Range range) const { return snapshot(range, usage::today()); }
        // Above, "requestReset" and "reset": reset true when the emptied file is in place (or none was owed); false (the
        // file as it was, the counts set aside kept) when the lock or the write failed.
        void requestReset();
        bool reset();
        bool resetOwed() const;
        bool lastResetFailed() const;
        bool pending() const;
        uint64_t revision() const noexcept { return revision_.load(std::memory_order_acquire); }
        void setFile(std::filesystem::path file);
        std::filesystem::path file() const;

    private:
        mutable std::mutex mutex_;             // the pending counts and the file's name (never held across the file's I/O)
        std::mutex flushMutex_;                // one flush at a time in this ledger
        std::filesystem::path file_;
        std::vector<Row> pending_;
        std::vector<Row> cut_;                 // what an owed reset will drop (requestReset)
        bool owed_ = false;
        bool failed_ = false;                  // the last reset failed (until the next requestReset)
        std::atomic<uint64_t> revision_{ 0 };
    };

    // The process's ledger, on usage.json in the chat store's folder (chatstore::directory(), looked up at each flush and
    // snapshot, so a probe's own folder takes it).
    void add(const Key& key, const Counts& counts);
    bool flush();
    void requestReset();
    bool reset();
    bool resetOwed();
    bool lastResetFailed();
    uint64_t revision() noexcept;                // moves with each count, reset and price table taken (offerPrices)
    Snapshot snapshot(Range range);
    std::filesystem::path processFile();
    // Probes only: every write of a ledger's file fails (as a full disk or a folder that may not be written would),
    // so a flush and a reset can be seen failing; nothing else changes.
    void setWritesFailForTests(bool fail);

    // The file's rows (none when it is missing or no ledger), and the JSON a set of rows is written as.
    std::vector<Row> read(const std::filesystem::path& file, bool* damaged = nullptr);
    std::string write(const std::vector<Row>& rows);
}
