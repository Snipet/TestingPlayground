// Source/mcp/McpTarget.h: which instance Claude Desktop's calls go to, and what each instance tells the others, as files
// in the folder mcp/McpProtocol.h names, which the instances (plugin/McpLink.h) and the server (mcp/Main.cpp) both read.
// juce_core only.
//
//   <n>.json     an instance's card: { token, id, name, track, project, pid }, written by its link when it changes
//                (beside, then renamed over) and removed when the link goes. token: its link's own random number, so an
//                instance that takes the number of one gone is never taken for it; pid: its process, so a card a crash
//                left is not listed
//   target.json  the instance the user chose in a plugin (Use with Claude Desktop, in any instance's explorer):
//                { id, token }; removed when the user clears the choice, or when that instance's link goes
//
// The target rule (the server's, which each plugin shows): the instance target.json names, while it is open (its card's
// token the same); else the only one open; else none, and a call that names no instance is refused with the list, never
// guessed. A call naming one (`instance`) goes to it, the target or not.
//
// Names: an instance is "Synth 2" (its number's name) until the user names it ("Bass"); the name is kept with its
// session. Its number and its number's name keep reaching it (nameRefusal keeps a name from taking another's).
#pragma once

#include "mcp/McpProtocol.h"

#include "Product.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

namespace playground::mcp::target
{
    inline constexpr const char* kTargetFile = "target.json";
    inline constexpr std::size_t kMaxName = 32;          // bytes of a name the user gives an instance

    struct Card
    {
        std::string token, id, name, track, project;
        int pid = 0;

        bool operator==(const Card&) const = default;
    };

    struct Choice
    {
        std::string id, token;                           // "" when none is chosen
    };

    inline juce::File in(const std::string& dir, const std::string& file)
    {
        return juce::File(juce::String::fromUTF8(dir.c_str())).getChildFile(juce::String::fromUTF8(file.c_str()));
    }

    // Its number's name: "Synth 2", "Effect 1".
    inline std::string numberName(const std::string& id) { return std::string(product::kIsInstrument ? "Synth " : "Effect ") + id; }

    // Written beside, then renamed over: another instance or the server may be reading it. The temporary file is the
    // process's and the thread's own (two links of one process may write at once).
    inline bool writeAtomically(const juce::File& f, const juce::String& text)
    {
        const auto thread = static_cast<juce::int64>(reinterpret_cast<juce::pointer_sized_int>(juce::Thread::getCurrentThreadId()));
        const juce::File temp = f.getSiblingFile("." + f.getFileName() + "." + juce::String(processId()) + "."
                                                 + juce::String::toHexString(thread) + ".writing");
        return temp.replaceWithText(text, false, false, "\n") && temp.moveFileTo(f);
    }

    inline std::string text(const juce::var& v, const char* key) { return v[key].toString().toStdString(); }

    inline std::optional<Card> readCard(const juce::File& f)
    {
        const juce::var v = juce::JSON::parse(f);
        if (!v.isObject() || text(v, "token").empty() || text(v, "id").empty())
            return std::nullopt;
        Card c{ text(v, "token"), text(v, "id"), text(v, "name"), text(v, "track"), text(v, "project"), static_cast<int>(v["pid"]) };
        return c;
    }

    inline std::optional<Card> readCard(const std::string& dir, const std::string& id) { return readCard(in(dir, id + ".json")); }

    inline bool writeCard(const std::string& dir, const Card& c)
    {
        auto* o = new juce::DynamicObject();
        const juce::var v(o);
        o->setProperty("token", juce::String(c.token));
        o->setProperty("id", juce::String(c.id));
        o->setProperty("name", juce::String::fromUTF8(c.name.c_str()));
        o->setProperty("track", juce::String::fromUTF8(c.track.c_str()));
        o->setProperty("project", juce::String::fromUTF8(c.project.c_str()));
        o->setProperty("pid", c.pid);
        return writeAtomically(in(dir, c.id + ".json"), juce::JSON::toString(v, true));
    }

    // Instance `id`'s card, when it is the one `token` names ("": any): an instance gone, or a card left behind.
    inline void removeCard(const std::string& dir, const std::string& id, const std::string& token = {})
    {
        const juce::File f = in(dir, id + ".json");
        if (const std::optional<Card> c = readCard(f); token.empty() || (c && c->token == token))
            f.deleteFile();
    }

    // The open instances' cards (their process runs), by number. A card whose socket is gone is not one.
    inline std::vector<Card> cards(const std::string& dir)
    {
        std::vector<std::pair<int, Card>> found;
        for (const juce::File& f : juce::File(juce::String::fromUTF8(dir.c_str())).findChildFiles(juce::File::findFiles, false, "*.json"))
        {
            const juce::String n = f.getFileNameWithoutExtension();
            if (n.isEmpty() || !n.containsOnly("0123456789"))
                continue;                                // target.json, tools.json
            const std::optional<Card> c = readCard(f);
            if (!c || c->id != n.toStdString() || !processAlive(c->pid) || !pathExists(dir + "/" + c->id + ".sock"))
                continue;
            found.push_back({ n.getIntValue(), *c });
        }
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<Card> out;
        for (auto& [n, c] : found)
            out.push_back(std::move(c));
        return out;
    }

    inline Choice readTarget(const std::string& dir)
    {
        const juce::var v = juce::JSON::parse(in(dir, kTargetFile));
        if (!v.isObject())
            return {};
        return { text(v, "id"), text(v, "token") };
    }

    inline bool writeTarget(const std::string& dir, const Choice& c)
    {
        auto* o = new juce::DynamicObject();
        const juce::var v(o);
        o->setProperty("id", juce::String(c.id));
        o->setProperty("token", juce::String(c.token));
        return writeAtomically(in(dir, kTargetFile), juce::JSON::toString(v, true));
    }

    // The choice cleared, when it names `token` ("": whichever it names).
    inline void clearTarget(const std::string& dir, const std::string& token = {})
    {
        if (token.empty() || readTarget(dir).token == token)
            in(dir, kTargetFile).deleteFile();
    }

    // The instance the user chose, when it is open: its card names it with the same token. "" for none.
    inline std::string chosen(const std::string& dir)
    {
        const Choice c = readTarget(dir);
        if (c.id.empty() || c.token.empty())
            return {};
        const std::optional<Card> card = readCard(dir, c.id);
        return card && card->token == c.token && processAlive(card->pid) ? c.id : std::string();
    }

    // Why instance `id` cannot take `name` ("" when it can; "" itself gives it its number's name back): a name is a few
    // words, never a number alone (numbers name instances) nor another instance's number's name.
    inline std::string nameRefusal(const std::string& name, const std::string& id)
    {
        if (name.size() > kMaxName)
            return "a name is at most " + std::to_string(kMaxName) + " characters";
        if (std::any_of(name.begin(), name.end(), [](char ch) { return static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f; }))
            return "a name is one line of text";
        if (!name.empty() && std::all_of(name.begin(), name.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)) != 0; }))
            return "a number alone names an instance by its number: give it a word";
        const std::string kind = product::kIsInstrument ? "synth " : "effect ";
        std::string lower;
        for (const char ch : name)
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (lower.size() > kind.size() && lower.compare(0, kind.size(), kind) == 0
            && std::all_of(lower.begin() + static_cast<std::ptrdiff_t>(kind.size()), lower.end(),
                           [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)) != 0; })
            && lower.substr(kind.size()) != id)
            return numberName(lower.substr(kind.size())) + " is another " + (product::kIsInstrument ? "synth" : "effect")
                 + "'s number";
        return {};
    }
}
