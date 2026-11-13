// PROBE name=plugin.factorystore timeout=180
//
// plugin.factorystore: the factory projects' store (plugin/FactoryStore.h) and the seam the rest of the plugin sees them
// through (plugin/Factory.h's current() and generation()), against the website's fixtures (Tools/probes/fixtures/
// factory/: each case a little site, cases.json what a plugin must do with it). A scripted transport serves a case's
// folder as the site; the stores are made here with the fixtures' test keys (keys.json) and folders of this probe's own,
// so nothing touches the network, the user's folders or a private key.
//   CASE      every row of cases.json this product can judge: a taken case is taken with exactly the projects it lists,
//             its files and index on disk; a refused one is refused at the step it names, with its reason, leaving no
//             set, no index and no file (not even a temporary). The other product's taken rows are its own run's (its
//             projects are of its kind, which this product's store refuses): skipped here, said.
//   GATE      nothing is asked of the site before an editor opens, nor with the answer not given or no, nor with
//             <ENV_PREFIX>FACTORY=off (processConfig reads it); the site the environment may name (siteFrom); the
//             offer's answer given through the processor (EditorPort::answerOffer): a yes fetches only once an editor
//             opens, a no never
//   FIRST     a first fetch: the set appears, the generation moves, the files are on disk, the request sends a
//             User-Agent alone; a second store on the folder reads it with no request; the same index again asks for
//             the index alone; an editor's open again asks nothing within 24 hours, and the index alone after
//   UPDATE    good-1 then good-2 (one added, one changed, one withdrawn: only the new files asked for, the withdrawn
//             one's file gone from the disk), then good-1 again refused as older (the set and the disk as they were),
//             then key-change (signed with the next key) taken
//   NEWER     needs-newer: the entry that needs 99.0.0 left out, its file never asked for (the synth's fixture)
//   OFFLINE   no connection with no set (failed, "no connection") and with one (the set kept, ready)
//   RESUME    a fetch cut short by a file's request failing leaves no set and the files that passed; the next asks
//             only for the rest
//   CORRUPT   a file on disk changed between runs: the set on disk refused (step file), then fetched afresh, that file
//             alone asked for
//   TWO       two stores on one folder fetching at once (one set on disk, no temporary left), and racing an older set
//             against a newer one: the newer sequence is never lost
//   CAP       a file larger than its entry says is read no further than a little past it, and refused
//   REDIRECT  a redirect answer is refused and not followed
//   CANCEL    destroying a store whose fetch is in flight returns promptly; cancel() ends one and leaves no failure
//   SEAM      factory::current() is the compiled-in set with no store's set; a processor's store with a set on disk
//             publishes it (current(), the port's FACTORY list, ProjectStore's load, exists, presets, read-only);
//             a set fetched later replaces it (the generation moves, a withdrawn project is no longer served); the
//             last processor gone takes it back
#include "Probe.h"

#include "plugin/ClaudeApi.h"
#include "plugin/EditorPort.h"
#include "plugin/Factory.h"
#include "plugin/FactoryKeys.h"
#include "plugin/FactoryStore.h"
#include "plugin/ProjectStore.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace pg = playground;
namespace fsx = pg::factorystore;

namespace
{
    constexpr const char* kSite = "https://fixture.test";

    juce::File fixtures() { return juce::File(PROBE_SOURCE_DIR).getChildFile("Tools/probes/fixtures/factory"); }

    juce::var readJson(const juce::File& f) { return juce::JSON::parse(f.loadFileAsString()); }

    // ---- the site: a case's folder served as the website would --------------------------------------------------------
    class Site final : public pg::claude::Transport
    {
    public:
        explicit Site(juce::File root) : root_(std::move(root)) {}

        void serve(const juce::File& root)
        {
            const std::lock_guard<std::mutex> lock(m_);
            root_ = root;
        }
        void setOffline(bool on) { offline_.store(on); }
        void failOn(const std::string& path)
        {
            const std::lock_guard<std::mutex> lock(m_);
            fail_ = path;
        }
        void redirectOn(const std::string& path)
        {
            const std::lock_guard<std::mutex> lock(m_);
            redirect_ = path;
        }
        void stallOn(const std::string& path)
        {
            const std::lock_guard<std::mutex> lock(m_);
            stall_ = path;
        }
        void extraOn(const std::string& path, std::size_t bytes)
        {
            const std::lock_guard<std::mutex> lock(m_);
            extra_[path] = bytes;
        }
        void clearRequests()
        {
            const std::lock_guard<std::mutex> lock(m_);
            paths_.clear();
        }
        std::vector<std::string> requests() const
        {
            const std::lock_guard<std::mutex> lock(m_);
            return paths_;
        }
        bool asked(const std::string& fragment) const
        {
            const std::lock_guard<std::mutex> lock(m_);
            return std::any_of(paths_.begin(), paths_.end(), [&](const std::string& p) { return p.find(fragment) != std::string::npos; });
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
        std::size_t delivered(const std::string& path) const
        {
            const std::lock_guard<std::mutex> lock(m_);
            const auto it = delivered_.find(path);
            return it != delivered_.end() ? it->second : 0;
        }
        int stalling() const { return stalling_.load(); }

        pg::claude::HttpResult post(const std::string&, const std::string&, const std::string&, const pg::claude::Headers&,
                                    const std::function<bool(std::string_view)>&, const std::atomic<bool>&) override
        {
            return { 500, {}, "no POST here", -1, {} };
        }

        pg::claude::HttpResult get(const std::string&, const std::string&, const pg::claude::Headers&,
                                   const std::atomic<bool>&) override
        {
            return { 500, {}, "the store asks with getLimited", -1, {} };
        }

        pg::claude::HttpResult getLimited(const std::string& host, const std::string& path, const pg::claude::Headers& headers,
                                          const std::function<bool(std::string_view)>& onChunk, std::size_t maxOther,
                                          const std::atomic<bool>& cancel) override
        {
            juce::File root;
            bool fail = false, redirect = false, stall = false;
            std::size_t extra = 0;
            {
                const std::lock_guard<std::mutex> lock(m_);
                paths_.push_back(path);
                headers_ = headers;
                host_ = host;
                root = root_;
                fail = path == fail_;
                redirect = !redirect_.empty() && path == redirect_;
                stall = !stall_.empty() && path == stall_;
                if (const auto it = extra_.find(path); it != extra_.end())
                    extra = it->second;
            }
            if (offline_.load() || fail)
                return { 0, "no connection to fixture.test", {}, -1, {} };
            if (redirect)
                return { 301, {}, std::string(std::min<std::size_t>(maxOther, 20), 'x'), -1, "https://elsewhere.test" + path };
            if (stall)
            {
                ++stalling_;
                while (!cancel.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                --stalling_;
                return { 0, "stopped", {}, -1, {} };
            }
            const juce::File file = root.getChildFile(juce::String(path).substring(1));
            if (!file.existsAsFile())
                return { 404, {}, "not found", -1, {} };
            juce::MemoryBlock data;
            (void)file.loadFileAsData(data);
            std::string body(static_cast<const char*>(data.getData()), data.getSize());
            body.append(extra, 'z');
            constexpr std::size_t kChunk = 256;
            for (std::size_t at = 0; at < body.size(); at += kChunk)
            {
                if (cancel.load())
                    return { 0, "stopped", {}, -1, {} };
                const std::size_t n = std::min(kChunk, body.size() - at);
                {
                    const std::lock_guard<std::mutex> lock(m_);
                    delivered_[path] += n;
                }
                if (!onChunk(std::string_view(body).substr(at, n)))
                    break;
            }
            return { 200, {}, {}, -1, {} };
        }

    private:
        mutable std::mutex m_;
        juce::File root_;
        std::atomic<bool> offline_{ false };
        std::string fail_, redirect_, stall_;
        std::map<std::string, std::size_t> extra_;
        std::map<std::string, std::size_t> delivered_;
        std::vector<std::string> paths_;
        pg::claude::Headers headers_;
        std::string host_;
        std::atomic<int> stalling_{ 0 };
    };

    // ---- a product's identity, as a store is configured with it ----------------------------------------------------------
    struct Identity
    {
        std::string key, kind, plugin;
    };

    // This product's, from Product.h; the other one's from its good set's index in the fixtures (no product is named
    // here).
    Identity identityOf(const std::string& key)
    {
        if (key == pg::product::kKey)
            return { key, pg::product::kKind, pg::product::kName };
        const juce::var index = readJson(fixtures().getChildFile("good-1/factory/v1/" + juce::String(key) + "/index.json"));
        const juce::var body = juce::JSON::parse(index["body"].toString());
        return { key, body["kind"].toString().toStdString(), body["plugin"].toString().toStdString() };
    }

    std::vector<fsx::Config::Key> testKeys()
    {
        std::vector<fsx::Config::Key> out;
        const juce::var keys = readJson(fixtures().getChildFile("keys.json"));
        if (const juce::DynamicObject* o = keys.getDynamicObject())
            for (const auto& k : o->getProperties())
            {
                juce::MemoryOutputStream bytes;
                if (!juce::Base64::convertFromBase64(bytes, k.value.toString()) || bytes.getDataSize() != 65)
                    continue;
                fsx::Config::Key key;
                key.id = k.name.toString().toStdString();
                std::copy_n(static_cast<const uint8_t*>(bytes.getData()), 65, key.point.begin());
                out.push_back(std::move(key));
            }
        return out;
    }

    std::atomic<int64_t> clockMs{ 1'800'000'000'000 };

    fsx::Config configOf(const Identity& id, const juce::File& home, std::shared_ptr<Site> site)
    {
        fsx::Config c;
        c.folder = home.getChildFile("Factory").getChildFile(id.key).getFullPathName().toStdString();
        c.answerFile = home.getChildFile("factory.json").getFullPathName().toStdString();
        c.key = id.key;
        c.kind = id.kind;
        c.plugin = id.plugin;
        c.version = pg::product::kVersion;
        c.site = kSite;
        c.keys = testKeys();
        c.transport = std::move(site);
        c.now = [] { return clockMs.load(); };
        c.off = false;
        c.userAgent = std::string(pg::product::kName) + "/" + pg::product::kVersion;
        return c;
    }

    bool settle(const fsx::Store& s, int ms = 20000)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < until)
        {
            if (s.idle())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    // A store over `home` for `id`, answered yes, started and settled.
    std::unique_ptr<fsx::Store> storeOf(const Identity& id, const juce::File& home, std::shared_ptr<Site> site,
                                        fsx::Answer answer = fsx::Answer::yes)
    {
        auto s = std::make_unique<fsx::Store>(configOf(id, home, std::move(site)));
        if (answer != fsx::Answer::notAsked)
            s->setAnswer(answer);
        s->start();
        (void)settle(*s);
        return s;
    }

    std::vector<std::string> ownNames(const pg::factory::Snapshot& s)
    {
        std::vector<std::string> out;
        if (s != nullptr)
            for (const pg::projectstore::Project& p : s->projects)
                out.push_back(pg::ownName(p.name));
        std::sort(out.begin(), out.end());
        return out;
    }

    std::vector<std::string> sorted(std::vector<std::string> v)
    {
        std::sort(v.begin(), v.end());
        return v;
    }

    juce::File folderOf(const juce::File& home, const std::string& key) { return home.getChildFile("Factory").getChildFile(key); }

    // The files in a folder and its p/, by name (temporaries too).
    std::vector<std::string> filesIn(const juce::File& dir)
    {
        std::vector<std::string> out;
        for (const juce::File& f : dir.findChildFiles(juce::File::findFiles, true, "*", juce::File::FollowSymlinks::no))
            if (f.getFileName() != ".lock")
                out.push_back(f.getRelativePathFrom(dir).replaceCharacter('\\', '/').toStdString());
        std::sort(out.begin(), out.end());
        return out;
    }

    bool anyTemp(const juce::File& dir)
    {
        const std::vector<std::string> files = filesIn(dir);
        return std::any_of(files.begin(), files.end(), [](const std::string& f) { return f.size() > 4 && f.substr(f.size() - 4) == ".tmp"; });
    }

    int fmpacks(const juce::File& dir) { return dir.getChildFile("p").getNumberOfChildFiles(juce::File::findFiles, "*.fmpack"); }

    // A case's index body's entries: name, hash, needs, bytes.
    struct FixtureEntry
    {
        std::string name, sha256, needs;
        std::size_t bytes = 0;
    };
    std::vector<FixtureEntry> entriesOf(const std::string& c, const std::string& key)
    {
        std::vector<FixtureEntry> out;
        const juce::var index = readJson(fixtures().getChildFile(juce::String(c) + "/factory/v1/" + juce::String(key) + "/index.json"));
        const juce::var body = juce::JSON::parse(index["body"].toString());
        if (const juce::Array<juce::var>* ps = body["projects"].getArray())
            for (const juce::var& p : *ps)
                out.push_back({ p["name"].toString().toStdString(), p["sha256"].toString().toStdString(),
                                p["needs"].toString().toStdString(), static_cast<std::size_t>(static_cast<juce::int64>(p["bytes"])) });
        return out;
    }

    std::string indexPath(const std::string& key) { return "/factory/v1/" + key + "/index.json"; }
    std::string filePath(const std::string& hash) { return "/projects/v1/p/" + hash + ".fmpack"; }

    std::string stepName(fsx::Step s)
    {
        switch (s)
        {
            case fsx::Step::none: return "none";
            case fsx::Step::connection: return "connection";
            case fsx::Step::address: return "address";
            case fsx::Step::size: return "size";
            case fsx::Step::signature: return "signature";
            case fsx::Step::index: return "index";
            case fsx::Step::sequence: return "sequence";
            case fsx::Step::file: return "file";
            case fsx::Step::project: return "project";
            case fsx::Step::disk: return "disk";
        }
        return "?";
    }

    std::string statusName(fsx::Status s)
    {
        switch (s)
        {
            case fsx::Status::ready: return "ready";
            case fsx::Status::fetching: return "fetching";
            case fsx::Status::off: return "off";
            case fsx::Status::notAsked: return "notAsked";
            case fsx::Status::declined: return "declined";
            case fsx::Status::failed: return "failed";
            case fsx::Status::none: return "none";
        }
        return "?";
    }

    void say(const std::string& what, const fsx::State& st)
    {
        std::printf("  %s: %s, set %lld (%d projects), failure %d at %s: %s\n", what.c_str(), statusName(st.status).c_str(),
                    static_cast<long long>(st.sequence), st.projects, static_cast<int>(st.failure), stepName(st.step).c_str(),
                    st.reason.c_str());
    }

    // The environment variable restored when it goes.
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
}

PROBE(plugin, factorystore)
{
    const funkgui::HeadlessGuiScope gui;
    namespace ps = pg::projectstore;
    namespace fy = pg::factory;
    const std::string own = pg::product::kKey;
    const Identity me = identityOf(own);
    const juce::File base = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getChildFile(juce::String(pg::product::kEnvPrefix) + "factorystore-probe-"
                                              + juce::String(juce::Time::currentTimeMillis()));
    base.deleteRecursively();
    int homes = 0;
    const auto freshHome = [&] { return base.getChildFile("home-" + juce::String(++homes)); };
    const juce::var cases = readJson(fixtures().getChildFile("cases.json"));
    if (!cases.isArray() || testKeys().size() < 2)
    {
        P.harnessError("no fixtures (cases.json, keys.json): the website repository makes them");
        return P.finish();
    }
    const std::vector<FixtureEntry> good1 = entriesOf("good-1", own), good2 = entriesOf("good-2", own);
    const auto namesOf = [](const std::vector<FixtureEntry>& v) {
        std::vector<std::string> out;
        for (const FixtureEntry& e : v)
            out.push_back(e.name);
        return sorted(out);
    };

    // ---- CASE ----------------------------------------------------------------------------------------------------------
    {
        int judged = 0;
        for (const juce::var& row : *cases.getArray())
        {
            const std::string c = row["case"].toString().toStdString(), key = row["key"].toString().toStdString();
            const bool taken = row["expect"].toString() == "taken";
            const int64_t highest = static_cast<int64_t>(static_cast<juce::int64>(row["highest"]));
            const std::string label = "case." + c + "." + key + (highest > 0 ? ".after" + std::to_string(highest) : std::string());
            if (taken && key != own)
            {
                probe::skip(P, label, "the other product's projects are of its kind: its own run takes them");
                continue;
            }
            ++judged;
            const juce::File home = freshHome();
            const juce::File folder = folderOf(home, key);
            if (highest > 0)
            {
                (void)folder.createDirectory();
                (void)folder.getChildFile("state.json").replaceWithText("{\"sequence\":" + juce::String(highest) + ",\"checked\":0}\n");
            }
            auto site = std::make_shared<Site>(fixtures().getChildFile(c));
            const std::unique_ptr<fsx::Store> s = storeOf(identityOf(key), home, site);
            s->refreshNow();
            const bool settled = settle(*s);
            const fsx::State st = s->state();
            say(label, st);
            if (taken)
            {
                std::vector<std::string> want;
                for (const juce::var& n : *row["projects"].getArray())
                    want.push_back(n.toString().toStdString());
                const bool same = settled && st.status == fsx::Status::ready && ownNames(s->set()) == sorted(want);
                P.eq(label + ".taken", same ? 1 : 0, 1);
                P.eq(label + ".on_disk", folder.getChildFile("index.json").existsAsFile() && folder.getChildFile("state.json").existsAsFile()
                                                 && fmpacks(folder) == static_cast<int>(want.size()) && !anyTemp(folder)
                                             ? 1 : 0, 1);
            }
            else
            {
                const std::string step = row["step"].toString().toStdString();
                const std::string reason = row["reason"].toString().toStdString();
                // A file larger than its entry is read no further than a byte past it, so its full size is not known:
                // the store says it is larger (CAP), where the website's check, holding the whole file, says how large.
                const bool capped = c == "file-size";
                const bool says = capped ? st.reason.rfind(reason.substr(0, reason.find(": ") + 2) + "its file is larger than", 0) == 0
                                         : st.reason == reason;
                P.eq(label + ".refused", settled && st.status == fsx::Status::failed && st.failure == fsx::Failure::check && s->set() == nullptr
                                             ? 1 : 0, 1);
                P.eq(label + ".step_" + step, stepName(st.step) == step ? 1 : 0, 1);
                P.eq(label + ".reason", says ? 1 : 0, 1);
                const std::vector<std::string> left = filesIn(folder);
                const bool clean = std::none_of(left.begin(), left.end(), [](const std::string& f) { return f != "state.json"; });
                P.eq(label + ".nothing_kept", clean ? 1 : 0, 1);
            }
        }
        P.ge("case.judged", judged, 3);
    }

    // ---- GATE ----------------------------------------------------------------------------------------------------------
    {
        const juce::File home = freshHome();
        auto site = std::make_shared<Site>(fixtures().getChildFile("good-1"));
        {
            const std::unique_ptr<fsx::Store> s = storeOf(me, home, site, fsx::Answer::notAsked);
            P.eq("gate.not_asked.status", s->state().status == fsx::Status::notAsked && s->state().loaded ? 1 : 0, 1);
            s->editorOpened();
            (void)settle(*s);
            P.eq("gate.not_asked.nothing_asked", site->requests().empty() && s->state().status == fsx::Status::notAsked ? 1 : 0, 1);
            s->setAnswer(fsx::Answer::no);
            s->editorOpened();
            s->refreshNow();
            (void)settle(*s);
            P.eq("gate.no.nothing_asked", site->requests().empty() && s->state().status == fsx::Status::declined ? 1 : 0, 1);
        }
        P.eq("gate.answer_kept", home.getChildFile("factory.json").loadFileAsString().contains("\"no\"") ? 1 : 0, 1);
        {
            // Asked in one product, the answer holds for the other's store too (one file, one level up).
            std::string otherKey;
            for (const juce::var& row : *cases.getArray())
                if (row["key"].toString().toStdString() != own)
                    otherKey = row["key"].toString().toStdString();
            const std::unique_ptr<fsx::Store> other = storeOf(identityOf(otherKey), home, site, fsx::Answer::notAsked);
            P.eq("gate.answer_shared", other->answer() == fsx::Answer::no ? 1 : 0, 1);
        }
        {
            const std::unique_ptr<fsx::Store> s = storeOf(me, home, site, fsx::Answer::yes);
            P.eq("gate.yes_no_editor.nothing_asked", site->requests().empty() && s->state().status == fsx::Status::none ? 1 : 0, 1);
        }
        {
            // The offer's answer through the processor (EditorPort::answerOffer, package 08's card): a yes asks nothing
            // before an editor opens, and the editor's open fetches; a no, nothing even then.
            for (const bool yes : { true, false })
            {
                const juce::File h = freshHome();
                auto s = std::make_shared<Site>(fixtures().getChildFile("good-1"));
                const fsx::Config c = configOf(me, h, s);
                fsx::useConfigForTests(&c);
                {
                    pg::Processor a;
                    std::shared_ptr<fsx::Store> store = fsx::process();
                    if (store != nullptr)
                        (void)settle(*store);
                    a.editorPort().answerOffer(true, yes, false);
                    if (store != nullptr)
                        (void)settle(*store);
                    const std::string label = yes ? "gate.offer_yes" : "gate.offer_no";
                    P.eq(label + ".answer", store != nullptr && store->answer() == (yes ? fsx::Answer::yes : fsx::Answer::no) ? 1 : 0, 1);
                    P.eq(label + ".nothing_before_an_editor", s->requests().empty() ? 1 : 0, 1);
                    a.editorPort().editorOpened();
                    if (store != nullptr)
                        (void)settle(*store);
                    P.eq(label + (yes ? ".editor_fetches" : ".editor_asks_nothing"), s->asked("index.json") == yes ? 1 : 0, 1);
                }
                fsx::useConfigForTests(nullptr);
            }
        }
        {
            fsx::Config c = configOf(me, home, site);
            c.off = true;
            fsx::Store s(c);
            s.setAnswer(fsx::Answer::yes);
            s.start();
            s.editorOpened();
            s.refreshNow();
            (void)settle(s);
            P.eq("gate.off.nothing_asked", site->requests().empty() && s.state().status == fsx::Status::off ? 1 : 0, 1);
        }
        {
            const EnvScope env(std::string(pg::product::kEnvPrefix) + "FACTORY");
            env.set("off");
            const bool off = fsx::processConfig().off;
            env.set(" OFF ");
            const bool offCase = fsx::processConfig().off;
            env.set("on");
            const bool on = !fsx::processConfig().off;
            env.unset();
            const bool unset = !fsx::processConfig().off;
            P.eq("gate.env_off", off && offCase && on && unset ? 1 : 0, 1);
        }
        {
            const fsx::Config c = fsx::processConfig();
            bool builtIn = c.keys.size() == pg::factory::kPublicKeys.size();
            for (std::size_t i = 0; builtIn && i < c.keys.size(); ++i)
                builtIn = c.keys[i].id == pg::factory::kPublicKeys[i].id && c.keys[i].point == pg::factory::kPublicKeys[i].point;
            P.eq("gate.process_keys_built_in", builtIn ? 1 : 0, 1);
            P.eq("gate.process_identity", c.key == own && c.kind == pg::product::kKind && c.plugin == pg::product::kName
                                              && c.version == pg::product::kVersion
                                              && juce::File(c.folder).isAChildOf(fsx::directory())
                                          ? 1 : 0, 1);
            const char* sandbox = std::getenv((std::string(pg::product::kEnvPrefix) + "PREFS_DIR").c_str());
            P.eq("gate.sandboxed", sandbox != nullptr && fsx::directory().isAChildOf(juce::File(juce::String::fromUTF8(sandbox))) ? 1 : 0, 1);
        }
        {
            const std::string def = fsx::kSite;
            const bool sites = fsx::siteFrom(nullptr) == def && fsx::siteFrom("") == def
                            && fsx::siteFrom("https://staging.example.test") == "https://staging.example.test"
                            && fsx::siteFrom("https://staging.example.test/") == "https://staging.example.test"
                            && fsx::siteFrom("http://127.0.0.1:8787") == "http://127.0.0.1:8787"
                            && fsx::siteFrom("http://localhost:8787/") == "http://localhost:8787"
                            && fsx::siteFrom("http://example.test") == def && fsx::siteFrom("https://x.test/path") == def
                            && fsx::siteFrom("ftp://x.test") == def && fsx::siteFrom("https://user@x.test") == def
                            && fsx::siteFrom("http://localhost:87a") == def;
            P.eq("gate.site", sites ? 1 : 0, 1);
        }
    }

    // ---- FIRST, UPDATE -------------------------------------------------------------------------------------------------
    {
        const juce::File home = freshHome();
        const juce::File folder = folderOf(home, own);
        auto site = std::make_shared<Site>(fixtures().getChildFile("good-1"));
        const std::unique_ptr<fsx::Store> s = storeOf(me, home, site);
        const uint64_t before = s->generation();
        s->editorOpened();
        (void)settle(*s);
        say("first", s->state());
        P.eq("first.set", s->state().status == fsx::Status::ready && ownNames(s->set()) == namesOf(good1) && s->set()->fetched
                              && s->set()->sequence == 1 ? 1 : 0, 1);
        P.eq("first.generation_moved", s->generation() > before && s->state().generation == s->generation() ? 1 : 0, 1);
        P.eq("first.requests", static_cast<int64_t>(site->requests().size()), static_cast<int64_t>(1 + good1.size()));
        P.eq("first.on_disk", folder.getChildFile("index.json").loadFileAsString()
                                      == fixtures().getChildFile("good-1" + indexPath(own)).loadFileAsString()
                                  && fmpacks(folder) == static_cast<int>(good1.size()) && !anyTemp(folder)
                                  && folder.getChildFile("state.json").loadFileAsString().contains("\"sequence\":1")
                              ? 1 : 0, 1);
        const pg::claude::Headers h = site->lastHeaders();
        P.eq("first.user_agent_alone", h.size() == 1 && h[0].first == "User-Agent"
                                           && h[0].second == std::string(pg::product::kName) + "/" + pg::product::kVersion
                                           && site->lastHost() == kSite ? 1 : 0, 1);
        P.eq("first.checked", s->state().checkedMs == clockMs.load() ? 1 : 0, 1);

        // A second store on the folder (another host, the Standalone): the set on disk, no request.
        {
            auto quiet = std::make_shared<Site>(fixtures().getChildFile("good-1"));
            const std::unique_ptr<fsx::Store> second = storeOf(me, home, quiet);
            P.eq("first.second_store_reads_disk", second->state().status == fsx::Status::ready && ownNames(second->set()) == namesOf(good1)
                                                      && quiet->requests().empty() ? 1 : 0, 1);
        }

        // The same index again: the index alone; the set, its generation and the disk as they were.
        site->clearRequests();
        const uint64_t g1 = s->generation();
        clockMs += 1000;
        s->refreshNow();
        (void)settle(*s);
        P.eq("first.same_index.index_alone", site->requests() == std::vector<std::string>{ indexPath(own) } ? 1 : 0, 1);
        P.eq("first.same_index.unchanged", s->generation() == g1 && s->state().status == fsx::Status::ready
                                               && s->state().checkedMs == clockMs.load() ? 1 : 0, 1);
        // An editor's open again: nothing within a day (and an editor's open asks once a store).
        site->clearRequests();
        s->editorOpened();
        (void)settle(*s);
        P.eq("first.editor_again.nothing", site->requests().empty() ? 1 : 0, 1);
        {
            // A day and more later, a new store (a later session): an editor's open asks for the index alone.
            clockMs += fsx::kRecheckMs + 1000;
            auto later = std::make_shared<Site>(fixtures().getChildFile("good-1"));
            const std::unique_ptr<fsx::Store> next = storeOf(me, home, later);
            next->editorOpened();
            (void)settle(*next);
            P.eq("first.after_a_day.index_alone", later->requests() == std::vector<std::string>{ indexPath(own) } ? 1 : 0, 1);
            const std::unique_ptr<fsx::Store> soon = storeOf(me, home, later);
            later->clearRequests();
            soon->editorOpened();
            (void)settle(*soon);
            P.eq("first.within_a_day.nothing", later->requests().empty() ? 1 : 0, 1);
        }

        // good-2: added, changed, withdrawn; only the files the folder lacks asked for.
        site->serve(fixtures().getChildFile("good-2"));
        site->clearRequests();
        s->refreshNow();
        (void)settle(*s);
        say("update", s->state());
        std::set<std::string> oldHashes, newHashes;
        for (const FixtureEntry& e : good1)
            oldHashes.insert(e.sha256);
        for (const FixtureEntry& e : good2)
            newHashes.insert(e.sha256);
        std::vector<std::string> want = { indexPath(own) };
        for (const FixtureEntry& e : good2)
            if (oldHashes.count(e.sha256) == 0)
                want.push_back(filePath(e.sha256));
        P.eq("update.taken", s->state().status == fsx::Status::ready && ownNames(s->set()) == namesOf(good2) && s->set()->sequence == 2
                                 && s->generation() > g1 ? 1 : 0, 1);
        P.eq("update.only_new_files", sorted(site->requests()) == sorted(want) ? 1 : 0, 1);
        P.ge("update.some_unchanged", static_cast<double>(good2.size() + 1 - want.size()), 1.0);
        bool withdrawnGone = true;
        for (const std::string& h1 : oldHashes)
            if (newHashes.count(h1) == 0)
                withdrawnGone = withdrawnGone && !folder.getChildFile("p/" + juce::String(h1) + ".fmpack").exists();
        P.eq("update.withdrawn_file_gone", withdrawnGone && fmpacks(folder) == static_cast<int>(good2.size()) ? 1 : 0, 1);

        // good-1 again: older than one taken, refused; the set and the disk as they were.
        const std::string indexBefore = folder.getChildFile("index.json").loadFileAsString().toStdString();
        const uint64_t g2 = s->generation();
        site->serve(fixtures().getChildFile("good-1"));
        s->refreshNow();
        (void)settle(*s);
        say("older", s->state());
        P.eq("update.older_refused", s->state().step == fsx::Step::sequence && s->state().failure == fsx::Failure::check
                                         && s->state().status == fsx::Status::ready && ownNames(s->set()) == namesOf(good2)
                                         && s->generation() == g2 ? 1 : 0, 1);
        P.eq("update.older_disk_unchanged", folder.getChildFile("index.json").loadFileAsString().toStdString() == indexBefore
                                                && folder.getChildFile("state.json").loadFileAsString().contains("\"sequence\":2") ? 1 : 0, 1);

        // key-change: sequence 3, signed with the next key (t2): taken.
        site->serve(fixtures().getChildFile("key-change"));
        s->refreshNow();
        (void)settle(*s);
        P.eq("update.key_change", s->state().status == fsx::Status::ready && s->set()->sequence == 3 && s->state().failure == fsx::Failure::none
                                      ? 1 : 0, 1);

        // Offline with a set: the set kept, ready (the UI shows no error), the failure said for whoever wants it.
        site->setOffline(true);
        s->refreshNow();
        (void)settle(*s);
        P.eq("offline.with_set", s->state().status == fsx::Status::ready && s->set()->sequence == 3
                                     && s->state().failure == fsx::Failure::connection ? 1 : 0, 1);
    }

    // ---- NEWER ---------------------------------------------------------------------------------------------------------
    if (fixtures().getChildFile("needs-newer" + indexPath(own)).existsAsFile())
    {
        const juce::File home = freshHome();
        auto site = std::make_shared<Site>(fixtures().getChildFile("needs-newer"));
        const std::unique_ptr<fsx::Store> s = storeOf(me, home, site);
        s->refreshNow();
        (void)settle(*s);
        bool leftOut = false, neverAsked = true;
        for (const FixtureEntry& e : entriesOf("needs-newer", own))
            if (e.needs == "99.0.0")
            {
                const std::vector<std::string> names = ownNames(s->set());
                leftOut = std::find(names.begin(), names.end(), e.name) == names.end();
                neverAsked = !site->asked(e.sha256);
            }
        P.eq("newer.left_out", s->state().status == fsx::Status::ready && leftOut ? 1 : 0, 1);
        P.eq("newer.file_never_asked", neverAsked ? 1 : 0, 1);
    }
    else
    {
        probe::skip(P, "newer", "needs-newer is the synth's fixture alone: the synth's run judges it");
    }

    // ---- OFFLINE (no set) ----------------------------------------------------------------------------------------------
    {
        auto site = std::make_shared<Site>(fixtures().getChildFile("good-1"));
        site->setOffline(true);
        const std::unique_ptr<fsx::Store> s = storeOf(me, freshHome(), site);
        s->editorOpened();
        (void)settle(*s);
        const fsx::State st = s->state();
        say("offline", st);
        P.eq("offline.no_set", st.status == fsx::Status::failed && st.failure == fsx::Failure::connection
                                   && st.reason.find("no connection") != std::string::npos && s->set() == nullptr ? 1 : 0, 1);
    }

    // ---- RESUME ----------------------------------------------------------------------------------------------------------
    {
        const juce::File home = freshHome();
        const juce::File folder = folderOf(home, own);
        auto site = std::make_shared<Site>(fixtures().getChildFile("good-1"));
        site->failOn(filePath(good1.at(1).sha256));
        const std::unique_ptr<fsx::Store> s = storeOf(me, home, site);
        s->refreshNow();
        (void)settle(*s);
        P.eq("resume.cut_short", s->state().status == fsx::Status::failed && s->state().failure == fsx::Failure::connection
                                     && s->set() == nullptr && !folder.getChildFile("index.json").exists()
                                     && folder.getChildFile("p/" + juce::String(good1[0].sha256) + ".fmpack").existsAsFile()
                                     && !anyTemp(folder) ? 1 : 0, 1);
        site->failOn({});
        site->clearRequests();
        s->refreshNow();
        (void)settle(*s);
        P.eq("resume.taken", s->state().status == fsx::Status::ready && ownNames(s->set()) == namesOf(good1) ? 1 : 0, 1);
        P.eq("resume.first_file_not_asked_again", !site->asked(good1[0].sha256) && site->asked(good1[1].sha256) ? 1 : 0, 1);
    }

    // ---- CORRUPT ---------------------------------------------------------------------------------------------------------
    {
        const juce::File home = freshHome();
        const juce::File folder = folderOf(home, own);
        auto site = std::make_shared<Site>(fixtures().getChildFile("good-1"));
        {
            const std::unique_ptr<fsx::Store> s = storeOf(me, home, site);
            s->refreshNow();
            (void)settle(*s);
        }
        const juce::File damaged = folder.getChildFile("p/" + juce::String(good1[0].sha256) + ".fmpack");
        juce::MemoryBlock bytes;
        (void)damaged.loadFileAsData(bytes);
        static_cast<char*>(bytes.getData())[bytes.getSize() - 1] ^= 0x01;
        (void)damaged.replaceWithData(bytes.getData(), bytes.getSize());
        site->clearRequests();
        const std::unique_ptr<fsx::Store> s = storeOf(me, home, site);
        say("corrupt", s->state());
        P.eq("corrupt.refused_on_disk", s->set() == nullptr && s->state().step == fsx::Step::file
                                            && s->state().failure == fsx::Failure::check && site->requests().empty() ? 1 : 0, 1);
        s->editorOpened();
        (void)settle(*s);
        P.eq("corrupt.fetched_afresh", s->state().status == fsx::Status::ready && ownNames(s->set()) == namesOf(good1) ? 1 : 0, 1);
        P.eq("corrupt.that_file_alone", sorted(site->requests()) == sorted({ indexPath(own), filePath(good1[0].sha256) }) ? 1 : 0, 1);
    }

    // ---- TWO -------------------------------------------------------------------------------------------------------------
    {
        const juce::File home = freshHome();
        const juce::File folder = folderOf(home, own);
        auto siteA = std::make_shared<Site>(fixtures().getChildFile("good-1"));
        auto siteB = std::make_shared<Site>(fixtures().getChildFile("good-1"));
        const std::unique_ptr<fsx::Store> a = storeOf(me, home, siteA), b = storeOf(me, home, siteB);
        a->refreshNow();
        b->refreshNow();
        (void)settle(*a);
        (void)settle(*b);
        P.eq("two.both_ready", a->state().status == fsx::Status::ready && b->state().status == fsx::Status::ready
                                   && ownNames(a->set()) == ownNames(b->set()) ? 1 : 0, 1);
        P.eq("two.one_set_on_disk", folder.getChildFile("index.json").loadFileAsString()
                                            == fixtures().getChildFile("good-1" + indexPath(own)).loadFileAsString()
                                        && fmpacks(folder) == static_cast<int>(good1.size()) && !anyTemp(folder) ? 1 : 0, 1);

        // An older set and a newer one taken at once, again and again: the disk ends with the newer, whichever lands last.
        int kept = 0;
        constexpr int kRaces = 8;
        for (int i = 0; i < kRaces; ++i)
        {
            const juce::File raceHome = freshHome();
            auto older = std::make_shared<Site>(fixtures().getChildFile("good-1"));
            auto newer = std::make_shared<Site>(fixtures().getChildFile("good-2"));
            const std::unique_ptr<fsx::Store> x = storeOf(me, raceHome, older), y = storeOf(me, raceHome, newer);
            x->refreshNow();
            y->refreshNow();
            (void)settle(*x);
            (void)settle(*y);
            const juce::File raceFolder = folderOf(raceHome, own);
            const bool newest = raceFolder.getChildFile("state.json").loadFileAsString().contains("\"sequence\":2")
                             && raceFolder.getChildFile("index.json").loadFileAsString()
                                    == fixtures().getChildFile("good-2" + indexPath(own)).loadFileAsString()
                             && y->set() != nullptr && y->set()->sequence == 2 && !anyTemp(raceFolder);
            // The disk's set reads as a whole: a third store finds it usable.
            auto quiet = std::make_shared<Site>(fixtures().getChildFile("good-2"));
            const std::unique_ptr<fsx::Store> z = storeOf(me, raceHome, quiet);
            kept += newest && z->set() != nullptr && z->set()->sequence == 2 ? 1 : 0;
        }
        P.eq("two.newer_never_lost", kept, kRaces);
    }

    // ---- CAP -------------------------------------------------------------------------------------------------------------
    {
        const juce::File home = freshHome();
        auto site = std::make_shared<Site>(fixtures().getChildFile("good-1"));
        const std::string path = filePath(good1[0].sha256);
        constexpr std::size_t kMore = 1 << 20;
        site->extraOn(path, kMore);
        const std::unique_ptr<fsx::Store> s = storeOf(me, home, site);
        s->refreshNow();
        (void)settle(*s);
        say("cap", s->state());
        P.eq("cap.refused", s->set() == nullptr && s->state().step == fsx::Step::file
                                && s->state().reason.find("larger than") != std::string::npos ? 1 : 0, 1);
        P.le("cap.read_no_further", static_cast<double>(site->delivered(path)), static_cast<double>(good1[0].bytes + 256));
    }

    // ---- REDIRECT --------------------------------------------------------------------------------------------------------
    {
        auto site = std::make_shared<Site>(fixtures().getChildFile("good-1"));
        site->redirectOn(indexPath(own));
        const std::unique_ptr<fsx::Store> s = storeOf(me, freshHome(), site);
        s->refreshNow();
        (void)settle(*s);
        say("redirect", s->state());
        P.eq("redirect.refused", s->set() == nullptr && s->state().step == fsx::Step::address
                                     && s->state().failure == fsx::Failure::check ? 1 : 0, 1);
        P.eq("redirect.not_followed", site->requests() == std::vector<std::string>{ indexPath(own) } && site->lastHost() == kSite ? 1 : 0, 1);
    }

    // ---- CANCEL ----------------------------------------------------------------------------------------------------------
    {
        auto site = std::make_shared<Site>(fixtures().getChildFile("good-1"));
        site->stallOn(filePath(good1[0].sha256));
        {
            auto s = storeOf(me, freshHome(), site);
            s->refreshNow();
            s->cancel();                                 // nothing in flight yet, or the fetch's start: either ends
            // cancel() ends only a fetch in flight: one it came before the thread took is still to run, and stalls.
            // So wait for either, and ask for a second fetch only when the first ended (a refreshNow() meanwhile
            // would leave one wanted, which would start, and stall, as soon as the cancel below ended the first).
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!s->idle() && site->stalling() == 0 && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (site->stalling() == 0)
                s->refreshNow();
            while (site->stalling() == 0 && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            P.eq("cancel.in_flight", site->stalling(), 1);
            s->cancel();
            const bool ended = settle(*s, 2000);
            P.eq("cancel.ends", ended && site->stalling() == 0 && s->state().failure == fsx::Failure::none && s->set() == nullptr
                                    && !s->state().fetching ? 1 : 0, 1);
            s->refreshNow();
            while (site->stalling() == 0 && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const auto t0 = std::chrono::steady_clock::now();
            s.reset();                                   // destroyed with a fetch in flight: cancelled and joined
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::printf("  destroyed in flight in %.1f ms\n", ms);
            P.le("cancel.join_ms", ms, 500.0);
        }
    }

    // ---- SEAM ------------------------------------------------------------------------------------------------------------
    {
        const juce::File projects = base.getChildFile("projects");
        ps::setDirectoryForTests(projects);
        P.eq("seam.built_in_without_a_set", fy::current() == fy::builtIn() && !fy::current()->fetched && fsx::process() == nullptr ? 1 : 0, 1);

        // A set on disk, as an earlier session left it.
        const juce::File home = freshHome();
        auto site = std::make_shared<Site>(fixtures().getChildFile("good-1"));
        {
            const std::unique_ptr<fsx::Store> s = storeOf(me, home, site);
            s->refreshNow();
            (void)settle(*s);
        }
        const fsx::Config c = configOf(me, home, site);
        fsx::useConfigForTests(&c);
        const uint64_t g0 = fy::generation();
        {
            pg::Processor a;
            std::shared_ptr<fsx::Store> store = fsx::process();
            P.eq("seam.processor_attached", store != nullptr ? 1 : 0, 1);
            if (store != nullptr)
                (void)settle(*store);
            const fy::Snapshot now = fy::current();
            P.eq("seam.fetched_set_current", now != nullptr && now->fetched && ownNames(now) == namesOf(good1) && fy::generation() > g0
                                                 ? 1 : 0, 1);
            pg::EditorPort& port = a.editorPort();
            std::vector<std::string> listed;
            for (const pg::ProjectEntry& e : port.factoryProjects())
                listed.push_back(pg::ownName(e.name));
            P.eq("seam.port_lists_it", sorted(listed) == namesOf(good1) && port.factoryGeneration() == fy::generation() ? 1 : 0, 1);
            std::vector<std::string> storeList;
            for (const ps::Entry& e : ps::factoryList())
                storeList.push_back(e.name);
            bool factoryNames = !storeList.empty();
            for (const std::string& n : storeList)
                factoryNames = factoryNames && pg::isFactoryName(n);
            P.eq("seam.store_lists_it", factoryNames && storeList.size() == good1.size() ? 1 : 0, 1);

            // Each project served by its factory name, read-only.
            const std::string name = pg::factoryName(good1[0].name);
            std::string why;
            const std::optional<ps::Project> loaded = ps::load(name, &why);
            P.eq("seam.load", loaded && loaded->name == name && loaded->versions.size() == 1 && ps::exists(name) ? 1 : 0, 1);
            P.eq("seam.presets", ps::presetsOf(name).has_value() ? 1 : 0, 1);
            P.eq("seam.read_only", !ps::saveVersion(name, loaded ? loaded->latest().files : pg::Sources{}, 0, {}, {}, {}, nullptr, &why)
                                        && why.find("factory") != std::string::npos ? 1 : 0, 1);

            // A set fetched later: good-2, its withdrawn project no longer served, the generation moved.
            std::string withdrawn;
            for (const FixtureEntry& e : good1)
                if (std::none_of(good2.begin(), good2.end(), [&](const FixtureEntry& f) { return f.name == e.name; }))
                    withdrawn = e.name;
            const uint64_t g1 = fy::generation();
            site->serve(fixtures().getChildFile("good-2"));
            if (store != nullptr)
            {
                store->refreshNow();
                (void)settle(*store);
            }
            P.eq("seam.later_set", ownNames(fy::current()) == namesOf(good2) && fy::generation() > g1 ? 1 : 0, 1);
            P.eq("seam.withdrawn_not_served", !withdrawn.empty() && !ps::exists(pg::factoryName(withdrawn))
                                                  && !ps::load(pg::factoryName(withdrawn)).has_value()
                                                  && fy::find(withdrawn) == nullptr ? 1 : 0, 1);
            P.eq("seam.projects_read_moves", port.projectsRead() >= fy::generation() ? 1 : 0, 1);
        }
        const uint64_t g2 = fy::generation();
        P.eq("seam.last_processor_takes_it_back", fsx::process() == nullptr && fy::current() == fy::builtIn() && g2 > g0 ? 1 : 0, 1);
        fsx::useConfigForTests(nullptr);
        ps::setDirectoryForTests(juce::File());
    }

    base.deleteRecursively();
    return P.finish();
}
