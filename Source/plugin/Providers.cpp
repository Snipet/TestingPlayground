// Source/plugin/Providers.cpp: see Providers.h.
#include "plugin/Providers.h"

#include "fminus/Lang.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <map>
#include <set>
#include <utility>

namespace playground::providers
{
    namespace
    {
        std::string str(const juce::var& v, const char* key) { return v.getProperty(key, "").toString().toStdString(); }

        juce::String utf8(const std::string& s) { return juce::String::fromUTF8(s.data(), static_cast<int>(s.size())); }

        std::string lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        bool startsWith(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
        bool contains(std::string_view s, std::string_view p) { return s.find(p) != std::string_view::npos; }
        // `word` as one of the id's dash-separated parts ("pro" in "o3-pro" and "gpt-5-pro-2025-10-06").
        bool part(std::string_view id, std::string_view word)
        {
            for (std::size_t at = id.find(word); at != std::string_view::npos; at = id.find(word, at + 1))
                if ((at == 0 || id[at - 1] == '-') && (at + word.size() == id.size() || id[at + word.size()] == '-'))
                    return true;
            return false;
        }

        // A deep copy of a JSON value (juce::var shares its objects and arrays).
        juce::var copyOf(const juce::var& v) { return claude::parseJson(claude::toJson(v)); }

        juce::Array<juce::var> arrayOf(const juce::var& v)
        {
            if (const juce::Array<juce::var>* a = v.getArray())
                return *a;
            return {};
        }

        // A tool call's id, made here for every other provider's call: unique, and in the Messages API's alphabet.
        std::string newCallId() { return "call_" + juce::Uuid().toString().substring(0, 24).toStdString(); }

        // The host part of an authority, without its port ("localhost", "[::1]", "192.168.1.4").
        std::string hostOnly(const std::string& authority)
        {
            if (!authority.empty() && authority.front() == '[')
            {
                const std::size_t close = authority.find(']');
                return close == std::string::npos ? authority : authority.substr(0, close + 1);
            }
            const std::size_t colon = authority.rfind(':');
            return colon == std::string::npos ? authority : authority.substr(0, colon);
        }

        bool hexDigit(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
        bool ipv6Char(char c) { return hexDigit(c) || c == ':' || c == '.'; }

        // An authority this parser and every client read alike: a host and at most a port of decimal digits (1 to
        // 65535), no backslash (some clients read one as the path's start) and no '%' (an escape a '/' or '@' may hide
        // behind), a bracketed host closed and holding an IPv6 address's characters alone. False for any other, so that
        // the origin a key is bound to is the server it reaches.
        bool plainAuthority(const std::string& authority)
        {
            if (authority.find_first_of("\\%") != std::string::npos)
                return false;
            std::string host = authority, port;
            if (!authority.empty() && authority.front() == '[')
            {
                const std::size_t close = authority.find(']');
                if (close == std::string::npos || close < 3)
                    return false;
                host = authority.substr(0, close + 1);
                const std::string inside = authority.substr(1, close - 1);
                if (inside.find(':') == std::string::npos
                    || !std::all_of(inside.begin(), inside.end(), ipv6Char))
                    return false;
                const std::string rest = authority.substr(close + 1);
                if (!rest.empty() && rest.front() != ':')
                    return false;                  // "[::1].example.com"
                port = rest;
            }
            else if (const std::size_t colon = authority.find(':'); colon != std::string::npos)
            {
                host = authority.substr(0, colon);
                port = authority.substr(colon);
            }
            if (host.empty())
                return false;
            if (!port.empty())
            {
                const std::string digits = port.substr(1);
                if (digits.empty() || digits.size() > 5
                    || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
                    return false;
                const int n = std::atoi(digits.c_str());
                if (n < 1 || n > 65535)
                    return false;
            }
            return true;
        }

        // A host taken as this Mac's or the local network's (http:// allowed): a name of its own (localhost,
        // *.localhost, *.local), or an address read strictly, so that the address a resolver reaches is the one judged
        // here. A dotted IPv4 address is four decimal parts with no leading zero (inet_aton and the resolvers behind it
        // read "010" as octal 8, "0x7f" as hex, "127.1" and "2130706433" as short forms: each refused, never local);
        // an IPv6 one is [::1] or a first group of four hex digits in fe80::/10's fe80 or fc00::/7 (fc.., fd..).
        bool localHost(const std::string& host)
        {
            if (host == "localhost" || (host.size() > 10 && host.compare(host.size() - 10, 10, ".localhost") == 0)
                || (host.size() > 6 && host.compare(host.size() - 6, 6, ".local") == 0))
                return true;
            if (!host.empty() && host.front() == '[')
            {
                if (host.size() < 4 || host.back() != ']'
                    || !std::all_of(host.begin() + 1, host.end() - 1, ipv6Char))
                    return false;
                if (host == "[::1]")
                    return true;
                const std::string first = host.substr(1, host.find(':') == std::string::npos ? 0 : host.find(':') - 1);
                return first.size() == 4 && (first == "fe80" || startsWith(first, "fc") || startsWith(first, "fd"));
            }
            // a dotted IPv4 address
            int parts[4] = { -1, -1, -1, -1 };
            int n = 0;
            std::size_t p = 0;
            while (n < 4 && p <= host.size())
            {
                const std::size_t dot = std::min(host.find('.', p), host.size());
                const std::string part = host.substr(p, dot - p);
                if (part.empty() || part.size() > 3 || (part.size() > 1 && part.front() == '0')
                    || !std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; }))
                    return false;
                parts[n++] = std::atoi(part.c_str());
                p = dot + 1;
                if (dot == host.size())
                    break;
            }
            if (n != 4 || p <= host.size())
                return false;
            for (const int v : parts)
                if (v < 0 || v > 255)
                    return false;
            return parts[0] == 127 || parts[0] == 10 || (parts[0] == 192 && parts[1] == 168)
                || (parts[0] == 172 && parts[1] >= 16 && parts[1] <= 31) || (parts[0] == 169 && parts[1] == 254);
        }

        struct Url
        {
            std::string scheme, authority, path;
        };

        bool split(std::string_view base, Url& u)
        {
            const std::size_t s = base.find("://");
            if (s == std::string_view::npos)
                return false;
            u.scheme = lower(std::string(base.substr(0, s)));
            std::string_view rest = base.substr(s + 3);
            const std::size_t slash = rest.find('/');
            u.authority = lower(std::string(rest.substr(0, slash)));
            u.path = slash == std::string_view::npos ? std::string() : std::string(rest.substr(slash));
            return true;
        }
    }

    Kind kindOf(std::string_view id)
    {
        return id == "openai" ? Kind::openai : id == "gemini" ? Kind::gemini : Kind::anthropic;
    }

    const char* idOf(Kind kind)
    {
        switch (kind)
        {
            case Kind::openai: return "openai";
            case Kind::gemini: return "gemini";
            case Kind::anthropic: break;
        }
        return "anthropic";
    }

    // ---- servers ----------------------------------------------------------------------------------------------------------
    std::string canonical(std::string_view in, std::string& why)
    {
        std::string base(in);
        while (!base.empty() && (base.back() == ' ' || base.back() == '\n' || base.back() == '\t'))
            base.pop_back();
        while (!base.empty() && (base.front() == ' ' || base.front() == '\t'))
            base.erase(base.begin());
        Url u;
        if (base.empty() || !split(base, u) || (u.scheme != "https" && u.scheme != "http"))
        {
            why = "the server's address must start with https:// (or http:// for a server on this Mac or the local network)";
            return {};
        }
        if (u.authority.empty() || contains(u.authority, "@") || !plainAuthority(u.authority) || contains(base, "?")
            || contains(base, "#")
            || std::any_of(base.begin(), base.end(), [](unsigned char c) { return c <= ' ' || c >= 127; }))
        {
            why = "that is not a server's address (https://host/path, with no spaces, user, query or fragment)";
            return {};
        }
        const std::string host = hostOnly(u.authority);
        if (u.scheme == "http" && !localHost(host))
        {
            // An address of numbers alone that is not read as local: a leading zero or another form the system reads
            // otherwise (010 is octal 8 there) is said, so the user writes it plainly.
            const bool numbers = !host.empty() && host.find_first_not_of("0123456789.") == std::string::npos;
            why = numbers ? "http:// is only for a server on this Mac or the local network, its address four plain "
                            "numbers (192.168.1.20: no leading zeros), else use https://"
                          : "http:// is only for a server on this Mac or the local network: use https://";
            return {};
        }
        std::string path = u.path;
        while (!path.empty() && path.back() == '/')
            path.pop_back();
        return u.scheme + "://" + u.authority + path;
    }

    std::string originOf(std::string_view base)
    {
        Url u;
        return split(base, u) ? u.scheme + "://" + u.authority : std::string();
    }

    std::string pathOf(std::string_view base)
    {
        Url u;
        return split(base, u) ? u.path : std::string();
    }

    std::string hostOf(std::string_view base)
    {
        Url u;
        return split(base, u) ? u.authority : std::string(base);
    }

    bool isLocal(std::string_view base)
    {
        Url u;
        return split(base, u) && plainAuthority(u.authority) && localHost(hostOnly(u.authority));
    }

    std::string baseOf(const ChatProvider& p)
    {
        switch (kindOf(p.kind))
        {
            case Kind::anthropic: return claude::kHost;
            case Kind::gemini: return kGeminiBase;
            case Kind::openai: break;
        }
        std::string why;
        return canonical(p.base, why);
    }

    const ChatPreset* presetOf(const ChatProvider& p)
    {
        const Kind k = kindOf(p.kind);
        for (const ChatPreset& preset : kChatPresets)
        {
            if (kindOf(preset.kind) != k || std::string_view(preset.id) == "custom")
                continue;
            if (k != Kind::openai || baseOf(p) == preset.base)
                return &preset;
        }
        return nullptr;
    }

    std::string nameOf(const ChatProvider& p)
    {
        if (const ChatPreset* preset = presetOf(p))
            return preset->name;
        const std::string base = baseOf(p);
        return base.empty() ? std::string("a server") : hostOf(base);
    }

    bool keyless(const ChatProvider& p) { return kindOf(p.kind) == Kind::openai && isLocal(baseOf(p)); }

    bool picksFirst(const ChatProvider& p) { return kindOf(p.kind) == Kind::gemini || keyless(p); }

    bool plausibleKey(const ChatProvider& p, std::string_view key, std::string& why)
    {
        if (kindOf(p.kind) != Kind::anthropic && startsWith(key, "sk-ant-"))
        {
            why = "that is an Anthropic key: it goes only to Anthropic, never to " + nameOf(p);
            return false;
        }
        if (key.size() < 8 || key.size() > 400
            || !std::all_of(key.begin(), key.end(), [](unsigned char c) { return c > ' ' && c < 127; }))
        {
            why = "that is not an API key (8 to 400 characters, no spaces)";
            return false;
        }
        return true;
    }

    bool plausibleModel(std::string_view model)
    {
        return !model.empty() && model.size() <= 200
            && std::all_of(model.begin(), model.end(), [](unsigned char c) { return c > ' ' && c < 127; });
    }

    // ---- requests ---------------------------------------------------------------------------------------------------------
    namespace
    {
        // The system prompt's text (the Messages API's system: a string or text blocks).
        std::string systemText(const juce::var& body)
        {
            const juce::var system = body.getProperty("system", juce::var());
            if (system.isString())
                return system.toString().toStdString();
            std::string out;
            for (const juce::var& b : arrayOf(system))
                out += (out.empty() ? "" : "\n\n") + str(b, "text");
            return out;
        }

        // A tool result's text: its content a string, or blocks (text; pictures counted apart).
        std::string resultText(const juce::var& block)
        {
            const juce::var c = block.getProperty("content", juce::var());
            if (c.isString())
                return c.toString().toStdString();
            std::string out;
            for (const juce::var& x : arrayOf(c))
                if (str(x, "type") == "text")
                    out += (out.empty() ? "" : "\n") + str(x, "text");
            return out;
        }

        std::vector<juce::var> resultImages(const juce::var& block)
        {
            std::vector<juce::var> out;
            for (const juce::var& x : arrayOf(block.getProperty("content", juce::var())))
                if (str(x, "type") == "image")
                    out.push_back(x);
            return out;
        }

        constexpr const char* kNoPictures = "(A picture was here: this model takes no pictures, so it cannot see it.)";
        constexpr const char* kToolPictures = "(The pictures your last tool calls returned, in order:)";
        constexpr const char* kSummaryStart = "(The conversation continues from a summary of what came before.)";

        // A compaction block's summary, as text another model reads.
        std::string summaryText(const juce::var& block)
        {
            return "(A summary of the conversation before this point:)\n" + str(block, "content");
        }

        // The tools another provider is given: the plugin's own (a server tool of Anthropic's, web search, has a type).
        std::vector<juce::var> functionTools(const juce::var& body)
        {
            std::vector<juce::var> out;
            for (const juce::var& t : arrayOf(body.getProperty("tools", juce::var())))
                if (!t.hasProperty("type") && t.hasProperty("input_schema"))
                    out.push_back(t);
            return out;
        }

        // OpenAI's schemas: an array's items said (it refuses an array without them; {} is any value).
        void withItems(juce::var& schema)
        {
            auto* o = schema.getDynamicObject();
            if (o == nullptr)
                return;
            if (o->getProperty("type").toString() == "array" && !o->hasProperty("items"))
                o->setProperty("items", juce::var(new juce::DynamicObject()));
            for (const char* key : { "items", "additionalProperties" })
                if (juce::var inner = o->getProperty(key); inner.isObject())
                    withItems(inner);
            if (auto* props = o->getProperty("properties").getDynamicObject())
                for (auto& kv : props->getProperties())
                {
                    juce::var p = kv.value;
                    withItems(p);
                }
            for (const char* key : { "anyOf", "oneOf", "allOf" })
                if (juce::Array<juce::var>* alts = o->getProperty(key).getArray())
                    for (juce::var& a : *alts)
                        withItems(a);
        }

        // An array whose items its schema leaves open (none, or {}: any value), which Gemini's subset cannot say:
        // run_function's args, write_file's files, edit_file's edits and files.
        bool openArray(const juce::var& schema)
        {
            const juce::var items = schema.getProperty("items", juce::var());
            return lower(schema.getProperty("type", "").toString().toStdString()) == "array"
                && (!items.isObject() || items.getDynamicObject()->getProperties().isEmpty());
        }

        // Gemini's own schema (its OpenAPI subset, the fallback): types in upper case, what it lacks left out (additional
        // properties; an alternative of several, the first with a type kept), an enum of words with no type (the tools
        // leave it out, as JSON Schema allows: the projects tool's action) a STRING, an array of open items one of
        // STRINGs, each a JSON value written out (fromSubset reads them back), and a property it cannot describe (an
        // object with no properties, an array of such) left out with its place in required. Void: nothing left.
        juce::var geminiSubset(const juce::var& schema)
        {
            const auto* o = schema.getDynamicObject();
            if (o == nullptr)
                return {};
            juce::var s = schema;
            if (!o->hasProperty("type"))
            {
                const juce::Array<juce::var> values = arrayOf(o->getProperty("enum"));
                if (!values.isEmpty() && std::all_of(values.begin(), values.end(), [](const juce::var& v) { return v.isString(); }))
                {
                    juce::var typed = copyOf(schema);
                    typed.getDynamicObject()->setProperty("type", "string");
                    return geminiSubset(typed);
                }
                for (const char* key : { "anyOf", "oneOf" })
                    for (const juce::var& alt : arrayOf(o->getProperty(key)))
                        if (alt.hasProperty("type"))
                        {
                            juce::var picked = copyOf(alt);
                            if (o->hasProperty("description") && !picked.hasProperty("description"))
                                picked.getDynamicObject()->setProperty("description", o->getProperty("description"));
                            return geminiSubset(picked);
                        }
                return {};
            }
            const std::string type = lower(s.getProperty("type", "").toString().toStdString());
            auto* out = new juce::DynamicObject();
            juce::var result(out);
            out->setProperty("type", juce::String(type).toUpperCase());
            for (const char* key : { "description", "enum", "minItems", "maxItems", "minimum", "maximum", "nullable" })
                if (o->hasProperty(key))
                    out->setProperty(key, copyOf(o->getProperty(key)));
            if (type == "object")
            {
                auto* props = new juce::DynamicObject();
                std::set<std::string> kept;
                if (const auto* in = o->getProperty("properties").getDynamicObject())
                    for (const auto& kv : in->getProperties())
                        if (const juce::var p = geminiSubset(kv.value); !p.isVoid())
                        {
                            props->setProperty(kv.name, p);
                            kept.insert(kv.name.toString().toStdString());
                        }
                if (kept.empty())
                {
                    delete props;
                    return {};
                }
                out->setProperty("properties", juce::var(props));
                juce::Array<juce::var> required;
                for (const juce::var& r : arrayOf(o->getProperty("required")))
                    if (kept.count(r.toString().toStdString()) > 0)
                        required.add(r);
                if (!required.isEmpty())
                    out->setProperty("required", required);
            }
            else if (openArray(schema))
            {
                out->setProperty("items", claude::object({ { "type", "STRING" } }));
                const juce::String said = o->getProperty("description").toString();
                out->setProperty("description", said + (said.isEmpty() ? "" : " ") + "(each item a JSON value written out: 0.5, "
                                                                                     "true, [1, 2], {\"name\": ...})");
            }
            else if (type == "array")
            {
                const juce::var items = geminiSubset(o->getProperty("items"));
                if (items.isVoid())
                    return {};
                out->setProperty("items", items);
            }
            return result;
        }

        // A model's id as Gemini's path names it: its own characters ("models/" taken off).
        std::string geminiModel(std::string m)
        {
            if (startsWith(m, "models/"))
                m.erase(0, 7);
            m.erase(std::remove_if(m.begin(), m.end(),
                                   [](unsigned char c) { return !(std::isalnum(c) != 0 || c == '.' || c == '-' || c == '_'); }),
                    m.end());
            return m;
        }

        // ---- thinking levels (Providers.h, levelsOf) ----
        // The words every provider here takes, in the menu's order (Anthropic's own run low to max the same way).
        constexpr const char* kLevels[] = { "none", "minimal", "low", "medium", "high", "xhigh", "max" };

        bool isLevel(std::string_view e)
        {
            return std::any_of(std::begin(kLevels), std::end(kLevels), [e](const char* l) { return e == l; });
        }

        void setLevels(ChatModel& m, std::vector<std::string> levels, const char* byDefault)
        {
            m.efforts = std::move(levels);
            m.defaultEffort = byDefault;
        }

        // OpenAI's own models by their family (its /models says nothing of reasoning). A level set only where its API
        // reference ties it to the family: minimal came with gpt-5, none with gpt-5.1 (its default there), xhigh with
        // the models after gpt-5.1-codex-max; any other gpt-5 or later gets the three every reasoning model takes.
        void openAiLevels(const std::string& id, ChatModel& m)
        {
            const std::string s = lower(id);
            if (contains(s, "-chat") || startsWith(s, "o1-mini") || startsWith(s, "o1-preview"))
                return;                                   // no reasoning_effort there
            if (s.size() >= 2 && s[0] == 'o' && (s[1] == '1' || s[1] == '3' || s[1] == '4') && (s.size() == 2 || s[2] == '-'))
            {
                setLevels(m, { "low", "medium", "high" }, "medium");
                return;
            }
            if (!startsWith(s, "gpt-"))
                return;
            // gpt-<major>[.<minor>], then the id's end or a dash: gpt-4o, gpt-4.1 and the like are no reasoning models.
            std::size_t at = 4;
            const auto number = [&s, &at](int& n) {
                const std::size_t from = at;
                while (at < s.size() && at - from < 4 && std::isdigit(static_cast<unsigned char>(s[at])) != 0)
                    n = n * 10 + (s[at++] - '0');
                return at > from;
            };
            int major = 0, minor = 0;
            if (!number(major))
                return;
            if (at < s.size() && s[at] == '.')
            {
                ++at;
                if (!number(minor))
                    return;
            }
            if ((at < s.size() && s[at] != '-') || major < 5)
                return;
            if (major == 5 && minor == 0)
                setLevels(m, { "minimal", "low", "medium", "high" }, "medium");
            else if (major == 5 && minor == 1)
                setLevels(m, { "none", "low", "medium", "high" }, "none");
            else if (major == 5 && minor == 2)
                setLevels(m, { "none", "low", "medium", "high", "xhigh" }, "");
            else
                setLevels(m, { "low", "medium", "high" }, "");
        }

        // OpenRouter's word on a model's thinking, its list's "reasoning" object (none: the model does not reason, no
        // levels): supported_efforts, in the menu's order, Off ("none") left out when reasoning is mandatory (it cannot
        // be turned off); default_effort, or Off when default_enabled is false (it does not reason unless asked) and Off
        // is offered.
        void openRouterLevels(const juce::var& r, ChatModel& m)
        {
            if (!r.isObject())
                return;
            const bool mandatory = static_cast<bool>(r.getProperty("mandatory", false));
            std::set<std::string> said;
            for (const juce::var& e : arrayOf(r.getProperty("supported_efforts", juce::var())))
                if (const std::string level = e.toString().toStdString(); isLevel(level) && !(mandatory && level == "none"))
                    said.insert(level);
            std::vector<std::string> levels;
            for (const char* l : kLevels)
                if (said.count(l) > 0)
                    levels.emplace_back(l);
            if (levels.empty())
                return;
            std::string byDefault = str(r, "default_effort");
            const juce::var enabled = r.getProperty("default_enabled", juce::var());
            if (enabled.isBool() && !static_cast<bool>(enabled) && said.count("none") > 0)
                byDefault = "none";
            if (said.count(byDefault) == 0)
                byDefault.clear();
            m.efforts = std::move(levels);
            m.defaultEffort = byDefault;
        }

        // Gemini's version by its id ("gemini-2.5-flash" 2.5, "gemini-3-pro-preview" 3): 0 for an alias that names none.
        double geminiVersion(const std::string& id)
        {
            const std::string s = lower(geminiModel(id));
            if (!startsWith(s, "gemini-") || s.size() < 8 || std::isdigit(static_cast<unsigned char>(s[7])) == 0)
                return 0.0;
            return std::atof(s.c_str() + 7);
        }

        // Gemini's models by their family: Gemini 3 and later take thinkingLevel (pro low and high, flash minimal to
        // high, high by default; neither turns thinking off); 2.5 takes thinkingBudget (Off, a budget of 0, on flash and
        // flash-lite alone; flash-lite off by default, the others thinking as much as they decide: no level).
        void geminiLevels(const std::string& id, ChatModel& m)
        {
            const std::string s = lower(geminiModel(id));
            const double version = geminiVersion(s);
            const bool lite = contains(s, "-lite"), flash = contains(s, "flash");
            if (version >= 3.0)
                setLevels(m, flash && !lite ? std::vector<std::string>{ "minimal", "low", "medium", "high" }
                                            : std::vector<std::string>{ "low", "high" },
                          "high");
            else if (version >= 2.5 && flash)
                setLevels(m, { "none", "low", "medium", "high" }, lite ? "none" : "");
            else if (version >= 2.5 && part(s, "pro"))
                setLevels(m, { "low", "medium", "high" }, "");
        }

        // Gemini 2.5's budgets for the levels: Google's own mapping of reasoning_effort on its OpenAI-compatible
        // endpoint (low 1,024 tokens, medium 8,192, high 24,576), and none 0.
        int geminiBudget(const std::string& effort)
        {
            return effort == "none" ? 0 : effort == "low" ? 1024 : effort == "medium" ? 8192 : 24576;
        }

        // Gemini's thinkingConfig for `effort` on `model`: thinkingLevel (Gemini 3 and later) or thinkingBudget (2.5),
        // never both, and its thoughts asked for (includeThoughts) unless thinking is off. Void for none, or a level the
        // model does not take.
        juce::var geminiThinking(const std::string& model, const std::string& effort)
        {
            ChatModel m;
            geminiLevels(model, m);
            if (effort.empty() || std::find(m.efforts.begin(), m.efforts.end(), effort) == m.efforts.end())
                return {};
            auto* o = new juce::DynamicObject();
            juce::var config(o);
            if (geminiVersion(model) >= 3.0)
                o->setProperty("thinkingLevel", juce::String(effort));
            else
                o->setProperty("thinkingBudget", geminiBudget(effort));
            if (effort != "none")
                o->setProperty("includeThoughts", true);
            return config;
        }

        // ---- OpenAI's Chat Completions ----
        juce::var openAiImage(const juce::var& image)
        {
            const juce::var src = image.getProperty("source", juce::var());
            const juce::String url = "data:" + src.getProperty("media_type", "image/png").toString() + ";base64,"
                                   + src.getProperty("data", "").toString();
            return claude::object({ { "type", "image_url" }, { "image_url", claude::object({ { "url", url } }) } });
        }

        // A user message's parts: a string when they are text alone (every server takes that), else parts.
        juce::var openAiContent(const juce::Array<juce::var>& parts)
        {
            bool textOnly = true;
            std::string joined;
            for (const juce::var& p : parts)
            {
                textOnly = textOnly && str(p, "type") == "text";
                joined += (joined.empty() ? "" : "\n\n") + str(p, "text");
            }
            if (textOnly)
                return utf8(joined);
            return parts;
        }

        juce::var openAiMessages(const juce::var& body, const Options& options)
        {
            juce::Array<juce::var> out;
            out.add(claude::object({ { "role", "system" }, { "content", utf8(systemText(body)) } }));
            for (const juce::var& m : arrayOf(body.getProperty("messages", juce::var())))
            {
                const std::string role = str(m, "role");
                const juce::var content = m.getProperty("content", juce::var());
                juce::Array<juce::var> blocks = arrayOf(content);
                if (content.isString())
                    blocks.add(claude::object({ { "type", "text" }, { "text", content } }));
                if (role == "user")
                {
                    juce::Array<juce::var> parts, pictures;
                    for (const juce::var& b : blocks)
                    {
                        const std::string type = str(b, "type");
                        if (type == "tool_result")
                        {
                            std::string t = resultText(b);
                            if (static_cast<bool>(b.getProperty("is_error", false)))
                                t = "Error: " + t;
                            const std::vector<juce::var> images = resultImages(b);
                            if (!images.empty() && !options.images)
                                t += std::string(t.empty() ? "" : "\n") + kNoPictures;
                            if (options.images)
                                for (const juce::var& image : images)
                                    pictures.add(openAiImage(image));
                            out.add(claude::object({ { "role", "tool" },
                                                     { "tool_call_id", b.getProperty("tool_use_id", "") },
                                                     { "content", utf8(t.empty() ? std::string("(no output)") : t) } }));
                        }
                        else if (type == "text")
                            parts.add(claude::object({ { "type", "text" }, { "text", b.getProperty("text", "") } }));
                        else if (type == "image")
                            parts.add(options.images ? openAiImage(b) : claude::object({ { "type", "text" }, { "text", kNoPictures } }));
                    }
                    // A tool's pictures go after every tool message (they come before any other message), with a line.
                    if (!pictures.isEmpty())
                    {
                        pictures.insert(0, claude::object({ { "type", "text" }, { "text", kToolPictures } }));
                        pictures.addArray(parts);
                        parts = pictures;
                    }
                    if (!parts.isEmpty())
                        out.add(claude::object({ { "role", "user" }, { "content", openAiContent(parts) } }));
                }
                else if (role == "assistant")
                {
                    std::string t;
                    juce::Array<juce::var> calls;
                    for (const juce::var& b : blocks)
                    {
                        const std::string type = str(b, "type");
                        if (type == "text")
                            t += (t.empty() ? "" : "\n\n") + str(b, "text");
                        else if (type == "compaction")
                            t += (t.empty() ? "" : "\n\n") + summaryText(b);
                        else if (type == "tool_use")
                            calls.add(claude::object(
                                { { "id", b.getProperty("id", "") },
                                  { "type", "function" },
                                  { "function", claude::object({ { "name", b.getProperty("name", "") },
                                                                 { "arguments", utf8(claude::toJson(b.getProperty("input", juce::var(new juce::DynamicObject())))) } }) } }));
                    }
                    juce::var msg = claude::object({ { "role", "assistant" } });
                    if (calls.isEmpty())
                        msg.getDynamicObject()->setProperty("content", utf8(t.empty() ? std::string("(no answer)") : t));
                    else
                    {
                        msg.getDynamicObject()->setProperty("content", t.empty() ? juce::var() : juce::var(utf8(t)));
                        msg.getDynamicObject()->setProperty("tool_calls", calls);
                    }
                    out.add(msg);
                }
            }
            return out;
        }

        std::string openAiBody(const juce::var& body, const Options& options, Reasoning reasoning)
        {
            juce::Array<juce::var> tools;
            for (const juce::var& t : functionTools(body))
            {
                juce::var schema = copyOf(t.getProperty("input_schema", juce::var()));
                withItems(schema);
                tools.add(claude::object({ { "type", "function" },
                                           { "function", claude::object({ { "name", t.getProperty("name", "") },
                                                                          { "description", t.getProperty("description", "") },
                                                                          { "parameters", schema } }) } }));
            }
            juce::var out = claude::object({ { "model", body.getProperty("model", "") },
                                             { "stream", true },
                                             { "stream_options", claude::object({ { "include_usage", true } }) },
                                             { "messages", openAiMessages(body, options) } });
            if (!tools.isEmpty())
                out.getDynamicObject()->setProperty("tools", tools);
            // The thinking level, chosen: OpenRouter's effort alone (never a max_tokens beside it), OpenAI's own field.
            if (isLevel(options.effort) && reasoning == Reasoning::openRouter)
                out.getDynamicObject()->setProperty("reasoning", claude::object({ { "effort", juce::String(options.effort) } }));
            else if (isLevel(options.effort) && reasoning == Reasoning::openAi)
                out.getDynamicObject()->setProperty("reasoning_effort", juce::String(options.effort));
            return claude::toJson(out);
        }

        // ---- Gemini's generateContent ----
        juce::var geminiImage(const juce::var& image)
        {
            const juce::var src = image.getProperty("source", juce::var());
            return claude::object({ { "inlineData", claude::object({ { "mimeType", src.getProperty("media_type", "image/png") },
                                                                     { "data", src.getProperty("data", "") } }) } });
        }

        juce::var textPart(const std::string& t) { return claude::object({ { "text", utf8(t) } }); }

        juce::var geminiContents(const juce::var& body, const Options& options)
        {
            juce::Array<juce::var> out;
            std::map<std::string, std::string> names;            // a call's id -> its tool's name
            const auto add = [&out](const std::string& role, juce::Array<juce::var> parts) {
                if (parts.isEmpty())
                    parts.add(textPart("(no answer)"));
                if (!out.isEmpty() && str(out.getReference(out.size() - 1), "role") == role)
                {
                    juce::Array<juce::var> merged = arrayOf(out.getReference(out.size() - 1).getProperty("parts", juce::var()));
                    merged.addArray(parts);
                    out.getReference(out.size() - 1).getDynamicObject()->setProperty("parts", merged);
                    return;
                }
                out.add(claude::object({ { "role", juce::String(role) }, { "parts", parts } }));
            };
            for (const juce::var& m : arrayOf(body.getProperty("messages", juce::var())))
            {
                const std::string role = str(m, "role");
                const juce::var content = m.getProperty("content", juce::var());
                juce::Array<juce::var> blocks = arrayOf(content);
                if (content.isString())
                    blocks.add(claude::object({ { "type", "text" }, { "text", content } }));
                juce::Array<juce::var> parts;
                if (role == "user")
                {
                    juce::Array<juce::var> responses, pictures;
                    for (const juce::var& b : blocks)
                    {
                        const std::string type = str(b, "type");
                        if (type == "tool_result")
                        {
                            std::string t = resultText(b);
                            const std::vector<juce::var> images = resultImages(b);
                            if (!images.empty() && !options.images)
                                t += std::string(t.empty() ? "" : "\n") + kNoPictures;
                            if (options.images)
                                for (const juce::var& image : images)
                                    pictures.add(geminiImage(image));
                            const auto it = names.find(str(b, "tool_use_id"));
                            const bool error = static_cast<bool>(b.getProperty("is_error", false));
                            responses.add(claude::object(
                                { { "functionResponse",
                                    claude::object({ { "name", utf8(it != names.end() ? it->second : std::string("tool")) },
                                                     { "response", claude::object({ { error ? "error" : "result",
                                                                                      utf8(t.empty() ? std::string("(no output)") : t) } }) } }) } }));
                        }
                        else if (type == "text")
                            parts.add(textPart(str(b, "text")));
                        else if (type == "image")
                            parts.add(options.images ? geminiImage(b) : textPart(kNoPictures));
                    }
                    if (!pictures.isEmpty())
                        pictures.insert(0, textPart(kToolPictures));
                    responses.addArray(pictures);
                    responses.addArray(parts);
                    add("user", responses);
                }
                else if (role == "assistant")
                {
                    for (const juce::var& b : blocks)
                    {
                        const std::string type = str(b, "type");
                        if (type == "text" && !str(b, "text").empty())
                            parts.add(textPart(str(b, "text")));
                        else if (type == "compaction")
                            parts.add(textPart(summaryText(b)));
                        else if (type == "tool_use")
                        {
                            names[str(b, "id")] = str(b, "name");
                            juce::var call = claude::object(
                                { { "functionCall", claude::object({ { "name", b.getProperty("name", "") },
                                                                     { "args", b.getProperty("input", juce::var(new juce::DynamicObject())) } }) } });
                            if (const std::string sig = str(b, "thought_signature"); !sig.empty())
                                call.getDynamicObject()->setProperty("thoughtSignature", utf8(sig));
                            parts.add(call);
                        }
                    }
                    add("model", parts);
                }
            }
            // The first turn must be the user's (a history that starts at a summary starts with Claude's).
            if (!out.isEmpty() && str(out.getReference(0), "role") != "user")
                out.insert(0, claude::object({ { "role", "user" }, { "parts", juce::Array<juce::var>{ textPart(kSummaryStart) } } }));
            return out;
        }

        std::string geminiBody(const juce::var& body, const Options& options)
        {
            juce::Array<juce::var> declarations;
            for (const juce::var& t : functionTools(body))
            {
                juce::var d = claude::object({ { "name", t.getProperty("name", "") }, { "description", t.getProperty("description", "") } });
                if (options.jsonSchema)
                    d.getDynamicObject()->setProperty("parametersJsonSchema", copyOf(t.getProperty("input_schema", juce::var())));
                else if (const juce::var p = geminiSubset(t.getProperty("input_schema", juce::var())); !p.isVoid())
                    d.getDynamicObject()->setProperty("parameters", p);
                declarations.add(d);
            }
            juce::var out = claude::object(
                { { "systemInstruction", claude::object({ { "parts", juce::Array<juce::var>{ textPart(systemText(body)) } } }) },
                  { "contents", geminiContents(body, options) } });
            if (!declarations.isEmpty())
                out.getDynamicObject()->setProperty("tools", juce::Array<juce::var>{ claude::object({ { "functionDeclarations", declarations } }) });
            if (const juce::var thinking = geminiThinking(str(body, "model"), options.effort); !thinking.isVoid())
                out.getDynamicObject()->setProperty("generationConfig", claude::object({ { "thinkingConfig", thinking } }));
            return claude::toJson(out);
        }
    }

    juce::var fromSubset(const juce::var& input, const juce::var& schema)
    {
        const auto* props = schema.getProperty("properties", juce::var()).getDynamicObject();
        if (props == nullptr || !input.isObject())
            return input;
        juce::var out = copyOf(input);
        for (const auto& kv : props->getProperties())
        {
            juce::var p = kv.value;                      // as geminiSubset reads it: an alternative's first with a type
            if (!p.hasProperty("type"))
                for (const char* key : { "anyOf", "oneOf" })
                    for (const juce::var& alt : arrayOf(p.getProperty(key, juce::var())))
                        if (!p.hasProperty("type") && alt.hasProperty("type"))
                            p = alt;
            const juce::Array<juce::var>* given = out.getProperty(kv.name, juce::var()).getArray();
            if (!openArray(p) || given == nullptr)
                continue;
            juce::Array<juce::var> read;
            for (const juce::var& e : *given)
            {
                const juce::var v = e.isString() ? juce::JSON::fromString(e.toString().trim()) : juce::var();   // any value
                read.add(v.isVoid() ? e : v);
            }
            out.getDynamicObject()->setProperty(kv.name, read);
        }
        return out;
    }

    std::string body(const ChatProvider& p, const juce::var& messagesApi, const Options& options)
    {
        switch (kindOf(p.kind))
        {
            case Kind::openai: return openAiBody(messagesApi, options, reasoningOf(p));
            case Kind::gemini: return geminiBody(messagesApi, options);
            case Kind::anthropic: break;
        }
        return claude::toJson(messagesApi);
    }

    Endpoint endpoint(const ChatProvider& p, const std::string& model)
    {
        switch (kindOf(p.kind))
        {
            case Kind::openai:
            {
                const std::string base = baseOf(p);
                return { originOf(base), pathOf(base) + "/chat/completions" };
            }
            case Kind::gemini: return { kGeminiBase, "/v1beta/models/" + geminiModel(model) + ":streamGenerateContent?alt=sse" };
            case Kind::anthropic: break;
        }
        return { claude::kHost, "/v1/messages" };
    }

    claude::Headers headers(const ChatProvider& p, const std::string& key)
    {
        claude::Headers h;
        if (key.empty())
            return h;
        if (kindOf(p.kind) == Kind::gemini)
            h.emplace_back("x-goog-api-key", key);
        else
            h.emplace_back("Authorization", "Bearer " + key);
        return h;
    }

    Endpoint modelsEndpoint(const ChatProvider& p)
    {
        if (kindOf(p.kind) == Kind::gemini)
            return { kGeminiBase, "/v1beta/models?pageSize=1000" };
        const std::string base = baseOf(p);
        return { originOf(base), pathOf(base) + "/models" };
    }

    ChatModel plainModel(const std::string& id)
    {
        ChatModel m;
        m.id = id;
        m.name = id;
        m.maxTokens = 64000;
        m.web = false;
        return m;
    }

    ChatModel plainModel(const ChatProvider& p, const std::string& id)
    {
        ChatModel m = plainModel(id);
        levelsOf(p, m);
        return m;
    }

    Reasoning reasoningOf(const ChatProvider& p)
    {
        switch (kindOf(p.kind))
        {
            case Kind::gemini: return Reasoning::gemini;
            case Kind::anthropic: return Reasoning::none;
            case Kind::openai: break;
        }
        const std::string host = hostOf(baseOf(p));
        return host == "openrouter.ai" ? Reasoning::openRouter : host == "api.openai.com" ? Reasoning::openAi : Reasoning::none;
    }

    void levelsOf(const ChatProvider& p, ChatModel& m)
    {
        m.efforts.clear();
        m.defaultEffort.clear();
        switch (reasoningOf(p))
        {
            case Reasoning::openAi: openAiLevels(m.id, m); break;
            case Reasoning::gemini: geminiLevels(m.id, m); break;
            case Reasoning::openRouter:                   // its list says (modelsOf)
            case Reasoning::none: break;
        }
    }

    bool aboutEffort(const ChatProvider& p, int status, const std::string& error)
    {
        if ((status != 400 && status != 422) || aboutModel(status, error))
            return false;
        const std::string e = lower(error);
        const auto any = [&e](std::initializer_list<const char*> words) {
            return std::any_of(words.begin(), words.end(), [&e](const char* w) { return contains(e, w); });
        };
        switch (reasoningOf(p))
        {
            case Reasoning::openAi:
            case Reasoning::openRouter:
                // Its own name; Anthropic's words, through OpenRouter, for thinking turned on with a history that keeps
                // none of it (the reasoning is never kept: a tool call's next request); or OpenAI's words for a parameter
                // the model does not take (never a picture's refusal).
                return any({ "reasoning_effort", "reasoning.effort", "'reasoning'", "\"reasoning\"", "reasoning is not",
                             "thinking block", "redacted_thinking", "when `thinking` is enabled" })
                    || (any({ "unsupported parameter", "not supported with this model" }) && !any({ "image", "vision" }));
            case Reasoning::gemini:
                return any({ "thinking_level", "thinkinglevel", "thinking level", "thinking_budget", "thinkingbudget",
                             "thinking budget", "thinking_config", "thinkingconfig", "include_thoughts", "includethoughts",
                             "thinking is not", "thinking isn't" });
            case Reasoning::none: break;
        }
        return false;
    }

    std::vector<ChatModel> modelsOf(const ChatProvider& p, const std::string& body)
    {
        const juce::var v = claude::parseJson(body);
        std::vector<std::pair<int64_t, ChatModel>> found;     // (when it was made, the model)
        if (kindOf(p.kind) == Kind::gemini)
        {
            for (const juce::var& m : arrayOf(v.getProperty("models", juce::var())))
            {
                const std::string id = geminiModel(str(m, "name"));
                bool generates = false;
                for (const juce::var& g : arrayOf(m.getProperty("supportedGenerationMethods", juce::var())))
                    generates = generates || g.toString() == "generateContent";
                // Gemini's chat models: not its speech, image, embedding or live (audio) ones.
                if (!generates || !startsWith(id, "gemini-") || contains(id, "tts") || contains(id, "image")
                    || contains(id, "embedding") || contains(id, "live") || contains(id, "audio"))
                    continue;
                ChatModel cm = plainModel(id);
                if (const std::string name = str(m, "displayName"); !name.empty())
                    cm.name = name;
                cm.context = static_cast<int>(m.getProperty("inputTokenLimit", 0));
                if (const int out = static_cast<int>(m.getProperty("outputTokenLimit", 0)); out > 0)
                    cm.maxTokens = out;
                // Its thinking levels by its family, unless the list says it does not think ("thinking": false).
                if (const juce::var thinks = m.getProperty("thinking", juce::var()); !thinks.isBool() || static_cast<bool>(thinks))
                    geminiLevels(id, cm);
                // The newest version first ("gemini-3-pro" before "gemini-2.5-flash"), then pro, flash, lite, a stable
                // one before its preview.
                const double version = std::atof(id.c_str() + 7);
                const int tier = contains(id, "lite") ? 0 : contains(id, "flash") ? 1 : 2;
                const int stable = contains(id, "preview") || contains(id, "exp") ? 0 : 1;
                found.emplace_back(static_cast<int64_t>(version * 1000.0) * 100 + tier * 10 + stable, std::move(cm));
            }
        }
        else
        {
            const Reasoning reasoning = reasoningOf(p);
            const bool openAi = reasoning == Reasoning::openAi;
            for (const juce::var& m : arrayOf(v.getProperty("data", juce::var())))
            {
                const std::string id = str(m, "id");
                if (!plausibleModel(id) || contains(lower(id), "embed"))
                    continue;
                // OpenAI's own list holds its speech, image, search and embedding models too, and models this chat
                // cannot use: those served by the Responses API alone (the -pro ones, codex, deep research) and those
                // that call no tools (chatgpt-*, *-chat-latest, o1-mini, o1-preview). Its chat models with tools alone.
                if (openAi && (!(startsWith(id, "gpt-") || startsWith(id, "o1") || startsWith(id, "o3") || startsWith(id, "o4"))
                               || contains(id, "audio") || contains(id, "realtime") || contains(id, "tts")
                               || contains(id, "transcribe") || contains(id, "image") || contains(id, "search")
                               || contains(id, "instruct") || contains(id, "vision") || part(id, "pro") || contains(id, "codex")
                               || contains(id, "deep-research") || contains(id, "chat-latest") || startsWith(id, "o1-mini")
                               || startsWith(id, "o1-preview")))
                    continue;
                // OpenRouter's says which take tools: a model that does not cannot work here.
                const juce::var params = m.getProperty("supported_parameters", juce::var());
                if (const juce::Array<juce::var>* list = params.getArray())
                    if (std::none_of(list->begin(), list->end(), [](const juce::var& x) { return x.toString() == "tools"; }))
                        continue;
                ChatModel cm = plainModel(id);
                if (const std::string name = str(m, "name"); !name.empty())
                    cm.name = name;
                for (const char* key : { "context_length", "context_window", "max_context_length", "max_model_len" })
                    if (const int c = static_cast<int>(m.getProperty(key, 0)); c > 0)
                    {
                        cm.context = c;
                        break;
                    }
                const juce::var modalities = m.getProperty("architecture", juce::var()).getProperty("input_modalities", juce::var());
                if (const juce::Array<juce::var>* list = modalities.getArray())
                    cm.images = std::any_of(list->begin(), list->end(), [](const juce::var& x) { return x.toString() == "image"; });
                if (openAi)
                    openAiLevels(id, cm);
                else if (reasoning == Reasoning::openRouter)
                    openRouterLevels(m.getProperty("reasoning", juce::var()), cm);
                found.emplace_back(static_cast<juce::int64>(m.getProperty("created", 0)), std::move(cm));
            }
        }
        std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<ChatModel> out;
        for (auto& f : found)
            out.push_back(std::move(f.second));
        return out;
    }

    // ---- answers ----------------------------------------------------------------------------------------------------------
    namespace
    {
        // The Messages API's events, written for a MessageBuilder.
        class Events
        {
        public:
            explicit Events(Sink sink) : sink_(std::move(sink)) {}

            bool started() const noexcept { return started_; }

            void start(const std::string& model)
            {
                if (started_)
                    return;
                started_ = true;
                emit("message_start",
                     claude::object({ { "type", "message_start" },
                                      { "message", claude::object({ { "type", "message" },
                                                                    { "role", "assistant" },
                                                                    { "model", utf8(model) },
                                                                    { "content", juce::Array<juce::var>() },
                                                                    { "usage", claude::object({ { "input_tokens", 0 }, { "output_tokens", 0 } }) } }) } }));
            }

            int open(const juce::var& block)
            {
                const int i = next_++;
                emit("content_block_start", claude::object({ { "type", "content_block_start" }, { "index", i }, { "content_block", block } }));
                return i;
            }

            void delta(int i, const juce::var& d)
            {
                emit("content_block_delta", claude::object({ { "type", "content_block_delta" }, { "index", i }, { "delta", d } }));
            }

            void close(int i) { emit("content_block_stop", claude::object({ { "type", "content_block_stop" }, { "index", i } })); }

            void end(const std::string& stopReason, int64_t input, int64_t cached, int64_t output)
            {
                emit("message_delta", claude::object({ { "type", "message_delta" },
                                                       { "delta", claude::object({ { "stop_reason", utf8(stopReason) } }) },
                                                       { "usage", claude::object({ { "input_tokens", static_cast<juce::int64>(std::max<int64_t>(0, input - cached)) },
                                                                                   { "cache_read_input_tokens", static_cast<juce::int64>(cached) },
                                                                                   { "output_tokens", static_cast<juce::int64>(output) } }) } }));
                emit("message_stop", claude::object({ { "type", "message_stop" } }));
            }

            void error(const std::string& message)
            {
                emit("error", claude::object({ { "type", "error" },
                                               { "error", claude::object({ { "type", "api_error" }, { "message", utf8(message) } }) } }));
            }

        private:
            void emit(const char* type, const juce::var& data) { sink_(type, claude::toJson(data)); }

            Sink sink_;
            bool started_ = false;
            int next_ = 0;
        };

        // The text or reasoning block being written, closed when the other kind (or a call) comes.
        struct Current
        {
            enum class Kind : uint8_t { none, text, thinking } kind = Kind::none;
            int index = -1;
        };

        void writeTo(Events& out, Current& cur, Current::Kind kind, const std::string& t)
        {
            if (t.empty())
                return;
            if (cur.kind != kind)
            {
                if (cur.kind != Current::Kind::none)
                    out.close(cur.index);
                cur.kind = kind;
                cur.index = out.open(kind == Current::Kind::text ? claude::object({ { "type", "text" }, { "text", "" } })
                                                                 : claude::object({ { "type", "thinking" }, { "thinking", "" } }));
            }
            out.delta(cur.index, kind == Current::Kind::text ? claude::object({ { "type", "text_delta" }, { "text", utf8(t) } })
                                                             : claude::object({ { "type", "thinking_delta" }, { "thinking", utf8(t) } }));
        }

        void closeCurrent(Events& out, Current& cur)
        {
            if (cur.kind != Current::Kind::none)
                out.close(cur.index);
            cur = {};
        }

        // The words of a model's own provider in OpenRouter's `metadata.raw` (its error body, as a string or an object):
        // its message, else the text itself, cut short.
        std::string rawWords(const juce::var& raw)
        {
            juce::var v = raw;
            if (raw.isString())
            {
                const std::string text = raw.toString().toStdString();
                v = claude::parseJson(text);
                if (!v.isObject())
                    return std::string(lang::headOf(text, 500));   // never inside a character
            }
            if (!v.isObject())
                return {};
            const juce::var inner = v.getProperty("error", juce::var());
            if (inner.isString())
                return inner.toString().toStdString();
            if (std::string m = str(inner, "message"); !m.empty())
                return m;
            if (std::string m = str(v, "message"); !m.empty())
                return m;
            return std::string(lang::headOf(claude::toJson(v), 500));
        }

        std::string errorOf(const juce::var& e)
        {
            if (e.isString())
                return e.toString().toStdString();
            std::string m = str(e, "message");
            if (m.empty())
                return claude::toJson(e);
            // OpenRouter passing on a refusal from the model's provider ("Provider returned error"): that provider's own
            // words beside it, so the error says what was refused and the turn's workarounds can read it.
            if (const juce::var meta = e.getProperty("metadata", juce::var()); meta.isObject())
                if (const std::string words = rawWords(meta.getProperty("raw", juce::var())); !words.empty() && words != m)
                {
                    const std::string who = str(meta, "provider_name");
                    m += " (" + (who.empty() ? std::string() : who + ": ") + words + ")";
                }
            return m;
        }

        class OpenAiStream final : public Stream
        {
        public:
            explicit OpenAiStream(Sink sink) : out_(std::move(sink)) {}

            void event(const std::string&, const std::string& data) override
            {
                if (juce::String(data).trim() == "[DONE]")
                {
                    done_ = true;
                    return;
                }
                const juce::var v = claude::parseJson(data);
                if (!v.isObject() || errored_)
                    return;
                if (v.hasProperty("error"))
                {
                    out_.start(model_);
                    out_.error(errorOf(v.getProperty("error", juce::var())));
                    errored_ = true;
                    return;
                }
                if (model_.empty())
                    model_ = str(v, "model");
                out_.start(model_);
                const juce::var u = v.getProperty("usage", juce::var());
                if (u.isObject())
                {
                    input_ = static_cast<juce::int64>(u.getProperty("prompt_tokens", 0));
                    output_ = static_cast<juce::int64>(u.getProperty("completion_tokens", 0));
                    cached_ = static_cast<juce::int64>(u.getProperty("prompt_tokens_details", juce::var()).getProperty("cached_tokens", 0));
                }
                const juce::Array<juce::var> choices = arrayOf(v.getProperty("choices", juce::var()));
                if (choices.isEmpty())
                    return;
                const juce::var& choice = choices.getReference(0);
                const juce::var d = choice.getProperty("delta", juce::var());
                // Reasoning, as the servers that show it send it: shown as thinking, never kept. OpenRouter's comes as
                // "reasoning" and again as reasoning_details' text (and only there from some models): the details are
                // read when no reasoning came as text, so it shows once.
                std::string reasoning;
                for (const char* key : { "reasoning_content", "reasoning" })
                    if (const juce::var r = d.getProperty(key, juce::var()); r.isString() && r.toString().isNotEmpty())
                    {
                        reasoning = r.toString().toStdString();
                        break;
                    }
                if (reasoning.empty())
                    for (const juce::var& detail : arrayOf(d.getProperty("reasoning_details", juce::var())))
                    {
                        const std::string type = str(detail, "type");
                        if (type == "reasoning.text")
                            reasoning += str(detail, "text");
                        else if (type == "reasoning.summary")
                            reasoning += detail.hasProperty("summary") ? str(detail, "summary") : str(detail, "text");
                    }
                writeTo(out_, cur_, Current::Kind::thinking, reasoning);
                if (const juce::var c = d.getProperty("content", juce::var()); c.isString())
                    writeTo(out_, cur_, Current::Kind::text, c.toString().toStdString());
                for (const juce::var& tc : arrayOf(d.getProperty("tool_calls", juce::var())))
                {
                    // By its index; a server that sends each call whole without one: a new call each.
                    const juce::var index = tc.getProperty("index", juce::var());
                    const int at = index.isVoid() || index.isUndefined() ? static_cast<int>(calls_.size()) : static_cast<int>(index);
                    Call& call = calls_[at];
                    const juce::var fn = tc.getProperty("function", juce::var());
                    if (const std::string name = str(fn, "name"); !name.empty() && call.name.empty())
                        call.name = name;
                    const juce::var args = fn.getProperty("arguments", juce::var());
                    call.pending += args.isString() ? args.toString().toStdString() : args.isObject() ? claude::toJson(args) : std::string();
                    if (!call.started && !call.name.empty())
                    {
                        closeCurrent(out_, cur_);
                        call.index = out_.open(claude::object({ { "type", "tool_use" },
                                                                { "id", utf8(newCallId()) },
                                                                { "name", utf8(call.name) },
                                                                { "input", juce::var(new juce::DynamicObject()) } }));
                        call.started = true;
                    }
                    if (call.started && !call.pending.empty())
                    {
                        out_.delta(call.index, claude::object({ { "type", "input_json_delta" }, { "partial_json", utf8(call.pending) } }));
                        call.pending.clear();
                    }
                }
                if (const juce::var fr = choice.getProperty("finish_reason", juce::var()); fr.isString() && fr.toString().isNotEmpty())
                    finish_ = fr.toString().toStdString();
            }

            void finish() override
            {
                if (!out_.started() || errored_ || (!done_ && finish_.empty()))
                    return;                              // nothing came, an error said so, or it broke off
                closeCurrent(out_, cur_);
                bool tools = false;
                for (auto& [at, call] : calls_)
                    if (call.started)
                    {
                        out_.close(call.index);
                        tools = true;
                    }
                // Some servers say "stop" after a call: a call is a tool_use whatever they say, but a cut-off one.
                const std::string stop = finish_ == "length"           ? "max_tokens"
                                       : finish_ == "content_filter"   ? "refusal"
                                       : tools                         ? "tool_use"
                                                                       : "end_turn";
                out_.end(stop, input_, cached_, output_);
            }

        private:
            struct Call
            {
                std::string name, pending;
                int index = -1;
                bool started = false;
            };
            Events out_;
            Current cur_;
            std::map<int, Call> calls_;
            std::string model_, finish_;
            int64_t input_ = 0, output_ = 0, cached_ = 0;
            bool done_ = false, errored_ = false;
        };

        class GeminiStream final : public Stream
        {
        public:
            explicit GeminiStream(Sink sink) : out_(std::move(sink)) {}

            void event(const std::string&, const std::string& data) override
            {
                const juce::var v = claude::parseJson(data);
                if (!v.isObject() || errored_)
                    return;
                if (v.hasProperty("error"))
                {
                    out_.start({});
                    out_.error(errorOf(v.getProperty("error", juce::var())));
                    errored_ = true;
                    return;
                }
                out_.start(str(v, "modelVersion"));
                const juce::var u = v.getProperty("usageMetadata", juce::var());
                if (u.isObject())
                {
                    input_ = static_cast<juce::int64>(u.getProperty("promptTokenCount", 0));
                    cached_ = static_cast<juce::int64>(u.getProperty("cachedContentTokenCount", 0));
                    output_ = static_cast<juce::int64>(u.getProperty("candidatesTokenCount", 0))
                            + static_cast<juce::int64>(u.getProperty("thoughtsTokenCount", 0));
                }
                if (const std::string blocked = str(v.getProperty("promptFeedback", juce::var()), "blockReason"); !blocked.empty())
                    finish_ = "SAFETY";
                const juce::Array<juce::var> candidates = arrayOf(v.getProperty("candidates", juce::var()));
                if (candidates.isEmpty())
                    return;
                const juce::var& c = candidates.getReference(0);
                for (const juce::var& part : arrayOf(c.getProperty("content", juce::var()).getProperty("parts", juce::var())))
                {
                    if (part.hasProperty("functionCall"))
                    {
                        const juce::var fc = part.getProperty("functionCall", juce::var());
                        closeCurrent(out_, cur_);
                        juce::var block = claude::object({ { "type", "tool_use" },
                                                           { "id", utf8(newCallId()) },
                                                           { "name", fc.getProperty("name", "") },
                                                           { "input", juce::var(new juce::DynamicObject()) } });
                        if (const std::string sig = str(part, "thoughtSignature"); !sig.empty())
                            block.getDynamicObject()->setProperty("thought_signature", utf8(sig));
                        const int i = out_.open(block);
                        const juce::var args = fc.getProperty("args", juce::var());
                        out_.delta(i, claude::object({ { "type", "input_json_delta" },
                                                       { "partial_json", utf8(args.isObject() ? claude::toJson(args) : std::string("{}")) } }));
                        out_.close(i);
                        tools_ = true;
                    }
                    else if (const juce::var t = part.getProperty("text", juce::var()); t.isString())
                        writeTo(out_, cur_, static_cast<bool>(part.getProperty("thought", false)) ? Current::Kind::thinking : Current::Kind::text,
                                t.toString().toStdString());
                }
                if (const std::string fr = str(c, "finishReason"); !fr.empty())
                    finish_ = fr;
            }

            void finish() override
            {
                if (!out_.started() || errored_ || finish_.empty())
                    return;                              // nothing came, an error said so, or it broke off
                closeCurrent(out_, cur_);
                if (finish_ == "MALFORMED_FUNCTION_CALL" || finish_ == "UNEXPECTED_TOOL_CALL")
                {
                    out_.error("Gemini wrote a tool call it could not finish (" + finish_ + ")");   // asked again
                    return;
                }
                const bool refused = finish_ == "SAFETY" || finish_ == "RECITATION" || finish_ == "BLOCKLIST"
                                  || finish_ == "PROHIBITED_CONTENT" || finish_ == "SPII" || finish_ == "IMAGE_SAFETY";
                const std::string stop = finish_ == "MAX_TOKENS" ? "max_tokens" : refused ? "refusal" : tools_ ? "tool_use" : "end_turn";
                out_.end(stop, input_, cached_, output_);
            }

        private:
            Events out_;
            Current cur_;
            std::string finish_;
            int64_t input_ = 0, output_ = 0, cached_ = 0;
            bool tools_ = false, errored_ = false;
        };
    }

    std::unique_ptr<Stream> streamFor(const ChatProvider& p, Sink sink)
    {
        if (kindOf(p.kind) == Kind::gemini)
            return std::make_unique<GeminiStream>(std::move(sink));
        return std::make_unique<OpenAiStream>(std::move(sink));
    }

    // ---- errors -----------------------------------------------------------------------------------------------------------
    namespace
    {
        std::string messageOf(const std::string& body)
        {
            const juce::var v = claude::parseJson(body);
            if (v.isObject() && v.hasProperty("error"))
                return errorOf(v.getProperty("error", juce::var()));
            if (v.isObject() && v.hasProperty("message"))
                return str(v, "message");
            return std::string(lang::headOf(body, 300));   // a page that is no JSON: never cut inside a character
        }
    }

    bool keyRefused(const ChatProvider& p, int status, const std::string& body)
    {
        if (status == 401)
            return true;
        const std::string m = lower(messageOf(body));
        return kindOf(p.kind) == Kind::gemini && (status == 400 || status == 403) && contains(m, "api key not valid");
    }

    std::string errorText(const ChatProvider& p, int status, const std::string& body)
    {
        const std::string name = nameOf(p);
        const std::string message = messageOf(body);
        if (keyRefused(p, status, body))
            return name + " refused the API key (" + message + "): add it again";
        if (aboutModel(status, message))
            return name + " cannot run " + (p.model.empty() ? std::string("this model") : p.model) + " for this chat ("
                 + message + "): it needs a model that chats and calls tools. Choose another in the model menu";
        switch (status)
        {
            case 403: return name + " does not let this key do that (" + message + ")";
            case 404: return name + " has no such model or address (" + message + ")";
            case 429: return name + " is rate limiting the requests, or the key's quota is spent (" + message + ")";
            case 503: return name + " is overloaded (" + message + ")";
            default: break;
        }
        std::string t = name + ": HTTP " + std::to_string(status) + ": " + message;
        if (aboutContext(message))
            t += ": the conversation is longer than this model's context; start a new chat (this one stays in the list)"
                 + std::string(keyless(p) ? ", or give the model a longer context in the server's settings" : "");
        return t;
    }

    std::string connectionText(const ChatProvider& p, const std::string& error)
    {
        if (error == "stopped" || error == "cancelled")
            return error;
        const std::string host = hostOf(baseOf(p));
        return "no connection to " + host + (keyless(p) ? ": is the server (" + nameOf(p) + ") running?" : std::string());
    }

    std::string redirectText(const ChatProvider& p, int status, const std::string& location)
    {
        return nameOf(p) + " answered with a redirect (HTTP " + std::to_string(status)
             + (location.empty() ? std::string() : ", to " + std::string(lang::headOf(location, 200)))
             + "): the chat follows none, so nothing went there. If you trust that address, make it the server's";
    }

    bool aboutModel(int status, const std::string& error)
    {
        if (status != 400 && status != 404 && status != 422)
            return false;
        const std::string e = lower(error);
        return contains(e, "v1/responses") || contains(e, "not a chat model") || contains(e, "does not support tools")
            || contains(e, "support tool use") || contains(e, "tools is not supported") || contains(e, "'tools' is not supported")
            || contains(e, "tool use is not supported") || contains(e, "function calling is not supported")
            || contains(e, "does not support function calling");
    }

    bool aboutImages(int status, const std::string& error)
    {
        if (status != 400 && status != 422 && status != 500)
            return false;
        const std::string e = lower(error);
        return contains(e, "image") || contains(e, "vision") || contains(e, "multimodal") || contains(e, "image_url");
    }

    bool aboutContext(const std::string& error)
    {
        const std::string e = lower(error);
        return contains(e, "context length") || contains(e, "context_length") || contains(e, "context window")
            || contains(e, "maximum context") || contains(e, "too many tokens") || contains(e, "exceeds the maximum number of tokens")
            || contains(e, "prompt is too long");
    }

    bool aboutSchema(int status, const std::string& error)
    {
        const std::string e = lower(error);
        return status == 400
            && (contains(e, "parametersjsonschema") || contains(e, "parameters_json_schema") || contains(e, "json schema")
                || contains(e, "function_declarations") || contains(e, "functiondeclarations"));
    }

    juce::var forAnthropic(const juce::Array<juce::var>& messages)
    {
        bool any = false;
        for (const juce::var& m : messages)
            for (const juce::var& b : arrayOf(m.getProperty("content", juce::var())))
                any = any || b.hasProperty("thought_signature");
        if (!any)
            return messages;
        juce::Array<juce::var> out;
        for (const juce::var& m : messages)
        {
            juce::var copy = copyOf(m);
            if (juce::Array<juce::var>* blocks = copy.getProperty("content", juce::var()).getArray())
                for (juce::var& b : *blocks)
                    if (auto* o = b.getDynamicObject())
                        o->removeProperty("thought_signature");
            out.add(copy);
        }
        return out;
    }
}
