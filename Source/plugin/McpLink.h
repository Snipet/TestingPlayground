// Source/plugin/McpLink.h: one instance of the plugin, reachable by its MCP server (mcp/Main.cpp, which Claude Desktop
// starts): a socket of its own in the folder mcp/McpProtocol.h names, its number the lowest free ("Synth 2" to the
// user, until the user names it: setName), answering info, tools, guide, call and cancel (the protocol there). It also leaves tools.json and guide.md in the folder, for a server started while no instance runs.
//
// Two threads. The poll thread accepts connections, reads their lines and answers at once what never waits: info,
// tools, guide (a part of it), cancel, and the live calls (ChatAgent::mcpLive: reads of what plays now, which keep
// CLAUDE.md's live-tool contract). Every other call goes to the worker, one at a time in the order they came (the
// chat's busy state allows one), and runs the chat's tool there (ChatAgent::mcpCall), as the chat's worker would. So a
// quick read answers while a long call (tune_dials) runs. Replies are whole lines, put in the connection's outbox under
// one mutex and sent at once as far as the socket takes them, so the two threads' replies interleave only line by line.
// The sockets are non-blocking: what one cannot take yet the poll thread sends when it has room, reading meanwhile, so
// neither thread ever waits for the server to read (a large reply and a large request crossing each other cannot stick
// both ends, and the link always stops at once). cancel {id} stops that request's call: one waiting is dropped (its
// reply says so), the one running is cancelled through the hook (a tune then leaves the dials as they were). A
// connection that closes takes its calls with it: those waiting are dropped, the running one is cancelled.
//
// A live call that takes seconds (ChatAgent::mcpLiveWaits: listen) runs on a thread of its own, numbered for cancel
// as the worker's calls are, and replies when it is done, so the poll thread goes on answering meanwhile (cancel, the
// quick reads, another listen): kListens at once at most (one more is refused at once, in its reply). A cancel or its
// connection closing stops it through the hook; the poll thread joins it once it is done, and the link going cancels
// and waits for every one.
//
// Claude Desktop's target (mcp/McpTarget.h): the poll thread also writes this instance's card into the folder when it
// changes (its name, track and project) and reads the others' and target.json about once a second (at once after
// choose() or setName()), which view() copies for the plugin to show, with what Claude Desktop's calls do here (the last
// call's tool, whether one runs, since when). choose() writes or clears target.json (the user's Use with Claude Desktop).
//
// Made by the Processor after its chat; gone first: its poll thread stopped, the call running cancelled and waited for,
// its socket, its card and a target.json naming it removed. Nothing it does touches the audio thread.
#pragma once

#include "plugin/ChatAgent.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace playground
{
    class McpLink
    {
    public:
        struct Hooks
        {
            std::function<juce::var()> info;             // the instance, for list_instances (its id and name added here)
            // Its track's and its project's names, for its card (the poll thread, about once a second).
            std::function<std::pair<std::string, std::string>()> place;
            // A tool call (ChatAgent::mcpCall), numbered for cancel; and the cancel of a call by its number (any thread).
            std::function<ChatAgent::McpResult(const std::string&, const juce::var&, uint64_t)> call;
            std::function<void(uint64_t)> cancel;
        };

        explicit McpLink(Hooks hooks);
        ~McpLink();
        McpLink(const McpLink&) = delete;
        McpLink& operator=(const McpLink&) = delete;

        const std::string& id() const noexcept { return id_; }           // "2"
        // Its name to Claude Desktop and the user: the user's ("Bass"), else its number's ("Synth 2"); "" when not
        // listening. Any thread.
        std::string name() const;
        std::string numberName() const;                  // "Synth 2" ("" when not listening)
        // The user's name for it ("" or its number's name: none); false, with why, for one it cannot take
        // (mcp::target::nameRefusal). Its card says it within a poll.
        bool setName(const std::string& name, std::string& why);
        bool named() const;                              // the user gave it a name
        // The user's name for it ("" none), listening or not: what the session keeps (name() is "" without a socket).
        std::string userName() const;
        const std::string& socketPath() const noexcept { return path_; }
        bool listening() const noexcept { return listen_ >= 0; }
        // A client has spoken to it and is still there: Claude Desktop knows this instance (any thread).
        bool connected() const noexcept { return talking_.load() > 0; }

        // The other instances open and which one the user chose as Claude Desktop's target, as the poll thread last
        // read them; and Claude Desktop's calls here.
        struct Peer
        {
            std::string id, name, track, project;
            bool operator==(const Peer&) const = default;
        };
        struct View
        {
            std::vector<Peer> others;                    // the other instances open, by number
            std::string chosen;                          // the instance the user chose: this one's id, another's, "" none
            std::string tool;                            // the newest call acting on it, else the last that ended ("": none
                                                         // since it opened)
            bool running = false;                        // a call acts on this instance now
            double ago = 0.0;                            // seconds since that call began (running) or ended
            uint64_t revision = 0;                       // bumps when the others or the choice change
        };
        View view() const;
        // The user's choice (any instance's Use with Claude Desktop): `id` (this one's, or another open's) made the
        // target, or, `on` false, the choice cleared when it is that one; view() says so at once only when target.json
        // was written (or cleared). The message thread.
        void choose(const std::string& id, bool on);
        void rescan();                                   // read the folder again soon (probes)

    private:
        struct Client;                                   // a connection: its socket (closed with the last owner), its lines,
                                                         // its outbox
        struct Job                                       // a call for the worker
        {
            std::shared_ptr<Client> client;
            juce::var id;                                // the request's
            uint64_t call = 0;                           // its number (Hooks::call, Hooks::cancel)
            std::string name;
            juce::var arguments;
        };

        void run();                                      // the poll thread
        void work();                                     // the worker
        // A listen (mcpLiveWaits) on a thread of its own; false when kListens run already (jobMutex_ not held).
        bool listen(const std::shared_ptr<Client>& client, const juce::var& id, const std::string& name, const juce::var& arguments);
        void reap();                                     // the listens done, joined (jobMutex_ held)
        void request(const std::shared_ptr<Client>& client, const std::string& line);
        void cancel(const std::shared_ptr<Client>& client, const juce::var& id, bool& found);
        void closed(const std::shared_ptr<Client>& client);   // its calls dropped or cancelled
        void answer(Client& client, const juce::var& reply);   // a whole line, into its outbox (outMutex_), sent now
                                                                // as far as it goes; the poll thread woken for the rest
        void wake();                                     // the poll thread, out of poll() (stop_, or an outbox to send)
        static juce::var resultOf(const ChatAgent::McpResult& r);
        void scan();                                     // the poll thread: its card written, the others' read
        void began(const std::string& tool);             // a call acts on it (any thread)
        void ended(const std::string& tool);

        Hooks hooks_;
        std::string id_, path_, dir_;
        std::string token_;                              // its card's: its own, random (mcp/McpTarget.h)
        mutable std::mutex nameMutex_;
        std::string name_;                               // the user's ("" none)
        mutable std::mutex viewMutex_;
        View view_;
        std::vector<std::pair<std::string, std::string>> tokens_;   // each other's id and card token (viewMutex_)
        std::string written_;                            // its card as last written (the poll thread)
        std::atomic<bool> scanNow_{ true };
        std::chrono::steady_clock::time_point scanned_{};
        // The calls acting on it now, each tool with when it began, the newest last; and the last that ended (viewMutex_).
        std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> acting_;
        std::string lastTool_;
        std::chrono::steady_clock::time_point lastAt_{};
        std::atomic<int> talking_{ 0 };
        int listen_ = -1;
        int wake_[2] = { -1, -1 };
        std::atomic<bool> stop_{ false };
        std::mutex outMutex_;                            // the connections' outboxes
        std::mutex jobMutex_;
        std::condition_variable jobCv_;
        std::deque<Job> jobs_;                           // waiting, in order
        std::shared_ptr<Client> runningClient_;          // the call running: its connection, request id and number
        juce::var runningId_;
        uint64_t running_ = 0;
        uint64_t nextCall_ = 1;
        bool workerStop_ = false;
        struct Listening                                 // a listen's thread (jobMutex_)
        {
            std::shared_ptr<Client> client;
            juce::var id;
            uint64_t call = 0;
            std::shared_ptr<std::atomic<bool>> done;
            std::thread thread;
        };
        static constexpr std::size_t kListens = 4;
        std::list<Listening> listens_;
        std::thread thread_, worker_;
    };
}
