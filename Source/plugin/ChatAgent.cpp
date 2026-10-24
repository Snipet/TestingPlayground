// Source/plugin/ChatAgent.cpp: see ChatAgent.h.
#include "plugin/ChatAgent.h"

#include "plugin/ChatStore.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace testingplayground
{
    namespace
    {
        constexpr int kMaxTokens = 64000;
        constexpr int kMaxAttempts = 4;            // a request, retried on an overload, a rate limit or a dropped line
        constexpr const char* kDefaultModel = "claude-opus-5-5";
        constexpr const char* kDefaultEffort = "medium";
        constexpr const char* kFallbackBeta = "server-side-fallback-2026-07-01";

        const std::vector<std::string> kAllEfforts{ "low", "medium", "high", "xhigh", "max" };

        // Before /v1/models answers (or without a key): the current models, which all take adaptive thinking and
        // every effort level.
        std::vector<ChatModel> builtInModels()
        {
            return { { "claude-opus-5-5", "Claude Opus 5.5", kAllEfforts },
                     { "claude-fable-5-1", "Claude Fable 5.1", kAllEfforts },
                     { "claude-sonnet-5-5", "Claude Sonnet 5.5", kAllEfforts },
                     { "claude-haiku-5-5", "Claude Haiku 5.5", kAllEfforts } };
        }

        // The models that take "fallbacks": "default" (a classifier's refusal retried on another model).
        bool takesDefaultFallbacks(const std::string& id)
        {
            return id == "claude-fable-5-1" || id == "claude-opus-5-5" || id == "claude-opus-5"
                || id == "claude-sonnet-5-5";
        }

        std::string str(const juce::var& v, const char* key)
        {
            return v.getProperty(key, "").toString().toStdString();
        }

        juce::var text(const std::string& t)
        {
            return claude::object({ { "type", "text" }, { "text", juce::String::fromUTF8(t.c_str()) } });
        }

        int lineCount(const std::string& t)
        {
            if (t.empty())
                return 0;
            return static_cast<int>(std::count(t.begin(), t.end(), '\n')) + (t.back() == '\n' ? 0 : 1);
        }

        std::string number(double v)
        {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%g", v);
            return buf;
        }

        std::string thousands(int64_t n)
        {
            char buf[32];
            if (n >= 10000)
                std::snprintf(buf, sizeof buf, "%.0fK", static_cast<double>(n) / 1000.0);
            else if (n >= 1000)
                std::snprintf(buf, sizeof buf, "%.1fK", static_cast<double>(n) / 1000.0);
            else
                std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(n));
            return buf;
        }

        const char* kindName(ChatEntry::Kind k)
        {
            switch (k)
            {
                case ChatEntry::Kind::user: return "user";
                case ChatEntry::Kind::assistant: return "assistant";
                case ChatEntry::Kind::thinking: return "thinking";
                case ChatEntry::Kind::tool: return "tool";
                case ChatEntry::Kind::error: return "error";
                case ChatEntry::Kind::note: return "note";
            }
            return "note";
        }

        ChatEntry::Kind kindOf(const std::string& s)
        {
            for (const auto k : { ChatEntry::Kind::user, ChatEntry::Kind::assistant, ChatEntry::Kind::thinking,
                                  ChatEntry::Kind::tool, ChatEntry::Kind::error, ChatEntry::Kind::note })
                if (s == kindName(k))
                    return k;
            return ChatEntry::Kind::note;
        }

        std::string controlType(const Control& c)
        {
            switch (c.type)
            {
                case ControlType::real: return "float";
                case ControlType::integer: return "int";
                case ControlType::toggle: return "bool";
            }
            return "float";
        }

        // The dials of `controls` with their values, one a line.
        std::string dialLines(const Controls& controls, const ChatHost& host)
        {
            std::string out;
            for (int c = 0; c < kMaxControls; ++c)
            {
                const Control& k = controls[static_cast<std::size_t>(c)];
                if (!k.used)
                    continue;
                out += std::to_string(c + 1) + " " + k.name + ": " + controlType(k) + " [" + number(k.min) + ", "
                     + number(k.max) + "] default " + number(k.def) + ", now " + number(host.chatDialValue(c)) + "\n";
            }
            return out.empty() ? "none\n" : out;
        }

        std::string compileLine(const CompileStatus& s)
        {
            if (s.revision == 0)
                return "nothing compiled yet\n";
            if (s.ok)
                return "compiled in " + number(std::round(s.ms)) + " ms and plays\n" + s.notes;
            return "did not compile (the program before it plays on):\n" + s.errors + s.notes;
        }

        juce::var tool(const char* name, const char* description, juce::var properties,
                       std::initializer_list<const char*> required)
        {
            juce::Array<juce::var> req;
            for (const char* r : required)
                req.add(juce::String(r));
            juce::var schema = claude::object({ { "type", "object" }, { "properties", properties } });
            if (req.size() > 0)
                schema.getDynamicObject()->setProperty("required", req);
            return claude::object({ { "name", name },
                                    { "description", description },
                                    { "eager_input_streaming", true },
                                    { "input_schema", schema } });
        }

        juce::var prop(const char* type, const char* description)
        {
            return claude::object({ { "type", type }, { "description", description } });
        }
    }

    // ---- construction -----------------------------------------------------------------------------------------------
    ChatAgent::ChatAgent(ChatHost& host, std::unique_ptr<claude::Transport> transport)
        : host_(host), transport_(std::move(transport))
    {
        models_ = builtInModels();
        const chatstore::Choice choice = chatstore::loadChoice();
        model_ = choice.model.empty() ? std::string(kDefaultModel) : choice.model;
        effort_ = choice.effort.empty() ? std::string(kDefaultEffort) : choice.effort;
    }

    ChatAgent::~ChatAgent()
    {
        cancel_.store(true);
        joinWorker();
    }

    void ChatAgent::setTransport(std::unique_ptr<claude::Transport> transport)
    {
        waitIdle();
        const std::lock_guard<std::mutex> lock(mutex_);
        transport_ = std::move(transport);
        modelsFetched_ = false;
    }

    void ChatAgent::waitIdle() { joinWorker(); }

    std::vector<std::string> ChatAgent::requestsSent() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

    void ChatAgent::joinWorker()
    {
        if (worker_.joinable())
            worker_.join();
    }

    // ---- ChatPort -----------------------------------------------------------------------------------------------------
    uint64_t ChatAgent::chatRevision() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return revision_;
    }

    ChatState ChatAgent::chatState() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        ChatState s;
        s.hasKey = !key_.empty() || chatstore::hasKey();
        s.keyProblem = keyProblem_;
        s.busy = busy_;
        s.activity = activity_;
        s.entries = entries_;
        s.models = models_;
        s.model = model_;
        s.effort = effort_;
        s.usage = usage_;
        return s;
    }

    bool ChatAgent::saveKey(const std::string& key, std::string& why)
    {
        std::string trimmed = juce::String(key).trim().toStdString();
        if (!chatstore::saveKey(trimmed, &why))
            return false;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            key_ = trimmed;
            keyProblem_.clear();
            modelsFetched_ = false;
            addEntry(ChatEntry::Kind::note, "API key saved, encrypted, for both playground plugins on this Mac.");
        }
        return true;
    }

    void ChatAgent::forgetKey()
    {
        chatstore::forgetKey();
        const std::lock_guard<std::mutex> lock(mutex_);
        key_.clear();
        keyProblem_.clear();
        addEntry(ChatEntry::Kind::note, "API key removed from this Mac.");
    }

    void ChatAgent::setModel(const std::string& id)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto it = std::find_if(models_.begin(), models_.end(), [&](const ChatModel& m) { return m.id == id; });
        if (it == models_.end())
            return;
        model_ = id;
        if (std::find(it->efforts.begin(), it->efforts.end(), effort_) == it->efforts.end() && !it->efforts.empty())
        {
            const auto medium = std::find(it->efforts.begin(), it->efforts.end(), std::string(kDefaultEffort));
            effort_ = medium != it->efforts.end() ? *medium : it->efforts.front();
        }
        chatstore::saveChoice({ model_, effort_ });
        touch();
    }

    void ChatAgent::setEffort(const std::string& effort)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto it = std::find_if(models_.begin(), models_.end(), [&](const ChatModel& m) { return m.id == model_; });
        if (it != models_.end() && std::find(it->efforts.begin(), it->efforts.end(), effort) == it->efforts.end())
            return;                                // a level this model does not take
        effort_ = effort;
        chatstore::saveChoice({ model_, effort_ });
        touch();
    }

    void ChatAgent::send(const std::string& message)
    {
        const std::string trimmed = juce::String(message).trim().toStdString();
        if (trimmed.empty())
            return;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (busy_)
                return;
        }
        joinWorker();                              // a finished turn's thread
        const std::string context = contextBlock();
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            addEntry(ChatEntry::Kind::user, trimmed);
            juce::Array<juce::var> content;
            content.add(text(trimmed));
            if (!context.empty())
                content.add(text(context));
            // A turn that ended without an answer (stopped, or an error) left the user's message last: this one joins
            // it, so the roles keep alternating. Nothing was answered to it, so no thinking block depends on it.
            const int n = messages_.size();
            if (n > 0 && str(messages_[n - 1], "role") == "user")
            {
                const juce::var last = messages_[n - 1];
                const juce::var before = last.getProperty("content", juce::var());
                juce::Array<juce::var> merged;
                if (const auto* arr = before.getArray())
                    merged = *arr;
                merged.addArray(content);
                if (auto* o = last.getDynamicObject())
                    o->setProperty("content", merged);
            }
            else
            {
                messages_.add(claude::object({ { "role", "user" }, { "content", content } }));
            }
            busy_ = true;
            activity_ = "THINKING";
            cancel_.store(false);
            touch();
        }
        startWorker();
    }

    void ChatAgent::stop()
    {
        cancel_.store(true);
        const std::lock_guard<std::mutex> lock(mutex_);
        if (busy_)
        {
            activity_ = "STOPPING";
            touch();
        }
    }

    void ChatAgent::newChat()
    {
        stop();
        joinWorker();
        const std::lock_guard<std::mutex> lock(mutex_);
        messages_.clear();
        entries_.clear();
        lastContext_.clear();
        usage_.clear();
        activity_.clear();
        busy_ = false;
        touch();
    }

    // ---- the session --------------------------------------------------------------------------------------------------
    std::string ChatAgent::saveState() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        juce::Array<juce::var> entries;
        for (const ChatEntry& e : entries_)
            entries.add(claude::object({ { "k", kindName(e.kind) }, { "t", juce::String::fromUTF8(e.text.c_str()) } }));
        return claude::toJson(claude::object({ { "version", 1 },
                                               { "messages", messages_ },
                                               { "entries", entries },
                                               { "context", juce::String::fromUTF8(lastContext_.c_str()) } }));
    }

    void ChatAgent::loadState(const std::string& json)
    {
        stop();
        joinWorker();
        const juce::var v = claude::parseJson(json);
        const std::lock_guard<std::mutex> lock(mutex_);
        messages_.clear();
        entries_.clear();
        lastContext_.clear();
        busy_ = false;
        activity_.clear();
        const juce::var messages = v.getProperty("messages", juce::var()), entries = v.getProperty("entries", juce::var());
        if (const auto* arr = messages.getArray())
            messages_ = *arr;
        if (const auto* arr = entries.getArray())
            for (const auto& e : *arr)
                entries_.push_back({ kindOf(str(e, "k")), str(e, "t") });
        lastContext_ = str(v, "context");
        touch();
    }

    // ---- bookkeeping (mutex held) -------------------------------------------------------------------------------------
    void ChatAgent::touch() { ++revision_; }

    void ChatAgent::addEntry(ChatEntry::Kind kind, std::string t)
    {
        entries_.push_back({ kind, std::move(t) });
        touch();
    }

    void ChatAgent::appendToLast(ChatEntry::Kind kind, std::string_view t)
    {
        if (entries_.empty() || entries_.back().kind != kind)
            entries_.push_back({ kind, {} });
        entries_.back().text += t;
        touch();
    }

    void ChatAgent::setActivity(std::string activity)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (activity_ != activity && !(cancel_.load() && busy_))
        {
            activity_ = std::move(activity);
            touch();
        }
    }

    void ChatAgent::startWorker()
    {
        worker_ = std::thread([this] { runTurn(); });
    }

    void ChatAgent::ensureKey()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!key_.empty())
            return;
        std::string why;
        if (const auto k = chatstore::loadKey(&why))
        {
            key_ = *k;
            keyProblem_.clear();
        }
        else if (chatstore::hasKey())
        {
            keyProblem_ = why;
        }
    }

    // ---- the plugin's state, for Claude -------------------------------------------------------------------------------
    std::string ChatAgent::contextBlock()
    {
        const Draft d = host_.chatDraft();
        const Sources running = host_.chatRunning();
        const CompileStatus status = host_.chatLastStatus();
        std::string c = "<plugin_state>\nThe plugin's files, a tab each in the editor (the program is the first; the "
                        "editor shows ";
        c += d.files.empty() ? std::string(kProgramName)
                             : d.files[static_cast<std::size_t>(std::clamp(d.active, 0, static_cast<int>(d.files.size()) - 1))].name;
        c += "):\n";
        for (const SourceFile& f : d.files)
            c += "<file name=\"" + f.name + "\">\n" + f.text + (f.text.empty() || f.text.back() == '\n' ? "" : "\n")
               + "</file>\n";
        c += "What plays: " + std::string(running == d.files ? "these files" : "an earlier version of the files (they "
                                                                               "were edited since the last run)")
           + "\nLast compile: " + compileLine(status) + "Dials of the program that plays:\n"
           + dialLines(host_.chatDials(), host_);
        const uint32_t a = host_.chatAlerts();
        c += "Output alerts: ";
        c += a == 0 ? "none" : ((a & 1u) != 0 ? ((a & 2u) != 0 ? "NaN or inf replaced, clipped" : "NaN or inf replaced")
                                              : "clipped (beyond +-4)");
        c += "\n</plugin_state>";
        const std::lock_guard<std::mutex> lock(mutex_);
        if (c == lastContext_)
            return {};
        lastContext_ = c;
        return c;
    }

    // ---- the request -------------------------------------------------------------------------------------------------
    juce::var ChatAgent::toolDefinitions()
    {
        juce::Array<juce::var> tools;
        tools.add(tool("read_file",
                       "Read one of the plugin's files as it is now in the editor. Call this before editing a file you "
                       "have not seen in its current form (the user may have typed in it since the last plugin_state).",
                       claude::object({ { "name", prop("string", "The file's name, such as program.fm") } }),
                       { "name" }));
        tools.add(tool("write_file",
                       "Create a file or replace a file's whole text. Use it for a new program or a large rewrite; use "
                       "edit_file for a change to part of a file. The editor shows the file and the user can undo the "
                       "change there. A write does not play anything: call run_program after the files are ready.",
                       claude::object({ { "name", prop("string", "The file's name: letters, digits, _, - and ., not "
                                                                  "starting with '.', such as program.fm or "
                                                                  "filters.fm") },
                                        { "content", prop("string", "The file's whole text") } }),
                       { "name", "content" }));
        tools.add(tool("edit_file",
                       "Replace one exact passage of a file with new text. old_text must occur exactly once in the "
                       "file, with the same whitespace; include enough surrounding lines to make it unique. Prefer "
                       "this to write_file for changes to part of a file.",
                       claude::object({ { "name", prop("string", "The file's name") },
                                        { "old_text", prop("string", "The passage as it is now, exactly") },
                                        { "new_text", prop("string", "What replaces it") } }),
                       { "name", "old_text", "new_text" }));
        tools.add(tool("rename_file",
                       "Rename a file. #include lines that name it are not changed: edit them too.",
                       claude::object({ { "name", prop("string", "The file's name now") },
                                        { "new_name", prop("string", "Its new name") } }),
                       { "name", "new_name" }));
        tools.add(tool("delete_file",
                       "Delete a file the program no longer needs (never the program, the first file). Ask the user "
                       "before deleting a file you did not write.",
                       claude::object({ { "name", prop("string", "The file's name") } }), { "name" }));
        tools.add(tool("run_program",
                       "Compile the files as they are now and, when they compile, play them (as the editor's RUN "
                       "does). Returns the compiler's messages, or the dials of the new program. Call it after every "
                       "set of changes, and fix any error it reports before you finish.",
                       claude::object({}), {}));
        tools.add(tool("set_dial",
                       "Set one of the running program's dials (its parameters) to a value in its range, in the "
                       "parameter's own units. Use it to leave a new sound at good settings, like a preset. The user "
                       "can move the dial afterwards.",
                       claude::object({ { "name", prop("string", "The parameter's name, as declared") },
                                        { "value", prop("number", "The value, inside the parameter's range") } }),
                       { "name", "value" }));
        tools.add(tool("get_status",
                       "Read the plugin's state now: the files, whether the program that plays matches them, the last "
                       "compile, the dials and their values, the CPU load and the output guard's alerts (NaN or inf "
                       "replaced, samples clipped). Use it when debugging what the user hears.",
                       claude::object({}), {}));
        return juce::var(tools);
    }

    juce::var ChatAgent::buildRequest(std::string& beta) const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        juce::Array<juce::var> system;
        system.add(claude::object({ { "type", "text" },
                                    { "text", juce::String::fromUTF8(systemPrompt().c_str()) },
                                    { "cache_control", claude::object({ { "type", "ephemeral" }, { "ttl", "1h" } }) } }));
        juce::var body = claude::object({ { "model", juce::String(turnModel_) },
                                          { "max_tokens", kMaxTokens },
                                          { "stream", true },
                                          { "system", system },
                                          { "tools", toolDefinitions() },
                                          { "messages", messages_ },
                                          { "thinking", claude::object({ { "type", "adaptive" },
                                                                         { "display", "summarized" } }) } });
        auto* o = body.getDynamicObject();
        const auto model = std::find_if(models_.begin(), models_.end(),
                                        [&](const ChatModel& m) { return m.id == turnModel_; });
        if (model != models_.end()
            && std::find(model->efforts.begin(), model->efforts.end(), turnEffort_) != model->efforts.end())
            o->setProperty("output_config", claude::object({ { "effort", juce::String(turnEffort_) } }));
        o->setProperty("cache_control", claude::object({ { "type", "ephemeral" } }));   // the conversation, automatic
        beta.clear();
        if (takesDefaultFallbacks(turnModel_))
        {
            o->setProperty("fallbacks", "default");
            beta = kFallbackBeta;
        }
        return body;
    }

    // ---- the turn -----------------------------------------------------------------------------------------------------
    bool ChatAgent::streamOnce(const std::string& body, const std::string& beta, claude::MessageBuilder& builder,
                               int& status, std::string& error, int& retryAfter)
    {
        std::string key;
        claude::Transport* transport = nullptr;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            key = key_;
            transport = transport_.get();
            requests_.push_back(body);
            if (requests_.size() > 64)
                requests_.erase(requests_.begin());
        }
        claude::Headers headers{ { "x-api-key", key },
                                 { "anthropic-version", claude::kApiVersion },
                                 { "content-type", "application/json" } };
        if (!beta.empty())
            headers.emplace_back("anthropic-beta", beta);
        claude::SseParser sse;
        const claude::HttpResult r = transport->post("/v1/messages", body, headers,
                                                     [&](std::string_view chunk) {
                                                         sse.feed(chunk, [&](const std::string& ev, const std::string& data) {
                                                             builder.event(ev, data);
                                                         });
                                                         return !cancel_.load();
                                                     },
                                                     cancel_);
        status = r.status;
        retryAfter = r.retryAfterSeconds;
        if (r.status != 200)
        {
            error = r.status == 0 ? r.error : claude::apiError(r.status, r.body);
            return false;
        }
        error.clear();
        return true;
    }

    void ChatAgent::fetchModels()
    {
        std::string key;
        claude::Transport* transport = nullptr;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (modelsFetched_ || key_.empty())
                return;
            modelsFetched_ = true;
            key = key_;
            transport = transport_.get();
        }
        const claude::Headers headers{ { "x-api-key", key }, { "anthropic-version", claude::kApiVersion } };
        const claude::HttpResult r = transport->get("/v1/models?limit=100", headers, cancel_);
        if (r.status == 401)
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            keyProblem_ = claude::apiError(r.status, r.body);
            touch();
            return;
        }
        if (r.status != 200)
            return;                                // the built-in list stays
        const juce::var v = claude::parseJson(r.body);
        std::vector<ChatModel> found;
        const juce::var models = v.getProperty("data", juce::var());
        if (const auto* data = models.getArray())
            for (const auto& m : *data)
            {
                const juce::var caps = m.getProperty("capabilities", juce::var());
                const bool adaptive = static_cast<bool>(caps.getProperty("thinking", juce::var())
                                                            .getProperty("types", juce::var())
                                                            .getProperty("adaptive", juce::var())
                                                            .getProperty("supported", false));
                if (!adaptive)
                    continue;                      // the requests use adaptive thinking
                ChatModel cm{ str(m, "id"), str(m, "display_name"), {} };
                const juce::var effort = caps.getProperty("effort", juce::var());
                for (const std::string& e : kAllEfforts)
                    if (static_cast<bool>(effort.getProperty(juce::String(e), juce::var()).getProperty("supported", false)))
                        cm.efforts.push_back(e);
                if (cm.name.empty())
                    cm.name = cm.id;
                found.push_back(std::move(cm));
            }
        if (found.empty())
            return;
        const std::lock_guard<std::mutex> lock(mutex_);
        models_ = std::move(found);
        if (std::none_of(models_.begin(), models_.end(), [&](const ChatModel& m) { return m.id == model_; }))
        {
            const auto def = std::find_if(models_.begin(), models_.end(),
                                          [](const ChatModel& m) { return m.id == kDefaultModel; });
            model_ = def != models_.end() ? def->id : models_.front().id;
        }
        touch();
    }

    void ChatAgent::runTurn()
    {
        ensureKey();
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (key_.empty())
            {
                addEntry(ChatEntry::Kind::error, keyProblem_.empty()
                                                     ? "Add your Anthropic API key (KEY, above) to chat with Claude."
                                                     : "The saved API key cannot be used: " + keyProblem_);
                busy_ = false;
                activity_.clear();
                return;
            }
        }
        fetchModels();
        {
            // The model and the thinking level for the whole turn: a change while it runs applies to the next one.
            const std::lock_guard<std::mutex> lock(mutex_);
            turnModel_ = model_;
            turnEffort_ = effort_;
        }

        for (int round = 0; round < kMaxRounds && !cancel_.load(); ++round)
        {
            std::string beta;
            const std::string body = claude::toJson(buildRequest(beta));

            std::unique_ptr<claude::MessageBuilder> builder;
            bool ok = false;
            std::string error;
            for (int attempt = 0; attempt < kMaxAttempts && !cancel_.load(); ++attempt)
            {
                setActivity("THINKING");
                claude::MessageBuilder::Callbacks cb;
                cb.text = [this](std::string_view t) {
                    {
                        const std::lock_guard<std::mutex> lock(mutex_);
                        appendToLast(ChatEntry::Kind::assistant, t);
                    }
                    setActivity("WRITING");
                };
                cb.thinking = [this](std::string_view t) {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    appendToLast(ChatEntry::Kind::thinking, t);
                };
                cb.toolStart = [this](const std::string& name) {
                    const std::string what = name == "write_file" || name == "edit_file" ? "WRITING CODE"
                                           : name == "run_program"                     ? "ABOUT TO RUN"
                                                                                       : "USING " + name;
                    setActivity(what);
                };
                cb.fallback = [this](const std::string& from, const std::string& to) {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    addEntry(ChatEntry::Kind::note, from + " declined this request; " + to + " carried on.");
                };
                builder = std::make_unique<claude::MessageBuilder>(std::move(cb));
                int status = 0, retryAfter = -1;
                ok = streamOnce(body, beta, *builder, status, error, retryAfter);
                if (ok || cancel_.load())
                    break;
                const bool retryable = status == 0 || status == 429 || status == 529 || status >= 500;
                if (!retryable || attempt + 1 == kMaxAttempts)
                    break;
                const int wait = retryAfter > 0 ? std::min(retryAfter, 60) : (2 << attempt);
                for (int s = wait; s > 0 && !cancel_.load(); --s)
                {
                    setActivity("RETRYING IN " + std::to_string(s) + " S");
                    for (int k = 0; k < 10 && !cancel_.load(); ++k)
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }

            const bool stopped = cancel_.load();
            if (!ok && !stopped)
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                addEntry(ChatEntry::Kind::error, error.empty() ? "The request failed." : error);
                break;                             // the user's message stays last: the next one joins it
            }

            // The answer's blocks, as they go back. After a fallback block the blocks before it keep only their text
            // (the API's rule for echoing a fallback turn); the block itself is an audit marker and is dropped.
            juce::Array<juce::var> blocks;
            const juce::var content = builder != nullptr ? builder->content() : juce::var();
            if (const auto* arr = content.getArray())   // the var holds the array: never getArray() on a temporary
                blocks = *arr;
            int lastFallback = -1;
            for (int i = 0; i < blocks.size(); ++i)
                if (str(blocks[i], "type") == "fallback")
                    lastFallback = i;
            if (lastFallback >= 0)
            {
                juce::Array<juce::var> kept;
                for (int i = 0; i < blocks.size(); ++i)
                    if (i > lastFallback || (i < lastFallback && str(blocks[i], "type") == "text"))
                        kept.add(blocks[i]);
                blocks = kept;
            }
            const std::string stopReason = builder != nullptr ? builder->stopReason() : std::string();
            const bool complete = builder != nullptr && builder->finished() && builder->error().empty();

            if (builder != nullptr)
            {
                const claude::Usage& u = builder->usage();
                const std::lock_guard<std::mutex> lock(mutex_);
                usage_ = "LAST REPLY " + thousands(u.input + u.cacheRead + u.cacheWrite) + " IN ("
                       + thousands(u.cacheRead) + " CACHED)  " + thousands(u.output) + " OUT";
                touch();
            }

            if (stopped || !complete || stopReason == "refusal")
            {
                // The turn ends here. The answer goes back as its text alone (an empty one as a short note), so the
                // history stays valid and the next message can follow it.
                juce::Array<juce::var> textOnly;
                for (const auto& b : blocks)
                    if (str(b, "type") == "text" && !str(b, "text").empty())
                        textOnly.add(b);
                if (textOnly.isEmpty())
                    textOnly.add(text(stopped ? "(stopped by the user)" : "(no answer)"));
                const std::lock_guard<std::mutex> lock(mutex_);
                messages_.add(claude::object({ { "role", "assistant" }, { "content", textOnly } }));
                if (stopped)
                    addEntry(ChatEntry::Kind::note, "Stopped.");
                else if (stopReason == "refusal")
                    addEntry(ChatEntry::Kind::error, "Claude declined this request.");
                else
                    addEntry(ChatEntry::Kind::error, builder != nullptr && !builder->error().empty()
                                                         ? "The answer broke off: " + builder->error()
                                                         : "The connection ended before the answer did.");
                break;
            }

            std::vector<juce::var> toolUses;
            for (const auto& b : blocks)
                if (str(b, "type") == "tool_use")
                    toolUses.push_back(b);
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                messages_.add(claude::object({ { "role", "assistant" }, { "content", blocks } }));
                touch();
            }
            if (toolUses.empty())
            {
                if (stopReason == "max_tokens")
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    addEntry(ChatEntry::Kind::note, "The answer reached its length limit.");
                }
                break;                             // end_turn
            }

            // The tools, in order; their results in one user message.
            juce::Array<juce::var> results;
            for (const auto& use : toolUses)
            {
                const std::string id = str(use, "id");
                bool invalid = false;
                std::string raw;
                for (const auto& [badId, badJson] : builder->invalidToolInputs())
                    if (badId == id)
                    {
                        invalid = true;
                        raw = badJson;
                    }
                if (stopReason == "max_tokens" || invalid || cancel_.load())
                {
                    const std::string why = stopReason == "max_tokens"
                                                ? "Your reply was cut off at its length limit before this call was "
                                                  "complete. Do the work in smaller steps (edit_file, or several "
                                                  "files)."
                                            : invalid ? claude::toJson(claude::object({ { "INVALID_JSON",
                                                                                          juce::String::fromUTF8(raw.c_str()) } }))
                                                      : "Stopped by the user before this ran.";
                    results.add(claude::object({ { "type", "tool_result" },
                                                 { "tool_use_id", juce::String(id) },
                                                 { "is_error", true },
                                                 { "content", juce::String::fromUTF8(why.c_str()) } }));
                    continue;
                }
                results.add(runTool(use));
            }
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                messages_.add(claude::object({ { "role", "user" }, { "content", results } }));
                touch();
            }
            if (cancel_.load())
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                addEntry(ChatEntry::Kind::note, "Stopped.");
                break;
            }
            if (round + 1 == kMaxRounds)
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                addEntry(ChatEntry::Kind::note, "Claude used this turn's " + std::to_string(kMaxRounds)
                                                    + " requests; send a message to let it carry on.");
            }
        }

        const std::lock_guard<std::mutex> lock(mutex_);
        busy_ = false;
        activity_.clear();
        touch();
    }

    // ---- the tools ----------------------------------------------------------------------------------------------------
    juce::var ChatAgent::runTool(const juce::var& use)
    {
        const std::string name = str(use, "name");
        const juce::var input = use.getProperty("input", juce::var());
        bool isError = false;
        const std::string result = toolText(name, input, isError);
        juce::var block = claude::object({ { "type", "tool_result" },
                                           { "tool_use_id", use.getProperty("id", "") },
                                           { "content", juce::String::fromUTF8(result.c_str()) } });
        if (isError)
            block.getDynamicObject()->setProperty("is_error", true);
        return block;
    }

    std::string ChatAgent::toolText(const std::string& name, const juce::var& input, bool& isError)
    {
        const auto fail = [&](std::string why) {
            isError = true;
            return why;
        };
        const auto note = [this](std::string t) {
            const std::lock_guard<std::mutex> lock(mutex_);
            addEntry(ChatEntry::Kind::tool, std::move(t));
        };
        const auto stringArg = [&](const char* k, std::string& out) {
            const juce::var v = input.getProperty(k, juce::var());
            if (!v.isString())
                return false;
            out = v.toString().toStdString();
            return true;
        };
        const auto findFile = [](const Draft& d, const std::string& n) {
            for (std::size_t i = 0; i < d.files.size(); ++i)
                if (d.files[i].name == n)
                    return static_cast<int>(i);
            return -1;
        };
        const auto names = [](const Draft& d) {
            std::string out;
            for (const SourceFile& f : d.files)
                out += (out.empty() ? "" : ", ") + f.name;
            return out;
        };

        if (name == "read_file")
        {
            std::string file;
            if (!stringArg("name", file))
                return fail("read_file needs a name");
            const Draft d = host_.chatDraft();
            const int i = findFile(d, file);
            if (i < 0)
                return fail("There is no file " + file + ". The files: " + names(d));
            setActivity("READING " + file);
            return d.files[static_cast<std::size_t>(i)].text;
        }
        if (name == "write_file")
        {
            std::string file, content;
            if (!stringArg("name", file) || !stringArg("content", content))
                return fail("write_file needs a name and the content");
            if (!validFileName(file))
                return fail("'" + file + "' cannot name a file: use letters, digits, _, - and ., not starting with "
                            "'.', at most " + std::to_string(kMaxNameLength) + " characters");
            Draft d = host_.chatDraft();
            int i = findFile(d, file);
            if (i < 0)
            {
                if (d.files.size() >= kMaxFiles)
                    return fail("The editor holds at most " + std::to_string(kMaxFiles) + " files; delete one first");
                d.files.push_back({ file, content });
                i = static_cast<int>(d.files.size()) - 1;
            }
            else
            {
                d.files[static_cast<std::size_t>(i)].text = content;
            }
            d.active = i;
            setActivity("WRITING " + file);
            host_.chatSetDraft(d);
            const int lines = lineCount(content);
            const std::string count = std::to_string(lines) + (lines == 1 ? " line" : " lines");
            note("wrote " + file + " (" + count + ")");
            std::string out = "Wrote " + file + " (" + count + ").";
            bool included = i == 0;
            for (const SourceFile& f : d.files)
                if (f.text.find("#include \"" + file + "\"") != std::string::npos)
                    included = true;
            if (!included)
                out += " No file #includes it yet.";
            return out;
        }
        if (name == "edit_file")
        {
            std::string file, oldText, newText;
            if (!stringArg("name", file) || !stringArg("old_text", oldText) || !stringArg("new_text", newText))
                return fail("edit_file needs name, old_text and new_text");
            Draft d = host_.chatDraft();
            const int i = findFile(d, file);
            if (i < 0)
                return fail("There is no file " + file + ". The files: " + names(d));
            if (oldText.empty())
                return fail("old_text is empty: give the exact passage to replace");
            std::string& t = d.files[static_cast<std::size_t>(i)].text;
            std::size_t count = 0, at = std::string::npos;
            for (std::size_t p = t.find(oldText); p != std::string::npos; p = t.find(oldText, p + 1))
            {
                if (count == 0)
                    at = p;
                ++count;
            }
            if (count == 0)
                return fail("old_text does not occur in " + file + " (check whitespace and the latest text: "
                            "read_file shows it)");
            if (count > 1)
                return fail("old_text occurs " + std::to_string(count) + " times in " + file
                            + ": include more of the surrounding lines so it occurs once");
            t.replace(at, oldText.size(), newText);
            d.active = i;
            setActivity("EDITING " + file);
            host_.chatSetDraft(d);
            note("edited " + file);
            return "Edited " + file + ".";
        }
        if (name == "rename_file")
        {
            std::string file, to;
            if (!stringArg("name", file) || !stringArg("new_name", to))
                return fail("rename_file needs name and new_name");
            Draft d = host_.chatDraft();
            const int i = findFile(d, file);
            if (i < 0)
                return fail("There is no file " + file + ". The files: " + names(d));
            if (!validFileName(to))
                return fail("'" + to + "' cannot name a file");
            if (findFile(d, to) >= 0)
                return fail("There is a file " + to + " already");
            d.files[static_cast<std::size_t>(i)].name = to;
            host_.chatSetDraft(d);
            note("renamed " + file + " to " + to);
            return "Renamed " + file + " to " + to + ". #include lines naming " + file + " still name it.";
        }
        if (name == "delete_file")
        {
            std::string file;
            if (!stringArg("name", file))
                return fail("delete_file needs a name");
            Draft d = host_.chatDraft();
            const int i = findFile(d, file);
            if (i < 0)
                return fail("There is no file " + file);
            if (i == 0)
                return fail("The first file is the program and cannot be deleted");
            d.files.erase(d.files.begin() + i);
            d.active = std::min(d.active, static_cast<int>(d.files.size()) - 1);
            host_.chatSetDraft(d);
            note("deleted " + file);
            return "Deleted " + file + ".";
        }
        if (name == "run_program")
        {
            setActivity("COMPILING");
            const Draft d = host_.chatDraft();
            const CompileStatus s = host_.chatRun(d.files);
            if (s.ok)
            {
                int dials = 0;
                for (const Control& c : s.controls)
                    dials += c.used ? 1 : 0;
                note("ran it: compiled in " + number(std::round(s.ms)) + " ms, " + std::to_string(dials) + " dial"
                     + (dials == 1 ? "" : "s"));
                return "Compiled in " + number(std::round(s.ms)) + " ms; it plays now.\nIts dials:\n"
                     + dialLines(s.controls, host_) + (s.notes.empty() ? "" : "Notes:\n" + s.notes);
            }
            int errors = 0;
            for (std::size_t p = s.errors.find(": error:"); p != std::string::npos; p = s.errors.find(": error:", p + 1))
                ++errors;
            note("ran it: " + std::to_string(std::max(errors, 1)) + " error" + (errors == 1 ? "" : "s"));
            isError = true;
            return "It did not compile; the program that played before plays on.\n" + s.errors + s.notes;
        }
        if (name == "set_dial")
        {
            std::string dial;
            const juce::var value = input.getProperty("value", juce::var());
            if (!stringArg("name", dial) || !(value.isDouble() || value.isInt() || value.isInt64() || value.isBool()))
                return fail("set_dial needs a name and a number");
            const Controls controls = host_.chatDials();
            for (int c = 0; c < kMaxControls; ++c)
            {
                const Control& k = controls[static_cast<std::size_t>(c)];
                if (!k.used || !juce::String(k.name).equalsIgnoreCase(juce::String(dial)))
                    continue;
                float v = std::clamp(static_cast<float>(static_cast<double>(value)), k.min, k.max);
                if (k.type != ControlType::real)
                    v = std::round(v);
                host_.chatSetDial(c, v);
                note("set " + std::string(k.name) + " to " + number(v));
                return std::string(k.name) + " is set to " + number(v) + " (its range " + number(k.min) + " to "
                     + number(k.max) + ").";
            }
            std::string have;
            for (const Control& k : controls)
                if (k.used)
                    have += (have.empty() ? "" : ", ") + std::string(k.name);
            return fail("The running program has no dial " + dial + ". Its dials: " + (have.empty() ? "none" : have));
        }
        if (name == "get_status")
        {
            const Draft d = host_.chatDraft();
            const Sources running = host_.chatRunning();
            std::string out = "Files: ";
            for (std::size_t i = 0; i < d.files.size(); ++i)
                out += (i == 0 ? "" : ", ") + d.files[i].name + " (" + std::to_string(lineCount(d.files[i].text))
                     + " lines" + (static_cast<int>(i) == d.active ? ", shown" : "") + ")";
            out += "\nWhat plays: ";
            out += running == d.files ? "these files\n" : "an earlier version (the files were edited since the run)\n";
            out += "Last compile: " + compileLine(host_.chatLastStatus());
            out += "Dials:\n" + dialLines(host_.chatDials(), host_);
            out += "CPU: " + number(std::round(host_.chatLoad() * 1000.0f) / 10.0) + "% of the audio thread's time "
                   "(the busiest recent block)\n";
            const uint32_t a = host_.chatAlerts();
            out += "Output guard, last two seconds: ";
            out += a == 0 ? "nothing to report" : ((a & 1u) != 0 ? "NaN or inf samples replaced by 0" : "");
            if ((a & 2u) != 0)
                out += std::string((a & 1u) != 0 ? "; " : "") + "samples beyond +-4 clipped";
            return out + "\n";
        }
        return fail("There is no tool " + name);
    }
}
