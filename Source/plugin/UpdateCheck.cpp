// Source/plugin/UpdateCheck.cpp: see UpdateCheck.h.
#include "plugin/UpdateCheck.h"

#include "plugin/ClaudeApi.h"
#include "plugin/FactoryStore.h"
#include "plugin/Usage.h"

#include "Product.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <system_error>
#include <thread>
#include <utility>

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

namespace playground::updates
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr std::size_t kMaxErrorBody = 4096;      // what of an error's body is read
        constexpr std::size_t kMaxAppFile = 256 * 1024;  // app.json, as read back
        constexpr std::size_t kMaxSummary = 400;         // bytes of the summary kept (one line)
        constexpr std::size_t kMaxField = 64;            // a version's, a tag's, a date's

        // ---- files: FactoryStore.cpp's writeFile and FolderLock, made private (as Usage.cpp's are) -----------------

        fs::path pathOf(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

        std::optional<std::string> readFile(const fs::path& p, std::size_t limit)
        {
            std::error_code ec;
            const std::uintmax_t size = fs::file_size(p, ec);
            if (ec || size > limit)
                return std::nullopt;
            std::ifstream in(p, std::ios::binary);
            if (!in)
                return std::nullopt;
            std::string out(static_cast<std::size_t>(size), '\0');
            in.read(out.data(), static_cast<std::streamsize>(out.size()));
            if (static_cast<std::size_t>(in.gcount()) != out.size())
                return std::nullopt;
            return out;
        }

        // `bytes` written beside `p` under a name of this write's own, then renamed over it (one step).
        bool writeFile(const fs::path& p, const std::string& bytes)
        {
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
            const std::string tag = juce::Uuid().toString().substring(0, 12).toStdString();
            const fs::path temp = p.parent_path() / pathOf(p.filename().string() + "." + tag + ".tmp");
            bool written = false;
            {
                std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                out.close();
                written = static_cast<bool>(out);
            }
            if (written)
                fs::rename(temp, p, ec);
            if (!written || ec)
            {
                std::error_code e;
                fs::remove(temp, e);
                return false;
            }
            return true;
        }

        // The folder's lock (.lock, the factory store's and the ledger's too): flock on its own open file, on Windows an
        // open that shares nothing; tried every few milliseconds for a few seconds at most, or until `cancel`.
        class FolderLock
        {
        public:
            FolderLock(const fs::path& dir, const std::atomic<bool>& cancel)
            {
                std::error_code ec;
                fs::create_directories(dir, ec);
                const fs::path p = dir / ".lock";
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (!cancel.load() && std::chrono::steady_clock::now() < until)
                {
#if defined(_WIN32)
                    int fd = -1;
                    if (_wsopen_s(&fd, p.c_str(), _O_RDWR | _O_CREAT | _O_BINARY, _SH_DENYRW, _S_IREAD | _S_IWRITE) == 0 && fd >= 0)
                    {
                        fd_ = fd;
                        return;
                    }
#else
                    const int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
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

        // ---- JSON ------------------------------------------------------------------------------------------------------

        bool parseJson(const std::string& text, juce::var& out)
        {
            if (text.size() > 0x7fffffff || !juce::CharPointer_UTF8::isValidString(text.data(), static_cast<int>(text.size())))
                return false;
            return juce::JSON::parse(juce::String::fromUTF8(text.data(), static_cast<int>(text.size())), out).wasOk();
        }

        std::string str(const juce::var& v) { return v.isString() ? v.toString().toStdString() : std::string(); }

        bool numberOf(const juce::var& v, double& out)
        {
            if (v.isInt() || v.isInt64() || v.isDouble())
            {
                out = static_cast<double>(v);
                return std::isfinite(out);
            }
            return false;
        }

        // `s` cut to at most `n` bytes where a character starts.
        std::string cut(std::string s, std::size_t n)
        {
            if (s.size() <= n)
                return s;
            while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
                --n;
            s.resize(n);
            return s;
        }

        // One line: line breaks and tabs made spaces, runs of spaces one, the ends trimmed, cut.
        std::string oneLine(const std::string& s, std::size_t n)
        {
            std::string out;
            for (const char ch : s)
            {
                const char c = (ch == '\n' || ch == '\r' || ch == '\t') ? ' ' : ch;
                if (static_cast<unsigned char>(c) < 0x20)
                    continue;
                if (c == ' ' && (out.empty() || out.back() == ' '))
                    continue;
                out += c;
            }
            while (!out.empty() && out.back() == ' ')
                out.pop_back();
            return cut(std::move(out), n);
        }

        const char* answerWord(Answer a)
        {
            switch (a)
            {
                case Answer::yes: return "yes";
                case Answer::no: return "no";
                case Answer::later: return "later";
                case Answer::notAsked: break;
            }
            return "";
        }

        Answer answerOf(const juce::var& v)
        {
            const std::string w = str(v);
            return w == "yes" ? Answer::yes : w == "no" ? Answer::no : w == "later" ? Answer::later : Answer::notAsked;
        }

        juce::var releaseVar(const Release& r)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("version", juce::String::fromUTF8(r.version.c_str()));
            o->setProperty("tag", juce::String::fromUTF8(r.tag.c_str()));
            o->setProperty("date", juce::String::fromUTF8(r.date.c_str()));
            o->setProperty("summary", juce::String::fromUTF8(r.summary.c_str()));
            return juce::var(o);
        }

        Release releaseOf(const juce::var& v)
        {
            Release r;
            if (!v.isObject())
                return r;
            r.version = cut(str(v["version"]), kMaxField);
            if (!validVersion(r.version))
                return {};
            r.tag = oneLine(str(v["tag"]), kMaxField);
            r.date = oneLine(str(v["date"]), kMaxField);
            r.summary = oneLine(str(v["summary"]), kMaxSummary);
            return r;
        }

        // ---- versions --------------------------------------------------------------------------------------------------

        struct Version
        {
            long long n[3]{};
            std::string pre;                             // after the '-' ("" a release)
        };

        bool versionParts(const std::string& s, Version& v)
        {
            std::size_t at = 0;
            for (int part = 0; part < 3; ++part)
            {
                const std::size_t start = at;
                long long n = 0;
                while (at < s.size() && s[at] >= '0' && s[at] <= '9' && at - start < 6)
                    n = n * 10 + (s[at++] - '0');
                if (at == start)
                    return false;
                v.n[part] = n;
                if (part < 2)
                {
                    if (at >= s.size() || s[at] != '.')
                        return false;
                    ++at;
                }
            }
            if (at == s.size())
                return true;
            if (s[at] != '-' || at + 1 >= s.size())
                return false;
            v.pre = s.substr(at + 1);
            return std::all_of(v.pre.begin(), v.pre.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-';
            });
        }

        // The status line's announcements, once a process (whichever Check the processors hold then).
        std::mutex announcedMutex;
        std::set<std::string> announced;

        std::mutex processMutex;
        std::weak_ptr<Check> theCheck;
#if defined(FP_PROBES)
        std::optional<Config> testConfig;
#endif
    }

    bool validVersion(const std::string& v)
    {
        Version x;
        return v.size() <= kMaxField && versionParts(v, x);
    }

    int compareVersions(const std::string& a, const std::string& b)
    {
        Version x, y;
        const bool okA = a.size() <= kMaxField && versionParts(a, x);
        const bool okB = b.size() <= kMaxField && versionParts(b, y);
        if (!okA || !okB)
            return okA == okB ? 0 : okA ? 1 : -1;
        for (int i = 0; i < 3; ++i)
            if (x.n[i] != y.n[i])
                return x.n[i] < y.n[i] ? -1 : 1;
        if (x.pre == y.pre)
            return 0;
        if (x.pre.empty() || y.pre.empty())
            return x.pre.empty() ? 1 : -1;           // a pre-release sorts below its release
        return x.pre < y.pre ? -1 : 1;
    }

    bool offFrom(const char* environment)
    {
        return environment != nullptr && juce::String(environment).trim().equalsIgnoreCase("off");
    }

    Parsed parse(const std::string& text)
    {
        Parsed p;
        if (text.size() > kMaxBytes)
        {
            p.why = "the reply is larger than " + std::to_string(kMaxBytes / 1024) + " KB";
            return p;
        }
        juce::var v;
        if (!parseJson(text, v) || !v.isObject())
        {
            p.why = "the reply does not read as a release file";
            return p;
        }
        if (!v["format"].isString() || v["format"].toString() != kFormat)
        {
            p.why = "the reply is not a release file";
            return p;
        }
        double version = 0;
        if (!numberOf(v["version"], version) || version != kFormatVersion)
        {
            p.why = "the reply's format is a version this plugin does not read";
            return p;
        }
        p.latest = releaseOf(v["latest"]);
        if (p.latest.version.empty())
        {
            p.why = "the reply names no version";
            return p;
        }
        if (const juce::var& prices = v["prices"]; prices.isObject())
        {
            p.pricesJson = juce::JSON::toString(prices, true).toStdString();
            p.pricesAsOf = oneLine(str(prices["asOf"]), kMaxField);
        }
        p.ok = true;
        return p;
    }

    bool offerPricesJson(const std::string& pricesJson)
    {
        juce::var v;
        if (pricesJson.empty() || !parseJson(pricesJson, v) || !v.isObject())
            return false;
        usage::PriceTable t;
        t.asOf = oneLine(str(v["asOf"]), kMaxField);
        if (t.asOf.empty())
            return false;
        double perThousand = 0;
        const double search = numberOf(v["webSearchPer1000"], perThousand) && perThousand >= 0 ? perThousand / 1000.0 : 0.0;
        const juce::DynamicObject* providers = v["usdPerMTok"].getDynamicObject();
        if (providers == nullptr)
            return false;
        for (const auto& provider : providers->getProperties())
        {
            const juce::DynamicObject* models = provider.value.getDynamicObject();
            if (models == nullptr)
                continue;
            const std::string name = provider.name.toString().toStdString();
            for (const auto& model : models->getProperties())
            {
                const juce::var& m = model.value;
                usage::Price price;
                if (!m.isObject() || !numberOf(m["in"], price.in) || !numberOf(m["out"], price.out) || price.in < 0 || price.out < 0)
                    continue;                            // a row the table cannot take
                const auto field = [&](const char* key, double fallback) {
                    double d = 0;
                    return numberOf(m[key], d) && d >= 0 ? d : fallback;
                };
                price.cacheRead = field("cacheRead", price.in * 0.1);
                price.write5m = field("cacheWrite5m", price.in * 1.25);
                price.write1h = field("cacheWrite1h", price.in * 2.0);
                price.fastIn = field("fastIn", 0.0);
                price.fastOut = field("fastOut", 0.0);
                price.webSearch = name == "anthropic" ? search : 0.0;   // the web search tool is Anthropic's
                t.rows.push_back({ name, model.name.toString().toStdString(), price });
            }
        }
        if (t.rows.empty())
            return false;
        return usage::offerPrices(std::move(t));
    }

    Config processConfig()
    {
        Config c;
        c.file = factorystore::directory().getChildFile("app.json").getFullPathName().toStdString();
        c.version = product::kVersion;
        c.site = factorystore::siteFrom(std::getenv((std::string(product::kEnvPrefix) + "FACTORY_SITE").c_str()));
        c.transport = claude::httpsTransport();
        c.now = [] { return static_cast<int64_t>(juce::Time::currentTimeMillis()); };
        c.off = offFrom(std::getenv((std::string(product::kEnvPrefix) + "UPDATES").c_str()));
        c.userAgent = std::string(product::kName) + "/" + product::kVersion;
        return c;
    }

    // ---- the check ---------------------------------------------------------------------------------------------------

    struct Check::Impl
    {
        Config config;
        fs::path file;

        mutable std::mutex m;
        std::condition_variable cv;
        bool stop = false;
        bool loadWanted = false, checkWanted = false, forced = false, writeWanted = false;
        bool answerDirty = false, skippedDirty = false, resultDirty = false, seenDirty = false;
        bool busy = false;
        bool editorSeen = false;
        bool checking = false;
        Answer answer = Answer::notAsked;
        bool loaded = false;
        std::string skipped;
        std::string seen;                        // What's new's: the version whose card the user last saw
        bool seenFresh = false;                  // seen read again since the last editor opened (rereadSeen)
        bool seenWanted = false;
        Release latest;
        int64_t checkedMs = 0, triedMs = 0;
        std::string why;
        std::string pricesJson;
        std::atomic<bool> cancel{ false };
        std::atomic<int> requests{ 0 };
        std::atomic<uint64_t> revision{ 1 };
        std::thread thread;

        explicit Impl(Config c) : config(std::move(c))
        {
            file = pathOf(config.file);
            if (!config.now)
                config.now = [] { return static_cast<int64_t>(juce::Time::currentTimeMillis()); };
        }

        void bump() { revision.fetch_add(1); }

        // ---- app.json --------------------------------------------------------------------------------------------------

        juce::var readApp() const
        {
            juce::var v;
            if (const std::optional<std::string> t = readFile(file, kMaxAppFile); t && parseJson(*t, v) && v.isObject())
                return v;
            return {};
        }

        void load()
        {
            const juce::var v = readApp();
            std::string prices;
            {
                const std::lock_guard<std::mutex> lock(m);
                if (v.isObject())
                {
                    if (!answerDirty)
                        answer = answerOf(v["updates"]);
                    if (!skippedDirty)
                        skipped = validVersion(str(v["skipped"])) ? str(v["skipped"]) : std::string();
                    if (!seenDirty)
                        seen = validVersion(str(v["seen"])) ? str(v["seen"]) : std::string();
                    if (!resultDirty)
                    {
                        double ms = 0;
                        checkedMs = numberOf(v["checkedMs"], ms) && ms > 0 ? static_cast<int64_t>(ms) : 0;
                        latest = releaseOf(v["latest"]);
                        pricesJson = v["prices"].isObject() ? juce::JSON::toString(v["prices"], true).toStdString() : std::string();
                        prices = pricesJson;
                    }
                }
                loaded = true;
            }
            if (config.offersPrices && !prices.empty())
                (void)offerPricesJson(prices);
            bump();
        }

        // The answer as app.json has it now (the other product may have answered meanwhile), unless this one's waits to
        // be written.
        void rereadAnswer()
        {
            const juce::var v = readApp();
            const std::lock_guard<std::mutex> lock(m);
            if (answerDirty || !v.isObject())
                return;
            const Answer onDisk = answerOf(v["updates"]);
            if (onDisk != answer)
            {
                answer = onDisk;
                bump();
            }
        }

        // What's new's seen as app.json has it now (the other product's editor may have shown the card meanwhile),
        // unless this one's waits to be written; then fresh, for the editor that opened.
        void rereadSeen()
        {
            const juce::var v = readApp();
            {
                const std::lock_guard<std::mutex> lock(m);
                if (!seenDirty && v.isObject())
                    seen = validVersion(str(v["seen"])) ? str(v["seen"]) : std::string();
                seenFresh = true;
            }
            bump();
        }

        // The fields this process changed, set on app.json as it is now (read again under the folder's lock): what the
        // other product (or another process) wrote meanwhile, and every field this one does not know, are kept.
        void writeApp()
        {
            bool a = false, s = false, r = false, w = false;
            Answer ans;
            std::string skip, prices, seenNow;
            Release rel;
            int64_t checked = 0;
            {
                const std::lock_guard<std::mutex> lock(m);
                a = answerDirty;
                s = skippedDirty;
                r = resultDirty;
                w = seenDirty;
                ans = answer;
                skip = skipped;
                seenNow = seen;
                rel = latest;
                checked = checkedMs;
                prices = pricesJson;
            }
            if (!a && !s && !r && !w)
                return;
            const std::atomic<bool> never{ false };
            const FolderLock lock(file.parent_path(), never);
            if (!lock.held())
                return;                                  // tried again with the next write
            juce::var v = readApp();
            if (!v.isObject())
                v = juce::var(new juce::DynamicObject());
            juce::DynamicObject* o = v.getDynamicObject();
            if (a)
            {
                if (ans == Answer::notAsked)
                    o->removeProperty("updates");
                else
                    o->setProperty("updates", answerWord(ans));
            }
            if (s)
                o->setProperty("skipped", juce::String::fromUTF8(skip.c_str()));
            if (w)
                o->setProperty("seen", juce::String::fromUTF8(seenNow.c_str()));
            if (r)
            {
                o->setProperty("checkedMs", juce::var(static_cast<juce::int64>(checked)));
                if (!rel.version.empty())
                    o->setProperty("latest", releaseVar(rel));
                juce::var pv;
                if (!prices.empty() && parseJson(prices, pv) && pv.isObject())
                    o->setProperty("prices", pv);
            }
            const std::string text = juce::JSON::toString(v).toStdString() + "\n";
            if (!writeFile(file, text))
                return;
            const std::lock_guard<std::mutex> l(m);
            if (a && answer == ans)
                answerDirty = false;
            if (s && skipped == skip)
                skippedDirty = false;
            if (w && seen == seenNow)
                seenDirty = false;
            if (r && checkedMs == checked)
                resultDirty = false;
        }

        // ---- checking --------------------------------------------------------------------------------------------------

        void fail(std::string reason)
        {
            {
                const std::lock_guard<std::mutex> lock(m);
                why = std::move(reason);
                checking = false;
            }
            bump();
        }

        void fetch()
        {
            const int64_t now = config.now();
            {
                const std::lock_guard<std::mutex> lock(m);
                checking = true;
                triedMs = now;
            }
            bump();
            std::string body;
            const claude::Headers headers = { { "User-Agent", config.userAgent } };
            const auto onChunk = [&](std::string_view chunk) {
                const std::size_t room = kMaxBytes + 1 - std::min(body.size(), kMaxBytes + 1);
                body.append(chunk.data(), std::min(chunk.size(), room));
                return body.size() <= kMaxBytes;         // one byte past the limit: no more is read
            };
            requests.fetch_add(1);
            if (config.transport == nullptr)
                return fail("no connection to " + claude::hostName(config.site));
            const claude::HttpResult res = config.transport->getLimited(config.site, kReleasePath, headers, onChunk, kMaxErrorBody, cancel);
            if (cancel.load())
            {
                {
                    const std::lock_guard<std::mutex> lock(m);
                    checking = false;
                }
                bump();
                return;
            }
            if (res.status == 0)
                return fail("no connection to " + claude::hostName(config.site));
            if (res.status >= 300 && res.status < 400)
                return fail("the site sent the request elsewhere (" + std::to_string(res.status) + "), and the plugin follows no redirect");
            if (res.status != 200)
                return fail("the site answered " + std::to_string(res.status));
            const Parsed p = parse(body);
            if (!p.ok)
                return fail(p.why);
            {
                const std::lock_guard<std::mutex> lock(m);
                latest = p.latest;
                checkedMs = now;
                pricesJson = p.pricesJson.empty() ? pricesJson : p.pricesJson;
                why.clear();
                checking = false;
                resultDirty = true;
            }
            if (config.offersPrices && !p.pricesJson.empty())
                (void)offerPricesJson(p.pricesJson);
            bump();
            writeApp();
        }

        void considerCheck(bool force)
        {
            rereadAnswer();
            bool due = false;
            {
                const std::lock_guard<std::mutex> lock(m);
                if (config.off || answer != Answer::yes)
                    return;
                if (force)
                    due = true;
                else if (editorSeen)
                {
                    const int64_t now = config.now();
                    const int64_t last = std::max(checkedMs, triedMs);
                    due = last <= 0 || now - last >= kRecheckMs || last - now >= kRecheckMs;   // a clock set back a day too
                }
            }
            if (due)
                fetch();
        }

        void run()
        {
            for (;;)
            {
                bool doLoad = false, doCheck = false, doForce = false, doSeen = false;
                {
                    std::unique_lock<std::mutex> lock(m);
                    cv.wait(lock, [&] { return stop || loadWanted || checkWanted || writeWanted || seenWanted; });
                    if (stop)
                        return;
                    doLoad = std::exchange(loadWanted, false);
                    doCheck = std::exchange(checkWanted, false);
                    doForce = std::exchange(forced, false);
                    doSeen = std::exchange(seenWanted, false);
                    writeWanted = false;
                    busy = true;
                }
                // Whatever the work throws ends that work, said in words as a failed check: the thread goes on.
                try
                {
                    bool wasLoaded = false;
                    {
                        const std::lock_guard<std::mutex> lock(m);
                        wasLoaded = loaded;
                    }
                    if (doLoad || !wasLoaded)
                        load();
                    writeApp();
                    if (doSeen)
                        rereadSeen();
                    if (doCheck && !cancel.load())
                        considerCheck(doForce);
                }
                catch (const std::exception& e)
                {
                    fail(std::string("the check stopped: ") + e.what());
                }
                catch (...)
                {
                    fail("the check stopped on an error");
                }
                // A write that failed (the lock not taken, the disk) leaves its fields dirty: they go with the next
                // request's write, never retried in a loop here.
                const std::lock_guard<std::mutex> lock(m);
                busy = false;
                cancel.store(false);
                cv.notify_all();
            }
        }
    };

    Check::Check(Config config) : s_(std::make_unique<Impl>(std::move(config)))
    {
        s_->thread = std::thread([impl = s_.get()] {
            try
            {
                impl->run();
            }
            catch (...)
            {
                // run() catches its work's throws itself; nothing else in it throws but the standard library's own
                // failures (a lock): the thread ends, and the destructor joins it.
            }
        });
    }

    Check::~Check()
    {
        {
            const std::lock_guard<std::mutex> lock(s_->m);
            s_->stop = true;
            s_->cancel.store(true);
        }
        s_->cv.notify_all();
        if (s_->thread.joinable())
            s_->thread.join();
    }

    void Check::start()
    {
        {
            const std::lock_guard<std::mutex> lock(s_->m);
            s_->loadWanted = true;
        }
        s_->cv.notify_all();
    }

    State Check::state() const
    {
        State st;
        const std::lock_guard<std::mutex> lock(s_->m);
        st.answer = s_->answer;
        st.loaded = s_->loaded;
        st.envOff = s_->config.off;
        st.latest = s_->latest;
        st.current = s_->config.version;
        st.newer = !s_->latest.version.empty() && compareVersions(s_->latest.version, s_->config.version) > 0;
        st.skipped = s_->skipped;
        st.seen = s_->seen;
        st.seenFresh = s_->seenFresh;
        st.checkedMs = s_->checkedMs;
        st.why = s_->why;
        st.revision = s_->revision.load();
        if (s_->config.off)
            st.status = Status::off;
        else if (s_->answer == Answer::notAsked)
            st.status = Status::unanswered;
        else if (s_->answer != Answer::yes)
            st.status = Status::off;
        else if (s_->checking)
            st.status = Status::checking;
        else if (!s_->why.empty())
            st.status = Status::failed;
        else
            st.status = Status::idle;
        return st;
    }

    uint64_t Check::revision() const noexcept { return s_->revision.load(); }

    Answer Check::answer() const
    {
        const std::lock_guard<std::mutex> lock(s_->m);
        return s_->answer;
    }

    void Check::setAnswer(Answer a)
    {
        {
            const std::lock_guard<std::mutex> lock(s_->m);
            s_->answer = a;
            s_->answerDirty = true;
            s_->writeWanted = true;
            if (a == Answer::yes)
                s_->checkWanted = true;                  // when an editor has opened and the last is a day old
            if (a != Answer::yes)
                s_->why.clear();
        }
        s_->bump();
        s_->cv.notify_all();
    }

    void Check::skip(const std::string& version)
    {
        {
            const std::lock_guard<std::mutex> lock(s_->m);
            s_->skipped = validVersion(version) ? version : std::string();
            s_->skippedDirty = true;
            s_->writeWanted = true;
        }
        s_->bump();
        s_->cv.notify_all();
    }

    void Check::setSeen(const std::string& version)
    {
        {
            const std::lock_guard<std::mutex> lock(s_->m);
            if (!validVersion(version) || (s_->seen == version && !s_->seenDirty))
                return;                                  // nothing to write
            s_->seen = version;
            s_->seenDirty = true;
            s_->writeWanted = true;
        }
        s_->bump();
        s_->cv.notify_all();
    }

    void Check::editorOpened()
    {
        {
            const std::lock_guard<std::mutex> lock(s_->m);
            s_->editorSeen = true;
            s_->checkWanted = true;
            s_->seenWanted = true;                       // What's new decides on app.json's seen as it is now
            s_->seenFresh = false;
        }
        s_->cv.notify_all();
    }

    void Check::checkNow()
    {
        {
            const std::lock_guard<std::mutex> lock(s_->m);
            s_->checkWanted = true;
            s_->forced = true;
        }
        s_->cv.notify_all();
    }

    bool Check::announce(const std::string& version)
    {
        const std::lock_guard<std::mutex> lock(announcedMutex);
        return announced.insert(version).second;
    }

    bool Check::idle() const
    {
        const std::lock_guard<std::mutex> lock(s_->m);
        return !s_->busy && !s_->loadWanted && !s_->checkWanted && !s_->writeWanted && !s_->seenWanted;
    }

    int Check::requests() const noexcept { return s_->requests.load(); }

    void Check::cancel()
    {
        const std::lock_guard<std::mutex> lock(s_->m);
        if (s_->busy)
            s_->cancel.store(true);
    }

    // ---- the process's -------------------------------------------------------------------------------------------------

    std::shared_ptr<Check> attach()
    {
        const std::lock_guard<std::mutex> lock(processMutex);
        std::shared_ptr<Check> c = theCheck.lock();
        if (!c)
        {
#if defined(FP_PROBES)
            c = std::make_shared<Check>(testConfig ? *testConfig : processConfig());
#else
            c = std::make_shared<Check>(processConfig());
#endif
            c->start();
            theCheck = c;
        }
        return c;
    }

    std::shared_ptr<Check> process()
    {
        const std::lock_guard<std::mutex> lock(processMutex);
        return theCheck.lock();
    }

#if defined(FP_PROBES)
    void useConfigForTests(const Config* config)
    {
        const std::lock_guard<std::mutex> lock(processMutex);
        if (config != nullptr)
            testConfig = *config;
        else
            testConfig.reset();
    }
#endif
}
