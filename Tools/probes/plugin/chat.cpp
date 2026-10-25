// PROBE name=plugin.chat timeout=180
//
// plugin.chat: the chat's agent (ChatAgent.h) over a real processor, against a scripted API (FakeClaude.h): nothing
// reaches the network, and the key folder is the probe's own.
//   PARSE    SseParser gives the same events however the stream is cut (every chunk size from 1 to 64; CR LF line
//            ends); MessageBuilder puts a message back together: a thinking block with its summary and signature, a
//            text block, a tool_use block whose input streamed in pieces, the stop reason and the usage, and the live
//            callbacks; a tool input that is not a JSON object is {} with its raw text in invalidToolInputs
//   NOKEY    without a key a message ends at once with an error that asks for one, and nothing is sent
//   TURN     with a key, a message: Claude thinks, writes program.fm (a level control) and runs it, sets its dial, and
//            answers. The draft holds the program and it plays, the dial has the value, the conversation shows the
//            user's message, the thinking, the answer and what Claude did; the models are /v1/models's with adaptive
//            thinking, each with its levels
//   REQUEST  each request has the model, max_tokens (the model's own limit, from /v1/models), stream, adaptive
//            thinking (summarized), the effort, the system prompt cached for an hour, the automatic cache breakpoint,
//            the nine tools each with eager_input_streaming, "fallbacks" and server-side compaction (its edit, trigger
//            and instructions) with their beta headers for a model that takes them, the API version and the key, and
//            no temperature, top_p or top_k; the first message carries the plugin's state; the history is append-only
//            (each request's messages begin with the previous request's, unchanged, signatures included) and the
//            tool results answer the tool calls by id
//   CHOICE   another model and level: the next request has them; a model that takes no fallbacks and no compaction
//            gets neither (and no beta header), and its own max_tokens; a level the model does not take is refused; a
//            change during a turn waits for the next
//   INVALID  a tool call whose input is not JSON gets an INVALID_JSON error result, runs nothing, and the turn goes on
//   RETRY    an overloaded API (529, retry-after 1 s) is asked again and the turn completes
//   ERROR    a key the API refuses (401): the turn ends with the API's message, and the next message joins the
//            unanswered one (the roles keep alternating)
//   REFUSAL  stop_reason refusal: the turn ends with an error entry; the answer stays in the history as text alone
//   FALLBACK a fallback block: a note; in the history the blocks before it keep only their text and it is dropped
//   CONTEXT  the plugin's state names a file Claude has seen (as it wrote it) without its text; a file the user edited
//            goes with its text; when nothing changed no state goes at all
//   MEASURE  measure_output through the agent: its result is the render's report, with the dials asked for
//   BROKEN   a stream cut short (no message_stop) is asked again; what it showed is taken back, and the answer appears
//            once
//   COMPACT  a compaction block: a note, the usage counts the compaction; the next request starts at the user message
//            before it (what came before is dropped), echoes the block, and sends the files' text again
//   STOP     a stalled stream: stop() ends the turn at once; the history ends with Claude's text so far
//   STATE    the conversation is in the session: a second processor loading the first's state has the same entries
//            and the same history
//   NEW      newChat forgets the conversation
#include "Probe.h"

#include "FakeClaude.h"

#include "plugin/ChatAgent.h"
#include "plugin/ChatStore.h"
#include "plugin/ClaudeApi.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace tp = testingplaygroundsynth;
namespace claude = testingplaygroundsynth::claude;

namespace
{
    // A program with one dial, "level", from 0 to 2.
    std::string levelProgram()
    {
        const std::string param = "    param float level [[fminus::range(0, 2)]] [[fminus::default(1)]];\n";
        if constexpr (tp::product::kIsInstrument)
            return "module Voice\n{\n" + param +
                   "    void start(float n, float v) {}\n"
                   "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n";
        else
            return "module Effect\n{\n" + param +
                   "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        for (int i = 0; i < n; ++i)\n        {\n"
                   "            left[i] *= level[i];\n            right[i] *= level[i];\n        }\n    }\n};\n";
    }

    std::string str(const juce::var& v, const char* key) { return v.getProperty(key, "").toString().toStdString(); }

    juce::Array<juce::var> arr(const juce::var& v)
    {
        if (const auto* a = v.getArray())
            return *a;
        return {};
    }

    juce::Array<juce::var> messagesOf(const std::string& body) { return arr(claude::parseJson(body).getProperty("messages", {})); }

    // The roles alternate, from the user's.
    bool alternates(const juce::Array<juce::var>& messages)
    {
        for (int i = 0; i < messages.size(); ++i)
            if (str(messages[i], "role") != (i % 2 == 0 ? "user" : "assistant"))
                return false;
        return true;
    }

    // `later` begins with every message of `earlier`, unchanged.
    bool extends(const juce::Array<juce::var>& later, const juce::Array<juce::var>& earlier)
    {
        if (later.size() <= earlier.size())
            return false;
        for (int i = 0; i < earlier.size(); ++i)
            if (claude::toJson(later[i]) != claude::toJson(earlier[i]))
                return false;
        return true;
    }

    std::string lastText(const tp::ChatState& s, tp::ChatEntry::Kind kind)
    {
        for (auto it = s.entries.rbegin(); it != s.entries.rend(); ++it)
            if (it->kind == kind)
                return it->text;
        return {};
    }

    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    // The conversation, a line an entry, for the log.
    void print(const char* what, const tp::ChatState& s)
    {
        static const char* const names[] = { "user", "assistant", "thinking", "tool", "error", "note" };
        std::printf("%s:\n", what);
        for (const tp::ChatEntry& e : s.entries)
            std::printf("  %-9s %s\n", names[static_cast<int>(e.kind)], e.text.substr(0, 110).c_str());
    }

    juce::var input(std::initializer_list<std::pair<const char*, juce::var>> fields) { return claude::object(fields); }
}

PROBE(plugin, chat)
{
    const funkgui::HeadlessGuiScope gui;
    const juce::File keys = juce::File(juce::String(probe::outPath("chat-keys")));
    keys.deleteRecursively();
    tp::chatstore::setDirectoryForTests(keys);

    // ---- PARSE ---------------------------------------------------------------------------------------------------------
    {
        fake::Sse sse;
        sse.start("claude-opus-5-5", 1200, 300, 40)
            .thinking("Plan: a gain stage.", "sig-abc")
            .text("Hello there, a gain.")
            .tool("toolu_1", "write_file", input({ { "name", "program.fm" }, { "content", "x\ny" } }))
            .ping()
            .end("tool_use", 77);
        using Events = std::vector<std::pair<std::string, std::string>>;
        const auto collect = [](const std::string& stream, std::size_t chunk) {
            Events out;
            claude::SseParser p;
            for (std::size_t at = 0; at < stream.size(); at += chunk)
                p.feed(std::string_view(stream).substr(at, chunk),
                       [&](const std::string& e, const std::string& d) { out.emplace_back(e, d); });
            return out;
        };
        const Events whole = collect(sse.str(), sse.str().size());
        int same = 0;
        for (std::size_t n = 1; n <= 64; ++n)
            same += collect(sse.str(), n) == whole ? 1 : 0;
        int64_t sent = 0;
        for (std::size_t at = sse.str().find("event: "); at != std::string::npos; at = sse.str().find("event: ", at + 1))
            ++sent;
        P.eq("parse.events", static_cast<int64_t>(whole.size()), sent);
        P.eq("parse.cut_anywhere", same, 64);

        std::string texts, thoughts;
        std::vector<std::string> tools;
        claude::MessageBuilder::Callbacks cb;
        cb.text = [&](std::string_view t) { texts += t; };
        cb.thinking = [&](std::string_view t) { thoughts += t; };
        cb.toolStart = [&](const std::string& name) { tools.push_back(name); };
        claude::MessageBuilder b(cb);
        for (const auto& [e, d] : whole)
            b.event(e, d);
        const juce::Array<juce::var> content = arr(b.content());
        P.eq("parse.blocks", content.size(), 3);
        P.eq("parse.thinking", content.size() == 3 && str(content[0], "type") == "thinking"
                                       && str(content[0], "thinking") == "Plan: a gain stage."
                                       && str(content[0], "signature") == "sig-abc" ? 1 : 0, 1);
        P.eq("parse.text", content.size() == 3 && str(content[1], "text") == "Hello there, a gain." ? 1 : 0, 1);
        const juce::var use = content.size() == 3 ? content[2] : juce::var();
        P.eq("parse.tool_use", str(use, "type") == "tool_use" && str(use, "id") == "toolu_1" && str(use, "name") == "write_file"
                                   && str(use.getProperty("input", {}), "content") == "x\ny" ? 1 : 0, 1);
        P.eq("parse.stop_reason", b.stopReason() == "tool_use" ? 1 : 0, 1);
        P.eq("parse.finished", b.finished() ? 1 : 0, 1);
        P.eq("parse.usage_in", b.usage().input, 1200);
        P.eq("parse.usage_cached", b.usage().cacheRead, 300);
        P.eq("parse.usage_out", b.usage().output, 77);
        P.eq("parse.text_callbacks", texts == "Hello there, a gain." ? 1 : 0, 1);
        P.eq("parse.thinking_callbacks", thoughts == "Plan: a gain stage." ? 1 : 0, 1);
        P.eq("parse.tool_callback", tools.size() == 1 && tools[0] == "write_file" ? 1 : 0, 1);

        claude::MessageBuilder bad;
        fake::Sse broken;
        broken.start().toolUse("toolu_x", "run_program", "{\"a\": ").end("tool_use");
        claude::SseParser p;
        p.feed(broken.str(), [&](const std::string& e, const std::string& d) { bad.event(e, d); });
        P.eq("parse.invalid_listed", bad.invalidToolInputs().size() == 1 && bad.invalidToolInputs()[0].first == "toolu_x"
                                         && bad.invalidToolInputs()[0].second == "{\"a\": " ? 1 : 0, 1);
        // A compaction block, and a usage with its iterations.
        std::size_t summarised = 0;
        claude::MessageBuilder::Callbacks ccb;
        ccb.compaction = [&](std::size_t n) { summarised = n; };
        claude::MessageBuilder comp(ccb);
        fake::Sse compacted;
        compacted.start().compaction("The story so far.").text("Go on.").endCompacted(160000, 2500, 3000, 120);
        claude::SseParser cp;
        cp.feed(compacted.str(), [&](const std::string& e, const std::string& d) { comp.event(e, d); });
        const juce::Array<juce::var> cc = arr(comp.content());
        P.eq("parse.compaction_block", cc.size() == 2 && str(cc[0], "type") == "compaction"
                                           && str(cc[0], "content") == "The story so far." ? 1 : 0, 1);
        P.eq("parse.compaction_callback", static_cast<int64_t>(summarised), 17);
        P.eq("parse.compaction_usage", comp.usage().input == 163000 && comp.usage().output == 2620
                                           && comp.usage().compactions == 1 ? 1 : 0, 1);

        const juce::Array<juce::var> badContent = arr(bad.content());
        const auto* in = badContent.isEmpty() ? nullptr : badContent[0].getProperty("input", {}).getDynamicObject();
        P.eq("parse.invalid_input_empty", in != nullptr && in->getProperties().size() == 0 ? 1 : 0, 1);
    }

    tp::Processor processor;
    tp::ChatAgent& agent = processor.chatAgent();
    auto script = std::make_shared<fake::Script>();
    script->models = fake::modelsBody({ { "claude-opus-5-5", "Claude Opus 5.5", true, { "low", "medium", "high", "xhigh", "max" } },
                                        { "claude-sonnet-5-5", "Claude Sonnet 5.5", true, { "low", "medium", "high", "xhigh" } },
                                        { "claude-haiku-5-5", "Claude Haiku 5.5", true, { "low", "medium", "high" }, 64000, false },
                                        { "claude-old-4", "Claude Old 4", false, {} } });
    agent.setTransport(std::make_unique<fake::Transport>(script));

    // ---- NOKEY ---------------------------------------------------------------------------------------------------------
    {
        P.eq("nokey.state", agent.chatState().hasKey ? 1 : 0, 0);
        agent.send("hello");
        agent.waitIdle();
        const tp::ChatState s = agent.chatState();
        P.eq("nokey.asks_for_key", has(lastText(s, tp::ChatEntry::Kind::error), "API key") ? 1 : 0, 1);
        P.eq("nokey.nothing_sent", static_cast<int64_t>(script->paths.size()), 0);
        P.eq("nokey.idle", s.busy ? 1 : 0, 0);
        agent.newChat();
    }

    // ---- TURN ----------------------------------------------------------------------------------------------------------
    std::string why;
    P.eq("turn.key_saved", agent.saveKey(fake::fakeKey(), why) ? 1 : 0, 1);
    const std::string program = levelProgram();
    script->add(fake::Response::ok(fake::Sse()
                                       .start("claude-opus-5-5", 5000, 0, 4800)
                                       .thinking("A level control: one dial.", "sig-1")
                                       .text("Writing it now.")
                                       .tool("toolu_1", "write_file", input({ { "name", "program.fm" },
                                                                              { "content", juce::String(program) } }))
                                       .tool("toolu_2", "run_program", input({}))
                                       .end("tool_use")));
    script->add(fake::Response::ok(fake::Sse().start("claude-opus-5-5", 5600, 4800).tool("toolu_3", "set_dial",
                                                                                         input({ { "name", "level" }, { "value", 1.5 } }))
                                       .end("tool_use")));
    script->add(fake::Response::ok(fake::Sse().start("claude-opus-5-5", 5800, 5600).text("Done: a level control, at 1.5.").end()));
    agent.send("make a level control");
    agent.waitIdle();
    processor.settle();
    {
        const tp::ChatState s = agent.chatState();
        print("turn", s);
        const tp::Draft d = processor.editorPort().draft();
        P.eq("turn.requests", script->postCount(), 3);
        P.eq("turn.models_fetched", !script->paths.empty() && script->paths[0] == "GET /v1/models?limit=100" ? 1 : 0, 1);
        P.eq("turn.draft_written", !d.files.empty() && d.files[0].text == program ? 1 : 0, 1);
        P.eq("turn.plays", processor.editorPort().running() == d.files ? 1 : 0, 1);
        P.eq("turn.dial_named", std::string(processor.control(0).name) == "level" ? 1 : 0, 1);
        P.near("turn.dial_value", processor.getParameters()[0]->getValue(), 0.75, 1e-4);
        std::vector<tp::ChatEntry::Kind> kinds;
        for (const tp::ChatEntry& e : s.entries)
            kinds.push_back(e.kind);
        using K = tp::ChatEntry::Kind;
        const std::vector<K> want{ K::note, K::user, K::thinking, K::assistant, K::tool, K::tool, K::tool, K::assistant };
        P.eq("turn.entries", kinds == want ? 1 : 0, 1);
        P.eq("turn.answer", lastText(s, K::assistant) == "Done: a level control, at 1.5." ? 1 : 0, 1);
        P.eq("turn.thinking_shown", lastText(s, K::thinking) == "A level control: one dial." ? 1 : 0, 1);
        P.eq("turn.dial_note", has(lastText(s, K::tool), "level") ? 1 : 0, 1);
        P.eq("turn.models_adaptive", static_cast<int64_t>(s.models.size()), 3);
        bool sonnetLevels = false;
        for (const tp::ChatModel& m : s.models)
            if (m.id == "claude-sonnet-5-5")
                sonnetLevels = m.efforts == std::vector<std::string>{ "low", "medium", "high", "xhigh" };
        P.eq("turn.model_levels", sonnetLevels ? 1 : 0, 1);
        P.eq("turn.usage", has(s.usage, "cached") ? 1 : 0, 1);
        P.eq("turn.idle", s.busy ? 1 : 0, 0);
    }

    // ---- REQUEST -------------------------------------------------------------------------------------------------------
    {
        const juce::var b0 = claude::parseJson(script->body(0));
        P.eq("request.model", str(b0, "model") == "claude-opus-5-5" ? 1 : 0, 1);
        P.eq("request.max_tokens", static_cast<int64_t>(static_cast<int>(b0.getProperty("max_tokens", 0))), 128000);
        const juce::var edits = b0.getProperty("context_management", {}).getProperty("edits", {});
        const juce::Array<juce::var> editList = arr(edits);
        P.eq("request.compaction_edit",
             editList.size() == 1 && str(editList[0], "type") == "compact_20260112"
                     && static_cast<int>(editList[0].getProperty("trigger", {}).getProperty("value", 0)) == 150000
                     && !str(editList[0], "instructions").empty() ? 1 : 0, 1);
        P.eq("request.stream", static_cast<bool>(b0.getProperty("stream", false)) ? 1 : 0, 1);
        const juce::var thinking = b0.getProperty("thinking", {});
        P.eq("request.thinking_adaptive", str(thinking, "type") == "adaptive" && str(thinking, "display") == "summarized" ? 1 : 0, 1);
        P.eq("request.effort", str(b0.getProperty("output_config", {}), "effort") == "medium" ? 1 : 0, 1);
        const juce::Array<juce::var> system = arr(b0.getProperty("system", {}));
        P.eq("request.system_cached_1h",
             system.size() == 1 && str(system[0].getProperty("cache_control", {}), "ttl") == "1h"
                     && str(system[0], "text") == tp::ChatAgent::systemPrompt() ? 1 : 0, 1);
        P.eq("request.auto_cache", str(b0.getProperty("cache_control", {}), "type") == "ephemeral" ? 1 : 0, 1);
        const juce::Array<juce::var> tools = arr(b0.getProperty("tools", {}));
        int eager = 0;
        for (const auto& t : tools)
            eager += static_cast<bool>(t.getProperty("eager_input_streaming", false)) ? 1 : 0;
        P.eq("request.tools", tools.size(), 9);
        P.eq("request.tools_eager", eager, 9);
        P.eq("request.fallbacks", str(b0, "fallbacks") == "default" ? 1 : 0, 1);
        P.eq("request.beta_header",
             script->header(1, "anthropic-beta") == "server-side-fallback-2026-07-01,compact-2026-01-12" ? 1 : 0, 1);
        P.eq("request.version", script->header(1, "anthropic-version") == claude::kApiVersion ? 1 : 0, 1);
        P.eq("request.key", script->header(1, "x-api-key") == fake::fakeKey() ? 1 : 0, 1);
        P.eq("request.no_sampling", b0.hasProperty("temperature") || b0.hasProperty("top_p") || b0.hasProperty("top_k") ? 1 : 0, 0);
        const juce::Array<juce::var> m0 = messagesOf(script->body(0));
        const juce::Array<juce::var> first = arr(m0.isEmpty() ? juce::var() : m0[0].getProperty("content", {}));
        P.eq("request.user_text", first.size() == 2 && str(first[0], "text") == "make a level control" ? 1 : 0, 1);
        P.eq("request.plugin_state", first.size() == 2 && str(first[1], "text").rfind("<plugin_state>", 0) == 0 ? 1 : 0, 1);
        const juce::Array<juce::var> m1 = messagesOf(script->body(1)), m2 = messagesOf(script->body(2));
        P.eq("request.append_only", extends(m1, m0) && extends(m2, m1) ? 1 : 0, 1);
        P.eq("request.alternates", alternates(m2) ? 1 : 0, 1);
        const juce::Array<juce::var> answer = arr(m1.size() > 1 ? m1[1].getProperty("content", {}) : juce::var());
        P.eq("request.signature_kept",
             answer.size() == 4 && str(answer[0], "signature") == "sig-1" && str(answer[0], "thinking") == "A level control: one dial."
                 ? 1 : 0, 1);
        const juce::Array<juce::var> results = arr(m1.size() > 2 ? m1[2].getProperty("content", {}) : juce::var());
        P.eq("request.results_by_id",
             results.size() == 2 && str(results[0], "tool_use_id") == "toolu_1" && str(results[1], "tool_use_id") == "toolu_2"
                     && !results[0].hasProperty("is_error") && !results[1].hasProperty("is_error") ? 1 : 0, 1);
    }

    // ---- CHOICE --------------------------------------------------------------------------------------------------------
    {
        agent.setModel("claude-sonnet-5-5");
        agent.setEffort("high");
        script->add(fake::Response::ok(fake::Sse().start("claude-sonnet-5-5").text("Sure.").end()));
        agent.send("thanks");
        agent.waitIdle();
        const int n = script->postCount();
        const juce::var b = claude::parseJson(script->body(n - 1));
        P.eq("choice.model", str(b, "model") == "claude-sonnet-5-5" ? 1 : 0, 1);
        P.eq("choice.effort", str(b.getProperty("output_config", {}), "effort") == "high" ? 1 : 0, 1);
        agent.setModel("claude-haiku-5-5");
        agent.setEffort("max");                         // haiku takes no max
        P.eq("choice.level_refused", agent.chatState().effort == "high" ? 1 : 0, 1);
        script->add(fake::Response::ok(fake::Sse().start("claude-haiku-5-5").text("Hi.").end()));
        agent.send("and again");
        agent.waitIdle();
        const juce::var h = claude::parseJson(script->body(script->postCount() - 1));
        P.eq("choice.haiku", str(h, "model") == "claude-haiku-5-5" ? 1 : 0, 1);
        P.eq("choice.no_fallbacks", h.hasProperty("fallbacks") ? 1 : 0, 0);
        P.eq("choice.no_compaction", h.hasProperty("context_management") ? 1 : 0, 0);
        P.eq("choice.own_max_tokens", static_cast<int64_t>(static_cast<int>(h.getProperty("max_tokens", 0))), 64000);
        P.eq("choice.no_beta", script->header(static_cast<int>(script->headers.size()) - 1, "anthropic-beta").empty() ? 1 : 0, 1);
        P.eq("choice.stored", tp::chatstore::loadChoice().model == "claude-haiku-5-5" ? 1 : 0, 1);

        // A change during a turn waits for the next: the turn's second request keeps the first's model.
        fake::Response slow = fake::Response::ok(fake::Sse().start().tool("toolu_s", "get_status", input({})).end("tool_use"));
        script->add(slow);
        script->add(fake::Response::ok(fake::Sse().start().text("Status read.").end()));
        agent.setModel("claude-opus-5-5");
        const int before = script->postCount();
        agent.send("status?");
        agent.setModel("claude-sonnet-5-5");            // while the turn runs (or before its first request: either way
        agent.waitIdle();                               // both of its requests have the same model)
        const std::string m1 = str(claude::parseJson(script->body(before)), "model");
        const std::string m2 = str(claude::parseJson(script->body(before + 1)), "model");
        P.eq("choice.fixed_for_turn", m1 == m2 ? 1 : 0, 1);
        agent.setModel("claude-opus-5-5");
        agent.setEffort("medium");
    }

    // ---- INVALID -------------------------------------------------------------------------------------------------------
    {
        script->add(fake::Response::ok(fake::Sse().start().toolUse("toolu_9", "write_file", "{\"name\": \"x.fm\", \"content\": ")
                                           .end("tool_use")));
        script->add(fake::Response::ok(fake::Sse().start().text("Sorry, again.").end()));
        agent.send("write x.fm");
        agent.waitIdle();
        const juce::Array<juce::var> m = messagesOf(script->body(script->postCount() - 1));
        const juce::Array<juce::var> results = arr(m.isEmpty() ? juce::var() : m[m.size() - 1].getProperty("content", {}));
        P.eq("invalid.error_result",
             results.size() == 1 && static_cast<bool>(results[0].getProperty("is_error", false))
                     && has(str(results[0], "content"), "INVALID_JSON") ? 1 : 0, 1);
        bool wrote = false;
        for (const tp::SourceFile& f : processor.editorPort().draft().files)
            wrote = wrote || f.name == "x.fm";
        P.eq("invalid.ran_nothing", wrote ? 1 : 0, 0);
        P.eq("invalid.turn_went_on", lastText(agent.chatState(), tp::ChatEntry::Kind::assistant) == "Sorry, again." ? 1 : 0, 1);
    }

    // ---- RETRY ---------------------------------------------------------------------------------------------------------
    {
        script->add(fake::Response::failure(529, "overloaded_error", "Overloaded", 1));
        script->add(fake::Response::ok(fake::Sse().start().text("Back.").end()));
        const int before = script->postCount();
        agent.send("are you there?");
        agent.waitIdle();
        P.eq("retry.asked_twice", script->postCount() - before, 2);
        P.eq("retry.answered", lastText(agent.chatState(), tp::ChatEntry::Kind::assistant) == "Back." ? 1 : 0, 1);
        P.eq("retry.same_request", script->body(before) == script->body(before + 1) ? 1 : 0, 1);
    }

    // ---- ERROR ---------------------------------------------------------------------------------------------------------
    {
        script->add(fake::Response::failure(401, "authentication_error", "invalid x-api-key"));
        const int before = script->postCount();
        agent.send("one");
        agent.waitIdle();
        P.eq("error.not_retried", script->postCount() - before, 1);
        P.eq("error.says_why", has(lastText(agent.chatState(), tp::ChatEntry::Kind::error), "invalid x-api-key") ? 1 : 0, 1);
        script->add(fake::Response::ok(fake::Sse().start().text("Answer.").end()));
        agent.send("two");
        agent.waitIdle();
        const juce::Array<juce::var> m = messagesOf(script->body(script->postCount() - 1));
        const juce::Array<juce::var> last = arr(m.isEmpty() ? juce::var() : m[m.size() - 1].getProperty("content", {}));
        bool one = false, two = false;
        for (const auto& b : last)
        {
            one = one || str(b, "text") == "one";
            two = two || str(b, "text") == "two";
        }
        P.eq("error.next_joins", one && two ? 1 : 0, 1);
        P.eq("error.alternates", alternates(m) ? 1 : 0, 1);
    }

    // ---- REFUSAL -------------------------------------------------------------------------------------------------------
    {
        script->add(fake::Response::ok(fake::Sse().start().thinking("hm", "sig-r").text("I can't help with").end("refusal")));
        agent.send("something it declines");
        agent.waitIdle();
        P.eq("refusal.error_entry", has(lastText(agent.chatState(), tp::ChatEntry::Kind::error), "declined") ? 1 : 0, 1);
        script->add(fake::Response::ok(fake::Sse().start().text("OK.").end()));
        agent.send("something else");
        agent.waitIdle();
        const juce::Array<juce::var> m = messagesOf(script->body(script->postCount() - 1));
        const juce::Array<juce::var> refused = arr(m.size() >= 2 ? m[m.size() - 2].getProperty("content", {}) : juce::var());
        P.eq("refusal.text_only", refused.size() == 1 && str(refused[0], "type") == "text"
                                          && str(refused[0], "text") == "I can't help with" ? 1 : 0, 1);
        P.eq("refusal.alternates", alternates(m) ? 1 : 0, 1);
    }

    // ---- FALLBACK ------------------------------------------------------------------------------------------------------
    {
        script->add(fake::Response::ok(fake::Sse()
                                           .start()
                                           .thinking("t", "sig-f")
                                           .text("Part one. ")
                                           .fallback("claude-opus-5-5", "claude-sonnet-5-5")
                                           .thinking("u", "sig-g")
                                           .text("Part two.")
                                           .end()));
        agent.send("a long one");
        agent.waitIdle();
        P.eq("fallback.note", has(lastText(agent.chatState(), tp::ChatEntry::Kind::note), "declined this request") ? 1 : 0, 1);
        script->add(fake::Response::ok(fake::Sse().start().text("OK.").end()));
        agent.send("go on");
        agent.waitIdle();
        const juce::Array<juce::var> m = messagesOf(script->body(script->postCount() - 1));
        const juce::Array<juce::var> c = arr(m.size() >= 2 ? m[m.size() - 2].getProperty("content", {}) : juce::var());
        P.eq("fallback.history",
             c.size() == 3 && str(c[0], "text") == "Part one. " && str(c[1], "type") == "thinking"
                     && str(c[1], "signature") == "sig-g" && str(c[2], "text") == "Part two." ? 1 : 0, 1);
    }

    // ---- CONTEXT -------------------------------------------------------------------------------------------------------
    {
        // The plugin's state in the last request's last user message ("" when it carries none).
        const auto lastState = [&] {
            const juce::Array<juce::var> m = messagesOf(script->body(script->postCount() - 1));
            if (m.isEmpty())
                return std::string();
            for (const auto& b : arr(m[m.size() - 1].getProperty("content", {})))
                if (str(b, "text").rfind("<plugin_state>", 0) == 0)
                    return str(b, "text");
            return std::string();
        };
        processor.getParameters()[0]->setValueNotifyingHost(0.3f);     // the dials changed: a state goes
        script->add(fake::Response::ok(fake::Sse().start().text("Noted.").end()));
        agent.send("what do you see?");
        agent.waitIdle();
        const std::string seen = lastState();
        P.eq("context.seen_file_named", has(seen, "<file name=\"program.fm\" lines=") && has(seen, "unchanged=\"true\"") ? 1 : 0, 1);
        P.eq("context.seen_file_no_text", has(seen, "left[i] *= level[i]") || has(seen, "return false;") ? 0 : 1, 1);
        tp::Draft d = processor.editorPort().draft();
        d.files[0].text += "// a comment of the user's\n";
        processor.editorPort().setDraft(d);
        script->add(fake::Response::ok(fake::Sse().start().text("Seen.").end()));
        agent.send("and now?");
        agent.waitIdle();
        P.eq("context.edited_file_sent", has(lastState(), "// a comment of the user's") ? 1 : 0, 1);
        script->add(fake::Response::ok(fake::Sse().start().text("Same.").end()));
        agent.send("again?");
        agent.waitIdle();
        P.eq("context.nothing_changed_nothing_sent", lastState().empty() ? 1 : 0, 1);
    }

    // ---- MEASURE -------------------------------------------------------------------------------------------------------
    {
        juce::Array<juce::var> notes;
        notes.add(60);
        script->add(fake::Response::ok(fake::Sse().start().tool("toolu_m", "measure_output",
                                                                 input({ { "notes", juce::var(notes) },
                                                                         { "dials", input({ { "level", 0.5 } }) } }))
                                           .end("tool_use")));
        script->add(fake::Response::ok(fake::Sse().start().text("Measured.").end()));
        agent.send("how does it sound?");
        agent.waitIdle();
        const juce::Array<juce::var> m = messagesOf(script->body(script->postCount() - 1));
        const juce::Array<juce::var> results = arr(m.isEmpty() ? juce::var() : m[m.size() - 1].getProperty("content", {}));
        const std::string report = results.isEmpty() ? std::string() : str(results[0], "content");
        std::printf("measure_output, through the agent:\n%s", report.c_str());
        P.eq("measure.report", has(report, "Rendered offline") && has(report, "level 0.5") ? 1 : 0, 1);
        P.eq("measure.kind", has(report, tp::product::kIsInstrument ? "Note 60" : "Sine 220 Hz") ? 1 : 0, 1);
        P.eq("measure.entry", lastText(agent.chatState(), tp::ChatEntry::Kind::tool) == "measured the sound" ? 1 : 0, 1);
    }

    // ---- BROKEN --------------------------------------------------------------------------------------------------------
    {
        fake::Response cut = fake::Response::ok(fake::Sse().start().text("Half an answ"));   // no message_stop
        cut.retryAfter = 1;
        script->add(cut);
        script->add(fake::Response::ok(fake::Sse().start().text("A whole answer.").end()));
        const int before = script->postCount();
        agent.send("tell me");
        agent.waitIdle();
        const tp::ChatState st = agent.chatState();
        P.eq("broken.asked_again", script->postCount() - before, 2);
        P.eq("broken.answer_once", lastText(st, tp::ChatEntry::Kind::assistant) == "A whole answer." ? 1 : 0, 1);
        bool half = false;
        for (const tp::ChatEntry& e : st.entries)
            half = half || has(e.text, "Half an answ");
        P.eq("broken.half_taken_back", half ? 0 : 1, 1);
        P.eq("broken.note", has(lastText(st, tp::ChatEntry::Kind::note), "broke off") ? 1 : 0, 1);
    }

    // ---- COMPACT -------------------------------------------------------------------------------------------------------
    {
        script->add(fake::Response::ok(fake::Sse()
                                           .start()
                                           .compaction("The user built a level control; the dial is at 1.5.")
                                           .text("Carrying on from the summary.")
                                           .endCompacted(160000, 2500, 3000, 120)));
        agent.send("keep going");
        agent.waitIdle();
        const tp::ChatState st = agent.chatState();
        P.eq("compact.note", has(lastText(st, tp::ChatEntry::Kind::note), "summarized") ? 1 : 0, 1);
        P.eq("compact.usage", has(st.usage, "163K tokens in") && has(st.usage, "summarized") ? 1 : 0, 1);
        script->add(fake::Response::ok(fake::Sse().start().text("Fine.").end()));
        agent.send("next");
        agent.waitIdle();
        const juce::Array<juce::var> m = messagesOf(script->body(script->postCount() - 1));
        P.eq("compact.history_pruned", m.size(), 3);
        const juce::Array<juce::var> answer = arr(m.size() > 1 ? m[1].getProperty("content", {}) : juce::var());
        P.eq("compact.block_echoed", !answer.isEmpty() && str(answer[0], "type") == "compaction"
                                         && str(answer[0], "content") == "The user built a level control; the dial is at 1.5."
                                         ? 1 : 0, 1);
        bool filesAgain = false;
        for (const auto& b : arr(m.size() > 2 ? m[2].getProperty("content", {}) : juce::var()))
            filesAgain = filesAgain || (has(str(b, "text"), "<file name=\"program.fm\">") && has(str(b, "text"), "// a comment"));
        P.eq("compact.files_sent_again", filesAgain ? 1 : 0, 1);
        P.eq("compact.alternates", alternates(m) ? 1 : 0, 1);
    }

    // ---- STOP ----------------------------------------------------------------------------------------------------------
    {
        fake::Response stall = fake::Response::ok(fake::Sse().start().text("Halfway"));
        stall.stall = true;
        script->add(stall);
        agent.send("go");
        const auto t0 = std::chrono::steady_clock::now();
        while (script->streaming.load() == 0 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10))
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        P.eq("stop.streaming", agent.chatState().busy ? 1 : 0, 1);
        P.eq("stop.partial_shown", lastText(agent.chatState(), tp::ChatEntry::Kind::assistant) == "Halfway" ? 1 : 0, 1);
        const auto s0 = std::chrono::steady_clock::now();
        agent.stop();
        agent.waitIdle();
        P.le("stop.prompt_ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count(), 500.0);
        const tp::ChatState s = agent.chatState();
        P.eq("stop.idle", s.busy ? 1 : 0, 0);
        P.eq("stop.note", lastText(s, tp::ChatEntry::Kind::note) == "Stopped." ? 1 : 0, 1);
        const juce::var saved = claude::parseJson(agent.saveState());
        const juce::Array<juce::var> m = arr(saved.getProperty("messages", {}));
        const juce::Array<juce::var> c = arr(m.isEmpty() ? juce::var() : m[m.size() - 1].getProperty("content", {}));
        P.eq("stop.history_text", c.size() == 1 && str(c[0], "text") == "Halfway" ? 1 : 0, 1);
        P.eq("stop.alternates", alternates(m) ? 1 : 0, 1);
    }

    // ---- STATE ---------------------------------------------------------------------------------------------------------
    {
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        tp::Processor second;
        second.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        const tp::ChatState a = agent.chatState(), b = second.chatAgent().chatState();
        bool same = a.entries.size() == b.entries.size();
        for (std::size_t i = 0; same && i < a.entries.size(); ++i)
            same = a.entries[i].kind == b.entries[i].kind && a.entries[i].text == b.entries[i].text;
        P.eq("state.entries", same ? 1 : 0, 1);
        P.ge("state.entry_count", static_cast<double>(b.entries.size()), 20.0);
        const auto history = [](tp::ChatAgent& x) { return claude::toJson(claude::parseJson(x.saveState()).getProperty("messages", {})); };
        P.eq("state.history", history(agent) == history(second.chatAgent()) ? 1 : 0, 1);
    }

    // ---- NEW -----------------------------------------------------------------------------------------------------------
    {
        agent.newChat();
        P.eq("new.entries", static_cast<int64_t>(agent.chatState().entries.size()), 0);
        P.eq("new.history", arr(claude::parseJson(agent.saveState()).getProperty("messages", {})).size(), 0);
    }

    tp::chatstore::setDirectoryForTests(juce::File());
    keys.deleteRecursively();
    return P.finish();
}
