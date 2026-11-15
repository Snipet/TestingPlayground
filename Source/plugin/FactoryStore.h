// Source/plugin/FactoryStore.h: the factory projects as the website serves them (the website repository's
// docs/factory-projects.md, sections 4 to 6): fetched, checked, kept on this computer and handed to the rest of the
// plugin as factory::current() (plugin/Factory.h). The checks are the website's scripts/factory.ts's, in its order
// (spec section 5), and make the same decisions for the same bytes; a change of format or of a check there is made here
// in the same step, with the fixtures made again (Tools/probes/fixtures/factory/).
//
//   <site>/factory/v1/<key>/index.json      the signed index of one product (key: effect, synth)
//   <site>/projects/v1/p/<sha256>.fmpack    one project's file, by its hash: the address is made here from the hash,
//                                           so nothing in a download says where to fetch from
//
// The site is kSite, fixed; <ENV_PREFIX>FACTORY_SITE may point a developer's build at another origin (https, or http
// for 127.0.0.1 and localhost alone, as `wrangler dev` serves): the keys stay the built-in ones whatever it says. A
// request sends nothing but a User-Agent of the plugin's name and version, and follows no redirect.
//
// On disk, one folder per user and product, shared by every host and the Standalone (folder()):
//   macOS ~/Library/Application Support/Funk/Playground/Factory/<key>/, Windows %LOCALAPPDATA%\Funk\Playground\Factory\
//   <key>\, Linux $XDG_DATA_HOME (else ~/.local/share)/Funk/Playground/Factory/<key>/
// holding index.json (as downloaded), p/<sha256>.fmpack and state.json ({"sequence": the highest taken, "checked": ms
// since 1970 of the last answer from the site}); the user's answer to the offer one level up, Funk/Playground/
// factory.json ({"answer": "yes" | "no"}), shared by both products. Under a probe's sandbox (<ENV_PREFIX>PREFS_DIR) all
// of it is under that folder's factory/ instead, and setDirectoryForTests moves it anywhere: no probe touches the user's.
// Every file is written beside its place under a name of its own and renamed over it (one step: a reader sees the old
// file or the new one whole); the index and the state of a set are written under a lock on the folder (.lock), after
// the state is read again, so that two processes (or two stores) never lose the higher sequence.
//
// When:
//   - Reading the set on disk: when the store starts (the first processor of the process registers: attach()), on the
//     store's own thread, whatever the answer: the index's signature, then every file's size and hash, then every
//     project, exactly as a download is checked. A set that fails is not used, and is fetched afresh when fetching is
//     allowed.
//   - Fetching (spec 6.2), all of: an editor has opened in this process (editorOpened()) or refreshNow() was called; the
//     answer is yes; <ENV_PREFIX>FACTORY is not "off"; and there is no usable set, or the last check was more than
//     kRecheckMs ago (refreshNow() skips that; an editor's open asks at most once a store). One fetch at a time, on the
//     store's thread: the index; the same bytes as on disk (with a usable set from them) is a check and nothing more;
//     else its checks, its entries that need a newer plugin left out (their files never asked for), the files the
//     folder lacks (each read no further than its entry's size; one already there is checked first), each file's and
//     each project's checks, and only then the index and the state written and the new set published. A set is taken
//     whole or not at all. A file that passed every check is kept as it arrives (it is named by its hash), so a fetch
//     cut short (no connection) leaves the old set whole and the next one fetches only what is left; one refused for a
//     check keeps none of that fetch's files.
//
// Threads: each Store has one thread of its own, which does all of its disk and network work; nothing here is called
// from the audio thread, and no call waits on that thread's work (state(), set() and answer() copy what it last
// published under a short lock). A Store's destructor cancels a fetch in flight (the transport ends within a poll) and
// joins its thread. The process's store (attach()) goes with the last processor that holds it, so its thread is
// stopped and joined before a host can unload the plugin; no thread is detached, and nothing runs in a static
// destructor.
#pragma once

#include "plugin/Factory.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace juce
{
    class File;
}

namespace playground::claude
{
    class Transport;
}

namespace playground::factorystore
{
    inline constexpr const char* kSite = "https://testingplayground.seanfunk.xyz";
    inline constexpr int64_t kRecheckMs = int64_t{ 24 } * 60 * 60 * 1000;   // an editor's open checks at most this often

    // What a plugin takes at most (spec section 5, step 2), checked before anything is parsed.
    inline constexpr std::size_t kMaxIndexBytes = 256 * 1024;
    inline constexpr std::size_t kMaxProjects = 256;
    inline constexpr std::size_t kMaxFileBytes = 4 * 1024 * 1024;
    inline constexpr std::size_t kMaxSetBytes = 32 * 1024 * 1024;

    // The user's answer to the offer (the next piece's card): not asked yet, yes (fetch and keep current), no.
    enum class Answer { notAsked, yes, no };

    // What the FACTORY group can say (spec section 7). In order of precedence:
    //   ready     a set is in use (fetched now or before; whatever the answer: a user who said no keeps what they have)
    //   fetching  none yet, and a fetch is under way
    //   off       none, and <ENV_PREFIX>FACTORY is "off"
    //   notAsked  none, and the user has not answered
    //   declined  none, and the user said no
    //   failed    none, and the last fetch failed (State::failure says how, State::reason in words)
    //   none      none yet, nothing failed: a yes with no fetch so far (no editor opened), or the disk still being read
    enum class Status { ready, fetching, off, notAsked, declined, failed, none };

    // How the last attempt failed: no answer from the site ("no connection", or the site's own error), or what it sent
    // could not be checked (and was not used). Kept while a set is ready too, for the UI to show or not.
    enum class Failure { none, connection, check };

    // Which check refused a set (spec section 5's order; the fixtures' `step`): connection for none (no answer).
    enum class Step { none, connection, address, size, signature, index, sequence, file, project, disk };

    struct State
    {
        Status status = Status::none;
        bool loaded = false;                             // the set on disk has been read (or found to be none)
        bool fetching = false;                           // a fetch is under way (with a set ready, too)
        Answer answer = Answer::notAsked;
        Failure failure = Failure::none;                 // the last attempt's (a set read from disk, or a fetch)
        Step step = Step::none;
        std::string reason;                              // in words a person can read; "" when nothing failed
        int64_t checkedMs = 0;                           // the site's last answer (ms since 1970); 0: never
        int64_t sequence = 0;                            // the set in use's (0: none)
        int projects = 0;                                // in the set in use
        uint64_t generation = 0;                         // moves whenever set() does
    };

    // What a store works with. The process's is processConfig(); a probe makes its own (a folder, a scripted transport,
    // the fixtures' keys, another product's identity), and a store made from one never reaches factory::current().
    struct Config
    {
        std::string folder;                              // Factory/<key>/ (absolute)
        std::string answerFile;                          // factory.json (absolute)
        std::string key;                                 // the product's in the site's paths: effect, synth
        std::string kind;                                // effect, instrument
        std::string plugin;                              // the product's name, as an index and a project name it
        std::string version;                             // this plugin's ("0.2.0"): an entry that needs a newer one is
                                                         // left out
        std::string site;                                // the origin, no trailing slash
        struct Key
        {
            std::string id;
            std::array<uint8_t, 65> point{};
        };
        std::vector<Key> keys;                           // the public keys an index may name
        std::shared_ptr<claude::Transport> transport;
        std::function<int64_t()> now;                    // ms since 1970
        bool off = false;                                // <ENV_PREFIX>FACTORY=off: never fetch
        std::string userAgent;
    };

    // The process's configuration: this product, the built-in keys (plugin/FactoryKeys.h), the folder (above), the site
    // (kSite, or <ENV_PREFIX>FACTORY_SITE when it is one that may be used), HTTPS (claude::httpsTransport) and the clock.
    Config processConfig();
    // Where a configuration's folders are made from: Funk/Playground/ in the user's data folder, a probe's sandbox's
    // factory/, or what setDirectoryForTests set (an empty path restores the default).
    juce::File directory();
    void setDirectoryForTests(const juce::File& dir);
    // The site <ENV_PREFIX>FACTORY_SITE names when it may be used (https, or http to 127.0.0.1 or localhost; an origin
    // alone), else kSite.
    std::string siteFrom(const char* environment);

    class Store
    {
    public:
        // `publishes`: the process's store, whose set is factory::current()'s.
        explicit Store(Config config, bool publishes = false);
        ~Store();                                        // cancels, joins
        Store(const Store&) = delete;
        Store& operator=(const Store&) = delete;

        void start();                                    // reads the set on disk, on the store's thread

        Answer answer() const;
        // The answer, kept at once for this store and written to factory.json on the store's thread (both products
        // read it). It starts nothing by itself: the next piece's "Get them" also calls refreshNow().
        void setAnswer(Answer a);

        void editorOpened();                             // an editor of this process opened: a fetch, if allowed
        void refreshNow();                               // a fetch now, if allowed, whenever the last check was

        State state() const;
        factory::Snapshot set() const;                   // the set in use, or null
        uint64_t generation() const noexcept;

        // For probes: true when nothing is asked of the thread and nothing runs on it; requests() counts the transport's
        // calls this store made; cancel() ends a fetch in flight (the store stays usable).
        bool idle() const;
        // The store's own answer of its last write of factory.json: true when the answer in use is the one on disk.
        bool answerWritten() const;
        int requests() const noexcept;
        void cancel();

    private:
        struct Impl;
        std::unique_ptr<Impl> s_;
    };

    // The process's store, made (with processConfig()) and started by the first processor that asks, and gone, its
    // thread joined, with the last holder. Each processor holds one for its life.
    std::shared_ptr<Store> attach();
    // The process's store while some processor holds it, else null (never makes one).
    std::shared_ptr<Store> process();

#if defined(FP_PROBES)
    // Probes only (the probe executable alone defines FP_PROBES, cmake/Probes.cmake: a plugin has no such call, so its
    // keys are the built-in ones whatever runs in it): the process's store, when it is next made (no processor holds
    // one), is made from `config` (a copy) instead of processConfig(); null restores processConfig().
    void useConfigForTests(const Config* config);
#endif
}
