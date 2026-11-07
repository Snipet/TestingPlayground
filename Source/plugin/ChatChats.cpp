// Source/plugin/ChatChats.cpp: the chats on this Mac, the agent's side (ChatAgent.h, "The chats"; ChatStore.h the
// files). Which chat shows, whose it is, when it is written, and the list the chat card shows.
#include "plugin/ChatAgent.h"

#include "plugin/ChatStore.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string_view>
#include <utility>

namespace playground
{
    namespace
    {
        constexpr std::size_t kAutoTitle = 48;             // a title made from the first message, at most

        uint64_t fnv(std::string_view t)
        {
            uint64_t h = 1469598103934665603ull;
            for (const char c : t)
            {
                h ^= static_cast<uint8_t>(c);
                h *= 1099511628211ull;
            }
            return h;
        }
    }

    // A session's conversation from before the chats were kept here: its chat's id, made from its text, so the same
    // session loaded again finds the chat it became.
    std::string ChatAgent::sessionChatId(const std::string& json)
    {
        char buf[40];
        std::snprintf(buf, sizeof buf, "%016llx%016llx", static_cast<unsigned long long>(fnv(json)),
                      static_cast<unsigned long long>(fnv("adopted|" + json)));
        return buf;
    }

    namespace
    {
        // The first line of the user's first message, cut at a word near kAutoTitle characters.
        std::string autoTitle(const std::string& text)
        {
            std::string line;
            std::size_t at = 0;
            while (at <= text.size() && line.empty())
            {
                const std::size_t end = std::min(text.find('\n', at), text.size());
                line = chatstore::tidyTitle(std::string_view(text).substr(at, end - at));
                at = end + 1;
            }
            if (line.size() <= kAutoTitle)
                return line;
            std::size_t cut = line.rfind(' ', kAutoTitle);
            if (cut == std::string::npos || cut < kAutoTitle / 2)
                cut = kAutoTitle;
            while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80)
                --cut;                                     // never inside a character
            return line.substr(0, cut) + "...";
        }

        // When a chat last changed, as the list says it: the time today, the day this year, else the date.
        std::string whenText(int64_t ms)
        {
            if (ms <= 0)
                return {};
            static constexpr const char* kMonths[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
            const juce::Time t(static_cast<juce::int64>(ms)), now = juce::Time::getCurrentTime();
            char buf[32];
            if (t.getYear() == now.getYear() && t.getMonth() == now.getMonth() && t.getDayOfMonth() == now.getDayOfMonth())
                std::snprintf(buf, sizeof buf, "%02d:%02d", t.getHours(), t.getMinutes());
            else if (t.getYear() == now.getYear())
                std::snprintf(buf, sizeof buf, "%s %d", kMonths[std::clamp(t.getMonth(), 0, 11)], t.getDayOfMonth());
            else
                std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", t.getYear(), t.getMonth() + 1, t.getDayOfMonth());
            return buf;
        }

        bool instanceOwned(const std::string& owner)
        {
            return owner.empty() || owner.rfind(chatstore::kInstancePrefix, 0) == 0;
        }
    }

    // ---- what the chat shown is (mutex held) --------------------------------------------------------------------------
    bool ChatAgent::worthKeeping() const
    {
        if (!messages_.isEmpty() || !checks_.empty())
            return true;
        for (const ChatEntry& e : entries_)
            if (e.kind != ChatEntry::Kind::note && e.kind != ChatEntry::Kind::error)
                return true;
        return false;                                  // the chat's own notes alone (a key saved): nothing to keep
    }

    std::string ChatAgent::titleNow() const
    {
        if (chatNamed_ && !chatTitle_.empty())
            return chatTitle_;
        for (const ChatEntry& e : entries_)
            if (e.kind == ChatEntry::Kind::user)
                if (std::string t = autoTitle(e.text); !t.empty())
                    return t;
        return "New chat";
    }

    bool ChatAgent::onTurnThread() const { return turnThread_.load() == std::this_thread::get_id(); }

    ChatAgent::ChatRef ChatAgent::chatRef() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return { chatOwner_, worthKeeping() ? chatId_ : std::string() };
    }

    std::string ChatAgent::chatOwner() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return chatOwner_;
    }

    // ---- writing it ---------------------------------------------------------------------------------------------------
    void ChatAgent::keep()
    {
        const std::lock_guard<std::mutex> store(storeMutex_);
        keepLocked();
    }

    void ChatAgent::keepLocked()
    {
        std::string owner, wasId;
        chatstore::ChatMeta meta;
        chatstore::Stamp stamp;
        uint64_t revision = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (chatOwner_.empty() || revision_ == storedRevision_ || !worthKeeping())
                return;
            owner = chatOwner_;
            revision = revision_;
            const int64_t now = juce::Time::currentTimeMillis();
            if (chatCreated_ <= 0)
                chatCreated_ = now;
            meta = { chatId_, titleNow(), chatNamed_, chatCreated_, now };
            wasId = chatId_;
            stamp = chatStamp_;
        }
        const std::string json = saveState(true);      // its pictures kept: opened again, it keeps its thinking
        chatstore::SaveResult r = chatstore::saveChat(owner, meta, json, stamp);
        if (r == chatstore::SaveResult::conflict)
        {
            // Another writer changed this chat's file since it was read here (the same chat open in another instance):
            // this one goes under an id of its own, and neither is lost.
            meta.id = chatstore::newId();
            stamp = {};
            r = chatstore::saveChat(owner, meta, json, stamp);
        }
        if (r != chatstore::SaveResult::ok)
            return;                                    // tried again at the next keep (still changed)
        const std::lock_guard<std::mutex> lock(mutex_);
        if (chatOwner_ != owner || chatId_ != wasId)
            return;
        chatId_ = meta.id;
        chatStamp_ = stamp;
        storedRevision_ = revision;
    }

    // ---- which chat shows (storeMutex_ held, no turn running) ---------------------------------------------------------
    void ChatAgent::showChat(const chatstore::Loaded& chat, const std::string& owner, const std::string& label)
    {
        readState(chat.state);
        const std::lock_guard<std::mutex> lock(mutex_);
        chatOwner_ = owner;
        ownerLabel_ = label;
        chatId_ = chat.meta.id;
        chatNamed_ = chat.meta.named;
        chatTitle_ = chat.meta.named ? chat.meta.title : std::string();
        chatCreated_ = chat.meta.createdMs;
        chatStamp_ = chat.stamp;
        touch();
        storedRevision_ = revision_;                   // as it is on disk
    }

    void ChatAgent::showNewest(const std::string& owner, const std::string& label)
    {
        if (!owner.empty())
            for (const chatstore::ChatMeta& m : chatstore::chatsOf(owner))
                if (const std::optional<chatstore::Loaded> l = chatstore::loadChat(owner, m.id))
                {
                    showChat(*l, owner, label);
                    return;
                }
        showFresh(owner, label);
    }

    void ChatAgent::showFresh(const std::string& owner, const std::string& label)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        clearChat();
        chatOwner_ = owner;
        ownerLabel_ = label;
        chatId_ = chatstore::newId();
        chatTitle_.clear();
        chatNamed_ = false;
        chatCreated_ = 0;
        chatStamp_ = {};
        touch();
        storedRevision_ = revision_;
    }

    // ---- the processor's side -----------------------------------------------------------------------------------------
    bool ChatAgent::setOwner(const std::string& owner, const std::string& label, OwnerChange how)
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (owner == chatOwner_)
            {
                if (ownerLabel_ != label)
                {
                    ownerLabel_ = label;               // the project renamed: its chats are the same
                    touch();
                }
                return false;
            }
            if (how == OwnerChange::open && onTurnThread())
            {
                pendingOwner_ = { true, owner, label };   // a tool of the turn opened a project: at the turn's end
                return false;
            }
        }
        if (how == OwnerChange::open)
        {
            stop();
            joinWorker();
            const std::lock_guard<std::mutex> store(storeMutex_);
            keepLocked();                              // the chat shown stays with its owner
            showNewest(owner, label);
            return true;
        }
        // carry: the conversation goes on, with the project it made.
        const std::lock_guard<std::mutex> store(storeMutex_);
        std::string from, id;
        bool stored = false, worth = false;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            from = chatOwner_;
            id = chatId_;
            stored = !chatStamp_.none();
            worth = worthKeeping();
            if (!worth)
            {
                chatOwner_ = owner;
                ownerLabel_ = label;
                touch();
                return false;
            }
        }
        if (instanceOwned(from))
        {
            // A chat with no project moves to the project it was saved as.
            const bool moved = stored && chatstore::moveChat(from, owner, id);
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                chatOwner_ = owner;
                ownerLabel_ = label;
                if (!moved)
                    chatStamp_ = {};
                touch();
            }
            keepLocked();
            return false;
        }
        // Another project's chat: it keeps its own, and a copy goes on with the new project.
        keepLocked();
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            chatOwner_ = owner;
            ownerLabel_ = label;
            chatId_ = chatstore::newId();
            chatCreated_ = 0;
            chatStamp_ = {};
            touch();
        }
        keepLocked();
        return false;
    }

    void ChatAgent::applyPendingOwner()
    {
        PendingOwner p;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            p = std::exchange(pendingOwner_, PendingOwner{});
        }
        if (!p.set)
            return;
        const std::lock_guard<std::mutex> store(storeMutex_);
        keepLocked();
        showNewest(p.owner, p.label);
    }

    void ChatAgent::restoreChat(const std::string& owner, const std::string& label, const std::string& id)
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (owner == chatOwner_ && !id.empty() && id == chatId_)
            {
                ownerLabel_ = label;                   // the chat shown already (a host setting the state it got)
                return;
            }
        }
        stop();
        joinWorker();
        const std::lock_guard<std::mutex> store(storeMutex_);
        keepLocked();
        if (chatstore::validId(id))
            if (const std::optional<chatstore::Loaded> l = chatstore::loadChat(owner, id))
            {
                showChat(*l, owner, label);
                return;
            }
        showNewest(owner, label);
    }

    void ChatAgent::adoptChat(const std::string& json, const std::string& owner, const std::string& label)
    {
        const juce::var v = claude::parseJson(json);
        const auto count = [&](const char* key) {
            const juce::Array<juce::var>* a = v.getProperty(key, juce::var()).getArray();
            return a == nullptr ? 0 : a->size();
        };
        if (count("messages") == 0 && count("entries") == 0)
        {
            restoreChat(owner, label, {});             // an empty conversation: nothing to keep
            return;
        }
        const std::string id = sessionChatId(json);
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (owner == chatOwner_ && id == chatId_)
            {
                ownerLabel_ = label;
                return;
            }
        }
        stop();
        joinWorker();
        const std::lock_guard<std::mutex> store(storeMutex_);
        keepLocked();
        if (const std::optional<chatstore::Loaded> l = chatstore::loadChat(owner, id))
        {
            showChat(*l, owner, label);                // kept before (and what was said since)
            return;
        }
        readState(json);
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            chatOwner_ = owner;
            ownerLabel_ = label;
            chatId_ = id;
            chatTitle_.clear();
            chatNamed_ = false;
            chatCreated_ = 0;
            chatStamp_ = {};
            touch();
            storedRevision_ = 0;                       // written now
        }
        keepLocked();
    }

    // ---- ChatPort: the chat list --------------------------------------------------------------------------------------
    ChatList ChatAgent::chatList() const
    {
        std::string owner, id, title;
        ChatList out;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            owner = chatOwner_;
            id = chatId_;
            title = titleNow();
            out.owner = ownerLabel_;
        }
        bool listed = false;
        if (!owner.empty())
            for (const chatstore::ChatMeta& m : chatstore::chatsOf(owner))
            {
                const bool current = m.id == id;
                listed = listed || current;
                out.chats.push_back({ m.id, current ? title : m.title, whenText(m.updatedMs), current });
            }
        if (!listed)
            out.chats.insert(out.chats.begin(), ChatListEntry{ id, title, {}, true });   // not written yet
        return out;
    }

    bool ChatAgent::openChat(const std::string& id)
    {
        std::string owner, label;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (id == chatId_)
                return true;
            owner = chatOwner_;
            label = ownerLabel_;
        }
        if (!chatstore::validId(id) || !chatstore::hasChat(owner, id))
            return false;
        stop();
        joinWorker();
        const std::lock_guard<std::mutex> store(storeMutex_);
        keepLocked();
        const std::optional<chatstore::Loaded> l = chatstore::loadChat(owner, id);
        if (!l)
            return false;
        showChat(*l, owner, label);
        return true;
    }

    void ChatAgent::renameChat(const std::string& title)
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            chatTitle_ = chatstore::tidyTitle(title);
            chatNamed_ = !chatTitle_.empty();
            touch();
        }
        keep();                                        // its name in the list at once (an empty chat's when it has more)
    }

    void ChatAgent::deleteChat()
    {
        stop();
        joinWorker();
        const std::lock_guard<std::mutex> store(storeMutex_);
        std::string owner, label, id;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            owner = chatOwner_;
            label = ownerLabel_;
            id = chatId_;
        }
        chatstore::removeChat(owner, id);
        showNewest(owner, label);
    }
}
