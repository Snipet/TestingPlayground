// Source/plugin/ClaudeApi.cpp: see ClaudeApi.h.
#include "plugin/ClaudeApi.h"

#include "fminus/Lang.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

#if defined(__linux__)
#include <curl/curl.h>

#include <cctype>
#include <cstdlib>
#include <mutex>
#endif

namespace playground::claude
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

    // ---- the host ------------------------------------------------------------------------------------------------------
    std::string hostName(const std::string& host)
    {
        std::string h = host;
        if (const std::size_t scheme = h.find("://"); scheme != std::string::npos)
            h.erase(0, scheme + 3);
        if (const std::size_t slash = h.find('/'); slash != std::string::npos)
            h.erase(slash);
        return h;
    }

    // ---- errors ------------------------------------------------------------------------------------------------------
    std::string errorMessage(const std::string& body)
    {
        const juce::var v = parseJson(body);
        if (auto* o = v.getDynamicObject())
            if (auto* e = o->getProperty("error").getDynamicObject())
                return e->getProperty("message").toString().toStdString();
        return {};
    }

    std::string apiError(int status, const std::string& body)
    {
        std::string message = errorMessage(body);
        if (message.empty())
            message = std::string(lang::headOf(body, 300));   // never inside a character
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
#if defined(__linux__)
        // Linux: libcurl. JUCE's own client there speaks plain http:// alone (an https:// address never connects), and
        // its curl client fills a whole read before it returns. This one gives a 200's body to onChunk piece by piece
        // as it arrives, stops within a poll (40 ms) of `cancel`, follows no redirect, and gives up a connection that
        // stays idle for two minutes (no byte, not even the API's pings), as NSURLSession's timeout does on macOS.
        std::once_flag curlStarted;

        struct CurlCall
        {
            CURL* easy = nullptr;
            const std::function<bool(std::string_view)>* onChunk = nullptr;
            HttpResult* result = nullptr;
            long status = 0;                          // the response's, read at its body's first byte
            std::size_t maxOther = 0;                 // what of another status's body is kept at most
        };

        std::size_t curlHeader(char* data, std::size_t size, std::size_t count, void* user)
        {
            auto& call = *static_cast<CurlCall*>(user);
            const std::size_t bytes = size * count;
            std::string_view line(data, bytes);
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())) != 0)
                line.remove_suffix(1);
            if (line.substr(0, 5) == "HTTP/")         // a response begins (another after a 100 Continue)
            {
                call.result->retryAfterSeconds = -1;
                call.result->location.clear();
            }
            else if (const std::size_t colon = line.find(':'); colon != std::string_view::npos)
            {
                std::string name(line.substr(0, colon));
                for (char& c : name)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                std::string_view value = line.substr(colon + 1);
                while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0)
                    value.remove_prefix(1);
                if (name == "retry-after")
                    call.result->retryAfterSeconds = std::atoi(std::string(value).c_str());
                else if (name == "location")
                    call.result->location = std::string(value);
            }
            return bytes;
        }

        std::size_t curlBody(char* data, std::size_t size, std::size_t count, void* user)
        {
            auto& call = *static_cast<CurlCall*>(user);
            const std::size_t bytes = size * count;
            if (call.status == 0)
                curl_easy_getinfo(call.easy, CURLINFO_RESPONSE_CODE, &call.status);
            if (call.status == 200 && *call.onChunk)
                return (*call.onChunk)(std::string_view(data, bytes)) ? bytes : 0;   // 0: false from it ends the read
            if (call.result->body.size() + bytes > call.maxOther)
            {
                call.result->body.append(data, call.maxOther - call.result->body.size());
                return 0;                             // past the limit: the read ends there
            }
            call.result->body.append(data, bytes);
            return bytes;
        }

        class HttpsTransport final : public Transport
        {
        public:
            HttpResult post(const std::string& host, const std::string& path, const std::string& body,
                            const Headers& headers, const std::function<bool(std::string_view)>& onChunk,
                            const std::atomic<bool>& cancel) override
            {
                return request(true, host, path, body, headers, onChunk, cancel);
            }

            HttpResult get(const std::string& host, const std::string& path, const Headers& headers,
                           const std::atomic<bool>& cancel) override
            {
                return request(false, host, path, {}, headers, {}, cancel);
            }

            HttpResult getLimited(const std::string& host, const std::string& path, const Headers& headers,
                                  const std::function<bool(std::string_view)>& onChunk, std::size_t maxOther,
                                  const std::atomic<bool>& cancel) override
            {
                return request(false, host, path, {}, headers, onChunk, cancel, maxOther);
            }

        private:
            // `maxOther`: what of a status other than 200's body is kept at most (getLimited's; else all of it).
            static HttpResult request(bool post, const std::string& host, const std::string& path,
                                      const std::string& body, const Headers& headers,
                                      const std::function<bool(std::string_view)>& onChunk,
                                      const std::atomic<bool>& cancel,
                                      std::size_t maxOther = std::numeric_limits<std::size_t>::max())
            {
                std::call_once(curlStarted, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
                HttpResult r;
                CURL* const easy = curl_easy_init();
                CURLM* const multi = curl_multi_init();
                curl_slist* list = nullptr;
                for (const auto& [k, v] : headers)
                    list = curl_slist_append(list, (k + ": " + v).c_str());
                list = curl_slist_append(list, "Expect:");   // a large body (pictures) sent at once, no 100 Continue
                const auto release = [&] {
                    if (multi != nullptr && easy != nullptr)
                        curl_multi_remove_handle(multi, easy);
                    if (multi != nullptr)
                        curl_multi_cleanup(multi);
                    if (easy != nullptr)
                        curl_easy_cleanup(easy);
                    curl_slist_free_all(list);
                };
                if (easy == nullptr || multi == nullptr || list == nullptr)
                {
                    release();
                    r.error = "no connection to " + hostName(host);
                    return r;
                }
                CurlCall call{ easy, &onChunk, &r, 0, maxOther };
                const std::string url = host + path;
                curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
                curl_easy_setopt(easy, CURLOPT_HTTPHEADER, list);
                curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
                // No redirect followed: the conversation and the key go to the host asked for, never to where its
                // answer points (another origin, or plain HTTP).
                curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
                curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
#else
                curl_easy_setopt(easy, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
                curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 120L);
                curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);   // idle: under a byte a second for two minutes
                curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 120L);
                curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, curlHeader);
                curl_easy_setopt(easy, CURLOPT_HEADERDATA, &call);
                curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, curlBody);
                curl_easy_setopt(easy, CURLOPT_WRITEDATA, &call);
                if (post)
                {
                    curl_easy_setopt(easy, CURLOPT_POST, 1L);
                    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body.data());
                    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
                }
                else
                {
                    curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
                }
                curl_multi_add_handle(multi, easy);

                bool stopped = false;
                int running = 1;
                while (running > 0)
                {
                    if (cancel.load())
                    {
                        stopped = true;
                        break;
                    }
                    if (curl_multi_perform(multi, &running) != CURLM_OK)
                        break;
                    if (running > 0 && curl_multi_poll(multi, nullptr, 0, 40, nullptr) != CURLM_OK)
                        break;
                }
                CURLcode result = CURLE_OK;
                int left = 0;
                while (const CURLMsg* m = curl_multi_info_read(multi, &left))
                    if (m->msg == CURLMSG_DONE && m->easy_handle == easy)
                        result = m->data.result;
                long status = 0;
                curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
                release();

                r.status = static_cast<int>(status);
                if (r.status < 300 || r.status >= 400)
                    r.location.clear();
                if (stopped || cancel.load())
                    r.error = "stopped";
                else if (r.status == 0)
                    r.error = "no connection to " + hostName(host) + " (" + curl_easy_strerror(result) + ")";
                return r;
            }
        };
#else
        class HttpsTransport final : public Transport
        {
        public:
            HttpResult post(const std::string& host, const std::string& path, const std::string& body,
                            const Headers& headers, const std::function<bool(std::string_view)>& onChunk,
                            const std::atomic<bool>& cancel) override
            {
                return request(true, host, path, body, headers, onChunk, cancel);
            }

            HttpResult get(const std::string& host, const std::string& path, const Headers& headers,
                           const std::atomic<bool>& cancel) override
            {
                return request(false, host, path, {}, headers, {}, cancel);
            }

            HttpResult getLimited(const std::string& host, const std::string& path, const Headers& headers,
                                  const std::function<bool(std::string_view)>& onChunk, std::size_t maxOther,
                                  const std::atomic<bool>& cancel) override
            {
                return request(false, host, path, {}, headers, onChunk, cancel, maxOther);
            }

        private:
            // `maxOther`: what of a status other than 200's body is kept at most (getLimited's; else all of it).
            static HttpResult request(bool post, const std::string& host, const std::string& path,
                                      const std::string& body, const Headers& headers,
                                      const std::function<bool(std::string_view)>& onChunk,
                                      const std::atomic<bool>& cancel,
                                      std::size_t maxOther = std::numeric_limits<std::size_t>::max())
            {
                HttpResult r;
                // The address as it was built, never parsed: JUCE would take a query apart (Gemini's ?alt=sse) and, for
                // a POST, write it into the body before the posted data, out of the request line.
                juce::URL url = juce::URL::createWithoutParsing(juce::String(host) + juce::String(path));
                if (post)
                    url = url.withPOSTData(juce::MemoryBlock(body.data(), body.size()));
                juce::WebInputStream stream(url, post);
                juce::String extra;
                for (const auto& [k, v] : headers)
                    extra << juce::String(k) << ": " << juce::String::fromUTF8(v.c_str()) << "\r\n";
                stream.withExtraHeaders(extra).withConnectionTimeout(120000);   // idle time allowed between data
                // No redirect followed (JUCE follows five by default, keeping the headers): the conversation and the
                // key go to the host asked for, never to where its answer points (another origin, or plain HTTP).
                stream.withNumRedirectsToFollow(0);

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
                    if (watcher.joinable())
                        watcher.join();
                };
                // On every way out, a throw too (out of memory in a read: fminus/Failure.h): the watcher stopped and
                // joined before the stream it watches goes; a joinable thread destroyed would end the process.
                struct Joined
                {
                    const decltype(finish)& f;
                    ~Joined() { f(); }
                } joined{ finish };

                if (!stream.connect(nullptr))
                {
                    r.error = cancel.load() ? "stopped" : "no connection to " + hostName(host);
                    finish();
                    return r;
                }
                r.status = stream.getStatusCode();
                const juce::StringPairArray responseHeaders = stream.getResponseHeaders();
                for (const auto& key : responseHeaders.getAllKeys())
                {
                    if (key.equalsIgnoreCase("retry-after"))
                        r.retryAfterSeconds = responseHeaders[key].getIntValue();
                    else if (key.equalsIgnoreCase("location") && r.status >= 300 && r.status < 400)
                        r.location = responseHeaders[key].toStdString();
                }
                if (r.status == 200 && onChunk && post)
                {
                    // A streamed answer (server-sent events) a byte at a time: JUCE's read returns only once the count
                    // asked for is there or the body has ended (macOS's waits for it; WinINet's InternetReadFile fills
                    // it), so a larger read would hold the events back until that many bytes came. Each line goes to
                    // onChunk as it ends (an event ends with an empty line), or a full buffer of a long one. A byte
                    // read on macOS also moves what is still buffered to the front, so a backlog of N bytes (a burst
                    // that came at once) would cost N * N / 2 bytes moved, about 50 s for 1 MB: reads that show one
                    // read it in blocks, which the parsers take as any piece. Only on macOS: WinINet's reads move
                    // nothing, so there a block would only wait to be filled, and they stay a byte at a time.
                    using Clock = std::chrono::steady_clock;
                    // Whether a read moves the backlog (macOS's NSMutableData): the only case blocks are read in.
#if defined(__APPLE__)
                    constexpr bool kReadMovesBacklog = true;
#else
                    constexpr bool kReadMovesBacklog = false;
#endif
                    // A read this slow moved a backlog of at least this time x the memory's bandwidth (3 GB/s: 15 KB).
                    constexpr auto kSlowRead = std::chrono::microseconds(5);
                    // One this slow or slower waited for data: no sign of a backlog.
                    constexpr auto kWait = std::chrono::milliseconds(5);
                    // Slow one-byte reads in a row before a block is read: not a stray one (the thread put aside).
                    constexpr int kSlowReads = 32;
                    // A block well under the backlog those reads show, so it is there and the read waits for nothing.
                    constexpr int kBlock = 4096;
                    const auto backlog = [&](Clock::time_point from) {
                        const auto took = Clock::now() - from;
                        return took >= kSlowRead && took < kWait;
                    };
                    char line[8192];
                    std::size_t used = 0;
                    int slow = 0;
                    bool blocks = false;
                    while (!cancel.load())
                    {
                        const Clock::time_point from = Clock::now();
                        if (blocks)
                        {
                            const int n = stream.read(line, kBlock);
                            if (n <= 0)
                                break;
                            blocks = backlog(from);       // a slow block moved a backlog too: the next one is there
                            if (!onChunk(std::string_view(line, static_cast<std::size_t>(n))))
                                break;
                            continue;
                        }
                        if (stream.read(line + used, 1) <= 0)
                            break;
                        slow = kReadMovesBacklog && backlog(from) ? slow + 1 : 0;
                        const char c = line[used++];
                        if (slow >= kSlowReads)
                        {
                            blocks = true;                 // the line so far first, then the blocks
                            slow = 0;
                        }
                        else if (c != '\n' && c != '\r' && used < sizeof line)
                        {
                            continue;
                        }
                        const bool more = onChunk(std::string_view(line, used));
                        used = 0;
                        if (!more)
                            break;
                    }
                    if (used > 0 && !cancel.load())
                        (void)onChunk(std::string_view(line, used));   // a last line the body ended without ending
                }
                else if (r.status == 200 && onChunk)
                {
                    // getLimited's 200 (a download read whole): block reads.
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
                else if (maxOther == std::numeric_limits<std::size_t>::max())
                {
                    r.body = stream.readEntireStreamAsString().toStdString();
                }
                else
                {
                    // getLimited's: read no more than it keeps.
                    std::vector<char> buffer(8192);
                    while (!cancel.load() && r.body.size() < maxOther)
                    {
                        const std::size_t want = std::min(buffer.size(), maxOther - r.body.size());
                        const int n = stream.read(buffer.data(), static_cast<int>(want));
                        if (n <= 0)
                            break;
                        r.body.append(buffer.data(), static_cast<std::size_t>(n));
                    }
                }
                if (cancel.load())
                    r.error = "stopped";
                finish();
                return r;
            }
        };
#endif
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
    // What a usage object says beyond the counts the event reads itself: that it came, the cache writes by lifetime
    // (usage.cache_creation; without it, all of cache_creation_input_tokens as five minutes'), a provider's own cost.
    void MessageBuilder::readUsageExtras(const juce::var& u)
    {
        if (!u.isObject())
            return;
        usage_.reported = true;
        const juce::var split = u.getProperty("cache_creation", juce::var());
        if (split.isObject())
        {
            usage_.cacheWrite5m = static_cast<juce::int64>(split.getProperty("ephemeral_5m_input_tokens", 0));
            usage_.cacheWrite1h = static_cast<juce::int64>(split.getProperty("ephemeral_1h_input_tokens", 0));
        }
        else if (u.hasProperty("cache_creation_input_tokens"))
        {
            usage_.cacheWrite5m = usage_.cacheWrite;
            usage_.cacheWrite1h = 0;
        }
        if (const juce::var cost = u.getProperty("cost", juce::var()); cost.isDouble() || cost.isInt() || cost.isInt64())
        {
            const double usd = static_cast<double>(cost);
            if (std::isfinite(usd) && usd >= 0 && usd < 1e6)
                usage_.costMicroUsd = static_cast<int64_t>(std::llround(usd * 1e6));
        }
    }

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
            usage_.input = static_cast<juce::int64>(u.getProperty("input_tokens", 0));
            usage_.cacheRead = static_cast<juce::int64>(u.getProperty("cache_read_input_tokens", 0));
            usage_.cacheWrite = static_cast<juce::int64>(u.getProperty("cache_creation_input_tokens", 0));
            usage_.output = static_cast<juce::int64>(u.getProperty("output_tokens", 0));
            if (u.hasProperty("speed"))
                usage_.speed = u.getProperty("speed", "").toString().toStdString();
            readUsageExtras(u);
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
            else if (b.type == "server_tool_use" && cb_.serverTool)
                cb_.serverTool(b.object.getProperty("name", "").toString().toStdString());
            else if (b.type.size() > 12 && b.type.compare(b.type.size() - 12, 12, "_tool_result") == 0 && cb_.serverResult)
                cb_.serverResult(b.object);          // web_search_tool_result, web_fetch_tool_result, ...: whole
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
            else if (dt == "citations_delta")
            {
                const juce::var c = d.getProperty("citation", juce::var());
                b.citations.add(c);
                if (cb_.citation)
                    cb_.citation(c);
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
            {
                bo->setProperty("text", juce::String::fromUTF8(b.text.c_str()));
                if (!b.citations.isEmpty())
                    bo->setProperty("citations", b.citations);
            }
            else if (b.type == "thinking")
            {
                bo->setProperty("thinking", juce::String::fromUTF8(b.thinking.c_str()));
                if (!b.signature.empty())
                    bo->setProperty("signature", juce::String(b.signature));
            }
            else if (b.type == "server_tool_use")
            {
                // The API's own call: its input as it streamed (valid; kept as the API wrote it).
                juce::var input;
                if (b.json.empty() || !juce::JSON::parse(juce::String::fromUTF8(b.json.c_str()), input).wasOk()
                    || input.getDynamicObject() == nullptr)
                    input = b.object.getProperty("input", juce::var(new juce::DynamicObject()));
                bo->setProperty("input", input);
                if (cb_.serverInput)
                    cb_.serverInput(b.object.getProperty("name", "").toString().toStdString(), input);
            }
            else if (b.type == "compaction" && !b.summary.empty())
            {
                // A compaction that made no summary keeps the content it came with (null): never an empty string.
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
                usage_.output = static_cast<juce::int64>(u.getProperty("output_tokens", 0));
            if (u.hasProperty("input_tokens"))
                usage_.input = static_cast<juce::int64>(u.getProperty("input_tokens", 0));
            if (u.hasProperty("cache_read_input_tokens"))
                usage_.cacheRead = static_cast<juce::int64>(u.getProperty("cache_read_input_tokens", 0));
            if (u.hasProperty("cache_creation_input_tokens"))
                usage_.cacheWrite = static_cast<juce::int64>(u.getProperty("cache_creation_input_tokens", 0));
            if (u.hasProperty("speed"))
                usage_.speed = u.getProperty("speed", "").toString().toStdString();
            readUsageExtras(u);
            const juce::var server = u.getProperty("server_tool_use", juce::var());
            if (server.isObject())
            {
                usage_.webSearches = static_cast<int>(server.getProperty("web_search_requests", 0));
                usage_.webFetches = static_cast<int>(server.getProperty("web_fetch_requests", 0));
            }
            // With compaction on, the iterations list every sampling step: the top-level counts leave out a
            // compaction's, so the request's whole cost is their sum.
            const juce::var iterations = u.getProperty("iterations", juce::var());
            if (const auto* its = iterations.getArray(); its != nullptr && !its->isEmpty())
            {
                int64_t in = 0, out = 0;
                int compactions = 0;
                for (const auto& it : *its)
                {
                    in += static_cast<juce::int64>(it.getProperty("input_tokens", 0));
                    out += static_cast<juce::int64>(it.getProperty("output_tokens", 0));
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
                {
                    o->setProperty("text", juce::String::fromUTF8(b.text.c_str()));
                    if (!b.citations.isEmpty())
                        o->setProperty("citations", b.citations);
                }
                else if (b.type == "thinking")
                    o->setProperty("thinking", juce::String::fromUTF8(b.thinking.c_str()));
                else if (b.type == "compaction" && !b.summary.empty())
                    o->setProperty("content", juce::String::fromUTF8(b.summary.c_str()));
                out.add(b.object);
            }
        return juce::var(out);
    }
}
