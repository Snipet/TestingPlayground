// PROBE name=plugin.mcp timeout=240
//
// plugin.mcp: Claude Desktop's way into the plugin (plugin/McpLink.h, mcp/Main.cpp), in a folder of the probe's own:
//   LINK     a processor listens on a private socket in the folder (0600, the folder 0700) and leaves tools.json and
//            guide.md there; info names its kind, plugin and files; tools are the chat's, but ask_user and
//            ask_planner, each MCP description its first sentence or paragraph (tools.json's size printed, with the
//            full descriptions for comparison); the guide's start says it is driven from outside; a guide request
//            naming no part (a 0.2.0 server's) gets the whole guide
//   GUIDE    the guide in parts (mcp/McpGuide.h): start (working from outside, FMINUS, DSP PRACTICE, CHECKING, the
//            notes of the tools it uses and the other parts named; its size printed, in bytes and about tokens),
//            large-program (with THREADS), interface, library (a line a file), examples, projects, all (the system prompt whole);
//            library:<file> its header, library:<file>:code its whole text with no tab added to the editor; an unknown
//            part refused, naming the parts
//   CALL     write_file through the socket writes the processor's draft (the chat's card notes Claude Desktop once);
//            run_program compiles and plays it; a dial set reads back at once (before the message thread applies it);
//            read_file gives it back; view_panel answers with a PNG; an unknown
//            tool, and ask_user, are errors
//   WAVE     on a second connection, with a slow program in the editor: get_status and info answered while a tune runs
//            (each reply a whole line); the tune cancelled, its dial as it was; a call waiting behind it cancelled,
//            answered at once; a tune with a time limit of a second stops there with the best so far; pictures fit
//            (view_panel's window and view_sound at most 100,000 base64 characters, the text saying what was sent;
//            detail "high" the picture whole)
//   CROSSING a large request (half a megabyte) sent while the link answers the guide's start (more than the socket's
//            buffers hold), without reading meanwhile: the link keeps reading, so the request goes through; then both
//            replies come, whole lines
//   NAMES    the first processor is "Synth 1" ("Effect 1"), its socket 1.sock; a second one "Synth 2"
//   SERVER   the server program (<Name>MCP) over stdio, as Claude Desktop runs it: initialize (its protocol version,
//            2025-11-25 too, tools with listChanged, prompts, instructions that keep tools and ids from the user),
//            tools/list (list_instances, playground_guide and the chat's tools, each with `instance` and a title in
//            words, the ones that only look marked read-only), list_instances naming this processor by its name,
//            get_status and write_file routed to it, playground_guide (its start, and a part); the processor then knows
//            Claude Desktop is connected, and its port says so with its name; the prompts (Make a sound, Improve the
//            sound, Explain the sound, Fix what I hear), one filled in with the user's words, fix_what_i_hear naming no
//            tool and carrying the plugin's state
//   DEADLINE a server whose calls may take 1.5 s: a tune past it is answered then ("still running"), once, while
//            list_instances and get_status answer meanwhile; the plugin stops the tune (its dial as it was) and stays
//            reachable
//   STUCK    an instance that never reads (a socket nobody accepts on): a large call to it waits in the server's
//            outbox, a ping is answered meanwhile, and the call is answered at its deadline
//   MATCH    a second processor, on the track "Bass Line" with the project "Moog Bass": `instance` reaches it by its
//            name, by "2", by its track's name in another case, by part of its project's; a word matching nothing
//            describes both so that Claude asks; a fresh server, with both open and none named, asks which one
//   GONE     a processor gone, its socket goes; the server, asked again, finds the one left; the server gone, the
//            processor no longer counts as connected; with none open, a call says to open the plugin, the tools
//            still list (from tools.json) and the guide's parts come from guide.md, as the plugin gave them
#include "Probe.h"

#include "mcp/McpProtocol.h"
#include "plugin/ChatAgent.h"
#include "plugin/EditorPort.h"
#include "plugin/McpLink.h"
#include "plugin/Processor.h"

#include "Library.h"
#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <vector>

extern char** environ;

namespace pg = playground;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;

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

    // The same, but slow (a few hundred operations a sample): a tune's renders take a while, so it can be cancelled.
    std::string slowProgram()
    {
        return kInstrument ? "module Voice\n{\n    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                             "    bool held;\n    float phase;\n"
                             "    void start(float n, float v) { held = true; }\n    void stop() { held = false; }\n"
                             "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                             "        for (int i = 0; i < n; ++i) { phase += 0.01f; if (phase > 1.0f) phase -= 1.0f;\n"
                             "            float x = phase - 0.5f;\n"
                             "            for (int k = 0; k < 300; ++k) x = x * 0.9999f + 0.00001f;\n"
                             "            left[i] += level[i] * x; right[i] += level[i] * x; }\n"
                             "        return held;\n    }\n};\n"
                           : "module Effect\n{\n    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                             "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                             "        for (int i = 0; i < n; ++i) { float x = left[i];\n"
                             "            for (int k = 0; k < 300; ++k) x = x * 0.9999f + 0.00001f;\n"
                             "            left[i] = x * level[i]; right[i] *= level[i]; }\n    }\n};\n";
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
    juce::var ask(pg::Processor& p, pg::mcp::LineReader& r, int id, const char* method, const juce::var& params)
    {
        std::atomic<bool> done{ false };
        std::string got;
        std::thread t([&] {
            if (pg::mcp::writeLine(r.fd(), line(object({ { "id", id }, { "method", method }, { "params", params } }))))
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

    bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

    // tune_dials on `level` toward a level it cannot reach, over long renders: it runs until stopped, or `seconds`.
    juce::var tuneCall(double seconds)
    {
        juce::Array<juce::var> tune{ "level" }, goals;
        goals.add(kInstrument ? object({ { "metric", "rms_db" }, { "target", 20.0 } }) : object({ { "metric", "sine_gain_db" }, { "target", 30.0 } }));
        return object({ { "name", "tune_dials" },
                        { "arguments", object({ { "tune", tune }, { "goals", goals }, { "budget", 400 }, { "hold_seconds", 10.0 },
                                                { "tail_seconds", 0.2 }, { "seconds", seconds } }) } });
    }

    // Until the chat's activity starts with `what` (the message thread's work done meanwhile); false after `ms`.
    bool waitActivity(pg::Processor& p, const char* what, int ms)
    {
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(ms))
        {
            if (p.chatAgent().chatState().activity.rfind(what, 0) == 0)
                return true;
            p.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    // Until the chat is no longer busy (an MCP call ended); false after `ms`.
    bool waitIdle(pg::Processor& p, int ms)
    {
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(ms))
        {
            if (!p.chatAgent().chatState().busy)
                return true;
            p.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    // A dial's value as get_status says it ("now 0.25").
    bool dialNow(pg::Processor& p, const std::string& value)
    {
        p.poll();
        const pg::ChatAgent::McpResult status = p.chatAgent().mcpCall("get_status", juce::var(new juce::DynamicObject()));
        std::string said;
        if (const juce::Array<juce::var>* c = status.content.getArray())
            said = (*c)[0]["text"].toString().toStdString();
        return said.find("level: float [0, 1] default 0.5, now " + value + "\n") != std::string::npos;
    }

    // A link's socket as the server uses it: several requests in flight, their replies read as they come (each line
    // counted, and those that are whole JSON objects), the processor's message-thread work done meanwhile.
    struct Wire
    {
        pg::Processor& p;
        int fd;
        pg::mcp::LineReader r;
        std::vector<juce::var> got;                      // the replies, in the order they came
        int lines = 0, whole = 0;

        Wire(pg::Processor& proc, int f) : p(proc), fd(f), r(f) {}

        bool send(int id, const char* method, const juce::var& params)
        {
            return pg::mcp::writeLine(fd, line(object({ { "id", id }, { "method", method }, { "params", params } })));
        }

        int position(int id) const
        {
            for (std::size_t k = 0; k < got.size(); ++k)
                if (static_cast<int>(got[k]["id"]) == id)
                    return static_cast<int>(k);
            return -1;
        }

        // The reply to `id` (void when none comes within `ms`).
        juce::var wait(int id, int ms)
        {
            const auto t0 = std::chrono::steady_clock::now();
            for (;;)
            {
                if (const int k = position(id); k >= 0)
                    return got[static_cast<std::size_t>(k)];
                if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(ms))
                    return {};
                std::string l;
                const int n = r.next(l, 5);
                if (n < 0)
                    return {};
                if (n == 1)
                {
                    ++lines;
                    const juce::var v = juce::JSON::parse(juce::String::fromUTF8(l.c_str()));
                    whole += v.isObject() ? 1 : 0;
                    got.push_back(v);
                }
                p.poll();
            }
        }
    };

    // `text` written to `fd` without reading from it meanwhile: false when the other end stops taking it for `ms` (it
    // waits for this end to read what it sent).
    bool writeWithin(int fd, const std::string& text, int ms)
    {
        const int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        std::size_t sent = 0;
        auto last = std::chrono::steady_clock::now();
        while (sent < text.size() && std::chrono::steady_clock::now() - last < std::chrono::milliseconds(ms))
        {
            const ssize_t n = send(fd, text.data() + sent, text.size() - sent, 0);
            if (n > 0)
            {
                sent += static_cast<std::size_t>(n);
                last = std::chrono::steady_clock::now();
                continue;
            }
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && errno != ENOBUFS)
                break;
            pollfd p{ fd, POLLOUT, 0 };
            poll(&p, 1, 20);
        }
        fcntl(fd, F_SETFL, flags);
        return sent == text.size();
    }

    // A picture in a tool's result: its base64 characters, its type, and the text after it.
    struct Picture
    {
        std::size_t chars = 0;
        std::string type, text;
    };

    Picture pictureOf(const juce::var& result)
    {
        Picture out;
        if (const juce::Array<juce::var>* c = result["content"].getArray())
            for (const juce::var& b : *c)
            {
                if (b["type"].toString() == "image")
                {
                    out.chars = static_cast<std::size_t>(b["data"].toString().length());
                    out.type = b["mimeType"].toString().toStdString();
                }
                else if (b["type"].toString() == "text")
                    out.text += b["text"].toString().toStdString() + "\n";
            }
        return out;
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
        std::unique_ptr<pg::mcp::LineReader> reader;
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
            reader = std::make_unique<pg::mcp::LineReader>(out);
            return rc == 0;
        }

        std::map<int, juce::var> early;                  // replies read while waiting for another's
        std::vector<int> order;                          // the ids of the replies, in the order they came
        int lines = 0, whole = 0;

        // A request sent, its reply not waited for: its id.
        int post(const char* method, const juce::var& params)
        {
            const int id = next++;
            const std::string msg = line(object({ { "jsonrpc", "2.0" }, { "id", id }, { "method", method }, { "params", params } })) + "\n";
            [[maybe_unused]] const ssize_t n = write(in, msg.data(), msg.size());
            return id;
        }

        // The reply to `id` (a notification, or another's reply, kept aside), the processors' work done meanwhile.
        juce::var wait(std::vector<pg::Processor*> ps, int id, int ms = 60000)
        {
            if (const auto it = early.find(id); it != early.end())
                return it->second;
            std::atomic<bool> done{ false };
            juce::var reply;
            std::thread t([&] {
                std::string got;
                const auto t0 = std::chrono::steady_clock::now();
                while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(ms) && reader->next(got, 100) >= 0)
                {
                    if (got.empty())
                        continue;
                    ++lines;
                    const juce::var v = juce::JSON::parse(juce::String::fromUTF8(got.c_str()));
                    got.clear();
                    whole += v.isObject() ? 1 : 0;
                    if (!v.hasProperty("id"))
                        continue;
                    order.push_back(static_cast<int>(v["id"]));
                    if (static_cast<int>(v["id"]) == id)
                    {
                        reply = v;
                        break;
                    }
                    early[static_cast<int>(v["id"])] = v;
                }
                done.store(true);
            });
            while (!done.load())
            {
                for (pg::Processor* p : ps)
                    p->poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            t.join();
            return reply;
        }

        // A request and its reply.
        juce::var request(std::vector<pg::Processor*> ps, const char* method, const juce::var& params)
        {
            return wait(std::move(ps), post(method, params));
        }

        // The replies that come within `ms` (the processors' work done meanwhile), by id.
        std::vector<int> drain(std::vector<pg::Processor*> ps, int ms)
        {
            const std::size_t from = order.size();
            wait(std::move(ps), -1, ms);
            return { order.begin() + static_cast<std::ptrdiff_t>(from), order.end() };
        }

        // Gone at once (a server stuck writing would never see its stdin close).
        void kill()
        {
            if (pid > 0)
                ::kill(pid, SIGKILL);
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
    const std::string dir = "/tmp/" + std::string(pg::product::kEnvPrefix) + "mcp-" + std::to_string(getpid());
    const std::string var = std::string(pg::product::kEnvPrefix) + "MCP_DIR";
    setenv(var.c_str(), dir.c_str(), 1);
    const char* serverPath = std::getenv((std::string(pg::product::kEnvPrefix) + "MCP_SERVER").c_str());

    // ---- LINK ------------------------------------------------------------------------------------------------------------
    auto a = std::make_unique<pg::Processor>();
    const pg::McpLink* link = a->mcpLink();
    P.eq("link.listening", link != nullptr && link->listening() ? 1 : 0, 1);
    struct stat st{};
    P.eq("link.private", stat(dir.c_str(), &st) == 0 && (st.st_mode & 0777) == 0700 && link != nullptr
                             && stat(link->socketPath().c_str(), &st) == 0 && (st.st_mode & 0777) == 0600 ? 1 : 0, 1);
    const juce::File folder(juce::String::fromUTF8(dir.c_str()));
    P.eq("link.cache", folder.getChildFile("tools.json").existsAsFile() && folder.getChildFile("guide.md").existsAsFile() ? 1 : 0, 1);
    const int fd = link != nullptr ? pg::mcp::connectTo(link->socketPath()) : -1;
    P.eq("link.connects", fd >= 0 ? 1 : 0, 1);
    pg::mcp::LineReader r(fd);
    std::printf("link: %s, fd %d\n", link != nullptr ? link->socketPath().c_str() : "-", fd);
    const juce::var info = ask(*a, r, 1, "info", {});
    std::printf("info: %s\n", line(info).substr(0, 200).c_str());
    const std::string first = std::string(kInstrument ? "Synth" : "Effect") + " 1";
    P.eq("names.first", link != nullptr && link->id() == "1" && link->name() == first
                            && juce::File(juce::String::fromUTF8(link->socketPath().c_str())).getFileName() == "1.sock" ? 1 : 0, 1);
    P.eq("link.info", info["result"]["id"].toString() == juce::String(link != nullptr ? link->id() : "") && info["result"]["name"].toString() == juce::String(first)
                          && info["result"]["plugin"].toString() == pg::product::kName
                          && info["result"]["kind"].toString() == pg::product::kKind ? 1 : 0, 1);
    const juce::var tools = ask(*a, r, 2, "tools", {})["result"];
    P.eq("link.tools", hasTool(tools, "write_file") && hasTool(tools, "run_program") && hasTool(tools, "view_panel")
                           && !hasTool(tools, "ask_user") && !hasTool(tools, "ask_planner") ? 1 : 0, 1);
    {
        // Each MCP description is the chat's first sentence (or paragraph); tools.json's size, and with the full ones.
        const juce::var full = pg::ChatAgent::toolDefinitions();
        juce::var whole = juce::JSON::parse(juce::JSON::toString(tools));
        bool leads = tools.size() > 0;
        if (const juce::Array<juce::var>* t = whole.getArray())
            for (const juce::var& tool : *t)
                if (const juce::Array<juce::var>* f = full.getArray())
                    for (const juce::var& d : *f)
                        if (d["name"] == tool["name"])
                        {
                            const juce::String lead = tool["description"].toString();
                            leads = leads && lead.isNotEmpty() && d["description"].toString().startsWith(lead)
                                 && (lead == d["description"].toString() || lead.endsWithChar('.'));
                            tool.getDynamicObject()->setProperty("description", d["description"]);
                        }
        const juce::int64 size = folder.getChildFile("tools.json").getSize();
        std::printf("tools.json: %lld bytes (with the chat's full descriptions: %d bytes)\n", static_cast<long long>(size),
                    juce::JSON::toString(whole, false).length());
        std::string each;
        if (const juce::Array<juce::var>* t = tools.getArray())
            for (const juce::var& tool : *t)
                each += " " + tool["name"].toString().toStdString() + " " + std::to_string(juce::JSON::toString(tool, false).length())
                      + " (" + std::to_string(juce::JSON::toString(tool["inputSchema"], false).length()) + ")";
        std::printf("tools.json by tool, bytes (its schema's):%s\n", each.c_str());
        P.eq("link.tools_short", leads ? 1 : 0, 1);
        // Lane A's 21000 / 19500, the renders lane's compare, scenario and try_edits (about 1.7 KB), and trace (about
        // 1.1 KB): the wave's integration sets the last (the wave's whole, about 24 KB).
        P.le("link.tools_bytes", static_cast<double>(size), kInstrument ? 23900.0 : 22450.0);
    }
    const std::string guide = ask(*a, r, 3, "guide", object({ { "part", "start" } }))["result"].toString().toStdString();
    P.eq("link.guide", guide.rfind("WORKING FROM OUTSIDE THE PLUGIN", 0) == 0 ? 1 : 0, 1);
    // A 0.2.0 server asks for the guide with no part, and has no way to ask for the others: it gets all of it.
    const std::string unnamed = ask(*a, r, 10, "guide", {})["result"].toString().toStdString();
    P.eq("link.guide_unnamed_whole", unnamed == pg::ChatAgent::mcpGuide() && unnamed != guide ? 1 : 0, 1);

    // ---- GUIDE -----------------------------------------------------------------------------------------------------------
    std::string interfacePart;
    {
        int id = 100;
        const auto part = [&](const std::string& name) { return ask(*a, r, id++, "guide", object({ { "part", juce::String(name) } })); };
        const auto text = [&](const std::string& name) { return part(name)["result"].toString().toStdString(); };
        const std::string all = text("all");
        P.eq("guide.all", all.rfind("WORKING FROM OUTSIDE THE PLUGIN", 0) == 0 && has(all, pg::ChatAgent::systemPrompt()) ? 1 : 0, 1);
        P.eq("guide.start", has(guide, "\nFMINUS, THE LANGUAGE\n") && has(guide, "\nDSP PRACTICE IN THIS PLUGIN\n")
                                && has(guide, "\nCHECKING YOUR WORK\n") && has(guide, "\nTHE OTHER PARTS OF THIS GUIDE\n")
                                && has(guide, "<tool name=\"write_file\" part=\"start\">") && has(guide, "\"library:<file>:code\"")
                                && !has(guide, "\nTHE INTERFACE: panel.gui\n") && !has(guide, "<library_file")
                                && !has(guide, "\nBUILDING A LARGE PROGRAM\n") && !has(guide, "\nTHREADS: parallel for")
                                && guide == text("start") ? 1 : 0, 1);
        std::printf("guide: start %zu bytes (about %zu tokens); all %zu bytes (about %zu tokens)\n", guide.size(), guide.size() / 4,
                    all.size(), all.size() / 4);
        P.le("guide.start_tokens", static_cast<double>(guide.size() / 4), 9150.0);   // 8500, the renders lane's ~400, trace's ~210
        const std::string large = text("large-program");
        interfacePart = text("interface");
        const std::string library = text("library"), examples = text("examples"), projects = text("projects");
        std::printf("guide parts: large-program %zu, interface %zu, library %zu, examples %zu, projects %zu bytes\n", large.size(),
                    interfacePart.size(), library.size(), examples.size(), projects.size());
        for (const auto& [name, body] : { std::pair<const char*, const std::string*>{ "start", &guide }, { "large-program", &large },
                                          { "interface", &interfacePart }, { "library", &library }, { "examples", &examples },
                                          { "projects", &projects } })
            juce::File(juce::String(probe::outPath(std::string("plugin.mcp.guide.") + name + ".md"))).replaceWithText(juce::String::fromUTF8(body->c_str()));
        juce::File(juce::String(probe::outPath("plugin.mcp.tools.json"))).replaceWithText(juce::JSON::toString(tools, false));
        P.eq("guide.large_program", large.rfind("BUILDING A LARGE PROGRAM\n", 0) == 0 && has(large, "\nTHREADS: parallel for")
                                        && has(large, "\nA PROGRAM BUILT FROM THE LIBRARY")
                                        && has(large, "<example_program") && !has(large, "\nTHE INTERFACE: panel.gui\n") ? 1 : 0, 1);
        P.eq("guide.interface", interfacePart.rfind("THE INTERFACE: panel.gui\n", 0) == 0 && has(interfacePart, "<tool name=\"view_panel\"")
                                    && !has(interfacePart, "<library_file") ? 1 : 0, 1);
        int listed = 0;
        for (const pg::LibraryFile& f : pg::kLibrary)
            listed += has(library, std::string("\n- ") + f.name + " (" + f.category + "): " + f.summary + "\n") ? 1 : 0;
        P.eq("guide.library", library.rfind("THE LIBRARY\n", 0) == 0 && has(library, "<tool name=\"add_library_file\"")
                                  && !has(library, "<library_file") && listed == static_cast<int>(std::size(pg::kLibrary)) ? 1 : 0, 1);
        P.eq("guide.examples", examples.rfind("THIS PLUGIN'S EXAMPLES", 0) == 0 && has(examples, "<example title=") ? 1 : 0, 1);
        P.eq("guide.projects", projects.rfind("RANDOM AND PROJECTS\n", 0) == 0 && !has(projects, "<tool name=") ? 1 : 0, 1);
        const pg::LibraryFile& lib = pg::kLibrary[0];
        const std::string header = text(std::string("library:") + lib.name);
        const std::string firstLine = std::string(lib.source).substr(0, std::string(lib.source).find('\n'));
        P.eq("guide.library_file", header.rfind(std::string("<library_file name=\"") + lib.name + "\"", 0) == 0 && has(header, firstLine)
                                       && header.size() < std::string(lib.source).size() ? 1 : 0, 1);
        const pg::Draft before = a->editorPort().draft();
        const std::string code = text(std::string("library:") + lib.name + ":code");
        a->poll();
        const pg::Draft after = a->editorPort().draft();
        P.eq("guide.library_code", has(code, lib.source) && before.files.size() == after.files.size() && before.files == after.files ? 1 : 0, 1);
        const juce::var unknown = part("nonsense");
        std::printf("unknown part: %s\n", unknown["error"].toString().toRawUTF8());
        P.eq("guide.unknown", unknown.hasProperty("error") && unknown["error"].toString().contains("\"interface\"") ? 1 : 0, 1);
    }

    // ---- CALL ------------------------------------------------------------------------------------------------------------
    const std::string text = program();
    juce::var res = ask(*a, r, 4, "call", object({ { "name", "write_file" },
                                                   { "arguments", object({ { "name", "program.fm" }, { "content", juce::String(text) } }) } }))["result"];
    std::printf("write_file: %s\n", firstText(res).c_str());
    P.eq("call.write", !static_cast<bool>(res["isError"]) && a->editorPort().draft().files.front().text == text ? 1 : 0, 1);
    {
        const pg::ChatState cs = a->chatAgent().chatState();
        int notes = 0;
        for (const pg::ChatEntry& e : cs.entries)
            notes += e.kind == pg::ChatEntry::Kind::note && e.text.find("Claude Desktop") != std::string::npos ? 1 : 0;
        P.eq("call.noted_once", notes, 1);
    }
    res = ask(*a, r, 5, "call", object({ { "name", "run_program" }, { "arguments", juce::var(new juce::DynamicObject()) } }))["result"];
    std::printf("run_program: %s\n", firstText(res).substr(0, 120).c_str());
    a->settle();
    P.eq("call.run", !static_cast<bool>(res["isError"]) && a->editorPort().status().ok && a->editorPort().running().front().text == text ? 1 : 0, 1);
    {
        // A dial set and read back at once, before the message thread has applied it: the value set (a measurement
        // right after set_dial renders it).
        pg::ChatAgent& agent = a->chatAgent();
        const pg::ChatAgent::McpResult set = agent.mcpCall("set_dial", object({ { "name", "level" }, { "value", 0.25 } }));
        const pg::ChatAgent::McpResult status = agent.mcpCall("get_status", juce::var(new juce::DynamicObject()));
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

    // ---- WAVE: calls side by side, cancel, the time limit, pictures that fit ---------------------------------------------
    {
        Wire w(*a, link != nullptr ? pg::mcp::connectTo(link->socketPath()) : -1);
        const auto call = [](const char* name, const juce::var& arguments) { return object({ { "name", name }, { "arguments", arguments } }); };
        const juce::var none(new juce::DynamicObject());
        w.send(200, "call", call("write_file", object({ { "name", "program.fm" }, { "content", juce::String(slowProgram()) } })));
        w.wait(200, 20000);
        P.eq("wave.level_before", dialNow(*a, "0.25") ? 1 : 0, 1);

        // A tune running; get_status and info answered meanwhile; then the tune cancelled: its dial as it was.
        w.send(201, "call", tuneCall(60.0));
        const bool tuning = waitActivity(*a, "Tuning", 20000);
        w.send(202, "call", call("get_status", none));
        w.send(203, "info", none);
        const juce::var status = w.wait(202, 10000), info2 = w.wait(203, 10000);
        P.eq("live.beside_a_call", tuning && w.position(201) < 0 && !static_cast<bool>(status["result"]["isError"])
                                       && has(firstText(status["result"]), "Dials:") && info2["result"]["name"].toString() == juce::String(first) ? 1 : 0, 1);
        w.send(204, "cancel", object({ { "id", 201 } }));
        const juce::var cancelled = w.wait(204, 5000), tuned = w.wait(201, 30000);
        std::printf("cancelled tune: %s\n", firstText(tuned["result"]).c_str());
        waitIdle(*a, 10000);
        P.eq("cancel.answered", static_cast<bool>(cancelled["result"]["cancelled"]) ? 1 : 0, 1);
        P.eq("cancel.dials_kept", has(firstText(tuned["result"]), "Claude Desktop stopped this call") && dialNow(*a, "0.25") ? 1 : 0, 1);

        // A call waiting behind a running one, cancelled: answered at once, never run.
        w.send(205, "call", tuneCall(60.0));
        waitActivity(*a, "Tuning", 20000);
        w.send(206, "call", call("set_dial", object({ { "name", "level" }, { "value", 0.9 } })));
        w.send(207, "cancel", object({ { "id", 206 } }));
        const juce::var dropped = w.wait(206, 5000);
        P.eq("cancel.waiting", static_cast<bool>(dropped["result"]["isError"]) && has(firstText(dropped["result"]), "before it started")
                                   && w.position(205) < 0 ? 1 : 0, 1);
        w.send(208, "cancel", object({ { "id", 205 } }));
        w.wait(205, 30000);
        waitIdle(*a, 10000);
        P.eq("cancel.never_ran", dialNow(*a, "0.25") ? 1 : 0, 1);
        P.eq("live.whole_lines", w.lines > 0 && w.whole == w.lines ? 1 : 0, 1);

        // A time limit of a second: the search stops there, with the best so far (applied: no cancel).
        w.send(209, "call", tuneCall(1.0));
        const juce::var limited = w.wait(209, 60000);
        const std::string said = firstText(limited["result"]);
        std::printf("tune with a limit: %s\n", said.c_str());
        waitIdle(*a, 10000);
        P.eq("tune.time_limit", has(said, "stopped at the time limit: the best so far")
                                    && (has(said, "found before the time limit") ? !dialNow(*a, "0.25") : dialNow(*a, "0.25")) ? 1 : 0, 1);
        a->chatAgent().mcpCall("set_dial", object({ { "name", "level" }, { "value", 0.25 } }));
        a->poll();

        // Pictures that fit a tool's result, the text saying what was sent; detail "high": whole.
        w.send(210, "call", call("view_panel", object({ { "view", "window" } })));
        const Picture window = pictureOf(w.wait(210, 30000)["result"]);
        w.send(211, "call", call("view_panel", object({ { "view", "window" }, { "detail", "high" } })));
        const Picture high = pictureOf(w.wait(211, 30000)["result"]);
        w.send(212, "call", call("view_sound", none));
        const Picture sound = pictureOf(w.wait(212, 60000)["result"]);
        std::printf("pictures: the window %zu characters (%s; whole %zu, %s), the sound %zu (%s)\n", window.chars, window.type.c_str(),
                    high.chars, high.type.c_str(), sound.chars, sound.type.c_str());
        std::printf("the window's text: %s", window.text.c_str());
        P.eq("picture.window_fits", window.chars > 0 && window.chars <= 100000
                                        && (high.chars <= 100000 || has(window.text, "sent smaller")) ? 1 : 0, 1);
        P.eq("picture.whole", high.type == "image/png" && high.chars >= window.chars && has(high.text, "sent whole") ? 1 : 0, 1);
        P.eq("picture.sound_fits", sound.chars > 0 && sound.chars <= 100000 ? 1 : 0, 1);
        w.send(213, "call", call("write_file", object({ { "name", "program.fm" }, { "content", juce::String(text) } })));
        w.wait(213, 20000);
        close(w.fd);
    }

    // ---- CROSSING: a large request while a large reply waits to be read -------------------------------------------------
    {
        Wire w(*a, link != nullptr ? pg::mcp::connectTo(link->socketPath()) : -1);
        w.send(300, "guide", object({ { "part", "start" } }));
        const std::string big = line(object({ { "id", 301 }, { "method", "call" },
                                              { "params", object({ { "name", "no_such_tool" },
                                                                   { "arguments", object({ { "padding", juce::String::repeatedString("x", 512 * 1024) } }) } }) } }))
                              + "\n";
        const bool through = writeWithin(w.fd, big, 5000);
        const juce::var start = w.wait(300, 10000), refused = w.wait(301, 10000);
        std::printf("crossing: the request %s, the guide's reply %zu bytes, the call's %s\n", through ? "went through" : "STUCK",
                    start["result"].toString().toStdString().size(), firstText(refused["result"]).substr(0, 60).c_str());
        P.eq("link.crossing", through && start["result"].toString().toStdString() == guide && static_cast<bool>(refused["result"]["isError"])
                                  && w.lines == 2 && w.whole == 2 ? 1 : 0, 1);
        close(w.fd);
    }

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
                                      && rep["result"]["capabilities"].hasProperty("prompts")
                                      && rep["result"]["instructions"].toString().contains("playground_guide")
                                      && rep["result"]["instructions"].toString().contains("never ask them about tools") ? 1 : 0, 1);
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
        std::printf("tools/list: %d of the plugin's, %d bytes in all\n", pluginTools, juce::JSON::toString(list, false).length());
        P.eq("server.tools", hasTool(list, "list_instances") && hasTool(list, "playground_guide") && hasTool(list, "write_file")
                                 && !hasTool(list, "ask_user") && pluginTools == tools.size() && instanceArg ? 1 : 0, 1);
        {
            juce::var measure, write;
            bool titled = true;
            if (const juce::Array<juce::var>* t = list.getArray())
                for (const juce::var& tool : *t)
                {
                    titled = titled && tool["title"].toString().isNotEmpty() && tool["title"].toString() != tool["name"].toString();
                    if (tool["name"].toString() == "measure_output")
                        measure = tool;
                    if (tool["name"].toString() == "write_file")
                        write = tool;
                }
            P.eq("server.titles", titled && measure["title"].toString() == "Measure the sound"
                                      && static_cast<bool>(measure["annotations"]["readOnlyHint"]) && !static_cast<bool>(write["annotations"]["readOnlyHint"]) ? 1 : 0, 1);
        }
        rep = s.request({ a.get() }, "tools/call", object({ { "name", "list_instances" }, { "arguments", juce::var(new juce::DynamicObject()) } }));
        std::printf("list_instances: %s", firstText(rep["result"]).c_str());
        P.eq("server.lists", firstText(rep["result"]).find(first) != std::string::npos ? 1 : 0, 1);
        rep = s.request({ a.get() }, "tools/call", object({ { "name", "get_status" }, { "arguments", juce::var(new juce::DynamicObject()) } }));
        std::printf("get_status: %s\n", firstText(rep["result"]).substr(0, 160).c_str());
        P.eq("server.routes", !static_cast<bool>(rep["result"]["isError"]) && !firstText(rep["result"]).empty() ? 1 : 0, 1);
        rep = s.request({ a.get() }, "tools/call", object({ { "name", "playground_guide" }, { "arguments", juce::var(new juce::DynamicObject()) } }));
        P.eq("server.guide", firstText(rep["result"]) == guide ? 1 : 0, 1);
        rep = s.request({ a.get() }, "tools/call", object({ { "name", "playground_guide" }, { "arguments", object({ { "part", "interface" } }) } }));
        P.eq("server.guide_part", firstText(rep["result"]) == interfacePart ? 1 : 0, 1);
        P.eq("server.connected", a->editorPort().desktopConnected() && a->editorPort().desktopName() == first ? 1 : 0, 1);
        rep = s.request({ a.get() }, "prompts/list", {});
        bool fixListed = false;
        if (const juce::Array<juce::var>* ps = rep["result"]["prompts"].getArray())
            for (const juce::var& pr : *ps)
                fixListed = fixListed || (pr["name"].toString() == "fix_what_i_hear" && pr["title"].toString() == "Fix what I hear");
        P.eq("server.prompts", rep["result"]["prompts"].size() == 4 && rep["result"]["prompts"][0]["title"].toString().isNotEmpty() && fixListed ? 1 : 0, 1);
        rep = s.request({ a.get() }, "prompts/get", object({ { "name", "make_a_sound" }, { "arguments", object({ { "sound", "a warm pad" } }) } }));
        P.eq("server.prompt_filled", rep["result"]["messages"][0]["content"]["text"].toString().contains("a warm pad") ? 1 : 0, 1);
        {
            rep = s.request({ a.get() }, "prompts/get", object({ { "name", "fix_what_i_hear" },
                                                               { "arguments", object({ { "what", "a click on fast notes" }, { "when", "in the chorus" } }) } }));
            const std::string asked = rep["result"]["messages"][0]["content"]["text"].toString().toStdString();
            const std::size_t mark = asked.find("(What the plugin reports now:)");
            const std::string words = asked.substr(0, mark);
            bool namesTool = false;
            if (const juce::Array<juce::var>* t = tools.getArray())
                for (const juce::var& tool : *t)
                    namesTool = namesTool || has(words, tool["name"].toString().toStdString());
            std::printf("fix_what_i_hear:\n%s\n", asked.substr(0, 1200).c_str());
            P.eq("server.prompt_fix", has(words, "a click on fast notes") && has(words, "in the chorus") && !namesTool && !has(words, "_")
                                          && mark != std::string::npos && has(asked.substr(mark), "Dials:") ? 1 : 0, 1);
            rep = s.request({ a.get() }, "prompts/get", object({ { "name", "improve_the_sound" }, { "arguments", juce::var(new juce::DynamicObject()) } }));
            P.eq("server.prompt_state", rep["result"]["messages"][0]["content"]["text"].toString().contains("in my song")
                                            && rep["result"]["messages"][0]["content"]["text"].toString().contains("(What the plugin reports now:)") ? 1 : 0, 1);
        }

        // ---- DEADLINE: a server whose calls may take 1.5 s ------------------------------------------------------------
        {
            const std::string var2 = std::string(pg::product::kEnvPrefix) + "MCP_DEADLINE_MS";
            setenv(var2.c_str(), "1500", 1);
            Server d;
            const bool dstarted = d.start(serverPath);
            unsetenv(var2.c_str());
            if (dstarted)
            {
                const std::vector<pg::Processor*> ps{ a.get() };
                rep = d.request(ps, "initialize", object({ { "protocolVersion", "2025-11-25" }, { "capabilities", juce::var(new juce::DynamicObject()) },
                                                          { "clientInfo", object({ { "name", "probe" }, { "version", "1" } }) } }));
                P.eq("server.protocol_2025_11_25", rep["result"]["protocolVersion"].toString() == "2025-11-25" ? 1 : 0, 1);
                d.request(ps, "tools/call", object({ { "name", "write_file" },
                                                    { "arguments", object({ { "name", "program.fm" }, { "content", juce::String(slowProgram()) } }) } }));
                const auto t0 = std::chrono::steady_clock::now();
                const int tuneId = d.post("tools/call", tuneCall(60.0));
                waitActivity(*a, "Tuning", 20000);
                const juce::var listed = d.request(ps, "tools/call", object({ { "name", "list_instances" }, { "arguments", juce::var(new juce::DynamicObject()) } }));
                const juce::var statusNow = d.request(ps, "tools/call", object({ { "name", "get_status" }, { "arguments", juce::var(new juce::DynamicObject()) } }));
                const bool beside = d.early.count(tuneId) == 0 && has(firstText(listed["result"]), first) && has(firstText(statusNow["result"]), "Dials:");
                const juce::var late = d.wait(ps, tuneId, 30000);
                const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                std::printf("past the deadline (%.1f s): %s\n", secs, firstText(late["result"]).c_str());
                const std::vector<int> after = d.drain(ps, 1500);
                waitIdle(*a, 10000);
                P.eq("deadline.beside", beside ? 1 : 0, 1);
                P.eq("deadline.answered", static_cast<bool>(late["result"]["isError"]) && has(firstText(late["result"]), "Still running")
                                              && secs < 10.0 ? 1 : 0, 1);
                P.eq("deadline.once", std::count(after.begin(), after.end(), tuneId) == 0 ? 1 : 0, 1);
                P.eq("deadline.dials_kept", dialNow(*a, "0.25") ? 1 : 0, 1);
                rep = d.request(ps, "tools/call", object({ { "name", "write_file" },
                                                          { "arguments", object({ { "name", "program.fm" }, { "content", juce::String(text) } }) } }));
                P.eq("deadline.still_reached", !static_cast<bool>(rep["result"]["isError"]) && a->editorPort().draft().files.front().text == text ? 1 : 0, 1);
                P.eq("deadline.whole_lines", d.lines > 0 && d.whole == d.lines ? 1 : 0, 1);
                d.stop();
            }
        }

        // ---- STUCK: an instance that never reads ---------------------------------------------------------------------
        {
            // A folder of its own, with one "instance": a socket nobody accepts on (it takes 8 KB, then nothing).
            const std::string stuckDir = dir + "-stuck";
            mkdir(stuckDir.c_str(), 0700);
            const int deaf = pg::mcp::listenAt(stuckDir + "/1.sock");
            const std::string var2 = std::string(pg::product::kEnvPrefix) + "MCP_DEADLINE_MS";
            setenv(var.c_str(), stuckDir.c_str(), 1);
            setenv(var2.c_str(), "1500", 1);
            Server k;
            const bool kstarted = deaf >= 0 && k.start(serverPath);
            setenv(var.c_str(), dir.c_str(), 1);
            unsetenv(var2.c_str());
            if (kstarted)
            {
                k.request({}, "initialize", object({ { "protocolVersion", "2025-11-25" } }));
                const int callId = k.post("tools/call", object({ { "name", "write_file" },
                                                                 { "arguments", object({ { "name", "big.fm" },
                                                                                       { "content", juce::String::repeatedString("x", 256 * 1024) } }) } }));
                const juce::var pong = k.wait({}, k.post("ping", juce::var(new juce::DynamicObject())), 5000);
                const bool answered = pong.hasProperty("result");
                const juce::var late = answered ? k.wait({}, callId, 10000) : juce::var();
                std::printf("stuck instance: ping %s; the call: %s\n", answered ? "answered" : "NOT answered",
                            firstText(late["result"]).substr(0, 80).c_str());
                P.eq("stuck.ping_answered", answered ? 1 : 0, 1);
                P.eq("stuck.call_deadline", static_cast<bool>(late["result"]["isError"]) && has(firstText(late["result"]), "Still running") ? 1 : 0, 1);
                if (!answered)
                    k.kill();
                k.stop();
            }
            if (deaf >= 0)
                close(deaf);
            juce::File(juce::String::fromUTF8(stuckDir.c_str())).deleteRecursively();
        }

        // ---- MATCH: a second instance, on a track, with a project ---------------------------------------------------
        auto b = std::make_unique<pg::Processor>();
        const std::string second = std::string(kInstrument ? "Synth" : "Effect") + " 2";
        P.eq("names.second", b->mcpLink() != nullptr && b->mcpLink()->name() == second ? 1 : 0, 1);
        juce::AudioProcessor::TrackProperties track;
        track.name = "Bass Line";
        b->updateTrackProperties(track);
        {
            std::string why;
            pg::SavedVersion saved;
            b->editorPort().saveVersion("Moog Bass", saved, why);
        }
        const auto writeTo = [&](const char* instance, const char* file) {
            return s.request({ a.get(), b.get() }, "tools/call",
                             object({ { "name", "write_file" },
                                      { "arguments", object({ { "name", file }, { "content", "// a file\n" }, { "instance", instance } }) } }))["result"];
        };
        const auto hasFile = [](pg::Processor& p, const char* file) {
            for (const pg::SourceFile& f : p.editorPort().draft().files)
                if (f.name == file)
                    return true;
            return false;
        };
        juce::var res2 = writeTo(second.c_str(), "byname.fm");
        P.eq("match.name", !static_cast<bool>(res2["isError"]) && hasFile(*b, "byname.fm") && !hasFile(*a, "byname.fm") ? 1 : 0, 1);
        res2 = writeTo("2", "bynumber.fm");
        P.eq("match.number", hasFile(*b, "bynumber.fm") ? 1 : 0, 1);
        res2 = writeTo("bass line", "bytrack.fm");
        P.eq("match.track", hasFile(*b, "bytrack.fm") ? 1 : 0, 1);
        res2 = writeTo("the moog one", "byproject.fm");
        std::printf("by project: %s\n", firstText(res2).substr(0, 200).c_str());
        P.eq("match.project_part", hasFile(*b, "byproject.fm") || hasFile(*b, "byproject.fm") ? 1 : 0, 1);
        res2 = writeTo("the drums", "nowhere.fm");
        std::printf("no match: %s\n", firstText(res2).c_str());
        P.eq("match.none_asks", static_cast<bool>(res2["isError"]) && firstText(res2).find("Ask the user") != std::string::npos
                                    && firstText(res2).find("Bass Line") != std::string::npos ? 1 : 0, 1);
        rep = s.request({ a.get(), b.get() }, "tools/call", object({ { "name", "list_instances" }, { "arguments", juce::var(new juce::DynamicObject()) } }));
        std::printf("list_instances: %s", firstText(rep["result"]).c_str());
        P.eq("server.lists_two", firstText(rep["result"]).rfind(std::string("2 ") + (kInstrument ? "synths" : "effects") + " are open", 0) == 0
                                     && firstText(rep["result"]).find("Moog Bass") != std::string::npos ? 1 : 0, 1);
        {
            // A server just started, two open and none named: it asks which.
            Server fresh;
            if (serverPath != nullptr && fresh.start(serverPath))
            {
                fresh.request({ a.get(), b.get() }, "initialize", object({ { "protocolVersion", "2025-06-18" } }));
                const juce::var r2 = fresh.request({ a.get(), b.get() }, "tools/call", object({ { "name", "get_status" }, { "arguments", juce::var(new juce::DynamicObject()) } }))["result"];
                std::printf("several open: %s\n", firstText(r2).c_str());
                P.eq("match.several_ask", static_cast<bool>(r2["isError"]) && firstText(r2).find("Ask the user which one") != std::string::npos ? 1 : 0, 1);
                fresh.stop();
            }
        }

        // ---- GONE ------------------------------------------------------------------------------------------------------
        const std::string bsock = b->mcpLink() != nullptr ? b->mcpLink()->socketPath() : "";
        b.reset();
        P.eq("gone.socket", !juce::File(juce::String::fromUTF8(bsock.c_str())).exists() ? 1 : 0, 1);
        rep = s.request({ a.get() }, "tools/call", object({ { "name", "list_instances" }, { "arguments", juce::var(new juce::DynamicObject()) } }));
        P.eq("gone.one_left", firstText(rep["result"]).rfind(std::string("1 ") + (kInstrument ? "synth" : "effect") + " is open", 0) == 0 ? 1 : 0, 1);
        a.reset();
        rep = s.request({}, "tools/call", object({ { "name", "get_status" }, { "arguments", juce::var(new juce::DynamicObject()) } }));
        std::printf("none open: %s\n", firstText(rep["result"]).c_str());
        P.eq("gone.says_open_it", static_cast<bool>(rep["result"]["isError"]) && firstText(rep["result"]).find("open it") != std::string::npos ? 1 : 0, 1);
        rep = s.request({}, "tools/list", {});
        P.eq("gone.tools_cached", hasTool(rep["result"]["tools"], "write_file") ? 1 : 0, 1);
        rep = s.request({}, "tools/call", object({ { "name", "playground_guide" }, { "arguments", juce::var(new juce::DynamicObject()) } }));
        const juce::var cachedPart = s.request({}, "tools/call", object({ { "name", "playground_guide" }, { "arguments", object({ { "part", "interface" } }) } }));
        const juce::var cachedCode = s.request({}, "tools/call", object({ { "name", "playground_guide" },
                                                                          { "arguments", object({ { "part", juce::String("library:") + pg::kLibrary[0].name + ":code" } }) } }));
        P.eq("gone.guide_cached", firstText(rep["result"]) == guide && firstText(cachedPart["result"]) == interfacePart
                                      && has(firstText(cachedCode["result"]), pg::kLibrary[0].source) ? 1 : 0, 1);
        s.stop();
    }
    {
        // The server gone: a processor no longer counts as connected (its link saw the connection close).
        auto c = std::make_unique<pg::Processor>();
        Server t;
        if (serverPath != nullptr && t.start(serverPath))
        {
            t.request({ c.get() }, "tools/call", object({ { "name", "list_instances" }, { "arguments", juce::var(new juce::DynamicObject()) } }));
            const bool during = c->editorPort().desktopConnected();
            t.stop();
            bool after = true;
            for (int k = 0; k < 100 && after; ++k)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                after = c->editorPort().desktopConnected();
            }
            P.eq("gone.disconnected", during && !after ? 1 : 0, 1);
        }
    }
    a.reset();
    folder.deleteRecursively();
    return P.finish();
}
