// The chat's usage ledger (Usage.h).
#include "plugin/Usage.h"
#include "plugin/FileReplace.h"

#include "plugin/ChatStore.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace playground::usage
{
    namespace fs = std::filesystem;

    Counts& Counts::operator+=(const Counts& o)
    {
        requests += o.requests;
        input += o.input;
        output += o.output;
        cacheRead += o.cacheRead;
        cacheWrite5m += o.cacheWrite5m;
        cacheWrite1h += o.cacheWrite1h;
        webSearches += o.webSearches;
        webFetches += o.webFetches;
        reportedMicroUsd += o.reportedMicroUsd;
        reportedRequests += o.reportedRequests;
        return *this;
    }

    // ---- the provider -------------------------------------------------------------------------------------------------
    std::string providerName(std::string_view kind, std::string_view host, bool local)
    {
        if (kind == "anthropic" || kind.empty())
            return "anthropic";
        if (kind == "gemini")
            return "gemini";
        if (kind != "openai")
            return "other";
        if (local)
            return "local";
        if (host == "openrouter.ai")
            return "openrouter";
        if (host == "api.openai.com")
            return "openai";
        return "other";
    }

    std::string modelName(std::string_view model)
    {
        if (model.empty() || model.size() > 200)
            return "unknown";
        for (const char c : model)
            if (c <= ' ' || c > '~')
                return "unknown";
        return std::string(model);
    }

    // ---- days -----------------------------------------------------------------------------------------------------------
    namespace
    {
        bool digits(std::string_view s)
        {
            return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
        }

        int number(std::string_view s)
        {
            int n = 0;
            for (const char c : s)
                n = n * 10 + (c - '0');
            return n;
        }

        // Days since 1970-01-01 of a civil date, and back (Howard Hinnant's algorithms).
        int64_t daysFromCivil(int64_t y, unsigned m, unsigned d)
        {
            y -= m <= 2 ? 1 : 0;
            const int64_t era = (y >= 0 ? y : y - 399) / 400;
            const auto yoe = static_cast<unsigned>(y - era * 400);
            const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
            const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            return era * 146097 + static_cast<int64_t>(doe) - 719468;
        }

        std::string twoDigits(unsigned n) { return (n < 10 ? "0" : "") + std::to_string(n); }
    }

    std::string today()
    {
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        return std::to_string(tm.tm_year + 1900) + "-" + twoDigits(static_cast<unsigned>(tm.tm_mon + 1)) + "-"
             + twoDigits(static_cast<unsigned>(tm.tm_mday));
    }

    bool validMonth(std::string_view s)
    {
        if (s.size() != 7 || s[4] != '-' || !digits(s.substr(0, 4)) || !digits(s.substr(5, 2)))
            return false;
        const int m = number(s.substr(5, 2));
        return m >= 1 && m <= 12;
    }

    bool validDay(std::string_view s)
    {
        if (s.size() != 10 || s[7] != '-' || !validMonth(s.substr(0, 7)) || !digits(s.substr(8, 2)))
            return false;
        const int d = number(s.substr(8, 2));
        return d >= 1 && d <= 31 && dayOf(dayNumber(s)) == s;   // no 31 February
    }

    int64_t dayNumber(std::string_view s)
    {
        if (s.size() < 7)
            return 0;
        const int d = s.size() >= 10 ? number(s.substr(8, 2)) : 1;
        return daysFromCivil(number(s.substr(0, 4)), static_cast<unsigned>(number(s.substr(5, 2))), static_cast<unsigned>(d));
    }

    std::string dayOf(int64_t z)
    {
        z += 719468;
        const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        const auto doe = static_cast<unsigned>(z - era * 146097);
        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const unsigned mp = (5 * doy + 2) / 153;
        const unsigned d = doy - (153 * mp + 2) / 5 + 1;
        const unsigned m = mp < 10 ? mp + 3 : mp - 9;
        const int64_t y = static_cast<int64_t>(yoe) + era * 400 + (m <= 2 ? 1 : 0);
        return std::to_string(y) + "-" + twoDigits(m) + "-" + twoDigits(d);
    }

    std::vector<Row> tidy(std::vector<Row> rows, std::string_view todayDay)
    {
        const int64_t now = dayNumber(todayDay);
        std::map<Key, Counts> sum;
        for (Row& r : rows)
        {
            if (validDay(r.key.day) && now - dayNumber(r.key.day) >= kDaysKept)
                r.key.day = r.key.day.substr(0, 7);
            sum[r.key] += r.counts;
        }
        std::vector<Row> out;
        out.reserve(sum.size());
        for (auto& [k, c] : sum)
            out.push_back({ k, c });
        return out;
    }

    // ---- prices -------------------------------------------------------------------------------------------------------
    namespace
    {
        std::mutex pricesMutex;
        std::optional<PriceTable> offered;     // a later table than the built-in one, once offered
        std::map<std::pair<std::string, std::string>, Price> listed;

        bool datedFrom(const std::string& model, const std::string& id)
        {
            // "claude-opus-5-5-20261001": the id, a dash and a date of eight digits.
            return model.size() == id.size() + 9 && model.compare(0, id.size(), id) == 0 && model[id.size()] == '-'
                && digits(std::string_view(model).substr(id.size() + 1));
        }
    }

    namespace
    {
        // Moves each time the table in use changes (a later one taken, the probes' reset): revision() adds it, so a
        // reader that follows the revision (ChatAgent::usage, the Usage page) prices with the new table at its next
        // read, not only at its periodic reread.
        std::atomic<uint64_t> pricesChanged{ 0 };
    }

    bool offerPrices(PriceTable table)
    {
        if (!validDay(table.asOf))
            return false;
        {
            const std::lock_guard<std::mutex> lock(pricesMutex);
            const std::string& now = offered ? offered->asOf : builtInPrices().asOf;
            if (table.asOf <= now)
                return false;
            offered = std::move(table);
        }
        pricesChanged.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }

    PriceTable pricesInUse()
    {
        const std::lock_guard<std::mutex> lock(pricesMutex);
        return offered ? *offered : builtInPrices();
    }

    void resetPricesForTests()
    {
        {
            const std::lock_guard<std::mutex> lock(pricesMutex);
            offered.reset();
            listed.clear();
        }
        pricesChanged.fetch_add(1, std::memory_order_acq_rel);
    }

    void setListedPrice(const std::string& provider, const std::string& model, const Price& price)
    {
        const std::lock_guard<std::mutex> lock(pricesMutex);
        listed[{ provider, model }] = price;
    }

    std::optional<Price> priceOf(const PriceTable& table, const std::string& provider, const std::string& model)
    {
        for (const PriceRow& r : table.rows)
            if (r.provider == provider && r.model == model)
                return r.price;
        for (const PriceRow& r : table.rows)
            if (r.provider == provider && datedFrom(model, r.model))
                return r.price;
        return std::nullopt;
    }

    std::optional<Price> priceOf(const std::string& provider, const std::string& model)
    {
        {
            const std::lock_guard<std::mutex> lock(pricesMutex);
            if (const auto it = listed.find({ provider, model }); it != listed.end())
                return it->second;
            if (offered)
                return priceOf(*offered, provider, model);
        }
        return priceOf(builtInPrices(), provider, model);
    }

    std::optional<int64_t> estimate(const Key& key, const Counts& c, const std::optional<Price>& price)
    {
        if (!price)
            return std::nullopt;
        Price p = *price;
        if (key.fast)
        {
            if (p.fastIn <= 0 || p.fastOut <= 0 || p.in <= 0)
                return std::nullopt;             // a fast row with no fast price: never a guess
            const double m = p.fastIn / p.in;
            p.cacheRead *= m;
            p.write5m *= m;
            p.write1h *= m;
            p.in = p.fastIn;
            p.out = p.fastOut;
        }
        // USD a million tokens times tokens is millionths of a dollar.
        const double micro = static_cast<double>(c.input) * p.in + static_cast<double>(c.output) * p.out
                           + static_cast<double>(c.cacheRead) * p.cacheRead + static_cast<double>(c.cacheWrite5m) * p.write5m
                           + static_cast<double>(c.cacheWrite1h) * p.write1h
                           + static_cast<double>(c.webSearches) * p.webSearch * 1e6;
        return static_cast<int64_t>(std::llround(micro));
    }

    Cost costOf(const Key& key, const Counts& counts, const std::optional<Price>& price)
    {
        if (counts.requests > 0 && counts.reportedRequests >= counts.requests)
            return { Cost::Kind::reported, counts.reportedMicroUsd };
        if (const auto e = estimate(key, counts, price))
            return { Cost::Kind::estimate, *e };
        return {};
    }

    Cost costOf(const Key& key, const Counts& counts) { return costOf(key, counts, priceOf(key.provider, key.model)); }

    std::string costText(const Cost& cost)
    {
        if (cost.kind == Cost::Kind::none)
            return {};
        const std::string mark = cost.kind == Cost::Kind::estimate ? "~" : "";
        const int64_t m = std::max<int64_t>(0, cost.microUsd);
        char buf[64];
        if (m > 0 && m < 1000)
            return mark + "<$0.001";
        if (m < 10000)
            std::snprintf(buf, sizeof(buf), "$%.3f", static_cast<double>(m) / 1e6);
        else
            std::snprintf(buf, sizeof(buf), "$%.2f", static_cast<double>(m) / 1e6);
        return mark + buf;
    }

    // ---- the file ---------------------------------------------------------------------------------------------------------
    namespace
    {
        // The counts' names in the file, in order.
        struct Field
        {
            const char* name;
            int64_t Counts::* member;
        };
        constexpr Field kFields[] = {
            { "requests", &Counts::requests },           { "input", &Counts::input },
            { "output", &Counts::output },               { "cache_read", &Counts::cacheRead },
            { "cache_write_5m", &Counts::cacheWrite5m }, { "cache_write_1h", &Counts::cacheWrite1h },
            { "web_searches", &Counts::webSearches },    { "web_fetches", &Counts::webFetches },
            { "reported_micro_usd", &Counts::reportedMicroUsd }, { "reported_requests", &Counts::reportedRequests },
        };

        std::optional<std::string> readBytes(const fs::path& p)
        {
            std::ifstream in(p, std::ios::binary);
            if (!in)
                return std::nullopt;
            std::ostringstream s;
            s << in.rdbuf();
            return s.str();
        }

        std::atomic<bool> writesFail{ false };     // setWritesFailForTests

        // `bytes` written beside `p` and renamed over it, 0600 (FactoryStore.cpp's writeFile, made private).
        bool writeFile(const fs::path& p, const std::string& bytes)
        {
            if (writesFail.load())
                return false;
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
            const std::string tag = juce::Uuid().toString().substring(0, 12).toStdString();
            const fs::path temp = p.parent_path() / (p.filename().string() + "." + tag + ".tmp");
            bool written = false, moved = false;
            {
                std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                out.close();
                written = static_cast<bool>(out);
            }
            if (written)
            {
                fs::permissions(temp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
                moved = files::replace(temp, p, ec);   // FileReplace.h: a reader on Windows waited out
            }
            if (!written || !moved)                  // the helper's answer: ec may say a try's error it waited out
            {
                std::error_code e;
                fs::remove(temp, e);
                return false;
            }
            return true;
        }

        // The folder's lock (.lock): FactoryStore.cpp's FolderLock, its twin (flock on its own open file, on Windows an
        // open that shares nothing), tried every few milliseconds for a few seconds at most.
        class FolderLock
        {
        public:
            explicit FolderLock(const fs::path& dir)
            {
                std::error_code ec;
                fs::create_directories(dir, ec);
                const fs::path p = dir / ".lock";
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (std::chrono::steady_clock::now() < until)
                {
#if defined(_WIN32)
                    int fd = -1;
                    if (_wsopen_s(&fd, p.c_str(), _O_RDWR | _O_CREAT | _O_BINARY, _SH_DENYRW, _S_IREAD | _S_IWRITE) == 0 && fd >= 0)
                    {
                        fd_ = fd;
                        return;
                    }
#else
                    const int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
                    if (fd >= 0)
                    {
                        if (::flock(fd, LOCK_EX | LOCK_NB) == 0)
                        {
                            fd_ = fd;
                            return;
                        }
                        ::close(fd);
                    }
#endif
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                }
            }
            ~FolderLock()
            {
                if (fd_ < 0)
                    return;
#if defined(_WIN32)
                _close(fd_);
#else
                ::flock(fd_, LOCK_UN);
                ::close(fd_);
#endif
            }
            FolderLock(const FolderLock&) = delete;
            FolderLock& operator=(const FolderLock&) = delete;
            bool held() const noexcept { return fd_ >= 0; }

        private:
            int fd_ = -1;
        };

        void addRows(std::vector<Row>& into, const Key& key, const Counts& counts)
        {
            for (Row& r : into)
                if (r.key == key)
                {
                    r.counts += counts;
                    return;
                }
            into.push_back({ key, counts });
        }
    }

    std::vector<Row> read(const fs::path& file, bool* damaged)
    {
        if (damaged != nullptr)
            *damaged = false;
        const auto bytes = readBytes(file);
        if (!bytes)
            return {};
        const juce::var v = juce::JSON::parse(juce::String::fromUTF8(bytes->data(), static_cast<int>(bytes->size())));
        if (!v.isObject() || v.getProperty("format", "").toString() != kFormat
            || static_cast<int>(v.getProperty("version", 0)) < 1)
        {
            if (damaged != nullptr)
                *damaged = true;
            return {};
        }
        std::vector<Row> rows;
        if (const auto* days = v.getProperty("days", juce::var()).getArray())
            for (const juce::var& d : *days)
            {
                Row r;
                r.key.day = d.getProperty("day", "").toString().toStdString();
                if (!validDay(r.key.day) && !validMonth(r.key.day))
                    continue;
                r.key.provider = "other";
                const std::string provider = d.getProperty("provider", "").toString().toStdString();
                for (const char* known : { "anthropic", "openrouter", "openai", "gemini", "local", "other" })
                    if (provider == known)
                        r.key.provider = known;
                r.key.model = modelName(d.getProperty("model", "").toString().toStdString());
                r.key.fast = static_cast<bool>(d.getProperty("fast", false));
                for (const Field& f : kFields)
                    r.counts.*f.member = std::max<int64_t>(0, static_cast<juce::int64>(d.getProperty(f.name, 0)));
                rows.push_back(std::move(r));
            }
        return rows;
    }

    std::string write(const std::vector<Row>& rows)
    {
        // Written by hand, a row a line: names and numbers alone, so the file says at a glance what it holds.
        std::string out = std::string("{\"format\": \"") + kFormat + "\", \"version\": " + std::to_string(kVersion)
                        + ", \"days\": [";
        bool first = true;
        for (const Row& r : rows)
        {
            out += first ? "\n  " : ",\n  ";
            first = false;
            out += "{\"day\": \"" + r.key.day + "\", \"provider\": \"" + r.key.provider + "\", \"model\": \""
                 + modelName(r.key.model) + "\", \"fast\": " + (r.key.fast ? "true" : "false");
            for (const Field& f : kFields)
                out += std::string(", \"") + f.name + "\": " + std::to_string(r.counts.*f.member);
            out += "}";
        }
        out += rows.empty() ? "]}\n" : "\n]}\n";
        return out;
    }

    // ---- the ledger -------------------------------------------------------------------------------------------------------
    void Ledger::add(const Key& key, const Counts& counts)
    {
        Key k = key;
        k.model = modelName(k.model);
        if (!validDay(k.day))
            k.day = today();
        const std::lock_guard<std::mutex> lock(mutex_);
        addRows(pending_, k, counts);
        revision_.fetch_add(1, std::memory_order_acq_rel);
    }

    bool Ledger::pending() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return !pending_.empty();
    }

    void Ledger::setFile(fs::path file)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        file_ = std::move(file);
    }

    fs::path Ledger::file() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return file_;
    }

    bool Ledger::flush(std::string_view todayDay)
    {
        const std::lock_guard<std::mutex> one(flushMutex_);
        std::vector<Row> taken;
        fs::path file;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (owed_)
                return false;                       // after the cut: written once the reset has run
            if (pending_.empty())
                return true;
            taken.swap(pending_);
            file = file_;
        }
        bool ok = false;
        if (!file.empty())
        {
            const FolderLock lock(file.parent_path());
            if (lock.held())
            {
                bool damaged = false;
                std::vector<Row> rows = read(file, &damaged);
                if (damaged)
                {
                    std::error_code ec;
                    fs::path bad = file;
                    bad += ".bad";
                    fs::rename(file, bad, ec);   // kept for the user, never added to or deleted
                }
                for (const Row& r : taken)
                    addRows(rows, r.key, r.counts);
                ok = writeFile(file, write(tidy(std::move(rows), todayDay)));
            }
        }
        if (ok)
            revision_.fetch_add(1, std::memory_order_acq_rel);
        if (!ok)
        {
            // Kept for the next flush, with what came meanwhile.
            const std::lock_guard<std::mutex> lock(mutex_);
            for (const Row& r : taken)
                addRows(pending_, r.key, r.counts);
        }
        return ok;
    }

    void Ledger::requestReset()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const Row& r : pending_)
            addRows(cut_, r.key, r.counts);
        pending_.clear();
        owed_ = true;
        failed_ = false;
        revision_.fetch_add(1, std::memory_order_acq_rel);
    }

    bool Ledger::resetOwed() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return owed_;
    }

    bool Ledger::lastResetFailed() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return failed_;
    }

    bool Ledger::reset()
    {
        const std::lock_guard<std::mutex> one(flushMutex_);        // one flush or reset at a time
        fs::path file;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (!owed_)
                return true;
            file = file_;
        }
        bool ok = false;
        if (!file.empty())
        {
            const FolderLock lock(file.parent_path());
            if (lock.held())
            {
                // A file that is no ledger is kept beside as usage.json.bad, as a flush keeps it, and put back when the
                // empty one cannot be written.
                bool damaged = false;
                (void) read(file, &damaged);
                fs::path bad = file;
                bad += ".bad";
                std::error_code ec;
                if (damaged)
                    fs::rename(file, bad, ec);
                ok = writeFile(file, write({}));
                if (!ok && damaged && !ec)
                {
                    std::error_code back;
                    fs::rename(bad, file, back);
                }
            }
        }
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (!ok)
                for (const Row& r : cut_)              // nothing lost: back with what came meanwhile
                    addRows(pending_, r.key, r.counts);
            cut_.clear();
            owed_ = false;
            failed_ = !ok;
        }
        revision_.fetch_add(1, std::memory_order_acq_rel);
        return ok;
    }

    Snapshot Ledger::snapshot(Range range, std::string_view todayDay) const
    {
        fs::path file;
        std::vector<Row> rows;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            file = file_;
            rows = pending_;
            rows.insert(rows.end(), cut_.begin(), cut_.end());   // in the file's place until the reset runs
        }
        if (!file.empty())
            for (Row& r : read(file))
                rows.push_back(std::move(r));
        rows = tidy(std::move(rows), todayDay);
        const int64_t now = dayNumber(todayDay);
        std::string from;                      // the first day in the range ("" all)
        if (range == Range::week)
            from = dayOf(now - 6);
        else if (range == Range::month)
            from = dayOf(now - 29);
        else if (range == Range::year)
        {
            // The first day of the month eleven months back: twelve months, this one included.
            int y = number(std::string_view(todayDay).substr(0, 4));
            int m = number(std::string_view(todayDay).substr(5, 2)) - 11;
            if (m < 1)
            {
                m += 12;
                --y;
            }
            from = std::to_string(y) + "-" + twoDigits(static_cast<unsigned>(m)) + "-01";
        }
        Snapshot s;
        s.today = std::string(todayDay);
        for (Row& r : rows)
        {
            const std::string first = r.key.day.size() == 7 ? r.key.day + "-01" : r.key.day;
            if (from.empty() || first >= from)
                s.rows.push_back(std::move(r));
        }
        return s;
    }

    // ---- the process's ----------------------------------------------------------------------------------------------------
    namespace
    {
        Ledger& processLedger()
        {
            static Ledger ledger;
            return ledger;
        }
    }

    fs::path processFile()
    {
        return fs::path(chatstore::directory().getChildFile("usage.json").getFullPathName().toStdString());
    }

    void add(const Key& key, const Counts& counts) { processLedger().add(key, counts); }

    bool flush()
    {
        Ledger& l = processLedger();
        if (!l.pending())
            return true;
        l.setFile(processFile());
        return l.flush();
    }

    void requestReset() { processLedger().requestReset(); }

    bool reset()
    {
        Ledger& l = processLedger();
        if (!l.resetOwed())
            return true;
        l.setFile(processFile());
        return l.reset();
    }

    bool resetOwed() { return processLedger().resetOwed(); }
    bool lastResetFailed() { return processLedger().lastResetFailed(); }

    uint64_t revision() noexcept { return processLedger().revision() + pricesChanged.load(std::memory_order_acquire); }

    void setWritesFailForTests(bool fail) { writesFail.store(fail); }

    Snapshot snapshot(Range range)
    {
        Ledger& l = processLedger();
        l.setFile(processFile());
        return l.snapshot(range);
    }
}
