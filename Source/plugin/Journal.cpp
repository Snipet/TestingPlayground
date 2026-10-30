// Source/plugin/Journal.cpp: see Journal.h.
#include "plugin/Journal.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>

namespace playground
{
    namespace
    {
        int64_t nowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        std::tm local(int64_t ms)
        {
            const std::time_t t = static_cast<std::time_t>(ms / 1000);
            std::tm tm{};
            localtime_r(&t, &tm);
            return tm;
        }

        // "14:02:31", "14:02:31-33" (the same minute), "14:02:31-14:05:01".
        std::string when(int64_t fromMs, int64_t toMs)
        {
            const std::tm a = local(fromMs), b = local(toMs);
            char buf[32];
            if (toMs / 1000 == fromMs / 1000)
                std::snprintf(buf, sizeof buf, "%02d:%02d:%02d", a.tm_hour, a.tm_min, a.tm_sec);
            else if (a.tm_hour == b.tm_hour && a.tm_min == b.tm_min)
                std::snprintf(buf, sizeof buf, "%02d:%02d:%02d-%02d", a.tm_hour, a.tm_min, a.tm_sec, b.tm_sec);
            else
                std::snprintf(buf, sizeof buf, "%02d:%02d:%02d-%02d:%02d:%02d", a.tm_hour, a.tm_min, a.tm_sec, b.tm_hour, b.tm_min,
                              b.tm_sec);
            return buf;
        }

        const char* subject(Journal::Who who)
        {
            switch (who)
            {
                case Journal::Who::you:    return "you";
                case Journal::Who::host:   return "the host";
                case Journal::Who::chat:   return "the plugin's own chat";
                case Journal::Who::plugin: return "the plugin";
            }
            return "";
        }

        // A token's two parts: "<epoch, hex>-<change>".
        bool parse(const std::string& token, uint32_t& epoch, uint64_t& seq)
        {
            const std::size_t dash = token.find('-');
            if (dash == std::string::npos || dash == 0 || dash + 1 >= token.size())
                return false;
            char* end = nullptr;
            const unsigned long e = std::strtoul(token.substr(0, dash).c_str(), &end, 16);
            if (end == nullptr || *end != '\0')
                return false;
            const unsigned long long s = std::strtoull(token.substr(dash + 1).c_str(), &end, 10);
            if (end == nullptr || *end != '\0')
                return false;
            epoch = static_cast<uint32_t>(e);
            seq = s;
            return true;
        }
    }

    Journal::Journal()
    {
        std::random_device rd;
        epoch_ = (static_cast<uint32_t>(rd()) & 0xfffffu) | 0x100000u;   // six hex digits, never 0
    }

    Journal::Change* Journal::mergeable(const std::string& key, Who who, int64_t windowMs, int64_t now)
    {
        for (auto it = changes_.rbegin(); it != changes_.rend(); ++it)
            if (it->key == key)
                return it->seq > issued_ && it->who == who && now - it->toMs <= windowMs ? &*it : nullptr;
        return nullptr;
    }

    void Journal::add(Change c)
    {
        c.seq = ++seq_;
        changes_.push_back(std::move(c));
        while (changes_.size() > kMax)
        {
            dropped_ = changes_.front().seq;
            changes_.pop_front();
        }
    }

    void Journal::dial(Who who, int slot, const std::string& name, const std::string& from, const std::string& to)
    {
        if (from == to)
            return;
        const int64_t now = nowMs();
        const std::string key = "dial:" + std::to_string(slot);
        const std::lock_guard<std::mutex> lock(mutex_);
        if (Change* c = mergeable(key, who, kMergeMs, now); c != nullptr && c->words == name)
        {
            c->to = to;
            c->toMs = now;
            return;
        }
        Change c;
        c.who = who;
        c.key = key;
        c.words = name;
        c.from = from;
        c.to = to;
        c.fromMs = c.toMs = now;
        add(std::move(c));
    }

    void Journal::file(Who who, const std::string& name, const std::string& what)
    {
        const int64_t now = nowMs();
        const std::string key = "file:" + name;
        const std::lock_guard<std::mutex> lock(mutex_);
        if (Change* c = mergeable(key, who, kTypingMs, now);
            c != nullptr
            && (c->words == what || what == "edited"
                || (what == "redrew" && (c->words == "edited" || c->words == "added" || c->words == "wrote"))))
        {
            // typing on: one line (an added file typed in stays "added"; a netlist redrawn, then edited, was edited;
            // a redraw adds nothing to an edit)
            if (what == "edited" && c->words == "redrew")
                c->words = what;
            c->toMs = now;
            return;
        }
        Change c;
        c.who = who;
        c.key = key;
        c.words = what;
        c.from = name;
        c.fromMs = c.toMs = now;
        add(std::move(c));
    }

    void Journal::note(Who who, const std::string& words, const std::string& key)
    {
        const int64_t now = nowMs();
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!key.empty())
            if (Change* c = mergeable(key, who, kMergeMs * 4, now); c != nullptr)
            {
                c->words = words;
                c->toMs = now;
                return;
            }
        Change c;
        c.who = who;
        c.key = key;
        c.words = words;
        c.fromMs = c.toMs = now;
        add(std::move(c));
    }

    void Journal::bypass(bool on)
    {
        const int64_t now = nowMs();
        const std::lock_guard<std::mutex> lock(mutex_);
        uint64_t ends = 0;
        if (!on)
            for (auto it = changes_.rbegin(); it != changes_.rend(); ++it)
                if (it->key == "bypass")
                {
                    if (it->open)
                    {
                        it->open = false;                 // whoever reads it reads until when
                        it->toMs = now;
                        if (it->seq > issued_)
                            return;                       // no token covers it yet: its one line says it all
                        ends = it->seq;                   // a token's readers saw it begin: they read the end
                    }
                    break;
                }
        Change c;
        c.who = Who::host;
        c.key = "bypass";
        c.words = on ? "bypassed" : "un-bypassed";
        c.open = on;
        c.ends = ends;
        c.fromMs = c.toMs = now;
        add(std::move(c));
    }

    void Journal::alert(uint32_t bits)
    {
        if (bits == 0)
            return;
        const int64_t now = nowMs();
        std::string what;
        if ((bits & 1u) != 0)
            what = "NaN or inf samples replaced by 0";
        if ((bits & 2u) != 0)
            what += std::string(what.empty() ? "" : ", ") + "samples beyond +-4 clipped";
        const std::lock_guard<std::mutex> lock(mutex_);
        const std::string key = "alert:" + std::to_string(bits);
        if (Change* c = mergeable(key, Who::plugin, 3000, now); c != nullptr)
        {
            c->toMs = now;
            return;
        }
        Change c;
        c.who = Who::plugin;
        c.key = key;
        c.words = "the output guard acted: " + what;
        c.fromMs = c.toMs = now;
        add(std::move(c));
    }

    std::string Journal::line(const Change& c) const
    {
        std::string out = when(c.fromMs, c.toMs) + " ";
        if (c.key.rfind("dial:", 0) == 0)
        {
            out += std::string(subject(c.who)) + (c.who == Who::chat ? " set " : " moved ") + c.words + " ";
            out += c.from == c.to ? "and back to " + c.to : c.from + " -> " + c.to;
            return out;
        }
        if (c.key.rfind("file:", 0) == 0)
            return out + subject(c.who) + " " + c.words + " " + c.from;
        if (c.key == "bypass")
            return when(c.fromMs, c.fromMs) + " "
                 + (c.words == "bypassed" ? (c.open ? "the host bypassed the plugin (it still is)"
                                                    : "the host bypassed the plugin, until " + when(c.toMs, c.toMs))
                                          : "the host un-bypassed the plugin");
        if (c.who == Who::plugin)
            return out + c.words;
        return out + subject(c.who) + " " + c.words;
    }

    Changes Journal::since(const std::string& since, bool issue)
    {
        Changes out;
        uint32_t epoch = 0;
        uint64_t from = 0;
        const bool parsed = parse(since, epoch, from);
        const std::lock_guard<std::mutex> lock(mutex_);
        out.known = parsed && epoch == epoch_ && from <= seq_;
        if (out.known)
        {
            out.complete = from >= dropped_;
            for (const Change& c : changes_)
            {
                if (c.seq <= from || c.ends > from)       // an un-bypass whose bypass is listed: that line says until
                    continue;
                out.lines.push_back(line(c));
                if (c.who == Who::plugin)
                    ++out.alerts;
                else
                    ++out.changes;
            }
        }
        if (issue)
        {
            if (issued_ == seq_)
                ++seq_;                                   // a token of its own: its files are its own (Live.cpp)
            issued_ = seq_;
        }
        char buf[48];
        std::snprintf(buf, sizeof buf, "%x-%llu", static_cast<unsigned>(epoch_), static_cast<unsigned long long>(seq_));
        out.token = buf;
        return out;
    }

    std::string Journal::token() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        char buf[48];
        std::snprintf(buf, sizeof buf, "%x-%llu", static_cast<unsigned>(epoch_), static_cast<unsigned long long>(seq_));
        return buf;
    }

    std::string runWords(const CompileStatus& s)
    {
        if (s.ok)
        {
            int dials = 0;
            for (const Control& c : s.controls)
                dials += c.used ? 1 : 0;
            return "ran it: it compiled, " + std::to_string(dials) + (dials == 1 ? " dial" : " dials");
        }
        int errors = 0;
        for (std::size_t p = s.errors.find(": error:"); p != std::string::npos; p = s.errors.find(": error:", p + 1))
            ++errors;
        return "ran it: it did not compile (" + std::to_string(std::max(errors, 1)) + (errors > 1 ? " errors" : " error")
             + "; the program before plays on)";
    }

    // ---- GesturePort ---------------------------------------------------------------------------------------------------
    void GesturePort::beginGesture()
    {
        ++gestures_;
        moved_ = true;
        inner_.beginGesture();
    }

    void GesturePort::setValue01(float v)
    {
        moved_ = true;
        inner_.setValue01(v);
    }

    void GesturePort::endGesture()
    {
        inner_.endGesture();
        gestures_ = std::max(0, gestures_ - 1);
    }

    bool GesturePort::takeMoved() noexcept
    {
        const bool moved = moved_ || gestures_ > 0;
        moved_ = false;
        return moved;
    }
}
