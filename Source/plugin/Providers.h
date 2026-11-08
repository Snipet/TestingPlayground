// Source/plugin/Providers.h: the chat's other model providers, beside Anthropic (ClaudeApi.h), which stays the default
// and is untouched by this file: any server that speaks OpenAI's Chat Completions (OpenAI, OpenRouter, a server on
// this Mac or the local network such as Ollama or LM Studio, by its base URL) and Google Gemini (its Generative
// Language API). ChatAgent keeps one history, in the Messages API's shape, and runs one turn loop; for another
// provider a request is that history translated (request), and its streamed answer is translated back into the
// Messages API's events (Stream), which the same MessageBuilder puts together. So the tools, the checks, the turn's
// rounds, the retries and the history work alike over every provider.
//
// What another provider does not have is left out, never broken: thinking blocks (bound to Anthropic's models and the
// prompt; a model's own reasoning is shown, never kept), prompt caching's marks (OpenAI and Gemini cache on their own),
// compaction, fallbacks, fast mode, web search and its blocks (their text stays), and the planner. The thinking level
// is the provider's own where its model takes one (Reasoning, below: OpenRouter's, OpenAI's and Gemini's), sent only
// when the user chose one. A model that takes no
// pictures gets a line of text for each. Tool calls keep their ids in the history as this file makes them (call_ and 24
// hex digits: unique, and in the Messages API's alphabet), so a chat can go on with another provider; a Gemini call's
// thought signature rides on its tool_use block as "thought_signature", which forAnthropic takes off again.
//
// The wire, per the providers' own API references (what could not be checked without the network is listed in the
// README's "Other model providers"):
//   OpenAI-compatible  POST <base>/chat/completions, stream (stream_options.include_usage), the system prompt as the
//                      first message, the tools as {type: function, function: {name, description, parameters}}, a
//                      tool's result as a {role: tool, tool_call_id} message (its pictures in a user message after),
//                      pictures as image_url data URLs; Authorization: Bearer <key> when there is a key.
//                      GET <base>/models lists the models (OpenRouter's says which take tools, pictures, their
//                      context and their thinking levels). A thinking level: OpenRouter's "reasoning": {"effort"} (its
//                      reasoning comes back as delta.reasoning, or reasoning_details' text), OpenAI's own
//                      "reasoning_effort"; another server's none.
//   Gemini             POST /v1beta/models/<model>:streamGenerateContent?alt=sse with x-goog-api-key, systemInstruction,
//                      contents (user and model; a call a functionCall part, its result a functionResponse part),
//                      tools' functionDeclarations with their schemas as JSON Schema (parametersJsonSchema; on a 400
//                      that refuses it, Gemini's own subset, parameters); GET /v1beta/models lists them. A thinking
//                      level: generationConfig.thinkingConfig, thinkingLevel (Gemini 3 and later) or thinkingBudget
//                      (2.5), never both, with includeThoughts (its thoughts, parts marked "thought", shown).
//
// Base URLs: https, or http only to a server on this Mac or the local network (localhost, 127.x, ::1, 10.x,
// 172.16-31.x, 192.168.x, 169.254.x, *.local), so a conversation never crosses the internet in clear. A key is
// bound to its server's origin (ChatStore.h) and never sent anywhere else; an Anthropic key is refused for another
// provider. JUCE's core and std only; no state.
#pragma once

#include "plugin/ChatPort.h"
#include "plugin/ClaudeApi.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace playground::providers
{
    enum class Kind : uint8_t { anthropic, openai, gemini };
    Kind kindOf(std::string_view id);                    // "openai", "gemini", else Anthropic
    const char* idOf(Kind kind);

    inline constexpr const char* kGeminiBase = "https://generativelanguage.googleapis.com";

    // ---- servers ------------------------------------------------------------------------------------------------------
    // An OpenAI-compatible server's base URL as the chat keeps it: http or https (the scheme and the host in lower
    // case), a host, a port, a path, no user, query or fragment, no slash at its end; http only to a server on this Mac
    // or the local network. "" with why when it is none.
    std::string canonical(std::string_view base, std::string& why);
    std::string originOf(std::string_view base);         // "https://openrouter.ai" (scheme, host and port)
    std::string pathOf(std::string_view base);           // "/api/v1" ("" for none)
    std::string hostOf(std::string_view base);           // "openrouter.ai", "localhost:11434"
    bool isLocal(std::string_view base);                 // on this Mac or the local network
    // The server a provider's requests go to (the one its key is bound to): Anthropic's and Gemini's own, else its base.
    std::string baseOf(const ChatProvider& p);
    const ChatPreset* presetOf(const ChatProvider& p);   // the preset it is, nullptr for none (a custom server)
    std::string nameOf(const ChatProvider& p);           // the preset's name, else the server's host
    bool keyless(const ChatProvider& p);                 // an OpenAI-compatible server on this Mac or the local network
    // Whether a turn with no model chosen takes the list's first: Gemini's (ordered here: the newest version, pro before
    // flash) and a server's on this Mac or the local network (the models its user loaded). OpenAI's, OpenRouter's and
    // another remote server's lists are long and their newest is anyone's guess, of any price: the user chooses.
    bool picksFirst(const ChatProvider& p);

    // Whether `key` may be `p`'s: printable ASCII with no space, 8 to 400 characters, and never an Anthropic key
    // (sk-ant-) for another provider (it would go to another company). False with why.
    bool plausibleKey(const ChatProvider& p, std::string_view key, std::string& why);
    // A model's id as a request may name it: 1 to 200 printable ASCII characters, no space.
    bool plausibleModel(std::string_view model);

    // ---- requests -----------------------------------------------------------------------------------------------------
    struct Options
    {
        bool images = true;                              // the model takes pictures (else a line of text each)
        bool jsonSchema = true;                          // Gemini: the tools' schemas as JSON Schema, else its subset
        std::string effort = {};                         // the thinking level chosen, one the model takes (levelsOf);
                                                         // "": none sent, the model's own default
    };
    // A tool call made against Gemini's own subset (Options::jsonSchema false), as the tool takes it: the subset asks
    // for an array whose items the tool's schema leaves open (run_function's args, write_file's files, edit_file's edits
    // and files) as one of strings, each a JSON value written out (0.5, true, [1, 2], {"name": ...}), and each such
    // string that reads as JSON is that value again. `schema`: the tool's input_schema. Anything else as it came.
    juce::var fromSubset(const juce::var& input, const juce::var& schema);
    struct Endpoint
    {
        std::string host, path;                          // the transport's (ClaudeApi.h): "https://openrouter.ai", "/api/v1/..."
    };
    // A request of the Messages API's shape (ChatAgent::buildRequest: its model, system, tools and messages; what else it
    // holds is Anthropic's alone and left out) as `p`'s own, JSON; where it goes; the headers (the key, when there is one).
    std::string body(const ChatProvider& p, const juce::var& messagesApi, const Options& options);
    Endpoint endpoint(const ChatProvider& p, const std::string& model);
    claude::Headers headers(const ChatProvider& p, const std::string& key);
    // The models: where the list is (GET), and the models in its answer that the chat can use (they chat and call
    // tools), in the order the picker offers them (the newest first where the list says when).
    Endpoint modelsEndpoint(const ChatProvider& p);
    std::vector<ChatModel> modelsOf(const ChatProvider& p, const std::string& body);
    // A model no list described (one typed, or chosen before the list came): its id and nothing known of it.
    ChatModel plainModel(const std::string& id);
    // The same for `p`'s server: its thinking levels where its name tells them (levelsOf: OpenAI's, Gemini's).
    ChatModel plainModel(const ChatProvider& p, const std::string& id);

    // ---- thinking levels ----------------------------------------------------------------------------------------------
    // How `p`'s server takes a thinking level (the chat's Thinking menu: ChatModel::efforts, in the words Anthropic's
    // use: "none" (Off), "minimal", "low", "medium", "high", "xhigh", "max"):
    //   openRouter  openrouter.ai: "reasoning": {"effort": <level>}; its models list says each model's levels, its
    //               default and whether reasoning can be turned off (modelsOf)
    //   openAi      api.openai.com: "reasoning_effort": <level>; its list says nothing of them: by the model's family
    //               (levelsOf)
    //   gemini      generationConfig.thinkingConfig: thinkingLevel or thinkingBudget, by the model's family
    //               (levelsOf)
    //   none        any other server (a custom one, Ollama, LM Studio): no levels, nothing sent
    enum class Reasoning : uint8_t { none, openRouter, openAi, gemini };
    Reasoning reasoningOf(const ChatProvider& p);
    // The levels a model takes by its id, where its server's list does not say them, into m.efforts (in the menu's
    // order) and m.defaultEffort (the level it uses when none is sent, "" when not known); none for a model that takes
    // none, or one this file cannot place in a family. OpenAI's: o1, o3 and o4 low, medium, high (medium by default);
    // gpt-5 minimal, low, medium, high (medium); gpt-5.1 none, low, medium, high (none); gpt-5.2 none, low, medium,
    // high, xhigh (its default unsaid); a later gpt-5.x or gpt-6 and on low, medium, high (unsaid); none for an id with
    // "-chat" (gpt-5-chat-latest), o1-mini, o1-preview and gpt-4 and before. Gemini's: Gemini 3 and later by
    // thinkingLevel, pro low and high, flash minimal, low, medium and high (high by default; neither can turn thinking
    // off); Gemini 2.5 by thinkingBudget, low, medium and high, and none (Off) on flash and flash-lite alone (flash-lite
    // off by default, the others' default dynamic: unsaid); none for Gemini 2.0 and before, and for an alias that names
    // no version (gemini-flash-latest).
    void levelsOf(const ChatProvider& p, ChatModel& m);
    // A 400 (or 422) that refuses the thinking level sent (OpenAI's "Unsupported parameter: 'reasoning_effort'",
    // Anthropic's through OpenRouter wanting a thinking block the history does not keep, Gemini's naming thinking_level,
    // thinkingBudget or thinking_config): asked again without it.
    bool aboutEffort(const ChatProvider& p, int status, const std::string& error);

    // ---- answers ------------------------------------------------------------------------------------------------------
    // A provider's streamed answer (its server-sent events, as claude::SseParser cuts them) in, the Messages API's events
    // out to `sink` (a MessageBuilder's event()): message_start, a block a text, a reasoning (shown, as thinking) or a
    // tool call, message_delta with the stop reason (end_turn, tool_use, max_tokens, refusal) and the usage,
    // message_stop. finish() after the transport returns: an answer that ended (OpenAI's [DONE] or a finish reason,
    // Gemini's finishReason) gets its end; one that broke off gets none, so the turn asks again. An error in the
    // stream is an error event (api_error: asked again).
    using Sink = std::function<void(const std::string& type, const std::string& data)>;
    class Stream
    {
    public:
        virtual ~Stream() = default;
        virtual void event(const std::string& type, const std::string& data) = 0;
        virtual void finish() = 0;
    };
    std::unique_ptr<Stream> streamFor(const ChatProvider& p, Sink sink);

    // An error response in words, naming the provider (and p.model when the model cannot serve this chat: aboutModel;
    // OpenRouter's "Provider returned error" with that provider's own words from its metadata.raw);
    // a request that reached no server ("is Ollama running?"); a redirect, never followed (ClaudeApi.h), and where it
    // pointed.
    std::string errorText(const ChatProvider& p, int status, const std::string& body);
    std::string connectionText(const ChatProvider& p, const std::string& error);
    std::string redirectText(const ChatProvider& p, int status, const std::string& location);
    // The response says the key is not valid (a 401; Gemini's 400 "API key not valid").
    bool keyRefused(const ChatProvider& p, int status, const std::string& body);
    // A 400 that says the model takes no pictures; one that says the conversation is longer than its context; Gemini's
    // that refuses the tools' JSON Schema.
    bool aboutImages(int status, const std::string& error);
    bool aboutContext(const std::string& error);
    bool aboutSchema(int status, const std::string& error);
    // A 400, 404 or 422 that says the model cannot serve this chat: served by OpenAI's Responses API alone, not a chat
    // model, or one that calls no tools (OpenAI's, OpenRouter's and Ollama's words).
    bool aboutModel(int status, const std::string& error);

    // The history as the Messages API takes it: without what another provider's answers carry of their own (a Gemini
    // call's thought_signature), which it refuses. `messages` as it is when nothing is (an Anthropic chat: unchanged).
    juce::var forAnthropic(const juce::Array<juce::var>& messages);
}
