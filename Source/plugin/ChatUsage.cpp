// The chat's side of the usage ledger (Usage.h): each request counted once, the turn's cost for the footer.
#include "plugin/ChatAgent.h"
#include "plugin/Providers.h"
#include "plugin/Usage.h"

#include <chrono>
#include <utility>

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

// ---- the usage page's reads (ChatPort::usage) -----------------------------------------------------------------------

namespace playground
{
    namespace
    {
        constexpr double kUsageRereadSeconds = 5.0;      // another process may have written the file meanwhile

        bool sameView(const UsageView& a, const UsageView& b)
        {
            if (a.range != b.range || a.pricesAsOf != b.pricesAsOf || a.resetFailed != b.resetFailed
                || a.snapshot.today != b.snapshot.today || a.snapshot.rows.size() != b.snapshot.rows.size())
                return false;
            for (std::size_t i = 0; i < a.snapshot.rows.size(); ++i)
                if (a.snapshot.rows[i].key != b.snapshot.rows[i].key || !(a.snapshot.rows[i].counts == b.snapshot.rows[i].counts))
                    return false;
            return true;
        }

        double steadySeconds()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
    }

    std::shared_ptr<const UsageView> ChatAgent::usage(usage::Range range)
    {
        const std::lock_guard<std::mutex> lock(usageMutex_);
        const bool stale = usageView_ == nullptr || usageView_->range != range || usageSeen_ != usage::revision()
                        || steadySeconds() - usageAt_ >= kUsageRereadSeconds;
        if (stale && !usageReading_)
        {
            usageReading_ = true;
            queueJob([this, range] { readUsage(range); });
        }
        return usageView_;
    }

    // A Reset the user confirmed is owed until it has run: the read under way runs it before it ends (readUsage's
    // loop), else a read is queued for it; and a job the agent's end drops leaves it to the destructor. It never waits
    // for the page to ask again.
    void ChatAgent::resetUsage()
    {
        const std::lock_guard<std::mutex> lock(usageMutex_);
        usageResetOwed_ = true;
        if (usageReading_)
            return;
        usageReading_ = true;
        const usage::Range range = usageView_ != nullptr ? usageView_->range : usage::Range::month;
        queueJob([this, range] { readUsage(range); });
    }

    // On the job thread: never the message thread, which never waits on the disk or the folder's lock. It reads until
    // no Reset is owed, and only then lets go of usageReading_.
    void ChatAgent::readUsage(usage::Range range)
    {
        for (;;)
        {
            bool resetFirst = false;
            {
                const std::lock_guard<std::mutex> lock(usageMutex_);
                resetFirst = std::exchange(usageResetOwed_, false);
            }
            if (usageReadHook_)
                usageReadHook_();                        // a probe holds the read here: past its look at the Reset
            const bool failed = resetFirst && !usage::reset();
            auto view = std::make_shared<UsageView>();
            const uint64_t seen = usage::revision();
            view->range = range;
            view->snapshot = usage::snapshot(range);
            view->pricesAsOf = usage::pricesInUse().asOf;
            const std::lock_guard<std::mutex> lock(usageMutex_);
            // A failed Reset is said until the next one (a reread a few seconds on keeps it).
            view->resetFailed = failed || (!resetFirst && usageView_ != nullptr && usageView_->resetFailed);
            // The same as the view held (a reread with nothing new): kept, so the page draws nothing again.
            if (usageView_ == nullptr || !sameView(*usageView_, *view))
            {
                view->revision = ++usageReads_;
                usageView_ = std::move(view);
            }
            usageSeen_ = seen;
            usageAt_ = steadySeconds();
            if (!usageResetOwed_)
            {
                usageReading_ = false;
                return;
            }
        }
    }

    // The agent's end (its destructor, after the job thread is joined): a Reset still owed (its job dropped before it
    // ran) runs here, on the thread that ends the agent, as the destructor's flush does.
    void ChatAgent::settleUsageReset()
    {
        bool owed = false;
        {
            const std::lock_guard<std::mutex> lock(usageMutex_);
            owed = std::exchange(usageResetOwed_, false);
        }
        if (owed)
            usage::reset();
    }
}
