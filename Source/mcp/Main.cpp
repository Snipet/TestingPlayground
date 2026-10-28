// Source/mcp/Main.cpp: <plugin>MCP, this playground's MCP server, for Claude Desktop (or any MCP client) to act as the
// AI in the plugin: JSON-RPC 2.0 a line at a time on stdin and stdout (MCP's stdio transport), the plugin's instances
// reached through their sockets (mcp/McpProtocol.h, plugin/McpLink.h).
//
//   tools/list   list_instances, playground_guide, and the chat's own tools (ChatAgent::mcpTools) from an instance
//                running, else from the tools.json the last one left; each takes `instance` (an id or a track's name)
//   tools/call   list_instances: the instances open, each with its host, track, project, files; playground_guide: the
//                guide (the chat's system prompt, after a word on working from outside); any other: to the instance
//                named, else the one used last, else the only one open
// stdout carries the protocol only; what went wrong goes to stderr (Claude Desktop keeps it in its MCP logs).
#include "mcp/McpProtocol.h"

#include "Product.h"

#include <juce_core/juce_core.h>

#include <csignal>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <dirent.h>

namespace tp = testingplaygroundsynth;

namespace
{
    constexpr int kCallTimeoutMs = 180000;              // a tool may run a while (tune_dials, a long render)
    constexpr int kQuickTimeoutMs = 5000;               // info, tools, guide

    juce::var object(std::initializer_list<std::pair<const char*, juce::var>> props)
    {
        auto* o = new juce::DynamicObject();
        for (const auto& [k, v] : props)
            o->setProperty(k, v);
        return juce::var(o);
    }

    juce::var textContent(const std::string& text, bool error = false)
    {
        juce::Array<juce::var> content;
        content.add(object({ { "type", "text" }, { "text", juce::String::fromUTF8(text.c_str()) } }));
        return object({ { "content", content }, { "isError", error } });
    }

    struct Instance
    {
        std::string id, path;
        int fd = -1;
        std::unique_ptr<tp::mcp::LineReader> reader;
        int next = 1;
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
                const juce::var msg = juce::JSON::parse(juce::String::fromUTF8(line.c_str()));
                if (!msg.isObject())
                {
                    send(object({ { "jsonrpc", "2.0" }, { "id", juce::var() },
                                  { "error", object({ { "code", -32700 }, { "message", "Parse error" } }) } }));
                    continue;
                }
                handle(msg);
            }
            for (auto& [id, in] : instances_)
                close(in.fd);
            return 0;
        }

    private:
        void send(const juce::var& msg)
        {
            std::cout << juce::JSON::toString(msg, true).toStdString() << "\n" << std::flush;
        }

        void reply(const juce::var& id, const juce::var& result)
        {
            send(object({ { "jsonrpc", "2.0" }, { "id", id }, { "result", result } }));
        }

        void fail(const juce::var& id, int code, const std::string& message)
        {
            send(object({ { "jsonrpc", "2.0" }, { "id", id },
                          { "error", object({ { "code", code }, { "message", juce::String::fromUTF8(message.c_str()) } }) } }));
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
                reply(id, object({ { "prompts", juce::Array<juce::var>() } }));
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
            const std::string kind = tp::product::kIsInstrument ? "synth" : "effect";
            const std::string instructions =
                std::string(tp::product::kName) + " is an " + (tp::product::kIsInstrument ? "instrument" : "effect")
                + " plugin whose sound is a program in FMinus, a small language the plugin compiles as you write it. "
                  "With it open in a DAW (or as its Standalone app), these tools let you write that program, run it, "
                  "listen to it in numbers and pictures, and set its dials, as the plugin's own chat would. Call "
                  "playground_guide once before writing code: it teaches FMinus and how the plugin works. Then get_status "
                  "and read_file; write_file or edit_file; run_program; measure_output, view_sound and view_panel to check "
                  "your work; set_dial or tune_dials to leave the "
                + kind + " at good settings.";
            return object({ { "protocolVersion", version },
                            { "capabilities", object({ { "tools", object({ { "listChanged", true } }) } }) },
                            { "serverInfo", object({ { "name", tp::product::kName }, { "version", tp::product::kVersion } }) },
                            { "instructions", juce::String::fromUTF8(instructions.c_str()) } });
        }

        // ---- the instances ----------------------------------------------------------------------------------------------
        void discover()
        {
            const std::string dir = tp::mcp::folder();
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
            std::map<std::string, bool> present;
            for (const std::string& id : socks)
            {
                present[id] = true;
                if (instances_.count(id) != 0)
                    continue;
                const std::string path = dir + "/" + id + ".sock";
                const int fd = tp::mcp::connectTo(path);
                if (fd < 0)
                {
                    // Its process gone (a crash, a host killed): the socket goes too.
                    const int pid = juce::String(id).upToFirstOccurrenceOf("-", false, false).getIntValue();
                    if (pid > 0 && kill(pid, 0) != 0 && errno == ESRCH)
                        unlink(path.c_str());
                    continue;
                }
                Instance in;
                in.id = id;
                in.path = path;
                in.fd = fd;
                in.reader = std::make_unique<tp::mcp::LineReader>(fd);
                instances_.emplace(id, std::move(in));
            }
            for (auto it = instances_.begin(); it != instances_.end();)
                if (present.count(it->first) == 0)
                {
                    close(it->second.fd);
                    it = instances_.erase(it);
                }
                else
                    ++it;
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
                why = "there is no instance " + id;
                return std::nullopt;
            }
            Instance& in = it->second;
            const int n = in.next++;
            if (!tp::mcp::writeLine(in.fd, juce::JSON::toString(object({ { "id", n }, { "method", method }, { "params", params } }), true).toStdString()))
            {
                why = "the instance " + id + " has closed";
                drop(id);
                return std::nullopt;
            }
            std::string line;
            for (;;)
            {
                const int got = in.reader->next(line, timeoutMs);
                if (got <= 0)
                {
                    why = got == 0 ? "the instance " + id + " did not answer in time" : "the instance " + id + " has closed";
                    drop(id);
                    return std::nullopt;
                }
                const juce::var r = juce::JSON::parse(juce::String::fromUTF8(line.c_str()));
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
            const std::string dir = tp::mcp::folder();
            if (dir.empty())
                return {};
            const juce::var cached = juce::JSON::parse(juce::File(juce::String::fromUTF8(dir.c_str())).getChildFile("tools.json"));
            return cached.isArray() ? cached : juce::var();
        }

        juce::var toolList()
        {
            juce::Array<juce::var> out;
            const juce::var noArguments = object({ { "type", "object" }, { "properties", juce::var(new juce::DynamicObject()) } });
            out.add(object({ { "name", "list_instances" },
                             { "description", juce::String("The instances of ") + tp::product::kName
                                                  + " open now (in a DAW or as the Standalone app): each one's id, host, "
                                                    "track, project and files. Pass an id as `instance` to the other tools "
                                                    "when more than one is open." },
                             { "inputSchema", noArguments } }));
            out.add(object({ { "name", "playground_guide" },
                             { "description", "The guide to this plugin and to FMinus, the language its sound is written "
                                              "in. Read it once before writing or changing code." },
                             { "inputSchema", noArguments } }));
            const juce::var tools = pluginTools();
            listedPlain_ = tools.isVoid();
            if (const juce::Array<juce::var>* a = tools.getArray())
                for (const juce::var& t : *a)
                {
                    juce::var schema = juce::JSON::parse(juce::JSON::toString(t["inputSchema"]));   // a copy
                    if (juce::DynamicObject* s = schema.getDynamicObject())
                    {
                        if (!s->hasProperty("properties"))
                            s->setProperty("properties", juce::var(new juce::DynamicObject()));
                        if (juce::DynamicObject* props = s->getProperty("properties").getDynamicObject())
                            props->setProperty("instance", object({ { "type", "string" },
                                                                    { "description", "Which instance (an id from "
                                                                                     "list_instances, or its track's "
                                                                                     "name). Leave it out when one is "
                                                                                     "open." } }));
                    }
                    out.add(object({ { "name", t["name"] }, { "description", t["description"] }, { "inputSchema", schema } }));
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
            // The instance: the one named, else the one used last, else the only one.
            const std::string wanted = args["instance"].toString().toStdString();
            args.getDynamicObject()->removeProperty("instance");
            std::string id;
            std::string why;
            if (!wanted.empty())
            {
                for (auto& [i, in] : instances_)
                    if (i == wanted)
                        id = i;
                if (id.empty())
                    for (const std::string& i : ids())       // asking may drop one: a copy
                        if (const std::optional<juce::var> info = ask(i, "info", juce::var(), kQuickTimeoutMs, why);
                            info && juce::String((*info)["track"].toString()).equalsIgnoreCase(juce::String::fromUTF8(wanted.c_str())))
                        {
                            id = i;
                            break;
                        }
                if (id.empty())
                    return textContent("There is no instance \"" + wanted + "\" open: list_instances names those that are.", true);
            }
            else if (instances_.count(chosen_) != 0)
                id = chosen_;
            else if (instances_.size() == 1)
                id = instances_.begin()->first;
            else if (instances_.empty())
                return textContent(std::string("No ") + tp::product::kName + " is open: open it in your DAW (or its Standalone app), then call again.", true);
            else
            {
                std::string ids;
                for (auto& [i, in] : instances_)
                    ids += (ids.empty() ? "" : ", ") + i;
                return textContent("More than one is open (" + ids + "): pass instance (list_instances says which is which).", true);
            }
            const std::optional<juce::var> r = ask(id, "call", object({ { "name", juce::String::fromUTF8(name.c_str()) }, { "arguments", args } }),
                                                   kCallTimeoutMs, why);
            if (!r)
                return textContent("The call did not reach the plugin: " + why + ".", true);
            chosen_ = id;
            return *r;
        }

        juce::var listInstances()
        {
            std::string text;
            int n = 0;
            for (const std::string& id : ids())
            {
                std::string why;
                const std::optional<juce::var> info = ask(id, "info", juce::var(), kQuickTimeoutMs, why);
                if (!info)
                    continue;
                const juce::var& i = *info;
                ++n;
                const std::string track = i["track"].toString().toStdString();
                const std::string project = i["project"].toString().toStdString();
                std::string files;
                if (const juce::Array<juce::var>* f = i["files"].getArray())
                    for (const juce::var& v : *f)
                        files += (files.empty() ? "" : ", ") + v.toString().toStdString();
                const std::string host = i["host"].toString().toStdString();
                const bool known = !host.empty() && host != "Unknown";
                text += "- " + id + (id == chosen_ ? " (used last)" : "") + ": " + (known ? "in " + host : std::string("in its host"))
                      + (track.empty() ? std::string() : ", track \"" + track + "\"")
                      + (project.empty() ? std::string(", no project open")
                                         : ", project \"" + project + "\" v" + i["version"].toString().toStdString()
                                               + (i["preset"].toString().isEmpty() ? std::string() : " (preset " + i["preset"].toString().toStdString() + ")")
                                               + (static_cast<bool>(i["edited"]) ? ", edited" : ""))
                      + "; files: " + files + (static_cast<bool>(i["compiled"]) ? "" : "; the last run did not compile")
                      + (static_cast<bool>(i["busy"]) ? "; its own chat is busy" : "") + "\n";
            }
            if (n == 0)
                return textContent(std::string("No ") + tp::product::kName + " is open: open it in your DAW (or its Standalone app), then call again.");
            return textContent(std::to_string(n) + (n == 1 ? " instance" : " instances") + " of " + tp::product::kName + " open:\n" + text);
        }

        juce::var guide()
        {
            for (const std::string& id : ids())
            {
                std::string why;
                if (const std::optional<juce::var> g = ask(id, "guide", juce::var(), kQuickTimeoutMs, why); g && g->isString())
                    return textContent(g->toString().toStdString());
            }
            const std::string dir = tp::mcp::folder();
            const juce::File cached = juce::File(juce::String::fromUTF8(dir.c_str())).getChildFile("guide.md");
            if (!dir.empty() && cached.existsAsFile())
                return textContent(cached.loadFileAsString().toStdString());
            return textContent(std::string("The guide comes from the plugin: open ") + tp::product::kName + " once, then call again.", true);
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
