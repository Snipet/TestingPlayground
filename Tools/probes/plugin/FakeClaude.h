// Tools/probes/plugin/FakeClaude.h: a scripted Claude API, and a scripted TestingPlayground server, for the chat's
// probes (plugin.chat, gui.chat). Nothing here reaches the network: the agent's transport is replaced by one that plays
// back responses the probe wrote, streamed in small chunks as the Messages API streams them, and records every request.
//
//   fake::Sse        a response's server-sent events: message_start, blocks (thinking with a signature, text,
//                    tool_use with its input JSON streamed in pieces, a fallback marker; the API's own tools: a
//                    server_tool_use, a result block whole, text with citations), message_delta, message_stop
//   fake::Response   one POST's answer: a stream (200), or a status with a JSON error body (and a retry-after), or a
//                    stream that stalls until the agent cancels
//   fake::Script     what the transport plays and what it saw (the bodies, the headers, the hosts, the paths),
//                    shared with the probe while the agent owns the transport. The server's own endpoints answer from
//                    it too: POST /auth/login (the `logins` queue, else a sign-in as "alice" with fakeToken()),
//                    POST /auth/logout, GET /account (accountStatus, accountBody); /v1/messages and /v1/models answer
//                    the same on either host, which the probe checks in `hosts`
#pragma once

#include "plugin/ClaudeApi.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fake
{
    namespace claude = testingplaygroundsynth::claude;

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

        static Response ok(const Sse& sse) { return { 200, sse.str(), {}, -1, false, 11 }; }
        static Response failure(int status, const std::string& type, const std::string& message, int retryAfter = -1)
        {
            const std::string body = claude::toJson(claude::object(
                { { "type", "error" },
                  { "error", claude::object({ { "type", juce::String(type) }, { "message", juce::String(message) } }) } }));
            return { status, {}, body, retryAfter, false, 11 };
        }
    };

    struct Script
    {
        std::mutex mutex;
        std::deque<Response> posts;          // played in order; past the end, a short text answer
        int modelsStatus = 200;
        std::string models;                  // GET /v1/models's body
        std::vector<std::string> bodies;     // every POST's body
        std::vector<claude::Headers> headers;
        std::vector<std::string> paths;      // every request's path, in order ("POST /v1/messages")
        std::vector<std::string> hosts;      // and its host, in the same order
        std::deque<Response> logins;         // POST /auth/login's answers; past the end, a sign-in as "alice"
        int accountStatus = 200;
        std::string accountBody;             // GET /account's ("" a default: alice, $4.50 left)
        std::vector<std::string> loginBodies;   // what each sign-in sent (the probe checks the password went)
        std::atomic<int> streaming{ 0 };     // a stalled stream is waiting

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

    // A sign-in token of the server's shape ("tpb_" and 43 base64url characters), which no server issued.
    inline std::string fakeToken() { return "tpb_probe-NotARealToken_0123456789abcdefghij"; }

    // A password for the probes' sign-ins (sent only to the fake).
    inline std::string fakePassword() { return "probe-Password-0123456789"; }

    // POST /auth/login's answer, and GET /account's.
    inline std::string loginBody(const std::string& username, double balance, const std::string& token = fakeToken())
    {
        return claude::toJson(claude::object(
            { { "token", juce::String(token) },
              { "expires_at", "2027-01-24T00:00:00.000Z" },
              { "user", claude::object({ { "username", juce::String(username) }, { "balance_usd", balance } }) } }));
    }

    inline std::string accountBody(const std::string& username, double balance)
    {
        return claude::toJson(claude::object(
            { { "username", juce::String(username) },
              { "balance_usd", balance },
              { "month", claude::object({ { "requests", 3 }, { "cost_usd", 0.5 }, { "input_tokens", 12000 },
                                          { "output_tokens", 900 } }) },
              { "session", claude::object({ { "client", "probe" },
                                            { "created_at", "2026-10-26T00:00:00.000Z" },
                                            { "expires_at", "2027-01-24T00:00:00.000Z" } }) } }));
    }

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
                if (path == "/auth/login")
                {
                    script_->loginBodies.push_back(body);
                    if (script_->logins.empty())
                        return { 200, {}, loginBody("alice", 5.0), -1 };
                    Response login = std::move(script_->logins.front());
                    script_->logins.pop_front();
                    return { login.status, {}, login.body, login.retryAfter };
                }
                if (path == "/auth/logout")
                    return { 200, {}, "{\"ok\":true}", -1 };
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
                return { r.status, {}, r.body, r.retryAfter };
            for (std::size_t p = 0; p < r.stream.size(); p += r.chunk)
            {
                if (cancel.load())
                    return { 0, "cancelled", {}, -1 };
                if (!onChunk(std::string_view(r.stream).substr(p, r.chunk)))
                    return { 200, {}, {}, -1 };
            }
            if (r.stall)
            {
                ++script_->streaming;
                while (!cancel.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                --script_->streaming;
                return { 0, "cancelled", {}, -1 };
            }
            return { 200, {}, {}, r.retryAfter };   // a stream cut short (no message_stop) may ask for a quick retry
        }

        claude::HttpResult get(const std::string& host, const std::string& path, const claude::Headers& headers,
                               const std::atomic<bool>&) override
        {
            const std::lock_guard<std::mutex> lock(script_->mutex);
            script_->paths.push_back("GET " + path);
            script_->headers.push_back(headers);
            script_->hosts.push_back(host);
            if (path == "/account")
                return { script_->accountStatus, {},
                         script_->accountBody.empty() ? accountBody("alice", 4.5) : script_->accountBody, -1 };
            return { script_->modelsStatus, {}, script_->models, -1 };
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
}
