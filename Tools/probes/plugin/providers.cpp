// PROBE name=plugin.providers timeout=240
//
// plugin.providers: the chat with other model providers (plugin/Providers.h; ChatAgent.h, "Other providers") over a
// real processor, against scripted servers (FakeClaude.h: OpenAiSse, GeminiSse and the models lists, in the shapes
// the providers' API references give): nothing reaches the network, and the keys' folder is the probe's own.
//   URL       a server's address made canonical (https anywhere; http only on this Mac or the local network; no user,
//             query or fragment; no slash at the end), its origin, host and path; the presets by their address
//   STORE     another provider's key sealed and bound to its server: read back for that origin only, never in clear
//             in its file or its name; an Anthropic key refused for another provider; forgotten
//   DEFAULT   a new chat talks to Anthropic, as before: its provider, its name, a key needed
//   OPENROUTER  set with its key: the models listed from its /models (Bearer key; the newest first, a model without
//             tools left out, one without pictures marked so), none chosen for the user (a turn asks for one, sending
//             nothing), no Anthropic option; a turn: the
//             request to openrouter.ai's /api/v1/chat/completions with the key as a Bearer token alone, streamed with
//             its usage, the full system prompt as the first message, the plugin's tools as functions (an array's
//             items said), none of Anthropic's fields; the answer's reasoning shown and never kept, its text, a tool
//             call in pieces run and answered by its id (made here), the footer's tokens; the history in the Messages
//             API's shape; another model chosen is kept, Anthropic's model untouched; a model its list says takes no
//             pictures gets a line of text for each, and the chat says so once
//   OPENAI    OpenAI's own list: its chat models with tools alone (no speech, image or embedding model, none the
//             Responses API alone serves, none that calls no tools); a key refused for its shape
//   LOCAL     Ollama on this Mac, chosen after a Stop: its list read all the same; the first it lists taken; no key,
//             no Authorization; the prompt's short form (its context unsaid); calls sent
//             whole without an index and a "stop" after them still run; a server that read far less than was sent is
//             noted once
//   PICTURES  a model that refuses a picture: the request made again with a line of text in its place, a note, and
//             the model takes none from then on
//   LIMITS    an answer cut at its length: the turn goes on with the plugin's line
//   CHECKS    a check that cannot pass sends the turn on twice, as with Anthropic
//   GEMINI    set with its key: its models (not its speech or embedding ones; the newest version first); a turn to
//             /v1beta/models/<model>:streamGenerateContent?alt=sse with x-goog-api-key, systemInstruction, contents
//             (user and model), the tools' functionDeclarations with JSON Schema; a call with its thought signature
//             run, the signature kept on its tool_use block and sent back on its functionCall, its result a
//             functionResponse by name; a 400 that refuses parametersJsonSchema: Gemini's own subset from then on
//             (types in upper case, no additionalProperties; an enum with no type a STRING: the projects tool's
//             action, compare's dials, measure_output's spectrum and run_function's on keep theirs; an array whose items
//             are left open, run_function's args and write_file's files, one of strings, each a JSON value written out,
//             read back before the tool runs: a write_file of files on it writes them); a key it calls not valid:
//             refused, said
//   KEPT      a new processor talks to the provider chosen last, its model kept
//   BACK      Anthropic again, with the planner chosen before the Gemini turns: its request as before (the full
//             prompt cached, thinking, its key alone), the history without another provider's thought_signature in the
//             builder's request and in the planner's, every tool id in its alphabet
//   ERRORS    the provider named in its errors; a local server not running asked about
//   SERVERS   a turn with no model whose list could not be read says why and sends nothing (LM Studio not running:
//             is it running?; a key the server refused: the key's problem); a redirect is never followed (the one
//             request, to the server chosen; the error says where it pointed); a model the server says cannot chat
//             with tools (OpenAI's Responses-only) goes off the menu and is no longer chosen
//   SWITCH    another provider chosen while a turn waits for the models list (nothing sent yet): the turn is the new
//             provider's, its request and its model both (never one's request with the other's model)
//   EFFORT    the thinking levels of another provider's models: OpenRouter's from its list's reasoning object (in the
//             menu's order; Off left out where reasoning is mandatory; its default, Off where it reasons only when
//             asked; none without the object, none from another server whatever its list holds), OpenAI's and Gemini's
//             by the model's family (o3 and gpt-5 yes, gpt-4o and gpt-5-chat-latest no; Gemini 3 by level, 2.5 by
//             budget, Off on 2.5 flash alone), a local server none; the bodies (OpenRouter's reasoning.effort alone and
//             no max_tokens, OpenAI's reasoning_effort, Gemini's thinkingLevel or thinkingBudget never both, with
//             includeThoughts, nothing when none is chosen); through the agent: the provider's note, the default shown
//             and nothing sent, a level chosen sent and kept per provider and model (chat.json, and a new processor's
//             turn), OpenRouter's reasoning_details shown once and never kept, a model's refusal (a canned 400: OpenAI's,
//             Gemini's) asked again at once without the level, its pills gone for the session, a note, no failed turn
//   REDIRECT  the real transport (ClaudeApi.cpp) against two servers of the probe's own on 127.0.0.1: a 307 from
//             the first is returned as it came, its location said, and the second is never reached
//   TRANSPORT the real transport against the probe's own server: an https:// address is spoken to in TLS (its first
//             byte a handshake record: on Linux JUCE's own client never connected to one); a 429's body whole and its
//             retry-after; a stream stopped by `cancel` ends soon, "stopped"; (Linux) a 200's body reaches onChunk
//             piece by piece as it comes, the first piece before the server sends the next
#include "Probe.h"

#include "FakeClaude.h"

#include "plugin/ChatAgent.h"
#include "plugin/ChatStore.h"
#include "plugin/ClaudeApi.h"
#include "plugin/Processor.h"
#include "plugin/Providers.h"

#include "mcp/McpGuide.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <juce_graphics/juce_graphics.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace pg = playground;
namespace claude = playground::claude;
namespace pr = playground::providers;

namespace
{
    // A request read whole from the probe's own server's connection: its headers, then its body by its length ("" when
    // it did not come within the time).
    std::string readRequest(juce::StreamingSocket& c)
    {
        std::string got;
        char buffer[4096];
        std::size_t need = std::string::npos;
        while (need == std::string::npos || got.size() < need)
        {
            if (c.waitUntilReady(true, 5000) != 1)
                return {};
            const int n = c.read(buffer, static_cast<int>(sizeof buffer), false);
            if (n <= 0)
                break;
            got.append(buffer, static_cast<std::size_t>(n));
            if (const std::size_t end = got.find("\r\n\r\n"); need == std::string::npos && end != std::string::npos)
            {
                std::size_t length = 0;
                const juce::String head = juce::String(got.substr(0, end)).toLowerCase();
                if (const int at = head.indexOf("content-length:"); at >= 0)
                    length = static_cast<std::size_t>(head.substring(at + 15).getIntValue());
                need = end + 4 + length;
            }
        }
        return got;
    }

    void sendText(juce::StreamingSocket& c, const std::string& text) { c.write(text.data(), static_cast<int>(text.size())); }

    std::string str(const juce::var& v, const char* key) { return v.getProperty(key, "").toString().toStdString(); }

    juce::Array<juce::var> arr(const juce::var& v)
    {
        if (const auto* a = v.getArray())
            return *a;
        return {};
    }

    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    juce::var input(std::initializer_list<std::pair<const char*, juce::var>> fields) { return claude::object(fields); }

    fake::Response stream(const std::string& events)
    {
        fake::Response r;
        r.stream = events;
        return r;
    }

    std::string lastText(const pg::ChatState& s, pg::ChatEntry::Kind kind)
    {
        for (auto it = s.entries.rbegin(); it != s.entries.rend(); ++it)
            if (it->kind == kind)
                return it->text;
        return {};
    }

    std::string allOf(const pg::ChatState& s, pg::ChatEntry::Kind kind)
    {
        std::string out;
        for (const pg::ChatEntry& e : s.entries)
            if (e.kind == kind)
                out += e.text + "\n";
        return out;
    }

    void print(const char* what, const pg::ChatState& s)
    {
        std::printf("%s:\n", what);
        for (const pg::ChatEntry& e : s.entries)
            std::printf("  %d %s\n", static_cast<int>(e.kind), e.text.substr(0, 120).c_str());
    }

    // Every tool call's id in the Messages API's alphabet.
    bool idsPlain(const juce::Array<juce::var>& messages)
    {
        for (const juce::var& m : messages)
            for (const juce::var& b : arr(m.getProperty("content", {})))
                if (str(b, "type") == "tool_use")
                {
                    const std::string id = str(b, "id");
                    if (id.empty() || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
                            return std::isalnum(c) != 0 || c == '_' || c == '-';
                        }))
                        return false;
                }
        return true;
    }

    // Whether any object in `v` (deep) has the property `key`.
    bool anyHas(const juce::var& v, const char* key)
    {
        if (const auto* o = v.getDynamicObject())
        {
            if (o->hasProperty(key))
                return true;
            for (const auto& kv : o->getProperties())
                if (anyHas(kv.value, key))
                    return true;
        }
        for (const juce::var& x : arr(v))
            if (anyHas(x, key))
                return true;
        return false;
    }

    // An array schema anywhere in `v` without its items.
    bool arrayWithoutItems(const juce::var& v)
    {
        if (const auto* o = v.getDynamicObject())
        {
            if (o->getProperty("type").toString() == "array" && !o->hasProperty("items"))
                return true;
            for (const auto& kv : o->getProperties())
                if (arrayWithoutItems(kv.value))
                    return true;
        }
        for (const juce::var& x : arr(v))
            if (arrayWithoutItems(x))
                return true;
        return false;
    }

    int toolCount() { return arr(pg::ChatAgent::toolDefinitions()).size(); }

    std::string levelProgram()
    {
        const std::string param = "    param float level [[fminus::range(0, 2)]] [[fminus::default(1)]];\n";
        if constexpr (pg::product::kIsInstrument)
            return "module Voice\n{\n" + param +
                   "    void start(float n, float v) {}\n"
                   "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n";
        else
            return "module Effect\n{\n" + param +
                   "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        for (int i = 0; i < n; ++i)\n        {\n"
                   "            left[i] *= level[i];\n            right[i] *= level[i];\n        }\n    }\n};\n";
    }

    // An orange PNG in a folder of the probe's own: a picture to attach.
    juce::File pictureFile(const char* folder)
    {
        const juce::File dir(juce::String(probe::outPath(folder)));
        dir.deleteRecursively();
        dir.createDirectory();
        juce::Image img(juce::Image::RGB, 64, 48, true);
        {
            juce::Graphics g(img);
            g.fillAll(juce::Colours::orange);
        }
        const juce::File f = dir.getChildFile("panel.png");
        juce::FileOutputStream out(f);
        juce::PNGImageFormat().writeImageToStream(img, out);
        return f;
    }

    std::string fileText(const juce::File& f)
    {
        juce::MemoryBlock b;
        f.loadFileAsData(b);
        return std::string(static_cast<const char*>(b.getData()), b.getSize());
    }
}

PROBE(plugin, providers)
{
    const funkgui::HeadlessGuiScope gui;
    const juce::File keys = juce::File(juce::String(probe::outPath("providers-keys")));
    keys.deleteRecursively();
    pg::chatstore::setDirectoryForTests(keys);
    std::string why;

    // ---- URL -------------------------------------------------------------------------------------------------------------
    {
        P.eq("url.https", pr::canonical(" https://OpenRouter.ai/api/v1/ ", why) == "https://openrouter.ai/api/v1" ? 1 : 0, 1);
        P.eq("url.local_http", pr::canonical("http://localhost:11434/v1", why) == "http://localhost:11434/v1" ? 1 : 0, 1);
        P.eq("url.lan_http", pr::canonical("http://192.168.1.20:1234/v1", why).empty() ? 0 : 1, 1);
        P.eq("url.loopback6", pr::canonical("http://[::1]:8080/v1", why).empty() ? 0 : 1, 1);
        why.clear();
        P.eq("url.remote_http_refused", pr::canonical("http://api.example.com/v1", why).empty() && has(why, "https") ? 1 : 0, 1);
        P.eq("url.public_ip_http_refused", pr::canonical("http://8.8.8.8/v1", why).empty() ? 1 : 0, 1);
        P.eq("url.user_refused", pr::canonical("https://me:pw@api.example.com/v1", why).empty() ? 1 : 0, 1);
        P.eq("url.query_refused", pr::canonical("https://api.example.com/v1?key=x", why).empty() ? 1 : 0, 1);
        P.eq("url.scheme_refused", pr::canonical("ftp://api.example.com", why).empty() ? 1 : 0, 1);
        P.eq("url.parts", pr::originOf("https://openrouter.ai/api/v1") == "https://openrouter.ai" && pr::pathOf("https://openrouter.ai/api/v1") == "/api/v1"
                              && pr::hostOf("http://localhost:11434/v1") == "localhost:11434" ? 1 : 0, 1);
        P.eq("url.local", pr::isLocal("http://localhost:1234/v1") && pr::isLocal("http://studio.local:1234/v1")
                              && pr::isLocal("http://10.0.0.5/v1") && !pr::isLocal("https://openrouter.ai/api/v1") ? 1 : 0, 1);
        pg::ChatProvider ollama{ "openai", "http://localhost:11434/v1/", "" }, custom{ "openai", "https://llm.example.org/v1", "" };
        P.eq("url.preset_names", pr::nameOf(ollama) == "Ollama (this Mac)" && pr::nameOf(custom) == "llm.example.org"
                                     && pr::nameOf({ "gemini", "", "" }) == "Google Gemini" && pr::nameOf({}) == "Anthropic" ? 1 : 0, 1);
        P.eq("url.keyless", pr::keyless(ollama) && !pr::keyless(custom) && !pr::keyless({ "gemini", "", "" }) ? 1 : 0, 1);
    }

    // ---- STORE -----------------------------------------------------------------------------------------------------------
    {
        const std::string key = fake::fakeOpenRouterKey();
        P.eq("store.saved", pg::chatstore::saveProviderKey("openai", "https://openrouter.ai", key, &why) ? 1 : 0, 1);
        P.eq("store.has", pg::chatstore::hasProviderKey("openai", "https://openrouter.ai") ? 1 : 0, 1);
        P.eq("store.loaded", pg::chatstore::loadProviderKey("openai", "https://openrouter.ai").value_or("") == key ? 1 : 0, 1);
        P.eq("store.other_server_none", pg::chatstore::loadProviderKey("openai", "https://evil.example").has_value()
                                            || pg::chatstore::hasProviderKey("openai", "https://evil.example") ? 1 : 0, 0);
        P.eq("store.other_kind_none", pg::chatstore::loadProviderKey("gemini", "https://openrouter.ai").has_value() ? 1 : 0, 0);
        bool clear = true, named = false;
        for (const juce::File& f : keys.findChildFiles(juce::File::findFiles, false, "key-*.bin"))
        {
            named = true;
            const std::string bytes = fileText(f);
            clear = clear && !has(bytes, key.substr(0, 12)) && !has(bytes, "openrouter") && !has(f.getFileName().toStdString(), "openrouter");
        }
        P.eq("store.sealed", named && clear ? 1 : 0, 1);
        why.clear();
        P.eq("store.anthropic_key_refused", !pr::plausibleKey({ "openai", "https://openrouter.ai/api/v1", "" }, fake::fakeKey(), why)
                                                && has(why, "only to Anthropic") ? 1 : 0, 1);
        P.eq("store.forgot", pg::chatstore::forgetProviderKey("openai", "https://openrouter.ai")
                                 && !pg::chatstore::hasProviderKey("openai", "https://openrouter.ai") ? 1 : 0, 1);
    }

    pg::Processor processor;
    pg::ChatAgent& agent = processor.chatAgent();
    auto script = std::make_shared<fake::Script>();
    script->models = fake::modelsBody({ { "claude-opus-5-5", "Claude Opus 5.5", true, { "low", "medium", "high", "xhigh", "max" } },
                                        { "claude-sonnet-5-5", "Claude Sonnet 5.5", true, { "low", "medium", "high", "xhigh" } } });
    agent.setTransport(std::make_unique<fake::Transport>(script));

    // ---- DEFAULT ---------------------------------------------------------------------------------------------------------
    {
        const pg::ChatState s = agent.chatState();
        P.eq("default.anthropic", s.provider.kind == "anthropic" && s.providerName == "Anthropic" && !s.keyless && !s.ready() ? 1 : 0, 1);
        agent.setModel("claude-sonnet-5-5");      // Anthropic's model: kept as it was whatever the other providers do
        P.eq("default.model", agent.chatState().model == "claude-sonnet-5-5" ? 1 : 0, 1);
        agent.setPlanner("claude-opus-5-5");      // kept (chat.json) while other providers chat; BACK uses it
        P.eq("default.planner", agent.chatState().planner == "claude-opus-5-5" ? 1 : 0, 1);
    }

    // ---- OPENROUTER ------------------------------------------------------------------------------------------------------
    const std::string orKey = fake::fakeOpenRouterKey();
    {
        script->gets["/api/v1/models"] = { 200, fake::openAiModels({ { "anthropic/claude-probe", "Claude via OpenRouter", 1762000000, 200000, true, true },
                                                                     { "meta/llama-probe", "Llama (text)", 1761000000, 131072, true, false },
                                                                     { "acme/no-tools", "No tools", 1763000000, 32768, false, true } },
                                                                   true) };
        pg::ChatProvider p{ "openai", "https://openrouter.ai/api/v1/", "" };
        why.clear();
        P.eq("openrouter.needs_key", agent.setProvider(p, "", why) ? 1 : 0, 0);
        P.eq("openrouter.anthropic_key_refused", agent.setProvider(p, fake::fakeKey(), why) ? 1 : 0, 0);
        P.eq("openrouter.set", agent.setProvider(p, orKey, why) ? 1 : 0, 1);
        agent.waitIdle();
        const pg::ChatState s = agent.chatState();
        P.eq("openrouter.provider", s.provider.kind == "openai" && s.provider.base == "https://openrouter.ai/api/v1" && s.providerName == "OpenRouter" ? 1 : 0, 1);
        P.eq("openrouter.ready", s.ready() && s.hasKey && !s.keyless ? 1 : 0, 1);
        P.eq("openrouter.models_asked", script->lastHost("GET /api/v1/models") == "https://openrouter.ai"
                                            && script->lastHeader("GET /api/v1/models", "Authorization") == "Bearer " + orKey ? 1 : 0, 1);
        P.eq("openrouter.models_with_tools", static_cast<int64_t>(s.models.size()), 2);
        P.eq("openrouter.newest_first", s.models.size() == 2 && s.models[0].id == "anthropic/claude-probe" && s.model.empty() ? 1 : 0, 1);
        P.eq("openrouter.context_and_pictures", s.models.size() == 2 && s.models[0].context == 200000 && s.models[0].images && !s.models[1].images ? 1 : 0, 1);
        P.eq("openrouter.no_anthropic_options", !s.fast && !s.web && s.planner.empty() && s.effort.empty()
                                                    && std::none_of(s.models.begin(), s.models.end(), [](const pg::ChatModel& m) { return m.fast || m.web || m.compaction || !m.efforts.empty(); })
                                                    ? 1 : 0, 1);
        const std::string json = fileText(keys.getChildFile("chat.json"));
        P.eq("openrouter.choice_kept", has(json, "\"provider\": \"openai\"") && has(json, "openrouter.ai") && !has(json, orKey.substr(0, 12)) ? 1 : 0, 1);
        P.eq("openrouter.notes", has(allOf(s, pg::ChatEntry::Kind::note), "talks to OpenRouter now")
                                     && has(allOf(s, pg::ChatEntry::Kind::note), "goes to openrouter.ai alone")
                                     && has(allOf(s, pg::ChatEntry::Kind::note), "the model you choose in the model menu") ? 1 : 0, 1);
        // No model chosen: the turn asks for one and sends nothing (the newest of a long list is anyone's guess).
        const int asked = script->postCount();
        agent.send("hello");
        agent.waitIdle();
        P.eq("openrouter.choose_first", has(lastText(agent.chatState(), pg::ChatEntry::Kind::error), "Choose a model in the model menu first: OpenRouter lists 2")
                                            && script->postCount() == asked && !agent.chatState().busy ? 1 : 0, 1);
        agent.setModel("anthropic/claude-probe");

        const int before = script->postCount();
        script->add(stream(fake::OpenAiSse("anthropic/claude-probe").reasoning("Let me look at the program.").text("Reading it.")
                               .call(0, "call_server1", "read_file", "{\"name\":\"program.fm\"}").finish("tool_calls").usage(12000, 80, 3000).done().str()));
        script->add(stream(fake::OpenAiSse("anthropic/claude-probe").text("It is the init program.").finish("stop").usage(14000, 20, 12000).done().str()));
        agent.send("what does it do?");
        agent.waitIdle();
        const pg::ChatState t = agent.chatState();
        print("openrouter", t);
        P.eq("openrouter.requests", script->postCount() - before, 2);
        const int last = script->last("POST /api/v1/chat/completions");
        P.eq("openrouter.endpoint", last >= 0 && script->lastHost("POST /api/v1/chat/completions") == "https://openrouter.ai" ? 1 : 0, 1);
        P.eq("openrouter.bearer_alone", script->lastHeader("POST /api/v1/chat/completions", "Authorization") == "Bearer " + orKey
                                            && !script->lastHas("POST /api/v1/chat/completions", "x-api-key")
                                            && !script->lastHas("POST /api/v1/chat/completions", "anthropic-version")
                                            && !script->lastHas("POST /api/v1/chat/completions", "anthropic-beta") ? 1 : 0, 1);
        const juce::var b0 = claude::parseJson(script->body(before)), b1 = claude::parseJson(script->body(before + 1));
        P.eq("openrouter.streamed", static_cast<bool>(b0.getProperty("stream", false))
                                        && static_cast<bool>(b0.getProperty("stream_options", {}).getProperty("include_usage", false))
                                        && str(b0, "model") == "anthropic/claude-probe" ? 1 : 0, 1);
        const juce::Array<juce::var> m0 = arr(b0.getProperty("messages", {}));
        P.eq("openrouter.system_first", !m0.isEmpty() && str(m0[0], "role") == "system" && str(m0[0], "content") == pg::ChatAgent::systemPrompt() ? 1 : 0, 1);
        P.eq("openrouter.state_sent", m0.size() == 2 && str(m0[1], "role") == "user" && has(str(m0[1], "content"), "<plugin_state>")
                                          && has(str(m0[1], "content"), "what does it do?") ? 1 : 0, 1);
        P.eq("openrouter.no_anthropic_fields", !b0.hasProperty("thinking") && !b0.hasProperty("system") && !anyHas(b0, "cache_control")
                                                   && !b0.hasProperty("output_config") && !b0.hasProperty("context_management")
                                                   && !b0.hasProperty("fallbacks") && !anyHas(b0, "eager_input_streaming") ? 1 : 0, 1);
        const juce::Array<juce::var> tools = arr(b0.getProperty("tools", {}));
        bool functions = !tools.isEmpty();
        for (const juce::var& t2 : tools)
            functions = functions && str(t2, "type") == "function" && !str(t2.getProperty("function", {}), "name").empty()
                     && t2.getProperty("function", {}).getProperty("parameters", {}).isObject();
        P.eq("openrouter.tools", tools.size() == toolCount() && functions ? 1 : 0, 1);
        P.eq("openrouter.array_items", arrayWithoutItems(b0.getProperty("tools", {})) ? 0 : 1, 1);
        // The tool call answered by its id, in OpenAI's shape.
        const juce::Array<juce::var> m1 = arr(b1.getProperty("messages", {}));
        const juce::var assistant = m1.size() >= 3 ? m1[2] : juce::var();
        const juce::Array<juce::var> calls = arr(assistant.getProperty("tool_calls", {}));
        const std::string callId = calls.isEmpty() ? std::string() : str(calls[0], "id");
        P.eq("openrouter.call_sent_back", str(assistant, "role") == "assistant" && str(assistant, "content") == "Reading it." && calls.size() == 1
                                              && str(calls[0], "type") == "function" && str(calls[0].getProperty("function", {}), "name") == "read_file"
                                              && claude::parseJson(str(calls[0].getProperty("function", {}), "arguments")).getProperty("name", "").toString() == "program.fm"
                                              ? 1 : 0, 1);
        P.eq("openrouter.call_id_made_here", callId.rfind("call_", 0) == 0 && callId.size() == 29 && callId != "call_server1" ? 1 : 0, 1);
        P.eq("openrouter.result_by_id", m1.size() == 4 && str(m1[3], "role") == "tool" && str(m1[3], "tool_call_id") == callId
                                            && has(str(m1[3], "content"), "program.fm") ? 1 : 0, 1);
        P.eq("openrouter.history_kept", m1.size() == 4 && claude::toJson(m1[0]) == claude::toJson(m0[0]) && claude::toJson(m1[1]) == claude::toJson(m0[1]) ? 1 : 0, 1);
        P.eq("openrouter.reasoning_shown", lastText(t, pg::ChatEntry::Kind::thinking) == "Let me look at the program." ? 1 : 0, 1);
        P.eq("openrouter.answer", has(lastText(t, pg::ChatEntry::Kind::assistant), "It is the init program.") && !t.busy ? 1 : 0, 1);
        P.eq("openrouter.not_cut", has(allOf(t, pg::ChatEntry::Kind::note), "cut the conversation") ? 1 : 0, 0);   // a remote server says so itself
        P.eq("openrouter.footer", has(t.usage, "14K tokens in (12K cached), 20 out") ? 1 : 0, 1);
        const juce::var saved = claude::parseJson(agent.saveState());
        const juce::Array<juce::var> history = arr(saved.getProperty("messages", {}));
        bool noThinking = true, toolUse = false;
        for (const juce::var& m : history)
            for (const juce::var& blk : arr(m.getProperty("content", {})))
            {
                noThinking = noThinking && str(blk, "type") != "thinking";
                toolUse = toolUse || (str(blk, "type") == "tool_use" && str(blk, "id") == callId);
            }
        P.eq("openrouter.history_shape", history.size() == 4 && noThinking && toolUse && idsPlain(history) ? 1 : 0, 1);

        agent.setModel("meta/llama-probe");
        const std::string after = fileText(keys.getChildFile("chat.json"));
        P.eq("openrouter.model_kept", agent.chatState().model == "meta/llama-probe" && has(after, "meta/llama-probe")
                                          && has(after, "\"model\": \"claude-sonnet-5-5\"") ? 1 : 0, 1);
        agent.setEffort("high");                  // Anthropic's alone: nothing changes
        agent.setFast(true);
        agent.setPlanner("claude-opus-5-5");
        const pg::ChatState u = agent.chatState();
        P.eq("openrouter.options_refused", !u.fast && u.planner.empty() && u.effort.empty() ? 1 : 0, 1);

        // A model its list says takes no pictures: a line of text for each, said once.
        agent.attach({ pictureFile("providers-text-only").getFullPathName().toStdString() });
        agent.waitIdle();
        const int pb = script->postCount();
        script->add(stream(fake::OpenAiSse("meta/llama-probe").text("I read your words.").finish("stop").usage(9000, 8).done().str()));
        script->add(stream(fake::OpenAiSse("meta/llama-probe").text("Still words.").finish("stop").usage(9000, 8).done().str()));
        agent.send("make the panel look like this");
        agent.waitIdle();
        agent.send("and again");
        agent.waitIdle();
        const std::string notes = allOf(agent.chatState(), pg::ChatEntry::Kind::note);
        const auto count = [](const std::string& text, const std::string& part) {
            int n = 0;
            for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1))
                ++n;
            return n;
        };
        P.eq("openrouter.text_only_request", script->postCount() - pb == 2 && !has(script->body(pb), "\"image_url\"")
                                                 && has(script->body(pb), "takes no pictures") ? 1 : 0, 1);
        P.eq("openrouter.text_only_noted_once", count(notes, "The model (meta/llama-probe) takes no pictures"), 1);
        agent.setModel("anthropic/claude-probe");
    }

    // ---- OPENAI ----------------------------------------------------------------------------------------------------------
    {
        const std::string list = fake::openAiModels({ { "gpt-probe-5", "", 1762000000 }, { "gpt-image-probe", "", 1763000000 },
                                                       { "text-embedding-probe", "", 1764000000 }, { "o4-probe", "", 1761000000 },
                                                       { "whisper-probe", "", 1765000000 }, { "gpt-probe-realtime", "", 1766000000 },
                                                       { "dall-e-probe", "", 1767000000 }, { "gpt-probe-5-pro", "", 1768000000 },
                                                       { "o3-pro-2025-06-10", "", 1768100000 }, { "gpt-probe-5-codex", "", 1768200000 },
                                                       { "o3-deep-research", "", 1768300000 }, { "chatgpt-probe-latest", "", 1768400000 },
                                                       { "gpt-probe-5-chat-latest", "", 1768500000 }, { "o1-mini", "", 1768600000 } });
        const std::vector<pg::ChatModel> found = pr::modelsOf({ "openai", "https://api.openai.com/v1", "" }, list);
        P.eq("openai.chat_models", found.size() == 2 && found[0].id == "gpt-probe-5" && found[1].id == "o4-probe" ? 1 : 0, 1);
        why.clear();
        P.eq("openai.short_key_refused", pr::plausibleKey({ "openai", "https://api.openai.com/v1", "" }, "sk-1", why) ? 1 : 0, 0);
        P.eq("openai.spaced_key_refused", pr::plausibleKey({ "openai", "https://api.openai.com/v1", "" }, "sk-proj abc def ghi", why) ? 1 : 0, 0);
        P.eq("openai.endpoint", pr::endpoint({ "openai", "https://api.openai.com/v1", "" }, "gpt-probe-5").host == "https://api.openai.com"
                                    && pr::endpoint({ "openai", "https://api.openai.com/v1", "" }, "").path == "/v1/chat/completions" ? 1 : 0, 1);
        P.eq("openai.picks_none", pr::picksFirst({ "openai", "https://api.openai.com/v1", "" }) || pr::picksFirst({ "openai", "https://openrouter.ai/api/v1", "" })
                                      || !pr::picksFirst({ "gemini", "", "" }) || !pr::picksFirst({ "openai", "http://localhost:11434/v1", "" }) ? 1 : 0, 0);
        P.eq("openai.responses_only_said", pr::aboutModel(404, "This model is only supported in v1/responses and not in v1/chat/completions.")
                                               && pr::aboutModel(404, "No endpoints found that support tool use.")
                                               && pr::aboutModel(400, "registry.ollama.ai/library/gemma:2b does not support tools")
                                               && !pr::aboutModel(400, "model qwen does not support image input") && !pr::aboutModel(429, "v1/responses") ? 1 : 0, 1);
    }

    // ---- LOCAL -----------------------------------------------------------------------------------------------------------
    {
        script->gets["/v1/models"] = { 200, fake::openAiModels({ { "qwen-probe:32b", "", 1762400000 }, { "llava-probe:13b", "", 1762300000 },
                                                                 { "nomic-embed-probe", "", 1762500000 } }) };
        why.clear();
        agent.stop();                             // a Stop with no turn under way: the list asked for next is read anyway
        P.eq("local.set_without_key", agent.setProvider({ "openai", "http://localhost:11434/v1", "" }, "", why) ? 1 : 0, 1);
        agent.waitIdle();
        const pg::ChatState s = agent.chatState();
        P.eq("local.ready", s.ready() && s.keyless && !s.hasKey && s.providerName == "Ollama (this Mac)" ? 1 : 0, 1);
        P.eq("local.models", s.models.size() == 2 && s.model == "qwen-probe:32b" ? 1 : 0, 1);
        P.eq("local.no_levels", s.effort.empty()
                                    && std::none_of(s.models.begin(), s.models.end(), [](const pg::ChatModel& m) { return !m.efforts.empty(); })
                                    ? 1 : 0, 1);
        P.eq("local.models_no_auth", script->lastHost("GET /v1/models") == "http://localhost:11434"
                                         && !script->lastHas("GET /v1/models", "Authorization") ? 1 : 0, 1);

        const int before = script->postCount();
        const juce::String program = juce::String(levelProgram());
        script->add(stream(fake::OpenAiSse("qwen-probe:32b").text("Writing it.")
                               .call(0, "call_0", "write_file", claude::toJson(input({ { "name", "program.fm" }, { "content", program } })), true)
                               .call(0, "call_1", "run_program", "{}", true)
                               .finish("stop").usage(2048, 300).done().str()));
        script->add(stream(fake::OpenAiSse("qwen-probe:32b").text("Done: a level dial.").finish("stop").usage(2048, 10).done().str()));
        agent.send("make a level control");
        agent.waitIdle();
        processor.settle();
        const pg::ChatState t = agent.chatState();
        print("local", t);
        const pg::Draft d = processor.editorPort().draft();
        P.eq("local.requests", script->postCount() - before, 2);
        P.eq("local.endpoint", script->lastHost("POST /v1/chat/completions") == "http://localhost:11434"
                                   && !script->lastHas("POST /v1/chat/completions", "Authorization") ? 1 : 0, 1);
        const juce::var b0 = claude::parseJson(script->body(before));
        const juce::Array<juce::var> m0 = arr(b0.getProperty("messages", {}));
        const std::string& shortForm = pg::ChatAgent::shortPrompt();
        std::printf("prompt: full %zu bytes, short %zu bytes\n", pg::ChatAgent::systemPrompt().size(), shortForm.size());
        P.eq("local.short_prompt", !m0.isEmpty() && str(m0[0], "content") == shortForm ? 1 : 0, 1);
        P.le("local.short_prompt_size", static_cast<double>(shortForm.size()), 0.5 * static_cast<double>(pg::ChatAgent::systemPrompt().size()));
        namespace guide = pg::mcp::guide;
        P.eq("local.short_prompt_says", has(shortForm, "SHORT FORM") && has(shortForm, "THE PROGRAM'S CONTRACT")
                                            && guide::detail::heading(shortForm, guide::kInterface) == std::string::npos
                                            && guide::detail::heading(pg::ChatAgent::systemPrompt(), guide::kInterface) != std::string::npos
                                            && guide::detail::heading(shortForm, guide::kLibrary) == std::string::npos ? 1 : 0, 1);
        P.eq("local.whole_calls_ran", !d.files.empty() && d.files[0].text == levelProgram() && processor.editorPort().running() == d.files ? 1 : 0, 1);
        const auto toolMessages = [&](int i) {
            int n = 0;
            for (const juce::var& m : arr(claude::parseJson(script->body(i)).getProperty("messages", {})))
                n += str(m, "role") == "tool" ? 1 : 0;
            return n;
        };
        P.eq("local.two_results", toolMessages(before + 1) - toolMessages(before), 2);
        P.eq("local.tools_shown", has(allOf(t, pg::ChatEntry::Kind::tool), "wrote program.fm") ? 1 : 0, 1);
        P.eq("local.answer", lastText(t, pg::ChatEntry::Kind::assistant) == "Done: a level dial." ? 1 : 0, 1);
        int cut = 0;
        for (const pg::ChatEntry& e : t.entries)
            cut += e.kind == pg::ChatEntry::Kind::note && has(e.text, "cut the conversation") ? 1 : 0;
        P.eq("local.cut_noted_once", cut, 1);
    }

    // ---- PICTURES ----------------------------------------------------------------------------------------------------------
    {
        const juce::File f = pictureFile("providers-pictures");
        agent.attach({ f.getFullPathName().toStdString() });
        agent.waitIdle();
        const int before = script->postCount();
        script->add(fake::Response::failure(400, "invalid_request_error", "model qwen-probe:32b does not support image input"));
        script->add(stream(fake::OpenAiSse("qwen-probe:32b").text("I cannot see pictures.").finish("stop").usage(9000, 8).done().str()));
        agent.send("make the panel look like this");
        agent.waitIdle();
        const pg::ChatState s = agent.chatState();
        P.eq("pictures.requests", script->postCount() - before, 2);
        P.eq("pictures.first_had_it", has(script->body(before), "\"image_url\"") && has(script->body(before), "data:image/") ? 1 : 0, 1);
        P.eq("pictures.again_without", !has(script->body(before + 1), "\"image_url\"") && has(script->body(before + 1), "takes no pictures") ? 1 : 0, 1);
        P.eq("pictures.noted", has(allOf(s, pg::ChatEntry::Kind::note), "takes no pictures") ? 1 : 0, 1);
        bool marked = false;
        for (const pg::ChatModel& m : s.models)
            marked = marked || (m.id == "qwen-probe:32b" && !m.images);
        P.eq("pictures.model_marked", marked ? 1 : 0, 1);
        script->add(stream(fake::OpenAiSse("qwen-probe:32b").text("Still none.").finish("stop").usage(9000, 8).done().str()));
        agent.send("and now?");
        agent.waitIdle();
        P.eq("pictures.none_after", !has(script->body(before + 2), "\"image_url\"") && script->postCount() - before == 3 ? 1 : 0, 1);
    }

    // ---- LIMITS ------------------------------------------------------------------------------------------------------------
    {
        const int before = script->postCount();
        script->add(stream(fake::OpenAiSse("qwen-probe:32b").text("A long answer that").finish("length").usage(9000, 4000).done().str()));
        script->add(stream(fake::OpenAiSse("qwen-probe:32b").text("goes on.").finish("stop").usage(9000, 8).done().str()));
        agent.send("explain it all");
        agent.waitIdle();
        const pg::ChatState s = agent.chatState();
        P.eq("limits.went_on", script->postCount() - before == 2 && has(allOf(s, pg::ChatEntry::Kind::note), "length limit")
                                   && has(script->body(before + 1), "your reply reached its limit") ? 1 : 0, 1);
    }

    // ---- CHECKS ------------------------------------------------------------------------------------------------------------
    {
        juce::Array<juce::var> impossible;
        impossible.add(input({ { "label", "Impossibly loud" }, { "metric", pg::product::kIsInstrument ? "rms_db" : "sine_rms_db" }, { "min", 30.0 } }));
        const int before = script->postCount();
        script->add(stream(fake::OpenAiSse("qwen-probe:32b").call(0, "c", "set_checks", claude::toJson(input({ { "checks", impossible } })))
                               .finish("tool_calls").usage(9000, 50).done().str()));
        script->add(stream(fake::OpenAiSse("qwen-probe:32b").text("Done.").finish("stop").done().str()));
        script->add(stream(fake::OpenAiSse("qwen-probe:32b").text("Still done.").finish("stop").done().str()));
        script->add(stream(fake::OpenAiSse("qwen-probe:32b").text("It cannot pass.").finish("stop").done().str()));
        agent.send("make it loud");
        agent.waitIdle();
        P.eq("checks.nudged_twice", script->postCount() - before, 4);
        P.eq("checks.results_sent", has(script->body(before + 2), "your checks") && has(script->body(before + 3), "will not ask again") ? 1 : 0, 1);
        P.eq("checks.answer", lastText(agent.chatState(), pg::ChatEntry::Kind::assistant) == "It cannot pass." ? 1 : 0, 1);
        agent.clearChecks();
    }

    // ---- GEMINI ------------------------------------------------------------------------------------------------------------
    const std::string gemKey = fake::fakeGeminiKey();
    std::string gemPath;
    {
        agent.newChat();
        script->gets["/v1beta/models?pageSize=1000"] = { 200, fake::geminiModels({ { "gemini-2.5-flash", "Gemini 2.5 Flash", 0, 1048576 },
                                                                                  { "gemini-3-pro-preview", "Gemini 3 Pro Preview", 0, 1048576 },
                                                                                  { "gemini-2.5-flash-preview-tts", "Gemini TTS", 0, 8192 },
                                                                                  { "gemini-embedding-001", "Gemini Embedding", 0, 2048 } }) };
        why.clear();
        P.eq("gemini.set", agent.setProvider({ "gemini", "", "" }, gemKey, why) ? 1 : 0, 1);
        agent.waitIdle();
        const pg::ChatState s = agent.chatState();
        P.eq("gemini.models_asked", script->lastHost("GET /v1beta/models?pageSize=1000") == pr::kGeminiBase
                                        && script->lastHeader("GET /v1beta/models?pageSize=1000", "x-goog-api-key") == gemKey ? 1 : 0, 1);
        P.eq("gemini.models", s.models.size() == 2 && s.model == "gemini-3-pro-preview" && s.providerName == "Google Gemini" ? 1 : 0, 1);

        const int before = script->postCount();
        script->add(stream(fake::GeminiSse("gemini-3-pro-preview").text("Looking.").call("read_file", input({ { "name", "program.fm" } }), "sig-gem-1")
                               .finish("STOP", 30000, 40).str()));
        script->add(stream(fake::GeminiSse("gemini-3-pro-preview").text("A level dial.").finish("STOP", 31000, 12).str()));
        agent.send("what is it?");
        agent.waitIdle();
        const pg::ChatState t = agent.chatState();
        print("gemini", t);
        gemPath = "POST /v1beta/models/gemini-3-pro-preview:streamGenerateContent?alt=sse";
        P.eq("gemini.requests", script->postCount() - before, 2);
        P.eq("gemini.endpoint", script->count(gemPath) == 2 && script->lastHost(gemPath) == pr::kGeminiBase ? 1 : 0, 1);
        P.eq("gemini.key_header_alone", script->lastHeader(gemPath, "x-goog-api-key") == gemKey && !script->lastHas(gemPath, "Authorization")
                                            && !script->lastHas(gemPath, "x-api-key") ? 1 : 0, 1);
        const juce::var b0 = claude::parseJson(script->body(before)), b1 = claude::parseJson(script->body(before + 1));
        const juce::Array<juce::var> sys = arr(b0.getProperty("systemInstruction", {}).getProperty("parts", {}));
        P.eq("gemini.system", sys.size() == 1 && str(sys[0], "text") == pg::ChatAgent::systemPrompt() ? 1 : 0, 1);
        const juce::Array<juce::var> c0 = arr(b0.getProperty("contents", {}));
        P.eq("gemini.contents", c0.size() == 1 && str(c0[0], "role") == "user"
                                    && has(claude::toJson(c0[0]), "plugin_state") && has(claude::toJson(c0[0]), "what is it?") ? 1 : 0, 1);
        const juce::Array<juce::var> decls = arr(arr(b0.getProperty("tools", {}))[0].getProperty("functionDeclarations", {}));
        bool jsonSchema = !decls.isEmpty();
        for (const juce::var& dcl : decls)
            jsonSchema = jsonSchema && dcl.hasProperty("parametersJsonSchema") && !dcl.hasProperty("parameters");
        P.eq("gemini.declarations", decls.size() == toolCount() && jsonSchema ? 1 : 0, 1);
        P.eq("gemini.no_anthropic_fields", !b0.hasProperty("thinking") && !b0.hasProperty("model") && !b0.hasProperty("messages")
                                               && !anyHas(b0, "cache_control") && !anyHas(b0, "eager_input_streaming") ? 1 : 0, 1);
        const juce::Array<juce::var> c1 = arr(b1.getProperty("contents", {}));
        const juce::var model = c1.size() >= 2 ? c1[1] : juce::var();
        const juce::Array<juce::var> mp = arr(model.getProperty("parts", {}));
        P.eq("gemini.call_sent_back", str(model, "role") == "model" && mp.size() == 2 && str(mp[0], "text") == "Looking."
                                          && str(mp[1].getProperty("functionCall", {}), "name") == "read_file"
                                          && str(mp[1], "thoughtSignature") == "sig-gem-1" ? 1 : 0, 1);
        const juce::Array<juce::var> rp = c1.size() >= 3 ? arr(c1[2].getProperty("parts", {})) : juce::Array<juce::var>();
        P.eq("gemini.result_by_name", c1.size() == 3 && str(c1[2], "role") == "user" && !rp.isEmpty()
                                          && str(rp[0].getProperty("functionResponse", {}), "name") == "read_file"
                                          && has(claude::toJson(rp[0]), "program.fm") ? 1 : 0, 1);
        P.eq("gemini.answer", has(lastText(t, pg::ChatEntry::Kind::assistant), "A level dial.") && !t.busy ? 1 : 0, 1);
        P.eq("gemini.signature_kept", has(agent.saveState(), "\"thought_signature\":\"sig-gem-1\"") ? 1 : 0, 1);

        // Gemini refusing the JSON Schema: its own subset, from then on.
        const int b2 = script->postCount();
        script->add(fake::Response::failure(400, "INVALID_ARGUMENT",
                                            "Invalid JSON payload received. Unknown name \"parametersJsonSchema\" at 'tools[0].function_declarations[0]': Cannot find field."));
        script->add(stream(fake::GeminiSse("gemini-3-pro-preview").text("OK.").finish("STOP").str()));
        agent.send("again");
        agent.waitIdle();
        const juce::var retry = claude::parseJson(script->body(b2 + 1));
        const juce::Array<juce::var> d2 = arr(arr(retry.getProperty("tools", {}))[0].getProperty("functionDeclarations", {}));
        bool subset = !d2.isEmpty();
        juce::var action, dials, spectrum, on;   // enums with no type in the tools' own schemas
        juce::var args, files;                   // arrays whose items they leave open
        for (const juce::var& dcl : d2)
        {
            subset = subset && !dcl.hasProperty("parametersJsonSchema")
                  && (!dcl.hasProperty("parameters") || str(dcl.getProperty("parameters", {}), "type") == "OBJECT");
            const juce::var props = dcl.getProperty("parameters", {}).getProperty("properties", {});
            if (str(dcl, "name") == "projects")
                action = props.getProperty("action", {});
            else if (str(dcl, "name") == "compare")
                dials = props.getProperty("dials", {});
            else if (str(dcl, "name") == "measure_output")
                spectrum = props.getProperty("spectrum", {});
            else if (str(dcl, "name") == "run_function")
            {
                on = props.getProperty("on", {});
                args = props.getProperty("args", {});
            }
            else if (str(dcl, "name") == "write_file")
                files = props.getProperty("files", {});
        }
        P.eq("gemini.subset", script->postCount() - b2 == 2 && subset && !anyHas(retry.getProperty("tools", {}), "additionalProperties")
                                  && !anyHas(retry.getProperty("tools", {}), "anyOf") ? 1 : 0, 1);
        std::printf("gemini subset: projects' action %s\n", claude::toJson(action).c_str());
        const auto words = [](const juce::var& p, int n) { return str(p, "type") == "STRING" && arr(p.getProperty("enum", {})).size() == n; };
        P.eq("gemini.subset_enums", words(action, 5) && arr(action.getProperty("enum", {}))[0].toString() == "list"
                                        && words(dials, 3) && words(spectrum, 3) && words(on, 2) ? 1 : 0, 1);
        // An array whose items the tool leaves open (run_function's args, write_file's files): strings there, each a
        // JSON value written out, read back before the tool runs (providers::fromSubset).
        std::printf("gemini subset: run_function's args %s\n", claude::toJson(args).c_str());
        const auto texts = [](const juce::var& p) {
            return str(p, "type") == "ARRAY" && str(p.getProperty("items", {}), "type") == "STRING" && has(str(p, "description"), "JSON value");
        };
        P.eq("gemini.subset_open_arrays", texts(args) && texts(files) ? 1 : 0, 1);
        juce::var args3;
        {
            juce::Array<juce::var> given;
            given.add("0.5");
            given.add("true");
            given.add("[1, 2]");
            given.add("not json");
            juce::var schema;
            for (const juce::var& tool : arr(pg::ChatAgent::toolDefinitions()))
                if (str(tool, "name") == "run_function")
                    schema = tool.getProperty("input_schema", {});
            args3 = pr::fromSubset(input({ { "name", "f" }, { "args", given } }), schema).getProperty("args", {});
        }
        const juce::Array<juce::var> a3 = arr(args3);
        P.eq("gemini.subset_read_back", a3.size() == 4 && a3[0].isDouble() && static_cast<double>(a3[0]) == 0.5 && a3[1].isBool()
                                            && static_cast<bool>(a3[1]) && arr(a3[2]).size() == 2 && a3[3].toString() == "not json" ? 1 : 0, 1);
        const pg::Draft before3 = processor.editorPort().draft();
        juce::Array<juce::var> written;
        written.add("{\"name\": \"extra.fm\", \"content\": \"// extra\\n\"}");
        script->add(stream(fake::GeminiSse("gemini-3-pro-preview").call("write_file", input({ { "files", written } })).finish("STOP").str()));
        script->add(stream(fake::GeminiSse("gemini-3-pro-preview").text("Wrote it.").finish("STOP").str()));
        agent.send("add extra.fm");
        agent.waitIdle();
        processor.settle();
        const pg::Draft after3 = processor.editorPort().draft();
        bool extra = false;
        for (const pg::SourceFile& f : after3.files)
            extra = extra || (f.name == "extra.fm" && f.text == "// extra\n");
        P.eq("gemini.subset_files_written", extra && after3.files.size() == before3.files.size() + 1 ? 1 : 0, 1);
        P.eq("gemini.subset_noted", has(allOf(agent.chatState(), pg::ChatEntry::Kind::note), "refused the tools' schemas") ? 1 : 0, 1);
        script->add(stream(fake::GeminiSse("gemini-3-pro-preview").text("Still OK.").finish("STOP").str()));
        agent.send("once more");
        agent.waitIdle();
        P.eq("gemini.subset_kept", !has(script->body(script->postCount() - 1), "parametersJsonSchema") ? 1 : 0, 1);

        // A key Gemini calls not valid.
        script->add(fake::Response::failure(400, "INVALID_ARGUMENT", "API key not valid. Please pass a valid API key."));
        agent.send("hello?");
        agent.waitIdle();
        const pg::ChatState k = agent.chatState();
        P.eq("gemini.key_refused", has(lastText(k, pg::ChatEntry::Kind::error), "Google Gemini refused the API key") && !k.keyProblem.empty() ? 1 : 0, 1);
        P.eq("gemini.key_again", agent.setProvider({ "gemini", "", "" }, gemKey, why) && agent.chatState().keyProblem.empty() ? 1 : 0, 1);
        agent.waitIdle();
    }

    // ---- KEPT --------------------------------------------------------------------------------------------------------------
    {
        pg::Processor second;
        const pg::ChatState s = second.chatAgent().chatState();
        P.eq("kept.provider", s.provider.kind == "gemini" && s.model == "gemini-3-pro-preview" && s.ready() ? 1 : 0, 1);
    }

    // ---- BACK --------------------------------------------------------------------------------------------------------------
    {
        P.eq("back.key", agent.saveKey(fake::fakeKey(), why) ? 1 : 0, 1);
        P.eq("back.set", agent.setProvider({}, "", why) ? 1 : 0, 1);
        const pg::ChatState s = agent.chatState();
        P.eq("back.anthropic", s.provider.kind == "anthropic" && s.providerName == "Anthropic" && s.model == "claude-sonnet-5-5" && !s.fast
                                   && !s.models.empty() && !s.models[0].efforts.empty() ? 1 : 0, 1);
        const int before = script->postCount();
        script->add(fake::Response::ok(fake::Sse().start("claude-opus-5-5").text("Plan: read the program first.").end()));
        script->add(fake::Response::ok(fake::Sse().start("claude-sonnet-5-5").text("Back with Claude.").end()));
        agent.send("and now?");
        agent.waitIdle();
        P.eq("back.requests", script->postCount() - before, 2);   // the planner's, then the builder's
        const juce::var planned = claude::parseJson(script->body(before));
        bool plannerSawCall = false;
        for (const juce::var& m : arr(planned.getProperty("messages", {})))
            for (const juce::var& blk : arr(m.getProperty("content", {})))
                plannerSawCall = plannerSawCall || (str(blk, "type") == "tool_use" && str(blk, "name") == "read_file");
        P.eq("back.planner_asked", str(planned, "model") == "claude-opus-5-5" && str(planned.getProperty("tool_choice", {}), "type") == "none"
                                       && plannerSawCall ? 1 : 0, 1);
        P.eq("back.planner_no_signature", has(script->body(before), "thought_signature") ? 1 : 0, 0);
        P.eq("back.endpoint", script->lastHost("POST /v1/messages") == claude::kHost
                                  && script->lastHeader("POST /v1/messages", "x-api-key") == fake::fakeKey()
                                  && !script->lastHas("POST /v1/messages", "Authorization")
                                  && !script->lastHas("POST /v1/messages", "x-goog-api-key") ? 1 : 0, 1);
        const juce::var b = claude::parseJson(script->body(before + 1));
        const juce::Array<juce::var> sys = arr(b.getProperty("system", {}));
        P.eq("back.shape", str(b, "model") == "claude-sonnet-5-5" && b.getProperty("thinking", {}).isObject() && sys.size() == 1
                               && str(sys[0], "text") == pg::ChatAgent::systemPrompt() && sys[0].getProperty("cache_control", {}).isObject()
                               && b.getProperty("cache_control", {}).isObject() && !b.hasProperty("stream_options") ? 1 : 0, 1);
        const juce::Array<juce::var> messages = arr(b.getProperty("messages", {}));
        bool gemCall = false;
        for (const juce::var& m : messages)
            for (const juce::var& blk : arr(m.getProperty("content", {})))
                gemCall = gemCall || (str(blk, "type") == "tool_use" && str(blk, "name") == "read_file");
        P.eq("back.history_carried", gemCall && idsPlain(messages) ? 1 : 0, 1);
        P.eq("back.no_signature", has(script->body(before + 1), "thought_signature") ? 1 : 0, 0);
        P.eq("back.signature_still_kept", has(agent.saveState(), "sig-gem-1") ? 1 : 0, 1);
        P.eq("back.answer", lastText(agent.chatState(), pg::ChatEntry::Kind::assistant) == "Back with Claude." ? 1 : 0, 1);
    }

    // ---- ERRORS ------------------------------------------------------------------------------------------------------------
    {
        const pg::ChatProvider ollama{ "openai", "http://localhost:11434/v1", "" }, openai{ "openai", "https://api.openai.com/v1", "" };
        const std::string body = claude::toJson(input({ { "error", input({ { "message", "Incorrect API key provided" }, { "type", "invalid_request_error" } }) } }));
        P.eq("errors.named", has(pr::errorText(openai, 401, body), "OpenAI refused the API key (Incorrect API key provided)") ? 1 : 0, 1);
        P.eq("errors.quota", has(pr::errorText(openai, 429, body), "rate limiting") ? 1 : 0, 1);
        const std::string ctx = claude::toJson(input({ { "error", input({ { "message", "This model's maximum context length is 8192 tokens" } }) } }));
        P.eq("errors.context", has(pr::errorText(ollama, 400, ctx), "longer context in the server's settings") ? 1 : 0, 1);
        P.eq("errors.not_running", has(pr::connectionText(ollama, "no connection"), "is the server (Ollama (this Mac)) running?") ? 1 : 0, 1);
        P.eq("errors.string_error", has(pr::errorText(ollama, 404, "{\"error\":\"model 'x' not found\"}"), "model 'x' not found") ? 1 : 0, 1);
    }

    // ---- SERVERS -----------------------------------------------------------------------------------------------------------
    {
        // LM Studio on this Mac, not running, no model chosen: the turn says why, and sends nothing.
        script->gets["/v1/models"] = { 0, "" };
        why.clear();
        P.eq("servers.studio_set", agent.setProvider({ "openai", "http://localhost:1234/v1", "" }, "", why) ? 1 : 0, 1);
        agent.waitIdle();
        int before = script->postCount();
        agent.send("hello");
        agent.waitIdle();
        P.eq("servers.not_running", has(lastText(agent.chatState(), pg::ChatEntry::Kind::error),
                                        "Could not read LM Studio (this Mac)'s models: no connection to localhost:1234: is the "
                                        "server (LM Studio (this Mac)) running?")
                                        && script->postCount() == before && !agent.chatState().busy ? 1 : 0, 1);

        // Another server whose list refuses the key, no model chosen: the key's problem, nothing sent.
        script->gets["/v1/models"] = { 401, claude::toJson(input({ { "error", input({ { "message", "Invalid API key" } }) } })) };
        P.eq("servers.custom_set", agent.setProvider({ "openai", "https://llm.example.org/v1", "" }, orKey, why) ? 1 : 0, 1);
        agent.waitIdle();
        before = script->postCount();
        agent.send("hello again");
        agent.waitIdle();
        P.eq("servers.key_refused", has(lastText(agent.chatState(), pg::ChatEntry::Kind::error),
                                        "The saved llm.example.org key cannot be used: llm.example.org refused the API key (Invalid API key)")
                                        && script->postCount() == before ? 1 : 0, 1);

        // Its model chosen: a redirect is never followed, and the error says where it pointed.
        script->gets["/v1/models"] = { 200, fake::openAiModels({ { "acme-probe", "", 1762000000 } }) };
        P.eq("servers.custom_model", agent.setProvider({ "openai", "https://llm.example.org/v1", "acme-probe" }, orKey, why) ? 1 : 0, 1);
        agent.waitIdle();
        before = script->postCount();
        script->add(fake::Response::redirect(307, "http://llm.example.com/v1/chat/completions"));
        agent.send("and this?");
        agent.waitIdle();
        const std::string redirected = lastText(agent.chatState(), pg::ChatEntry::Kind::error);
        std::printf("redirect: %s\n", redirected.c_str());
        P.eq("servers.redirect_not_followed", script->postCount() - before == 1
                                                  && script->lastHost("POST /v1/chat/completions") == "https://llm.example.org"
                                                  && has(redirected, "answered with a redirect (HTTP 307, to http://llm.example.com/v1/chat/completions)")
                                                  && has(redirected, "follows none") ? 1 : 0, 1);

        // A model the server says cannot serve this chat (OpenAI's Responses API alone): off the menu, no longer chosen.
        before = script->postCount();
        script->add(fake::Response::failure(404, "invalid_request_error",
                                            "This model is only supported in v1/responses and not in v1/chat/completions."));
        agent.send("once more");
        agent.waitIdle();
        const pg::ChatState m = agent.chatState();
        const std::string refusal = lastText(m, pg::ChatEntry::Kind::error);
        std::printf("model refused: %s\n", refusal.c_str());
        P.eq("servers.model_refused", script->postCount() - before == 1 && has(refusal, "cannot run acme-probe for this chat")
                                          && has(refusal, "Choose another in the model menu") && m.model.empty()
                                          && std::none_of(m.models.begin(), m.models.end(), [](const pg::ChatModel& x) { return x.id == "acme-probe"; })
                                          ? 1 : 0, 1);
        P.eq("servers.model_forgotten", has(fileText(keys.getChildFile("chat.json")), "acme-probe") ? 1 : 0, 0);
    }

    // ---- SWITCH ------------------------------------------------------------------------------------------------------------
    {
        // The other server's list held back (a slow GET) while a turn waits for it; Gemini chosen meanwhile.
        script->gets["/v1/models"] = { 200, fake::openAiModels({ { "acme-probe", "", 1762000000 } }) };
        script->holdGets = true;
        P.eq("switch.custom", agent.setProvider({ "openai", "https://llm.example.org/v1", "" }, orKey, why) ? 1 : 0, 1);
        for (int i = 0; i < 1000 && script->heldGets.load() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const int before = script->postCount(), toOther = script->count("POST /v1/chat/completions"), toGemini = script->count(gemPath);
        script->add(stream(fake::GeminiSse("gemini-3-pro-preview").text("From Gemini.").finish("STOP").str()));
        agent.send("which one?");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));   // the turn waits for the list by now
        P.eq("switch.gemini", agent.setProvider({ "gemini", "", "" }, "", why) ? 1 : 0, 1);
        script->holdGets = false;
        agent.waitIdle();
        P.eq("switch.turn_followed", script->postCount() - before == 1 && script->count("POST /v1/chat/completions") == toOther
                                         && script->count(gemPath) == toGemini + 1
                                         && has(lastText(agent.chatState(), pg::ChatEntry::Kind::assistant), "From Gemini.") ? 1 : 0, 1);
    }

    // ---- EFFORT ------------------------------------------------------------------------------------------------------------
    {
        const pg::ChatProvider orP{ "openai", "https://openrouter.ai/api/v1", "" }, oaP{ "openai", "https://api.openai.com/v1", "" },
            gemP{ "gemini", "", "" }, localP{ "openai", "http://localhost:11434/v1", "" }, customP{ "openai", "https://llm.example.org/v1", "" };
        using Levels = std::vector<std::string>;
        const auto is = [](const pg::ChatModel& m, const Levels& levels, const std::string& byDefault) {
            return m.efforts == levels && m.defaultEffort == byDefault;
        };
        const auto named = [](const pg::ChatProvider& p, const char* id) { return pr::plainModel(p, id); };
        const auto byId = [](const std::vector<pg::ChatModel>& models, const std::string& id) {
            for (const pg::ChatModel& m : models)
                if (m.id == id)
                    return m;
            return pg::ChatModel{};
        };
        const auto count = [](const pg::ChatState& s, pg::ChatEntry::Kind kind) {
            return std::count_if(s.entries.begin(), s.entries.end(), [kind](const pg::ChatEntry& e) { return e.kind == kind; });
        };
        P.eq("effort.wires", pr::reasoningOf(orP) == pr::Reasoning::openRouter && pr::reasoningOf(oaP) == pr::Reasoning::openAi
                                 && pr::reasoningOf(gemP) == pr::Reasoning::gemini && pr::reasoningOf(localP) == pr::Reasoning::none
                                 && pr::reasoningOf(customP) == pr::Reasoning::none && pr::reasoningOf({}) == pr::Reasoning::none ? 1 : 0, 1);

        // OpenRouter's list: its reasoning objects.
        const std::vector<fake::ListedModel> orModels{
            { "openai/gpt-probe-5", "GPT-5 via OpenRouter", 1764000000, 400000, true, true, { "xhigh", "high", "medium", "low", "minimal", "none" }, "medium" },
            { "deepseek/r-probe", "R via OpenRouter", 1763900000, 128000, true, false, { "high", "medium", "low", "none" }, "high", true, true },
            { "acme/think-probe", "Think via OpenRouter", 1763800000, 128000, true, false, { "high", "low", "none" }, "low", false },
            { "meta/llama-probe", "Llama (text)", 1761000000, 131072, true, false } };
        const std::string orList = fake::openAiModels(orModels, true);
        const std::vector<pg::ChatModel> listed = pr::modelsOf(orP, orList);
        P.eq("effort.openrouter_levels", is(byId(listed, "openai/gpt-probe-5"), { "none", "minimal", "low", "medium", "high", "xhigh" }, "medium") ? 1 : 0, 1);
        P.eq("effort.openrouter_mandatory_no_off", is(byId(listed, "deepseek/r-probe"), { "low", "medium", "high" }, "high") ? 1 : 0, 1);
        P.eq("effort.openrouter_off_by_default", is(byId(listed, "acme/think-probe"), { "none", "low", "high" }, "none") ? 1 : 0, 1);
        P.eq("effort.openrouter_none_without", byId(listed, "meta/llama-probe").id == "meta/llama-probe"
                                                   && is(byId(listed, "meta/llama-probe"), {}, "") ? 1 : 0, 1);
        bool othersNone = true;
        for (const pg::ChatProvider& p : { customP, localP })
            for (const pg::ChatModel& m : pr::modelsOf(p, orList))
                othersNone = othersNone && m.efforts.empty() && m.defaultEffort.empty();
        P.eq("effort.other_servers_none", othersNone ? 1 : 0, 1);

        // OpenAI's and Gemini's by the model's family.
        P.eq("effort.openai_o_series", is(named(oaP, "o3"), { "low", "medium", "high" }, "medium")
                                           && is(named(oaP, "o4-mini-2025-04-16"), { "low", "medium", "high" }, "medium")
                                           && is(named(oaP, "o1"), { "low", "medium", "high" }, "medium") ? 1 : 0, 1);
        P.eq("effort.openai_gpt5", is(named(oaP, "gpt-5"), { "minimal", "low", "medium", "high" }, "medium")
                                       && is(named(oaP, "gpt-5-mini-2025-08-07"), { "minimal", "low", "medium", "high" }, "medium")
                                       && is(named(oaP, "gpt-5.1"), { "none", "low", "medium", "high" }, "none")
                                       && is(named(oaP, "gpt-5.2-2025-12-11"), { "none", "low", "medium", "high", "xhigh" }, "")
                                       && is(named(oaP, "gpt-5.4"), { "low", "medium", "high" }, "")
                                       && is(named(oaP, "gpt-6"), { "low", "medium", "high" }, "") ? 1 : 0, 1);
        P.eq("effort.openai_none", named(oaP, "gpt-4o").efforts.empty() && named(oaP, "gpt-4o-mini").efforts.empty()
                                       && named(oaP, "gpt-4.1-mini").efforts.empty() && named(oaP, "gpt-5-chat-latest").efforts.empty()
                                       && named(oaP, "gpt-5.1-chat-latest").efforts.empty() && named(oaP, "o1-mini").efforts.empty()
                                       && named(localP, "o3").efforts.empty() && named(orP, "o3").efforts.empty() ? 1 : 0, 1);
        P.eq("effort.gemini3", is(named(gemP, "gemini-3-pro-preview"), { "low", "high" }, "high")
                                   && is(named(gemP, "models/gemini-3-flash-preview"), { "minimal", "low", "medium", "high" }, "high") ? 1 : 0, 1);
        P.eq("effort.gemini25", is(named(gemP, "gemini-2.5-pro"), { "low", "medium", "high" }, "")
                                    && is(named(gemP, "gemini-2.5-flash"), { "none", "low", "medium", "high" }, "")
                                    && is(named(gemP, "gemini-2.5-flash-lite"), { "none", "low", "medium", "high" }, "none") ? 1 : 0, 1);
        P.eq("effort.gemini_none", named(gemP, "gemini-2.0-flash").efforts.empty() && named(gemP, "gemini-flash-latest").efforts.empty() ? 1 : 0, 1);
        const std::vector<pg::ChatModel> gl = pr::modelsOf(gemP, fake::geminiModels({ { "gemini-2.5-flash", "Flash", 0, 1048576, true, true, {}, "", true, false, 0 },
                                                                                     { "gemini-2.5-pro", "Pro", 0, 1048576, true, true, {}, "", true, false, 1 } }));
        P.eq("effort.gemini_list_says", gl.size() == 2 && byId(gl, "gemini-2.5-flash").efforts.empty() && byId(gl, "gemini-2.5-pro").efforts.size() == 3 ? 1 : 0, 1);

        // The bodies.
        juce::var request = claude::object({ { "model", "openai/gpt-probe-5" }, { "max_tokens", 64000 }, { "system", "Be brief." },
                                             { "messages", juce::Array<juce::var>{ claude::object({ { "role", "user" }, { "content", "hi" } }) } } });
        const auto bodyOf = [&request](const pg::ChatProvider& p, const char* model, const std::string& effort) {
            request.getDynamicObject()->setProperty("model", model);
            pr::Options o;
            o.effort = effort;
            return claude::parseJson(pr::body(p, request, o));
        };
        const juce::var orHigh = bodyOf(orP, "openai/gpt-probe-5", "high"), orNone = bodyOf(orP, "openai/gpt-probe-5", "");
        P.eq("effort.openrouter_body", str(orHigh.getProperty("reasoning", {}), "effort") == "high" && !orHigh.hasProperty("max_tokens")
                                           && !orHigh.hasProperty("reasoning_effort") && !anyHas(orHigh.getProperty("reasoning", {}), "max_tokens") ? 1 : 0, 1);
        P.eq("effort.openrouter_none_sent", !orNone.hasProperty("reasoning") && !orNone.hasProperty("reasoning_effort") ? 1 : 0, 1);
        const juce::var oaHigh = bodyOf(oaP, "o3", "high");
        P.eq("effort.openai_body", str(oaHigh, "reasoning_effort") == "high" && !oaHigh.hasProperty("reasoning") ? 1 : 0, 1);
        const juce::var localHigh = bodyOf(localP, "qwen-probe:32b", "high"), customHigh = bodyOf(customP, "acme-probe", "high");
        P.eq("effort.other_servers_none_sent", !localHigh.hasProperty("reasoning") && !localHigh.hasProperty("reasoning_effort")
                                                   && !customHigh.hasProperty("reasoning") && !customHigh.hasProperty("reasoning_effort") ? 1 : 0, 1);
        const auto thinking = [](const juce::var& b) { return b.getProperty("generationConfig", {}).getProperty("thinkingConfig", {}); };
        const juce::var g3 = bodyOf(gemP, "gemini-3-pro-preview", "low"), g25 = bodyOf(gemP, "gemini-2.5-flash", "medium"),
                        g25off = bodyOf(gemP, "gemini-2.5-flash", "none"), g25proOff = bodyOf(gemP, "gemini-2.5-pro", "none"),
                        g3off = bodyOf(gemP, "gemini-3-pro-preview", "none"), g3none = bodyOf(gemP, "gemini-3-pro-preview", "");
        P.eq("effort.gemini3_level", str(thinking(g3), "thinkingLevel") == "low" && !thinking(g3).hasProperty("thinkingBudget")
                                         && static_cast<bool>(thinking(g3).getProperty("includeThoughts", false)) ? 1 : 0, 1);
        P.eq("effort.gemini25_budget", static_cast<int>(thinking(g25).getProperty("thinkingBudget", -1)) == 8192
                                           && !thinking(g25).hasProperty("thinkingLevel")
                                           && static_cast<bool>(thinking(g25).getProperty("includeThoughts", false)) ? 1 : 0, 1);
        P.eq("effort.gemini25_off", static_cast<int>(thinking(g25off).getProperty("thinkingBudget", -1)) == 0
                                        && !thinking(g25off).hasProperty("includeThoughts") && !thinking(g25off).hasProperty("thinkingLevel") ? 1 : 0, 1);
        P.eq("effort.gemini_off_only_where_allowed", !g25proOff.hasProperty("generationConfig") && !g3off.hasProperty("generationConfig") ? 1 : 0, 1);
        P.eq("effort.gemini_none_sent", !g3none.hasProperty("generationConfig") ? 1 : 0, 1);
        P.eq("effort.refusal_said", pr::aboutEffort(oaP, 400, "OpenAI: HTTP 400: Unsupported parameter: 'reasoning_effort' is not supported with this model.")
                                        && pr::aboutEffort(gemP, 400, "Google Gemini: HTTP 400: Thinking level is not supported for this model.")
                                        && pr::aboutEffort(gemP, 400, "Invalid JSON payload received. Unknown name \"thinkingBudget\" at 'generation_config.thinking_config'")
                                        && !pr::aboutEffort(oaP, 400, "model o3 does not support image input")
                                        && !pr::aboutEffort(oaP, 429, "reasoning_effort")
                                        && !pr::aboutEffort(localP, 400, "Unsupported parameter: 'reasoning_effort'") ? 1 : 0, 1);
        // OpenAI's words for a parameter it does not take are the level's only when they name no picture.
        P.eq("effort.refusal_not_a_picture", !pr::aboutEffort(oaP, 400, "Unsupported parameter: image_url is not supported with this model.")
                                                 && !pr::aboutEffort(orP, 400, "Unsupported parameter: 'image_url' is not supported with this model.")
                                                 && pr::aboutEffort(orP, 400, "Unsupported parameter: 'reasoning' is not supported with this model.") ? 1 : 0, 1);
        // OpenRouter passing on a refusal from the model's provider: "Provider returned error", the provider's words in its
        // metadata.raw (Anthropic's, for thinking on with no thinking block before a tool call's result).
        const std::string thinkingRaw =
            "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"messages.1.content.0.type: Expected "
            "`thinking` or `redacted_thinking`, but found `tool_use`. When `thinking` is enabled, a final `assistant` message must "
            "start with a thinking block. To avoid this requirement, disable `thinking`.\"}}";
        const auto providerError = [](const juce::var& raw) {
            return claude::toJson(claude::object(
                { { "error", claude::object({ { "message", "Provider returned error" }, { "code", 400 },
                                              { "metadata", claude::object({ { "raw", raw }, { "provider_name", "Anthropic" } }) } }) } }));
        };
        const std::string orThinking = providerError(juce::String(thinkingRaw));
        const std::string orThinkingText = pr::errorText(orP, 400, orThinking);
        P.eq("effort.provider_words", has(orThinkingText, "Provider returned error (Anthropic: messages.1.content.0.type: Expected `thinking`")
                                          && has(pr::errorText(orP, 400, providerError(claude::object({ { "error", claude::object({ { "message", "Overloaded." } }) } }))),
                                                 "Provider returned error (Anthropic: Overloaded.)")
                                          && has(pr::errorText(orP, 400, providerError("upstream said no")), "Provider returned error (Anthropic: upstream said no)")
                                          && has(pr::errorText(orP, 400, claude::toJson(claude::object({ { "error", claude::object({ { "message", "Bad input." } }) } }))),
                                                 "HTTP 400: Bad input.") ? 1 : 0, 1);
        P.eq("effort.refusal_through_openrouter", pr::aboutEffort(orP, 400, orThinkingText) && !pr::aboutModel(400, orThinkingText)
                                                      && !pr::aboutEffort(orP, 400, "OpenRouter: HTTP 400: Provider returned error") ? 1 : 0, 1);

        // OpenRouter through the agent: the note, the default shown and nothing sent, a level chosen sent and kept.
        script->gets["/api/v1/models"] = { 200, orList };
        why.clear();
        P.eq("effort.openrouter_set", agent.setProvider({ "openai", "https://openrouter.ai/api/v1", "openai/gpt-probe-5" }, "", why) ? 1 : 0, 1);
        agent.waitIdle();
        pg::ChatState s = agent.chatState();
        P.eq("effort.openrouter_note", has(allOf(s, pg::ChatEntry::Kind::note), "The Thinking menu stays where its model takes a level (that model's own levels; until you choose one, the model's own default)")
                                           && has(allOf(s, pg::ChatEntry::Kind::note), "fast mode, web search and the planner stay Anthropic's") ? 1 : 0, 1);
        P.eq("effort.openrouter_default_shown", s.model == "openai/gpt-probe-5" && s.effort == "medium"
                                                    && byId(s.models, "openai/gpt-probe-5").efforts.size() == 6 ? 1 : 0, 1);
        int before = script->postCount();
        script->add(stream(fake::OpenAiSse("openai/gpt-probe-5").reasoningDetails("Weighing the filter.", "reasoning.text", true)
                               .text("A one-pole.").finish("stop").usage(9000, 20).done().str()));
        agent.send("which filter?");
        agent.waitIdle();
        s = agent.chatState();
        const juce::var t1 = claude::parseJson(script->body(before));
        P.eq("effort.nothing_chosen_nothing_sent", script->postCount() - before == 1 && !t1.hasProperty("reasoning")
                                                       && !t1.hasProperty("reasoning_effort") ? 1 : 0, 1);
        P.eq("effort.reasoning_once", lastText(s, pg::ChatEntry::Kind::thinking) == "Weighing the filter." ? 1 : 0, 1);
        agent.setEffort("max");                   // a level the model does not take: nothing changes
        P.eq("effort.unknown_level_refused", agent.chatState().effort == "medium" ? 1 : 0, 1);
        agent.setEffort("high");
        s = agent.chatState();
        const auto keptLevel = [&keys](const std::string& forKey, const std::string& model) {
            for (const juce::var& e : arr(claude::parseJson(fileText(keys.getChildFile("chat.json"))).getProperty("efforts", {})))
                if (str(e, "for") == forKey && str(e, "model") == model)
                    return str(e, "effort");
            return std::string();
        };
        const std::string orFor = "openai https://openrouter.ai/api/v1";
        P.eq("effort.chosen_kept", s.effort == "high" && keptLevel(orFor, "openai/gpt-probe-5") == "high" ? 1 : 0, 1);
        before = script->postCount();
        script->add(stream(fake::OpenAiSse("openai/gpt-probe-5").reasoningDetails("Summed up.", "reasoning.summary")
                               .text("Done.").finish("stop").usage(9000, 20).done().str()));
        agent.send("go on");
        agent.waitIdle();
        s = agent.chatState();
        const juce::var t2 = claude::parseJson(script->body(before));
        P.eq("effort.openrouter_sent", str(t2.getProperty("reasoning", {}), "effort") == "high" && !t2.hasProperty("max_tokens")
                                           && !t2.hasProperty("reasoning_effort") ? 1 : 0, 1);
        P.eq("effort.details_shown", lastText(s, pg::ChatEntry::Kind::thinking) == "Summed up." ? 1 : 0, 1);
        bool reasoningKept = false;
        for (const juce::var& m : arr(claude::parseJson(agent.saveState()).getProperty("messages", {})))
            for (const juce::var& blk : arr(m.getProperty("content", {})))
                reasoningKept = reasoningKept || str(blk, "type") == "thinking" || has(claude::toJson(blk), "Summed up.")
                             || has(claude::toJson(blk), "Weighing the filter.");
        P.eq("effort.reasoning_not_kept", reasoningKept ? 1 : 0, 0);
        // Per model: one without levels shows none and takes none; another's own; the first's again.
        agent.setModel("meta/llama-probe");
        agent.setEffort("high");
        s = agent.chatState();
        P.eq("effort.no_levels_none", s.effort.empty() && keptLevel(orFor, "meta/llama-probe").empty() ? 1 : 0, 1);
        agent.setModel("deepseek/r-probe");
        P.eq("effort.mandatory_default", agent.chatState().effort == "high" ? 1 : 0, 1);
        agent.setEffort("none");                  // reasoning mandatory: no Off
        agent.setEffort("low");
        P.eq("effort.other_model_own", agent.chatState().effort == "low" && keptLevel(orFor, "deepseek/r-probe") == "low" ? 1 : 0, 1);
        agent.setModel("openai/gpt-probe-5");
        P.eq("effort.per_model", agent.chatState().effort == "high" ? 1 : 0, 1);
        // A new processor: the provider, the model and its level kept (chat.json); its turn sends it once the list is in.
        {
            pg::Processor second;
            pg::ChatAgent& again = second.chatAgent();
            again.setTransport(std::make_unique<fake::Transport>(script));
            const int b = script->postCount();
            script->add(stream(fake::OpenAiSse("openai/gpt-probe-5").text("Still high.").finish("stop").usage(9000, 8).done().str()));
            again.send("still there?");
            again.waitIdle();
            const juce::var t3 = claude::parseJson(script->body(b));
            P.eq("effort.reload_kept", script->postCount() - b == 1 && str(t3.getProperty("reasoning", {}), "effort") == "high"
                                           && again.chatState().effort == "high" && again.chatState().model == "openai/gpt-probe-5" ? 1 : 0, 1);
        }
        // A model that thinks only when asked (Claude through OpenRouter: Off by default) with a level chosen, then a tool
        // call: its next request has no reasoning_details (the reasoning is never kept), which Anthropic refuses while
        // thinking is on, passed on by OpenRouter as "Provider returned error". Asked again without the level, the tool's
        // result kept (not run again), the turn goes on.
        agent.setModel("acme/think-probe");
        P.eq("effort.asked_off_default", agent.chatState().effort == "none" ? 1 : 0, 1);
        agent.setEffort("high");
        before = script->postCount();
        const auto orErrors = count(agent.chatState(), pg::ChatEntry::Kind::error);
        script->add(stream(fake::OpenAiSse("acme/think-probe").reasoning("Read it first.")
                               .call(0, "call_think1", "read_file", "{\"name\":\"program.fm\"}").finish("tool_calls").usage(9000, 30).done().str()));
        script->add(fake::Response{ 400, {}, orThinking, -1, false, 11, {} });
        script->add(stream(fake::OpenAiSse("acme/think-probe").text("It reads the input.").finish("stop").usage(9100, 10).done().str()));
        agent.send("what does the program do?");
        agent.waitIdle();
        s = agent.chatState();
        print("effort refused after a tool call", s);
        const auto toolResults = [](const juce::var& b) {
            int n = 0;
            for (const juce::var& m : arr(b.getProperty("messages", {})))
                n += str(m, "role") == "tool" ? 1 : 0;
            return n;
        };
        const juce::var k0 = claude::parseJson(script->body(before)), k1 = claude::parseJson(script->body(before + 1)),
                        k2 = claude::parseJson(script->body(before + 2));
        P.eq("effort.tool_turn_sent", script->postCount() - before == 3 && str(k0.getProperty("reasoning", {}), "effort") == "high"
                                          && str(k1.getProperty("reasoning", {}), "effort") == "high" && toolResults(k1) == toolResults(k0) + 1
                                          && !has(claude::toJson(k1.getProperty("messages", {})), "Read it first.") ? 1 : 0, 1);
        P.eq("effort.tool_turn_asked_again", !k2.hasProperty("reasoning") && toolResults(k2) == toolResults(k1) ? 1 : 0, 1);
        P.eq("effort.tool_turn_goes_on", lastText(s, pg::ChatEntry::Kind::assistant) == "It reads the input."
                                             && count(s, pg::ChatEntry::Kind::error) == orErrors && !s.busy
                                             && has(allOf(s, pg::ChatEntry::Kind::note), "The model (acme/think-probe) refused the thinking level High")
                                             && s.effort.empty() && byId(s.models, "acme/think-probe").efforts.empty()
                                             && keptLevel(orFor, "acme/think-probe").empty() ? 1 : 0, 1);

        // OpenAI: its levels by family; a model that refuses the level is asked again at once without it.
        script->gets["/v1/models"] = { 200, fake::openAiModels({ { "o3-probe", "", 1762000000 }, { "gpt-4o-probe", "", 1761000000 },
                                                                 { "gpt-5-probe", "", 1763000000 } }) };
        P.eq("effort.openai_set", agent.setProvider({ "openai", "https://api.openai.com/v1", "o3-probe" }, fake::fakeOpenAiKey(), why) ? 1 : 0, 1);
        agent.waitIdle();
        s = agent.chatState();
        P.eq("effort.openai_levels", s.effort == "medium" && byId(s.models, "o3-probe").efforts.size() == 3
                                         && byId(s.models, "gpt-4o-probe").efforts.empty() && byId(s.models, "gpt-5-probe").efforts.size() == 4 ? 1 : 0, 1);
        agent.setEffort("high");
        before = script->postCount();
        const auto errors = count(agent.chatState(), pg::ChatEntry::Kind::error);
        script->add(fake::Response::failure(400, "invalid_request_error", "Unsupported parameter: 'reasoning_effort' is not supported with this model."));
        script->add(stream(fake::OpenAiSse("o3-probe").text("Without it.").finish("stop").usage(9000, 8).done().str()));
        agent.send("think hard");
        agent.waitIdle();
        s = agent.chatState();
        print("effort refused", s);
        const juce::var r0 = claude::parseJson(script->body(before)), r1 = claude::parseJson(script->body(before + 1));
        P.eq("effort.openai_sent", str(r0, "reasoning_effort") == "high" && !r0.hasProperty("reasoning") ? 1 : 0, 1);
        P.eq("effort.refused_asked_again", script->postCount() - before == 2 && !r1.hasProperty("reasoning_effort") ? 1 : 0, 1);
        P.eq("effort.refused_no_failed_turn", lastText(s, pg::ChatEntry::Kind::assistant) == "Without it."
                                                  && count(s, pg::ChatEntry::Kind::error) == errors && !s.busy ? 1 : 0, 1);
        P.eq("effort.refused_noted", has(allOf(s, pg::ChatEntry::Kind::note), "The model (o3-probe) refused the thinking level High") ? 1 : 0, 1);
        P.eq("effort.refused_pills_gone", s.effort.empty() && byId(s.models, "o3-probe").efforts.empty()
                                              && keptLevel("openai https://api.openai.com/v1", "o3-probe").empty() ? 1 : 0, 1);
        agent.setEffort("low");
        before = script->postCount();
        script->add(stream(fake::OpenAiSse("o3-probe").text("Still without.").finish("stop").usage(9000, 8).done().str()));
        agent.send("again");
        agent.waitIdle();
        P.eq("effort.refused_for_the_session", agent.chatState().effort.empty()
                                                   && !claude::parseJson(script->body(before)).hasProperty("reasoning_effort") ? 1 : 0, 1);
        agent.setModel("gpt-4o-probe");
        before = script->postCount();
        script->add(stream(fake::OpenAiSse("gpt-4o-probe").text("No levels here.").finish("stop").usage(9000, 8).done().str()));
        agent.send("and you?");
        agent.waitIdle();
        P.eq("effort.openai_gpt4o_none", agent.chatState().effort.empty()
                                             && !claude::parseJson(script->body(before)).hasProperty("reasoning_effort") ? 1 : 0, 1);
        // The refusal holds when the list is read again (another provider and back): the model's levels stay gone.
        const int lists = script->count("GET /v1/models");
        P.eq("effort.relist_away", agent.setProvider({}, "", why) ? 1 : 0, 1);
        P.eq("effort.relist_back", agent.setProvider({ "openai", "https://api.openai.com/v1", "o3-probe" }, "", why) ? 1 : 0, 1);
        agent.waitIdle();
        s = agent.chatState();
        P.eq("effort.refused_relisted", script->count("GET /v1/models") > lists && script->lastHost("GET /v1/models") == "https://api.openai.com"
                                            && s.model == "o3-probe" && s.effort.empty() && byId(s.models, "o3-probe").efforts.empty()
                                            && byId(s.models, "gpt-5-probe").efforts.size() == 4 ? 1 : 0, 1);

        // Gemini: Gemini 3 by thinkingLevel (no Off), 2.5 by thinkingBudget (Off on flash), its thoughts shown and never
        // kept, a refusal asked again without.
        P.eq("effort.gemini_set", agent.setProvider({ "gemini", "", "gemini-3-pro-preview" }, "", why) ? 1 : 0, 1);
        agent.waitIdle();
        P.eq("effort.gemini3_default", agent.chatState().effort == "high" ? 1 : 0, 1);
        agent.setEffort("none");                  // Gemini 3 cannot turn thinking off
        P.eq("effort.gemini3_no_off", agent.chatState().effort == "high" ? 1 : 0, 1);
        agent.setEffort("low");
        before = script->postCount();
        script->add(stream(fake::GeminiSse("gemini-3-pro-preview").text("Weighing it.", true).text("Low it is.").finish("STOP").str()));
        agent.send("think a little");
        agent.waitIdle();
        s = agent.chatState();
        const juce::var gs = claude::parseJson(script->body(before));
        P.eq("effort.gemini3_sent", str(thinking(gs), "thinkingLevel") == "low" && !thinking(gs).hasProperty("thinkingBudget")
                                        && static_cast<bool>(thinking(gs).getProperty("includeThoughts", false)) ? 1 : 0, 1);
        P.eq("effort.gemini_thoughts_shown", lastText(s, pg::ChatEntry::Kind::thinking) == "Weighing it."
                                                 && lastText(s, pg::ChatEntry::Kind::assistant) == "Low it is." ? 1 : 0, 1);
        bool thoughtKept = false;
        for (const juce::var& m : arr(claude::parseJson(agent.saveState()).getProperty("messages", {})))
            thoughtKept = thoughtKept || has(claude::toJson(m), "Weighing it.");
        P.eq("effort.gemini_thoughts_not_kept", thoughtKept ? 1 : 0, 0);
        agent.setModel("gemini-2.5-flash");
        s = agent.chatState();
        P.eq("effort.gemini25_levels", s.effort.empty() && byId(s.models, "gemini-2.5-flash").efforts.size() == 4 ? 1 : 0, 1);
        agent.setEffort("none");
        before = script->postCount();
        script->add(stream(fake::GeminiSse("gemini-2.5-flash").text("Off.").finish("STOP").str()));
        agent.send("no thinking");
        agent.waitIdle();
        const juce::var go = claude::parseJson(script->body(before));
        P.eq("effort.gemini25_off_sent", thinking(go).hasProperty("thinkingBudget") && static_cast<int>(thinking(go).getProperty("thinkingBudget", -1)) == 0
                                             && !thinking(go).hasProperty("thinkingLevel") && !thinking(go).hasProperty("includeThoughts") ? 1 : 0, 1);
        agent.setEffort("medium");
        before = script->postCount();
        script->add(fake::Response::failure(400, "INVALID_ARGUMENT", "Unable to submit request because thinking_budget is not supported by this model."));
        script->add(stream(fake::GeminiSse("gemini-2.5-flash").text("Fine without.").finish("STOP").str()));
        agent.send("medium then");
        agent.waitIdle();
        s = agent.chatState();
        P.eq("effort.gemini_refused", script->postCount() - before == 2
                                          && static_cast<int>(thinking(claude::parseJson(script->body(before))).getProperty("thinkingBudget", -1)) == 8192
                                          && !claude::parseJson(script->body(before + 1)).hasProperty("generationConfig")
                                          && has(allOf(s, pg::ChatEntry::Kind::note), "The model (gemini-2.5-flash) refused the thinking level Medium")
                                          && s.effort.empty() && byId(s.models, "gemini-2.5-flash").efforts.empty()
                                          && lastText(s, pg::ChatEntry::Kind::assistant) == "Fine without." ? 1 : 0, 1);
    }

    // ---- REDIRECT ----------------------------------------------------------------------------------------------------------
    {
        // The real transport, against two servers of the probe's own on 127.0.0.1 (nothing beyond this Mac): the first
        // answers 307 to the second, which must never be reached.
        juce::StreamingSocket first, second;
        const bool listening = first.createListener(0, "127.0.0.1") && second.createListener(0, "127.0.0.1");
        P.eq("redirect.listening", listening ? 1 : 0, 1);
        if (listening)
        {
            const std::string to = "http://127.0.0.1:" + std::to_string(second.getBoundPort()) + "/v1/chat/completions";
            std::thread server([&first, to] {
                if (first.waitUntilReady(true, 20000) != 1)
                    return;
                const std::unique_ptr<juce::StreamingSocket> c(first.waitForNextConnection());
                if (c == nullptr || readRequest(*c).empty())   // the request whole, then the redirect
                    return;
                const std::string reply = "HTTP/1.1 307 Temporary Redirect\r\nLocation: " + to
                                        + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                c->write(reply.data(), static_cast<int>(reply.size()));
            });
            const std::atomic<bool> cancel{ false };
            const claude::HttpResult r = claude::httpsTransport()->post("http://127.0.0.1:" + std::to_string(first.getBoundPort()),
                                                                        "/v1/chat/completions", "{\"probe\":true}",
                                                                        { { "content-type", "application/json" } },
                                                                        [](std::string_view) { return true; }, cancel);
            server.join();
            std::printf("redirect: status %d, location %s, error %s\n", r.status, r.location.c_str(), r.error.c_str());
            P.eq("redirect.returned", r.status, 307);
            P.eq("redirect.location", r.location == to ? 1 : 0, 1);
            P.eq("redirect.not_followed", second.waitUntilReady(true, 500), 0);   // 1: a connection came
        }
    }

    // ---- TRANSPORT ---------------------------------------------------------------------------------------------------------
    {
        const auto post = [](const std::string& host, const std::function<bool(std::string_view)>& onChunk,
                             const std::atomic<bool>& cancel) {
            return claude::httpsTransport()->post(host, "/v1/messages", "{\"probe\":true}",
                                                  { { "content-type", "application/json" } }, onChunk, cancel);
        };
        // https://: the transport opens a TLS connection (the server, plain TCP, sees a handshake record and closes).
        {
            juce::StreamingSocket listener;
            const bool listening = listener.createListener(0, "127.0.0.1");
            std::atomic<int> firstByte{ -1 };
            std::thread server([&] {
                if (!listening || listener.waitUntilReady(true, 20000) != 1)
                    return;
                const std::unique_ptr<juce::StreamingSocket> c(listener.waitForNextConnection());
                listener.close();                        // a second try is refused at once, never left waiting
                unsigned char b = 0;
                if (c != nullptr && c->waitUntilReady(true, 5000) == 1 && c->read(&b, 1, false) == 1)
                    firstByte = b;
            });
            const std::atomic<bool> cancel{ false };
            const claude::HttpResult r = post("https://127.0.0.1:" + std::to_string(listener.getBoundPort()), {}, cancel);
            server.join();
            std::printf("transport: https first byte %d, status %d, error %s\n", firstByte.load(), r.status, r.error.c_str());
            P.eq("transport.https_speaks_tls", listening && firstByte.load() == 0x16 ? 1 : 0, 1);
            P.eq("transport.https_refused_plain", r.status == 0 && !r.error.empty() ? 1 : 0, 1);
        }
        // A 429: its body whole (never to onChunk) and its retry-after.
        {
            juce::StreamingSocket listener;
            const bool listening = listener.createListener(0, "127.0.0.1");
            const std::string body = "{\"type\":\"error\",\"error\":{\"type\":\"rate_limit_error\",\"message\":\"slow down\"}}";
            std::thread server([&] {
                if (!listening || listener.waitUntilReady(true, 20000) != 1)
                    return;
                const std::unique_ptr<juce::StreamingSocket> c(listener.waitForNextConnection());
                if (c == nullptr || readRequest(*c).empty())
                    return;
                sendText(*c, "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 7\r\nContent-Type: application/json\r\nContent-Length: "
                             + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
            });
            int chunks = 0;
            const std::atomic<bool> cancel{ false };
            const claude::HttpResult r = post("http://127.0.0.1:" + std::to_string(listener.getBoundPort()),
                                              [&chunks](std::string_view) { ++chunks; return true; }, cancel);
            server.join();
            P.eq("transport.error_status", r.status, 429);
            P.eq("transport.error_body", r.body == body && chunks == 0 ? 1 : 0, 1);
            P.eq("transport.retry_after", r.retryAfterSeconds, 7);
        }
        // A stream: one event, then the server waits. On Linux the first piece reaches onChunk before the next is sent
        // (JUCE's macOS client fills its whole read first); then `cancel` ends the call soon.
        {
            juce::StreamingSocket listener;
            const bool listening = listener.createListener(0, "127.0.0.1");
            std::atomic<bool> firstGot{ false }, secondSent{ false }, release{ false };
            std::atomic<bool> gotBeforeSecond{ false };
            std::thread server([&] {
                if (!listening || listener.waitUntilReady(true, 20000) != 1)
                    return;
                const std::unique_ptr<juce::StreamingSocket> c(listener.waitForNextConnection());
                if (c == nullptr || readRequest(*c).empty())
                    return;
                sendText(*c, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\nevent: ping\ndata: {}\n\n");
                for (int i = 0; i < 300 && !firstGot.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                gotBeforeSecond = firstGot.load();
                sendText(*c, "event: ping\ndata: {}\n\n");
                secondSent = true;
                for (int i = 0; i < 1000 && !release.load(); ++i)     // held open until the probe is done
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            });
            std::atomic<bool> cancel{ false };
            std::string streamed;
            claude::HttpResult r;
            std::atomic<bool> returned{ false };
            std::thread client([&] {
                r = post("http://127.0.0.1:" + std::to_string(listener.getBoundPort()),
                         [&](std::string_view piece) {
                             streamed.append(piece);
                             firstGot = true;
                             return true;
                         },
                         cancel);
                returned = true;
            });
            for (int i = 0; i < 1000 && !secondSent.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));   // the second piece read; the server holds on
            const auto from = std::chrono::steady_clock::now();
            cancel = true;
            for (int i = 0; i < 500 && !returned.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
            release = true;
            client.join();
            server.join();
            std::printf("transport: stream got before the second %d, stopped in %.3f s (%s), %zu bytes\n",
                        gotBeforeSecond.load() ? 1 : 0, took, r.error.c_str(), streamed.size());
            P.eq("transport.stream_status", r.status, 200);
            P.eq("transport.cancel_soon", returned.load() && took < 0.5 && r.error == "stopped" ? 1 : 0, 1);
#if defined(__linux__)
            P.eq("transport.piece_by_piece", gotBeforeSecond.load() && streamed.rfind("event: ping\ndata: {}\n\n", 0) == 0 ? 1 : 0, 1);
#endif
        }
    }
    pg::chatstore::setDirectoryForTests(juce::File());
    return P.finish();
}
