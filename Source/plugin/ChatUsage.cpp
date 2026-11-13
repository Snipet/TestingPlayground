// The chat's side of the usage ledger (Usage.h): each request counted once, the turn's cost for the footer.
#include "plugin/ChatAgent.h"
#include "plugin/Providers.h"
#include "plugin/Usage.h"

namespace playground
{
    void ChatAgent::countUsage(const claude::MessageBuilder& builder, bool other)
    {
        const claude::Usage& u = builder.usage();
        if (!u.reported)
            return;                                // no usage came: a refused request, or a stream that broke at once
        std::string provider = "anthropic";
        std::string model = builder.model();
        if (other)
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            provider = providers::usageName(turnProvider_);
            if (model.empty())
                model = turnProvider_.model;       // a server that names none (Gemini's modelVersion unsaid)
        }
        usage::Key key{ usage::today(), provider, usage::modelName(model), u.speed == "fast" };
        usage::Counts c;
        c.requests = 1;
        c.input = u.input;
        c.output = u.output;
        c.cacheRead = u.cacheRead;
        c.cacheWrite5m = u.cacheWrite5m;
        c.cacheWrite1h = u.cacheWrite1h;
        // A server that said a total and no split: the rest as five minutes', so every token is counted once.
        if (c.cacheWrite5m + c.cacheWrite1h < u.cacheWrite)
            c.cacheWrite5m = u.cacheWrite - c.cacheWrite1h;
        c.webSearches = u.webSearches;
        c.webFetches = u.webFetches;
        if (u.costMicroUsd >= 0)
        {
            c.reportedMicroUsd = u.costMicroUsd;
            c.reportedRequests = 1;
        }
        usage::add(key, c);
        const usage::Cost cost = usage::costOf(key, c);
        const std::lock_guard<std::mutex> lock(mutex_);
        ++turnCost_.requests;
        if (cost.kind == usage::Cost::Kind::none)
            turnCost_.unknown = true;
        else
        {
            turnCost_.microUsd += cost.microUsd;
            turnCost_.estimate = turnCost_.estimate || cost.kind == usage::Cost::Kind::estimate;
        }
    }

    std::string ChatAgent::turnCostText() const
    {
        if (turnCost_.requests == 0 || turnCost_.unknown)
            return {};
        usage::Cost c;
        c.kind = turnCost_.estimate ? usage::Cost::Kind::estimate : usage::Cost::Kind::reported;
        c.microUsd = turnCost_.microUsd;
        return ", " + usage::costText(c);
    }
}

// ---- The Settings card's Usage page (ChatPort::usage, resetUsage) ------------------------------------------------------
namespace playground
{
    namespace
    {
        // The same thing read again: nothing the page keys on moves.
        bool sameRead(const UsageView& a, const UsageView& b)
        {
            return a.read == b.read && a.range == b.range && a.today == b.today && a.rows == b.rows
                && a.pricesAsOf == b.pricesAsOf && a.resetting == b.resetting && a.resetError == b.resetError;
        }

        const char* const kResetFailed = "The usage record could not be emptied (its folder could not be locked or written). "
                                         "Nothing was lost: try Reset again.";

        UsageView readView(usage::Range range)
        {
            UsageView v;
            v.read = true;
            v.range = range;
            usage::Snapshot s = usage::snapshot(range);
            v.today = s.today;
            v.pricesAsOf = usage::pricesInUse().asOf;
            v.rows.reserve(s.rows.size());
            for (usage::Row& r : s.rows)
            {
                UsageRow u;
                u.cost = usage::costOf(r.key, r.counts);   // the listed price, else the table in use
                u.key = std::move(r.key);
                u.counts = r.counts;
                v.rows.push_back(std::move(u));
            }
            return v;
        }
    }

    std::shared_ptr<const UsageView> ChatAgent::usage(usage::Range range)
    {
        bool ask = false;
        std::shared_ptr<const UsageView> view;
        {
            const std::lock_guard<std::mutex> lock(usageMutex_);
            view = usageView_;
            usageAsked_ = range;
            if (!usageReading_)
            {
                const auto age = std::chrono::duration<double>(std::chrono::steady_clock::now() - usageReadAt_).count();
                ask = !view->read || view->range != range || usage::revision() != usageReadRevision_ || age >= usageReread_;
                usageReading_ = ask;
            }
        }
        if (ask)
            queueJob([this, range] { readUsage(range); });
        return view;
    }

    void ChatAgent::resetUsage()
    {
        usage::requestReset();                  // the cut: what was counted until now goes, what comes after stays
        {
            const std::lock_guard<std::mutex> lock(usageMutex_);
            UsageView v = *usageView_;
            v.resetting = true;
            v.resetError.clear();               // said until this reset, which now runs
            publishUsage(std::move(v));
        }
        queueJob([this] { runUsageReset(); });  // dropped when the agent goes: its destructor runs it then
    }

    void ChatAgent::setUsageRereadSecondsForTests(double seconds)
    {
        const std::lock_guard<std::mutex> lock(usageMutex_);
        usageReread_ = seconds;
    }

    void ChatAgent::publishUsage(UsageView view)
    {
        if (sameRead(view, *usageView_))
            return;
        view.serial = usageView_->serial + 1;
        usageView_ = std::make_shared<const UsageView>(std::move(view));
    }

    void ChatAgent::holdUsageReadsForTests(bool hold)
    {
        {
            const std::lock_guard<std::mutex> lock(usageMutex_);
            usageHold_ = hold;
        }
        usageCv_.notify_all();
    }

    uint64_t ChatAgent::usageReadsForTests() const
    {
        const std::lock_guard<std::mutex> lock(usageMutex_);
        return usageReads_;
    }

    void ChatAgent::readUsage(usage::Range range)
    {
        {
            std::unique_lock<std::mutex> lock(usageMutex_);
            usageCv_.wait(lock, [this] { return !usageHold_ || usageStopping_; });
        }
        const uint64_t revision = usage::revision();     // a count added while reading asks for another read
        UsageView v = readView(range);
        {
            const std::lock_guard<std::mutex> lock(usageMutex_);
            ++usageReads_;
            usageReading_ = false;
            usageReadRevision_ = revision;
            usageReadAt_ = std::chrono::steady_clock::now();
            v.resetting = usage::resetOwed();
            v.resetError = usage::lastResetFailed() ? kResetFailed : "";   // the ledger's, whoever ran it
            publishUsage(std::move(v));
        }
        runUsageReset();                        // one confirmed meanwhile runs before this job lets go
    }

    void ChatAgent::runUsageReset()
    {
        while (usage::resetOwed())              // a reset confirmed while one ran is owed again: run it too
        {
            (void) usage::reset();               // its outcome is the ledger's (lastResetFailed), every page's
            const uint64_t revision = usage::revision();
            usage::Range range;
            {
                const std::lock_guard<std::mutex> lock(usageMutex_);
                range = usageAsked_;
            }
            UsageView v = readView(range);
            const std::lock_guard<std::mutex> lock(usageMutex_);
            usageReadRevision_ = revision;
            usageReadAt_ = std::chrono::steady_clock::now();
            v.resetting = usage::resetOwed();
            v.resetError = usage::lastResetFailed() ? kResetFailed : "";
            publishUsage(std::move(v));
        }
    }
}
