// Source/plugin/ChatAgent.cpp: see ChatAgent.h.
#include "plugin/ChatAgent.h"

#include "plugin/ChatStore.h"

#include "dsp/Measure.h"

#include "Library.h"
#include "Product.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>
#include <utility>

namespace testingplaygroundsynth
{
    namespace
    {
        constexpr int kDefaultMaxTokens = 64000;   // a model /v1/models has not described
        constexpr int kMaxTokensCap = 128000;      // the longest answer asked for: a big file is written in one call
        constexpr int kMaxAttempts = 4;            // a request, retried on an overload, a rate limit or a dropped line
        constexpr int kMaxCutOffs = 2;             // replies cut off at max_tokens in a row before a turn ends
        constexpr std::size_t kMaxToolOutput = 12000;   // a tool result's characters (a compile's messages)
        constexpr const char* kDefaultModel = "claude-opus-5-5";
        constexpr const char* kDefaultEffort = "medium";
        constexpr const char* kFallbackBeta = "server-side-fallback-2026-07-01";

        // Server-side compaction (the compaction docs): once a request's input reaches the trigger, the API summarizes
        // the conversation before it and answers from the summary; the summary comes back as a compaction block, which
        // goes back with the rest of the answer, and the API ignores what came before it. The history then starts at
        // the block (appendAnswer, repairHistory).
        constexpr const char* kCompactBeta = "compact-2026-01-12";
        constexpr int kCompactTrigger = 150000;   // input tokens (the API's default; 50,000 at least)
        constexpr const char* kCompactInstructions =
            "Summarize the conversation so far between a user and Claude, who builds and changes FMinus DSP programs "
            "inside an audio plugin through tools (read_file, write_file, edit_file, run_program, set_dial, "
            "measure_output, get_status). The summary replaces that part of the conversation, so keep everything "
            "needed to carry on: what the user asked for, and their preferences about the sound and the code; what "
            "was built: each file and what it holds, the signal flow and the modulation routing, what each dial does "
            "and the values chosen; decisions made and ideas rejected, and why; problems met and how they were "
            "solved, and measurements that matter; what is unfinished or was promised next; and the user's latest "
            "request, word for word. Do not copy the programs' code: the plugin sends the current files after this "
            "summary, and read_file shows any of them. Plain prose and short lists.";

        const std::vector<std::string> kAllEfforts{ "low", "medium", "high", "xhigh", "max" };

        // Before /v1/models answers (or without a key): the current models, which all take adaptive thinking and
        // every effort level; Claude Opus 5.5 takes fast mode too.
        std::vector<ChatModel> builtInModels()
        {
            return { { "claude-opus-5-5", "Claude Opus 5.5", kAllEfforts, 128000, true, true },
                     { "claude-fable-5-1", "Claude Fable 5.1", kAllEfforts, 128000, true },
                     { "claude-sonnet-5-5", "Claude Sonnet 5.5", kAllEfforts, 128000, true },
                     { "claude-haiku-5-5", "Claude Haiku 5.5", kAllEfforts, 128000, true } };
        }

        // The models that take "fallbacks": "default" (a classifier's refusal retried on another model).
        bool takesDefaultFallbacks(const std::string& id)
        {
            return id == "claude-fable-5-1" || id == "claude-opus-5-5" || id == "claude-opus-5"
                || id == "claude-sonnet-5-5";
        }

        // Fast mode (the fast-mode docs; /v1/models does not list it): Claude Opus 5.5, and in research preview
        // Claude Opus 5 and Opus 4.8, whose fast requests need the beta header (without it they get a 400).
        constexpr const char* kFastBeta = "fast-mode-2026-02-01";

        bool takesFast(const std::string& id)
        {
            return id == "claude-opus-5-5" || id == "claude-opus-5" || id == "claude-opus-4-8";
        }

        bool fastNeedsBeta(const std::string& id) { return id == "claude-opus-5" || id == "claude-opus-4-8"; }

        // Whether a refused request's error is about fast mode itself (not open to this key, not on this model).
        bool aboutSpeed(const std::string& error)
        {
            const juce::String e = juce::String(error).toLowerCase();
            return e.contains("speed") || e.contains("fast mode") || e.contains("fast-mode");
        }

        std::string str(const juce::var& v, const char* key)
        {
            return v.getProperty(key, "").toString().toStdString();
        }

        // The credit as the footer says it ("$4.96 credit left").
        std::string creditText(double usd)
        {
            if (!(usd > 0.004))
                return "No credit left";
            char buf[48];
            std::snprintf(buf, sizeof buf, "$%.2f credit left", usd);
            return buf;
        }

        // FNV-1a, 64 bits: how the agent remembers which text of a file Claude has seen.
        uint64_t hashText(std::string_view t)
        {
            uint64_t h = 1469598103934665603ull;
            for (const char c : t)
            {
                h ^= static_cast<uint8_t>(c);
                h *= 1099511628211ull;
            }
            return h;
        }

        // The string value of `key` in a JSON object that may still be arriving: "" until its closing quote has.
        std::string partialString(const std::string& json, const char* key)
        {
            const std::string k = std::string("\"") + key + "\"";
            std::size_t p = json.find(k);
            if (p == std::string::npos)
                return {};
            p = json.find(':', p + k.size());
            if (p == std::string::npos)
                return {};
            p = json.find('"', p + 1);
            if (p == std::string::npos)
                return {};
            std::string out;
            for (std::size_t i = p + 1; i < json.size(); ++i)
            {
                if (json[i] == '\\')
                {
                    if (++i >= json.size())
                        return {};
                    out += json[i];
                }
                else if (json[i] == '"')
                    return out;
                else
                    out += json[i];
            }
            return {};
        }

        // `t` cut to kMaxToolOutput characters at a line's end, saying how much was left out.
        std::string capped(const std::string& t)
        {
            if (t.size() <= kMaxToolOutput)
                return t;
            std::size_t cut = t.rfind('\n', kMaxToolOutput);
            if (cut == std::string::npos || cut < kMaxToolOutput / 2)
                cut = kMaxToolOutput;
            const int lines = static_cast<int>(std::count(t.begin() + static_cast<std::ptrdiff_t>(cut), t.end(), '\n'));
            return t.substr(0, cut) + "\n(" + std::to_string(lines) + " more lines left out: fix these first)\n";
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

        // What one reply holds, told to Claude with the user's message at xhigh and max effort, where thinking is long:
        // it may otherwise draft a whole file in its thinking, write it again, and run out of room (the API's advice
        // for long deliverables at those levels). Once sent it stays in the history as it was.
        std::string limitNote(int maxTokens)
        {
            return "(From the plugin: everything in one reply of yours, your thinking included, counts toward one limit "
                   "of about " + thousands(maxTokens) + " tokens, and a reply that reaches it is cut off. So don't write "
                   "a file out in full in your thinking and then again in the reply: in your thinking settle the design "
                   "and the hard decisions, then write the code once, straight into its files, a few files to a reply.)";
        }

        // After a reply cut off at max_tokens with no tool call: what the turn goes on with.
        std::string cutOffNote(int maxTokens)
        {
            return "(From the plugin: your reply reached its limit of about " + thousands(maxTokens)
                 + " tokens, your thinking included, and was cut off. Go on from where it stopped: write what you have "
                   "worked out into its files now, then do what remains a step at a time.)";
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

        // The panel a program's #pragma panel lines made, in a line: its sections in order ("/" where a row starts),
        // their items (a display as its source and kind), then the labels, small dials, styles, options and colours;
        // none without a panel line.
        std::string panelLine(const PanelSpec& p, const Controls& controls)
        {
            const auto dialName = [&](int c) { return std::string(controls[static_cast<std::size_t>(c)].name); };
            // A .gui file's interface: what is in it, in a line (the file itself is with the others).
            if (!p.gui.empty())
            {
                int shownControls = 0, pages = 0, sections = 0;
                std::string titles;
                for (const GuiNode& n : p.gui.nodes)
                {
                    shownControls += n.slot >= 0 ? (n.slot2 >= 0 ? 2 : 1) : 0;
                    sections += n.kind == GuiKind::section ? 1 : 0;
                    if (n.kind == GuiKind::page)
                    {
                        ++pages;
                        titles += (titles.empty() ? "" : ", ") + n.text;
                    }
                }
                return "Its interface is " + p.gui.file + "'s: " + std::to_string(shownControls) + " controls, "
                       + std::to_string(p.displays.size()) + " displays, " + std::to_string(sections) + " sections"
                       + (pages > 0 ? ", pages " + titles : std::string()) + "\n";
            }
            const auto kindOf = [](DisplayKind k) {
                switch (k)
                {
                    case DisplayKind::meter:  return "meter";
                    case DisplayKind::vu:     return "vu";
                    case DisplayKind::led:    return "led";
                    case DisplayKind::number: return "number";
                    case DisplayKind::scope:  return "scope";
                    case DisplayKind::spectrum: return "spectrum";
                }
                return "";
            };
            const auto itemName = [&](const PanelItem& it) {
                if (!it.display)
                    return dialName(it.index);
                const PanelDisplay& d = p.displays[static_cast<std::size_t>(it.index)];
                return d.name + " (" + kindOf(d.kind) + (d.name == d.source ? std::string() : " of " + d.source) + ")";
            };
            std::string out;
            for (std::size_t k = 0; k < p.sections.size(); ++k)
            {
                const PanelSection& s = p.sections[k];
                out += k == 0 ? "" : (s.newRow ? " / " : " | ");
                out += (s.title.empty() ? std::string("(untitled)") : s.title) + ": ";
                for (std::size_t j = 0; j < s.items.size(); ++j)
                    out += (j == 0 ? "" : " ") + itemName(s.items[j]);
            }
            const char* const styleNames[] = { "knob", "vintage", "fader", "switch", "buttons" };
            std::string labels, small, styles, options, colors;
            for (int c = 0; c < kMaxControls; ++c)
            {
                const std::size_t i = static_cast<std::size_t>(c);
                if (!controls[i].used)
                    continue;
                if (!p.labels[i].empty())
                    labels += (labels.empty() ? "" : ", ") + dialName(c) + " \"" + p.labels[i] + "\"";
                if (p.small[i])
                    small += (small.empty() ? "" : " ") + dialName(c);
                if (p.styles[i] != DialStyle::knob)
                    styles += (styles.empty() ? "" : ", ") + dialName(c) + " " + styleNames[static_cast<int>(p.styles[i])];
                if (!p.options[i].empty())
                {
                    options += (options.empty() ? "" : ", ") + dialName(c);
                    for (const std::string& o : p.options[i])
                        options += " \"" + o + "\"";
                }
                if (!p.colors[i].empty())
                    colors += (colors.empty() ? "" : ", ") + dialName(c) + " " + p.colors[i];
            }
            for (const PanelSection& sec : p.sections)
                if (!sec.color.empty())
                    colors += (colors.empty() ? "" : ", ") + ("\"" + sec.title + "\" ") + sec.color;
            for (const PanelDisplay& d : p.displays)
                if (!d.color.empty())
                    colors += (colors.empty() ? "" : ", ") + d.name + " " + d.color;
            if (out.empty() && labels.empty() && small.empty() && styles.empty() && options.empty() && colors.empty())
                return {};
            return "Its panel: " + (out.empty() ? std::string("no sections") : out) + (labels.empty() ? "" : "; labels " + labels)
                   + (small.empty() ? "" : "; small " + small) + (styles.empty() ? "" : "; styles " + styles)
                   + (options.empty() ? "" : "; options " + options) + (colors.empty() ? "" : "; colours " + colors) + "\n";
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
        fast_ = choice.fast;
        const chatstore::Account account = chatstore::loadAccount();
        account_ = account.username;
        useAccount_ = account.useAccount;
    }

    ChatAgent::~ChatAgent()
    {
        cancel_.store(true);
        authCancel_.store(true);
        {
            const std::lock_guard<std::mutex> lock(authMutex_);
            authStop_ = true;                      // a job still queued is dropped
        }
        authCv_.notify_all();
        joinWorker();
        if (auth_.joinable())
            auth_.join();
    }

    void ChatAgent::setTransport(std::unique_ptr<claude::Transport> transport)
    {
        waitIdle();
        const std::lock_guard<std::mutex> lock(mutex_);
        transport_ = std::move(transport);
        modelsFetched_ = false;
    }

    void ChatAgent::waitIdle()
    {
        joinWorker();
        waitAuth();
    }

    void ChatAgent::queueAuth(std::function<void()> job)
    {
        {
            const std::lock_guard<std::mutex> lock(authMutex_);
            if (authStop_)
                return;
            authJobs_.push_back(std::move(job));
            if (!auth_.joinable())
                auth_ = std::thread([this] { authLoop(); });
        }
        authCv_.notify_all();
    }

    void ChatAgent::authLoop()
    {
        std::unique_lock<std::mutex> lock(authMutex_);
        for (;;)
        {
            authCv_.wait(lock, [this] { return authStop_ || !authJobs_.empty(); });
            if (authStop_)
                return;
            std::function<void()> job = std::move(authJobs_.front());
            authJobs_.pop_front();
            authRunning_ = true;
            lock.unlock();
            job();
            job = nullptr;                         // what it held (a password) goes now
            lock.lock();
            authRunning_ = false;
            authCv_.notify_all();
        }
    }

    void ChatAgent::waitAuth()
    {
        std::unique_lock<std::mutex> lock(authMutex_);
        authCv_.wait(lock, [this] { return authStop_ || (authJobs_.empty() && !authRunning_); });
    }

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
        s.signedIn = !token_.empty() || chatstore::hasToken();
        s.account = account_;
        s.accountProblem = accountProblem_;
        s.credit = s.signedIn ? credit_ : std::string();
        s.details = s.signedIn ? details_ : ChatAccount{};
        s.useAccount = useAccount_;
        s.signingIn = signingIn_;
        s.signInError = signInError_;
        s.busy = busy_;
        s.activity = activity_;
        s.entries = entries_;
        s.models = models_;
        s.model = model_;
        s.effort = effort_;
        s.fast = fast_;
        s.usage = usage_;
        return s;
    }

    bool ChatAgent::saveKey(const std::string& key, std::string& why)
    {
        std::string trimmed = juce::String(key).trim().toStdString();
        if (!chatstore::saveKey(trimmed, &why))
            return false;
        {
            // The one set up last is the one the chat uses.
            const std::lock_guard<std::mutex> lock(mutex_);
            key_ = trimmed;
            keyProblem_.clear();
            modelsFetched_ = false;
            const bool wasAccount = useAccount_;
            useAccount_ = false;
            chatstore::saveAccount({ account_, false });
            addEntry(ChatEntry::Kind::note, std::string("API key saved, encrypted, for both playground plugins on this Mac")
                                                + (wasAccount && (!token_.empty() || chatstore::hasToken())
                                                       ? ": the chat uses it now, instead of your account."
                                                       : "."));
        }
        return true;
    }

    void ChatAgent::forgetKey()
    {
        chatstore::forgetKey();
        const std::lock_guard<std::mutex> lock(mutex_);
        key_.clear();
        keyProblem_.clear();
        const bool signedIn = !token_.empty() || chatstore::hasToken();
        if (!useAccount_ && signedIn)
        {
            useAccount_ = true;                    // the account is what is left
            modelsFetched_ = false;
            chatstore::saveAccount({ account_, true });
        }
        addEntry(ChatEntry::Kind::note, std::string("API key removed from this Mac")
                                            + (signedIn ? ": the chat goes through your account." : "."));
    }

    void ChatAgent::useAccount(bool account)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (useAccount_ == account)
            return;
        useAccount_ = account;
        modelsFetched_ = false;
        chatstore::saveAccount({ account_, account });
        addEntry(ChatEntry::Kind::note, account ? "The chat goes through your TestingPlayground account now."
                                                : "The chat uses your own API key now.");
    }

    // ---- signing in and out ---------------------------------------------------------------------------------------------
    void ChatAgent::signIn(const std::string& username, const std::string& password)
    {
        const std::string user = juce::String(username).trim().toStdString();
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (signingIn_)
                return;
            if (user.empty() || password.empty())
            {
                signInError_ = user.empty() ? "Type your username." : "Type your password.";
                touch();
                return;
            }
            signingIn_ = true;
            signInError_.clear();
            touch();
        }
        queueAuth([this, user, password]() mutable { runSignIn(user, std::move(password)); });
    }

    // POST /auth/login. The password goes in its body, once, and is wiped from memory here after.
    void ChatAgent::runSignIn(const std::string& username, std::string password)
    {
        const std::string host = claude::serverHost();
        claude::Transport* transport = nullptr;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            transport = transport_.get();
        }
        std::string body = claude::toJson(claude::object(
            { { "username", juce::String::fromUTF8(username.c_str()) },
              { "password", juce::String::fromUTF8(password.c_str()) },
              { "client", juce::String(std::string(product::kName) + " " + product::kVersion + " (" + product::kKind + ")") } }));
        std::fill(password.begin(), password.end(), '\0');
        const claude::HttpResult r =
            transport->post(host, "/auth/login", body, { { "content-type", "application/json" } }, {}, authCancel_);
        std::fill(body.begin(), body.end(), '\0');

        const juce::var v = r.status == 200 ? claude::parseJson(r.body) : juce::var();
        const std::string token = str(v, "token");
        const juce::var user = v.getProperty("user", juce::var());
        const std::lock_guard<std::mutex> lock(mutex_);
        signingIn_ = false;
        touch();
        if (r.status != 200)
        {
            const std::string message = claude::errorMessage(r.body);
            signInError_ = r.status == 0 ? (authCancel_.load() ? std::string("Stopped.")
                                                               : "Cannot reach the server (" + claude::hostName(host)
                                                                     + "). Check the connection and try again.")
                         : !message.empty() ? message
                                            : "The server refused the sign-in (HTTP " + std::to_string(r.status) + ").";
            return;
        }
        std::string why;
        if (!chatstore::saveToken(token, &why))
        {
            signInError_ = "Signed in, but the sign-in cannot be kept: " + why + ".";
            return;
        }
        token_ = token;
        const std::string named = str(user, "username");
        account_ = named.empty() ? username : named;
        accountProblem_.clear();
        credit_ = user.hasProperty("balance_usd") ? creditText(static_cast<double>(user.getProperty("balance_usd", 0.0)))
                                                  : std::string();
        details_ = ChatAccount{};                  // the account's view asks the server again (refreshAccount)
        useAccount_ = true;                        // the one set up last
        modelsFetched_ = false;
        chatstore::saveAccount({ account_, true });
        addEntry(ChatEntry::Kind::note, "Signed in as " + account_ + ": the chat goes through the TestingPlayground "
                                        "server, charged to your account" + (credit_.empty() ? "" : " (" + credit_ + ")")
                                        + ".");
    }

    void ChatAgent::signOut()
    {
        std::string token;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            token = token_;
            if (token.empty())
                token = chatstore::loadToken().value_or("");
            const bool had = !token.empty() || chatstore::hasToken();
            chatstore::forgetToken();
            token_.clear();
            credit_.clear();
            details_ = ChatAccount{};
            accountProblem_.clear();
            signInError_.clear();
            const bool key = !key_.empty() || chatstore::hasKey();
            if (useAccount_ && key)
            {
                useAccount_ = false;               // the key is what is left
                modelsFetched_ = false;
            }
            chatstore::saveAccount({ account_, useAccount_ });
            if (had)
                addEntry(ChatEntry::Kind::note, std::string("Signed out")
                                                    + (key && !useAccount_ ? ": the chat uses your API key." : "."));
            touch();
        }
        if (token.empty())
            return;
        // The server ends the session too (a token copied elsewhere stops working), in the background.
        queueAuth([this, token] {
            claude::Transport* transport = nullptr;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                transport = transport_.get();
            }
            const std::string host = claude::serverHost();
            transport->post(host, "/auth/logout", "{}", headersFor({ true, host, token }), {}, authCancel_);
        });
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
        chatstore::saveChoice({ model_, effort_, fast_ });
        touch();
    }

    void ChatAgent::setEffort(const std::string& effort)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto it = std::find_if(models_.begin(), models_.end(), [&](const ChatModel& m) { return m.id == model_; });
        if (it != models_.end() && std::find(it->efforts.begin(), it->efforts.end(), effort) == it->efforts.end())
            return;                                // a level this model does not take
        effort_ = effort;
        chatstore::saveChoice({ model_, effort_, fast_ });
        touch();
    }

    void ChatAgent::setFast(bool on)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        fast_ = on;
        chatstore::saveChoice({ model_, effort_, fast_ });
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
            if (effort_ == "xhigh" || effort_ == "max")
                content.add(text(limitNote(maxTokensFor(model_))));
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
            repairHistory();
            resendState_ = false;                  // the state (whole, after a summary) is in this message
            busy_ = true;
            activity_ = "Thinking";
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
            activity_ = "Stopping";
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
        seen_.clear();
        lastSignature_ = 0;                        // the next message sends the files again
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
        // The panel's pictures (view_panel's images, hundreds of KB each) stay out of the session: a line in each one's
        // place, and a mark that the history was changed, so a session that loads it drops its thinking (bound to the
        // pictures).
        bool cut = false;
        juce::Array<juce::var> messages;
        for (const juce::var& m : messages_)
        {
            const juce::Array<juce::var>* content = m.getProperty("content", juce::var()).getArray();
            bool pictures = false;
            if (content != nullptr)
                for (const juce::var& b : *content)
                    if (str(b, "type") == "tool_result")
                        if (const auto* inner = b.getProperty("content", juce::var()).getArray())
                            for (const juce::var& x : *inner)
                                pictures = pictures || str(x, "type") == "image";
            if (!pictures)
            {
                messages.add(m);
                continue;
            }
            cut = true;
            juce::Array<juce::var> blocks;
            for (const juce::var& b : *content)
            {
                const auto* inner = b.getProperty("content", juce::var()).getArray();
                if (str(b, "type") != "tool_result" || inner == nullptr)
                {
                    blocks.add(b);
                    continue;
                }
                juce::Array<juce::var> kept;
                for (const juce::var& x : *inner)
                    kept.add(str(x, "type") == "image"
                                 ? claude::object({ { "type", "text" }, { "text", "(A picture of the panel: not kept in the session.)" } })
                                 : x);
                juce::var nb = claude::object({});
                for (const auto& kv : b.getDynamicObject()->getProperties())
                    nb.getDynamicObject()->setProperty(kv.name, kv.value);
                nb.getDynamicObject()->setProperty("content", kept);
                blocks.add(nb);
            }
            juce::var nm = claude::object({});
            for (const auto& kv : m.getDynamicObject()->getProperties())
                nm.getDynamicObject()->setProperty(kv.name, kv.value);
            nm.getDynamicObject()->setProperty("content", blocks);
            messages.add(nm);
        }
        juce::var state = claude::object({ { "version", 1 },
                                           { "prompt", juce::String(promptPrint()) },
                                           { "messages", messages },
                                           { "entries", entries } });
        if (cut)
            state.getDynamicObject()->setProperty("pictures_cut", true);
        return claude::toJson(state);
    }

    // What a replayed thinking block is bound to besides the messages before it (the preserved-thinking docs): the
    // system prompt and the tools.
    std::string ChatAgent::promptPrint()
    {
        char buf[24];
        const uint64_t h = hashText(systemPrompt() + '\x1f' + claude::toJson(toolDefinitions()));
        std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
        return buf;
    }

    void ChatAgent::loadState(const std::string& json)
    {
        stop();
        joinWorker();
        const juce::var v = claude::parseJson(json);
        const std::lock_guard<std::mutex> lock(mutex_);
        messages_.clear();
        entries_.clear();
        seen_.clear();
        lastSignature_ = 0;                        // the next message sends the files again
        busy_ = false;
        activity_.clear();
        const juce::var messages = v.getProperty("messages", juce::var()), entries = v.getProperty("entries", juce::var());
        if (const auto* arr = messages.getArray())
            messages_ = *arr;
        if (const auto* arr = entries.getArray())
            for (const auto& e : *arr)
                entries_.push_back({ kindOf(str(e, "k")), str(e, "t") });
        // Saved with another system prompt or other tools (an earlier version of the plugin): the API would refuse
        // its thinking blocks (bound to a different conversation), so the history goes on without them.
        if (str(v, "prompt") != promptPrint() || static_cast<bool>(v.getProperty("pictures_cut", false)))
            dropThinking();
        repairHistory();                           // a session saved by a version that cut it at the wrong place
        touch();
    }

    // ---- bookkeeping (mutex held) -------------------------------------------------------------------------------------
    void ChatAgent::touch() { ++revision_; }

    void ChatAgent::appendAnswer(const juce::Array<juce::var>& content)
    {
        messages_.add(claude::object({ { "role", "assistant" }, { "content", content } }));
        const bool compacted = std::any_of(content.begin(), content.end(),
                                           [](const juce::var& b) { return str(b, "type") == "compaction"; });
        if (!compacted)
            return;
        // The API ignores everything before a compaction block: the history starts at it (repairHistory). Claude has
        // seen no file since, as far as the summary goes (it leaves their text out): the state goes again, whole, with
        // what comes next, the user's next message or this turn's next tool results.
        repairHistory();
        seen_.clear();
        lastSignature_ = 0;
        resendState_ = true;
    }

    // A history the API takes. After a compaction block, nothing before it: the API ignores it anyway, and a message
    // kept from before it can be the results of tool calls the summary took away, which the API refuses ("unexpected
    // tool_use_id found in tool_result blocks"). The history then starts with the assistant message that carries the
    // block, as the API's examples continue after one. And no tool result without its call in the message before it:
    // such a result goes (a message left with nothing says so in a line, so the roles still alternate).
    void ChatAgent::repairHistory()
    {
        for (int i = messages_.size(); --i >= 0;)
        {
            if (str(messages_[i], "role") != "assistant")
                continue;
            const juce::var content = messages_[i].getProperty("content", juce::var());
            const auto* blocks = content.getArray();
            int at = -1;
            for (int j = 0; blocks != nullptr && j < blocks->size(); ++j)
                if (str((*blocks)[j], "type") == "compaction")
                    at = j;
            if (at < 0)
                continue;
            if (at > 0)
            {
                juce::Array<juce::var> fromBlock;
                for (int j = at; j < blocks->size(); ++j)
                    fromBlock.add((*blocks)[j]);
                if (auto* o = messages_[i].getDynamicObject())
                    o->setProperty("content", fromBlock);
            }
            messages_.removeRange(0, i);
            break;
        }
        for (int i = 0; i < messages_.size(); ++i)
        {
            if (str(messages_[i], "role") != "user")
                continue;
            std::set<std::string> calls;           // the tool calls of the message before
            const juce::var before = i > 0 ? messages_[i - 1].getProperty("content", juce::var()) : juce::var();
            if (const auto* arr = before.getArray())   // the var holds the array: never getArray() on a temporary
                for (const auto& b : *arr)
                    if (str(b, "type") == "tool_use")
                        calls.insert(str(b, "id"));
            const juce::var content = messages_[i].getProperty("content", juce::var());
            const auto* blocks = content.getArray();
            if (blocks == nullptr)
                continue;
            juce::Array<juce::var> kept;
            for (const auto& b : *blocks)
                if (str(b, "type") != "tool_result" || calls.count(str(b, "tool_use_id")) > 0)
                    kept.add(b);
            if (kept.size() == blocks->size())
                continue;
            if (kept.isEmpty())
                kept.add(text("(The results of tool calls that are no longer in this conversation.)"));
            if (auto* o = messages_[i].getDynamicObject())
                o->setProperty("content", kept);
        }
    }

    // The history without its thinking: an assistant message keeps its text, tool calls and compaction (one left with
    // nothing says so in a line, so the roles still alternate).
    void ChatAgent::dropThinking()
    {
        for (const auto& message : messages_)
        {
            if (str(message, "role") != "assistant")
                continue;
            const juce::var content = message.getProperty("content", juce::var());
            const auto* blocks = content.getArray();
            if (blocks == nullptr)
                continue;
            juce::Array<juce::var> kept;
            for (const auto& b : *blocks)
                if (const std::string type = str(b, "type"); type != "thinking" && type != "redacted_thinking")
                    kept.add(b);
            if (kept.size() == blocks->size())
                continue;
            if (kept.isEmpty())
                kept.add(text("(no answer)"));
            if (auto* o = message.getDynamicObject())
                o->setProperty("content", kept);
        }
    }

    // The max_tokens a request to this model asks for: the model's own limit, at most kMaxTokensCap.
    int ChatAgent::maxTokensFor(const std::string& id) const
    {
        const auto model = std::find_if(models_.begin(), models_.end(), [&](const ChatModel& m) { return m.id == id; });
        return std::min(kMaxTokensCap, model != models_.end() ? model->maxTokens : kDefaultMaxTokens);
    }

    void ChatAgent::markSeen(const std::string& name, const std::string& text) { seen_[name] = hashText(text); }

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

    void ChatAgent::ensureAccess()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        loadAccess();
    }

    void ChatAgent::loadAccess()
    {
        std::string why;
        if (useAccount_)
        {
            if (!token_.empty())
                return;
            if (const auto t = chatstore::loadToken(&why))
            {
                token_ = *t;
                accountProblem_.clear();
            }
            else if (chatstore::hasToken())
                accountProblem_ = why;
            return;
        }
        if (!key_.empty())
            return;
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

    ChatAgent::Access ChatAgent::access() const
    {
        if (useAccount_)
            return { true, claude::serverHost(), token_ };
        return { false, claude::kHost, key_ };
    }

    claude::Headers ChatAgent::headersFor(const Access& a)
    {
        if (a.account)
            return { { "authorization", "Bearer " + a.secret }, { "anthropic-version", claude::kApiVersion } };
        return { { "x-api-key", a.secret }, { "anthropic-version", claude::kApiVersion } };
    }

    // A 401: through the server the sign-in is over (expired, signed out elsewhere, or the admin's doing), so its token
    // goes, unless another sign-in has replaced it since. With the key (from /v1/models), the key is refused: it stays,
    // and the chat says so until the user replaces or removes it.
    void ChatAgent::refused(const Access& a, int status, const std::string& why)
    {
        if (status != 401)
            return;
        if (!a.account)
        {
            keyProblem_ = why;
            touch();
            return;
        }
        if (token_ != a.secret)
            return;
        token_.clear();
        credit_.clear();
        details_ = ChatAccount{};
        if (chatstore::loadToken().value_or("") == a.secret)
            chatstore::forgetToken();
        accountProblem_ = why;
        modelsFetched_ = false;
        touch();
    }

    // ---- the plugin's state, for Claude -------------------------------------------------------------------------------
    // Sent with a user message when anything in it changed since Claude last got one. A file whose text Claude has
    // already seen (in an earlier block, through read_file, or as it wrote it) goes as one line instead of its text, so
    // a long program is not sent again with every message.
    std::string ChatAgent::contextBlock()
    {
        const Draft d = host_.chatDraft();
        const Sources running = host_.chatRunning();
        const CompileStatus status = host_.chatLastStatus();
        const std::string shown =
            d.files.empty() ? std::string(kProgramName)
                            : d.files[static_cast<std::size_t>(std::clamp(d.active, 0, static_cast<int>(d.files.size()) - 1))].name;
        std::string tail = "What plays: "
                         + std::string(running == d.files ? "these files" : "an earlier version of the files (they "
                                                                            "were edited since the last run)")
                         + "\nLast compile: " + compileLine(status) + "Dials of the program that plays:\n"
                         + dialLines(host_.chatDials(), host_) + (status.ok ? panelLine(status.panel, status.controls) : "");
        const uint32_t a = host_.chatAlerts();
        tail += "Output alerts: ";
        tail += a == 0 ? "none" : ((a & 1u) != 0 ? ((a & 2u) != 0 ? "NaN or inf replaced, clipped" : "NaN or inf replaced")
                                                 : "clipped (beyond +-4)");
        uint64_t signature = hashText(tail + '\x1f' + shown);
        for (const SourceFile& f : d.files)
            signature = (signature ^ hashText(f.name + '\x1f' + f.text)) * 1099511628211ull;

        const std::lock_guard<std::mutex> lock(mutex_);
        if (signature == lastSignature_)
            return {};
        lastSignature_ = signature;
        std::string c = "<plugin_state>\nThe plugin's files, a tab each in the editor (the program is the first; the "
                        "editor shows " + shown + "):\n";
        bool unchanged = false;
        for (const SourceFile& f : d.files)
        {
            const auto it = seen_.find(f.name);
            if (it != seen_.end() && it->second == hashText(f.text))
            {
                c += "<file name=\"" + f.name + "\" lines=\"" + std::to_string(lineCount(f.text)) + "\" unchanged=\"true\"/>\n";
                unchanged = true;
                continue;
            }
            c += "<file name=\"" + f.name + "\">\n" + f.text + (f.text.empty() || f.text.back() == '\n' ? "" : "\n")
               + "</file>\n";
            markSeen(f.name, f.text);
        }
        if (unchanged)
            c += "(A file marked unchanged is as you last saw it in this conversation; read_file shows it again.)\n";
        c += tail + "\n</plugin_state>";
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
        {
            juce::Array<juce::var> names;
            for (const LibraryFile& f : kLibrary)
                names.add(juce::String(f.name));
            juce::var name = prop("string", "The library file's name, as THE LIBRARY lists it");
            name.getDynamicObject()->setProperty("enum", names);
            tools.add(tool("add_library_file",
                           "Add one of the plugin's ready-made building blocks (THE LIBRARY in your instructions) to the "
                           "editor as a tab of its own, and get its whole text back. #include it in the program to use "
                           "it. A file of that name in the editor already is left as it is.",
                           claude::object({ { "name", name } }), { "name" }));
        }
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
        {
            juce::var notes = claude::object({ { "type", "array" },
                                               { "items", claude::object({ { "type", "integer" } }) },
                                               { "description", "An instrument's MIDI notes, played together (69 is A, "
                                                                "440 Hz). Default: one note, 48 (C3). An effect "
                                                                "ignores it." } });
            juce::var dials = claude::object({ { "type", "object" },
                                               { "additionalProperties", claude::object({ { "type", "number" } }) },
                                               { "description", "Dial values to render with, by parameter name, in "
                                                                "the parameter's own units. Dials not named keep the "
                                                                "values they have now." } });
            tools.add(tool("measure_output",
                           "Render the program in the editor offline and measure what comes out. An instrument: the "
                           "notes held and released, reporting the held level (peak and RMS in dBFS, DC, the stereo "
                           "correlation), the strongest partials and the spectral centroid, with one note the strongest "
                           "component that is no harmonic of it (aliasing, or the sound's own), the largest step between "
                           "two samples (clicks), when the release falls 60 dB and whether the voice ends, and a "
                           "chord's summed level. An effect: a sine, noise, an impulse and silence through it, "
                           "reporting the gain, the partials it adds, the brightness in and out, the impulse "
                           "response's delay and decay, and any sound it makes from silence. Both: the output "
                           "guard's alerts and the CPU cost. You cannot hear the plugin: measure every new sound once "
                           "it compiles, and measure when debugging what the user hears. It compiles the files as "
                           "they are now (they need not be running) and changes nothing that plays.",
                           claude::object({ { "notes", notes },
                                            { "velocity", prop("number", "An instrument's velocity, 0 to 1 (default 0.8)") },
                                            { "hold_seconds", prop("number", "How long the notes are held (an effect: how "
                                                                             "long the sine and the noise play); default "
                                                                             "1, at most 10") },
                                            { "tail_seconds", prop("number", "How long to go on after the release (an "
                                                                             "effect: after the impulse); default 1.5, at "
                                                                             "most 10") },
                                            { "dials", dials } }),
                           {}));
        }
        tools.add(tool("view_panel",
                       "See the plugin's interface: a picture (PNG) of the program's panel as the user sees it in the "
                       "PANEL view (its panel.gui, or its dials), drawn now from the program that plays. Use it after "
                       "you write or change panel.gui and run it, to check the layout as a designer would: crowding, "
                       "overlaps, text cut off, ragged rows, sizes and colours that do not read, a page left empty; "
                       "then fix what you see. view \"window\" shows the whole editor instead (the code and the "
                       "chat). page names a tabs element's page to show (the text says which pages there are). "
                       "Shaders show as a striped placeholder (the GPU draws them) and the displays show the sound "
                       "playing now. A picture costs about 2,000 input tokens: look once or twice for a new interface, "
                       "not after every small change.",
                       claude::object({ { "view", claude::object({ { "type", "string" },
                                                                   { "enum", juce::Array<juce::var>{ "panel", "window" } },
                                                                   { "description", "panel (the default): the program's "
                                                                                    "panel; window: the whole editor" } }) },
                                        { "page", prop("string", "A page's title to show (a tabs element's), else the "
                                                                 "pages the user has") } }),
                       {}));
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
        const auto model = std::find_if(models_.begin(), models_.end(),
                                        [&](const ChatModel& m) { return m.id == turnModel_; });
        const int maxTokens = maxTokensFor(turnModel_);
        juce::var body = claude::object({ { "model", juce::String(turnModel_) },
                                          { "max_tokens", maxTokens },
                                          { "stream", true },
                                          { "system", system },
                                          { "tools", toolDefinitions() },
                                          { "messages", messages_ },
                                          { "thinking", claude::object({ { "type", "adaptive" },
                                                                         { "display", "summarized" } }) } });
        auto* o = body.getDynamicObject();
        if (model != models_.end()
            && std::find(model->efforts.begin(), model->efforts.end(), turnEffort_) != model->efforts.end())
            o->setProperty("output_config", claude::object({ { "effort", juce::String(turnEffort_) } }));
        o->setProperty("cache_control", claude::object({ { "type", "ephemeral" } }));   // the conversation, automatic
        beta.clear();
        if (turnFast_)
        {
            // Fast mode goes without "fallbacks": the docs say nothing of the two together, and a fallback model need
            // not take fast mode.
            o->setProperty("speed", "fast");
            if (fastNeedsBeta(turnModel_))
                beta = kFastBeta;
        }
        else if (takesDefaultFallbacks(turnModel_))
        {
            o->setProperty("fallbacks", "default");
            beta = kFallbackBeta;
        }
        if (model != models_.end() && model->compaction)
        {
            juce::Array<juce::var> edits;
            edits.add(claude::object({ { "type", "compact_20260112" },
                                       { "trigger", claude::object({ { "type", "input_tokens" },
                                                                     { "value", kCompactTrigger } }) },
                                       { "instructions", kCompactInstructions } }));
            o->setProperty("context_management", claude::object({ { "edits", edits } }));
            beta += std::string(beta.empty() ? "" : ",") + kCompactBeta;
        }
        return body;
    }

    // ---- the turn -----------------------------------------------------------------------------------------------------
    bool ChatAgent::streamOnce(const std::string& body, const std::string& beta, claude::MessageBuilder& builder,
                               int& status, std::string& error, int& retryAfter, bool& viaServer)
    {
        Access a;
        claude::Transport* transport = nullptr;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            loadAccess();                          // the other access, when the user switched during the turn
            a = access();
            transport = transport_.get();
            viaServer = a.account;
            if (a.secret.empty())
            {
                // Signed out (or the key removed) while the turn ran: nothing to send it with.
                status = 401;
                retryAfter = -1;
                error = a.account ? (accountProblem_.empty() ? std::string("You are signed out. Sign in again.")
                                                             : accountProblem_)
                                  : std::string("The API key was removed. Add it again.");
                return false;
            }
            requests_.push_back(body);
            if (requests_.size() > 64)
                requests_.erase(requests_.begin());
        }
        claude::Headers headers = headersFor(a);
        headers.emplace_back("content-type", "application/json");
        if (!beta.empty())
            headers.emplace_back("anthropic-beta", beta);
        claude::SseParser sse;
        const claude::HttpResult r = transport->post(a.host, "/v1/messages", body, headers,
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
            error = r.status == 0 ? r.error
                    : a.account   ? claude::serverError(r.status, r.body)
                                  : claude::apiError(r.status, r.body);
            if (r.status == 401 && a.account)
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                refused(a, r.status, error);
            }
            return false;
        }
        error.clear();
        return true;
    }

    void ChatAgent::fetchModels()
    {
        Access a;
        claude::Transport* transport = nullptr;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            a = access();
            if (modelsFetched_ || a.secret.empty())
                return;
            modelsFetched_ = true;
            transport = transport_.get();
        }
        // Through the server: only the models it can charge for.
        const claude::HttpResult r = transport->get(a.host, "/v1/models?limit=100", headersFor(a), cancel_);
        if (r.status == 401)
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            refused(a, r.status, a.account ? claude::serverError(r.status, r.body) : claude::apiError(r.status, r.body));
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
                cm.maxTokens = static_cast<int>(m.getProperty("max_tokens", kDefaultMaxTokens));
                if (cm.maxTokens <= 0)
                    cm.maxTokens = kDefaultMaxTokens;
                cm.compaction = static_cast<bool>(caps.getProperty("context_management", juce::var())
                                                      .getProperty("compact_20260112", juce::var())
                                                      .getProperty("supported", false));
                cm.fast = takesFast(cm.id);        // /v1/models has no capability for it
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

    // GET /account: the credit left, for the footer, and the rest for the account's view.
    void ChatAgent::fetchAccount(const std::atomic<bool>& cancel, bool asked)
    {
        Access a;
        claude::Transport* transport = nullptr;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (!asked && !useAccount_)
                return;
            if (token_.empty())                    // the view asks while the chat uses the key: the stored sign-in
                if (const auto t = chatstore::loadToken())
                    token_ = *t;
            a = { true, claude::serverHost(), token_ };
            if (a.secret.empty())
            {
                details_.refreshing = false;
                touch();
                return;
            }
            transport = transport_.get();
        }
        const claude::HttpResult r = transport->get(a.host, "/account", headersFor(a), cancel);
        const std::lock_guard<std::mutex> lock(mutex_);
        if (asked)
            details_.refreshing = false;
        if (r.status == 401)
        {
            refused(a, r.status, claude::serverError(r.status, r.body));
            touch();
            return;
        }
        const juce::var v = r.status == 200 ? claude::parseJson(r.body) : juce::var();
        if (token_ != a.secret)
        {
            touch();
            return;
        }
        if (!v.hasProperty("balance_usd"))
        {
            if (asked)
                details_.error = r.status == 0 ? "Cannot reach the server (" + claude::hostName(a.host) + ")."
                                               : claude::serverError(r.status, r.body);
            touch();
            return;
        }
        const double balance = static_cast<double>(v.getProperty("balance_usd", 0.0));
        credit_ = creditText(balance);
        if (const std::string named = str(v, "username"); !named.empty())
            account_ = named;
        ChatAccount d;
        d.known = true;
        d.balanceUsd = balance;
        const juce::var month = v.getProperty("month", juce::var());
        d.requests = static_cast<int64_t>(month.getProperty("requests", 0));
        d.costUsd = static_cast<double>(month.getProperty("cost_usd", 0.0));
        d.inputTokens = static_cast<int64_t>(month.getProperty("input_tokens", 0));
        d.outputTokens = static_cast<int64_t>(month.getProperty("output_tokens", 0));
        const juce::var session = v.getProperty("session", juce::var());
        d.client = str(session, "client");
        d.since = str(session, "created_at");
        d.expires = str(session, "expires_at");
        details_ = std::move(d);
        touch();
    }

    void ChatAgent::refreshAccount()
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (details_.refreshing || (token_.empty() && !chatstore::hasToken()))
                return;
            details_.refreshing = true;
            details_.error.clear();
            touch();
        }
        queueAuth([this] { fetchAccount(authCancel_, true); });
    }

    void ChatAgent::runTurn()
    {
        ensureAccess();
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (useAccount_ ? token_.empty() : key_.empty())
            {
                const bool key = !key_.empty() || chatstore::hasKey();
                addEntry(ChatEntry::Kind::error,
                         useAccount_ ? (!accountProblem_.empty()
                                            ? "Sign in again (KEY, above): " + accountProblem_
                                            : std::string("Sign in (KEY, above) to chat with Claude")
                                                  + (key ? ", or switch to your API key there." : ", or add your own Anthropic API key there."))
                                     : (keyProblem_.empty() ? "Add your Anthropic API key (KEY, above) to chat with Claude."
                                                            : "The saved API key cannot be used: " + keyProblem_));
                busy_ = false;
                activity_.clear();
                return;
            }
        }
        fetchModels();
        int maxTokens = 0;                         // what one reply of this turn's model holds
        {
            // The model and the thinking level for the whole turn: a change while it runs applies to the next one.
            const std::lock_guard<std::mutex> lock(mutex_);
            turnModel_ = model_;
            turnEffort_ = effort_;
            const auto model = std::find_if(models_.begin(), models_.end(),
                                            [&](const ChatModel& m) { return m.id == model_; });
            turnFast_ = fast_ && model != models_.end() && model->fast;
            maxTokens = maxTokensFor(turnModel_);
        }

        int cutOffs = 0;                           // replies cut off at max_tokens in a row, this turn
        for (int round = 0; round < kMaxRounds && !cancel_.load(); ++round)
        {
            std::string beta;
            std::string body = claude::toJson(buildRequest(beta));
            int fastBusy = 0;                      // the fast requests the API turned away as busy, this round

            std::unique_ptr<claude::MessageBuilder> builder;
            bool ok = false;
            std::string error;
            for (int attempt = 0; attempt < kMaxAttempts && !cancel_.load(); ++attempt)
            {
                setActivity("Thinking");
                std::size_t shown = 0;                     // the entries before this attempt's
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    shown = entries_.size();
                }
                auto tool = std::make_shared<std::string>();   // the tool call streaming in
                claude::MessageBuilder::Callbacks cb;
                cb.text = [this](std::string_view t) {
                    {
                        const std::lock_guard<std::mutex> lock(mutex_);
                        appendToLast(ChatEntry::Kind::assistant, t);
                    }
                    setActivity("Writing");
                };
                cb.thinking = [this](std::string_view t) {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    appendToLast(ChatEntry::Kind::thinking, t);
                };
                cb.toolStart = [this, tool](const std::string& name) {
                    *tool = name;
                    setActivity(name == "write_file" || name == "edit_file" ? std::string("Writing code")
                                : name == "run_program"                     ? std::string("About to run the program")
                                : name == "measure_output"                  ? std::string("About to measure the sound")
                                : name == "view_panel"                      ? std::string("Looking at the panel")
                                                                            : "Using " + name);
                };
                cb.toolInput = [this, tool](const std::string& json) {
                    // A big file takes a while to arrive: its name and size so far.
                    if (*tool != "write_file" && *tool != "edit_file")
                        return;
                    const std::string file = partialString(json, "name");
                    const std::string kb = std::to_string((json.size() + 1023) / 1024) + " KB";
                    setActivity((*tool == "write_file" ? "Writing " : "Editing ") + (file.empty() ? std::string("code") : file)
                                + " (" + kb + ")");
                };
                cb.fallback = [this](const std::string& from, const std::string& to) {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    addEntry(ChatEntry::Kind::note, from + " declined this request; " + to + " carried on.");
                };
                cb.compaction = [this](std::size_t) {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    addEntry(ChatEntry::Kind::note, "The conversation got long, so its earlier part was summarized to "
                                                    "make room; Claude carries on from the summary.");
                };
                builder = std::make_unique<claude::MessageBuilder>(std::move(cb));
                int status = 0, retryAfter = -1;
                bool viaServer = false;
                ok = streamOnce(body, beta, *builder, status, error, retryAfter, viaServer);
                if (cancel_.load())
                    break;
                // A stream that broke off (the connection dropped, or the server was overloaded mid-answer) is asked
                // again, as a failed request is: nothing of it has gone into the history yet.
                const std::string& et = builder->errorType();
                const bool brokeOff = ok && !builder->finished()
                                   && (builder->error().empty() || et == "overloaded_error" || et == "api_error");
                if (ok && !brokeOff)
                    break;
                // Fast mode turned away: busy again after one wait (its rate limit, or its capacity), or refused
                // (not open to this key, not on this model). The turn goes on at standard speed (the fast-mode docs'
                // fallback; the cache starts again, as speeds share no cached prefix), and a refusal turns it off.
                bool fastNow = false;
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    fastNow = turnFast_;
                }
                const bool fastRefused = fastNow && !ok && status >= 400 && status < 500 && status != 429
                                      && aboutSpeed(error);
                const bool fastBusyAgain = fastNow && !ok && (status == 429 || status == 529) && ++fastBusy >= 2;
                if (fastRefused || fastBusyAgain)
                {
                    {
                        const std::lock_guard<std::mutex> lock(mutex_);
                        turnFast_ = false;
                        if (fastRefused)
                        {
                            fast_ = false;
                            chatstore::saveChoice({ model_, effort_, fast_ });
                            addEntry(ChatEntry::Kind::note, "Fast mode is not open to this request (" + error
                                                                + "): it is off now, and this turn goes on at "
                                                                  "standard speed.");
                        }
                        else
                            addEntry(ChatEntry::Kind::note,
                                     "Fast mode is busy; this turn goes on at standard speed.");
                    }
                    body = claude::toJson(buildRequest(beta));
                    --attempt;                     // the standard request gets attempts of its own
                    continue;
                }
                // The server's own 502 (Anthropic refused its key) does not pass with time.
                const bool retryable = brokeOff || status == 0 || status == 429 || status == 529
                                    || (status >= 500 && !(viaServer && status == 502));
                if (!retryable || attempt + 1 == kMaxAttempts)
                    break;
                if (brokeOff)
                {
                    // What the broken attempt showed goes; the next attempt streams the answer again.
                    const std::lock_guard<std::mutex> lock(mutex_);
                    if (entries_.size() > shown)
                        entries_.resize(shown);
                    addEntry(ChatEntry::Kind::note, "The answer broke off; asking again.");
                }
                const int wait = retryAfter > 0 ? std::min(retryAfter, 60) : (2 << attempt);
                for (int s = wait; s > 0 && !cancel_.load(); --s)
                {
                    setActivity("Retrying in " + std::to_string(s) + " s");
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
                    if (i > lastFallback
                        || (i < lastFallback && (str(blocks[i], "type") == "text" || str(blocks[i], "type") == "compaction")))
                        kept.add(blocks[i]);
                blocks = kept;
            }
            const std::string stopReason = builder != nullptr ? builder->stopReason() : std::string();
            const bool complete = builder != nullptr && builder->finished() && builder->error().empty();

            if (builder != nullptr)
            {
                const claude::Usage& u = builder->usage();
                const std::lock_guard<std::mutex> lock(mutex_);
                usage_ = "Last reply: " + thousands(u.input + u.cacheRead + u.cacheWrite) + " tokens in ("
                       + thousands(u.cacheRead) + " cached), " + thousands(u.output) + " out"
                       + (u.speed == "fast" ? ", fast" : "")
                       + (u.compactions > 0 ? "; the conversation was summarized" : "");
                touch();
            }

            if (stopped || !complete || stopReason == "refusal")
            {
                // The turn ends here. The answer goes back as its text alone (an empty one as a short note), so the
                // history stays valid and the next message can follow it.
                juce::Array<juce::var> textOnly;
                bool hasText = false;
                for (const auto& b : blocks)
                {
                    const std::string type = str(b, "type");
                    if (type == "compaction")
                        textOnly.add(b);                   // a summary made in this request stays (no second one)
                    else if (type == "text" && !str(b, "text").empty())
                    {
                        textOnly.add(b);
                        hasText = true;
                    }
                }
                if (!hasText)
                    textOnly.add(text(stopped ? "(stopped by the user)" : "(no answer)"));
                const std::lock_guard<std::mutex> lock(mutex_);
                appendAnswer(textOnly);
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
                appendAnswer(blocks);
                touch();
            }
            // A reply cut off at max_tokens (its thinking counts toward it): the turn goes on, Claude told so, unless
            // the reply before it was cut off too, since a reply that only thinks costs a whole reply each time.
            const bool cutOff = stopReason == "max_tokens";
            cutOffs = cutOff ? cutOffs + 1 : 0;
            const bool endsHere = cutOff && cutOffs >= kMaxCutOffs;
            if (cutOff)
            {
                const std::string limit = "Claude's reply reached its length limit (" + thousands(maxTokens)
                                        + " tokens, its thinking included)";
                const std::lock_guard<std::mutex> lock(mutex_);
                addEntry(ChatEntry::Kind::note,
                         endsHere ? limit + " twice in a row, so the turn ends here. Send \"continue\" to let it go "
                                            "on. A lower thinking level leaves it more room to write: at Max its "
                                            "thinking has no limit of its own."
                                  : limit + ", so it goes on from where it stopped.");
            }
            if (toolUses.empty() && (!cutOff || endsHere))
                break;                             // end_turn, or cut off again

            // The tools, in order, their results in one user message; after a reply cut off without a tool call, a
            // line that says so.
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
                if (cutOff || invalid || cancel_.load())
                {
                    const std::string why = cutOff
                                                ? "Your reply reached its limit of about " + thousands(maxTokens)
                                                      + " tokens, your thinking included, and was cut off before this "
                                                        "call was complete, so it did not run. Do the work in smaller "
                                                        "steps (edit_file, or several files)."
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
            if (toolUses.empty())
                results.add(text(cutOffNote(maxTokens)));
            // A summary in this request left the files out of what Claude has: the state goes with the results, after
            // them, so that the turn goes on with the code in front of it.
            bool resend = false;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                resend = std::exchange(resendState_, false);
            }
            if (resend)
                if (const std::string context = contextBlock(); !context.empty())
                    results.add(text(context));
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
            if (endsHere)
                break;                             // the results are in the history; the next message joins them
            if (round + 1 == kMaxRounds)
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                addEntry(ChatEntry::Kind::note, "Claude used this turn's " + std::to_string(kMaxRounds)
                                                    + " requests; send a message to let it carry on.");
            }
        }

        if (!cancel_.load())
            fetchAccount(cancel_, false);          // the credit left, for the footer (through the account only)

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
        if (name == "view_panel")
        {
            // A picture of the panel: an image block and a line of what it shows (the tool_result's content).
            setActivity("Looking at the panel");
            const std::string view = str(input, "view");
            const PanelPicture pic = host_.chatPicture(view == "window", str(input, "page"));
            juce::var block = claude::object({ { "type", "tool_result" }, { "tool_use_id", use.getProperty("id", "") } });
            if (pic.png.empty())
            {
                block.getDynamicObject()->setProperty("content", juce::String::fromUTF8(("No picture: " + pic.why).c_str()));
                block.getDynamicObject()->setProperty("is_error", true);
                return block;
            }
            juce::Array<juce::var> content;
            content.add(claude::object({ { "type", "image" },
                                         { "source", claude::object({ { "type", "base64" },
                                                                      { "media_type", "image/png" },
                                                                      { "data", juce::Base64::toBase64(pic.png.data(), pic.png.size()) } }) } }));
            content.add(claude::object({ { "type", "text" }, { "text", juce::String::fromUTF8(pic.about.c_str()) } }));
            block.getDynamicObject()->setProperty("content", content);
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                addEntry(ChatEntry::Kind::tool, view == "window" ? "Looked at the editor" : "Looked at the panel");
            }
            return block;
        }
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
            setActivity("Reading " + file);
            const std::string& t = d.files[static_cast<std::size_t>(i)].text;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                markSeen(file, t);
            }
            return t;
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
            setActivity("Writing " + file);
            host_.chatSetDraft(d);
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                markSeen(file, content);
            }
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
            setActivity("Editing " + file);
            host_.chatSetDraft(d);
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                markSeen(file, t);
            }
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
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                if (const auto it = seen_.find(file); it != seen_.end())
                {
                    seen_[to] = it->second;
                    seen_.erase(file);
                }
            }
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
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                seen_.erase(file);
            }
            note("deleted " + file);
            return "Deleted " + file + ".";
        }
        if (name == "add_library_file")
        {
            std::string file;
            if (!stringArg("name", file))
                return fail("add_library_file needs a name");
            const LibraryFile* lib = findLibraryFile(file);
            if (lib == nullptr)
            {
                std::string have;
                for (const LibraryFile& f : kLibrary)
                    have += (have.empty() ? "" : ", ") + std::string(f.name);
                return fail("The library has no file " + file + ". Its files: " + have);
            }
            const std::string text = lib->source;
            const int lines = lineCount(text);
            const std::string count = std::to_string(lines) + (lines == 1 ? " line" : " lines");
            Draft d = host_.chatDraft();
            if (const int i = findFile(d, file); i >= 0)
            {
                if (d.files[static_cast<std::size_t>(i)].text != text)
                    return "The editor has a file " + file + " already, changed from the library's; it is left as it "
                           "is (read_file shows it, write_file replaces it).";
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    markSeen(file, text);
                }
                return "The editor has " + file + " already, as the library has it (" + count + "):\n" + text;
            }
            if (d.files.size() >= kMaxFiles)
                return fail("The editor holds at most " + std::to_string(kMaxFiles) + " files; delete one first");
            setActivity("Adding " + file);
            d.files.push_back({ file, text });
            d.active = static_cast<int>(d.files.size()) - 1;
            host_.chatSetDraft(d);
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                markSeen(file, text);
            }
            note("added " + file + " from the library");
            return "Added " + file + " (" + count + "). #include \"" + file + "\" in the program to use it. Its text:\n"
                   + text;
        }
        if (name == "run_program")
        {
            setActivity("Compiling");
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
                     + dialLines(s.controls, host_) + panelLine(s.panel, s.controls)
                     + (s.notes.empty() ? "" : "Notes:\n" + capped(s.notes));
            }
            int errors = 0;
            for (std::size_t p = s.errors.find(": error:"); p != std::string::npos; p = s.errors.find(": error:", p + 1))
                ++errors;
            note("ran it: " + std::to_string(std::max(errors, 1)) + " error" + (errors == 1 ? "" : "s"));
            isError = true;
            return "It did not compile; the program that played before plays on.\n" + capped(s.errors + s.notes);
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
        if (name == "measure_output")
        {
            setActivity("Measuring the sound");
            measure::Request req;
            const auto numberArg = [&](const char* k, float& out) {
                const juce::var v = input.getProperty(k, juce::var());
                if (v.isDouble() || v.isInt() || v.isInt64())
                    out = static_cast<float>(static_cast<double>(v));
            };
            const juce::var notes = input.getProperty("notes", juce::var());
            if (const auto* arr = notes.getArray())
                for (const auto& n : *arr)
                    if (n.isInt() || n.isInt64() || n.isDouble())
                        req.notes.push_back(static_cast<int>(std::lround(static_cast<double>(n))));
            numberArg("velocity", req.velocity);
            numberArg("hold_seconds", req.holdSeconds);
            numberArg("tail_seconds", req.tailSeconds);
            const juce::var dials = input.getProperty("dials", juce::var());
            if (const auto* o = dials.getDynamicObject())
                for (const auto& kv : o->getProperties())
                    if (kv.value.isDouble() || kv.value.isInt() || kv.value.isInt64() || kv.value.isBool())
                        req.dials[kv.name.toString().toStdString()] = static_cast<float>(static_cast<double>(kv.value));
            const Controls controls = host_.chatDials();
            for (int c = 0; c < kMaxControls; ++c)
                if (controls[static_cast<std::size_t>(c)].used)
                    req.current[controls[static_cast<std::size_t>(c)].name] = host_.chatDialValue(c);
            const std::string report = measure::run(host_.chatDraft().files, req);
            note(report.rfind("It did not compile", 0) == 0 ? "measured the sound: it did not compile"
                                                             : "measured the sound");
            return report;
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
