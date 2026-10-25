// Source/plugin/ClaudeApi.cpp: see ClaudeApi.h.
#include "plugin/ClaudeApi.h"

#include <chrono>
#include <thread>

namespace testingplaygroundsynth::claude
{
    // ---- JSON helpers ------------------------------------------------------------------------------------------------
    juce::var parseJson(const std::string& text)
    {
        juce::var v;
        if (juce::JSON::parse(juce::String::fromUTF8(text.data(), static_cast<int>(text.size())), v).failed())
            return {};
        return v;
    }

    std::string toJson(const juce::var& v)
    {
        return juce::JSON::toString(v, juce::JSON::FormatOptions{}.withSpacing(juce::JSON::Spacing::none))
            .toStdString();
    }

    juce::var object(std::initializer_list<std::pair<const char*, juce::var>> fields)
    {
        auto* o = new juce::DynamicObject();
        for (const auto& [k, v] : fields)
            o->setProperty(k, v);
        return juce::var(o);
    }

    std::string apiError(int status, const std::string& body)
    {
        std::string message;
        const juce::var v = parseJson(body);
        if (auto* o = v.getDynamicObject())
            if (auto* e = o->getProperty("error").getDynamicObject())
                message = e->getProperty("message").toString().toStdString();
        if (message.empty())
            message = body.substr(0, 300);
        switch (status)
        {
            case 401: return "the API key was refused (" + message + "): add it again";
            case 403: return "this key may not do that (" + message + ")";
            case 404: return "not found (" + message + ")";
            case 413: return "the conversation is too long for one request (" + message + "): start a new chat";
            case 429: return "rate limited (" + message + ")";
            case 529: return "Anthropic's API is overloaded (" + message + ")";
            default: break;
        }
        return "HTTP " + std::to_string(status) + ": " + message;
    }

    // ---- the transport ---------------------------------------------------------------------------------------------
    namespace
    {
        class HttpsTransport final : public Transport
        {
        public:
            HttpResult post(const std::string& path, const std::string& body, const Headers& headers,
                            const std::function<bool(std::string_view)>& onChunk,
                            const std::atomic<bool>& cancel) override
            {
                return request(true, path, body, headers, onChunk, cancel);
            }

            HttpResult get(const std::string& path, const Headers& headers, const std::atomic<bool>& cancel) override
            {
                return request(false, path, {}, headers, {}, cancel);
            }

        private:
            static HttpResult request(bool post, const std::string& path, const std::string& body,
                                      const Headers& headers, const std::function<bool(std::string_view)>& onChunk,
                                      const std::atomic<bool>& cancel)
            {
                HttpResult r;
                juce::URL url(juce::String(kHost) + juce::String(path));
                if (post)
                    url = url.withPOSTData(juce::MemoryBlock(body.data(), body.size()));
                juce::WebInputStream stream(url, post);
                juce::String extra;
                for (const auto& [k, v] : headers)
                    extra << juce::String(k) << ": " << juce::String::fromUTF8(v.c_str()) << "\r\n";
                stream.withExtraHeaders(extra).withConnectionTimeout(120000);   // idle time allowed between data

                // A blocking read ends only by data or cancel(): a watcher cancels it when asked.
                std::atomic<bool> done{ false };
                std::thread watcher([&] {
                    while (!done.load())
                    {
                        if (cancel.load())
                        {
                            stream.cancel();
                            return;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(40));
                    }
                });
                const auto finish = [&] {
                    done.store(true);
                    watcher.join();
                };

                if (!stream.connect(nullptr))
                {
                    r.error = cancel.load() ? "stopped" : "no connection to api.anthropic.com";
                    finish();
                    return r;
                }
                r.status = stream.getStatusCode();
                const juce::StringPairArray responseHeaders = stream.getResponseHeaders();
                for (const auto& key : responseHeaders.getAllKeys())
                    if (key.equalsIgnoreCase("retry-after"))
                        r.retryAfterSeconds = responseHeaders[key].getIntValue();
                if (r.status == 200 && onChunk)
                {
                    char buffer[8192];
                    while (!cancel.load())
                    {
                        const int n = stream.read(buffer, static_cast<int>(sizeof buffer));
                        if (n <= 0)
                            break;
                        if (!onChunk(std::string_view(buffer, static_cast<std::size_t>(n))))
                            break;
                    }
                }
                else
                {
                    r.body = stream.readEntireStreamAsString().toStdString();
                }
                if (cancel.load())
                    r.error = "stopped";
                finish();
                return r;
            }
        };
    }

    std::unique_ptr<Transport> httpsTransport() { return std::make_unique<HttpsTransport>(); }

    // ---- SSE ----------------------------------------------------------------------------------------------------------
    void SseParser::feed(std::string_view chunk,
                         const std::function<void(const std::string&, const std::string&)>& onEvent)
    {
        const auto endLine = [&] {
            if (line_.empty())
            {
                if (!data_.empty() || !event_.empty())
                    onEvent(event_, data_);
                event_.clear();
                data_.clear();
                return;
            }
            if (line_.front() != ':')               // a comment
            {
                const std::size_t colon = line_.find(':');
                const std::string field = line_.substr(0, colon);
                std::string value = colon == std::string::npos ? std::string() : line_.substr(colon + 1);
                if (!value.empty() && value.front() == ' ')
                    value.erase(value.begin());
                if (field == "event")
                    event_ = value;
                else if (field == "data")
                    data_ += (data_.empty() ? "" : "\n") + value;
            }
            line_.clear();
        };
        for (const char c : chunk)
        {
            if (c == '\n')
            {
                if (cr_)
                {
                    cr_ = false;                     // the LF of a CR LF
                    continue;
                }
                endLine();
            }
            else if (c == '\r')
            {
                cr_ = true;
                endLine();
            }
            else
            {
                cr_ = false;
                line_ += c;
            }
        }
    }

    // ---- the message -------------------------------------------------------------------------------------------------
    void MessageBuilder::event(const std::string& type, const std::string& data)
    {
        const juce::var v = parseJson(data);
        auto* o = v.getDynamicObject();
        if (o == nullptr)
            return;
        const std::string kind = type.empty() ? o->getProperty("type").toString().toStdString() : type;
        if (kind == "message_start")
        {
            const juce::var message = o->getProperty("message");
            model_ = message.getProperty("model", "").toString().toStdString();
            const juce::var u = message.getProperty("usage", juce::var());
            usage_.input = static_cast<int64_t>(u.getProperty("input_tokens", 0));
            usage_.cacheRead = static_cast<int64_t>(u.getProperty("cache_read_input_tokens", 0));
            usage_.cacheWrite = static_cast<int64_t>(u.getProperty("cache_creation_input_tokens", 0));
            usage_.output = static_cast<int64_t>(u.getProperty("output_tokens", 0));
        }
        else if (kind == "content_block_start")
        {
            const int index = static_cast<int>(o->getProperty("index"));
            if (index < 0 || index > 4096)
                return;
            if (static_cast<std::size_t>(index) >= blocks_.size())
                blocks_.resize(static_cast<std::size_t>(index) + 1);
            Block& b = blocks_[static_cast<std::size_t>(index)];
            b.object = parseJson(toJson(o->getProperty("content_block")));   // a copy of its own
            b.type = b.object.getProperty("type", "").toString().toStdString();
            if (b.type == "text")
                b.text = b.object.getProperty("text", "").toString().toStdString();
            else if (b.type == "thinking")
                b.thinking = b.object.getProperty("thinking", "").toString().toStdString();
            else if (b.type == "compaction")
                b.summary = b.object.getProperty("content", "").toString().toStdString();
            else if (b.type == "tool_use" && cb_.toolStart)
                cb_.toolStart(b.object.getProperty("name", "").toString().toStdString());
            else if (b.type == "fallback" && cb_.fallback)
                cb_.fallback(b.object.getProperty("from", juce::var()).getProperty("model", "").toString().toStdString(),
                             b.object.getProperty("to", juce::var()).getProperty("model", "").toString().toStdString());
            if (b.type == "text" && !b.text.empty() && cb_.text)
                cb_.text(b.text);
        }
        else if (kind == "content_block_delta")
        {
            const int index = static_cast<int>(o->getProperty("index"));
            if (index < 0 || static_cast<std::size_t>(index) >= blocks_.size())
                return;
            Block& b = blocks_[static_cast<std::size_t>(index)];
            const juce::var d = o->getProperty("delta");
            const std::string dt = d.getProperty("type", "").toString().toStdString();
            if (dt == "text_delta")
            {
                const std::string t = d.getProperty("text", "").toString().toStdString();
                b.text += t;
                if (cb_.text)
                    cb_.text(t);
            }
            else if (dt == "thinking_delta")
            {
                const std::string t = d.getProperty("thinking", "").toString().toStdString();
                b.thinking += t;
                if (cb_.thinking)
                    cb_.thinking(t);
            }
            else if (dt == "signature_delta")
            {
                b.signature += d.getProperty("signature", "").toString().toStdString();
            }
            else if (dt == "input_json_delta")
            {
                b.json += d.getProperty("partial_json", "").toString().toStdString();
                if (cb_.toolInput)
                    cb_.toolInput(b.json);
            }
            else if (dt == "compaction_delta")
            {
                // The summary arrives whole, in one delta (the compaction docs).
                b.summary += d.getProperty("content", "").toString().toStdString();
            }
        }
        else if (kind == "content_block_stop")
        {
            const int index = static_cast<int>(o->getProperty("index"));
            if (index < 0 || static_cast<std::size_t>(index) >= blocks_.size())
                return;
            Block& b = blocks_[static_cast<std::size_t>(index)];
            auto* bo = b.object.getDynamicObject();
            if (bo == nullptr)
                return;
            if (b.type == "text")
                bo->setProperty("text", juce::String::fromUTF8(b.text.c_str()));
            else if (b.type == "thinking")
            {
                bo->setProperty("thinking", juce::String::fromUTF8(b.thinking.c_str()));
                if (!b.signature.empty())
                    bo->setProperty("signature", juce::String(b.signature));
            }
            else if (b.type == "compaction")
            {
                bo->setProperty("content", juce::String::fromUTF8(b.summary.c_str()));
                if (cb_.compaction)
                    cb_.compaction(b.summary.size());
            }
            else if (b.type == "tool_use")
            {
                // Streamed eagerly: the API did not validate the input, so it is parsed strictly here.
                juce::var input;
                const bool ok = b.json.empty()
                                    ? true
                                    : juce::JSON::parse(juce::String::fromUTF8(b.json.c_str()), input).wasOk();
                if (b.json.empty())
                    input = juce::var(new juce::DynamicObject());
                if (!ok || input.getDynamicObject() == nullptr)
                {
                    invalid_.emplace_back(b.object.getProperty("id", "").toString().toStdString(), b.json);
                    input = juce::var(new juce::DynamicObject());
                }
                bo->setProperty("input", input);
            }
        }
        else if (kind == "message_delta")
        {
            const juce::var d = o->getProperty("delta");
            const juce::var reason = d.getProperty("stop_reason", juce::var());
            if (!reason.isVoid() && !reason.isUndefined())
                stopReason_ = reason.toString().toStdString();
            const juce::var u = o->getProperty("usage");
            if (u.hasProperty("output_tokens"))
                usage_.output = static_cast<int64_t>(u.getProperty("output_tokens", 0));
            if (u.hasProperty("input_tokens"))
                usage_.input = static_cast<int64_t>(u.getProperty("input_tokens", 0));
            if (u.hasProperty("cache_read_input_tokens"))
                usage_.cacheRead = static_cast<int64_t>(u.getProperty("cache_read_input_tokens", 0));
            if (u.hasProperty("cache_creation_input_tokens"))
                usage_.cacheWrite = static_cast<int64_t>(u.getProperty("cache_creation_input_tokens", 0));
            // With compaction on, the iterations list every sampling step: the top-level counts leave out a
            // compaction's, so the request's whole cost is their sum.
            const juce::var iterations = u.getProperty("iterations", juce::var());
            if (const auto* its = iterations.getArray(); its != nullptr && !its->isEmpty())
            {
                int64_t in = 0, out = 0;
                int compactions = 0;
                for (const auto& it : *its)
                {
                    in += static_cast<int64_t>(it.getProperty("input_tokens", 0));
                    out += static_cast<int64_t>(it.getProperty("output_tokens", 0));
                    compactions += it.getProperty("type", "").toString() == "compaction" ? 1 : 0;
                }
                if (compactions > 0)
                {
                    usage_.input = in;
                    usage_.output = out;
                }
                usage_.compactions = compactions;
            }
        }
        else if (kind == "message_stop")
        {
            finished_ = true;
        }
        else if (kind == "error")
        {
            const juce::var e = o->getProperty("error");
            error_ = e.getProperty("message", "").toString().toStdString();
            errorType_ = e.getProperty("type", "").toString().toStdString();
            if (!errorType_.empty())
                error_ = errorType_ + ": " + error_;
        }
    }

    juce::var MessageBuilder::content() const
    {
        juce::Array<juce::var> out;
        for (const Block& b : blocks_)
            if (auto* o = b.object.getDynamicObject())
            {
                // A block the stream broke off in (no content_block_stop) has what arrived of it.
                if (b.type == "text")
                    o->setProperty("text", juce::String::fromUTF8(b.text.c_str()));
                else if (b.type == "thinking")
                    o->setProperty("thinking", juce::String::fromUTF8(b.thinking.c_str()));
                else if (b.type == "compaction" && !b.summary.empty())
                    o->setProperty("content", juce::String::fromUTF8(b.summary.c_str()));
                out.add(b.object);
            }
        return juce::var(out);
    }
}
