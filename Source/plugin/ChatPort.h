// Source/plugin/ChatPort.h: what the editor's chat reaches (JUCE-free; message thread). The processor's ChatAgent
// implements it (ChatAgent.h): a conversation with Claude, which can read and write the program's files, run them,
// set the dials and answer questions, through the Claude API: with the user's own key, or through the TestingPlayground
// server once the user has signed in (the server's key, charged to the account's credit). Both can be stored; the
// chat uses the one set up last (useAccount).
//
//   chatRevision()   bumps whenever anything below changes (a streamed word included)
//   chatState()      the conversation as the view shows it, the models and the choice, the key's and the account's
//                    state
//   saveKey()        stores the key, encrypted (ChatStore.h), and uses it; false, with why, for one that is not a key
//   forgetKey()      deletes the stored key
//   signIn()         signs in to the server in the background (chatState().signingIn until it answers); then the token
//                    is stored, encrypted, and the chat uses the account, or signInError says why not. The password
//                    is sent once and kept nowhere
//   signOut()        forgets the token (and ends the session on the server); the chat uses the key, if one is stored
//   useAccount()     which of the two stored the chat uses: true the account, false the key
//   refreshAccount() asks the server for the account again (GET /account), in the background: chatState().details
//                    has its answer (the credit, this month's use, the sign-in), .refreshing until it comes
//   setModel()       a model of chatState().models; setEffort() a thinking level that model has; setFast() fast mode,
//                    on the models that take it. All three machine-wide
//   send()           a message from the user; Claude answers (and works) in the background
//   stop()           ends Claude's turn as soon as it can; the work done so far stays
//   newChat()        forgets the conversation
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace testingplayground
{
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
        };
        Kind kind = Kind::note;
        std::string text;
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
    };

    // The signed-in account as the server said it last (GET /account), for the account's view.
    struct ChatAccount
    {
        bool known = false;                   // the server has answered since the sign-in
        double balanceUsd = 0.0;              // the credit left
        int64_t requests = 0;                 // this month's (UTC) requests through the server, their cost and tokens
        double costUsd = 0.0;
        int64_t inputTokens = 0, outputTokens = 0;
        std::string client;                   // what signed in ("TestingPlayground 0.1.0 (instrument)")
        std::string since, expires;           // the sign-in's start and end, ISO 8601 (UTC)
        bool refreshing = false;              // a refresh is waiting for the server
        std::string error;                    // why the last refresh failed ("" when it did not)
    };

    struct ChatState
    {
        bool hasKey = false;
        std::string keyProblem;               // why the stored key cannot be used ("" when it can)
        bool signedIn = false;                // a sign-in to the server is stored
        std::string account;                  // its username (the last one signed in, after signing out)
        std::string accountProblem;           // why the sign-in no longer works ("" when it does)
        std::string credit;                   // "$4.96 credit left", once the server has said
        ChatAccount details;                  // the account's view (signed in only)
        bool useAccount = true;               // the chat goes through the account (else the key)
        bool signingIn = false;               // a sign-in is waiting for the server
        std::string signInError;              // why the last sign-in failed ("" when it did not)
        bool busy = false;                    // Claude is working on a turn
        std::string activity;                 // what it is doing: "THINKING", "WRITING filters.fm", ...
        std::vector<ChatEntry> entries;
        std::vector<ChatModel> models;
        std::string model;                    // the chosen model's id
        std::string effort;                   // the chosen thinking level
        bool fast = false;                    // fast mode chosen (it applies on a model that takes it)
        std::string usage;                    // the last reply's tokens, for the footer

        // The chat has what it needs to send: the account it uses is signed in, or the key it uses is stored.
        bool ready() const noexcept { return useAccount ? signedIn : hasKey; }
    };

    class ChatPort
    {
    public:
        virtual ~ChatPort() = default;

        virtual uint64_t chatRevision() const = 0;
        virtual ChatState chatState() const = 0;

        virtual bool saveKey(const std::string& key, std::string& why) = 0;
        virtual void forgetKey() = 0;
        virtual void signIn(const std::string& username, const std::string& password) = 0;
        virtual void signOut() = 0;
        virtual void useAccount(bool account) = 0;
        virtual void refreshAccount() = 0;
        virtual void setModel(const std::string& id) = 0;
        virtual void setEffort(const std::string& effort) = 0;
        virtual void setFast(bool on) = 0;

        virtual void send(const std::string& text) = 0;
        virtual void stop() = 0;
        virtual void newChat() = 0;
    };
}
