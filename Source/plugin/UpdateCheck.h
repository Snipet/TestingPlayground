// Source/plugin/UpdateCheck.h: whether a newer version of the plugin is out, as the website says (docs/plan/
// 08-updates-and-whats-new.md, sections 2.1 to 2.4), and the user's answers that govern it, in app.json.
//
//   <site>/app/v1/release.json    one small JSON file the website builds from its release (format
//                                 "funk-playground-release", version 1): `latest` {version, tag, date, summary} and
//                                 `prices` (the chat's price table, handed to usage::offerPrices). Read no further than
//                                 kMaxBytes. Not signed, by design: nothing in it is an address that is followed or
//                                 anything that runs (Download opens the plugin's own fixed page, kDownloadPath), so a
//                                 forged copy can only nag or stay silent. The format only gains optional fields: what is
//                                 not known is ignored; another `format` word, a `version` not 1, or a file that does not
//                                 parse is refused.
//
// The site is the factory store's (factorystore::kSite, or <ENV_PREFIX>FACTORY_SITE: factorystore::siteFrom), and a
// request sends a User-Agent of the plugin's name and version and nothing else, following no redirect.
//
// app.json, in the folder both products share (factorystore::directory(): Funk/Playground/, beside factory.json; a
// probe's sandbox's factory/), holds {"updates": "yes" | "no" | "later", "skipped": "0.3.0", "checkedMs": ms since 1970
// of the site's last answer, "latest": {version, tag, date, summary}, "prices": the last table taken, "seen": the
// version whose What's new the user last saw (setSeen; plugin/Changelog.h)}; other fields are kept as they are. Every write reads the file again under the
// folder's lock (.lock) and sets only the fields this process changed, then writes beside and renames over: two
// processes (two products) never lose each other's answer. "later" is the offer's Not now: the update check off, and
// the offer not made again by itself.
//
// When (all of): an editor has opened in this process (editorOpened(): each open checks, when the last check is
// kRecheckMs old) or checkNow() is called (whatever the last check was); the answer is yes; <ENV_PREFIX>UPDATES is not
// "off" (the probes' environment sets it, cmake/Probes.cmake). A host scanning plugins, a processor made with no editor,
// a probe: none of them ever causes a request. A check that fails (no connection, a redirect, a reply too large or that
// does not read) says why in words and keeps the last good `latest`; it is tried again a day later (or at Check now).
//
// Threads: one Check a process (attach(): the first processor makes and starts it, the last one gone stops and joins its
// thread, before a host can unload the plugin; no thread is detached, nothing runs in a static destructor), with one
// thread of its own that does all of its disk and network work, catching whatever that work throws (the check fails, in
// words). Nothing here is called from the audio thread, and no call waits on the thread's work: state() copies what it
// last published under a short lock.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace playground::claude
{
    class Transport;
}

namespace playground::updates
{
    inline constexpr const char* kReleasePath = "/app/v1/release.json";
    inline constexpr const char* kDownloadPath = "/download";   // the page Download opens: the site's, fixed
    inline constexpr const char* kFormat = "funk-playground-release";
    inline constexpr int kFormatVersion = 1;
    inline constexpr std::size_t kMaxBytes = 64 * 1024;
    inline constexpr int64_t kRecheckMs = int64_t{ 24 } * 60 * 60 * 1000;

    // The user's answer: not asked yet; yes (check); no (the Network page's switch off); later (the offer's Not now).
    enum class Answer { notAsked, yes, no, later };

    // off: <ENV_PREFIX>UPDATES is "off", or the answer is no or later. unanswered: not asked. idle: a yes, no check under
    // way, the last one (if any) answered. checking: one under way. failed: the last one failed (State::why).
    enum class Status { off, unanswered, idle, checking, failed };

    struct Release
    {
        std::string version, tag, date, summary;   // "0.2.0", "v0.2.0", "2026-11-20", one line (may be empty)
    };

    struct State
    {
        Status status = Status::unanswered;
        Answer answer = Answer::notAsked;
        bool loaded = false;                       // app.json has been read
        bool envOff = false;                       // <ENV_PREFIX>UPDATES=off
        Release latest;                            // the site's last good answer ("" version: none yet)
        bool newer = false;                        // latest.version is above this plugin's
        std::string current;                       // this plugin's version
        std::string skipped;                       // the version the user skipped ("" none)
        std::string seen;                          // the version whose What's new was last shown ("" never)
        bool seenFresh = false;                    // seen read again from app.json since an editor last opened
        int64_t checkedMs = 0;                     // the site's last answer (ms since 1970); 0: never
        std::string why;                           // the last check's failure, in words ("" none)
        uint64_t revision = 0;                     // moves whenever any of the above does
    };

    // What a check works with. The process's is processConfig(); a probe makes its own (a file, a scripted transport and
    // clock, a version).
    struct Config
    {
        std::string file;                          // app.json (absolute)
        std::string version;                       // this plugin's ("0.1.0", or a pre-release "0.2.0-rc.1")
        std::string site;                          // the origin, no trailing slash
        std::shared_ptr<claude::Transport> transport;
        std::function<int64_t()> now;              // ms since 1970
        bool off = false;                          // <ENV_PREFIX>UPDATES=off: never check
        std::string userAgent;
        bool offersPrices = true;                  // a good reply's prices go to usage::offerPrices
    };

    // The process's: app.json in factorystore::directory(), this plugin's version, the site the factory store uses,
    // HTTPS and the clock, <ENV_PREFIX>UPDATES.
    Config processConfig();
    // Whether the environment's value of <ENV_PREFIX>UPDATES turns the check off ("off", any case, spaces aside).
    bool offFrom(const char* environment);

    // Version order: "1.2.3", optionally "-<pre-release>" after it, which sorts below its release ("0.2.0-rc.1" <
    // "0.2.0"); two pre-releases of one version by their text. -1, 0, 1; and false from `valid` for a text that is no
    // version (it then compares below every version).
    int compareVersions(const std::string& a, const std::string& b);
    bool validVersion(const std::string& v);

    // A release file's text read: its `latest` and its prices (the asOf date "" when none came), or why not, in words.
    struct Parsed
    {
        bool ok = false;
        std::string why;
        Release latest;
        std::string pricesJson;                    // the `prices` object as JSON ("" none)
        std::string pricesAsOf;
    };
    Parsed parse(const std::string& text);
    // A `prices` object (the release file's, or app.json's copy) offered to the usage ledger (usage::offerPrices): true
    // when it took it (a later date than the table in use). Rows it cannot take (no `in` or `out`) are left out.
    bool offerPricesJson(const std::string& pricesJson);

    class Check
    {
    public:
        explicit Check(Config config);
        ~Check();                                  // cancels, joins
        Check(const Check&) = delete;
        Check& operator=(const Check&) = delete;

        void start();                              // reads app.json, on the check's thread

        State state() const;
        uint64_t revision() const noexcept;
        Answer answer() const;
        // The answer, kept at once and written to app.json on the thread. A yes asks for a check when an editor has
        // opened (or one was asked for) and the last is a day old.
        void setAnswer(Answer a);
        void skip(const std::string& version);     // the pill and the card no longer show this version
        // What's new was shown for `version` (by itself or by hand): kept at once, written to app.json on the thread,
        // whatever the answer and <ENV_PREFIX>UPDATES (What's new reaches no network). Shared by both products.
        void setSeen(const std::string& version);
        // An editor of this process opened: a check, when one is due; What's new's seen read again (State::seenFresh).
        void editorOpened();
        void checkNow();                           // a check now, when the answer is yes, however recent the last
        // The status line's "VERSION 0.2.0 IS OUT", once a process: true the first time it is asked for `version`.
        bool announce(const std::string& version);

        // For probes: true when nothing is asked of the thread and nothing runs on it; how many requests it made; a check
        // in flight ended (the check stays usable).
        bool idle() const;
        int requests() const noexcept;
        void cancel();

    private:
        struct Impl;
        std::unique_ptr<Impl> s_;
    };

    // The process's check, made (with processConfig()) and started by the first processor that asks, gone (its thread
    // joined) with the last holder.
    std::shared_ptr<Check> attach();
    std::shared_ptr<Check> process();

#if defined(FP_PROBES)
    // Probes only: the process's check, when next made, is made from `config` (a copy); null restores processConfig().
    void useConfigForTests(const Config* config);
#endif
}
