// Source/mcp/Main.cpp: <plugin>MCP, this playground's MCP server, for Claude Desktop (or any MCP client) to act as the
// AI in the plugin: JSON-RPC 2.0 a line at a time on stdin and stdout (MCP's stdio transport), the plugin's instances
// reached through their sockets (mcp/McpProtocol.h, plugin/McpLink.h).
//
// It is made for someone who never sees a tool's name: the instructions tell Claude to talk about sounds, tracks and
// projects, never ids or tools; each tool has a title in words; an instance is named as the user would name it.
//   tools/list   list_instances ("Find the open synths"), playground_guide ("Read the guide"), and the chat's own tools
//                (ChatAgent::mcpTools, each with its title) from an instance running, else from the tools.json the last
//                one left; each takes `instance`
//   tools/call   any of the chat's tools goes to the instance named: by its name ("Synth 2", or just "2"), its
//                track's, or its project's (whole or in part, case aside); else to the one used last; else to the only
//                one open. With several open and none named, the reply describes them so that Claude asks the user
//   prompts      Make a sound, Improve the sound, Explain the sound: requests the user picks in Claude Desktop
// stdout carries the protocol only; what went wrong goes to stderr (Claude Desktop keeps it in its MCP logs).
#include "mcp/McpProtocol.h"

#include "Product.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <dirent.h>

namespace pg = playground;

namespace
{
    constexpr int kCallTimeoutMs = 180000;              // a tool may run a while (tune_dials, a long render)
    constexpr int kQuickTimeoutMs = 5000;               // info, tools, guide
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

    std::string lower(std::string s)
    {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

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

    struct Instance
    {
        std::string id, path;
        int fd = -1;
        std::unique_ptr<pg::mcp::LineReader> reader;
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

    class Server
    {
    public:
        int run()
        {
            std::string line;
            while (std::getline(std::cin, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty())
                    continue;
                const juce::var msg = juce::JSON::parse(str(line));
                if (!msg.isObject())
                {
                    send(object({ { "jsonrpc", "2.0" }, { "id", juce::var() },
                                  { "error", object({ { "code", -32700 }, { "message", "Parse error" } }) } }));
                    continue;
                }
                handle(msg);
            }
            for (auto& entry : instances_)
                close(entry.second.fd);
            return 0;
        }

    private:
        void send(const juce::var& msg) { std::cout << juce::JSON::toString(msg, true).toStdString() << "\n" << std::flush; }

        void reply(const juce::var& id, const juce::var& result) { send(object({ { "jsonrpc", "2.0" }, { "id", id }, { "result", result } })); }

        void fail(const juce::var& id, int code, const std::string& message)
        {
            send(object({ { "jsonrpc", "2.0" }, { "id", id }, { "error", object({ { "code", code }, { "message", str(message) } }) } }));
        }

        void handle(const juce::var& msg)
        {
            const juce::String method = msg["method"].toString();
            const juce::DynamicObject* o = msg.getDynamicObject();
            if (o == nullptr || !o->hasProperty("id"))
                return;                                  // a notification: initialized, cancelled
            const juce::var id = msg["id"];
            const juce::var params = msg["params"];
            if (method == "initialize")
                reply(id, initialize(params));
            else if (method == "ping")
                reply(id, juce::var(new juce::DynamicObject()));
            else if (method == "tools/list")
                reply(id, object({ { "tools", toolList() } }));
            else if (method == "tools/call")
                reply(id, call(params));
            else if (method == "prompts/list")
                reply(id, object({ { "prompts", promptList() } }));
            else if (method == "prompts/get")
            {
                const std::optional<juce::var> p = prompt(params);
                if (p)
                    reply(id, *p);
                else
                    fail(id, -32602, "There is no prompt " + params["name"].toString().toStdString());
            }
            else if (method == "resources/list")
                reply(id, object({ { "resources", juce::Array<juce::var>() } }));
            else
                fail(id, -32601, "Method not found: " + method.toStdString());
            // A first instance since the tools were listed: say the list changed (a client may list them again).
            if (method != "tools/list" && listedPlain_ && !pluginTools().isVoid())
            {
                listedPlain_ = false;
                send(object({ { "jsonrpc", "2.0" }, { "method", "notifications/tools/list_changed" } }));
            }
        }

        juce::var initialize(const juce::var& params)
        {
            juce::String version = params["protocolVersion"].toString();
            if (version != "2024-11-05" && version != "2025-03-26" && version != "2025-06-18")
                version = "2025-06-18";
            const std::string thing = kThing;
            const std::string instructions =
                std::string(pg::product::kName) + " is " + (kInstrument ? "a synth" : "an effect")
                + " plugin whose sound is a short program in FMinus, a small language it compiles as you write. With it open "
                  "in the user's DAW (or as its own app), these tools let you make and change its sound as the plugin's "
                  "own chat would: write the program, run it, measure it, look at it, and set its dials.\n"
                  "The person you talk to is a musician, not a programmer of this plugin: never ask them about tools, ids or "
                  "file names, and never tell them to call a tool. Talk about the sound, their tracks and their projects. "
                  "When they ask for a sound, just make it.\n"
                  "Before you first write or change code in a conversation, read playground_guide once: it teaches FMinus and "
                  "how the plugin works. Then check the plugin's state, write the program, run it, measure it and look at it, "
                  "fix what is wrong, and leave its dials at good settings. Tell the user what you made in a few plain "
                  "sentences, and what to play to hear it.\n"
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
            // A connection whose instance went away goes (its number may already be another's).
            for (const std::string& id : ids())
                if (!alive(instances_.at(id).fd))
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
                Instance in;
                in.id = id;
                in.path = path;
                in.fd = fd;
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

        void drop(const std::string& id)
        {
            if (auto it = instances_.find(id); it != instances_.end())
            {
                close(it->second.fd);
                instances_.erase(it);
            }
        }

        // A request to an instance and its reply's result; nothing (with why) when it does not answer.
        std::optional<juce::var> ask(const std::string& id, const char* method, const juce::var& params, int timeoutMs, std::string& why)
        {
            auto it = instances_.find(id);
            if (it == instances_.end())
            {
                why = "it is no longer open";
                return std::nullopt;
            }
            Instance& in = it->second;
            const int n = in.next++;
            if (!pg::mcp::writeLine(in.fd, juce::JSON::toString(object({ { "id", n }, { "method", method }, { "params", params } }), true).toStdString()))
            {
                why = "it has closed";
                drop(id);
                return std::nullopt;
            }
            std::string line;
            for (;;)
            {
                const int got = in.reader->next(line, timeoutMs);
                if (got <= 0)
                {
                    why = got == 0 ? "it did not answer in time" : "it has closed";
                    drop(id);
                    return std::nullopt;
                }
                const juce::var r = juce::JSON::parse(str(line));
                if (static_cast<int>(r["id"]) != n)
                    continue;                            // an older request's late reply
                if (r.hasProperty("error"))
                {
                    why = r["error"].toString().toStdString();
                    return std::nullopt;
                }
                return r["result"];
            }
        }

        // What each open instance is, as the user knows it.
        std::vector<Seen> seen()
        {
            std::vector<Seen> out;
            for (const std::string& id : ids())
            {
                std::string why;
                const std::optional<juce::var> info = ask(id, "info", juce::var(), kQuickTimeoutMs, why);
                if (!info)
                    continue;
                const juce::var& i = *info;
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
                out.push_back(std::move(s));
            }
            return out;
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
        juce::var pluginTools()
        {
            discover();
            for (const std::string& id : ids())
            {
                std::string why;
                if (const std::optional<juce::var> t = ask(id, "tools", juce::var(), kQuickTimeoutMs, why); t && t->isArray())
                    return *t;
            }
            const std::string dir = pg::mcp::folder();
            if (dir.empty())
                return {};
            const juce::var cached = juce::JSON::parse(juce::File(str(dir)).getChildFile("tools.json"));
            return cached.isArray() ? cached : juce::var();
        }

        static juce::var ownTool(const char* name, const std::string& title, const std::string& description)
        {
            return object({ { "name", name },
                            { "title", str(title) },
                            { "description", str(description) },
                            { "inputSchema", object({ { "type", "object" }, { "properties", juce::var(new juce::DynamicObject()) } }) },
                            { "annotations", object({ { "title", str(title) }, { "readOnlyHint", true }, { "openWorldHint", false } }) } });
        }

        juce::var toolList()
        {
            juce::Array<juce::var> out;
            const std::string thing = kThing, things = kThings;
            out.add(ownTool("list_instances", "Find the open " + things,
                            "Which " + things + " (" + pg::product::kName + ") are open now: each one's name (as the plugin "
                            "shows it), its track, its project and where it runs. Use it when more than one may be open."));
            out.add(ownTool("playground_guide", "Read the guide",
                            "The guide to this plugin and to FMinus, the language its sound is written in. Read it once in a "
                            "conversation, before you first write or change code."));
            const juce::var tools = pluginTools();
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
                                                        { "description", str("Which " + thing + ", when more than one is open: "
                                                                             "its name as the plugin shows it (\"" + (kInstrument ? "Synth" : "Effect")
                                                                             + " 2\"), its track's name or its project's, as the user said it. "
                                                                               "Leave it out when one is open.") } }));
                    }
                    out.add(tool);
                }
            return out;
        }

        juce::var call(const juce::var& params)
        {
            const std::string name = params["name"].toString().toStdString();
            juce::var args = params["arguments"].isObject() ? juce::JSON::parse(juce::JSON::toString(params["arguments"]))
                                                             : juce::var(new juce::DynamicObject());
            discover();
            if (name == "list_instances")
                return listInstances();
            if (name == "playground_guide")
                return guide();
            const std::string thing = kThing, things = kThings;
            const std::string wanted = args["instance"].toString().toStdString();
            args.getDynamicObject()->removeProperty("instance");
            if (instances_.empty())
                return textContent("No " + thing + " is open (" + pg::product::kName + "). Ask the user to open it in their DAW ("
                                   + (kInstrument ? "on an instrument track" : "on an audio track") + ") or as its own app, then try again.",
                                   true);
            std::string id;
            if (!wanted.empty())
            {
                const std::vector<Seen> all = seen();
                const std::vector<const Seen*> found = matching(all, wanted);
                if (found.size() == 1)
                    id = found.front()->id;
                else
                {
                    std::string list;
                    if (found.size() > 1)
                        for (const Seen* s : found)
                            list += "- " + s->describe() + "\n";
                    else
                        for (const Seen& s : all)
                            list += "- " + s.describe() + "\n";
                    return textContent(std::string(found.empty() ? "No open " + thing + " matches \"" + wanted + "\"."
                                                                 : "More than one open " + thing + " matches \"" + wanted + "\".")
                                           + " Ask the user which one they mean, in their words:\n" + list,
                                       true);
                }
            }
            else if (instances_.count(chosen_) != 0)
                id = chosen_;
            else if (instances_.size() == 1)
                id = instances_.begin()->first;
            else
            {
                std::string list;
                for (const Seen& s : seen())
                    list += "- " + s.describe() + "\n";
                return textContent(std::to_string(instances_.size()) + " " + things + " are open. Ask the user which one they mean "
                                   "(by its track or its project, as below), then call again with `instance` set to what they said:\n" + list,
                                   true);
            }
            std::string why;
            const std::optional<juce::var> r = ask(id, "call", object({ { "name", str(name) }, { "arguments", args } }), kCallTimeoutMs, why);
            if (!r)
                return textContent("The " + thing + " did not answer: " + why + ". Ask the user whether it is still open.", true);
            chosen_ = id;
            return *r;
        }

        juce::var listInstances()
        {
            const std::string thing = kThing, things = kThings;
            const std::vector<Seen> all = seen();
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

        juce::var guide()
        {
            for (const std::string& id : ids())
            {
                std::string why;
                if (const std::optional<juce::var> g = ask(id, "guide", juce::var(), kQuickTimeoutMs, why); g && g->isString())
                    return textContent(g->toString().toStdString());
            }
            const std::string dir = pg::mcp::folder();
            const juce::File cached = juce::File(str(dir)).getChildFile("guide.md");
            if (!dir.empty() && cached.existsAsFile())
                return textContent(cached.loadFileAsString().toStdString());
            return textContent(std::string("The guide comes from the plugin: ask the user to open ") + pg::product::kName + " once.", true);
        }

        // ---- the prompts: requests the user picks in Claude Desktop ------------------------------------------------------
        juce::var promptList() const
        {
            juce::Array<juce::var> out;
            const auto arg = [](const char* n, const char* d, bool required) {
                return object({ { "name", n }, { "description", d }, { "required", required } });
            };
            juce::Array<juce::var> makeArgs, improveArgs;
            makeArgs.add(arg("sound", kInstrument ? "What it should sound like: \"a warm pad with a slow filter sweep\""
                                                  : "What it should do: \"a tape delay with a little wow\"", true));
            improveArgs.add(arg("change", "What to change: \"brighter\", \"less harsh\", \"more movement\"", false));
            out.add(object({ { "name", "make_a_sound" },
                             { "title", kInstrument ? "Make a sound" : "Make an effect" },
                             { "description", kInstrument ? "Describe a sound; Claude builds it in the synth and sets its dials."
                                                          : "Describe an effect; Claude builds it in the plugin and sets its dials." },
                             { "arguments", makeArgs } }));
            out.add(object({ { "name", "improve_the_sound" },
                             { "title", "Improve the sound" },
                             { "description", str(std::string("Claude listens (measures and looks at it) and makes what the ") + kThing + " plays better.") },
                             { "arguments", improveArgs } }));
            out.add(object({ { "name", "explain_the_sound" },
                             { "title", "Explain the sound" },
                             { "description", str(std::string("Claude explains, in plain words, what the ") + kThing + " plays now and what each dial does.") },
                             { "arguments", juce::Array<juce::var>() } }));
            return out;
        }

        std::optional<juce::var> prompt(const juce::var& params) const
        {
            const juce::String name = params["name"].toString();
            const juce::var a = params["arguments"];
            const std::string thing = kThing;
            std::string text;
            if (name == "make_a_sound")
                text = std::string("In the ") + thing + " (" + pg::product::kName + "), make this: " + a["sound"].toString().toStdString()
                     + ". Read its guide first if you have not in this conversation, build it, run it, measure it and look at it, fix "
                       "what is wrong, and leave its dials at good settings. Then tell me in a few plain sentences what you made and "
                       "how to play it.";
            else if (name == "improve_the_sound")
                text = std::string("Listen to what the ") + thing + " (" + pg::product::kName + ") plays now: measure it and look at it. "
                     + (a["change"].toString().isEmpty() ? std::string("Make it sound better")
                                                         : "Make it " + a["change"].toString().toStdString())
                     + ", keeping what already works. Tell me briefly what you changed and why.";
            else if (name == "explain_the_sound")
                text = std::string("Explain in plain words what the ") + thing + " (" + pg::product::kName + ") plays now: how the sound "
                       "is made and what each of its dials does. Read its program and measure it; change nothing.";
            else
                return std::nullopt;
            juce::Array<juce::var> messages;
            messages.add(object({ { "role", "user" }, { "content", object({ { "type", "text" }, { "text", str(text) } }) } }));
            return object({ { "description", "" }, { "messages", messages } });
        }

        std::map<std::string, Instance> instances_;
        std::string chosen_;
        bool listedPlain_ = false;                       // the last tools/list had none of the plugin's tools
    };
}

int main()
{
    std::signal(SIGPIPE, SIG_IGN);
    std::ios::sync_with_stdio(false);
    Server server;
    return server.run();
}
