// Source/plugin/ChatPort.h: what the editor's chat reaches (JUCE-free; message thread). The processor's ChatAgent
// implements it (ChatAgent.h): a conversation with Claude, which can read and write the program's files, run them,
// set the dials and answer questions, through the Claude API with the user's own key.
//
//   chatRevision()   bumps whenever anything below changes (a streamed word included)
//   chatState()      the conversation as the view shows it, the models and the choice, the key's state
//   saveKey()        stores the key, encrypted (ChatStore.h), and uses it; false, with why, for one that is not a key
//   forgetKey()      deletes the stored key
//   setProvider()    the model provider (Anthropic, an OpenAI-compatible server by base URL, Google Gemini), with its
//                    key, stored encrypted and bound to that server ("" keeps the one stored, or none: a server on this
//                    Mac or the local network needs none), and its model; false, with why, for a base URL or a key it
//                    cannot take. providerInfo() what is kept for one (its key, its model); forgetProviderKey() deletes
//                    its key. Anthropic's stays as it was: saveKey and forgetKey are its key's
//   setPlanner()     the model that plans each request before the chosen model builds it ("" for none): it writes a
//                    plan, which the chat shows and the builder follows, and answers the builder's ask_planner
//   setModel()       a model of chatState().models; setEffort() a thinking level that model has (with another provider:
//                    kept for that provider's model, "" for none chosen: the model's own default, nothing sent);
//                    setFast() fast mode, on the models that take it; setWeb() web search (Claude searches the web and
//                    reads pages and PDFs, the API's server tools), with the user's own key on the models that take
//                    it. All four machine-wide
//   attach()         pictures (files) for the next message: read in the background (ChatImages.h), then in
//                    chatState().attached; one that cannot be read gets a note. detach() takes one back
//   send()           a message from the user (and the pictures attached, which it takes; a message of pictures alone
//                    goes too); Claude answers (and works) in the background. While Claude's question
//                    waits (asking), the message answers it in the user's own words. False when it was not taken
//                    (nothing to send, or the chat busy: a turn, or a call of Claude Desktop's): the box keeps it
//   answer()         the user's answer to Claude's question (ask_user), one per question: the chosen options' labels;
//                    Claude's turn goes on with it
//   clearChecks()    forgets Claude's checks (set_checks): the plugin no longer measures them after a run
//   stop()           ends Claude's turn as soon as it can; the work done so far stays
//   newChat()        a new conversation; the one shown stays in the chat list
//   chatList()       the chats kept on this Mac (ChatStore.h) for the project open, or for this plugin while none is
//                    (a chat with no project stays with it): the newest first, the one shown marked
//   openChat()       another chat of the list shown (the one shown is kept); false when it is no longer there, or
//                    cannot be read (the chat shown says why, and nothing stops)
//   renameChat()     the chat shown named ("" for its first message's words again)
//   deleteChat()     the chat shown deleted from this Mac: the newest other one shows, else a new chat
//   usage()          what the chat's requests used and cost (Usage.h's ledger: every product's, on this Mac) for a range
//                    of days, as last read: never waits. It returns the last view read (none before the first) and asks
//                    for a read on the chat's job thread when the range differs, this process's ledger moved
//                    (usage::revision) or a few seconds passed (another process may have written the file); the
//                    message thread never reads the file or takes its folder's lock. A read that finds what the view
//                    already holds publishes nothing: the view and its serial stay as they were
//   resetUsage()     empties the ledger (the Settings card's Reset, asked first): queued on the job thread, and always
//                    run once asked (the agent's destructor runs one still queued); the view says it is resetting
//                    until it is done, and a reset that failed says why until the next reset
#pragma once

#include "plugin/Usage.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace playground
{
    // A question of Claude's (its ask_user tool), answered with a click.
    struct ChatOption
    {
        std::string label;                    // a few words ("Warm and analog")
        std::string description;              // a line of what it means ("" for none)
    };

    struct ChatQuestion
    {
        std::string header;                   // a short tag ("Sound"), "" for none
        std::string question;
        std::vector<ChatOption> options;      // 2 to 6
        bool multi = false;                   // several may be chosen
    };

    // A picture the user added to a message (attach): what the chat shows of it. The picture itself, as sent, is the
    // agent's.
    struct ChatPicture
    {
        uint32_t id = 0;                      // unique in the process: the editor draws its thumbnail by it
        std::string name;                     // the file's
        int width = 0, height = 0;            // as sent
        int thumbWidth = 0, thumbHeight = 0;
        std::vector<uint8_t> thumb;           // straight RGBA8, row by row
        std::vector<uint8_t> thumbData;       // the thumbnail encoded (the session keeps it)
        std::string thumbType;                // image/png or image/jpeg
    };
    using ChatPicturePtr = std::shared_ptr<const ChatPicture>;

    // A file's name a picture's: what attach() reads and the chat takes from a drop.
    inline bool isPictureName(std::string_view path)
    {
        const std::size_t dot = path.rfind('.');
        if (dot == std::string_view::npos)
            return false;
        std::string ext(path.substr(dot + 1));
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const char* e : { "png", "jpg", "jpeg", "gif", "webp", "heic", "heif", "tif", "tiff", "bmp" })
            if (ext == e)
                return true;
        return false;
    }

    // One of Claude's checks (its set_checks tool): what it checks and how it came out the last time the plugin
    // measured it (after each run of the program).
    struct ChatCheck
    {
        std::string label;                    // a few words ("Releases within two seconds")
        std::string detail;                   // the value against the goal ("2.61 s; at most 2.00 s"), or why it was
                                              // not measured
        bool measured = false;
        bool pass = false;
    };

    struct ChatEntry
    {
        enum class Kind : uint8_t
        {
            user,        // the user's message
            assistant,   // Claude's answer (streams in)
            thinking,    // a summary of Claude's reasoning, or a note between its tool calls
            tool,        // something Claude did: wrote a file, ran the program, set a dial
            error,       // something went wrong: the key, the network, the API
            note,        // the chat's own remarks
            question,    // Claude asks (ask_user): its questions, answered with a click; the turn waits for them
            checks,      // Claude's checks (set_checks), each with its last result; one entry, brought up to date in
                         // place after each run
            plan,        // the planning model's plan for a request, or its answer to the builder's ask_planner (streams
                         // in); `from` names the model
        };
        Kind kind = Kind::note;
        std::string text;                     // a question's: the questions and their options, as plain text
        std::vector<ChatQuestion> questions;  // a question's
        std::vector<std::string> answers;     // a question's answer, one per question: the labels chosen (", "
                                              // between several), "" for none
        bool open = false;                    // a question still waiting for its answer
        bool typed = false;                   // a question answered by a message instead
        std::vector<ChatPicturePtr> pictures; // a user message's, or what a tool showed Claude (view_sound's picture)
        std::vector<ChatCheck> checks;        // a checks entry's
        std::string from;                     // a plan's: the model that wrote it ("Claude Fable 5.1")
        bool answer = false;                  // a plan entry that answers ask_planner (else a request's plan)
        bool failed = false;                  // a tool's: its result was an error (a run that did not compile or
                                              // stopped, a refusal), drawn so
    };

    struct ChatModel
    {
        std::string id;                       // "claude-opus-5-5"
        std::string name;                     // "Claude Opus 5.5"
        std::vector<std::string> efforts;     // the thinking levels it takes: "low", "medium", ...
        int maxTokens = 64000;                // its longest answer (thinking included), from /v1/models
        bool compaction = false;              // it takes server-side compaction (long conversations summarized)
        bool fast = false;                    // it takes fast mode: the same model, its answers written faster, at a
                                              // higher price (the API's speed "fast")
        bool web = true;                      // it takes web search (/v1/models' server_tools.web_search)
        // Another provider's model (Providers.h): whether it takes pictures (a model that refuses one is told so and
        // is sent them as a line of text from then on), and its context window in tokens (0: not said), by which a
        // small one gets the prompt's short form.
        bool images = true;
        int context = 0;
        // Another provider's model: the thinking level it uses when none is sent (one of `efforts`; "" when its server
        // does not say), shown as the level until one is chosen, which alone is ever sent.
        std::string defaultEffort = {};
    };

    // The model provider the chat talks to (plugin/Providers.h): Anthropic (the default), any server that speaks
    // OpenAI's Chat Completions by its base URL (OpenAI, OpenRouter, a server on this Mac or the local network such as
    // Ollama or LM Studio), or Google Gemini. Machine-wide, as the model is.
    struct ChatProvider
    {
        std::string kind = "anthropic";       // "anthropic", "openai" (OpenAI-compatible) or "gemini"
        std::string base;                     // an OpenAI-compatible server's base URL ("https://openrouter.ai/api/v1")
        std::string model;                    // its model ("" for the first the server lists); Anthropic's is the picker's
        friend bool operator==(const ChatProvider&, const ChatProvider&) = default;
    };

    // What the chat keeps for a provider (providerInfo): whether a key is stored for it (bound to its server), whether it
    // needs none (a server on this Mac or the local network), the model chosen with it last, its name as the chat says it.
    struct ChatProviderInfo
    {
        bool hasKey = false;
        bool keyless = false;
        bool picksFirst = false;              // with no model chosen a turn takes its list's first (else the user chooses)
        std::string model;
        std::string name;                     // "OpenRouter", "Ollama (this Mac)", a server's host
    };

    // The providers the settings offer: a kind and a base URL each (the kind's own for Gemini and Anthropic); "custom"
    // is any OpenAI-compatible server, its base URL typed. `key` is how its keys start ("" for a server that needs none),
    // `keys` where one is made.
    struct ChatPreset
    {
        const char* id;
        const char* name;
        const char* kind;
        const char* base;
        const char* key;
        const char* keys;
    };
    inline constexpr ChatPreset kChatPresets[] = {
        { "anthropic", "Anthropic", "anthropic", "https://api.anthropic.com", "sk-ant-", "console.anthropic.com (API keys)" },
        { "openai", "OpenAI", "openai", "https://api.openai.com/v1", "sk-", "platform.openai.com (API keys)" },
        { "openrouter", "OpenRouter", "openai", "https://openrouter.ai/api/v1", "sk-or-", "openrouter.ai (Keys)" },
        { "gemini", "Google Gemini", "gemini", "https://generativelanguage.googleapis.com", "AIza", "aistudio.google.com (Get API key)" },
        // The servers on this machine, named in kThisMachine's words (EditorPort.h).
#if defined(__APPLE__)
        { "ollama", "Ollama (this Mac)", "openai", "http://localhost:11434/v1", "", "" },
        { "lmstudio", "LM Studio (this Mac)", "openai", "http://localhost:1234/v1", "", "" },
#else
        { "ollama", "Ollama (this computer)", "openai", "http://localhost:11434/v1", "", "" },
        { "lmstudio", "LM Studio (this computer)", "openai", "http://localhost:1234/v1", "", "" },
#endif
        { "custom", "Other OpenAI-compatible server", "openai", "", "", "" },
    };

    inline constexpr std::size_t kMaxChatTitle = 60;       // a chat's name, characters at most

    // A chat of the list (chatList): its title, and when it last changed.
    struct ChatListEntry
    {
        std::string id;
        std::string title;
        std::string when;                     // "14:02" today, "Nov 6" this year, else "2025-11-06" (local time)
        bool current = false;                 // the one shown
    };

    struct ChatList
    {
        std::string owner;                    // the project the chats are kept with ("" for none: this plugin's own)
        std::vector<ChatListEntry> chats;     // the newest first
    };

    struct ChatState
    {
        bool hasKey = false;
        std::string keyProblem;               // why the stored key cannot be used ("" when it can)
        bool busy = false;                    // Claude is working on a turn
        bool asking = false;                  // Claude's question (the last question entry) waits for the answer
        bool checks = false;                  // Claude's checks are set (set_checks): measured after each run
        std::vector<ChatPicturePtr> attached; // pictures for the next message
        int attaching = 0;                    // files still being read
        std::string activity;                 // what it is doing: "THINKING", "WRITING filters.fm", ...
        std::vector<ChatEntry> entries;
        uint64_t conversation = 0;            // moves when other words replace the conversation shown (another chat
                                              // opened, a new chat, a session loaded)
        std::vector<ChatModel> models;
        std::string model;                    // the chosen model's id (the builder's)
        std::string planner;                  // the planning model's id ("" for none)
        std::string effort;                   // the chosen thinking level (another provider's: its model's default
                                              // while none is chosen; "" for none known)
        bool fast = false;                    // fast mode chosen (it applies on a model that takes it)
        bool web = false;                     // web search chosen
        std::string usage;                    // the last reply's tokens, for the footer
        std::string title;                    // the chat shown: its name, else its first message's words, else
                                              // "New chat"
        std::string owner;                    // the project its chats are kept with ("" for none)
        ChatProvider provider;                // the provider the chat talks to (its model is `model`)
        std::string providerName;             // as the chat says it ("Anthropic", "OpenRouter", a server's host)
        bool keyless = false;                 // the provider needs no key (a server on this Mac or the local network)

        // The chat has what it needs to send: a key is stored, or the provider needs none.
        bool ready() const noexcept { return hasKey || keyless; }
    };

    // What the Settings card's Usage page shows (ChatPort::usage): the ledger's rows in a range, each with its cost
    // as Usage.h prices it (the price table in use when read). Counts and names only.
    struct UsageRow
    {
        usage::Key key;
        usage::Counts counts;
        usage::Cost cost;                     // the provider's figure, an estimate, or none ("no price known")
        friend bool operator==(const UsageRow& a, const UsageRow& b)
        {
            return a.key == b.key && a.counts == b.counts && a.cost.kind == b.cost.kind && a.cost.microUsd == b.cost.microUsd;
        }
    };

    struct UsageView
    {
        uint64_t serial = 0;                  // moves whenever anything below changes, and only then
        bool read = false;                    // a read has landed (false: none yet)
        usage::Range range = usage::Range::week;   // what the rows are for
        std::string today;                    // the day they were read on ("2026-11-12")
        std::vector<UsageRow> rows;           // in the range, summed by key, in order
        std::string pricesAsOf;               // the price table's date (usage::pricesInUse)
        bool resetting = false;               // a confirmed reset not done yet
        std::string resetError;               // why the last reset failed ("" when it did not), until the next one
    };

    class ChatPort
    {
    public:
        virtual ~ChatPort() = default;

        virtual uint64_t chatRevision() const = 0;
        virtual ChatState chatState() const = 0;

        virtual bool saveKey(const std::string& key, std::string& why) = 0;
        virtual void forgetKey() = 0;
        virtual bool setProvider(const ChatProvider& provider, const std::string& key, std::string& why) = 0;
        virtual ChatProviderInfo providerInfo(const ChatProvider& provider) const = 0;
        virtual void forgetProviderKey(const ChatProvider& provider) = 0;
        virtual void setModel(const std::string& id) = 0;
        virtual void setPlanner(const std::string& id) = 0;
        virtual void setEffort(const std::string& effort) = 0;
        virtual void setFast(bool on) = 0;
        virtual void setWeb(bool on) = 0;

        virtual void attach(const std::vector<std::string>& paths) = 0;
        virtual void detach(uint32_t id) = 0;
        virtual bool send(const std::string& text) = 0;
        virtual void answer(const std::vector<std::string>& answers) = 0;
        virtual void clearChecks() = 0;
        virtual void stop() = 0;
        virtual void newChat() = 0;
        virtual ChatList chatList() const = 0;
        virtual bool openChat(const std::string& id) = 0;
        virtual void renameChat(const std::string& title) = 0;
        virtual void deleteChat() = 0;
        virtual std::shared_ptr<const UsageView> usage(usage::Range range) = 0;
        virtual void resetUsage() = 0;
    };
}
