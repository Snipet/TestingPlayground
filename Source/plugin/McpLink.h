// Source/plugin/McpLink.h: one instance of the plugin, reachable by its MCP server (mcp/Main.cpp, which Claude Desktop
// starts): a socket of its own in the folder mcp/McpProtocol.h names, its number the lowest free ("Synth 2" to the
// user: the plugin shows it while Claude Desktop talks to it), answering info, tools, guide, call and cancel (the
// protocol there). It also leaves tools.json and guide.md in the folder, for a server started while no instance runs.
//
// Two threads. The poll thread accepts connections, reads their lines and answers at once what never waits: info,
// tools, guide (a part of it), cancel, and the live calls (ChatAgent::mcpLive: reads of what plays now, which keep
// CLAUDE.md's live-tool contract). Every other call goes to the worker, one at a time in the order they came (the
// chat's busy state allows one), and runs the chat's tool there (ChatAgent::mcpCall), as the chat's worker would. So a
// quick read answers while a long call (tune_dials) runs. Replies are whole lines, written under one mutex, so the
// two threads' replies interleave only line by line. cancel {id} stops that request's call: one waiting is dropped (its
// reply says so), the one running is cancelled through the hook (a tune then leaves the dials as they were). A
// connection that closes takes its calls with it: those waiting are dropped, the running one is cancelled.
//
// Made by the Processor after its chat; gone first: its poll thread stopped, the call running cancelled and waited for,
// its socket removed. Nothing it does touches the audio thread.
#pragma once

#include "plugin/ChatAgent.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace playground
{
    class McpLink
    {
    public:
        struct Hooks
        {
            std::function<juce::var()> info;             // the instance, for list_instances (its id added here)
            // A tool call (ChatAgent::mcpCall), numbered for cancel; and the cancel of a call by its number (any thread).
            std::function<ChatAgent::McpResult(const std::string&, const juce::var&, uint64_t)> call;
            std::function<void(uint64_t)> cancel;
        };

        explicit McpLink(Hooks hooks);
        ~McpLink();
        McpLink(const McpLink&) = delete;
        McpLink& operator=(const McpLink&) = delete;

        const std::string& id() const noexcept { return id_; }           // "2"
        const std::string& name() const noexcept { return name_; }       // "Synth 2" ("" when not listening)
        const std::string& socketPath() const noexcept { return path_; }
        bool listening() const noexcept { return listen_ >= 0; }
        // A client has spoken to it and is still there: Claude Desktop knows this instance (any thread).
        bool connected() const noexcept { return talking_.load() > 0; }

    private:
        struct Client;                                   // a connection: its socket (closed with the last owner), its lines
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
        void request(const std::shared_ptr<Client>& client, const std::string& line);
        void cancel(const std::shared_ptr<Client>& client, const juce::var& id, bool& found);
        void closed(const std::shared_ptr<Client>& client);   // its calls dropped or cancelled
        bool answer(Client& client, const juce::var& reply);   // a whole line, under writeMutex_
        static juce::var resultOf(const ChatAgent::McpResult& r);

        Hooks hooks_;
        std::string id_, name_, path_;
        std::atomic<int> talking_{ 0 };
        int listen_ = -1;
        int wake_[2] = { -1, -1 };
        std::atomic<bool> stop_{ false };
        std::mutex writeMutex_;                          // a reply's line, whole
        std::mutex jobMutex_;
        std::condition_variable jobCv_;
        std::deque<Job> jobs_;                           // waiting, in order
        std::shared_ptr<Client> runningClient_;          // the call running: its connection, request id and number
        juce::var runningId_;
        uint64_t running_ = 0;
        uint64_t nextCall_ = 1;
        bool workerStop_ = false;
        std::thread thread_, worker_;
    };
}
