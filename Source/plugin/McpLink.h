// Source/plugin/McpLink.h: one instance of the plugin, reachable by its MCP server (mcp/Main.cpp, which Claude Desktop
// starts): a socket of its own in the folder mcp/McpProtocol.h names, listened to on a thread of its own, answering
// info, tools, guide and call (the protocol there). A call runs the chat's tool (ChatAgent::mcpCall) on that thread, as
// the chat's worker would. It also leaves tools.json and guide.md in the folder, for a server started while no
// instance runs.
//
// Made by the Processor after its chat; gone first (its thread stopped, its socket removed). Nothing it does touches
// the audio thread.
#pragma once

#include "plugin/ChatAgent.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace testingplaygroundsynth
{
    class McpLink
    {
    public:
        struct Hooks
        {
            std::function<juce::var()> info;             // the instance, for list_instances (its id added here)
            std::function<ChatAgent::McpResult(const std::string&, const juce::var&)> call;
        };

        explicit McpLink(Hooks hooks);
        ~McpLink();
        McpLink(const McpLink&) = delete;
        McpLink& operator=(const McpLink&) = delete;

        const std::string& id() const noexcept { return id_; }           // "<pid>-<n>"
        const std::string& socketPath() const noexcept { return path_; }
        bool listening() const noexcept { return listen_ >= 0; }

    private:
        void run();
        std::string answer(const std::string& line);

        Hooks hooks_;
        std::string id_, path_;
        int listen_ = -1;
        int wake_[2] = { -1, -1 };
        std::atomic<bool> stop_{ false };
        std::thread thread_;
    };
}
