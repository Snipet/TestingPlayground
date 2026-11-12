// Source/plugin/ChatProviderTurn.cpp: the chat with another model provider (ChatAgent.h, "Other providers";
// Providers.h): the provider chosen and its key, its models, and a turn's requests to it and their answers. Anthropic's
// own requests never come here (ChatAgent.cpp's streamOnce and fetchModels, as they were).
#include "plugin/ChatAgent.h"

#include "plugin/ChatStore.h"
#include "plugin/Providers.h"

#include <algorithm>
#include <cctype>

namespace playground
{
    namespace
    {
        constexpr std::size_t kListed = 40;       // models the picker lists at most (OpenRouter's has hundreds); the
                                                  // one chosen is always among them

        std::string trimmed(const std::string& s) { return juce::String::fromUTF8(s.c_str()).trim().toStdString(); }

        std::string str(const juce::var& v, const char* key) { return v.getProperty(key, "").toString().toStdString(); }

        // The pictures' base64 in the history: a request's bytes that a server counts as few tokens.
        std::size_t pictureBytes(const juce::Array<juce::var>& messages)
        {
            std::size_t n = 0;
            const auto add = [&n](const juce::var& block) {
                if (str(block, "type") == "image")
                    n += static_cast<std::size_t>(block.getProperty("source", juce::var()).getProperty("data", "").toString().length());
            };
            for (const juce::var& m : messages)
                if (const juce::Array<juce::var>* blocks = m.getProperty("content", juce::var()).getArray())
                    for (const juce::var& b : *blocks)
                    {
                        add(b);
                        if (const juce::Array<juce::var>* inner = b.getProperty("content", juce::var()).getArray())
                            for (const juce::var& x : *inner)
                                add(x);
                    }
            return n;
        }

        // A provider as the chat keeps it: its kind's id, an OpenAI-compatible server's base made canonical (as typed
        // when it is none: nothing then finds a key for it).
        ChatProvider tidy(const ChatProvider& wanted)
        {
            ChatProvider p;
            const providers::Kind kind = providers::kindOf(wanted.kind);
            p.kind = providers::idOf(kind);
            if (kind == providers::Kind::openai)
            {
                std::string why;
                p.base = providers::canonical(wanted.base, why);
                if (p.base.empty())
                    p.base = wanted.base;
            }
            return p;
        }

        std::string originOf(const ChatProvider& p) { return providers::originOf(providers::baseOf(p)); }

        // A thinking level as the Thinking menu names it ("none" Off, "xhigh" Extra high, else its word capitalised).
        std::string levelName(const std::string& e)
        {
            if (e == "none")
                return "Off";
            if (e == "xhigh")
                return "Extra high";
            std::string n = e;
            if (!n.empty())
                n[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(n[0])));
            return n;
        }

        bool sameServer(const ChatProvider& a, const ChatProvider& b) { return a.kind == b.kind && a.base == b.base; }
    }

    // ---- the provider (ChatPort) -------------------------------------------------------------------------------------------
    bool ChatAgent::setProvider(const ChatProvider& wanted, const std::string& typedKey, std::string& why)
    {
        const providers::Kind kind = providers::kindOf(wanted.kind);
        const std::string key = trimmed(typedKey);
        if (kind == providers::Kind::anthropic)
        {
            // Anthropic's key as it always was (saveKey); the provider back to Anthropic.
            if (!key.empty() && !saveKey(key, why))
                return false;
            const std::lock_guard<std::mutex> lock(mutex_);
            if (provider_.kind != "anthropic")
            {
                provider_ = ChatProvider{};
                saveProviderChoice();
                addEntry(ChatEntry::Kind::note, "The chat talks to Anthropic (Claude) again.");
            }
            touch();
            return true;
        }
        ChatProvider p;
        p.kind = providers::idOf(kind);
        if (kind == providers::Kind::openai)
        {
            p.base = providers::canonical(wanted.base, why);
            if (p.base.empty())
                return false;
        }
        const std::string origin = originOf(p);
        const std::string model = trimmed(wanted.model);
        if (!model.empty() && !providers::plausibleModel(model))
        {
            why = "that is not a model's name (no spaces, at most 200 characters)";
            return false;
        }
        if (!key.empty())
        {
            if (!providers::plausibleKey(p, key, why) || !chatstore::saveProviderKey(p.kind, origin, key, &why))
                return false;
        }
        else if (!providers::keyless(p) && !chatstore::hasProviderKey(p.kind, origin))
        {
            why = "paste " + providers::nameOf(p) + "'s API key first";
            return false;
        }
        const std::string name = providers::nameOf(p), host = providers::hostOf(providers::baseOf(p));
        const std::string mk = chatstore::modelKey(p.kind, p.base);
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            const bool changed = provider_.kind != p.kind || provider_.base != p.base;
            const std::string before = otherModel_;
            if (changed)
            {
                provider_ = p;
                otherModels_.clear();
                const auto chosen = otherChosen_.find(mk);
                otherModel_ = chosen != otherChosen_.end() ? chosen->second : std::string();
            }
            if (changed || !key.empty())
            {
                ++keyGeneration_;                  // a read begun before is not kept
                otherKey_.clear();                 // read again, for this server (a new key may list more models)
                otherKeyFor_.clear();
                otherKeyProblem_.clear();
                otherFetched_ = false;
                otherListError_.clear();
            }
            if (!model.empty())
                otherModel_ = model;
            if (!otherModel_.empty())
            {
                otherChosen_[mk] = otherModel_;
                if (otherModel(otherModel_) == nullptr)
                    otherModels_.insert(otherModels_.begin(), otherPlain(otherModel_));
            }
            saveProviderChoice();
            if (!key.empty())
                addEntry(ChatEntry::Kind::note, "API key for " + name + " saved, encrypted, on " + kThisMachine
                                                    + ", for both playground plugins; it goes to " + host + " alone.");
            if (changed)
                addEntry(ChatEntry::Kind::note,
                         "The chat talks to " + name + " now: your messages, the plugin's files and what the tools find go "
                             "to " + host
                             + (!otherModel_.empty()           ? ", to " + otherModel_
                                : providers::picksFirst(p)     ? std::string(", to the first model it lists")
                                                               : std::string(", to the model you choose in the model menu"))
                             + (providers::reasoningOf(p) != providers::Reasoning::none
                                    ? ". The Thinking menu stays where its model takes a level (that model's own levels; "
                                      "until you choose one, the model's own default); fast mode, web search and the planner "
                                      "stay Anthropic's, off with it."
                                    : ". Anthropic's own options (the thinking levels, fast mode, web search, the planner) are "
                                      "off with it."));
            else if (otherModel_ != before)
                addEntry(ChatEntry::Kind::note, "The chat uses " + otherModel_ + " on " + name + " now.");
            touch();
        }
        // Its list, for the picker, off the message thread: a flag of its own ends it (a Stop, the agent's end), never a
        // turn's stop left over from before.
        listCancel_.store(false);
        queueJob([this] { fetchOtherModels(listCancel_); });
        return true;
    }

    ChatProviderInfo ChatAgent::providerInfo(const ChatProvider& wanted) const
    {
        const ChatProvider p = tidy(wanted);
        ChatProviderInfo info;
        info.name = providers::nameOf(p);
        info.keyless = providers::keyless(p);
        info.picksFirst = providers::picksFirst(p);
        info.hasKey = p.kind == "anthropic" ? chatstore::hasKey() : chatstore::hasProviderKey(p.kind, originOf(p));
        const std::lock_guard<std::mutex> lock(mutex_);
        if (p.kind == "anthropic")
            info.model = model_;
        else if (const auto it = otherChosen_.find(chatstore::modelKey(p.kind, p.base)); it != otherChosen_.end())
            info.model = it->second;
        return info;
    }

    void ChatAgent::forgetProviderKey(const ChatProvider& wanted)
    {
        const ChatProvider p = tidy(wanted);
        if (p.kind == "anthropic")
        {
            forgetKey();
            return;
        }
        std::string why;
        const bool removed = chatstore::forgetProviderKey(p.kind, originOf(p), &why);
        const std::lock_guard<std::mutex> lock(mutex_);
        ++keyGeneration_;
        if (sameServer(provider_, p))
        {
            otherKey_.clear();
            otherKeyFor_.clear();
            otherKeyProblem_.clear();
            otherListError_.clear();
        }
        if (removed)
            addEntry(ChatEntry::Kind::note, "API key for " + providers::nameOf(p) + " removed from " + kThisMachine + ".");
        else
            addEntry(ChatEntry::Kind::error, "The API key for " + providers::nameOf(p) + " could not be removed: " + why + ".");
    }

    // ---- its state ------------------------------------------------------------------------------------------------------------
    void ChatAgent::otherState(ChatState& s, bool kept) const
    {
        s.hasKey = !otherKey_.empty() || kept;
        s.keyless = providers::keyless(provider_);
        s.keyProblem = otherKeyProblem_;
        // The model chosen; with none, the list's first where a turn takes it (picksFirst), else none (the user chooses).
        s.model = otherModelNow();
        s.models.clear();
        for (const ChatModel& m : otherModels_)
            if (s.models.size() < kListed || m.id == s.model)
                s.models.push_back(m);
        s.provider.model = s.model;
        // The thinking level: the one chosen for this model, else the level it uses by default (shown, never sent), on
        // a model that takes levels; none of Anthropic's other options: no pills for them.
        const ChatModel* m = otherModel(s.model);
        const std::string chosen = otherEffort(provider_, s.model);
        s.effort = m == nullptr || m->efforts.empty() ? std::string() : !chosen.empty() ? chosen : m->defaultEffort;
        s.planner.clear();
        s.fast = false;
        s.web = false;
    }

    void ChatAgent::saveProviderChoice() const
    {
        chatstore::saveProvider({ provider_.kind, provider_.base, otherChosen_, otherEfforts_ });
    }

    const ChatModel* ChatAgent::otherModel(const std::string& id) const
    {
        const auto it = std::find_if(otherModels_.begin(), otherModels_.end(), [&](const ChatModel& m) { return m.id == id; });
        return it != otherModels_.end() ? &*it : nullptr;
    }

    std::string ChatAgent::otherModelNow() const
    {
        return otherModel_.empty() && !otherModels_.empty() && providers::picksFirst(provider_) ? otherModels_.front().id
                                                                                                : otherModel_;
    }

    ChatModel ChatAgent::otherPlain(const std::string& id) const
    {
        ChatModel m = providers::plainModel(provider_, id);
        if (effortRefused_.count(effortKey(provider_, id)) > 0)
        {
            m.efforts.clear();
            m.defaultEffort.clear();
        }
        return m;
    }

    std::pair<std::string, std::string> ChatAgent::effortKey(const ChatProvider& p, const std::string& model)
    {
        return { chatstore::modelKey(p.kind, p.base), model };
    }

    // The level chosen for `model` of `p`'s, while it is one the model takes now ("" for none: its own default).
    std::string ChatAgent::otherEffort(const ChatProvider& p, const std::string& model) const
    {
        const auto it = otherEfforts_.find(effortKey(p, model));
        if (it == otherEfforts_.end())
            return {};
        const ChatModel* m = sameServer(provider_, p) ? otherModel(model) : nullptr;
        return m != nullptr && std::find(m->efforts.begin(), m->efforts.end(), it->second) != m->efforts.end() ? it->second
                                                                                                                : std::string();
    }

    bool ChatAgent::otherTurn() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return turnProvider_.kind != "anthropic";
    }

    bool ChatAgent::keyKept(const ChatProvider& p) const
    {
        return p.kind == "anthropic" ? chatstore::hasKey() : chatstore::hasProviderKey(p.kind, originOf(p));
    }

    // The key of `p`'s server, read with mutex_ released (ChatAgent.h): the one read before while `p` is the provider
    // chosen (read once for its server, again when it changed or was saved anew); else read, and kept for the provider
    // chosen when `p` still is and no key was saved or removed meanwhile, with why it cannot be used.
    std::string ChatAgent::otherKeyOf(const ChatProvider& p)
    {
        const std::string origin = originOf(p);
        const std::string forServer = p.kind + " " + origin;
        uint64_t generation = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (sameServer(provider_, p) && otherKeyFor_ == forServer && !otherKey_.empty())
                return otherKey_;
            generation = keyGeneration_;
        }
        std::string key, why;
        const bool kept = chatstore::hasProviderKey(p.kind, origin);
        if (kept)
            if (const auto k = chatstore::loadProviderKey(p.kind, origin, &why))
                key = *k;
        const std::lock_guard<std::mutex> lock(mutex_);
        if (sameServer(provider_, p) && keyGeneration_ == generation)
        {
            otherKey_ = key;
            otherKeyFor_ = forServer;
            if (!key.empty())
                otherKeyProblem_.clear();
            else if (kept)
                otherKeyProblem_ = why;
        }
        return key;
    }

    // ---- its models -------------------------------------------------------------------------------------------------------
    void ChatAgent::fetchOtherModels(const std::atomic<bool>& cancel)
    {
        ChatProvider p;
        claude::Transport* transport = nullptr;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (otherFetched_ || provider_.kind == "anthropic")
                return;
            p = provider_;
        }
        const std::string key = otherKeyOf(p);     // without the lock: the keyring may ask to be unlocked
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (otherFetched_ || !sameServer(provider_, p))
                return;                            // listed meanwhile, or another provider chosen: its own job
            transport = transport_.get();
            if (key.empty() && !providers::keyless(p))
                return;                            // asked again once a key is there
            otherFetched_ = true;
        }
        const providers::Endpoint e = providers::modelsEndpoint(p);
        const claude::HttpResult r = transport->get(e.host, e.path, providers::headers(p, key), cancel);
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!sameServer(provider_, p))
            return;                                // another provider since
        if (r.status != 200)
        {
            // Why, kept for the turn that finds no model (startTurn), and asked again next turn; a key the server
            // refused is said as the key's problem and not asked again until another is saved. A stop says nothing.
            otherFetched_ = false;
            if (cancel.load())
                return;
            if (r.status == 0)
                otherListError_ = providers::connectionText(p, r.error);
            else if (r.status >= 300 && r.status < 400)
                otherListError_ = providers::redirectText(p, r.status, r.location);
            else
            {
                otherListError_ = providers::errorText(p, r.status, r.body);
                if (providers::keyRefused(p, r.status, r.body) && key == otherKey_)
                {
                    otherKeyProblem_ = otherListError_;
                    otherFetched_ = true;
                }
            }
            touch();
            return;                                // the model chosen stays the list
        }
        otherListError_.clear();
        // The prices its list gives (OpenRouter's), for the usage ledger's estimates of requests whose cost was not
        // reported.
        for (const auto& [id, price] : providers::listedPrices(p, r.body))
            usage::setListedPrice(providers::usageName(p), id, price);
        std::vector<ChatModel> found = providers::modelsOf(p, r.body);
        if (found.empty())
            return;
        // A model that refused a thinking level this session keeps none, whatever its list says.
        for (ChatModel& m : found)
            if (effortRefused_.count(effortKey(p, m.id)) > 0)
            {
                m.efforts.clear();
                m.defaultEffort.clear();
            }
        otherModels_ = std::move(found);
        if (!otherModel_.empty() && otherModel(otherModel_) == nullptr)
            otherModels_.insert(otherModels_.begin(), otherPlain(otherModel_));   // one typed the list lacks
        touch();
    }

    // ---- a turn's start ---------------------------------------------------------------------------------------------------
    ChatAgent::Start ChatAgent::startTurn(int& maxTokens)
    {
        for (int look = 0; look < 3; ++look)
        {
            ChatProvider chosen;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                turnProvider_ = provider_;
                if (provider_.kind == "anthropic")
                    return Start::anthropic;
                chosen = provider_;
            }
            const std::string key = otherKeyOf(chosen);   // without the lock: the keyring may ask to be unlocked
            std::string missing;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                if (!sameServer(provider_, chosen))
                    continue;                      // chosen meanwhile, before anything was sent: the turn's now
                if (key.empty() && !providers::keyless(turnProvider_))
                    missing = otherKeyProblem_.empty()
                                  ? "Add your " + providers::nameOf(turnProvider_) + " API key (KEY, above) to chat."
                                  : "The saved " + providers::nameOf(turnProvider_) + " key cannot be used: " + otherKeyProblem_;
            }
            if (missing.empty())
            {
                fetchModels();                     // the turn's own (a Stop ends it), unless the list is in or on its way
                waitJobs();                        // a list asked for when the provider was chosen, still on its way
                const std::lock_guard<std::mutex> lock(mutex_);
                if (!sameServer(provider_, turnProvider_))
                    continue;                      // chosen meanwhile, before anything was sent: the turn's now
                if (cancel_.load())
                {
                    addEntry(ChatEntry::Kind::note, "Stopped.");
                    return Start::ended;
                }
                if (otherModel_.empty() && !otherModels_.empty() && providers::picksFirst(turnProvider_))
                {
                    otherModel_ = otherModels_.front().id;   // the server's first, until another is chosen
                    otherChosen_[chatstore::modelKey(turnProvider_.kind, turnProvider_.base)] = otherModel_;
                    saveProviderChoice();
                }
                if (!otherModel_.empty())
                {
                    // The model for the whole turn, from this same moment: its thinking level when one was chosen for it
                    // (none chosen, none sent: the model's own default), none of Anthropic's other options, its pictures
                    // if it takes them, the prompt's short form for a small context (or one a server on this Mac or the
                    // local network does not say).
                    turnModel_ = otherModel_;
                    turnProvider_.model = turnModel_;
                    turnEffort_ = otherEffort(turnProvider_, turnModel_);
                    turnFast_ = false;
                    turnWeb_ = false;
                    const ChatModel* m = otherModel(turnModel_);
                    turnImages_ = m == nullptr || m->images;
                    turnJsonSchema_ = !schemaRefused_;
                    const int context = m != nullptr ? m->context : 0;
                    turnShort_ = context > 0 ? context < kShortPromptBelow : providers::isLocal(providers::baseOf(turnProvider_));
                    maxTokens = (m != nullptr ? *m : providers::plainModel(turnModel_)).maxTokens;
                    plannerAsks_ = 0;
                    turnPlanUsage_.clear();
                    return Start::other;
                }
                // No model: the user chooses one (a long list, of many prices), or the list could not be read (why).
                const std::string name = providers::nameOf(turnProvider_);
                if (!otherModels_.empty())
                    missing = "Choose a model in the model menu first: " + name + " lists "
                            + std::to_string(otherModels_.size()) + ", and their prices and skills differ.";
                else if (!otherKeyProblem_.empty())
                    missing = "The saved " + name + " key cannot be used: " + otherKeyProblem_;
                else if (!otherListError_.empty())
                    missing = "Could not read " + name + "'s models: " + otherListError_;
                else
                    missing = "Choose a model: " + name + " listed none (KEY, above, takes one by its name).";
            }
            const std::lock_guard<std::mutex> lock(mutex_);
            addEntry(ChatEntry::Kind::error, missing);
            return Start::ended;
        }
        const std::lock_guard<std::mutex> lock(mutex_);
        addEntry(ChatEntry::Kind::error, "The provider changed again and again as this message started: nothing was sent.");
        return Start::ended;
    }

    // ---- its requests -----------------------------------------------------------------------------------------------------
    std::string ChatAgent::requestBody(std::string& beta)
    {
        const juce::var request = buildRequest(beta);
        ChatProvider p;
        providers::Options options;
        std::size_t pictures = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (turnProvider_.kind == "anthropic")
                p.kind.clear();
            else
            {
                p = turnProvider_;
                options.images = turnImages_;
                options.jsonSchema = turnJsonSchema_;
                options.effort = turnEffort_;
                pictures = pictureBytes(messages_);
                turnHadPictures_ = pictures > 0;
                if (!turnImages_ && turnHadPictures_ && !picturesNoted_)
                {
                    // A model known to take none (its list, or a refusal before): said once a chat.
                    picturesNoted_ = true;
                    addEntry(ChatEntry::Kind::note, "The model (" + turnModel_ + ") takes no pictures: it gets a line of text "
                                                    "in each one's place, and cannot see them. Choose a model that takes "
                                                    "pictures for it to see one.");
                }
            }
        }
        if (p.kind.empty())
            return claude::toJson(request);        // Anthropic's, as it always was
        beta.clear();                              // Anthropic's betas are its own
        std::string body = providers::body(p, request, options);
        const std::lock_guard<std::mutex> lock(mutex_);
        turnTextBytes_ = body.size() - (options.images ? std::min(pictures, body.size()) : 0);
        return body;
    }

    bool ChatAgent::streamOther(const std::string& body, claude::MessageBuilder& builder, int& status, std::string& error,
                                int& retryAfter)
    {
        ChatProvider p;
        claude::Transport* transport = nullptr;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            p = turnProvider_;                     // the worker's own, set at the turn's start
        }
        // The key of the turn's own server, never another's (the provider may have changed since the turn began), read
        // without the lock (the keyring may ask to be unlocked).
        const std::string key = otherKeyOf(p);
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            transport = transport_.get();
            if (key.empty() && !providers::keyless(p))
            {
                status = 401;
                retryAfter = -1;
                error = "The API key for " + providers::nameOf(p) + " was removed. Add it again.";
                return false;
            }
            requests_.push_back(body);
            if (requests_.size() > 64)
                requests_.erase(requests_.begin());
        }
        const providers::Endpoint e = providers::endpoint(p, p.model);
        claude::Headers headers = providers::headers(p, key);
        headers.emplace_back("content-type", "application/json");
        claude::SseParser sse;
        const std::unique_ptr<providers::Stream> stream =
            providers::streamFor(p, [&builder](const std::string& type, const std::string& data) { builder.event(type, data); });
        const claude::HttpResult r = transport->post(e.host, e.path, body, headers,
                                                     [&](std::string_view chunk) {
                                                         sse.feed(chunk, [&](const std::string& ev, const std::string& data) {
                                                             stream->event(ev, data);
                                                         });
                                                         return !cancel_.load();
                                                     },
                                                     cancel_);
        status = r.status;
        retryAfter = r.retryAfterSeconds;
        if (r.status != 200)
        {
            error = r.status == 0                       ? providers::connectionText(p, r.error)
                  : r.status >= 300 && r.status < 400   ? providers::redirectText(p, r.status, r.location)
                                                        : providers::errorText(p, r.status, r.body);
            if (r.status >= 400 && providers::keyRefused(p, r.status, r.body))
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                if (sameServer(provider_, p) && key == otherKey_)
                    otherKeyProblem_ = error;
                touch();
            }
            else if (providers::aboutModel(r.status, error))
            {
                // A model that cannot serve this chat (the Responses API's alone, or one that calls no tools): off the
                // menu, and no longer chosen, so the next turn takes another or asks for one (never one switched to
                // here: a model is the user's choice).
                const std::lock_guard<std::mutex> lock(mutex_);
                if (sameServer(provider_, p))
                {
                    otherModels_.erase(std::remove_if(otherModels_.begin(), otherModels_.end(),
                                                      [&](const ChatModel& m) { return m.id == p.model; }),
                                       otherModels_.end());
                    if (otherModel_ == p.model)
                    {
                        otherModel_.clear();
                        otherChosen_.erase(chatstore::modelKey(p.kind, p.base));
                        saveProviderChoice();
                    }
                    touch();
                }
            }
            return false;
        }
        stream->finish();                          // its end, when the answer ended (else the turn asks again)
        error.clear();
        return true;
    }

    bool ChatAgent::workAround(int status, const std::string& error)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!turnEffort_.empty() && providers::aboutEffort(turnProvider_, status, error))
        {
            // A thinking level the model refuses: asked again at once without one, and no levels for that model for the
            // rest of the session (its pills gone), the choice kept for it forgotten; never a failed turn.
            const std::string level = levelName(turnEffort_);
            turnEffort_.clear();
            const auto key = effortKey(turnProvider_, turnModel_);
            effortRefused_.insert(key);
            if (otherEfforts_.erase(key) > 0)
                saveProviderChoice();
            if (sameServer(provider_, turnProvider_))
                for (ChatModel& m : otherModels_)
                    if (m.id == turnModel_)
                    {
                        m.efforts.clear();
                        m.defaultEffort.clear();
                    }
            addEntry(ChatEntry::Kind::note, "The model (" + turnModel_ + ") refused the thinking level " + level
                                                + ": asked again without one (its own default), and the Thinking menu is off "
                                                  "for it until the plugin is opened again.");
            touch();
            return true;
        }
        if (turnImages_ && turnHadPictures_ && !providers::aboutModel(status, error) && providers::aboutImages(status, error))
        {
            turnImages_ = false;
            picturesNoted_ = true;
            // The model marked in its provider's list: the turn's provider's, never another chosen since.
            if (sameServer(provider_, turnProvider_))
            {
                bool listed = false;
                for (ChatModel& m : otherModels_)
                    if (m.id == turnModel_)
                    {
                        m.images = false;
                        listed = true;
                    }
                if (!listed)
                {
                    ChatModel m = otherPlain(turnModel_);
                    m.images = false;
                    otherModels_.push_back(std::move(m));
                }
            }
            addEntry(ChatEntry::Kind::note, "The model (" + turnModel_ + ") takes no pictures (" + error
                                                + "): from now on it gets a line of text in each one's place, and cannot "
                                                  "see them.");
            return true;
        }
        if (turnJsonSchema_ && turnProvider_.kind == "gemini" && providers::aboutSchema(status, error))
        {
            turnJsonSchema_ = false;
            schemaRefused_ = true;
            addEntry(ChatEntry::Kind::note, "Gemini refused the tools' schemas as JSON Schema (" + error
                                                + "): they go in its own form now, a few of their options left out.");
            return true;
        }
        return false;
    }
}
