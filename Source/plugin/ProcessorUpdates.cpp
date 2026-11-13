// Source/plugin/ProcessorUpdates.cpp: the processor's side of the website (EditorPort's "The website"; docs/plan/
// 08-updates-and-whats-new.md, sections 2.1 to 2.4): the offer's answers, the update check (plugin/UpdateCheck.h) and
// the factory projects' answer (plugin/FactoryStore.h), as the editor's offer card, top bar, update card and the Settings
// card's Network page see them. Message thread; nothing here reaches the network itself: the check's and the store's
// threads do, each only once an editor has opened (editorOpened) and the user said yes.
#include "plugin/Processor.h"

#include "plugin/Changelog.h"
#include "plugin/Launch.h"

#include "Product.h"

#include <cstdlib>

namespace playground
{
    namespace
    {
        UpdateState::Check checkOf(updates::Status s)
        {
            using C = UpdateState::Check;
            switch (s)
            {
                case updates::Status::off: return C::off;
                case updates::Status::unanswered: return C::unanswered;
                case updates::Status::idle: return C::idle;
                case updates::Status::checking: return C::checking;
                case updates::Status::failed: return C::failed;
            }
            return C::off;
        }

        // The factory store's state in a few words, for the Network page.
        std::string factoryWords(const factorystore::State& f)
        {
            switch (f.status)
            {
                case factorystore::Status::ready:
                    return std::to_string(f.projects) + (f.projects == 1 ? " project" : " projects") + " from the website"
                         + (f.fetching ? ", checking for new ones" : "");
                case factorystore::Status::fetching: return "fetching them now";
                case factorystore::Status::off: return "off on this computer (" + std::string(product::kEnvPrefix) + "FACTORY=off)";
                case factorystore::Status::notAsked: return "not asked yet: the ones built in are shown";
                case factorystore::Status::declined: return "off: the ones built in are shown";
                case factorystore::Status::failed: return "could not fetch them: " + f.reason;
                case factorystore::Status::none: return "not fetched yet: the ones built in are shown";
            }
            return {};
        }
    }

    UpdateState Processor::updateState() const
    {
        UpdateState u;
        u.current = product::kVersion;
        bool checkLoaded = false;
        if (updateCheck_ != nullptr)
        {
            const updates::State s = updateCheck_->state();
            u.check = checkOf(s.status);
            checkLoaded = s.loaded;
            u.envOff = s.envOff;
            u.updatesOn = s.answer == updates::Answer::yes;
            u.updatesAnswered = s.answer != updates::Answer::notAsked;
            u.latest = s.latest.version;
            u.date = s.latest.date;
            u.summary = s.latest.summary;
            u.skipped = s.skipped;
            u.newer = s.newer && u.updatesOn && !s.envOff && s.latest.version != s.skipped;
            u.checkedMs = s.checkedMs;
            u.why = s.why;
            u.revision = s.revision;
        }
        bool factoryLoaded = false;
        if (factoryStore_ != nullptr)
        {
            const factorystore::State f = factoryStore_->state();
            factoryLoaded = f.loaded;
            u.factory = f.answer == factorystore::Answer::yes ? 1 : f.answer == factorystore::Answer::no ? 2 : 0;
            u.factoryOff = f.status == factorystore::Status::off;
            u.factoryWords = factoryWords(f);
        }
        u.loaded = checkLoaded && factoryLoaded;
        u.offer = u.loaded && !u.envOff && !u.updatesAnswered && u.factory == 0;
        // What's new: once per version, both products' (app.json's seen), never with <ENV_PREFIX>WHATSNEW=off.
        bool seenFresh = false;
        if (updateCheck_ != nullptr)
        {
            const updates::State s = updateCheck_->state();
            u.seen = s.seen;
            seenFresh = s.seenFresh;
        }
        u.whatsNewOff = updates::offFrom(std::getenv((std::string(product::kEnvPrefix) + "WHATSNEW").c_str()));
        u.whatsNewReady = checkLoaded && seenFresh;
        u.whatsNewDue = u.whatsNewReady && !u.whatsNewOff && u.seen != u.current
                     && !changelog::since(u.seen, u.current).empty();
        return u;
    }

    void Processor::whatsNewShown()
    {
        if (updateCheck_ != nullptr)
            updateCheck_->setSeen(product::kVersion);
    }

    void Processor::editorOpened()
    {
        editorSeen_ = true;
        if (factoryStore_ != nullptr)
            factoryStore_->editorOpened();
        if (updateCheck_ != nullptr)
            updateCheck_->editorOpened();
    }

    void Processor::answerOffer(bool allow, bool factory, bool updates)
    {
        if (!allow)
        {
            // Not now: remembered (the offer is not made again by itself); the factory projects stay unasked, so their
            // switch on the Network page says so.
            if (updateCheck_ != nullptr)
                updateCheck_->setAnswer(updates::Answer::later);
            return;
        }
        if (factoryStore_ != nullptr)
        {
            factoryStore_->setAnswer(factory ? factorystore::Answer::yes : factorystore::Answer::no);
            if (factory && editorSeen_)
                factoryStore_->refreshNow();             // the factory spec's "Get them": a fetch now (an editor asked)
        }
        if (updateCheck_ != nullptr)
            updateCheck_->setAnswer(updates ? updates::Answer::yes : updates::Answer::no);   // a yes checks when due
    }

    void Processor::setUpdates(bool on)
    {
        if (updateCheck_ != nullptr)
            updateCheck_->setAnswer(on ? updates::Answer::yes : updates::Answer::no);
    }

    void Processor::setFactoryProjects(bool on)
    {
        if (factoryStore_ == nullptr)
            return;
        factoryStore_->setAnswer(on ? factorystore::Answer::yes : factorystore::Answer::no);
        if (on && editorSeen_)
            factoryStore_->editorOpened();               // a fetch when the set on disk is missing or a day old
    }

    void Processor::checkForUpdates()
    {
        if (updateCheck_ != nullptr && editorSeen_)
            updateCheck_->checkNow();                    // Check now: an editor's (never before one opened)
    }

    void Processor::skipUpdate(const std::string& version)
    {
        if (updateCheck_ != nullptr)
            updateCheck_->skip(version);
    }

    bool Processor::announceUpdate(const std::string& version)
    {
        return updateCheck_ != nullptr && !version.empty() && updateCheck_->announce(version);
    }

    bool Processor::openDownloadPage(std::string& why)
    {
        // The site's own page, fixed in the build (or a developer's <ENV_PREFIX>FACTORY_SITE): never an address the
        // release file gave.
        const std::string url = factorystore::siteFrom(std::getenv((std::string(product::kEnvPrefix) + "FACTORY_SITE").c_str()))
                              + updates::kDownloadPath;
        return openPage(url, why);
    }

    bool Processor::openLink(const std::string& url, std::string& why)
    {
        // What's new's links: only the repository's pages and the website's (the changelog's own links), never another
        // address, whatever the text compiled in says.
        if (!changelog::linkAllowed(url))
        {
            why = "only the repository's and the website's pages open from here";
            return false;
        }
        return openPage(url, why);
    }

    bool Processor::openPage(const std::string& url, std::string& why)
    {
        if (pageLauncher_)
            return pageLauncher_(url, why);
        // Never from a probe (CTest's sandbox): it opens nothing on the machine that runs it.
        if (const char* sandbox = std::getenv((std::string(product::kEnvPrefix) + "PREFS_DIR").c_str());
            sandbox != nullptr && *sandbox != '\0')
        {
            why = "nothing opens under a probe";
            return false;
        }
#if defined(__linux__)
        std::string launchWhy;                           // xdg-open with the address as its one argument: no shell
        if (launch::detached(launch::openerArgs(url), launchWhy))
            return true;
        why = "the browser did not open: " + launchWhy;
        return false;
#else
        if (juce::URL(juce::String::fromUTF8(url.c_str())).launchInDefaultBrowser())
            return true;
        why = "the browser did not open";
        return false;
#endif
    }
}
