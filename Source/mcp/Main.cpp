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

#include "Library.h"
#include "Product.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
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

namespace pg = playground;

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr int kQuickMs = 5000;                      // info, tools, guide, a prompt's state: answered beside a call
    constexpr int kCallMs = 200000;                     // a tool call: under Claude Desktop's 240 s (its docs), above
                                                        // tune_dials' own limit (170 s at most)
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
              { "get_status", "measure_output", "view_sound", "edit_file", "run_program", "set_checks" } },
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
                expire();
            }
            // Nobody waits for what is still running: the plugin need not finish it (a cancel the socket cannot take now
            // is lost, but the plugin cancels a closed connection's calls anyway).
            for (const Ask& a : asks_)
                if (a.call)
                    stop(a.instance, a.number);
            for (auto& entry : instances_)
                close(entry.second.fd);
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
                  "you cannot tell, ask them in their words, describing each by its track and project.";
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
            if (asks_.empty())
                return -1;
            Clock::time_point first = asks_.front().deadline;
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
                                                                         "library:<file>:code, examples, projects or all" } }) } })));
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
            const std::string thing = kThing;
            const std::string wanted = args["instance"].toString().toStdString();
            args.getDynamicObject()->removeProperty("instance");
            if (instances_.empty())
            {
                reply(id, textContent("No " + thing + " is open (" + pg::product::kName + "). Ask the user to open it in their DAW ("
                                      + (kInstrument ? "on an instrument track" : "on an audio track") + ") or as its own app, then try again.",
                                      true));
                return;
            }
            if (!wanted.empty())
            {
                gather([this, id, name, args, wanted](std::vector<Seen> all) {
                    const std::string thing2 = kThing;
                    const std::vector<const Seen*> found = matching(all, wanted);
                    if (found.size() == 1)
                    {
                        callTool(id, found.front()->id, name, args);
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
                callTool(id, chosen_, name, args);
            else if (instances_.size() == 1)
                callTool(id, instances_.begin()->first, name, args);
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
                       "1. First look at what it plays now in my song, as it is, before you guess.\n"
                       "2. If it happens only while I play, tell me what to play and ask me to play it (recording it, if you can); "
                       "otherwise work from what I told you.\n"
                       "3. Reproduce it away from my song, so you can study it without me.\n"
                       "4. Find where it starts: the part of the program, and the moment it goes wrong.\n"
                       "5. Try a fix without changing what I hear until you know it works; then put it in.\n"
                       "6. Set a check so it stays fixed.\n"
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
    };
}

int main()
{
    std::signal(SIGPIPE, SIG_IGN);
    Server server;
    return server.run();
}
