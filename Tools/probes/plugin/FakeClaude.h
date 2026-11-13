// Tools/probes/plugin/FakeClaude.h: a scripted Claude API, for the chat's probes (plugin.chat, gui.chat). Nothing here reaches the network: the agent's transport is replaced by one that plays
// back responses the probe wrote, streamed in small chunks as the Messages API streams them, and records every request.
//
//   fake::Sse        a response's server-sent events: message_start, blocks (thinking with a signature, text,
//                    tool_use with its input JSON streamed in pieces, a fallback marker; the API's own tools: a
//                    server_tool_use, a result block whole, text with citations), message_delta, message_stop
//   fake::Response   one POST's answer: a stream (200), or a status with a JSON error body (and a retry-after), or a
//                    stream that stalls until the agent cancels
//   fake::Script     what the transport plays and what it saw (the bodies, the headers, the hosts, the paths),
//                    shared with the probe while the agent owns the transport
//   fake::OpenAiSse  another provider's answer as an OpenAI-compatible server streams Chat Completions (OpenAI's API
//                    reference, "Streaming"; OpenRouter, Ollama and LM Studio send the same chunks): data: lines of
//                    chat.completion.chunk (a delta's role, content, reasoning or reasoning_content or OpenRouter's
//                    reasoning_details, tool_calls by
//                    index, their arguments in pieces; a finish_reason; a last chunk of usage alone), then [DONE]
//   fake::GeminiSse  Gemini's streamGenerateContent?alt=sse (the Gemini API reference): data: lines of
//                    GenerateContentResponse (candidates[0].content.parts: text, a functionCall whole with its
//                    thoughtSignature; finishReason; usageMetadata), with no end marker
//   (and the models lists: OpenAI's, OpenRouter's, Ollama's, Gemini's, as their references show them)
#pragma once

#include "plugin/ClaudeApi.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fake
{
    namespace claude = playground::claude;

    class Sse
    {
    public:
        Sse& start(const std::string& model = "claude-opus-5-5", int input = 1200, int cacheRead = 0,
                   int cacheWrite = 0)
        {
            return event("message_start",
                         claude::object({ { "type", "message_start" },
                                          { "message",
                                            claude::object({ { "id", "msg_fake" },
                                                             { "type", "message" },
                                                             { "role", "assistant" },
                                                             { "model", juce::String(model) },
                                                             { "content", juce::Array<juce::var>() },
                                                             { "stop_reason", juce::var() },
                                                             { "usage", claude::object({ { "input_tokens", input },
                                                                                         { "cache_read_input_tokens", cacheRead },
                                                                                         { "cache_creation_input_tokens", cacheWrite },
                                                                                         { "output_tokens", 1 } }) } }) } }));
        }

        // message_start with the cache writes split by lifetime (usage.cache_creation's ephemeral_5m_input_tokens and
        // ephemeral_1h_input_tokens), cache_creation_input_tokens their sum, as the API reference has it.
        Sse& startSplit(const std::string& model, int input, int cacheRead, int write5m, int write1h)
        {
            return event("message_start",
                         claude::object({ { "type", "message_start" },
                                          { "message",
                                            claude::object({ { "id", "msg_fake" },
                                                             { "type", "message" },
                                                             { "role", "assistant" },
                                                             { "model", juce::String(model) },
                                                             { "content", juce::Array<juce::var>() },
                                                             { "stop_reason", juce::var() },
                                                             { "usage", claude::object({ { "input_tokens", input },
                                                                                         { "cache_read_input_tokens", cacheRead },
                                                                                         { "cache_creation_input_tokens", write5m + write1h },
                                                                                         { "cache_creation", claude::object({ { "ephemeral_5m_input_tokens", write5m },
                                                                                                                              { "ephemeral_1h_input_tokens", write1h } }) },
                                                                                         { "output_tokens", 1 } }) } }) } }));
        }

        Sse& thinking(const std::string& summary, const std::string& signature)
        {
            const int i = blocks_++;
            blockStart(i, claude::object({ { "type", "thinking" }, { "thinking", "" }, { "signature", "" } }));
            for (const std::string& piece : pieces(summary, 9))
                delta(i, claude::object({ { "type", "thinking_delta" }, { "thinking", juce::String::fromUTF8(piece.c_str()) } }));
            delta(i, claude::object({ { "type", "signature_delta" }, { "signature", juce::String(signature) } }));
            return blockStop(i);
        }

        Sse& text(const std::string& t)
        {
            const int i = blocks_++;
            blockStart(i, claude::object({ { "type", "text" }, { "text", "" } }));
            for (const std::string& piece : pieces(t, 7))
                delta(i, claude::object({ { "type", "text_delta" }, { "text", juce::String::fromUTF8(piece.c_str()) } }));
            return blockStop(i);
        }

        // `inputJson` is streamed as it is, valid or not.
        Sse& toolUse(const std::string& id, const std::string& name, const std::string& inputJson)
        {
            const int i = blocks_++;
            blockStart(i, claude::object({ { "type", "tool_use" },
                                           { "id", juce::String(id) },
                                           { "name", juce::String(name) },
                                           { "input", juce::var(new juce::DynamicObject()) } }));
            for (const std::string& piece : pieces(inputJson, 5))
                delta(i, claude::object({ { "type", "input_json_delta" },
                                          { "partial_json", juce::String::fromUTF8(piece.c_str()) } }));
            return blockStop(i);
        }

        // A tool_use block whose input is `input` (a JSON object), as JSON.
        Sse& tool(const std::string& id, const std::string& name, const juce::var& input)
        {
            return toolUse(id, name, claude::toJson(input));
        }

        // A server tool's call (web_search, web_fetch), its input streamed as a tool_use's.
        Sse& serverTool(const std::string& id, const std::string& name, const juce::var& input)
        {
            const int i = blocks_++;
            blockStart(i, claude::object({ { "type", "server_tool_use" }, { "id", juce::String(id) }, { "name", juce::String(name) } }));
            for (const std::string& piece : pieces(claude::toJson(input), 6))
                delta(i, claude::object({ { "type", "input_json_delta" },
                                          { "partial_json", juce::String::fromUTF8(piece.c_str()) } }));
            return blockStop(i);
        }

        // A block that arrives whole in its content_block_start (a web_search_tool_result, ...).
        Sse& whole(const juce::var& block)
        {
            const int i = blocks_++;
            blockStart(i, block);
            return blockStop(i);
        }

        // A text block and its citations, each a citations_delta after the text.
        Sse& cited(const std::string& t, const juce::Array<juce::var>& citations)
        {
            const int i = blocks_++;
            blockStart(i, claude::object({ { "type", "text" }, { "text", "" } }));
            for (const std::string& piece : pieces(t, 7))
                delta(i, claude::object({ { "type", "text_delta" }, { "text", juce::String::fromUTF8(piece.c_str()) } }));
            for (const juce::var& c : citations)
                delta(i, claude::object({ { "type", "citations_delta" }, { "citation", c } }));
            return blockStop(i);
        }

        // The usage's server_tool_use, for the next end().
        Sse& serverUse(int searches, int fetches)
        {
            searches_ = searches;
            fetches_ = fetches;
            return *this;
        }

        Sse& fallback(const std::string& from, const std::string& to)
        {
            const int i = blocks_++;
            blockStart(i, claude::object({ { "type", "fallback" },
                                           { "from", claude::object({ { "model", juce::String(from) } }) },
                                           { "to", claude::object({ { "model", juce::String(to) } }) } }));
            return blockStop(i);
        }

        // A compaction block: its summary arrives whole, in one compaction_delta (the compaction docs).
        Sse& compaction(const std::string& summary)
        {
            const int i = blocks_++;
            blockStart(i, claude::object({ { "type", "compaction" } }));
            delta(i, claude::object({ { "type", "compaction_delta" }, { "content", juce::String::fromUTF8(summary.c_str()) } }));
            return blockStop(i);
        }

        Sse& ping() { return event("ping", claude::object({ { "type", "ping" } })); }

        // `speed`, when given, is the usage's ("fast" or "standard"), as a fast-mode request's response says it.
        Sse& end(const std::string& stopReason = "end_turn", int output = 321, const std::string& speed = "")
        {
            juce::var usage = claude::object({ { "output_tokens", output } });
            if (!speed.empty())
                usage.getDynamicObject()->setProperty("speed", juce::String(speed));
            if (searches_ > 0 || fetches_ > 0)
                usage.getDynamicObject()->setProperty("server_tool_use", claude::object({ { "web_search_requests", searches_ },
                                                                                          { "web_fetch_requests", fetches_ } }));
            event("message_delta", claude::object({ { "type", "message_delta" },
                                                    { "delta", claude::object({ { "stop_reason", juce::String(stopReason) },
                                                                                { "stop_sequence", juce::var() } }) },
                                                    { "usage", usage } }));
            return event("message_stop", claude::object({ { "type", "message_stop" } }));
        }

        // The end of a request that compacted: its usage lists the iterations (a compaction's, then the answer's).
        // "tool_use" for a summary in the middle of a turn, whose answer asks for tools.
        Sse& endCompacted(int compactIn, int compactOut, int in, int out, const std::string& stopReason = "end_turn")
        {
            juce::Array<juce::var> iterations;
            iterations.add(claude::object({ { "type", "compaction" }, { "input_tokens", compactIn }, { "output_tokens", compactOut } }));
            iterations.add(claude::object({ { "type", "message" }, { "input_tokens", in }, { "output_tokens", out } }));
            event("message_delta", claude::object({ { "type", "message_delta" },
                                                    { "delta", claude::object({ { "stop_reason", juce::String(stopReason) },
                                                                                { "stop_sequence", juce::var() } }) },
                                                    { "usage", claude::object({ { "input_tokens", in }, { "output_tokens", out },
                                                                                { "iterations", iterations } }) } }));
            return event("message_stop", claude::object({ { "type", "message_stop" } }));
        }

        Sse& error(const std::string& type, const std::string& message)
        {
            return event("error", claude::object({ { "type", "error" },
                                                   { "error", claude::object({ { "type", juce::String(type) },
                                                                               { "message", juce::String(message) } }) } }));
        }

        const std::string& str() const noexcept { return text_; }

    private:
        Sse& event(const char* type, const juce::var& data)
        {
            text_ += std::string("event: ") + type + "\r\ndata: " + claude::toJson(data) + "\r\n\r\n";
            return *this;
        }
        void blockStart(int i, const juce::var& block)
        {
            event("content_block_start",
                  claude::object({ { "type", "content_block_start" }, { "index", i }, { "content_block", block } }));
        }
        void delta(int i, const juce::var& d)
        {
            event("content_block_delta", claude::object({ { "type", "content_block_delta" }, { "index", i }, { "delta", d } }));
        }
        Sse& blockStop(int i)
        {
            return event("content_block_stop", claude::object({ { "type", "content_block_stop" }, { "index", i } }));
        }
        static std::vector<std::string> pieces(const std::string& s, std::size_t n)
        {
            std::vector<std::string> out;
            for (std::size_t p = 0; p < s.size(); p += n)
                out.push_back(s.substr(p, n));
            return out;
        }

        std::string text_;
        int blocks_ = 0;
        int searches_ = 0, fetches_ = 0;
    };

    struct Response
    {
        int status = 200;
        std::string stream;                  // a 200's events
        std::string body;                    // another status's body
        int retryAfter = -1;
        bool stall = false;                  // after the stream, the connection hangs until the agent cancels
        std::size_t chunk = 11;              // the stream arrives in pieces of this many bytes
        std::string location;                // a redirect's (3xx): where it points (the transport follows none)

        static Response ok(const Sse& sse) { return { 200, sse.str(), {}, -1, false, 11, {} }; }
        static Response failure(int status, const std::string& type, const std::string& message, int retryAfter = -1)
        {
            const std::string body = claude::toJson(claude::object(
                { { "type", "error" },
                  { "error", claude::object({ { "type", juce::String(type) }, { "message", juce::String(message) } }) } }));
            return { status, {}, body, retryAfter, false, 11, {} };
        }
        static Response redirect(int status, const std::string& to) { return { status, {}, {}, -1, false, 11, to }; }
    };

    struct Script
    {
        std::mutex mutex;
        std::deque<Response> posts;          // played in order; past the end, a short text answer
        int modelsStatus = 200;
        std::string models;                  // GET /v1/models's body
        std::map<std::string, std::pair<int, std::string>> gets;   // another GET's (status, body), by its path
        std::vector<std::string> bodies;     // every POST's body
        std::vector<claude::Headers> headers;
        std::vector<std::string> paths;      // every request's path, in order ("POST /v1/messages")
        std::vector<std::string> hosts;      // and its host, in the same order
        std::atomic<int> streaming{ 0 };     // a stalled stream is waiting
        std::atomic<bool> holdGets{ false };  // while set, a GET waits (a slow models list) until it is cleared or cancelled
        std::atomic<int> heldGets{ 0 };      // the GETs waiting so

        void add(Response r)
        {
            const std::lock_guard<std::mutex> lock(mutex);
            posts.push_back(std::move(r));
        }
        int postCount()
        {
            const std::lock_guard<std::mutex> lock(mutex);
            return static_cast<int>(bodies.size());
        }
        std::string body(int i)
        {
            const std::lock_guard<std::mutex> lock(mutex);
            return i >= 0 && i < static_cast<int>(bodies.size()) ? bodies[static_cast<std::size_t>(i)] : std::string();
        }
        std::string header(int i, const std::string& name)
        {
            const std::lock_guard<std::mutex> lock(mutex);
            if (i < 0 || i >= static_cast<int>(headers.size()))
                return {};
            for (const auto& [k, v] : headers[static_cast<std::size_t>(i)])
                if (k == name)
                    return v;
            return {};
        }
        // The headers of the last request to `path` ("POST /v1/messages"), and its host.
        std::string lastHeader(const std::string& path, const std::string& name)
        {
            const int i = last(path);
            return i < 0 ? std::string() : header(i, name);
        }
        bool lastHas(const std::string& path, const std::string& name)
        {
            const std::lock_guard<std::mutex> lock(mutex);
            const int i = lastLocked(path);
            if (i < 0)
                return false;
            for (const auto& [k, v] : headers[static_cast<std::size_t>(i)])
                if (k == name)
                    return true;
            return false;
        }
        std::string lastHost(const std::string& path)
        {
            const std::lock_guard<std::mutex> lock(mutex);
            const int i = lastLocked(path);
            return i < 0 ? std::string() : hosts[static_cast<std::size_t>(i)];
        }
        int count(const std::string& path)
        {
            const std::lock_guard<std::mutex> lock(mutex);
            int n = 0;
            for (const std::string& p : paths)
                n += p == path ? 1 : 0;
            return n;
        }
        int last(const std::string& path)
        {
            const std::lock_guard<std::mutex> lock(mutex);
            return lastLocked(path);
        }

    private:
        // Requests are numbered in `paths` order; `headers` follows the same order (every request records both).
        int lastLocked(const std::string& path) const
        {
            for (int i = static_cast<int>(paths.size()); --i >= 0;)
                if (paths[static_cast<std::size_t>(i)] == path)
                    return i;
            return -1;
        }
    };

    class Transport final : public claude::Transport
    {
    public:
        explicit Transport(std::shared_ptr<Script> script) : script_(std::move(script)) {}

        claude::HttpResult post(const std::string& host, const std::string& path, const std::string& body,
                                const claude::Headers& headers, const std::function<bool(std::string_view)>& onChunk,
                                const std::atomic<bool>& cancel) override
        {
            Response r;
            {
                const std::lock_guard<std::mutex> lock(script_->mutex);
                script_->headers.push_back(headers);
                script_->paths.push_back("POST " + path);
                script_->hosts.push_back(host);
                script_->bodies.push_back(body);
                if (script_->posts.empty())
                    r = Response::ok(Sse().start().text("(nothing scripted)").end());
                else
                {
                    r = std::move(script_->posts.front());
                    script_->posts.pop_front();
                }
            }
            if (r.status != 200)
                return { r.status, {}, r.body, r.retryAfter, r.location };
            for (std::size_t p = 0; p < r.stream.size(); p += r.chunk)
            {
                if (cancel.load())
                    return { 0, "cancelled", {}, -1, {} };
                if (!onChunk(std::string_view(r.stream).substr(p, r.chunk)))
                    return { 200, {}, {}, -1, {} };
            }
            if (r.stall)
            {
                ++script_->streaming;
                while (!cancel.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                --script_->streaming;
                return { 0, "cancelled", {}, -1, {} };
            }
            return { 200, {}, {}, r.retryAfter, {} };   // a stream cut short (no message_stop) may ask for a quick retry
        }

        claude::HttpResult get(const std::string& host, const std::string& path, const claude::Headers& headers,
                               const std::atomic<bool>& cancel) override
        {
            if (script_->holdGets.load())
            {
                ++script_->heldGets;
                while (script_->holdGets.load() && !cancel.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                --script_->heldGets;
            }
            const std::lock_guard<std::mutex> lock(script_->mutex);
            script_->paths.push_back("GET " + path);
            script_->headers.push_back(headers);
            script_->hosts.push_back(host);
            if (cancel.load())
                return { 0, "cancelled", {}, -1, {} };   // as the real one: a set flag ends it at once
            if (const auto it = script_->gets.find(path); it != script_->gets.end())
                return { it->second.first, {}, it->second.second, -1, {} };
            return { script_->modelsStatus, {}, script_->models, -1, {} };
        }

    private:
        std::shared_ptr<Script> script_;
    };

    // GET /v1/models's body: the models with their capabilities (a model with adaptive thinking takes `efforts`).
    struct ModelInfo
    {
        std::string id, name;
        bool adaptive = true;
        std::vector<std::string> efforts;
        int maxTokens = 128000;
        bool compaction = true;
    };

    inline std::string modelsBody(const std::vector<ModelInfo>& models)
    {
        juce::Array<juce::var> data;
        for (const ModelInfo& m : models)
        {
            juce::var effort(new juce::DynamicObject());
            for (const char* e : { "low", "medium", "high", "xhigh", "max" })
            {
                bool has = false;
                for (const std::string& x : m.efforts)
                    has = has || x == e;
                effort.getDynamicObject()->setProperty(e, claude::object({ { "supported", has } }));
            }
            effort.getDynamicObject()->setProperty("supported", !m.efforts.empty());
            const juce::var caps = claude::object(
                { { "thinking", claude::object({ { "supported", true },
                                                 { "types", claude::object({ { "adaptive", claude::object({ { "supported", m.adaptive } }) },
                                                                             { "enabled", claude::object({ { "supported", !m.adaptive } }) } }) } }) },
                  { "effort", effort },
                  { "context_management",
                    claude::object({ { "compact_20260112", claude::object({ { "supported", m.compaction } }) } }) } });
            data.add(claude::object({ { "type", "model" },
                                      { "id", juce::String(m.id) },
                                      { "display_name", juce::String(m.name) },
                                      { "max_input_tokens", 1000000 },
                                      { "max_tokens", m.maxTokens },
                                      { "capabilities", caps } }));
        }
        return claude::toJson(claude::object({ { "data", data }, { "has_more", false } }));
    }

    // A key of the right shape for plausibleKey, which no server knows (and which no secret scanner takes for a key).
    inline std::string fakeKey() { return "sk-ant-probe-NotARealKey_0123456789abcdefghijklmnop-QRSTUV"; }
    // Other providers' keys, as plausible and as unreal.
    inline std::string fakeOpenRouterKey() { return "sk-or-v1-probe-NotARealKey-0123456789abcdef"; }
    inline std::string fakeOpenAiKey() { return "sk-proj-probe-NotARealKey-0123456789abcdef"; }
    inline std::string fakeGeminiKey() { return "AIzaProbeNotARealKey_0123456789abcdefg"; }

    // An OpenAI-compatible server's streamed Chat Completion: each call a chunk (a data: line).
    class OpenAiSse
    {
    public:
        explicit OpenAiSse(std::string model = "gpt-probe") : model_(std::move(model)) {}

        // A delta of text, cut in pieces as a server streams it; reasoning (OpenRouter's and Ollama's "reasoning",
        // DeepSeek's and LM Studio's "reasoning_content").
        OpenAiSse& text(const std::string& t)
        {
            for (std::size_t p = 0; p < t.size(); p += 6)
                chunk(claude::object({ { "content", juce::String::fromUTF8(t.substr(p, 6).c_str()) } }));
            return *this;
        }
        OpenAiSse& reasoning(const std::string& t, const char* key = "reasoning")
        {
            for (std::size_t p = 0; p < t.size(); p += 8)
                chunk(claude::object({ { key, juce::String::fromUTF8(t.substr(p, 8).c_str()) } }));
            return *this;
        }
        // OpenRouter's reasoning_details, a piece a chunk: { type, text (a summary's: summary), format, index }, of type
        // "reasoning.text" or "reasoning.summary"; `alsoReasoning`: the same piece as "reasoning" in the same chunk, as
        // OpenRouter sends both for most models.
        OpenAiSse& reasoningDetails(const std::string& t, const std::string& type = "reasoning.text", bool alsoReasoning = false)
        {
            for (std::size_t p = 0; p < t.size(); p += 8)
            {
                const juce::String piece = juce::String::fromUTF8(t.substr(p, 8).c_str());
                juce::var detail = claude::object({ { "type", juce::String(type) }, { "format", "unknown" }, { "index", 0 } });
                detail.getDynamicObject()->setProperty(type == "reasoning.summary" ? "summary" : "text", piece);
                juce::var delta = claude::object({ { "reasoning_details", juce::Array<juce::var>{ detail } } });
                if (alsoReasoning)
                    delta.getDynamicObject()->setProperty("reasoning", piece);
                chunk(delta);
            }
            return *this;
        }
        // A tool call by its index: the first chunk its id and name, then its arguments in pieces (OpenAI's way);
        // `whole`: the call in one chunk, without an index (some local servers' way).
        OpenAiSse& call(int index, const std::string& id, const std::string& name, const std::string& args, bool whole = false)
        {
            if (whole)
            {
                juce::var fn = claude::object({ { "name", juce::String(name) }, { "arguments", juce::String::fromUTF8(args.c_str()) } });
                juce::var tc = claude::object({ { "id", juce::String(id) }, { "type", "function" }, { "function", fn } });
                return chunk(claude::object({ { "tool_calls", juce::Array<juce::var>{ tc } } }));
            }
            juce::var first = claude::object({ { "index", index },
                                               { "id", juce::String(id) },
                                               { "type", "function" },
                                               { "function", claude::object({ { "name", juce::String(name) }, { "arguments", "" } }) } });
            chunk(claude::object({ { "tool_calls", juce::Array<juce::var>{ first } } }));
            for (std::size_t p = 0; p < args.size(); p += 7)
            {
                juce::var part = claude::object({ { "index", index },
                                                  { "function", claude::object({ { "arguments", juce::String::fromUTF8(args.substr(p, 7).c_str()) } }) } });
                chunk(claude::object({ { "tool_calls", juce::Array<juce::var>{ part } } }));
            }
            return *this;
        }
        OpenAiSse& finish(const std::string& reason)
        {
            juce::var choice = claude::object({ { "index", 0 }, { "delta", juce::var(new juce::DynamicObject()) }, { "finish_reason", juce::String(reason) } });
            return raw(claude::object({ { "id", "chatcmpl-probe" }, { "object", "chat.completion.chunk" }, { "model", juce::String(model_) },
                                        { "choices", juce::Array<juce::var>{ choice } } }));
        }
        // The usage chunk (stream_options.include_usage): no choices.
        OpenAiSse& usage(int prompt, int completion, int cached = 0)
        {
            juce::var u = claude::object({ { "prompt_tokens", prompt }, { "completion_tokens", completion }, { "total_tokens", prompt + completion },
                                           { "prompt_tokens_details", claude::object({ { "cached_tokens", cached } }) } });
            return raw(claude::object({ { "id", "chatcmpl-probe" }, { "object", "chat.completion.chunk" }, { "model", juce::String(model_) },
                                        { "choices", juce::Array<juce::var>() }, { "usage", u } }));
        }
        // OpenRouter's usage chunk with its usage accounting's cost (USD).
        OpenAiSse& usageCost(int prompt, int completion, int cached, double cost)
        {
            juce::var u = claude::object({ { "prompt_tokens", prompt }, { "completion_tokens", completion }, { "total_tokens", prompt + completion },
                                           { "prompt_tokens_details", claude::object({ { "cached_tokens", cached } }) },
                                           { "cost", cost } });
            return raw(claude::object({ { "id", "chatcmpl-probe" }, { "object", "chat.completion.chunk" }, { "model", juce::String(model_) },
                                        { "choices", juce::Array<juce::var>() }, { "usage", u } }));
        }
        OpenAiSse& error(const std::string& message)
        {
            return raw(claude::object({ { "error", claude::object({ { "message", juce::String(message) }, { "code", 502 } }) } }));
        }
        OpenAiSse& done()
        {
            text_ += "data: [DONE]\n\n";
            return *this;
        }
        const std::string& str() const noexcept { return text_; }

    private:
        OpenAiSse& chunk(const juce::var& delta)
        {
            if (first_)
            {
                delta.getDynamicObject()->setProperty("role", "assistant");
                first_ = false;
            }
            juce::var choice = claude::object({ { "index", 0 }, { "delta", delta }, { "finish_reason", juce::var() } });
            return raw(claude::object({ { "id", "chatcmpl-probe" }, { "object", "chat.completion.chunk" }, { "created", 1762500000 },
                                        { "model", juce::String(model_) }, { "choices", juce::Array<juce::var>{ choice } } }));
        }
        OpenAiSse& raw(const juce::var& v)
        {
            text_ += "data: " + claude::toJson(v) + "\n\n";
            return *this;
        }
        std::string model_, text_;
        bool first_ = true;
    };

    // Gemini's streamed answer: each call a GenerateContentResponse (a data: line), its usage on every one.
    class GeminiSse
    {
    public:
        explicit GeminiSse(std::string model = "gemini-probe-pro") : model_(std::move(model)) {}

        GeminiSse& text(const std::string& t, bool thought = false)
        {
            juce::var part = claude::object({ { "text", juce::String::fromUTF8(t.c_str()) } });
            if (thought)
                part.getDynamicObject()->setProperty("thought", true);
            return parts({ part }, {});
        }
        // A function call arrives whole, its thought signature on its part (Gemini 3's).
        GeminiSse& call(const std::string& name, const juce::var& args, const std::string& signature = {})
        {
            juce::var part = claude::object({ { "functionCall", claude::object({ { "name", juce::String(name) }, { "args", args } }) } });
            if (!signature.empty())
                part.getDynamicObject()->setProperty("thoughtSignature", juce::String(signature));
            return parts({ part }, {});
        }
        GeminiSse& finish(const std::string& reason, int prompt = 900, int out = 42)
        {
            prompt_ = prompt;
            out_ = out;
            return parts({}, reason);
        }
        const std::string& str() const noexcept { return text_; }

    private:
        GeminiSse& parts(const juce::Array<juce::var>& ps, const std::string& reason)
        {
            juce::var cand = claude::object({ { "content", claude::object({ { "role", "model" }, { "parts", ps } }) }, { "index", 0 } });
            if (!reason.empty())
                cand.getDynamicObject()->setProperty("finishReason", juce::String(reason));
            juce::var v = claude::object({ { "candidates", juce::Array<juce::var>{ cand } },
                                           { "usageMetadata", claude::object({ { "promptTokenCount", prompt_ },
                                                                               { "candidatesTokenCount", out_ },
                                                                               { "totalTokenCount", prompt_ + out_ } }) },
                                           { "modelVersion", juce::String(model_) } });
            text_ += "data: " + claude::toJson(v) + "\r\n\r\n";
            return *this;
        }
        std::string model_, text_;
        int prompt_ = 900, out_ = 1;
    };

    // The models lists. OpenAI's: { object: list, data: [{ id, object: model, created, owned_by }] }; OpenRouter's adds
    // name, context_length, architecture.input_modalities, supported_parameters and, for a model that reasons, a
    // reasoning object (supported_efforts, default_effort, default_enabled, supports_max_tokens, mandatory); Ollama's and
    // LM Studio's are OpenAI's. Gemini's: { models: [{ name: models/<id>, displayName, inputTokenLimit,
    // outputTokenLimit, supportedGenerationMethods, thinking }] }.
    struct ListedModel
    {
        std::string id, name;
        int64_t created = 0;
        int context = 0;
        bool tools = true, images = true;
        // OpenRouter's reasoning object (none while `efforts` is empty): its levels, its default, whether it reasons by
        // default and whether reasoning is mandatory. Gemini's "thinking": -1 not said, 0 false, 1 true.
        std::vector<std::string> efforts = {};
        std::string defaultEffort = {};
        bool defaultEnabled = true, mandatory = false;
        int thinking = -1;
        // OpenRouter's pricing (USD a token, as strings), written only when prompt is given.
        std::string pricePrompt = {}, priceCompletion = {}, priceCacheRead = {};
    };
    inline std::string openAiModels(const std::vector<ListedModel>& models, bool openRouter = false)
    {
        juce::Array<juce::var> data;
        for (const ListedModel& m : models)
        {
            juce::var v = claude::object({ { "id", juce::String(m.id) }, { "object", "model" }, { "created", static_cast<juce::int64>(m.created) },
                                           { "owned_by", "probe" } });
            if (openRouter)
            {
                auto* o = v.getDynamicObject();
                o->setProperty("name", juce::String(m.name));
                o->setProperty("context_length", m.context);
                juce::Array<juce::var> in{ "text" };
                if (m.images)
                    in.add("image");
                o->setProperty("architecture", claude::object({ { "input_modalities", in } }));
                juce::Array<juce::var> params{ "max_tokens", "temperature" };
                if (m.tools)
                    params.add("tools");
                if (!m.efforts.empty())
                {
                    params.add("reasoning");
                    params.add("include_reasoning");
                    juce::Array<juce::var> efforts;
                    for (const std::string& e : m.efforts)
                        efforts.add(juce::String(e));
                    o->setProperty("reasoning", claude::object({ { "supported_efforts", efforts },
                                                                 { "default_effort", juce::String(m.defaultEffort) },
                                                                 { "default_enabled", m.defaultEnabled },
                                                                 { "supports_max_tokens", true },
                                                                 { "mandatory", m.mandatory } }));
                }
                o->setProperty("supported_parameters", params);
                if (!m.pricePrompt.empty())
                {
                    juce::var pricing = claude::object({ { "prompt", juce::String(m.pricePrompt) },
                                                         { "completion", juce::String(m.priceCompletion) } });
                    if (!m.priceCacheRead.empty())
                        pricing.getDynamicObject()->setProperty("input_cache_read", juce::String(m.priceCacheRead));
                    o->setProperty("pricing", pricing);
                }
            }
            data.add(v);
        }
        return claude::toJson(claude::object({ { "object", "list" }, { "data", data } }));
    }
    inline std::string geminiModels(const std::vector<ListedModel>& models, const std::vector<std::string>& methods = { "generateContent", "countTokens" })
    {
        juce::Array<juce::var> list;
        for (const ListedModel& m : models)
        {
            juce::Array<juce::var> ms;
            for (const std::string& x : methods)
                ms.add(juce::String(x));
            juce::var v = claude::object({ { "name", juce::String("models/" + m.id) }, { "displayName", juce::String(m.name) },
                                           { "inputTokenLimit", m.context }, { "outputTokenLimit", 65536 },
                                           { "supportedGenerationMethods", m.id.find("embedding") != std::string::npos
                                                                               ? juce::var(juce::Array<juce::var>{ "embedContent" }) : juce::var(ms) } });
            if (m.thinking >= 0)
                v.getDynamicObject()->setProperty("thinking", m.thinking > 0);
            list.add(v);
        }
        return claude::toJson(claude::object({ { "models", list } }));
    }
}
