// Source/plugin/ClaudeApi.h: the Claude Messages API over HTTPS (raw HTTP: there is no Anthropic SDK for C++), for the
// chat (ChatAgent.h).
//
//   Transport       POST and GET against a host: https://api.anthropic.com with the user's own key, or the
//                   TestingPlayground server (serverHost()) once the user has signed in, which passes the same requests
//                   on to Anthropic on its own key and charges them to the user's credit. httpsTransport() is JUCE's
//                   WebInputStream; the probes give the agent a scripted one. A POST's body streams to a callback as
//                   it arrives.
//   SseParser       Server-Sent Events: the stream cut into (event, data) pairs, however the chunks fall.
//   MessageBuilder  a streamed message (message_start, content_block_start/delta/stop, message_delta, error) put back
//                   together: its content blocks exactly as the API sent them (thinking blocks with their signatures,
//                   tool_use blocks with their input parsed from the streamed JSON, a compaction block's summary), the
//                   stop reason and the usage (summed over the request's iterations when the API lists them), and
//                   callbacks as text, thinking, tool calls and compactions arrive, for the chat's live view.
//   apiError        an error response's message ("invalid x-api-key"), from its JSON body; serverError the same for a
//                   response from the server, whose own errors are written for the user ("Your credit has run out").
//
// Requests are built by ChatAgent; the API version is kApiVersion (2026-10-21: no temperature, top_p or top_k).
#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace playground::claude
{
    inline constexpr const char* kHost = "https://api.anthropic.com";
    inline constexpr const char* kApiVersion = "2026-10-21";

    // The TestingPlayground server (a Cloudflare Worker): sign-in (POST /auth/login, /auth/logout), the account
    // (GET /account) and the Claude API's /v1/messages and /v1/models, as Anthropic's, with "Authorization: Bearer".
    // The Worker as deployed (the TestingPlaygroundBackend repo: `npm run deploy`).
    inline constexpr const char* kServerHost = "https://playground-backend.example.workers.dev";

    // The server the chat signs in to: kServerHost, or <ENV_PREFIX>SERVER_URL when it is set to an https:// address
    // or to http:// on this Mac (http://localhost:8787 for `wrangler dev`); anything else in it is ignored, so a
    // sign-in never goes over plain HTTP to another machine. No trailing slash.
    std::string serverHost();

    // The host part of an address ("api.anthropic.com"), for messages.
    std::string hostName(const std::string& host);

    using Headers = std::vector<std::pair<std::string, std::string>>;

    struct HttpResult
    {
        int status = 0;                       // 0: no response (error says why)
        std::string error;                    // a connection failure
        std::string body;                     // the body when it was not streamed (a GET, or an error status)
        int retryAfterSeconds = -1;           // the retry-after header, when there is one
    };

    class Transport
    {
    public:
        virtual ~Transport() = default;

        // POST `body` (JSON) to `host` + `path` (host: kHost or serverHost()). A 200's body goes to onChunk as it
        // arrives (false from it stops reading); any other status's body, or a 200's when onChunk is empty, is
        // returned whole. `cancel` set from another thread ends the call soon.
        virtual HttpResult post(const std::string& host, const std::string& path, const std::string& body,
                                const Headers& headers, const std::function<bool(std::string_view)>& onChunk,
                                const std::atomic<bool>& cancel) = 0;
        virtual HttpResult get(const std::string& host, const std::string& path, const Headers& headers,
                               const std::atomic<bool>& cancel) = 0;
    };

    std::unique_ptr<Transport> httpsTransport();

    class SseParser
    {
    public:
        // Feeds a chunk; calls onEvent(event, data) for each complete event (a blank line ends one).
        void feed(std::string_view chunk, const std::function<void(const std::string&, const std::string&)>& onEvent);

    private:
        std::string line_, event_, data_;
        bool cr_ = false;
    };

    struct Usage
    {
        int64_t input = 0, output = 0, cacheRead = 0, cacheWrite = 0;
        int compactions = 0;                  // compaction iterations in the request (usage.iterations)
        int webSearches = 0, webFetches = 0;  // usage.server_tool_use: searches cost $10 a thousand
        std::string speed;                    // the speed it ran at, "fast" or "standard" (usage.speed); "" unsaid
    };

    class MessageBuilder
    {
    public:
        struct Callbacks
        {
            std::function<void(std::string_view text)> text;           // a text delta
            std::function<void(std::string_view text)> thinking;       // a thinking (summary) delta
            std::function<void(const std::string& name)> toolStart;    // a tool_use block begins
            std::function<void(const std::string& json)> toolInput;    // its input so far (partial JSON)
            // A server tool's call (web_search, web_fetch, code_execution) begins, and its input once complete; a
            // server tool's result block (web_search_tool_result, ...) as it came; a citation of a text block.
            std::function<void(const std::string& name)> serverTool;
            std::function<void(const std::string& name, const juce::var& input)> serverInput;
            std::function<void(const juce::var& block)> serverResult;
            std::function<void(const juce::var& citation)> citation;
            std::function<void(const std::string& from, const std::string& to)> fallback;   // a fallback block
            std::function<void(std::size_t chars)> compaction;         // a compaction block (the summary's size)
        };

        explicit MessageBuilder(Callbacks callbacks = {}) : cb_(std::move(callbacks)) {}

        void event(const std::string& type, const std::string& data);

        bool finished() const noexcept { return finished_; }          // message_stop arrived
        const std::string& stopReason() const noexcept { return stopReason_; }
        const std::string& model() const noexcept { return model_; }
        const std::string& error() const noexcept { return error_; }  // an error event's message
        const std::string& errorType() const noexcept { return errorType_; }   // its type ("overloaded_error")
        const Usage& usage() const noexcept { return usage_; }

        // The content blocks as received (an array of objects). A tool_use block whose streamed input is not a JSON
        // object has input {} here and its raw text in invalidToolInputs (by tool_use id).
        juce::var content() const;
        const std::vector<std::pair<std::string, std::string>>& invalidToolInputs() const noexcept { return invalid_; }

    private:
        struct Block
        {
            juce::var object;                 // the block as it stands
            std::string text, thinking, signature, json, summary;
            std::string type;
            juce::Array<juce::var> citations;   // a text block's, as they streamed
        };

        Callbacks cb_;
        std::vector<Block> blocks_;
        std::string stopReason_, model_, error_, errorType_;
        Usage usage_;
        bool finished_ = false;
        std::vector<std::pair<std::string, std::string>> invalid_;
    };

    // The message of an error response ({"type":"error","error":{"type":..,"message":..}}), or the body itself.
    std::string apiError(int status, const std::string& body);

    // The same for a response from the server. Its own refusals are written for the user and shown as they are: 401
    // (the sign-in is no longer good: sign in again), 402 (the credit is used up), 403 (the account is disabled), 400
    // (a model it has no price for), 502 (Anthropic refused the server's key). Anthropic's other answers, which it
    // passes back, read as apiError's.
    std::string serverError(int status, const std::string& body);

    // Just the message of an error response's body ("" when it has none).
    std::string errorMessage(const std::string& body);

    // JSON helpers on juce::var.
    juce::var parseJson(const std::string& text);
    std::string toJson(const juce::var& v);
    juce::var object(std::initializer_list<std::pair<const char*, juce::var>> fields);
}
