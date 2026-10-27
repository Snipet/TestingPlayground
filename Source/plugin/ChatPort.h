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
//                    on the models that take it; setWeb() web search (Claude searches the web and reads pages and
//                    PDFs, the API's server tools), with the user's own key on the models that take it. All four
//                    machine-wide
//   attach()         pictures (files) for the next message: read in the background (ChatImages.h), then in
//                    chatState().attached; one that cannot be read gets a note. detach() takes one back
//   send()           a message from the user (and the pictures attached, which it takes; a message of pictures alone
//                    goes too); Claude answers (and works) in the background. While Claude's question
//                    waits (asking), the message answers it in the user's own words
//   answer()         the user's answer to Claude's question (ask_user), one per question: the chosen options' labels;
//                    Claude's turn goes on with it
//   stop()           ends Claude's turn as soon as it can; the work done so far stays
//   newChat()        forgets the conversation
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace testingplayground
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
        };
        Kind kind = Kind::note;
        std::string text;                     // a question's: the questions and their options, as plain text
        std::vector<ChatQuestion> questions;  // a question's
        std::vector<std::string> answers;     // a question's answer, one per question: the labels chosen (", "
                                              // between several), "" for none
        bool open = false;                    // a question still waiting for its answer
        bool typed = false;                   // a question answered by a message instead
        std::vector<ChatPicturePtr> pictures; // a user message's
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
        bool asking = false;                  // Claude's question (the last question entry) waits for the answer
        std::vector<ChatPicturePtr> attached; // pictures for the next message
        int attaching = 0;                    // files still being read
        std::string activity;                 // what it is doing: "THINKING", "WRITING filters.fm", ...
        std::vector<ChatEntry> entries;
        std::vector<ChatModel> models;
        std::string model;                    // the chosen model's id
        std::string effort;                   // the chosen thinking level
        bool fast = false;                    // fast mode chosen (it applies on a model that takes it)
        bool web = false;                     // web search chosen
        std::string webBlocked;               // why it cannot apply now ("" when it can): the account in use
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
        virtual void setWeb(bool on) = 0;

        virtual void attach(const std::vector<std::string>& paths) = 0;
        virtual void detach(uint32_t id) = 0;
        virtual void send(const std::string& text) = 0;
        virtual void answer(const std::vector<std::string>& answers) = 0;
        virtual void stop() = 0;
        virtual void newChat() = 0;
    };
}
