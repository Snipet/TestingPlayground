// Source/plugin/ChatPort.h: what the editor's chat reaches (JUCE-free; message thread). The processor's ChatAgent
// implements it (ChatAgent.h): a conversation with Claude, which can read and write the program's files, run them,
// set the dials and answer questions, through the Claude API with the user's own key.
//
//   chatRevision()   bumps whenever anything below changes (a streamed word included)
//   chatState()      the conversation as the view shows it, the models and the choice, the key's state
//   saveKey()        stores the key, encrypted (ChatStore.h); false, with why, for one that is not a key
//   forgetKey()      deletes the stored key
//   setModel()       a model of chatState().models; setEffort() a thinking level that model has. Both machine-wide
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
    };

    struct ChatState
    {
        bool hasKey = false;
        std::string keyProblem;               // why the stored key cannot be used ("" when it can)
        bool busy = false;                    // Claude is working on a turn
        std::string activity;                 // what it is doing: "THINKING", "WRITING filters.fm", ...
        std::vector<ChatEntry> entries;
        std::vector<ChatModel> models;
        std::string model;                    // the chosen model's id
        std::string effort;                   // the chosen thinking level
        std::string usage;                    // the last reply's tokens, for the footer
    };

    class ChatPort
    {
    public:
        virtual ~ChatPort() = default;

        virtual uint64_t chatRevision() const = 0;
        virtual ChatState chatState() const = 0;

        virtual bool saveKey(const std::string& key, std::string& why) = 0;
        virtual void forgetKey() = 0;
        virtual void setModel(const std::string& id) = 0;
        virtual void setEffort(const std::string& effort) = 0;

        virtual void send(const std::string& text) = 0;
        virtual void stop() = 0;
        virtual void newChat() = 0;
    };
}
