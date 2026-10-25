// Source/plugin/ChatAgent.h: the chat with Claude, owned by the processor (so a turn goes on while the editor is
// closed) and shown by the editor through ChatPort.
//
// A turn: the user's message (with the plugin's files, the last compile and the dials, when they changed since Claude
// last saw them) goes to the Messages API, streamed; while Claude asks for tools, the agent runs them and sends the
// results back, up to kMaxRounds requests a turn. Claude's tools act on the plugin through ChatHost: read, write,
// edit, rename and delete the files (the editor's tabs follow, each change undoable there), run the program (RUN: the
// compile's outcome comes back), set a dial, render and measure what the program puts out (measure_output,
// dsp/Measure.h: Claude cannot hear), and read the plugin's status.
//
// Requests (ClaudeApi.h; the claude-api reference): stream on; adaptive thinking with summarized display and the chosen
// effort (the models list comes from /v1/models, filtered to those with adaptive thinking); max_tokens the model's own
// limit (from /v1/models, at most 128K), so a large file written in one call is not cut off; the system prompt
// (ChatPrompt.cpp) frozen and cached for an hour, the conversation cached by the top-level automatic breakpoint; the
// tools fixed for the session, each with eager_input_streaming, their input parsed strictly here; "fallbacks":
// "default" on the models that take it, so a classifier's refusal is retried on another model. The history is
// append-only (preserved thinking): every response's content goes back unchanged, but after a fallback block the
// blocks before it lose their thinking and tool calls, as the API asks. A refusal, the stop button and an error each
// end the turn with the history still valid.
//
// Large work. A long conversation is compacted server-side (beta compact-2026-01-12, on the models that take it): once
// a request's input passes kCompactTrigger tokens the API summarizes what came before (with instructions that keep
// the sound's design and leave out code) and answers from the summary; the compaction block goes into the history and
// what came before it is dropped. The plugin's state sends a file's text only when Claude has not seen that text (in
// an earlier block, through read_file, or as it wrote it); after a compaction it sends every file again. A request
// that fails on the connection, a rate limit, an overload or a server error, or whose stream breaks off before its
// end, is sent again (kMaxAttempts, retry-after honoured; what the broken stream showed is taken back). While a file
// streams in, the activity says which and how much has arrived. A tool's long output (a compile's messages) is cut.
//
// Threads: the ChatPort calls run on the message thread; a turn runs on the agent's worker thread, which calls
// ChatHost (whose implementation must be safe from that thread) and the transport.
#pragma once

#include "plugin/ChatPort.h"
#include "plugin/ClaudeApi.h"
#include "plugin/EditorPort.h"

#include "dsp/Compiler.h"
#include "dsp/Controls.h"
#include "dsp/Sources.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace testingplayground
{
    // What Claude's tools act on. Every call may come from the agent's worker thread.
    class ChatHost
    {
    public:
        virtual ~ChatHost() = default;

        virtual Draft chatDraft() const = 0;                        // the editor's files and tab
        virtual void chatSetDraft(const Draft& draft) = 0;          // the files Claude wrote; the editor follows
        virtual CompileStatus chatRun(const Sources& files) = 0;    // RUN, then the compile's outcome
        virtual CompileStatus chatLastStatus() const = 0;
        virtual Sources chatRunning() const = 0;
        virtual Controls chatDials() const = 0;                     // the running program's
        virtual float chatDialValue(int slot) const = 0;            // plain value
        virtual void chatSetDial(int slot, float plain) = 0;        // a host gesture, applied soon on the message thread
        virtual float chatLoad() const = 0;
        virtual uint32_t chatAlerts() const = 0;
    };

    class ChatAgent final : public ChatPort
    {
    public:
        static constexpr int kMaxRounds = 80;                      // requests in one turn

        ChatAgent(ChatHost& host, std::unique_ptr<claude::Transport> transport);
        ~ChatAgent() override;                                      // stops a turn and waits for it
        ChatAgent(const ChatAgent&) = delete;
        ChatAgent& operator=(const ChatAgent&) = delete;

        // ChatPort
        uint64_t chatRevision() const override;
        ChatState chatState() const override;
        bool saveKey(const std::string& key, std::string& why) override;
        void forgetKey() override;
        void setModel(const std::string& id) override;
        void setEffort(const std::string& effort) override;
        void send(const std::string& text) override;
        void stop() override;
        void newChat() override;

        // The conversation in the session (the API's messages and the view's entries, as JSON), and back. A turn that
        // is running is not saved half-done: its user message and what came back so far are.
        std::string saveState() const;
        void loadState(const std::string& json);

        // For probes: the transport (the next turn uses it), waiting for the current turn, the request bodies sent.
        void setTransport(std::unique_ptr<claude::Transport> transport);
        void waitIdle();
        std::vector<std::string> requestsSent() const;

        // The system prompt and the tools as sent (for probes and the docs).
        static const std::string& systemPrompt();
        // The program in several files the system prompt shows, for this plugin's kind (the program first).
        static Sources promptExample();
        static juce::var toolDefinitions();

    private:
        void runTurn();                                             // the worker
        bool streamOnce(const std::string& body, const std::string& beta, claude::MessageBuilder& builder,
                        int& status, std::string& error, int& retryAfter);
        juce::var buildRequest(std::string& beta) const;
        std::string contextBlock();                                 // the plugin's state, when it changed
        juce::var runTool(const juce::var& toolUse);               // a tool_result block
        std::string toolText(const std::string& name, const juce::var& input, bool& isError);
        void fetchModels();                                         // the worker, before a turn, once a key is known
        void ensureKey();
        void touch();                                               // revision + 1 (mutex held)
        void appendAnswer(const juce::Array<juce::var>& content);   // an assistant message into the history (mutex held)
        void markSeen(const std::string& name, const std::string& text);   // Claude knows this text (mutex held)
        void addEntry(ChatEntry::Kind kind, std::string text);     // mutex held
        void appendToLast(ChatEntry::Kind kind, std::string_view text);   // mutex held
        void setActivity(std::string activity);
        void startWorker();
        void joinWorker();

        ChatHost& host_;
        std::unique_ptr<claude::Transport> transport_;

        mutable std::mutex mutex_;
        uint64_t revision_ = 1;
        std::vector<ChatEntry> entries_;
        juce::Array<juce::var> messages_;                           // the API's history
        std::vector<ChatModel> models_;
        bool modelsFetched_ = false;
        std::string model_, effort_;
        std::string turnModel_, turnEffort_;                        // the turn under way's (the worker's)
        std::string activity_;
        std::string usage_;
        std::string key_;                                           // loaded on first use
        std::string keyProblem_;
        std::map<std::string, uint64_t> seen_;                      // file name -> the text Claude saw (hashText)
        uint64_t lastSignature_ = 0;                                // the plugin's state as Claude last got it
        std::vector<std::string> requests_;                         // bodies sent (probes)
        bool busy_ = false;

        std::atomic<bool> cancel_{ false };
        std::thread worker_;
    };
}
