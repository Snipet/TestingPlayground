// Source/sandbox/Main.cpp: <Name>GuiSandbox, the GUI sandbox's helper: a headless process the MCP server
// (mcp/Main.cpp, its gui_sandbox tool) starts on demand, so that Claude can design a plugin's interface, a .gui file,
// and see it drawn exactly as the plugin's PANEL view draws it, with no plugin open and nothing of an open one touched.
// It holds the editor's .gui code (editor/GuiView.h and what it is made of), the JUCE-free .gui and program reading
// (fminus/GuiSpec.h, PanelSpec.h, Lang.h, Shaders.h) and FunkGui (its canvas, text, panel and GPU), and nothing of the
// plugin: no FMinus compiler, no LLVM, no audio, no Processor or ChatAgent (CMakeLists.txt links it so;
// lint.layers checks its includes).
//
// Its main thread is JUCE's message thread (funkgui::HeadlessGuiScope makes the MessageManager here), as
// renderGpuPicture needs: called on it, it draws at once. The protocol is SandboxProtocol.h: a JSON request a line on
// stdin, its reply a line on stdout, one at a time. stdout carries the replies alone: anything else that would print
// there (a library's log) goes to stderr, where the server's own goes (Claude Desktop's MCP log). It ends when its stdin
// closes (the server gone, or done with it after kIdleSeconds with no request), and by itself after kHelperIdleSeconds;
// the server starts it again on the next call.
#include "sandbox/Sandbox.h"
#include "sandbox/SandboxProtocol.h"

#include "mcp/McpProtocol.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <juce_core/juce_core.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <thread>

#include <unistd.h>

namespace pg = playground;

namespace
{
    // `line` and a line feed, all of it, on the replies' descriptor.
    void reply(int fd, const juce::var& msg)
    {
        const std::string out = juce::JSON::toString(msg, true).toStdString() + "\n";
        std::size_t sent = 0;
        while (sent < out.size())
        {
            const ssize_t n = write(fd, out.data() + sent, out.size() - sent);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return;
            sent += static_cast<std::size_t>(n);
        }
    }

    // Tests only (the probe's hung helper): <ENV_PREFIX>SANDBOX_TEST_HANG names a request this helper never answers.
    bool hangsOn(const std::string& op)
    {
        const std::string var = std::string(pg::product::kEnvPrefix) + "SANDBOX_TEST_HANG";
        const char* v = std::getenv(var.c_str());
        return v != nullptr && op == v;
    }
}

int main()
{
    // The replies' descriptor is stdout's own; stdout itself then points at stderr, for whatever else would print.
    const int replies = dup(1);
    dup2(2, 1);
    std::signal(SIGPIPE, SIG_IGN);
    const funkgui::HeadlessGuiScope gui;          // JUCE's GUI side; this thread is its message thread
    pg::sandbox::Sandbox sandbox(pg::product::kIsInstrument);
    pg::mcp::LineReader in(0);
    for (;;)
    {
        std::string line;
        const int got = in.next(line, pg::sandbox::kHelperIdleSeconds * 1000);
        if (got <= 0)
            break;                                // stdin closed (the server gone or done with it), or idle long past it
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        const juce::var request = juce::JSON::parse(juce::String::fromUTF8(line.c_str()));
        const juce::var id = request["id"];
        const std::string op = request["op"].toString().toStdString();
        if (op == "quit")
        {
            juce::var done(new juce::DynamicObject());
            done.getDynamicObject()->setProperty("id", id);
            done.getDynamicObject()->setProperty("ok", true);
            reply(replies, done);
            break;
        }
        if (hangsOn(op))
            for (;;)
                std::this_thread::sleep_for(std::chrono::seconds(60));
        juce::var out;
        try
        {
            out = sandbox.handle(request);
        }
        catch (const std::exception& e)
        {
            out = juce::var(new juce::DynamicObject());
            out.getDynamicObject()->setProperty("ok", false);
            out.getDynamicObject()->setProperty("text", juce::String("The sandbox failed: ") + e.what());
        }
        if (juce::DynamicObject* o = out.getDynamicObject())
            o->setProperty("id", id);
        reply(replies, out);
    }
    close(replies);
    return 0;
}
