// Source/plugin/ClaudeApi.h: the Claude Messages API over HTTPS (raw HTTP: there is no Anthropic SDK for C++), for the
// chat (ChatAgent.h).
//
//   Transport       POST and GET against https://api.anthropic.com. httpsTransport() is JUCE's WebInputStream; the
//                   probes give the agent a scripted one. A POST's body streams to a callback as it arrives.
//   SseParser       Server-Sent Events: the stream cut into (event, data) pairs, however the chunks fall.
//   MessageBuilder  a streamed message (message_start, content_block_start/delta/stop, message_delta, error) put back
//                   together: its content blocks exactly as the API sent them (thinking blocks with their signatures,
//                   tool_use blocks with their input parsed from the streamed JSON, a compaction block's summary), the
//                   stop reason and the usage (summed over the request's iterations when the API lists them), and
//                   callbacks as text, thinking, tool calls and compactions arrive, for the chat's live view.
//   apiError        an error response's message ("invalid x-api-key"), from its JSON body.
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

namespace testingplayground::claude
{
    inline constexpr const char* kHost = "https://api.anthropic.com";
    inline constexpr const char* kApiVersion = "2026-10-21";

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

        // POST `body` (JSON) to `path`. A 200's body goes to onChunk as it arrives (false from it stops reading);
        // any other status's body is returned whole. `cancel` set from another thread ends the call soon.
        virtual HttpResult post(const std::string& path, const std::string& body, const Headers& headers,
                                const std::function<bool(std::string_view)>& onChunk,
                                const std::atomic<bool>& cancel) = 0;
        virtual HttpResult get(const std::string& path, const Headers& headers, const std::atomic<bool>& cancel) = 0;
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

    // JSON helpers on juce::var.
    juce::var parseJson(const std::string& text);
    std::string toJson(const juce::var& v);
    juce::var object(std::initializer_list<std::pair<const char*, juce::var>> fields);
}
