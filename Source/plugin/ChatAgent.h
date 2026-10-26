// Source/plugin/ChatAgent.h: the chat with Claude, owned by the processor (so a turn goes on while the editor is
// closed) and shown by the editor through ChatPort.
//
// A turn: the user's message (with the plugin's files, the last compile and the dials, when they changed since Claude
// last saw them) goes to the Messages API, streamed; while Claude asks for tools, the agent runs them and sends the
// results back, up to kMaxRounds requests a turn. Claude's tools act on the plugin through ChatHost: read, write,
// edit, rename and delete the files (the editor's tabs follow, each change undoable there), run the program (RUN: the
// compile's outcome comes back), set a dial, render and measure what the program puts out (measure_output,
// dsp/Measure.h: Claude cannot hear), look at the panel (view_panel: a PNG the message thread draws headless,
// ChatHost::chatPicture, sent as an image in the tool's result and left out of the saved session), and read the
// plugin's status.
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
// Fast mode (the fast-mode docs), when chosen, on the models that take it (/v1/models does not say which: the docs
// list Claude Opus 5.5, and Claude Opus 5 and Opus 4.8 in preview, behind the fast-mode-2026-02-01 beta): the request
// asks for "speed": "fast", up to 2.5 times the output's speed at twice the price, and goes without "fallbacks" (the
// docs say nothing of the two together). The footer says "fast" when the response's usage does. When fast capacity
// stays busy (a 429 or 529 again, after waiting once) the turn goes on at standard speed; when the API refuses fast
// mode itself (not open to the key, say), the turn goes on at standard speed and fast mode is turned off. A change of
// speed reads the conversation without the cache once: requests at different speeds share no cached prefix.
//
// Large work. A long conversation is compacted server-side (beta compact-2026-01-12, on the models that take it): once
// a request's input passes kCompactTrigger tokens the API summarizes what came before (with instructions that keep
// the sound's design and leave out code) and answers from the summary; the compaction block goes into the history, and
// what came before it is dropped: the history then starts at the block, in its assistant message, as the API's own
// examples continue after one. (Not at the user message before it: in the middle of a turn that is the results of
// tool calls the summary took away, which the API refuses.) A history from a session is repaired the same way when it
// loads, and before each message (repairHistory). The plugin's state sends a file's text only when Claude has not seen
// that text (in an earlier block, through read_file, or as it wrote it); after a compaction it sends every file
// again, with the next message or, when the summary came in the middle of a turn, with the tool results that follow
// it.
//
// A reply holds max_tokens, thinking included, and at high effort a hard design can think that long. The system prompt
// says so, and at xhigh and max effort a note with the user's message says it again (the API's advice: at those
// levels a long deliverable may be drafted in the thinking first). A reply cut off there goes back to the history as
// it came, and the turn goes on with a line that says so (after a cut-off tool call, its error result); a second reply
// cut off in a row ends the turn, with a note, since a reply that only thinks costs a whole reply each time. A session
// saved with another system prompt or other tools (an earlier version) loses its thinking blocks when it loads: the
// API binds them to the conversation they were made in (preserved thinking) and would refuse them.
//
// A request
// that fails on the connection, a rate limit, an overload or a server error, or whose stream breaks off before its
// end, is sent again (kMaxAttempts, retry-after honoured; what the broken stream showed is taken back). While a file
// streams in, the activity says which and how much has arrived. A tool's long output (a compile's messages) is cut.
//
// Access. The requests go to Anthropic with the user's own key (x-api-key), or to the TestingPlayground server with the
// token it gave when the user signed in ("Authorization: Bearer"); the server passes them on unchanged, on its own key,
// and charges each to the account's credit. Both can be stored (ChatStore.h); the chat uses the one set up last. The
// server's /v1/models lists only the models it can charge for. Its 401 always means the sign-in is no longer good: the
// token is forgotten and the chat asks the user to sign in again. Its 402 (the credit is used up), 403 (the account is
// disabled), 400 (a model it has no price for) and 502 (Anthropic refused the server's key) carry messages written for
// the user, shown as they are, and are not retried. After a turn through the account the agent asks the server for
// the credit left (GET /account), which the footer shows; the account's view asks for it again (refreshAccount), with
// this month's use and the sign-in's dates.
//
// Threads: the ChatPort calls run on the message thread; a turn runs on the agent's worker thread, which calls
// ChatHost (whose implementation must be safe from that thread) and the transport. Signing in and out runs on a thread
// of its own, so the message thread never waits for the network.
#pragma once

#include "plugin/ChatPort.h"
#include "plugin/ClaudeApi.h"
#include "plugin/EditorPort.h"

#include "dsp/Compiler.h"
#include "dsp/Controls.h"
#include "dsp/Sources.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
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
        // A picture of the panel (view_panel), made on the message thread; this thread waits for it (10 s at most).
        virtual PanelPicture chatPicture(bool window, const std::string& page) = 0;
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
        void signIn(const std::string& username, const std::string& password) override;
        void signOut() override;
        void useAccount(bool account) override;
        void refreshAccount() override;
        void setModel(const std::string& id) override;
        void setEffort(const std::string& effort) override;
        void setFast(bool on) override;
        void send(const std::string& text) override;
        void stop() override;
        void newChat() override;

        // The conversation in the session (the API's messages and the view's entries, as JSON), and back. A turn that
        // is running is not saved half-done: its user message and what came back so far are.
        std::string saveState() const;
        void loadState(const std::string& json);

        // For probes: the transport (the next turn uses it), waiting for the current turn (and for a sign-in or
        // sign-out under way), the Messages API request bodies sent.
        void setTransport(std::unique_ptr<claude::Transport> transport);
        void waitIdle();
        std::vector<std::string> requestsSent() const;

        // The system prompt and the tools as sent (for probes and the docs).
        static const std::string& systemPrompt();
        // The program in several files the system prompt shows, for this plugin's kind (the program first).
        static Sources promptExample();
        static juce::var toolDefinitions();

    private:
        // Where a request goes and what it carries: the server and the token, or Anthropic and the key.
        struct Access
        {
            bool account = false;
            std::string host, secret;
        };
        Access access() const;                                      // the one in use (mutex held)
        static claude::Headers headersFor(const Access& access);    // its credential and the API version

        void runTurn();                                             // the worker
        bool streamOnce(const std::string& body, const std::string& beta, claude::MessageBuilder& builder,
                        int& status, std::string& error, int& retryAfter, bool& viaServer);
        juce::var buildRequest(std::string& beta) const;
        std::string contextBlock();                                 // the plugin's state, when it changed
        juce::var runTool(const juce::var& toolUse);               // a tool_result block
        std::string toolText(const std::string& name, const juce::var& input, bool& isError);
        void fetchModels();                                         // the worker, before a turn, once a key is known
        // GET /account: the credit left and the account's details. The worker's after a turn through the account
        // (`asked` false); the auth thread's when the account's view asks (true: whichever access the chat uses).
        void fetchAccount(const std::atomic<bool>& cancel, bool asked);
        void ensureAccess();                                        // the key or the token in use, loaded
        void loadAccess();                                          // the same (mutex held)
        void refused(const Access& access, int status, const std::string& why);   // a 401: that sign-in or key is
                                                                    // no longer good (mutex held)
        void runSignIn(const std::string& username, std::string password);   // the sign-in thread
        void queueAuth(std::function<void()> job);                  // a sign-in or sign-out, on the auth thread
        void authLoop();                                            // the auth thread
        void waitAuth();                                            // until no job is queued or running
        void touch();                                               // revision + 1 (mutex held)
        void appendAnswer(const juce::Array<juce::var>& content);   // an assistant message into the history (mutex held)
        void repairHistory();                                       // one the API takes (mutex held)
        void dropThinking();                                        // the history without thinking (mutex held)
        int maxTokensFor(const std::string& id) const;              // a request's max_tokens (mutex held)
        static std::string promptPrint();                           // the system prompt's and tools' hash
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
        bool fast_ = false;                                         // fast mode chosen
        bool turnFast_ = false;                                     // the turn under way asks for it
        std::string activity_;
        std::string usage_;
        std::string key_;                                           // loaded on first use
        std::string keyProblem_;
        std::string token_;                                         // the server's sign-in, loaded on first use
        std::string account_;                                       // its username (kept after signing out)
        std::string accountProblem_;                                // why the sign-in no longer works
        std::string credit_;                                        // "$4.96 credit left", as the server last said
        ChatAccount details_;                                       // the rest of what it said (GET /account)
        bool useAccount_ = true;                                    // the chat goes through the account, else the key
        bool signingIn_ = false;
        std::string signInError_;
        std::map<std::string, uint64_t> seen_;                      // file name -> the text Claude saw (hashText)
        uint64_t lastSignature_ = 0;                                // the plugin's state as Claude last got it
        bool resendState_ = false;                                  // a summary came: the state goes with what follows
        std::vector<std::string> requests_;                         // bodies sent (probes)
        bool busy_ = false;

        std::atomic<bool> cancel_{ false };
        std::thread worker_;
        // Signing in and out: jobs run in order on one thread, started on first use, so the message thread never
        // waits for the network.
        std::atomic<bool> authCancel_{ false };                    // the destructor ends a request under way
        std::mutex authMutex_;
        std::condition_variable authCv_;
        std::deque<std::function<void()>> authJobs_;
        bool authRunning_ = false;                                  // a job is running
        bool authStop_ = false;
        std::thread auth_;
    };
}
