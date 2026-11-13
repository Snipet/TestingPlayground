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
