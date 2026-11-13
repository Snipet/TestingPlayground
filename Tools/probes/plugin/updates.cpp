// PROBE name=plugin.updates timeout=180
//
// plugin.updates: the update check (plugin/UpdateCheck.h) and the processor's side of the website (plugin/
// ProcessorUpdates.cpp: EditorPort's "The website"), over a scripted transport and clock of this probe's own: nothing
// here reaches the network, opens a browser or touches the user's folders (app.json and factory.json in a folder of the
// probe's own, through factorystore::setDirectoryForTests and the checks' own configurations).
//   GATE      nothing is asked of the site with no editor opened (a check answered yes, and a processor made and left
//             with no editor), nor with the answer not given, no or later, nor with Config::off, nor with
//             <ENV_PREFIX>UPDATES=off (processConfig reads it: offFrom)
//   FIRST     an editor's open with a yes asks once: the release file's path at the site, a User-Agent of the plugin's
//             name and version and no other header; the state idle with the latest version, newer; app.json written
//   VERSIONS  the order: newer, the same, older, a pre-release build below its release, 0.10.0 above 0.9.0, a text that
//             is no version; a check whose build is the latest says not newer, one above it neither
//   DAY       an editor's open again asks nothing before a day has passed, and asks once after; Check now asks whenever
//   REFUSE    a redirect (not followed: one request, its location never asked), a body past 64 KB (read no further than
//             a little past it), one that does not parse, another format, another format version, no version: each
//             failed with why in words, the last good answer kept; then a good one clears the failure
//   OFFLINE   no connection: failed, "no connection", tried again a day later, not before
//   EXTRA     fields the plugin does not know (at the top, in latest, in prices) are ignored; a summary on several lines
//             is one line
//   SKIP      a skipped version is kept in app.json and the processor's newer is false for it; a later one shows again
//   LOAD      a check made over an app.json it wrote reads its answer, latest, checkedMs and skipped, and asks nothing
//   LOCK      two checks writing one app.json at once (one the answer, the other skipped), with a field neither knows
//             (What's new's "seen"): neither's write is lost, the unknown field kept
//   PRICES    a good reply's prices handed to the usage ledger (usage::pricesInUse), its rows as the table takes them
//             (the cache's defaults, a row without a price left out); an older table not taken
//   LEDGER    end to end, the check to the Usage page (package 09's refresh): a ledger seeded with a model's requests,
//             the page's view (ChatPort::usage, its periodic reread put off for good) priced at the built-in table and
//             its date; a check whose reply carries a later asOf and another price for that model: the view read again
//             at once (taking a table moves usage::revision) with the new date and the row's estimate at the new
//             price; a reply with an earlier asOf changes neither, and asks for no read
//   HELD      a store's file renamed over while another process reads it (plugin/FileReplace.h): a reader holding the
//             file open (a std::ifstream, as every reader of these files) through the write and letting go within the
//             helper's window, and the write lands: the helper itself, usage.json's flush and Reset (usage::Ledger),
//             app.json (an answer) and factory.json (an answer); a reader that never lets go: the write fails as before,
//             nothing lost (the ledger's counts kept and said failed, app.json's answer written by the next write). On
//             POSIX a rename over an open file always lands, so the never-lets-go rows are Windows' alone (skipped,
//             saying so); the others pass there by nature and are Windows' test
//   TWO       two processors share one check: one request for two editors' opens
//   JOIN      a processor (the last holder) going while a check is in flight returns promptly, the thread joined
//   OFFER     the processor's offer: due with neither answered; Allow with the factory unticked: updates yes, the factory
//             no (factory.json); Not now: updates later, the factory unasked, the offer not due again
//   DOWNLOAD  openDownloadPage gives the site's fixed /download to the processor's hook, and opens nothing under a probe
//             without one
#include "Probe.h"

#include "plugin/ChatAgent.h"
#include "plugin/ChatPort.h"
#include "plugin/ChatStore.h"
#include "plugin/ClaudeApi.h"
#include "plugin/EditorPort.h"
#include "plugin/FactoryStore.h"
#include "plugin/FileReplace.h"
#include "plugin/Processor.h"
#include "plugin/UpdateCheck.h"
#include "plugin/Usage.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace pg = playground;
namespace up = pg::updates;

namespace
{
    constexpr const char* kSite = "https://fixture.test";

    std::string release(const std::string& version, const std::string& extra = {}, const std::string& prices = {})
    {
        return "{\"format\":\"funk-playground-release\",\"version\":1,\"latest\":{\"version\":\"" + version
             + "\",\"tag\":\"v" + version + "\",\"date\":\"2026-11-20\",\"summary\":\"One line about it.\"}"
             + (prices.empty() ? std::string() : ",\"prices\":" + prices) + extra + "}";
    }

    // The site: one release file, as the website serves it (or a failure), and what was asked of it.
    class Site final : public pg::claude::Transport
    {
    public:
        void serve(std::string body, int status = 200)
        {
            const std::lock_guard<std::mutex> lock(m_);
            body_ = std::move(body);
            status_ = status;
        }
        void setOffline(bool on) { offline_.store(on); }
        void setStall(bool on) { stall_.store(on); }
        int count() const
        {
            const std::lock_guard<std::mutex> lock(m_);
            return static_cast<int>(paths_.size());
        }
        std::vector<std::string> paths() const
        {
            const std::lock_guard<std::mutex> lock(m_);
            return paths_;
        }
        pg::claude::Headers lastHeaders() const
        {
            const std::lock_guard<std::mutex> lock(m_);
            return headers_;
        }
        std::string lastHost() const
        {
            const std::lock_guard<std::mutex> lock(m_);
            return host_;
        }
        std::size_t delivered() const { return delivered_.load(); }
        int stalling() const { return stalling_.load(); }

        pg::claude::HttpResult post(const std::string&, const std::string&, const std::string&, const pg::claude::Headers&,
                                    const std::function<bool(std::string_view)>&, const std::atomic<bool>&) override
        {
            return { 500, {}, "no POST here", -1, {} };
        }
        pg::claude::HttpResult get(const std::string&, const std::string&, const pg::claude::Headers&, const std::atomic<bool>&) override
        {
            return { 500, {}, "the check asks with getLimited", -1, {} };
        }
        pg::claude::HttpResult getLimited(const std::string& host, const std::string& path, const pg::claude::Headers& headers,
                                          const std::function<bool(std::string_view)>& onChunk, std::size_t,
                                          const std::atomic<bool>& cancel) override
        {
            std::string body;
            int status = 200;
            {
                const std::lock_guard<std::mutex> lock(m_);
                paths_.push_back(path);
                headers_ = headers;
                host_ = host;
                body = body_;
                status = status_;
            }
            if (offline_.load())
                return { 0, "no connection", {}, -1, {} };
            if (stall_.load())
            {
                ++stalling_;
                while (!cancel.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                --stalling_;
                return { 0, "stopped", {}, -1, {} };
            }
            if (status >= 300 && status < 400)
                return { status, {}, "moved", -1, "https://elsewhere.test/app/v1/release.json" };
            if (status != 200)
                return { status, {}, "error", -1, {} };
            delivered_.store(0);
            constexpr std::size_t kChunk = 1024;
            for (std::size_t at = 0; at < body.size(); at += kChunk)
            {
                if (cancel.load())
                    return { 0, "stopped", {}, -1, {} };
                const std::size_t n = std::min(kChunk, body.size() - at);
                delivered_ += n;
                if (!onChunk(std::string_view(body).substr(at, n)))
                    break;
            }
            return { 200, {}, {}, -1, {} };
        }

    private:
        mutable std::mutex m_;
        std::string body_;
        int status_ = 200;
        std::atomic<bool> offline_{ false }, stall_{ false };
        std::vector<std::string> paths_;
        pg::claude::Headers headers_;
        std::string host_;
        std::atomic<std::size_t> delivered_{ 0 };
        std::atomic<int> stalling_{ 0 };
    };

    std::atomic<int64_t> clockMs{ 1'800'000'000'000 };

    up::Config configOf(const juce::File& home, std::shared_ptr<Site> site, std::string version = "0.1.0")
    {
        up::Config c;
        c.file = home.getChildFile("app.json").getFullPathName().toStdString();
        c.version = std::move(version);
        c.site = kSite;
        c.transport = std::move(site);
        c.now = [] { return clockMs.load(); };
        c.off = false;
        c.userAgent = std::string(pg::product::kName) + "/" + pg::product::kVersion;
        c.offersPrices = false;
        return c;
    }

    bool settle(const up::Check& c)
    {
        const auto until = std::chrono::steady_clock::now() + probe::kWorkWait;
        while (std::chrono::steady_clock::now() < until)
        {
            if (c.idle())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    std::unique_ptr<up::Check> checkOf(const juce::File& home, std::shared_ptr<Site> site, up::Answer answer, std::string version = "0.1.0")
    {
        auto c = std::make_unique<up::Check>(configOf(home, std::move(site), std::move(version)));
        c->start();
        if (answer != up::Answer::notAsked)
            c->setAnswer(answer);
        (void)settle(*c);
        return c;
    }

    juce::var appJson(const juce::File& home) { return juce::JSON::parse(home.getChildFile("app.json").loadFileAsString()); }

    class EnvScope
    {
    public:
        explicit EnvScope(std::string name) : name_(std::move(name))
        {
            if (const char* v = std::getenv(name_.c_str()))
                old_ = std::string(v);
        }
        ~EnvScope()
        {
            if (old_)
                ::setenv(name_.c_str(), old_->c_str(), 1);
            else
                ::unsetenv(name_.c_str());
        }
        EnvScope(const EnvScope&) = delete;
        EnvScope& operator=(const EnvScope&) = delete;
        void set(const char* v) const { ::setenv(name_.c_str(), v, 1); }
        void unset() const { ::unsetenv(name_.c_str()); }

    private:
        std::string name_;
        std::optional<std::string> old_;
    };

    // A reader of `path` as another process's would be: a std::ifstream (the C runtime's share mode on Windows, which
    // shares reading and writing, not deletion) held open for `holdMs` on a thread of its own, from open() on.
    class Reader
    {
    public:
        explicit Reader(std::filesystem::path path) : path_(std::move(path)) {}
        ~Reader() { close(); }
        bool open(int holdMs)
        {
            thread_ = std::thread([this, holdMs] {
                std::ifstream in(path_, std::ios::binary);
                opened_.store(static_cast<bool>(in) ? 1 : -1);
                std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
            });
            while (opened_.load() == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return opened_.load() > 0;
        }
        void close()
        {
            if (thread_.joinable())
                thread_.join();
        }

    private:
        std::filesystem::path path_;
        std::thread thread_;
        std::atomic<int> opened_{ 0 };
    };

    std::string textOf(const std::filesystem::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    // Polls `done` until it holds, kWorkWait at most.
    template <typename F>
    bool waitFor(F done)
    {
        const auto until = std::chrono::steady_clock::now() + probe::kWorkWait;
        while (std::chrono::steady_clock::now() < until)
        {
            if (done())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return done();
    }
}

PROBE(plugin, updates)
{
    const funkgui::HeadlessGuiScope gui;
    const juce::File base = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getChildFile(juce::String(pg::product::kEnvPrefix) + "updates-probe-"
                                              + juce::String(juce::Time::currentTimeMillis()));
    base.deleteRecursively();
    int homes = 0;
    const auto freshHome = [&] {
        const juce::File h = base.getChildFile("home-" + juce::String(++homes));
        (void)h.createDirectory();
        return h;
    };

    // ---- GATE ----------------------------------------------------------------------------------------------------------
    {
        const juce::File home = freshHome();
        auto site = std::make_shared<Site>();
        site->serve(release("0.2.0"));
        {
            const auto c = checkOf(home, site, up::Answer::yes);
            (void)settle(*c);
            P.eq("gate.yes_no_editor.nothing_asked", site->count(), 0);
            P.eq("gate.yes_no_editor.idle", c->state().status == up::Status::idle && c->state().checkedMs == 0 ? 1 : 0, 1);
        }
        for (const auto& [label, answer] : { std::pair{ "not_asked", up::Answer::notAsked }, std::pair{ "no", up::Answer::no },
                                             std::pair{ "later", up::Answer::later } })
        {
            const juce::File h = freshHome();
            const auto c = checkOf(h, site, answer);
            c->editorOpened();
            c->checkNow();
            (void)settle(*c);
            const up::Status want = answer == up::Answer::notAsked ? up::Status::unanswered : up::Status::off;
            P.eq(std::string("gate.") + label + ".nothing_asked", site->count() == 0 && c->state().status == want ? 1 : 0, 1);
        }
        {
            up::Config cfg = configOf(freshHome(), site);
            cfg.off = true;
            up::Check c(cfg);
            c.start();
            c.setAnswer(up::Answer::yes);
            c.editorOpened();
            c.checkNow();
            (void)settle(c);
            P.eq("gate.off.nothing_asked", site->count() == 0 && c.state().status == up::Status::off && c.state().envOff ? 1 : 0, 1);
        }
        {
            const EnvScope env(std::string(pg::product::kEnvPrefix) + "UPDATES");
            env.set("off");
            const bool off = up::processConfig().off;
            env.set(" OFF ");
            const bool offCase = up::processConfig().off;
            env.set("on");
            const bool on = !up::processConfig().off;
            env.unset();
            const bool unset = !up::processConfig().off;
            P.eq("gate.env_off", off && offCase && on && unset ? 1 : 0, 1);
            P.eq("gate.env_words", up::offFrom("off") && up::offFrom("Off") && !up::offFrom("") && !up::offFrom(nullptr) && !up::offFrom("0") ? 1 : 0, 1);
        }
        {
            // A processor made and left with no editor (a host scanning plugins): its check reads app.json and asks
            // nothing, a yes in app.json notwithstanding.
            const juce::File h = freshHome();
            h.getChildFile("app.json").replaceWithText("{\"updates\":\"yes\"}\n");
            const up::Config cfg = configOf(h, site);
            up::useConfigForTests(&cfg);
            pg::factorystore::setDirectoryForTests(h);
            {
                pg::Processor a;
                const std::shared_ptr<up::Check> c = up::process();
                P.eq("gate.processor_attached", c != nullptr ? 1 : 0, 1);
                if (c != nullptr)
                    (void)settle(*c);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                P.eq("gate.processor_no_editor.nothing_asked", site->count(), 0);
                P.eq("gate.processor_read_the_yes", c != nullptr && c->answer() == up::Answer::yes ? 1 : 0, 1);
                // The editor opens: now, and only now, the one request.
                a.editorPort().editorOpened();
                if (c != nullptr)
                    (void)waitFor([&] { return site->count() >= 1 && c->idle(); });
                P.eq("gate.processor_editor.asked_once", site->count(), 1);
            }
            P.eq("gate.processor_gone.released", up::process() == nullptr ? 1 : 0, 1);
            up::useConfigForTests(nullptr);
            pg::factorystore::setDirectoryForTests(juce::File());
        }
    }

    // ---- FIRST ---------------------------------------------------------------------------------------------------------
    const juce::File home = freshHome();
    auto site = std::make_shared<Site>();
    site->serve(release("0.2.0"));
    {
        const auto c = checkOf(home, site, up::Answer::yes);
        c->editorOpened();
        (void)settle(*c);
        P.eq("first.one_request", site->count(), 1);
        P.eq("first.path", !site->paths().empty() && site->paths()[0] == up::kReleasePath ? 1 : 0, 1);
        P.eq("first.host", site->lastHost() == kSite ? 1 : 0, 1);
        const pg::claude::Headers h = site->lastHeaders();
        P.eq("first.user_agent_alone", h.size() == 1 && h[0].first == "User-Agent"
                                           && h[0].second == std::string(pg::product::kName) + "/" + pg::product::kVersion ? 1 : 0, 1);
        const up::State s = c->state();
        P.eq("first.idle_newer", s.status == up::Status::idle && s.latest.version == "0.2.0" && s.newer && s.why.empty() ? 1 : 0, 1);
        P.eq("first.release_read", s.latest.tag == "v0.2.0" && s.latest.date == "2026-11-20" && s.latest.summary == "One line about it." ? 1 : 0, 1);
        P.eq("first.checked_at", s.checkedMs, clockMs.load());
        const juce::var app = appJson(home);
        P.eq("first.app_json", app["updates"].toString() == "yes" && app["latest"]["version"].toString() == "0.2.0"
                                   && static_cast<juce::int64>(app["checkedMs"]) == clockMs.load() ? 1 : 0, 1);

        // ---- DAY -------------------------------------------------------------------------------------------------------
        c->editorOpened();
        (void)settle(*c);
        P.eq("day.again_at_once.nothing", site->count(), 1);
        clockMs += int64_t{ 23 } * 3600 * 1000;
        c->editorOpened();
        (void)settle(*c);
        P.eq("day.23h.nothing", site->count(), 1);
        clockMs += int64_t{ 1 } * 3600 * 1000 + 1;
        c->editorOpened();
        (void)settle(*c);
        P.eq("day.24h.asked", site->count(), 2);
        c->checkNow();
        (void)settle(*c);
        P.eq("day.check_now.asked", site->count(), 3);
        c->checkNow();
        (void)settle(*c);
        P.eq("day.check_now_again.asked", site->count(), 4);

        // ---- REFUSE ----------------------------------------------------------------------------------------------------
        const auto refused = [&](const char* label, const std::string& body, int status, const char* words) {
            site->serve(body, status);
            const int before = site->count();
            c->checkNow();
            (void)settle(*c);
            const up::State st = c->state();
            P.eq(std::string("refuse.") + label + ".failed", st.status == up::Status::failed ? 1 : 0, 1);
            const bool said = st.why.find(words) != std::string::npos;
            if (!said)
                std::printf("refuse.%s: why = \"%s\"\n", label, st.why.c_str());
            P.eq(std::string("refuse.") + label + ".why", said ? 1 : 0, 1);
            P.eq(std::string("refuse.") + label + ".kept", st.latest.version == "0.2.0" && st.newer ? 1 : 0, 1);
            P.eq(std::string("refuse.") + label + ".one_request", site->count() - before, 1);
        };
        refused("redirect", release("0.9.0"), 301, "follows no redirect");
        const std::vector<std::string> asked = site->paths();   // one copy: begin and end of the same vector
        P.eq("refuse.redirect.not_followed", std::none_of(asked.begin(), asked.end(),
                                                          [](const std::string& p) { return p.find("elsewhere") != std::string::npos; })
                                                 ? 1 : 0, 1);
        refused("large", release("0.9.0", ",\"pad\":\"" + std::string(70 * 1024, 'x') + "\""), 200, "larger than 64 KB");
        P.le("refuse.large.read_no_further", static_cast<double>(site->delivered()), static_cast<double>(up::kMaxBytes + 1024));
        refused("broken", "{\"format\":\"funk-playground-release\",", 200, "does not read");
        refused("format", "{\"format\":\"something-else\",\"version\":1,\"latest\":{\"version\":\"0.9.0\"}}", 200, "not a release file");
        refused("version", "{\"format\":\"funk-playground-release\",\"version\":2,\"latest\":{\"version\":\"0.9.0\"}}", 200,
                "version this plugin does not read");
        refused("no_version", "{\"format\":\"funk-playground-release\",\"version\":1,\"latest\":{\"version\":\"latest\"}}", 200,
                "names no version");
        refused("status", release("0.9.0"), 503, "answered 503");
        site->serve(release("0.2.0"));
        c->checkNow();
        (void)settle(*c);
        P.eq("refuse.good_again_clears", c->state().status == up::Status::idle && c->state().why.empty() ? 1 : 0, 1);

        // ---- OFFLINE ---------------------------------------------------------------------------------------------------
        site->setOffline(true);
        clockMs += int64_t{ 25 } * 3600 * 1000;
        c->editorOpened();
        (void)settle(*c);
        const int afterOffline = site->count();
        P.eq("offline.failed", c->state().status == up::Status::failed && c->state().why.find("no connection") != std::string::npos ? 1 : 0, 1);
        P.eq("offline.kept", c->state().latest.version == "0.2.0" ? 1 : 0, 1);
        clockMs += int64_t{ 2 } * 3600 * 1000;
        c->editorOpened();
        (void)settle(*c);
        P.eq("offline.not_again_soon", site->count(), afterOffline);
        clockMs += int64_t{ 23 } * 3600 * 1000;
        c->editorOpened();
        (void)settle(*c);
        P.eq("offline.again_a_day_later", site->count(), afterOffline + 1);
        site->setOffline(false);

        // ---- EXTRA -----------------------------------------------------------------------------------------------------
        site->serve("{\"format\":\"funk-playground-release\",\"version\":1,\"channel\":\"stable\",\"latest\":{\"version\":\"0.3.0\","
                    "\"tag\":\"v0.3.0\",\"date\":\"2026-12-01\",\"summary\":\"Two\\nlines.\",\"notes\":[1,2]},\"later\":{\"x\":1}}");
        c->checkNow();
        (void)settle(*c);
        P.eq("extra.ignored", c->state().status == up::Status::idle && c->state().latest.version == "0.3.0" ? 1 : 0, 1);
        P.eq("extra.one_line", c->state().latest.summary == "Two lines." ? 1 : 0, 1);

        // ---- SKIP ------------------------------------------------------------------------------------------------------
        c->skip("0.3.0");
        (void)settle(*c);
        P.eq("skip.kept", c->state().skipped == "0.3.0" && appJson(home)["skipped"].toString() == "0.3.0" ? 1 : 0, 1);
    }

    // ---- LOAD ----------------------------------------------------------------------------------------------------------
    {
        const int before = site->count();
        const auto c = checkOf(home, site, up::Answer::notAsked);
        const up::State s = c->state();
        P.eq("load.answers", s.loaded && s.answer == up::Answer::yes && s.skipped == "0.3.0" ? 1 : 0, 1);
        P.eq("load.latest", s.latest.version == "0.3.0" && s.checkedMs > 0 ? 1 : 0, 1);
        P.eq("load.nothing_asked", site->count(), before);
    }

    // ---- VERSIONS ------------------------------------------------------------------------------------------------------
    {
        P.eq("versions.newer", up::compareVersions("0.2.0", "0.1.0"), 1);
        P.eq("versions.same", up::compareVersions("0.2.0", "0.2.0"), 0);
        P.eq("versions.older", up::compareVersions("0.1.9", "0.2.0"), -1);
        P.eq("versions.pre_release_below", up::compareVersions("0.2.0-rc.1", "0.2.0"), -1);
        P.eq("versions.release_above_pre", up::compareVersions("0.2.0", "0.2.0-rc.1"), 1);
        P.eq("versions.pre_releases", up::compareVersions("0.2.0-rc.1", "0.2.0-rc.2"), -1);
        P.eq("versions.numbers_not_text", up::compareVersions("0.10.0", "0.9.0"), 1);
        P.eq("versions.no_version_below", up::compareVersions("latest", "0.0.1"), -1);
        P.eq("versions.valid", up::validVersion("1.2.3") && up::validVersion("1.2.3-beta.1") && !up::validVersion("1.2")
                                   && !up::validVersion("v1.2.3") && !up::validVersion("1.2.3-") && !up::validVersion("1.2.3 ") ? 1 : 0, 1);
        const juce::File h = freshHome();
        auto s = std::make_shared<Site>();
        s->serve(release("0.2.0"));
        const auto same = checkOf(h, s, up::Answer::yes, "0.2.0");
        same->checkNow();
        (void)settle(*same);
        P.eq("versions.check_same_not_newer", same->state().latest.version == "0.2.0" && !same->state().newer ? 1 : 0, 1);
        const auto above = checkOf(freshHome(), s, up::Answer::yes, "0.3.0");
        above->checkNow();
        (void)settle(*above);
        P.eq("versions.check_older_not_newer", !above->state().newer ? 1 : 0, 1);
        const auto pre = checkOf(freshHome(), s, up::Answer::yes, "0.2.0-rc.1");
        pre->checkNow();
        (void)settle(*pre);
        P.eq("versions.check_pre_release_build_newer", pre->state().newer ? 1 : 0, 1);
    }

    // ---- LOCK ----------------------------------------------------------------------------------------------------------
    {
        const juce::File h = freshHome();
        h.getChildFile("app.json").replaceWithText("{\"seen\":\"0.1.0\",\"future\":{\"a\":1}}\n");
        auto s = std::make_shared<Site>();
        const auto a = checkOf(h, s, up::Answer::notAsked);
        const auto b = checkOf(h, s, up::Answer::notAsked);
        // Each writes its own field, many times over, at once.
        for (int i = 0; i < 40; ++i)
        {
            a->setAnswer(i % 2 == 0 ? up::Answer::no : up::Answer::yes);
            b->skip(i % 2 == 0 ? "0.4.0" : "0.5.0");
        }
        a->setAnswer(up::Answer::yes);
        b->skip("0.5.0");
        (void)settle(*a);
        (void)settle(*b);
        const juce::var app = appJson(h);
        P.eq("lock.answer_kept", app["updates"].toString() == "yes" ? 1 : 0, 1);
        P.eq("lock.skipped_kept", app["skipped"].toString() == "0.5.0" ? 1 : 0, 1);
        P.eq("lock.unknown_kept", app["seen"].toString() == "0.1.0" && static_cast<int>(app["future"]["a"]) == 1 ? 1 : 0, 1);
        int temps = 0;
        for (const juce::File& f : h.findChildFiles(juce::File::findFiles, false, "*.tmp"))
            temps += f.exists() ? 1 : 0;
        P.eq("lock.no_temporary_left", temps, 0);
    }

    // ---- PRICES --------------------------------------------------------------------------------------------------------
    {
        pg::usage::resetPricesForTests();
        const std::string prices = "{\"asOf\":\"2099-01-02\",\"webSearchPer1000\":12,\"usdPerMTok\":{\"anthropic\":{"
                                   "\"claude-test-1\":{\"in\":3,\"out\":15,\"cacheRead\":0.3,\"cacheWrite5m\":3.75,\"cacheWrite1h\":6,"
                                   "\"fastIn\":6,\"fastOut\":30},\"claude-test-2\":{\"in\":1,\"out\":5},\"claude-bad\":{\"out\":5}},"
                                   "\"elsewhere\":{\"model-x\":{\"in\":2,\"out\":4,\"later\":9}}},\"more\":true}";
        const juce::File h = freshHome();
        auto s = std::make_shared<Site>();
        s->serve(release("0.2.0", {}, prices));
        up::Config cfg = configOf(h, s);
        cfg.offersPrices = true;
        up::Check c(cfg);
        c.start();
        c.setAnswer(up::Answer::yes);
        c.checkNow();
        (void)settle(c);
        const pg::usage::PriceTable t = pg::usage::pricesInUse();
        P.eq("prices.taken", t.asOf == "2099-01-02" ? 1 : 0, 1);
        const auto row = [&](const char* provider, const char* model) -> const pg::usage::PriceRow* {
            for (const pg::usage::PriceRow& r : t.rows)
                if (r.provider == provider && r.model == model)
                    return &r;
            return nullptr;
        };
        const pg::usage::PriceRow* one = row("anthropic", "claude-test-1");
        P.eq("prices.row", one != nullptr && one->price.in == 3 && one->price.out == 15 && one->price.cacheRead == 0.3
                               && one->price.write1h == 6 && one->price.fastOut == 30 && std::abs(one->price.webSearch - 0.012) < 1e-12
                               ? 1 : 0, 1);
        const pg::usage::PriceRow* two = row("anthropic", "claude-test-2");
        P.eq("prices.defaults", two != nullptr && std::abs(two->price.cacheRead - 0.1) < 1e-12 && two->price.write5m == 1.25
                                    && two->price.write1h == 2 && two->price.fastIn == 0 ? 1 : 0, 1);
        P.eq("prices.row_without_price_left_out", row("anthropic", "claude-bad") == nullptr ? 1 : 0, 1);
        const pg::usage::PriceRow* other = row("elsewhere", "model-x");
        P.eq("prices.other_provider", other != nullptr && other->price.in == 2 && other->price.webSearch == 0 ? 1 : 0, 1);
        P.eq("prices.older_not_taken", !up::offerPricesJson("{\"asOf\":\"2000-01-01\",\"usdPerMTok\":{\"anthropic\":{\"m\":{\"in\":1,\"out\":1}}}}")
                                           && pg::usage::pricesInUse().asOf == "2099-01-02" ? 1 : 0, 1);
        P.eq("prices.kept_in_app_json", appJson(h)["prices"]["asOf"].toString() == "2099-01-02" ? 1 : 0, 1);
        pg::usage::resetPricesForTests();
        {
            // A check made later offers the table app.json kept, with no request.
            const int before = s->count();
            up::Check again(cfg);
            again.start();
            (void)settle(again);
            P.eq("prices.from_app_json", pg::usage::pricesInUse().asOf == "2099-01-02" && s->count() == before ? 1 : 0, 1);
        }
        pg::usage::resetPricesForTests();
    }

    // ---- LEDGER: the check's prices reach the Usage page ------------------------------------------------------------------
    {
        namespace us = pg::usage;
        us::resetPricesForTests();
        const juce::File h = freshHome();
        const juce::File chats = h.getChildFile("chat");
        (void)chats.createDirectory();
        pg::chatstore::setDirectoryForTests(chats);
        const char* const model = "claude-haiku-4-5";
        const us::Key key{ us::today(), "anthropic", model, false };
        us::Counts counts;
        counts.requests = 3;
        counts.input = 400000;
        counts.output = 50000;
        us::add(key, counts);
        P.eq("ledger.seeded", us::flush() ? 1 : 0, 1);
        auto s = std::make_shared<Site>();
        up::Config cfg = configOf(h, s, pg::product::kVersion);
        cfg.offersPrices = true;
        h.getChildFile("app.json").replaceWithText("{\"updates\":\"yes\"}\n");
        up::useConfigForTests(&cfg);
        pg::factorystore::setDirectoryForTests(h);
        {
            pg::Processor a;
            pg::EditorPort& port = a.editorPort();
            a.chatAgent().setUsageRereadSecondsForTests(1.0e9);   // only a moved revision makes a read
            const auto view = [&] { return port.chat().usage(us::Range::week); };
            const auto rowOf = [&](const pg::UsageView& v) -> std::optional<pg::UsageRow> {
                for (const pg::UsageRow& r : v.rows)
                    if (r.key.model == model && r.key.provider == "anthropic" && !r.key.fast)
                        return r;
                return std::nullopt;
            };
            std::shared_ptr<const pg::UsageView> v;
            (void)waitFor([&] {
                v = view();
                return v->read && rowOf(*v).has_value();
            });
            const std::optional<pg::UsageRow> before = rowOf(*v);
            const us::Cost builtIn = us::costOf(key, counts, us::priceOf(us::builtInPrices(), "anthropic", model));
            P.eq("ledger.built_in.date", v->pricesAsOf == us::builtInPrices().asOf ? 1 : 0, 1);
            P.eq("ledger.built_in.estimate", before && before->cost.kind == us::Cost::Kind::estimate
                                                 && before->cost.microUsd == builtIn.microUsd ? 1 : 0, 1);

            // The check's reply: a later table, another price for that model.
            const std::string later = "{\"asOf\":\"2099-03-04\",\"webSearchPer1000\":10,\"usdPerMTok\":{\"anthropic\":{"
                                      "\"" + std::string(model) + "\":{\"in\":7,\"out\":9}}}}";
            s->serve(release("0.1.0", {}, later));
            const uint64_t serial = v->serial;
            port.editorOpened();
            const std::shared_ptr<up::Check> c = up::process();
            if (c != nullptr)
                (void)waitFor([&] { return s->count() >= 1 && c->idle(); });
            P.eq("ledger.later.asked", s->count(), 1);
            const bool reread = waitFor([&] {
                v = view();
                return v->read && v->serial > serial && v->pricesAsOf == "2099-03-04";
            });
            P.eq("ledger.later.read_at_once", reread ? 1 : 0, 1);
            P.eq("ledger.later.date", v->pricesAsOf == "2099-03-04" ? 1 : 0, 1);
            us::Price price;
            price.in = 7;
            price.out = 9;
            const us::Cost wanted = us::costOf(key, counts, price);
            const std::optional<pg::UsageRow> after = rowOf(*v);
            if (after)
                std::printf("ledger: %lld micro-USD at the built-in table, %lld at the check's\n",
                            static_cast<long long>(before ? before->cost.microUsd : -1), static_cast<long long>(after->cost.microUsd));
            P.eq("ledger.later.estimate", after && after->cost.kind == us::Cost::Kind::estimate && after->cost.microUsd == wanted.microUsd
                                              && wanted.microUsd != builtIn.microUsd ? 1 : 0, 1);

            // An earlier table: neither the date nor the estimate moves, and no read is asked for.
            const std::string earlier = "{\"asOf\":\"2099-01-01\",\"usdPerMTok\":{\"anthropic\":{"
                                        "\"" + std::string(model) + "\":{\"in\":100,\"out\":100}}}}";
            s->serve(release("0.1.0", {}, earlier));
            const uint64_t revision = us::revision();
            port.checkForUpdates();
            if (c != nullptr)
                (void)waitFor([&] { return s->count() >= 2 && c->idle(); });
            P.eq("ledger.earlier.asked", s->count(), 2);
            P.eq("ledger.earlier.revision_kept", us::revision() == revision ? 1 : 0, 1);
            const std::shared_ptr<const pg::UsageView> again = view();
            const std::optional<pg::UsageRow> kept = rowOf(*again);
            P.eq("ledger.earlier.date_kept", again->pricesAsOf == "2099-03-04" && us::pricesInUse().asOf == "2099-03-04" ? 1 : 0, 1);
            P.eq("ledger.earlier.estimate_kept", kept && kept->cost.microUsd == wanted.microUsd ? 1 : 0, 1);
        }
        up::useConfigForTests(nullptr);
        pg::factorystore::setDirectoryForTests(juce::File());
        pg::chatstore::setDirectoryForTests(juce::File());
        us::resetPricesForTests();
    }

    // ---- HELD: a store's file renamed over while another process reads it --------------------------------------------
    {
        namespace fs = std::filesystem;
        namespace us = pg::usage;
#if defined(_WIN32)
        constexpr bool kPosix = false;
#else
        constexpr bool kPosix = true;
#endif
        const char* const kPosixWhy = "a rename over an open file always lands on POSIX: the reader that never lets go is Windows' case";
        const int kLetGo = pg::files::kWaitMs / 5;       // a reader that lets go well within the helper's window
        const int kNever = pg::files::kWaitMs * 3;       // one that outlasts it
        const juce::File h = freshHome();
        const fs::path dir(h.getFullPathName().toStdString());
        const auto writeText = [](const fs::path& p, const std::string& t) {
            std::ofstream out(p, std::ios::binary | std::ios::trunc);
            out << t;
        };

        // The helper itself.
        {
            const fs::path to = dir / "held.txt", from = dir / "held.txt.tmp";
            writeText(to, "old");
            writeText(from, "new");
            Reader r(to);
            P.eq("held.replace.reader_open", r.open(kLetGo) ? 1 : 0, 1);
            std::error_code ec;
            const bool ok = pg::files::replace(from, to, ec);
            r.close();
            P.eq("held.replace.lands", ok && textOf(to) == "new" && !fs::exists(from) ? 1 : 0, 1);
            writeText(from, "newer");
            if (kPosix)
                probe::skip(P, "held.replace.never", kPosixWhy);
            else
            {
                Reader never(to);
                (void)never.open(kNever);
                std::error_code e2;
                const bool moved = pg::files::replace(from, to, e2);
                P.eq("held.replace.never", !moved && e2 && textOf(to) == "new" && fs::exists(from) ? 1 : 0, 1);
                never.close();
            }
        }

        // usage.json: a flush and a Reset.
        {
            const fs::path file = dir / "usage.json";
            us::Ledger ledger(file);
            us::Counts one;
            one.requests = 1;
            one.input = 1000;
            const us::Key key{ us::today(), "anthropic", "claude-haiku-4-5", false };
            ledger.add(key, one);
            P.eq("held.usage.first_flush", ledger.flush() ? 1 : 0, 1);
            ledger.add(key, one);
            Reader r(file);
            (void)r.open(kLetGo);
            const bool flushed = ledger.flush();
            r.close();
            const std::vector<us::Row> rows = us::read(file);
            P.eq("held.usage.flush_lands", flushed && rows.size() == 1 && rows[0].counts.requests == 2 ? 1 : 0, 1);
            // What it wrote is gone from its rows: a second flush with nothing added counts nothing again.
            const bool again = ledger.flush();
            const std::vector<us::Row> rows2 = us::read(file);
            P.eq("held.usage.flush_lands.counted_once", again && !ledger.pending() && rows2.size() == 1
                                                            && rows2[0].counts.requests == 2 ? 1 : 0, 1);
            ledger.requestReset();
            Reader r2(file);
            (void)r2.open(kLetGo);
            const bool reset = ledger.reset();
            r2.close();
            P.eq("held.usage.reset_lands", reset && !ledger.lastResetFailed() && us::read(file).empty() ? 1 : 0, 1);
            // The cut's rows went with the file: nothing waits, and the next flush brings no count back.
            const bool after = ledger.flush();
            P.eq("held.usage.reset_lands.stays_empty", after && !ledger.pending() && !ledger.resetOwed()
                                                           && us::read(file).empty() ? 1 : 0, 1);
            if (kPosix)
            {
                probe::skip(P, "held.usage.flush_never", kPosixWhy);
                probe::skip(P, "held.usage.reset_never", kPosixWhy);
            }
            else
            {
                ledger.add(key, one);
                {
                    Reader never(file);
                    (void)never.open(kNever);
                    const bool f = ledger.flush();
                    never.close();
                    P.eq("held.usage.flush_never.fails", f ? 0 : 1, 1);
                }
                P.eq("held.usage.flush_never.kept", ledger.pending() && ledger.flush() && us::read(file).size() == 1 ? 1 : 0, 1);
                ledger.add(key, one);                    // counted before the cut: the reset owes it, and keeps it on failing
                const std::string before = textOf(file);
                ledger.requestReset();
                {
                    Reader never(file);
                    (void)never.open(kNever);
                    const bool f = ledger.reset();
                    never.close();
                    P.eq("held.usage.reset_never.fails", !f && ledger.lastResetFailed() ? 1 : 0, 1);
                }
                P.eq("held.usage.reset_never.nothing_lost", textOf(file) == before && ledger.pending() ? 1 : 0, 1);
            }
        }

        // app.json: an answer.
        {
            auto heldSite = std::make_shared<Site>();
            const fs::path app = dir / "app.json";
            writeText(app, "{\"seen\":\"0.1.0\"}\n");
            up::Config cfg = configOf(h, heldSite);
            up::Check c(cfg);
            c.start();
            (void)settle(c);
            {
                Reader r(app);
                (void)r.open(kLetGo);
                c.setAnswer(up::Answer::no);
                (void)settle(c);
                r.close();
            }
            P.eq("held.app.lands", appJson(h)["updates"].toString() == "no" && appJson(h)["seen"].toString() == "0.1.0" ? 1 : 0, 1);
            P.eq("held.app.lands.written", c.written() ? 1 : 0, 1);   // the check's own answer, not the file's alone
            if (kPosix)
                probe::skip(P, "held.app.never", kPosixWhy);
            else
            {
                {
                    Reader never(app);
                    (void)never.open(kNever);
                    c.setAnswer(up::Answer::later);
                    (void)settle(c);
                    P.eq("held.app.never.unchanged", appJson(h)["updates"].toString() == "no" ? 1 : 0, 1);
                    P.eq("held.app.never.not_written", c.written() ? 0 : 1, 1);
                    never.close();
                }
                c.skip("0.9.0");                         // the next write takes the answer that waited with it
                (void)settle(c);
                P.eq("held.app.never.nothing_lost", appJson(h)["updates"].toString() == "later"
                                                        && appJson(h)["skipped"].toString() == "0.9.0" ? 1 : 0, 1);
                P.eq("held.app.never.written_after", c.written() ? 1 : 0, 1);
            }
        }

        // factory.json: an answer (the factory store's own thread writes it).
        {
            const fs::path answerFile = dir / "factory.json";
            writeText(answerFile, "{}\n");
            pg::factorystore::Config fc = pg::factorystore::processConfig();
            fc.folder = (dir / "Factory" / pg::product::kKey).string();
            fc.answerFile = answerFile.string();
            fc.transport = std::make_shared<Site>();
            fc.off = true;                               // nothing fetched: the answer's file alone
            pg::factorystore::Store store(fc);
            store.start();
            const auto idle = [&] { return waitFor([&] { return store.idle(); }); };
            (void)idle();
            {
                Reader r(answerFile);
                (void)r.open(kLetGo);
                store.setAnswer(pg::factorystore::Answer::yes);
                (void)idle();
                r.close();
            }
            P.eq("held.factory.lands", textOf(answerFile).find("\"yes\"") != std::string::npos ? 1 : 0, 1);
            P.eq("held.factory.lands.written", store.answerWritten() ? 1 : 0, 1);   // the store's own answer
            if (kPosix)
                probe::skip(P, "held.factory.never", kPosixWhy);
            else
            {
                Reader never(answerFile);
                (void)never.open(kNever);
                store.setAnswer(pg::factorystore::Answer::no);
                (void)idle();
                // As before this change: the file as it was, the answer kept in the store (its next write takes it).
                P.eq("held.factory.never", textOf(answerFile).find("\"yes\"") != std::string::npos
                                               && store.answer() == pg::factorystore::Answer::no ? 1 : 0, 1);
                P.eq("held.factory.never.not_written", store.answerWritten() ? 0 : 1, 1);
                never.close();
                store.setAnswer(pg::factorystore::Answer::no);   // the next write takes it
                (void)idle();
                P.eq("held.factory.never.written_after", store.answerWritten()
                                                             && textOf(answerFile).find("\"no\"") != std::string::npos ? 1 : 0, 1);
            }
        }
    }

    // ---- TWO, OFFER, SKIP (the processor's), DOWNLOAD ------------------------------------------------------------------
    {
        const juce::File h = freshHome();
        auto s = std::make_shared<Site>();
        s->serve(release("0.9.0"));
        const up::Config cfg = configOf(h, s, pg::product::kVersion);
        up::useConfigForTests(&cfg);
        pg::factorystore::setDirectoryForTests(h);
        {
            pg::Processor a, b;
            const std::shared_ptr<up::Check> c = up::process();
            P.eq("two.one_check", c != nullptr ? 1 : 0, 1);
            pg::EditorPort& pa = a.editorPort();
            pg::EditorPort& pb = b.editorPort();
            (void)waitFor([&] { return pa.updateState().loaded; });
            const pg::UpdateState u0 = pa.updateState();
            P.eq("offer.due", u0.offer && !u0.updatesAnswered && u0.factory == 0 && u0.check == pg::UpdateState::Check::unanswered ? 1 : 0, 1);
            pa.editorOpened();
            pb.editorOpened();
            if (c != nullptr)
                (void)settle(*c);
            P.eq("offer.unanswered_nothing_asked", s->count(), 0);
            pa.answerOffer(true, false, true);
            if (c != nullptr)
                (void)waitFor([&] { return s->count() >= 1 && c->idle(); });
            const pg::UpdateState u1 = pb.updateState();
            P.eq("offer.allow.updates_yes", u1.updatesOn && u1.updatesAnswered ? 1 : 0, 1);
            P.eq("offer.allow.factory_no", u1.factory, 2);
            P.eq("offer.allow.not_due_again", u1.offer ? 1 : 0, 0);
            // Written on the factory store's own thread: waited for (kWorkWait), as any other thread's work.
            P.eq("offer.allow.factory_json", waitFor([&] { return h.getChildFile("factory.json").loadFileAsString().contains("\"no\""); }) ? 1 : 0, 1);
            P.eq("two.one_request_for_two_editors", s->count(), 1);
            P.eq("two.newer", u1.newer && u1.latest == "0.9.0" ? 1 : 0, 1);
            P.eq("two.announced_once", pa.announceUpdate("0.9.0") && !pb.announceUpdate("0.9.0") ? 1 : 0, 1);
            // The processor's newer is false for a skipped version.
            pa.skipUpdate("0.9.0");
            if (c != nullptr)
                (void)settle(*c);
            P.eq("skip.processor_not_newer", !pb.updateState().newer && pb.updateState().skipped == "0.9.0" ? 1 : 0, 1);
            s->serve(release("0.10.0"));
            pa.checkForUpdates();
            if (c != nullptr)
                (void)settle(*c);
            P.eq("skip.later_one_shows", pb.updateState().newer && pb.updateState().latest == "0.10.0" ? 1 : 0, 1);
            pa.setUpdates(false);
            if (c != nullptr)
                (void)settle(*c);
            P.eq("offer.switch_off.no_pill", !pb.updateState().newer && !pb.updateState().updatesOn ? 1 : 0, 1);

            // DOWNLOAD
            std::string asked;
            a.setPageLauncher([&](const std::string& url, std::string&) {
                asked = url;
                return true;
            });
            std::string why;
            P.eq("download.hook", pa.openDownloadPage(why) && asked == std::string(pg::factorystore::kSite) + "/download" ? 1 : 0, 1);
            std::string whyB;
            P.eq("download.nothing_under_a_probe", !pb.openDownloadPage(whyB) && whyB.find("probe") != std::string::npos ? 1 : 0, 1);
        }
        up::useConfigForTests(nullptr);
        pg::factorystore::setDirectoryForTests(juce::File());
    }
    {
        // Not now: updates later, the factory unasked, the offer not due again (a new processor reads it so).
        const juce::File h = freshHome();
        auto s = std::make_shared<Site>();
        s->serve(release("0.9.0"));
        const up::Config cfg = configOf(h, s, pg::product::kVersion);
        up::useConfigForTests(&cfg);
        pg::factorystore::setDirectoryForTests(h);
        {
            pg::Processor a;
            pg::EditorPort& port = a.editorPort();
            (void)waitFor([&] { return port.updateState().loaded; });
            port.editorOpened();
            port.answerOffer(false, true, true);
            const std::shared_ptr<up::Check> c = up::process();
            if (c != nullptr)
                (void)settle(*c);
            const pg::UpdateState u = port.updateState();
            P.eq("offer.not_now.later", c != nullptr && c->answer() == up::Answer::later && u.updatesAnswered && !u.updatesOn ? 1 : 0, 1);
            P.eq("offer.not_now.factory_unasked", u.factory, 0);
            P.eq("offer.not_now.nothing_asked", s->count(), 0);
        }
        {
            pg::Processor again;
            pg::EditorPort& port = again.editorPort();
            (void)waitFor([&] { return port.updateState().loaded; });
            P.eq("offer.not_now.not_due_again", port.updateState().offer ? 1 : 0, 0);
            P.eq("offer.not_now.app_json", appJson(h)["updates"].toString() == "later" ? 1 : 0, 1);
        }
        up::useConfigForTests(nullptr);
        pg::factorystore::setDirectoryForTests(juce::File());
    }

    // ---- JOIN ----------------------------------------------------------------------------------------------------------
    {
        const juce::File h = freshHome();
        auto s = std::make_shared<Site>();
        s->setStall(true);
        const up::Config cfg = configOf(h, s, pg::product::kVersion);
        up::useConfigForTests(&cfg);
        pg::factorystore::setDirectoryForTests(h);
        h.getChildFile("app.json").replaceWithText("{\"updates\":\"yes\"}\n");
        {
            auto a = std::make_unique<pg::Processor>();
            a->editorPort().editorOpened();
            P.eq("join.in_flight", waitFor([&] { return s->stalling() > 0; }) ? 1 : 0, 1);
            // The processor goes here, on the thread that made it; a watchdog ends the stall after kWorkWait, so a check
            // that never stops on its cancel fails the row instead of hanging the probe.
            std::atomic<bool> gone{ false }, rescued{ false };
            std::thread watchdog([&] {
                if (!waitFor([&] { return gone.load(); }))
                {
                    rescued.store(true);
                    s->setStall(false);
                }
            });
            a.reset();
            gone.store(true);
            watchdog.join();
            P.eq("join.returns", rescued.load() ? 0 : 1, 1);
        }
        P.eq("join.released", up::process() == nullptr && s->stalling() == 0 ? 1 : 0, 1);
        up::useConfigForTests(nullptr);
        pg::factorystore::setDirectoryForTests(juce::File());
    }

    base.deleteRecursively();
    return P.finish();
}
