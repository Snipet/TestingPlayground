// PROBE name=plugin.mcp timeout=240
//
// plugin.mcp: Claude Desktop's way into the plugin (plugin/McpLink.h, mcp/Main.cpp), in a folder of the probe's own:
//   LINK     a processor listens on a private socket in the folder (0600, the folder 0700) and leaves tools.json and
//            guide.md there; info names its kind, plugin and files; tools are the chat's, but ask_user and
//            ask_planner; the guide says it is driven from outside, then the system prompt
//   CALL     write_file through the socket writes the processor's draft (the chat's card notes Claude Desktop once);
//            run_program compiles and plays it; a dial set reads back at once (before the message thread applies it);
//            read_file gives it back; view_panel answers with a PNG; an unknown
//            tool, and ask_user, are errors
//   SERVER   the server program (<Name>MCP) over stdio, as Claude Desktop runs it: initialize (its protocol version,
//            tools with listChanged, instructions), tools/list (list_instances, playground_guide and the chat's tools,
//            each with `instance`), list_instances naming this processor, get_status and write_file routed to it,
//            playground_guide; a second processor: a call without `instance` asks for one, with it goes there
//   GONE     a processor gone, its socket goes; the server, asked again, finds the one left; with none, it says to
//            open the plugin, and still lists the tools (from tools.json)
#include "Probe.h"

#include "mcp/McpProtocol.h"
#include "plugin/ChatAgent.h"
#include "plugin/EditorPort.h"
#include "plugin/McpLink.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <spawn.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <vector>

extern char** environ;

namespace tp = testingplayground;

namespace
{
    constexpr bool kInstrument = tp::product::kIsInstrument;

    std::string program()
    {
        return kInstrument ? "module Voice\n{\n    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                             "    bool held;\n    float phase;\n"
                             "    void start(float n, float v) { held = true; }\n    void stop() { held = false; }\n"
                             "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                             "        for (int i = 0; i < n; ++i) { phase += 0.01f; if (phase > 1.0f) phase -= 1.0f;\n"
                             "            left[i] += level[i] * (phase - 0.5f); right[i] += level[i] * (phase - 0.5f); }\n"
                             "        return held;\n    }\n};\n"
                           : "module Effect\n{\n    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                             "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                             "        for (int i = 0; i < n; ++i) { left[i] *= level[i]; right[i] *= level[i]; }\n    }\n};\n";
    }

    juce::var object(std::initializer_list<std::pair<const char*, juce::var>> props)
    {
        auto* o = new juce::DynamicObject();
        for (const auto& [k, v] : props)
            o->setProperty(k, v);
        return juce::var(o);
    }

    std::string line(const juce::var& v) { return juce::JSON::toString(v, true).toStdString(); }

    // A request on a link's socket, its reply, the processor's message-thread work done meanwhile (view_panel's
    // picture is drawn there).
    juce::var ask(tp::Processor& p, tp::mcp::LineReader& r, int id, const char* method, const juce::var& params)
    {
        std::atomic<bool> done{ false };
        std::string got;
        std::thread t([&] {
            if (tp::mcp::writeLine(r.fd(), line(object({ { "id", id }, { "method", method }, { "params", params } }))))
                r.next(got, 20000);
            done.store(true);
        });
        while (!done.load())
        {
            p.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        t.join();
        return juce::JSON::parse(juce::String::fromUTF8(got.c_str()));
    }

    std::string firstText(const juce::var& result)
    {
        if (const juce::Array<juce::var>* c = result["content"].getArray())
            for (const juce::var& b : *c)
                if (b["type"].toString() == "text")
                    return b["text"].toString().toStdString();
        return {};
    }

    bool hasTool(const juce::var& tools, const char* name)
    {
        if (const juce::Array<juce::var>* a = tools.getArray())
            for (const juce::var& t : *a)
                if (t["name"].toString() == name)
                    return true;
        return false;
    }

    // The server, as Claude Desktop runs it: its stdin and stdout, the probe's environment (its folder) passed on.
    struct Server
    {
        pid_t pid = -1;
        int in = -1, out = -1;
        std::unique_ptr<tp::mcp::LineReader> reader;
        int next = 1;

        bool start(const std::string& path)
        {
            int toChild[2], fromChild[2];
            if (pipe(toChild) != 0 || pipe(fromChild) != 0)
                return false;
            posix_spawn_file_actions_t fa;
            posix_spawn_file_actions_init(&fa);
            posix_spawn_file_actions_adddup2(&fa, toChild[0], 0);
            posix_spawn_file_actions_adddup2(&fa, fromChild[1], 1);
            posix_spawn_file_actions_addclose(&fa, toChild[1]);
            posix_spawn_file_actions_addclose(&fa, fromChild[0]);
            char* argv[] = { const_cast<char*>(path.c_str()), nullptr };
            const int rc = posix_spawn(&pid, path.c_str(), &fa, nullptr, argv, environ);
            posix_spawn_file_actions_destroy(&fa);
            close(toChild[0]);
            close(fromChild[1]);
            in = toChild[1];
            out = fromChild[0];
            reader = std::make_unique<tp::mcp::LineReader>(out);
            return rc == 0;
        }

        // A request and its reply (a notification before it is skipped), the processors' work done meanwhile.
        juce::var request(std::vector<tp::Processor*> ps, const char* method, const juce::var& params)
        {
            const int id = next++;
            std::atomic<bool> done{ false };
            juce::var reply;
            std::thread t([&] {
                const std::string msg = line(object({ { "jsonrpc", "2.0" }, { "id", id }, { "method", method }, { "params", params } })) + "\n";
                if (write(in, msg.data(), msg.size()) == static_cast<ssize_t>(msg.size()))
                {
                    std::string got;
                    while (reader->next(got, 60000) == 1)
                    {
                        const juce::var v = juce::JSON::parse(juce::String::fromUTF8(got.c_str()));
                        if (v.hasProperty("id") && static_cast<int>(v["id"]) == id)
                        {
                            reply = v;
                            break;
                        }
                    }
                }
                done.store(true);
            });
            while (!done.load())
            {
                for (tp::Processor* p : ps)
                    p->poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            t.join();
            return reply;
        }

        void stop()
        {
            if (in >= 0)
                close(in);
            int status = 0;
            if (pid > 0)
                waitpid(pid, &status, 0);
            if (out >= 0)
                close(out);
        }
    };
}

PROBE(plugin, mcp)
{
    const funkgui::HeadlessGuiScope gui;
    std::setvbuf(stdout, nullptr, _IONBF, 0);            // its lines as they come (a hang shows where)
    // A folder of the probe's own, short enough for a socket's path.
    const std::string dir = "/tmp/" + std::string(tp::product::kEnvPrefix) + "mcp-" + std::to_string(getpid());
    const std::string var = std::string(tp::product::kEnvPrefix) + "MCP_DIR";
    setenv(var.c_str(), dir.c_str(), 1);
    const char* serverPath = std::getenv((std::string(tp::product::kEnvPrefix) + "MCP_SERVER").c_str());

    // ---- LINK ------------------------------------------------------------------------------------------------------------
    auto a = std::make_unique<tp::Processor>();
    const tp::McpLink* link = a->mcpLink();
    P.eq("link.listening", link != nullptr && link->listening() ? 1 : 0, 1);
    struct stat st{};
    P.eq("link.private", stat(dir.c_str(), &st) == 0 && (st.st_mode & 0777) == 0700 && link != nullptr
                             && stat(link->socketPath().c_str(), &st) == 0 && (st.st_mode & 0777) == 0600 ? 1 : 0, 1);
    const juce::File folder(juce::String::fromUTF8(dir.c_str()));
    P.eq("link.cache", folder.getChildFile("tools.json").existsAsFile() && folder.getChildFile("guide.md").existsAsFile() ? 1 : 0, 1);
    const int fd = link != nullptr ? tp::mcp::connectTo(link->socketPath()) : -1;
    P.eq("link.connects", fd >= 0 ? 1 : 0, 1);
    tp::mcp::LineReader r(fd);
    std::printf("link: %s, fd %d\n", link != nullptr ? link->socketPath().c_str() : "-", fd);
    const juce::var info = ask(*a, r, 1, "info", {});
    std::printf("info: %s\n", line(info).substr(0, 200).c_str());
    P.eq("link.info", info["result"]["id"].toString() == juce::String(link != nullptr ? link->id() : "") && info["result"]["plugin"].toString() == tp::product::kName
                          && info["result"]["kind"].toString() == tp::product::kKind ? 1 : 0, 1);
    const juce::var tools = ask(*a, r, 2, "tools", {})["result"];
    P.eq("link.tools", hasTool(tools, "write_file") && hasTool(tools, "run_program") && hasTool(tools, "view_panel")
                           && !hasTool(tools, "ask_user") && !hasTool(tools, "ask_planner") ? 1 : 0, 1);
    const std::string guide = ask(*a, r, 3, "guide", {})["result"].toString().toStdString();
    P.eq("link.guide", guide.rfind("WORKING FROM OUTSIDE THE PLUGIN", 0) == 0 && guide.find(tp::ChatAgent::systemPrompt()) != std::string::npos ? 1 : 0, 1);

    // ---- CALL ------------------------------------------------------------------------------------------------------------
    const std::string text = program();
    juce::var res = ask(*a, r, 4, "call", object({ { "name", "write_file" },
                                                   { "arguments", object({ { "name", "program.fm" }, { "content", juce::String(text) } }) } }))["result"];
    std::printf("write_file: %s\n", firstText(res).c_str());
    P.eq("call.write", !static_cast<bool>(res["isError"]) && a->editorPort().draft().files.front().text == text ? 1 : 0, 1);
    {
        const tp::ChatState cs = a->chatAgent().chatState();
        int notes = 0;
        for (const tp::ChatEntry& e : cs.entries)
            notes += e.kind == tp::ChatEntry::Kind::note && e.text.find("Claude Desktop") != std::string::npos ? 1 : 0;
        P.eq("call.noted_once", notes, 1);
    }
    res = ask(*a, r, 5, "call", object({ { "name", "run_program" }, { "arguments", juce::var(new juce::DynamicObject()) } }))["result"];
    std::printf("run_program: %s\n", firstText(res).substr(0, 120).c_str());
    a->settle();
    P.eq("call.run", !static_cast<bool>(res["isError"]) && a->editorPort().status().ok && a->editorPort().running().front().text == text ? 1 : 0, 1);
    {
        // A dial set and read back at once, before the message thread has applied it: the value set (a measurement
        // right after set_dial renders it).
        tp::ChatAgent& agent = a->chatAgent();
        const tp::ChatAgent::McpResult set = agent.mcpCall("set_dial", object({ { "name", "level" }, { "value", 0.25 } }));
        const tp::ChatAgent::McpResult status = agent.mcpCall("get_status", juce::var(new juce::DynamicObject()));
        const std::string said = firstText(object({ { "content", status.content } }));
        std::printf("dial now: %s\n", said.substr(said.find("level"), 60).c_str());
        P.eq("call.dial_now", !set.isError && said.find("now 0.25") != std::string::npos ? 1 : 0, 1);
        a->poll();
    }
    res = ask(*a, r, 6, "call", object({ { "name", "read_file" }, { "arguments", object({ { "name", "program.fm" } }) } }))["result"];
    P.eq("call.read", firstText(res) == text ? 1 : 0, 1);
    res = ask(*a, r, 7, "call", object({ { "name", "view_panel" }, { "arguments", juce::var(new juce::DynamicObject()) } }))["result"];
    bool png = false;
    if (const juce::Array<juce::var>* c = res["content"].getArray())
        for (const juce::var& b : *c)
            png = png || (b["type"].toString() == "image" && b["mimeType"].toString() == "image/png" && b["data"].toString().startsWith("iVBOR"));
    P.eq("call.picture", png ? 1 : 0, 1);
    res = ask(*a, r, 8, "call", object({ { "name", "no_such_tool" }, { "arguments", juce::var(new juce::DynamicObject()) } }))["result"];
    P.eq("call.unknown", static_cast<bool>(res["isError"]) ? 1 : 0, 1);
    res = ask(*a, r, 9, "call", object({ { "name", "ask_user" }, { "arguments", juce::var(new juce::DynamicObject()) } }))["result"];
    P.eq("call.chat_only", static_cast<bool>(res["isError"]) ? 1 : 0, 1);
    close(fd);

    // ---- SERVER ----------------------------------------------------------------------------------------------------------
    P.eq("server.built", serverPath != nullptr && juce::File(serverPath).existsAsFile() ? 1 : 0, 1);
    Server s;
    const bool started = serverPath != nullptr && s.start(serverPath);
    P.eq("server.starts", started ? 1 : 0, 1);
    if (started)
    {
        juce::var rep = s.request({ a.get() }, "initialize", object({ { "protocolVersion", "2025-06-18" }, { "capabilities", juce::var(new juce::DynamicObject()) },
                                                                    { "clientInfo", object({ { "name", "probe" }, { "version", "1" } }) } }));
        std::printf("initialize: %s\n", line(rep["result"]["serverInfo"]).c_str());
        P.eq("server.initialize", rep["result"]["protocolVersion"].toString() == "2025-06-18" && static_cast<bool>(rep["result"]["capabilities"]["tools"]["listChanged"])
                                      && rep["result"]["instructions"].toString().contains("playground_guide") ? 1 : 0, 1);
        rep = s.request({ a.get() }, "tools/list", {});
        const juce::var list = rep["result"]["tools"];
        bool instanceArg = true;
        int pluginTools = 0;
        if (const juce::Array<juce::var>* t = list.getArray())
            for (const juce::var& tool : *t)
                if (tool["name"].toString() != "list_instances" && tool["name"].toString() != "playground_guide")
                {
                    ++pluginTools;
                    instanceArg = instanceArg && tool["inputSchema"]["properties"].hasProperty("instance");
                }
        std::printf("tools/list: %d of the plugin's\n", pluginTools);
        P.eq("server.tools", hasTool(list, "list_instances") && hasTool(list, "playground_guide") && hasTool(list, "write_file")
                                 && !hasTool(list, "ask_user") && pluginTools == tools.size() && instanceArg ? 1 : 0, 1);
        rep = s.request({ a.get() }, "tools/call", object({ { "name", "list_instances" }, { "arguments", juce::var(new juce::DynamicObject()) } }));
        std::printf("list_instances: %s", firstText(rep["result"]).c_str());
        P.eq("server.lists", firstText(rep["result"]).find(link != nullptr ? link->id() : "?") != std::string::npos ? 1 : 0, 1);
        rep = s.request({ a.get() }, "tools/call", object({ { "name", "get_status" }, { "arguments", juce::var(new juce::DynamicObject()) } }));
        std::printf("get_status: %s\n", firstText(rep["result"]).substr(0, 160).c_str());
        P.eq("server.routes", !static_cast<bool>(rep["result"]["isError"]) && !firstText(rep["result"]).empty() ? 1 : 0, 1);
        rep = s.request({ a.get() }, "tools/call", object({ { "name", "playground_guide" }, { "arguments", juce::var(new juce::DynamicObject()) } }));
        P.eq("server.guide", firstText(rep["result"]) == guide ? 1 : 0, 1);

        // A second instance: a call without `instance` is still the one used last; another names it.
        auto b = std::make_unique<tp::Processor>();
        const std::string bid = b->mcpLink() != nullptr ? b->mcpLink()->id() : "";
        rep = s.request({ a.get(), b.get() }, "tools/call",
                        object({ { "name", "write_file" },
                                 { "arguments", object({ { "name", "notes.fm" }, { "content", "// for b\n" }, { "instance", juce::String(bid) } }) } }));
        P.eq("server.second", !static_cast<bool>(rep["result"]["isError"]) && b->editorPort().draft().files.size() == 2
                                  && a->editorPort().draft().files.size() == 1 ? 1 : 0, 1);
        rep = s.request({ a.get(), b.get() }, "tools/call", object({ { "name", "list_instances" }, { "arguments", juce::var(new juce::DynamicObject()) } }));
        P.eq("server.lists_two", firstText(rep["result"]).rfind("2 instances", 0) == 0 ? 1 : 0, 1);

        // ---- GONE ------------------------------------------------------------------------------------------------------
        const std::string bsock = b->mcpLink() != nullptr ? b->mcpLink()->socketPath() : "";
        b.reset();
        P.eq("gone.socket", !juce::File(juce::String::fromUTF8(bsock.c_str())).exists() ? 1 : 0, 1);
        rep = s.request({ a.get() }, "tools/call", object({ { "name", "list_instances" }, { "arguments", juce::var(new juce::DynamicObject()) } }));
        P.eq("gone.one_left", firstText(rep["result"]).rfind("1 instance", 0) == 0 ? 1 : 0, 1);
        a.reset();
        rep = s.request({}, "tools/call", object({ { "name", "get_status" }, { "arguments", juce::var(new juce::DynamicObject()) } }));
        std::printf("none open: %s\n", firstText(rep["result"]).c_str());
        P.eq("gone.says_open_it", static_cast<bool>(rep["result"]["isError"]) && firstText(rep["result"]).find("open it") != std::string::npos ? 1 : 0, 1);
        rep = s.request({}, "tools/list", {});
        P.eq("gone.tools_cached", hasTool(rep["result"]["tools"], "write_file") ? 1 : 0, 1);
        s.stop();
    }
    a.reset();
    folder.deleteRecursively();
    return P.finish();
}
