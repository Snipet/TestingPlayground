// Source/plugin/ChatProviderTurn.cpp: the chat with another model provider (ChatAgent.h, "Other providers";
// Providers.h): the provider chosen and its key, its models, and a turn's requests to it and their answers. Anthropic's
// own requests never come here (ChatAgent.cpp's streamOnce and fetchModels, as they were).
#include "plugin/ChatAgent.h"

#include "plugin/ChatStore.h"
#include "plugin/Providers.h"

#include <algorithm>

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
                otherKey_.clear();                 // read again, for this server (a new key may list more models)
                otherKeyFor_.clear();
                otherKeyProblem_.clear();
                otherFetched_ = false;
            }
            if (!model.empty())
                otherModel_ = model;
            if (!otherModel_.empty())
            {
                otherChosen_[mk] = otherModel_;
                if (otherModel(otherModel_) == nullptr)
                    otherModels_.insert(otherModels_.begin(), providers::plainModel(otherModel_));
            }
            saveProviderChoice();
            if (!key.empty())
                addEntry(ChatEntry::Kind::note, "API key for " + name + " saved, encrypted, on " + kThisMachine
                                                    + ", for both playground plugins; it goes to " + host + " alone.");
            if (changed)
                addEntry(ChatEntry::Kind::note,
                         "The chat talks to " + name + " now: your messages, the plugin's files and what the tools find go "
                             "to " + host + (otherModel_.empty() ? ", to the first model it lists" : ", to " + otherModel_)
                             + ". Anthropic's own options (the thinking levels, fast mode, web search, the planner) are off "
                               "with it.");
            else if (otherModel_ != before)
                addEntry(ChatEntry::Kind::note, "The chat uses " + otherModel_ + " on " + name + " now.");
            touch();
        }
        queueJob([this] { fetchModels(); });      // its list, for the picker, off the message thread
        return true;
    }

    ChatProviderInfo ChatAgent::providerInfo(const ChatProvider& wanted) const
    {
        const ChatProvider p = tidy(wanted);
        ChatProviderInfo info;
        info.name = providers::nameOf(p);
        info.keyless = providers::keyless(p);
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
        chatstore::forgetProviderKey(p.kind, originOf(p));
        const std::lock_guard<std::mutex> lock(mutex_);
        if (provider_.kind == p.kind && provider_.base == p.base)
        {
            otherKey_.clear();
            otherKeyFor_.clear();
            otherKeyProblem_.clear();
        }
        addEntry(ChatEntry::Kind::note, "API key for " + providers::nameOf(p) + " removed from " + kThisMachine + ".");
    }

    // ---- its state ------------------------------------------------------------------------------------------------------------
    void ChatAgent::otherState(ChatState& s) const
    {
        s.hasKey = !otherKey_.empty() || chatstore::hasProviderKey(provider_.kind, originOf(provider_));
        s.keyless = providers::keyless(provider_);
        s.keyProblem = otherKeyProblem_;
        s.model = otherModel_.empty() && !otherModels_.empty() ? otherModels_.front().id : otherModel_;
        s.models.clear();
        for (const ChatModel& m : otherModels_)
            if (s.models.size() < kListed || m.id == s.model)
                s.models.push_back(m);
        s.provider.model = s.model;
        s.effort.clear();                          // none of Anthropic's options: no pills for them
        s.planner.clear();
        s.fast = false;
        s.web = false;
    }

    void ChatAgent::saveProviderChoice() const
    {
        chatstore::saveProvider({ provider_.kind, provider_.base, otherChosen_ });
    }

    const ChatModel* ChatAgent::otherModel(const std::string& id) const
    {
        const auto it = std::find_if(otherModels_.begin(), otherModels_.end(), [&](const ChatModel& m) { return m.id == id; });
        return it != otherModels_.end() ? &*it : nullptr;
    }

    bool ChatAgent::otherTurn() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return turnProvider_.kind != "anthropic";
    }

    // The key for the provider chosen (mutex held): read once for its server, again when it changed or was saved anew.
    void ChatAgent::loadOtherKey()
    {
        const std::string origin = originOf(provider_);
        const std::string forServer = provider_.kind + " " + origin;
        if (otherKeyFor_ == forServer && !otherKey_.empty())
            return;
        otherKey_.clear();
        otherKeyFor_ = forServer;
        if (!chatstore::hasProviderKey(provider_.kind, origin))
            return;
        std::string why;
        if (const auto k = chatstore::loadProviderKey(provider_.kind, origin, &why))
        {
            otherKey_ = *k;
            otherKeyProblem_.clear();
        }
        else
            otherKeyProblem_ = why;
    }

    // ---- its models -------------------------------------------------------------------------------------------------------
    void ChatAgent::fetchOtherModels()
    {
        ChatProvider p;
        std::string key;
        claude::Transport* transport = nullptr;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (otherFetched_ || provider_.kind == "anthropic")
                return;
            loadOtherKey();
            p = provider_;
            key = otherKey_;
            transport = transport_.get();
            if (key.empty() && !providers::keyless(p))
                return;                            // asked again once a key is there
            otherFetched_ = true;
        }
        const providers::Endpoint e = providers::modelsEndpoint(p);
        const claude::HttpResult r = transport->get(e.host, e.path, providers::headers(p, key), cancel_);
        const std::lock_guard<std::mutex> lock(mutex_);
        if (provider_.kind != p.kind || provider_.base != p.base)
            return;                                // another provider since
        if (r.status != 200)
        {
            if (r.status != 0 && providers::keyRefused(p, r.status, r.body) && key == otherKey_)
            {
                otherKeyProblem_ = providers::errorText(p, r.status, r.body);
                touch();
            }
            if (r.status == 0)
                otherFetched_ = false;             // no server yet (a local one not started): asked again next turn
            return;                                // the model chosen stays the list
        }
        std::vector<ChatModel> found = providers::modelsOf(p, r.body);
        if (found.empty())
            return;
        otherModels_ = std::move(found);
        if (!otherModel_.empty() && otherModel(otherModel_) == nullptr)
            otherModels_.insert(otherModels_.begin(), providers::plainModel(otherModel_));   // one typed the list lacks
        touch();
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
                pictures = pictureBytes(messages_);
                turnHadPictures_ = pictures > 0;
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
        std::string key;
        claude::Transport* transport = nullptr;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            p = turnProvider_;
            // The key of the turn's own server, never another's (the provider may have changed since the turn began).
            if (provider_.kind == p.kind && provider_.base == p.base)
            {
                loadOtherKey();
                key = otherKey_;
            }
            else if (const auto k = chatstore::loadProviderKey(p.kind, originOf(p)))
                key = *k;
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
            error = r.status == 0 ? providers::connectionText(p, r.error) : providers::errorText(p, r.status, r.body);
            if (r.status != 0 && providers::keyRefused(p, r.status, r.body))
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                if (provider_.kind == p.kind && provider_.base == p.base && key == otherKey_)
                    otherKeyProblem_ = error;
                touch();
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
        if (turnImages_ && turnHadPictures_ && providers::aboutImages(status, error))
        {
            turnImages_ = false;
            bool listed = false;
            for (ChatModel& m : otherModels_)
                if (m.id == turnModel_)
                {
                    m.images = false;
                    listed = true;
                }
            if (!listed)
            {
                ChatModel m = providers::plainModel(turnModel_);
                m.images = false;
                otherModels_.push_back(std::move(m));
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
