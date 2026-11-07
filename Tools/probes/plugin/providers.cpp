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
//             (types in upper case, no additionalProperties); a key it calls not valid: refused, said
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
//   REDIRECT  the real transport (ClaudeApi.cpp) against two servers of the probe's own on 127.0.0.1: a 307 from
//             the first is returned as it came, its location said, and the second is never reached
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
        for (const juce::var& dcl : d2)
            subset = subset && !dcl.hasProperty("parametersJsonSchema")
                  && (!dcl.hasProperty("parameters") || str(dcl.getProperty("parameters", {}), "type") == "OBJECT");
        P.eq("gemini.subset", script->postCount() - b2 == 2 && subset && !anyHas(retry.getProperty("tools", {}), "additionalProperties")
                                  && !anyHas(retry.getProperty("tools", {}), "anyOf") ? 1 : 0, 1);
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
                if (c == nullptr)
                    return;
                // The request whole (its headers, then its body by its length), then the redirect.
                std::string got;
                char buffer[4096];
                std::size_t need = std::string::npos;
                while (need == std::string::npos || got.size() < need)
                {
                    if (c->waitUntilReady(true, 5000) != 1)
                        return;
                    const int n = c->read(buffer, static_cast<int>(sizeof buffer), false);
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
    pg::chatstore::setDirectoryForTests(juce::File());
    return P.finish();
}
