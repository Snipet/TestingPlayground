// Source/mcp/Main.cpp: <plugin>MCP, this playground's MCP server, for Claude Desktop (or any MCP client) to act as the
// AI in the plugin: JSON-RPC 2.0 a line at a time on stdin and stdout (MCP's stdio transport), the plugin's instances
// reached through their sockets (mcp/McpProtocol.h, plugin/McpLink.h).
//
// It is made for someone who never sees a tool's name: the instructions tell Claude to talk about sounds, tracks and
// projects, never ids or tools; each tool has a title in words; an instance is named as the user would name it.
//   tools/list   list_instances ("Find the open synths"), playground_guide ("Read the guide", in parts: mcp/McpGuide.h),
//                and the chat's own tools (ChatAgent::mcpTools, each with its title) from an instance running, else
//                from the tools.json the last one left; each takes `instance`
//   tools/call   any of the chat's tools goes to the instance named: by its name ("Synth 2", or just "2"), its
//                track's, or its project's (whole or in part, case aside); else to the one used last; else to the only
//                one open. With several open and none named, the reply describes them so that Claude asks the user
//   prompts      Make a sound, Improve the sound, Explain the sound, Fix what I hear: requests the user picks in Claude
//                Desktop (one whose tools the plugin lacks is not listed); those about what plays carry the plugin's
//                state, so Claude starts from it
//
// One thread, one loop: poll() over stdin and every instance's socket. A request from the client never waits for an
// instance: what it asks of one is sent with a deadline and a continuation, and its answer is written when the
// instance's reply comes, so a long call (tune_dials) never holds up another request, a quick read (list_instances,
// get_status) answers meanwhile, and replies go out in the order they are ready, each a whole line, from this one
// writer. A call that passes its deadline (kCallMs, under Claude Desktop's own limit) is answered then ("still running;
// asked the plugin to stop") and the plugin is asked to stop it; its late reply is dropped, as is the reply to a request
// the client cancelled (notifications/cancelled, passed on to the plugin as a cancel): never two answers to one id.
// A slow instance is never dropped: only one whose socket closes. Nor does the loop ever wait to write to one: the
// sockets are non-blocking, what an instance has not taken yet waits in its outbox (sent on POLLOUT), and the loop
// keeps reading meanwhile, so a large request and a large reply crossing each other cannot leave the server and the
// plugin each waiting for the other to read.
//
// stdout carries the protocol only; what went wrong, and what the client is, go to stderr (Claude Desktop keeps it in
// its MCP logs).
#include "mcp/McpGuide.h"
#include "mcp/McpProtocol.h"
#include "sandbox/SandboxProtocol.h"

#include "Library.h"
#include "Product.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>

extern char** environ;

namespace pg = playground;

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr int kQuickMs = 5000;                      // info, tools, guide, a prompt's state: answered beside a call
    constexpr int kCallMs = 200000;                     // a tool call: under Claude Desktop's 240 s (its docs), above
                                                        // tune_dials' own limit (170 s at most)
    constexpr std::size_t kPictureBytes = 75000;         // a picture's bytes at most in a result: ChatAgent::kMcpPictureBytes's
                                                        // own copy (100,000 base64 characters), the same rule
    constexpr bool kInstrument = pg::product::kIsInstrument;
    const char* const kThing = kInstrument ? "synth" : "effect";          // what the user calls it
    const char* const kThings = kInstrument ? "synths" : "effects";

    juce::var object(std::initializer_list<std::pair<const char*, juce::var>> props)
    {
        auto* o = new juce::DynamicObject();
        for (const auto& [k, v] : props)
            o->setProperty(k, v);
        return juce::var(o);
    }

    juce::String str(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

    juce::var textContent(const std::string& text, bool error = false)
    {
        juce::Array<juce::var> content;
        content.add(object({ { "type", "text" }, { "text", str(text) } }));
        return object({ { "content", content }, { "isError", error } });
    }

    // The text of a tool result's first text block.
    std::string firstText(const juce::var& result)
    {
        if (const juce::Array<juce::var>* c = result["content"].getArray())
            for (const juce::var& b : *c)
                if (b["type"].toString() == "text")
                    return b["text"].toString().toStdString();
        return {};
    }

    std::string lower(std::string s)
    {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // A JSON-RPC id as a key (a number and a string are different ids).
    std::string key(const juce::var& id) { return juce::JSON::toString(id, true).toStdString(); }

    // Whether the other end of `fd` is still there (a peer gone reads as end of file).
    bool alive(int fd)
    {
        pollfd p{ fd, POLLIN, 0 };
        if (poll(&p, 1, 0) < 0)
            return false;
        if ((p.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
            return false;
        if ((p.revents & POLLIN) != 0)
        {
            char c = 0;
            return recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT) != 0;
        }
        return true;
    }

    // `line` and a line feed on stdout, all of it (the one writer).
    void writeOut(const std::string& line)
    {
        const std::string out = line + "\n";
        std::size_t sent = 0;
        while (sent < out.size())
        {
            const ssize_t n = write(1, out.data() + sent, out.size() - sent);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return;
            sent += static_cast<std::size_t>(n);
        }
    }

    // The deadline of a tool call, in ms: kCallMs, or <ENV_PREFIX>MCP_DEADLINE_MS (the probes' own, shorter).
    int callDeadlineMs()
    {
        const std::string var = std::string(pg::product::kEnvPrefix) + "MCP_DEADLINE_MS";
        if (const char* v = std::getenv(var.c_str()); v != nullptr && std::atoi(v) > 0)
            return std::atoi(v);
        return kCallMs;
    }

    // A sandbox request's deadline, in ms: sandbox::kDeadlineMs, or <ENV_PREFIX>SANDBOX_DEADLINE_MS (the probes').
    int sandboxDeadlineMs()
    {
        const std::string var = std::string(pg::product::kEnvPrefix) + "SANDBOX_DEADLINE_MS";
        if (const char* v = std::getenv(var.c_str()); v != nullptr && std::atoi(v) > 0)
            return std::atoi(v);
        return pg::sandbox::kDeadlineMs;
    }

    // What of `pending` the non-blocking pipe `fd` takes now, written and removed from it (the rest waits for POLLOUT);
    // false when its reader is gone.
    bool flushPipe(int fd, std::string& pending)
    {
        std::size_t sent = 0;
        bool open = true;
        while (sent < pending.size())
        {
            const ssize_t n = write(fd, pending.data() + sent, pending.size() - sent);
            if (n > 0)
            {
                sent += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            open = false;
            sent = pending.size();
        }
        pending.erase(0, sent);
        return open;
    }

    // Lines added and removed between two texts, as a multiset of lines (what a summary says).
    std::pair<int, int> lineChanges(const std::string& before, const std::string& after)
    {
        const auto lines = [](const std::string& t) {
            std::vector<std::string> out;
            std::size_t from = 0;
            while (from < t.size())
            {
                const std::size_t nl = t.find('\n', from);
                out.push_back(t.substr(from, nl == std::string::npos ? std::string::npos : nl - from));
                if (nl == std::string::npos)
                    break;
                from = nl + 1;
            }
            return out;
        };
        std::map<std::string, int> count;
        for (const std::string& l : lines(before))
            ++count[l];
        int added = 0, removed = 0;
        for (const std::string& l : lines(after))
            if (auto it = count.find(l); it != count.end() && it->second > 0)
                --it->second;
            else
                ++added;
        for (const auto& entry : count)
            removed += entry.second;
        return { added, removed };
    }

    int lineCount(const std::string& t)
    {
        if (t.empty())
            return 0;
        return static_cast<int>(std::count(t.begin(), t.end(), '\n')) + (t.back() == '\n' ? 0 : 1);
    }

    // The files get_status lists with files "all" ("<file name=\"x\">\n...\n</file>\n"), in order.
    std::vector<std::pair<std::string, std::string>> filesIn(const std::string& status)
    {
        std::vector<std::pair<std::string, std::string>> out;
        const std::size_t list = status.find("The files:\n");
        if (list == std::string::npos)
            return out;
        for (std::size_t p = status.find("<file name=\"", list); p != std::string::npos; p = status.find("<file name=\"", p + 1))
        {
            const std::size_t q = status.find("\">\n", p);
            if (q == std::string::npos)
                break;
            const std::size_t end = status.find("\n</file>\n", q + 2);
            if (end == std::string::npos)
                break;
            out.push_back({ status.substr(p + 12, q - p - 12), status.substr(q + 3, end + 1 - (q + 3)) });
            p = end;
        }
        return out;
    }

    struct Instance
    {
        std::string id, path;
        int fd = -1;                                     // non-blocking
        uint64_t serial = 0;                             // its connection's number: the same id reconnected is another
        std::unique_ptr<pg::mcp::LineReader> reader;
        std::string out;                                 // requests its socket has not taken yet, whole lines, in order
        int next = 1;
    };

    // What the user knows an instance by: its name, its track, its project, where it runs.
    struct Seen
    {
        std::string id, name, track, project, host, preset, files;
        int version = 0;
        bool edited = false, compiled = true, busy = false;

        std::string describe() const
        {
            std::string s = name;
            if (!track.empty())
                s += " on the track \"" + track + "\"";
            if (!project.empty())
                s += std::string(track.empty() ? " with" : ",") + " the project \"" + project + "\" (version " + std::to_string(version)
                   + (preset.empty() ? std::string() : ", preset \"" + preset + "\"") + (edited ? ", changed since it was saved" : "") + ")";
            else
                s += ", no project saved yet";
            s += host.empty() || host == "Unknown" ? std::string(", in its own window or a host") : ", in " + host;
            return s;
        }
    };

    // How a request to an instance ended.
    enum class Got
    {
        result,                                          // its reply's result
        error,                                           // its reply's error (why)
        late,                                            // its deadline passed
        closed                                           // the instance is gone
    };

    using Done = std::function<void(Got, const juce::var& result, const std::string& why)>;
    using Then = std::function<void(const juce::var&)>;

    // A request sent to an instance, waiting for its reply.
    struct Ask
    {
        std::string instance;
        int number = 0;                                  // its id on the instance's socket
        Clock::time_point deadline;
        bool call = false;                               // a tool call: past its deadline, the instance is asked to stop it
        std::string client;                              // the client's request it answers (key(id); "" for none)
        Done done;
    };

    // A prompt: its name and title, whether it carries the plugin's state, and the plugin's tools it needs (it is not
    // listed without them). Its words are promptList's and prompt's.
    struct PromptInfo
    {
        const char* name;
        const char* title;
        bool state;                                      // its message carries the plugin's state (get_status)
        std::vector<const char*> needs;
    };

    const std::vector<PromptInfo>& prompts()
    {
        static const std::vector<PromptInfo> all{
            { "make_a_sound", kInstrument ? "Make a sound" : "Make an effect", false, { "write_file", "run_program" } },
            { "improve_the_sound", "Improve the sound", true, { "get_status", "measure_output", "edit_file", "run_program" } },
            { "explain_the_sound", "Explain the sound", true, { "get_status", "read_file", "measure_output" } },
            { "fix_what_i_hear", "Fix what I hear", true,
              { "get_status", "listen", "capture", "trace", "compare", "measure_output", "view_sound", "edit_file", "run_program",
                "set_checks" } },
        };
        return all;
    }

    class Server
    {
    public:
        int run()
        {
            pg::mcp::LineReader in(0);
            for (;;)
            {
                std::vector<pollfd> fds;
                std::vector<std::pair<std::string, uint64_t>> order;   // each instance polled: its id and connection
                fds.push_back({ 0, POLLIN, 0 });
                for (const auto& entry : instances_)
                {
                    const short events = static_cast<short>(POLLIN | (entry.second.out.empty() ? 0 : POLLOUT));
                    fds.push_back({ entry.second.fd, events, 0 });
                    order.push_back({ entry.first, entry.second.serial });
                }
                // The sandbox's helper: its replies, and room for what it has not taken yet.
                std::size_t sbRead = 0, sbWrite = 0;
                if (sbOut_ >= 0)
                {
                    fds.push_back({ sbOut_, POLLIN, 0 });
                    sbRead = fds.size() - 1;
                }
                if (sbIn_ >= 0 && !sbOutbox_.empty())
                {
                    fds.push_back({ sbIn_, POLLOUT, 0 });
                    sbWrite = fds.size() - 1;
                }
                const uint64_t helper = sbSerial_;
                const int r = poll(fds.data(), static_cast<nfds_t>(fds.size()), msToNextDeadline());
                if (r < 0)
                {
                    if (errno == EINTR)
                        continue;
                    break;
                }
                if (fds[0].revents != 0)
                {
                    if (!in.readSome())
                        break;                           // the client has gone
                    std::string line;
                    while (in.take(line))
                        onClientLine(line);
                }
                for (std::size_t k = 0; k < order.size(); ++k)
                    if (fds[k + 1].revents != 0)
                        onInstance(order[k].first, order[k].second, fds[k + 1].revents);
                if (sbWrite != 0 && fds[sbWrite].revents != 0 && helper == sbSerial_ && sbIn_ >= 0 && !flushPipe(sbIn_, sbOutbox_))
                    helperGone("stopped reading");
                if (sbRead != 0 && fds[sbRead].revents != 0 && helper == sbSerial_)
                    onHelper();
                expire();
            }
            // Nobody waits for what is still running: the plugin need not finish it (a cancel the socket cannot take now
            // is lost, but the plugin cancels a closed connection's calls anyway).
            for (const Ask& a : asks_)
                if (a.call)
                    stop(a.instance, a.number);
            for (auto& entry : instances_)
                close(entry.second.fd);
            // The sandbox's helper reads the end of its stdin and quits by itself.
            if (sbIn_ >= 0)
                close(sbIn_);
            if (sbOut_ >= 0)
                close(sbOut_);
            return 0;
        }

    private:
        // ---- the client ---------------------------------------------------------------------------------------------
        void send(const juce::var& msg) { writeOut(juce::JSON::toString(msg, true).toStdString()); }

        // The answer to the client's request `id`: once, and only while it is open (not answered, not cancelled).
        void reply(const juce::var& id, const juce::var& result)
        {
            if (open_.erase(key(id)) == 0)
                return;
            send(object({ { "jsonrpc", "2.0" }, { "id", id }, { "result", result } }));
        }

        void fail(const juce::var& id, int code, const std::string& message)
        {
            if (open_.erase(key(id)) == 0)
                return;
            send(object({ { "jsonrpc", "2.0" }, { "id", id }, { "error", object({ { "code", code }, { "message", str(message) } }) } }));
        }

        bool stillOpen(const juce::var& id) const { return open_.count(key(id)) != 0; }

        void onClientLine(std::string line)
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                return;
            const juce::var msg = juce::JSON::parse(str(line));
            if (!msg.isObject())
            {
                send(object({ { "jsonrpc", "2.0" }, { "id", juce::var() },
                              { "error", object({ { "code", -32700 }, { "message", "Parse error" } }) } }));
                return;
            }
            handle(msg);
        }

        void handle(const juce::var& msg)
        {
            const juce::String method = msg["method"].toString();
            const juce::DynamicObject* o = msg.getDynamicObject();
            if (o == nullptr || !o->hasProperty("id"))
            {
                // A notification: initialized; cancelled, which goes on to the plugin.
                if (method == "notifications/cancelled")
                    cancelled(msg["params"]["requestId"], msg["params"]["reason"].toString().toStdString());
                return;
            }
            const juce::var id = msg["id"];
            const juce::var params = msg["params"];
            open_.insert(key(id));
            if (method == "initialize")
                reply(id, initialize(params));
            else if (method == "ping")
                reply(id, juce::var(new juce::DynamicObject()));
            else if (method == "tools/list")
                withTools([this, id](const juce::var& tools) { reply(id, object({ { "tools", toolList(tools) } })); });
            else if (method == "tools/call")
                call(id, params);
            else if (method == "prompts/list")
                withTools([this, id](const juce::var& tools) { reply(id, object({ { "prompts", promptList(tools) } })); });
            else if (method == "prompts/get")
                prompt(id, params);
            else if (method == "resources/list")
                reply(id, object({ { "resources", juce::Array<juce::var>() } }));
            else
                fail(id, -32601, "Method not found: " + method.toStdString());
            // A first instance since the tools were listed: say the list changed (a client may list them again).
            if (method != "tools/list" && listedPlain_)
            {
                discover();
                if (!instances_.empty())
                {
                    listedPlain_ = false;
                    send(object({ { "jsonrpc", "2.0" }, { "method", "notifications/tools/list_changed" } }));
                }
            }
        }

        // The client gave up on request `id`: no answer goes to it, and the plugin is asked to stop what it does for it.
        void cancelled(const juce::var& id, const std::string& reason)
        {
            if (open_.erase(key(id)) == 0)
                return;
            std::cerr << "cancelled: request " << key(id) << (reason.empty() ? std::string() : " (" + reason + ")") << std::endl;
            for (auto it = asks_.begin(); it != asks_.end();)
                if (it->client == key(id))
                {
                    if (it->call)
                        stop(it->instance, it->number);
                    it = asks_.erase(it);                // its reply, when it comes, is dropped
                }
                else
                    ++it;
        }

        juce::var initialize(const juce::var& params)
        {
            juce::String version = params["protocolVersion"].toString();
            std::cerr << "initialize: protocol " << version.toStdString() << ", client "
                      << juce::JSON::toString(params["clientInfo"], true).toStdString() << ", capabilities "
                      << juce::JSON::toString(params["capabilities"], true).toStdString() << std::endl;
            if (version != "2024-11-05" && version != "2025-03-26" && version != "2025-06-18" && version != "2025-11-25")
                version = "2025-11-25";
            const std::string thing = kThing;
            const std::string instructions =
                std::string(pg::product::kName) + " is " + (kInstrument ? "a synth" : "an effect")
                + " plugin whose sound is a short program in FMinus, a small language it compiles as you write. With it open "
                  "in the user's DAW (or as its own app), these tools let you make and change its sound as the plugin's "
                  "own chat would: write the program, run it, measure it, look at it, and set its dials.\n"
                  "The person you talk to is a musician, not a programmer of this plugin: never ask them about tools, ids or "
                  "file names, and never tell them to call a tool. Talk about the sound, their tracks and their projects. "
                  "When they ask for a sound, just make it.\n"
                  "Before you first write or change code in a conversation, read playground_guide once (its start, the "
                  "default): it teaches FMinus and how the plugin works, and it ends by naming its other parts. Read the "
                  "part it names before you write a panel (\"interface\") or use a building block (\"library\"). Then check "
                  "the plugin's state, write the program, run it, measure it and look at it, fix what is wrong, and leave "
                  "its dials at good settings. Tell the user what you made in a few plain sentences, and what to play to "
                  "hear it.\n"
                  "Each open " + thing + " has a name the plugin shows (\"" + (kInstrument ? "Synth" : "Effect") + " 1\", \""
                + (kInstrument ? "Synth" : "Effect") + " 2\"). When only one is open, use it. When several are, work out from "
                  "what the user said which one they mean (its track, its project, its name) and pass that as `instance`; if "
                  "you cannot tell, ask them in their words, describing each by its track and project.\n"
                  "To design or rework a panel (the plugin's interface, panel.gui), with or without the plugin open, use "
                  "gui_sandbox (read the guide's \"interface\" part first): it draws the panel as the plugin would and never "
                  "touches what the plugin plays or shows; apply its design to the plugin only when the user asks.";
            return object({ { "protocolVersion", version },
                            { "capabilities", object({ { "tools", object({ { "listChanged", true } }) },
                                                       { "prompts", object({ { "listChanged", false } }) } }) },
                            { "serverInfo", object({ { "name", pg::product::kName }, { "title", pg::product::kName }, { "version", pg::product::kVersion } }) },
                            { "instructions", str(instructions) } });
        }

        // ---- the instances ----------------------------------------------------------------------------------------------
        void discover()
        {
            const std::string dir = pg::mcp::folder();
            if (dir.empty())
                return;
            // The sockets, by readdir: a socket is no regular file to JUCE's listing.
            std::vector<std::string> socks;
            if (DIR* d = opendir(dir.c_str()))
            {
                while (const dirent* e = readdir(d))
                {
                    const std::string n = e->d_name;
                    if (n.size() > 5 && n.compare(n.size() - 5, 5, ".sock") == 0)
                        socks.push_back(n.substr(0, n.size() - 5));
                }
                closedir(d);
            }
            // A connection whose instance went away goes (its number may already be another's). Dropping one runs what
            // waited for it, which may ask, and so drop, another first.
            for (const std::string& id : ids())
                if (const auto it = instances_.find(id); it != instances_.end() && !alive(it->second.fd))
                    drop(id);
            std::map<std::string, bool> present;
            for (const std::string& id : socks)
            {
                present[id] = true;
                if (instances_.count(id) != 0)
                    continue;
                const std::string path = dir + "/" + id + ".sock";
                const int fd = pg::mcp::connectTo(path);
                if (fd < 0)
                {
                    if (errno == ECONNREFUSED)
                        unlink(path.c_str());            // nothing listens: its instance is gone (a crash, a host killed)
                    continue;
                }
                if (!pg::mcp::nonBlocking(fd))
                {
                    close(fd);
                    continue;
                }
                Instance in;
                in.id = id;
                in.path = path;
                in.fd = fd;
                in.serial = ++serials_;
                in.reader = std::make_unique<pg::mcp::LineReader>(fd);
                instances_.emplace(id, std::move(in));
            }
            for (const std::string& id : ids())
                if (present.count(id) == 0)
                    drop(id);
        }

        // The instances' ids, a copy (asking one may drop it).
        std::vector<std::string> ids() const
        {
            std::vector<std::string> out;
            for (const auto& entry : instances_)
                out.push_back(entry.first);
            return out;
        }

        // An instance gone: its connection closed, and what waited for it told so.
        void drop(const std::string& id)
        {
            const auto it = instances_.find(id);
            if (it == instances_.end())
                return;
            close(it->second.fd);
            instances_.erase(it);
            std::vector<Ask> lost;
            for (auto a = asks_.begin(); a != asks_.end();)
                if (a->instance == id)
                {
                    lost.push_back(std::move(*a));
                    a = asks_.erase(a);
                }
                else
                    ++a;
            for (Ask& a : lost)
                a.done(Got::closed, juce::var(), "it has closed");
        }

        // A request to instance `in`: into its outbox, and sent as far as its socket takes it now (the loop sends the
        // rest). It never waits, and never drops the instance (a socket gone is the loop's to find): its number.
        static int post(Instance& in, const char* method, const juce::var& params)
        {
            const int n = in.next++;
            in.out += juce::JSON::toString(object({ { "id", n }, { "method", method }, { "params", params } }), true).toStdString() + "\n";
            pg::mcp::flush(in.fd, in.out);
            return n;
        }

        // Request `method` of instance `id`, its answer to `done` when its reply comes (or it fails, or `ms` pass). `call`:
        // a tool call, which the instance is asked to stop past its deadline; `client`, the client's request it serves.
        void ask(const std::string& id, const char* method, const juce::var& params, int ms, bool call, const std::string& client, Done done)
        {
            const auto it = instances_.find(id);
            if (it == instances_.end())
            {
                done(Got::closed, juce::var(), "it is no longer open");
                return;
            }
            const int n = post(it->second, method, params);
            asks_.push_back({ id, n, Clock::now() + std::chrono::milliseconds(ms), call, client, std::move(done) });
        }

        // Ask instance `id` to stop the call it was sent as request `number` (its reply, if any, is dropped).
        void stop(const std::string& id, int number)
        {
            if (const auto it = instances_.find(id); it != instances_.end())
                post(it->second, "cancel", object({ { "id", number } }));
        }

        // Instance `id`'s socket has news (`revents`), if it is still connection `serial`: room for its outbox, its
        // replies, or its end.
        void onInstance(const std::string& id, uint64_t serial, short revents)
        {
            {
                const auto it = instances_.find(id);
                if (it == instances_.end() || it->second.serial != serial)
                    return;                              // dropped (or replaced) meanwhile
                if ((revents & POLLOUT) != 0 && !pg::mcp::flush(it->second.fd, it->second.out))
                {
                    drop(id);
                    return;
                }
                if ((revents & ~POLLOUT) == 0)
                    return;
                if (!it->second.reader->readSome())
                {
                    drop(id);
                    return;
                }
            }
            for (;;)
            {
                const auto it = instances_.find(id);
                if (it == instances_.end() || it->second.serial != serial)
                    return;
                std::string line;
                if (!it->second.reader->take(line))
                    return;
                const juce::var r = juce::JSON::parse(str(line));
                const int number = static_cast<int>(r["id"]);
                const auto a = std::find_if(asks_.begin(), asks_.end(), [&](const Ask& x) { return x.instance == id && x.number == number; });
                if (a == asks_.end())
                    continue;                            // a late reply (past its deadline, cancelled) or a cancel's
                Ask got = std::move(*a);
                asks_.erase(a);
                if (r.hasProperty("error"))
                    got.done(Got::error, juce::var(), r["error"].toString().toStdString());
                else
                    got.done(Got::result, r["result"], {});
            }
        }

        int msToNextDeadline() const
        {
            if (asks_.empty() && !sbBusy_)
                return -1;
            Clock::time_point first = sbBusy_ ? sbDeadline_ : asks_.front().deadline;
            for (const Ask& a : asks_)
                first = std::min(first, a.deadline);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(first - Clock::now()).count();
            return static_cast<int>(std::clamp<long long>(ms + 1, 0, 60000));
        }

        // The asks past their deadline: answered as late; a tool call's instance asked to stop it.
        void expire()
        {
            const Clock::time_point now = Clock::now();
            std::vector<Ask> late;
            for (auto a = asks_.begin(); a != asks_.end();)
                if (a->deadline <= now)
                {
                    late.push_back(std::move(*a));
                    a = asks_.erase(a);
                }
                else
                    ++a;
            for (Ask& a : late)
            {
                if (a.call)
                {
                    std::cerr << "deadline: request " << a.client << " still running on " << a.instance << ", asked to stop" << std::endl;
                    stop(a.instance, a.number);
                }
                a.done(Got::late, juce::var(), "it did not answer in time");
            }
            if (sbBusy_ && sbDeadline_ <= now)
            {
                std::cerr << "deadline: the sandbox's helper did not answer in " << sbMs_ << " ms: stopped" << std::endl;
                helperGone("did not answer in " + std::to_string((sbMs_ + 500) / 1000) + " s, so it was stopped", true);
            }
        }

        // The first instance whose reply to `method` passes `accept` (in id order), to `done`; Got::closed when none
        // does. An instance's error ends it (the same request is refused by every one).
        void firstOf(const char* method, const juce::var& params, std::function<bool(const juce::var&)> accept, Done done)
        {
            discover();
            tryEach(std::make_shared<std::vector<std::string>>(ids()), 0, method, params, std::move(accept), std::move(done));
        }

        void tryEach(std::shared_ptr<std::vector<std::string>> order, std::size_t k, const char* method, const juce::var& params,
                     std::function<bool(const juce::var&)> accept, Done done)
        {
            if (k >= order->size())
            {
                done(Got::closed, juce::var(), "none is open");
                return;
            }
            ask((*order)[k], method, params, kQuickMs, false, {},
                [this, order, k, method, params, accept, done](Got g, const juce::var& r, const std::string& why) {
                    if ((g == Got::result && accept(r)) || g == Got::error)
                        done(g, r, why);
                    else
                        tryEach(order, k + 1, method, params, accept, done);
                });
        }

        // What each open instance is, as the user knows it (those that answer), in id order.
        void gather(std::function<void(std::vector<Seen>)> then)
        {
            discover();
            const std::vector<std::string> all = ids();
            if (all.empty())
            {
                then({});
                return;
            }
            struct State
            {
                std::vector<std::optional<Seen>> got;
                std::size_t left = 0;
            };
            auto state = std::make_shared<State>();
            state->got.resize(all.size());
            state->left = all.size();
            for (std::size_t k = 0; k < all.size(); ++k)
                ask(all[k], "info", juce::var(), kQuickMs, false, {},
                    [state, k, id = all[k], then](Got g, const juce::var& i, const std::string&) {
                        if (g == Got::result)
                        {
                            Seen s;
                            s.id = id;
                            s.name = i["name"].toString().toStdString();
                            if (s.name.empty())
                                s.name = std::string(kInstrument ? "Synth " : "Effect ") + id;
                            s.track = i["track"].toString().toStdString();
                            s.project = i["project"].toString().toStdString();
                            s.host = i["host"].toString().toStdString();
                            s.preset = i["preset"].toString().toStdString();
                            s.version = static_cast<int>(i["version"]);
                            s.edited = static_cast<bool>(i["edited"]);
                            s.compiled = static_cast<bool>(i["compiled"]);
                            s.busy = static_cast<bool>(i["busy"]);
                            if (const juce::Array<juce::var>* f = i["files"].getArray())
                                for (const juce::var& v : *f)
                                    s.files += (s.files.empty() ? "" : ", ") + v.toString().toStdString();
                            state->got[k] = std::move(s);
                        }
                        if (--state->left == 0)
                        {
                            std::vector<Seen> out;
                            for (std::optional<Seen>& s : state->got)
                                if (s)
                                    out.push_back(std::move(*s));
                            then(std::move(out));
                        }
                    });
        }

        // Its words, lower case, but those that say nothing of which one ("the", "one", "track", "synth", ...).
        static std::vector<std::string> words(const std::string& text)
        {
            static constexpr const char* kFiller[] = { "the", "a", "an", "one", "on", "my", "track", "project", "synth", "effect",
                                                       "instance", "with", "of", "in", "that", "this", "called", "named", "plugin",
                                                       "playground", "use", "please", "it", "its", "for", "and" };
            std::vector<std::string> out;
            std::string w;
            const auto flush = [&] {
                if (!w.empty() && std::none_of(std::begin(kFiller), std::end(kFiller), [&](const char* f) { return w == f; }))
                    out.push_back(w);
                w.clear();
            };
            for (const char ch : text)
            {
                if (std::isalnum(static_cast<unsigned char>(ch)) != 0)
                    w += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                else
                    flush();
            }
            flush();
            return out;
        }

        // The instances `wanted` names: its id or its name ("2", "Synth 2"), its track's or its project's name, whole
        // (case aside); else those sharing the most words with it ("the moog one" for the project "Moog Bass").
        static std::vector<const Seen*> matching(const std::vector<Seen>& all, const std::string& wanted)
        {
            const std::string w = lower(juce::String(str(wanted)).trim().toStdString());
            std::vector<const Seen*> exact;
            for (const Seen& s : all)
                if (w == s.id || w == lower(s.name) || (!s.track.empty() && w == lower(s.track)) || (!s.project.empty() && w == lower(s.project)))
                    exact.push_back(&s);
            if (!exact.empty())
                return exact;
            const std::vector<std::string> asked = words(wanted);
            std::vector<const Seen*> best;
            int top = 0;
            for (const Seen& s : all)
            {
                int score = 0;
                for (const std::string& mine : words(s.name + " " + s.track + " " + s.project))
                    score += std::count(asked.begin(), asked.end(), mine) > 0 ? 1 : 0;
                if (score > top)
                {
                    top = score;
                    best.clear();
                }
                if (score == top && score > 0)
                    best.push_back(&s);
            }
            return best;
        }

        // ---- the tools -------------------------------------------------------------------------------------------------
        // The chat's tools: from an instance, else from the last one's tools.json; void when there are none to be had.
        void withTools(Then then)
        {
            firstOf("tools", juce::var(), [](const juce::var& t) { return t.isArray(); },
                    [then](Got g, const juce::var& tools, const std::string&) {
                        if (g == Got::result)
                        {
                            then(tools);
                            return;
                        }
                        const std::string dir = pg::mcp::folder();
                        const juce::var cached = dir.empty() ? juce::var() : juce::JSON::parse(juce::File(str(dir)).getChildFile("tools.json"));
                        then(cached.isArray() ? cached : juce::var());
                    });
        }

        static juce::var ownTool(const char* name, const std::string& title, const std::string& description, juce::var properties)
        {
            return object({ { "name", name },
                            { "title", str(title) },
                            { "description", str(description) },
                            { "inputSchema", object({ { "type", "object" }, { "properties", properties } }) },
                            { "annotations", object({ { "title", str(title) }, { "readOnlyHint", true }, { "openWorldHint", false } }) } });
        }

        juce::var toolList(const juce::var& tools)
        {
            juce::Array<juce::var> out;
            const std::string thing = kThing, things = kThings;
            out.add(ownTool("list_instances", "Find the open " + things,
                            "Which " + things + " (" + pg::product::kName + ") are open now: each one's name (as the plugin "
                            "shows it), its track, its project and where it runs. Use it when more than one may be open.",
                            juce::var(new juce::DynamicObject())));
            out.add(ownTool("playground_guide", "Read the guide",
                            "The guide to this plugin and to FMinus, the language its sound is written in, in parts. Read its "
                            "start once in a conversation, before you first write or change code; it names the other parts.",
                            object({ { "part", object({ { "type", "string" },
                                                        { "description", "The part: start (the default), large-program, "
                                                                         "interface, library, library:<file>, "
                                                                         "library:<file>:code, debugging, examples, "
                                                                         "projects or all" } }) } })));
            out.add(sandboxDefinition());
            listedPlain_ = tools.isVoid();
            if (const juce::Array<juce::var>* a = tools.getArray())
                for (const juce::var& t : *a)
                {
                    juce::var tool = juce::JSON::parse(juce::JSON::toString(t));   // a copy
                    juce::DynamicObject* schema = tool["inputSchema"].getDynamicObject();
                    if (schema != nullptr)
                    {
                        if (!schema->hasProperty("properties"))
                            schema->setProperty("properties", juce::var(new juce::DynamicObject()));
                        if (juce::DynamicObject* props = schema->getProperty("properties").getDynamicObject())
                            props->setProperty("instance",
                                               object({ { "type", "string" },
                                                        { "description", str("Which " + thing + " when several are open: its name (\""
                                                                             + (kInstrument ? "Synth" : "Effect") + " 2\"), track or "
                                                                             "project, in the user's words") } }));
                    }
                    out.add(tool);
                }
            return out;
        }

        // gui_sandbox: the server's own, as list_instances is (listed with no instance open, routed to none but for apply).
        // Its schema is lean: what each setting does in full is in the guide's interface part (ChatAgent::mcpGuide).
        static juce::var sandboxDefinition()
        {
            const auto prop = [](const char* type, const char* description) {
                return object({ { "type", type }, { "description", description } });
            };
            const auto choice = [](std::initializer_list<const char*> words, const char* description) {
                juce::Array<juce::var> e;
                for (const char* w : words)
                    e.add(w);
                return description == nullptr ? object({ { "type", "string" }, { "enum", e } })
                                              : object({ { "type", "string" }, { "enum", e }, { "description", description } });
            };
            juce::var edit = object({ { "type", "object" },
                                      { "properties", object({ { "old_text", object({ { "type", "string" } }) },
                                                               { "new_text", object({ { "type", "string" } }) } }) },
                                      { "required", juce::Array<juce::var>{ "old_text", "new_text" } } });
            const juce::var properties = object({
                { "action", choice({ "open", "edit", "render", "apply", "close" }, nullptr) },
                { "from", choice({ "blank", "instance", "file" }, "open: a blank panel, the open plugin's, or a file the user named (text alone: that .gui)") },
                { "path", prop("string", "open from file: a .gui file or a program's folder, as the user named it") },
                { "text", prop("string", "open, edit: the .gui's whole text") },
                { "edits", object({ { "type", "array" }, { "items", edit }, { "description", "edit: replacements in the .gui, each old_text found exactly once" } }) },
                { "dials", object({ { "type", "array" }, { "items", object({ { "type", "object" } }) },
                                    { "description", "open, edit: dials to mock: {name, range: [lo, hi], default, type, unit, options, style}" } }) },
                { "width", prop("integer", "render: the panel's width in px (853, the plugin's PANEL view)") },
                { "height", prop("integer", "render: its height in px (612)") },
                { "theme", choice({ "graphite", "paper" }, nullptr) },
                { "page", prop("string", "render: a tabs page to show, by its title") },
                { "values", object({ { "type", "object" }, { "description", "render: dials at these values, by name" } }) },
                { "detail", choice({ "high" }, "render: the picture whole; open, edit: the whole report") },
                { "instance", prop("string", "open from instance, apply: which plugin, when several are open") },
            });
            const std::string title = "Design a panel";
            return object({ { "name", "gui_sandbox" },
                            { "title", str(title) },
                            { "description", "A sandbox to design a plugin's interface (its panel.gui) and see it drawn exactly as the plugin "
                                             "draws it, with or without the plugin open, never touching what an open plugin plays or shows. "
                                             "open, then edit and render as often as needed; apply only when the user asks for the design to "
                                             "go into the plugin." },
                            { "inputSchema", object({ { "type", "object" }, { "properties", properties },
                                                      { "required", juce::Array<juce::var>{ "action" } } }) },
                            { "annotations", object({ { "title", str(title) }, { "readOnlyHint", false }, { "destructiveHint", false },
                                                      { "openWorldHint", false } }) } });
        }

        void call(const juce::var& id, const juce::var& params)
        {
            const std::string name = params["name"].toString().toStdString();
            juce::var args = params["arguments"].isObject() ? juce::JSON::parse(juce::JSON::toString(params["arguments"]))
                                                             : juce::var(new juce::DynamicObject());
            discover();
            if (name == "list_instances")
            {
                gather([this, id](std::vector<Seen> all) { reply(id, listInstances(all)); });
                return;
            }
            if (name == "playground_guide")
            {
                guide(args["part"].toString().toStdString(), [this, id](const juce::var& r) { reply(id, r); });
                return;
            }
            if (name == "gui_sandbox")
            {
                sandboxTool(id, args);
                return;
            }
            const std::string wanted = args["instance"].toString().toStdString();
            args.getDynamicObject()->removeProperty("instance");
            withInstance(id, wanted, [this, id, name, args](const std::string& instance) { callTool(id, instance, name, args); });
        }

        // The instance `wanted` names (its name, number, track or project), else the one used last, else the only one
        // open, to `go`; when none is open, or it is not clear which, the client is answered (for Claude to ask the user).
        void withInstance(const juce::var& id, const std::string& wanted, std::function<void(const std::string&)> go)
        {
            const std::string thing = kThing;
            if (instances_.empty())
            {
                reply(id, textContent("No " + thing + " is open (" + pg::product::kName + "). Ask the user to open it in their DAW ("
                                      + (kInstrument ? "on an instrument track" : "on an audio track") + ") or as its own app, then try again.",
                                      true));
                return;
            }
            if (!wanted.empty())
            {
                gather([this, id, wanted, go](std::vector<Seen> all) {
                    const std::string thing2 = kThing;
                    const std::vector<const Seen*> found = matching(all, wanted);
                    if (found.size() == 1)
                    {
                        go(found.front()->id);
                        return;
                    }
                    std::string list;
                    if (found.size() > 1)
                        for (const Seen* s : found)
                            list += "- " + s->describe() + "\n";
                    else
                        for (const Seen& s : all)
                            list += "- " + s.describe() + "\n";
                    reply(id, textContent(std::string(found.empty() ? "No open " + thing2 + " matches \"" + wanted + "\"."
                                                                    : "More than one open " + thing2 + " matches \"" + wanted + "\".")
                                              + " Ask the user which one they mean, in their words:\n" + list,
                                          true));
                });
                return;
            }
            if (instances_.count(chosen_) != 0)
                go(chosen_);
            else if (instances_.size() == 1)
                go(instances_.begin()->first);
            else
                gather([this, id](std::vector<Seen> all) {
                    std::string list;
                    for (const Seen& s : all)
                        list += "- " + s.describe() + "\n";
                    reply(id, textContent(std::to_string(all.size()) + " " + kThings + " are open. Ask the user which one they mean "
                                          "(by its track or its project, as below), then call again with `instance` set to what they said:\n" + list,
                                          true));
                });
        }

        // The tool call itself, on instance `instance`, answered when it answers or when its deadline passes.
        void callTool(const juce::var& id, const std::string& instance, const std::string& name, const juce::var& args)
        {
            if (!stillOpen(id))
                return;                                  // cancelled while the instance was found
            ask(instance, "call", object({ { "name", str(name) }, { "arguments", args } }), callMs_, true, key(id),
                [this, id, instance](Got g, const juce::var& r, const std::string& why) {
                    const std::string thing = kThing;
                    if (g == Got::result)
                    {
                        chosen_ = instance;
                        reply(id, r);
                    }
                    else if (g == Got::late)
                        reply(id, textContent("Still running after " + std::to_string(callMs_ / 1000) + " s, longer than a call may "
                                              "take here, so the " + thing + " was asked to stop it; nothing more will come of "
                                              "this call. Check the plugin's state before going on (a tuning stopped this way "
                                              "leaves the dials as they were), and ask for less at a time.",
                                              true));
                    else if (g == Got::error)
                        reply(id, textContent("The " + thing + " could not do it: " + why, true));
                    else
                        reply(id, textContent("The " + thing + " did not answer: " + why + ". Ask the user whether it is still open.", true));
                });
        }

        juce::var listInstances(const std::vector<Seen>& all)
        {
            const std::string thing = kThing, things = kThings;
            if (all.empty())
                return textContent("No " + thing + " is open (" + pg::product::kName + "). Ask the user to open it in their DAW ("
                                   + (kInstrument ? "on an instrument track" : "on an audio track") + ") or as its own app.");
            std::string text = std::to_string(all.size()) + (all.size() == 1 ? " " + thing + " is" : " " + things + " are") + " open:\n";
            for (const Seen& s : all)
                text += "- " + s.describe() + (s.id == chosen_ ? " (the one used last)" : "")
                      + "; its files: " + s.files + (s.compiled ? "" : "; its last run did not compile")
                      + (s.busy ? "; its own chat is busy" : "") + "\n";
            if (all.size() > 1)
                text += "Pass the one the user means as `instance` (its name, its track or its project).";
            return textContent(text);
        }

        // A part of the guide (mcp/McpGuide.h): from an instance, else cut from the guide.md the last one left.
        void guide(const std::string& part, Then then)
        {
            // The part always named: a plugin answers a guide request without one (a 0.2.0 server's) with all of it.
            firstOf("guide", object({ { "part", str(part.empty() ? std::string("start") : part) } }), [](const juce::var& g) { return g.isString(); },
                    [part, then](Got g, const juce::var& r, const std::string& why) {
                        if (g == Got::result)
                        {
                            then(textContent(r.toString().toStdString()));
                            return;
                        }
                        if (g == Got::error)
                        {
                            then(textContent(why, true));        // a part the plugin does not have
                            return;
                        }
                        const std::string dir = pg::mcp::folder();
                        const juce::File cached = juce::File(str(dir)).getChildFile("guide.md");
                        if (dir.empty() || !cached.existsAsFile())
                        {
                            then(textContent(std::string("The guide comes from the plugin: ask the user to open ") + pg::product::kName + " once.", true));
                            return;
                        }
                        bool found = true;
                        const std::string text = pg::mcp::guide::part(cached.loadFileAsString().toStdString(), part,
                                                                      [](const std::string& file) {
                                                                          const pg::LibraryFile* f = pg::findLibraryFile(file);
                                                                          return f != nullptr ? std::string(f->source) : std::string();
                                                                      },
                                                                      found);
                        then(textContent(text, !found));
                    });
        }

        // ---- the GUI sandbox (sandbox/SandboxProtocol.h) ----------------------------------------------------------------
        // One sandbox for the server, in a helper process started on the first call: Claude Desktop runs one server for
        // the extension, so its conversations share it (each answer names it, "Sandbox #3"; an open replaces it). Its
        // requests go one at a time, queued, each with a deadline (sbMs_): past it the helper is killed and the call says
        // so. The server keeps what the helper last said the sandbox is (sbState_), so a helper that died, or quit after
        // its idle minutes, is started again with the panel as it was. Never a blocking write: what the helper has not
        // taken waits in sbOutbox_ (POLLOUT), as an instance's does.
        using SandboxDone = std::function<void(bool ok, const juce::var& reply, const std::string& why)>;

        struct SandboxJob
        {
            juce::var msg;                               // the request (its id set when sent)
            std::string client;                          // the client's request it serves (key(id)); "" for the server's own
            SandboxDone done;
        };

        std::string helperPath() const
        {
            const std::string var = std::string(pg::product::kEnvPrefix) + "GUI_SANDBOX";
            if (const char* v = std::getenv(var.c_str()); v != nullptr && *v != '\0')
                return v;
            return juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile(str(pg::sandbox::helperName()))
                .getFullPathName()
                .toStdString();
        }

        // The helper started, on two pipes (its stdin and stdout; its stderr is ours), holding none of our other
        // descriptors (an instance's socket kept open by it would outlive our closing it).
        bool startHelper(std::string& why)
        {
            const std::string path = helperPath();
            if (access(path.c_str(), X_OK) != 0)
            {
                why = "its helper is not there (" + path + ")";
                return false;
            }
            int toChild[2], fromChild[2];
            if (pipe(toChild) != 0)
            {
                why = "no pipe";
                return false;
            }
            if (pipe(fromChild) != 0)
            {
                close(toChild[0]);
                close(toChild[1]);
                why = "no pipe";
                return false;
            }
            posix_spawn_file_actions_t fa;
            posix_spawn_file_actions_init(&fa);
            posix_spawn_file_actions_adddup2(&fa, toChild[0], 0);
            posix_spawn_file_actions_adddup2(&fa, fromChild[1], 1);
            posix_spawnattr_t attr;
            posix_spawnattr_init(&attr);
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
            posix_spawn_file_actions_addinherit_np(&fa, 2);
            posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
            char* argv[] = { const_cast<char*>(path.c_str()), nullptr };
            pid_t pid = -1;
            const int rc = posix_spawn(&pid, path.c_str(), &fa, &attr, argv, environ);
            posix_spawn_file_actions_destroy(&fa);
            posix_spawnattr_destroy(&attr);
            close(toChild[0]);
            close(fromChild[1]);
            if (rc != 0)
            {
                close(toChild[1]);
                close(fromChild[0]);
                why = "its helper did not start (" + std::string(std::strerror(rc)) + ")";
                return false;
            }
            for (const int fd : { toChild[1], fromChild[0] })
            {
                pg::mcp::nonBlocking(fd);
                fcntl(fd, F_SETFD, FD_CLOEXEC);
            }
            sbPid_ = pid;
            sbIn_ = toChild[1];
            sbOut_ = fromChild[0];
            sbReader_ = std::make_unique<pg::mcp::LineReader>(sbOut_);
            sbOutbox_.clear();
            ++sbSerial_;
            std::cerr << "sandbox: started its helper, pid " << pid << std::endl;
            return true;
        }

        // The helper is gone (it closed its end, stopped reading, or is killed here: `kill`); the request it had fails
        // with `why`, and the next one starts it again.
        void helperGone(const std::string& why, bool kill = false)
        {
            if (sbPid_ > 0)
            {
                int status = 0;
                pid_t r = kill ? 0 : waitpid(sbPid_, &status, WNOHANG);
                if (r == 0)
                {
                    ::kill(sbPid_, SIGKILL);     // hung, or closed its end and lingers: of no use either way
                    r = waitpid(sbPid_, &status, 0);
                }
                const bool quit = r == sbPid_ && WIFEXITED(status) && WEXITSTATUS(status) == 0;
                std::cerr << "sandbox: its helper ended (" << why << (quit ? ", exit 0" : "") << ")" << std::endl;
                if (!quit && !sbState_.isVoid())
                    sbNote_ = "(The sandbox's helper had stopped; it started again, with the panel as it was.)";
            }
            if (sbIn_ >= 0)
                close(sbIn_);
            if (sbOut_ >= 0)
                close(sbOut_);
            sbPid_ = -1;
            sbIn_ = sbOut_ = -1;
            sbReader_.reset();
            sbOutbox_.clear();
            ++sbSerial_;
            if (sbBusy_)
            {
                SandboxJob job = std::move(*sbBusy_);
                sbBusy_.reset();
                sbNote_.clear();
                job.done(false, juce::var(), "the sandbox's helper " + why + "; it starts again, with the panel as it was, on the next call");
            }
            pumpSandbox();
        }

        // Its replies: the one it owes, then the next request.
        void onHelper()
        {
            if (!sbReader_ || !sbReader_->readSome())
            {
                helperGone("closed its end (it quit or crashed)");
                return;
            }
            std::string line;
            while (sbReader_ && sbReader_->take(line))
            {
                const juce::var r = juce::JSON::parse(str(line));
                if (!sbBusy_ || static_cast<int>(r["id"]) != sbBusyId_)
                    continue;                            // a reply nobody waits for
                SandboxJob job = std::move(*sbBusy_);
                sbBusy_.reset();
                if (r.hasProperty("state"))
                    sbState_ = r["state"];
                job.done(static_cast<bool>(r["ok"]), r, {});
                pumpSandbox();
                return;
            }
        }

        // A request for the helper, answered to `done` (never twice); `client`: the client's request (dropped when it
        // is cancelled before it is sent).
        void sandboxAsk(const juce::var& msg, const std::string& client, SandboxDone done)
        {
            sbQueue_.push_back({ msg, client, std::move(done) });
            pumpSandbox();
        }

        void pumpSandbox()
        {
            while (!sbBusy_ && !sbQueue_.empty())
            {
                if (!sbQueue_.front().client.empty() && open_.count(sbQueue_.front().client) == 0)
                {
                    sbQueue_.pop_front();                // cancelled meanwhile
                    continue;
                }
                const bool opening = sbQueue_.front().msg["op"].toString() == "open";
                if (sbPid_ < 0)
                {
                    std::string why;
                    if (!startHelper(why))
                    {
                        std::deque<SandboxJob> failed;
                        failed.swap(sbQueue_);
                        for (SandboxJob& j : failed)
                            j.done(false, juce::var(), why);
                        return;
                    }
                    // The sandbox as it was, in the new helper, before what is asked of it now.
                    if (!sbState_.isVoid() && !opening)
                    {
                        juce::var restore = juce::JSON::parse(juce::JSON::toString(sbState_));
                        if (juce::DynamicObject* o = restore.getDynamicObject())
                        {
                            o->setProperty("op", "open");
                            o->setProperty("from", "files");
                            o->setProperty("restore", true);
                        }
                        sbQueue_.push_front({ restore, {}, [this](bool made, const juce::var&, const std::string& whyNot) {
                                                 if (!made)
                                                 {
                                                     std::cerr << "sandbox: could not make it again: " << whyNot << std::endl;
                                                     sbState_ = juce::var();
                                                 }
                                             } });
                    }
                }
                SandboxJob job = std::move(sbQueue_.front());
                sbQueue_.pop_front();
                sbBusyId_ = sbNext_++;
                if (juce::DynamicObject* o = job.msg.getDynamicObject())
                    o->setProperty("id", sbBusyId_);
                sbOutbox_ += juce::JSON::toString(job.msg, true).toStdString() + "\n";
                sbDeadline_ = Clock::now() + std::chrono::milliseconds(sbMs_);
                sbBusy_ = std::move(job);
                if (!flushPipe(sbIn_, sbOutbox_))
                {
                    helperGone("could not be written to");
                    return;
                }
            }
        }

        // The files of instance `instance`'s program, read through its get_status (files "all": read only, live), to
        // `then` (false, and why, when it could not).
        void instanceFiles(const std::string& instance, const std::string& client,
                           std::function<void(bool, std::vector<std::pair<std::string, std::string>>, const std::string&)> then)
        {
            ask(instance, "call", object({ { "name", "get_status" }, { "arguments", object({ { "files", "all" } }) } }), 20000, false, client,
                [then](Got g, const juce::var& r, const std::string& why) {
                    if (g != Got::result || static_cast<bool>(r["isError"]))
                    {
                        then(false, {}, g == Got::result ? firstText(r) : why);
                        return;
                    }
                    std::string all;
                    if (const juce::Array<juce::var>* c = r["content"].getArray())
                        for (const juce::var& b : *c)
                            if (b["type"].toString() == "text")
                                all += b["text"].toString().toStdString() + "\n";
                    then(true, filesIn(all), {});
                });
        }

        // A sandbox reply as the tool's result: its picture (render's) and its text, the note of a helper started again
        // first.
        juce::var sandboxResult(bool ok, const juce::var& r, const std::string& why)
        {
            juce::Array<juce::var> content;
            std::string text = r.isVoid() ? "The sandbox could not do it: " + why + "." : r["text"].toString().toStdString();
            if (!sbNote_.empty() && ok)
            {
                text = sbNote_ + "\n" + text;
                sbNote_.clear();
            }
            if (ok && r.hasProperty("png"))
                content.add(object({ { "type", "image" }, { "data", r["png"] }, { "mimeType", "image/png" } }));
            content.add(object({ { "type", "text" }, { "text", str(text) } }));
            return object({ { "content", content }, { "isError", !ok } });
        }

        std::string instanceName(const std::string& instance) const { return std::string(kInstrument ? "Synth " : "Effect ") + instance; }

        void sandboxTool(const juce::var& id, const juce::var& args)
        {
            const std::string action = args["action"].toString().toStdString();
            const std::string wanted = args["instance"].toString().trim().toStdString();
            const juce::var detail = args["detail"];
            const auto answer = [this, id](bool ok, const juce::var& r, const std::string& why) { reply(id, sandboxResult(ok, r, why)); };
            const auto copy = [&](juce::DynamicObject& o, std::initializer_list<const char*> keys) {
                for (const char* k : keys)
                    if (args.hasProperty(k))
                        o.setProperty(k, args[k]);
            };
            if (action == "open")
            {
                const std::string from = args["from"].toString().toStdString();
                if (from == "instance")
                {
                    withInstance(id, wanted, [this, id, args, answer](const std::string& instance) {
                        instanceFiles(instance, key(id), [this, id, args, instance, answer](bool ok, std::vector<std::pair<std::string, std::string>> files,
                                                                                           const std::string& why) {
                            if (!ok)
                            {
                                reply(id, textContent("Could not read the " + std::string(kThing) + "'s program: " + why, true));
                                return;
                            }
                            chosen_ = instance;
                            juce::Array<juce::var> list;
                            for (const auto& [name, text] : files)
                                list.add(object({ { "name", str(name) }, { "text", str(text) } }));
                            juce::var msg = object({ { "op", "open" }, { "from", "files" }, { "files", list }, { "number", ++sbOpens_ },
                                                     { "origin", str("the program of " + instanceName(instance) + " (read only)") } });
                            for (const char* k : { "dials", "detail" })
                                if (args.hasProperty(k))
                                    msg.getDynamicObject()->setProperty(k, args[k]);
                            sandboxAsk(msg, key(id), answer);
                        });
                    });
                    return;
                }
                if (!from.empty() && from != "blank" && from != "file")
                {
                    reply(id, textContent("from is blank, instance or file (or give text alone: that .gui).", true));
                    return;
                }
                juce::var msg = object({ { "op", "open" }, { "number", ++sbOpens_ } });
                juce::DynamicObject& o = *msg.getDynamicObject();
                o.setProperty("from", from == "file" ? "file" : (args.hasProperty("text") && from.empty() ? "text" : "blank"));
                copy(o, { "path", "text", "dials", "detail" });
                sandboxAsk(msg, key(id), answer);
                return;
            }
            if (action == "edit" || action == "render")
            {
                juce::var msg = object({ { "op", str(action) } });
                juce::DynamicObject& o = *msg.getDynamicObject();
                if (action == "edit")
                    copy(o, { "text", "edits", "dials", "detail" });
                else
                {
                    copy(o, { "width", "height", "theme", "page", "values" });
                    o.setProperty("cap", detail.toString() == "high" ? 0 : static_cast<int>(kPictureBytes));
                }
                if (sbState_.isVoid() && sbPid_ < 0)
                {
                    reply(id, textContent("Nothing is open in the sandbox: open it first (a blank panel, the open plugin's, a file "
                                          "the user named, or a .gui as text).", true));
                    return;
                }
                sandboxAsk(msg, key(id), answer);
                return;
            }
            if (action == "apply")
            {
                if (sbState_.isVoid())
                {
                    reply(id, textContent("Nothing is open in the sandbox: there is nothing to apply.", true));
                    return;
                }
                if (instances_.empty())
                {
                    discover();
                    if (instances_.empty())
                    {
                        const juce::var st = sbState_;
                        const std::string guiName = st["gui"].toString().toStdString();
                        std::string text;
                        if (const juce::Array<juce::var>* f = st["files"].getArray())
                            for (const juce::var& x : *f)
                                if (x["name"].toString().toStdString() == guiName)
                                    text = x["text"].toString().toStdString();
                        reply(id, textContent("No " + std::string(kThing) + " is open (" + pg::product::kName + "), so nothing was applied. "
                                              "The sandbox's " + guiName + " (sandbox #" + std::to_string(static_cast<int>(st["number"]))
                                              + ") is below. Tell the user how to use it: open " + pg::product::kName
                                              + " and ask you again to apply it, or paste it into a new tab named " + guiName
                                              + " beside the program in the plugin's editor and press RUN.\n\n" + text, false));
                        return;
                    }
                }
                withInstance(id, wanted, [this, id](const std::string& instance) { applyTo(id, instance); });
                return;
            }
            if (action == "close")
            {
                sbState_ = juce::var();
                sbNote_.clear();
                if (sbPid_ < 0)
                {
                    reply(id, textContent("The sandbox is closed."));
                    return;
                }
                sandboxAsk(object({ { "op", "quit" } }), key(id), [this, id](bool, const juce::var&, const std::string&) {
                    reply(id, textContent("The sandbox is closed (its helper ended)."));
                    helperGone("was closed");
                });
                return;
            }
            reply(id, textContent("action is open, edit, render, apply or close.", true));
        }

        // apply: the sandbox's .gui written into instance `instance`'s program (into its own .gui file when it has one,
        // else as the sandbox names it), and run there, so that it shows; what changed said.
        void applyTo(const juce::var& id, const std::string& instance)
        {
            const juce::var st = sbState_;
            const std::string guiName = st["gui"].toString().toStdString();
            std::string text;
            if (const juce::Array<juce::var>* f = st["files"].getArray())
                for (const juce::var& x : *f)
                    if (x["name"].toString().toStdString() == guiName)
                        text = x["text"].toString().toStdString();
            const int number = static_cast<int>(st["number"]);
            instanceFiles(instance, key(id), [this, id, instance, guiName, text, number](bool ok, std::vector<std::pair<std::string, std::string>> files,
                                                                                         const std::string& why) {
                if (!ok)
                {
                    reply(id, textContent("Could not read the " + std::string(kThing) + "'s program, so nothing was applied: " + why, true));
                    return;
                }
                std::string target = guiName, before;
                bool had = false;
                for (const auto& [name, t] : files)
                    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".gui") == 0)
                    {
                        target = name;                   // the interface is the program's first .gui file
                        before = t;
                        had = true;
                        break;
                    }
                const std::string thing = kThing;
                ask(instance, "call", object({ { "name", "write_file" }, { "arguments", object({ { "name", str(target) }, { "content", str(text) } }) } }),
                    callMs_, true, key(id),
                    [this, id, instance, target, text, before, had, number, thing](Got g, const juce::var& w, const std::string& why2) {
                        if (g != Got::result || static_cast<bool>(w["isError"]))
                        {
                            reply(id, textContent("The " + thing + " did not take it, so nothing was applied: "
                                                  + (g == Got::result ? firstText(w) : why2), true));
                            return;
                        }
                        ask(instance, "call", object({ { "name", "run_program" }, { "arguments", juce::var(new juce::DynamicObject()) } }), callMs_, true,
                            key(id), [this, id, instance, target, text, before, had, number, thing](Got g3, const juce::var& run, const std::string& why3) {
                                chosen_ = instance;
                                const auto [added, removed] = lineChanges(before, text);
                                std::string said = "Applied sandbox #" + std::to_string(number) + " to " + instanceName(instance) + ": wrote "
                                                 + target + " into its program (" + std::to_string(lineCount(text)) + " lines; "
                                                 + (!had ? std::string("it had no .gui file")
                                                         : before == text ? std::string("the same as it was")
                                                                          : "it had " + std::to_string(lineCount(before)) + ": +" + std::to_string(added)
                                                                                + " -" + std::to_string(removed) + " lines")
                                                 + ") and ran it, so the user sees it in the plugin's PANEL view. ";
                                if (g3 != Got::result)
                                    said += "The run did not answer: " + why3 + ".";
                                else
                                {
                                    std::string runText;
                                    if (const juce::Array<juce::var>* c = run["content"].getArray())
                                        for (const juce::var& b : *c)
                                            if (b["type"].toString() == "text")
                                                runText += b["text"].toString().toStdString() + "\n";
                                    said += "The run:\n" + runText;
                                }
                                reply(id, textContent(said, g3 != Got::result || static_cast<bool>(run["isError"])));
                            });
                    });
            });
        }

        // ---- the prompts: requests the user picks in Claude Desktop ------------------------------------------------------
        // Those whose tools `tools` has (all of them when it is void: no instance, no tools.json).
        juce::var promptList(const juce::var& tools) const
        {
            juce::Array<juce::var> out;
            const auto arg = [](const char* n, const char* d, bool required) {
                return object({ { "name", n }, { "description", d }, { "required", required } });
            };
            for (const PromptInfo& p : prompts())
            {
                if (!offered(p, tools))
                    continue;
                juce::Array<juce::var> args;
                std::string description;
                const std::string name = p.name;
                if (name == "make_a_sound")
                {
                    args.add(arg("sound", kInstrument ? "What it should sound like: \"a warm pad with a slow filter sweep\""
                                                      : "What it should do: \"a tape delay with a little wow\"", true));
                    description = kInstrument ? "Describe a sound; Claude builds it in the synth and sets its dials."
                                              : "Describe an effect; Claude builds it in the plugin and sets its dials.";
                }
                else if (name == "improve_the_sound")
                {
                    args.add(arg("change", "What to change: \"brighter\", \"less harsh\", \"more movement\"", false));
                    description = std::string("Claude listens (looks at what plays now, measures it and looks at it) and makes what the ")
                                + kThing + " plays better.";
                }
                else if (name == "explain_the_sound")
                    description = std::string("Claude explains, in plain words, what the ") + kThing + " plays now and what each dial does.";
                else if (name == "fix_what_i_hear")
                {
                    args.add(arg("what", "What you hear: \"a click on fast notes\", \"it crackles with the drive up\", \"it goes silent\"", true));
                    args.add(arg("when", "When it happens: \"in the chorus\", \"on high notes\", \"after a minute\"", false));
                    description = std::string("Tell Claude what sounds wrong (a click, a crackle, silence, a sound that blows up); it "
                                              "finds where it comes from in the ") + kThing + ", fixes it, and tells you how to check.";
                }
                out.add(object({ { "name", p.name }, { "title", p.title }, { "description", str(description) }, { "arguments", args } }));
            }
            return out;
        }

        static bool offered(const PromptInfo& p, const juce::var& tools)
        {
            const juce::Array<juce::var>* a = tools.getArray();
            if (a == nullptr)
                return true;
            for (const char* need : p.needs)
                if (std::none_of(a->begin(), a->end(), [need](const juce::var& t) { return t["name"].toString() == need; }))
                    return false;
            return true;
        }

        void prompt(const juce::var& id, const juce::var& params)
        {
            const std::string name = params["name"].toString().toStdString();
            const juce::var a = params["arguments"];
            const auto info = std::find_if(prompts().begin(), prompts().end(), [&](const PromptInfo& p) { return name == p.name; });
            if (info == prompts().end())
            {
                fail(id, -32602, "There is no prompt " + name);
                return;
            }
            const std::string thing = std::string(kThing) + " (" + pg::product::kName + ")";
            const auto arg = [&](const char* k) { return juce::String(a[k].toString()).trim().toStdString(); };
            std::string text;
            if (name == "make_a_sound")
                text = "In the " + thing + ", make this: " + arg("sound")
                     + ". Read its guide first if you have not in this conversation, build it, run it, measure it and look at it, fix "
                       "what is wrong, and leave its dials at good settings. Then tell me in a few plain sentences what you made and "
                       "how to play it.";
            else if (name == "improve_the_sound")
                text = "Listen to what the " + thing + " plays now: start from what it is doing in my song as it is, then measure it "
                       "and look at it. " + (arg("change").empty() ? std::string("Make it sound better") : "Make it " + arg("change"))
                     + ", keeping what already works. Tell me briefly what you changed and why.";
            else if (name == "explain_the_sound")
                text = "Explain in plain words what the " + thing + " plays now: start from what it is doing in my song as it is, then "
                       "how the sound is made and what each of its dials does. Read its program and measure it; change nothing.";
            else
                text = "Something in the " + thing + " sounds wrong: " + arg("what") + (arg("when").empty() ? std::string() : ", " + arg("when"))
                     + ". Find out why and fix it:\n"
                       "1. First hear what it plays now in my song, as it is, before you guess.\n"
                       "2. If it happens only while I play, tell me you will record me and what to play, record me playing it, "
                       "and work from that recording; otherwise work from what I told you.\n"
                       "3. Reproduce it away from my song (on my recording, or on notes and moves you write), so you can study "
                       "it without me.\n"
                       "4. Find where it starts: the moment, the note, and the part of the program that goes wrong first.\n"
                       "5. Try a fix on a copy, without changing what I hear, and play it the same way against what plays now "
                       "until you know it works; then put it in.\n"
                       "6. Set a check, on a passage like mine, so it stays fixed.\n"
                       "7. Tell me in a few plain sentences what was wrong, what you changed, and what to play to hear that it is gone.";
            const auto finish = [this, id](const std::string& t) {
                juce::Array<juce::var> messages;
                messages.add(object({ { "role", "user" }, { "content", object({ { "type", "text" }, { "text", str(t) } }) } }));
                reply(id, object({ { "description", "" }, { "messages", messages } }));
            };
            withTools([this, id, info, text, finish](const juce::var& tools) {
                if (!offered(*info, tools))
                {
                    fail(id, -32602, std::string("The prompt ") + info->name + " needs tools this " + kThing + " does not have");
                    return;
                }
                // The plugin's state, when it is clear which instance: what Claude would read first anyway.
                std::string which;
                if (instances_.count(chosen_) != 0)
                    which = chosen_;
                else if (instances_.size() == 1)
                    which = instances_.begin()->first;
                if (!info->state || which.empty())
                {
                    finish(text);
                    return;
                }
                ask(which, "call", object({ { "name", "get_status" }, { "arguments", juce::var(new juce::DynamicObject()) } }), kQuickMs, false, {},
                    [text, finish](Got g, const juce::var& r, const std::string&) {
                        const std::string state = g == Got::result && !static_cast<bool>(r["isError"]) ? firstText(r) : std::string();
                        finish(state.empty() ? text : text + "\n\n(What the plugin reports now:)\n" + state);
                    });
            });
        }

        std::map<std::string, Instance> instances_;
        uint64_t serials_ = 0;                           // the connections made so far
        std::list<Ask> asks_;
        std::set<std::string> open_;                     // the client's requests not answered yet (key(id))
        std::string chosen_;
        bool listedPlain_ = false;                       // the last tools/list had none of the plugin's tools
        const int callMs_ = callDeadlineMs();
        // The GUI sandbox's helper (above): its process and pipes (non-blocking), its connection's number (a helper
        // started again is another), the requests waiting and the one it works on, and the sandbox as it last said it is.
        pid_t sbPid_ = -1;
        int sbIn_ = -1, sbOut_ = -1;
        uint64_t sbSerial_ = 0;
        std::unique_ptr<pg::mcp::LineReader> sbReader_;
        std::string sbOutbox_;
        std::deque<SandboxJob> sbQueue_;
        std::optional<SandboxJob> sbBusy_;
        int sbBusyId_ = 0, sbNext_ = 1;
        int sbOpens_ = 0;                                // the sandbox's number: opens so far
        Clock::time_point sbDeadline_;
        juce::var sbState_;
        std::string sbNote_;                             // said first in the next answer: the helper was started again
        const int sbMs_ = sandboxDeadlineMs();
    };
}

int main()
{
    std::signal(SIGPIPE, SIG_IGN);
    Server server;
    return server.run();
}
