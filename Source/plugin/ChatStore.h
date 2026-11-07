// Source/plugin/ChatStore.h: what the chat keeps on this Mac, shared by both playground plugins (the effect and the
// synth, in any host and in their Standalones): the Anthropic API key, encrypted, the other providers' keys, encrypted
// and each bound to its server (below), the choices (the provider, the model, thinking level, fast mode, web search and
// planner chosen last) and the chats themselves, encrypted. They live in ~/Library/Application Support/Funk/Playground/
// (0700): anthropic-key.bin (0600), key-<kind>-<origin>.bin (0600), chat.json and chats/ (below). On
// Windows: %APPDATA%\Funk\Playground\, the user's profile's own, and the same files through Windows' CNG (bcrypt) in
// place of CommonCrypto. On Linux: $XDG_DATA_HOME (else ~/.local/share)/Funk/Playground/, through OpenSSL's libcrypto,
// and the keys (the Anthropic key and the other providers') in the desktop's keyring when a Secret Service runs
// (plugin/SecretStore.h: GNOME Keyring, KWallet, KeePassXC), a file only where none does or it refused the key (left
// locked); such a file is newer than whatever the keyring holds, so it is read first, and moves into the keyring the
// next time it is read with one there that takes it. The chats stay files (below) everywhere.
//
// The key's file is "FPK1", a 16-byte salt, a 16-byte IV, the key encrypted with AES-256-CBC, and an HMAC-SHA256 of
// all that before it (CommonCrypto). The two keys come from PBKDF2-HMAC-SHA256 (100,000 rounds, the salt) over this
// Mac's device ID (juce::SystemStats::getUniqueDeviceID; on Linux the machine's key, machineKey(): /etc/machine-id) and
// the user's account: the file is useless on another Mac or to another account, and nothing on disk shows the key in
// clear. Any program running as this user on this Mac can
// derive the same keys, as it could read anything else of the user's: the encryption protects the key at rest
// (backups, a copied folder, a stray search), not from such a program.
//
// The chats (CLAUDE.md, "The chats: on this Mac only"): chats/<owner>/ (0700) a folder an owner, a project's id
// (ProjectStore.h: a UUID in its file) or an instance's ("instance-" and the id its session keeps, for a chat while no
// project is open), and in it two files a chat (0600), named by the chat's id (32 hex digits): <id>.chat, the
// conversation as ChatAgent::saveState writes it, and <id>.meta, what the chat list shows (its title, whether the user
// named it, when it was made and last changed). Both sealed as the key's file is, "FPC1" in place of "FPK1", with the
// salt of the store (chats/.salt, 16 random bytes made once, 0600) so that a process derives the keys once whatever
// number of chats it lists; each file still names its salt, so one written under another salt reads too. Nothing of a
// chat is ever in clear on disk, nor anywhere but this folder: not in a session, a project, an export or a picture.
//
// Message thread or any other; the functions do their own I/O and hold no state but the directory override and the
// chats' derived keys (by salt).
#pragma once

#include "plugin/ChatPort.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace juce
{
    class File;
}

namespace playground::secrets
{
    class Store;
}

namespace playground::chatstore
{
    // The folder (tests point it elsewhere; an empty path restores the default). Under a probe's preferences sandbox
    // (<ENV_PREFIX>PREFS_DIR set) it is that folder's chat/.
    juce::File directory();
    void setDirectoryForTests(const juce::File& dir);

    // The API key: whether one is saved, the key (nothing, with why, when it is missing, does not decrypt or sits in a
    // keyring left locked), save (false with why: not a plausible key, or the file cannot be written), forget (the
    // keyring's item and the file deleted; false with why when either still holds it: a keyring the user left locked).
    // On Linux loadKey, saveKey and forgetKey may wait for the user to answer the desktop's prompt to unlock the
    // keyring: never call them holding a lock the message thread may take.
    bool hasKey();
    std::optional<std::string> loadKey(std::string* why = nullptr);
    bool saveKey(std::string_view key, std::string* why = nullptr);
    bool forgetKey(std::string* why = nullptr);

    // Whether `key` looks like an Anthropic API key: "sk-ant-" then 20 to 300 letters, digits, '-' and '_'.
    bool plausibleKey(std::string_view key);

    // Where the keys go now: the keyring's name ("the desktop's keyring") when one takes them (Linux, a Secret Service
    // running), "" when they go to files. The store is never the user's while the folder is a test's or a probe's
    // sandbox; a probe gives one of its own (overridden: `store`, null for none), or the default back (not overridden).
    std::string keyPlace();
    void setSecretStoreForTests(secrets::Store* store, bool overridden);
    // What the key's file is sealed with besides the account: on Linux the machine's key (systemd's /etc/machine-id,
    // else D-Bus's /var/lib/dbus/machine-id, else "host:" and the host's name); elsewhere the device's id.
    std::string machineKey();

    // The model, thinking level and fast mode chosen last ("" and off when none was).
    struct Choice
    {
        std::string model;
        std::string effort;
        bool fast = false;
        bool web = false;                            // web search and fetch (ChatAgent.h)
        std::string planner = {};                    // the planning model ("" for none)
    };
    Choice loadChoice();
    void saveChoice(const Choice& choice);

    // ---- the other providers (Providers.h) -------------------------------------------------------------------------
    // A provider's key, bound to its server: key-<kind>-<16 hex digits of the server's origin>.bin (0600), sealed as
    // anthropic-key.bin is (FPK1), holding the origin ("https://openrouter.ai") and the key, so a key is only ever read
    // back for the server it was saved for: a file that names another origin reads as none. `kind` is the provider's
    // ("openai", "gemini"); the key's plausibility is the caller's (Providers.h).
    bool hasProviderKey(std::string_view kind, std::string_view origin);
    std::optional<std::string> loadProviderKey(std::string_view kind, std::string_view origin, std::string* why = nullptr);
    bool saveProviderKey(std::string_view kind, std::string_view origin, std::string_view key, std::string* why = nullptr);
    bool forgetProviderKey(std::string_view kind, std::string_view origin, std::string* why = nullptr);

    // The provider chosen last (chat.json "provider", "base": none is Anthropic, as before the providers) and the model
    // chosen last with each other provider (chat.json "models", by modelKey: the kind, and an OpenAI-compatible
    // server's base URL). Anthropic's model stays Choice's "model".
    struct ProviderChoice
    {
        std::string kind = "anthropic";
        std::string base;
        std::map<std::string, std::string> models;
    };
    std::string modelKey(std::string_view kind, std::string_view base);
    ProviderChoice loadProvider();
    void saveProvider(const ProviderChoice& choice);

    // History: until 2026-10-28 the chat could also sign in to a TestingPlayground server instead of using a key, and
    // kept that sign-in here (account-token.bin, and chat.json's "account" and "use"). Deletes what is left of it: the
    // token's file (and a temporary beside it), and those two fields (chat.json's others stay). Nothing there, nothing
    // changes; a file that cannot be read or deleted is left as it is. The agent calls it once, when it starts.
    void forgetOldSignIn();

    // ---- the chats (above) ------------------------------------------------------------------------------------------
    inline constexpr std::size_t kMaxTitle = kMaxChatTitle;      // characters (ChatPort.h)
    inline constexpr const char* kInstancePrefix = "instance-";

    // A new id: 32 random hex digits (a chat's, an instance's). Whether `id` is one (a chat's file is named by it).
    std::string newId();
    bool validId(std::string_view id);
    // Whether `owner` may name a folder of chats: 1 to 64 of a-z, 0-9 and '-', starting with neither '-' nor '.': a
    // project's id (a UUID, lower case) or kInstancePrefix and an id. An owner read from a file or a session is checked
    // so before it reaches a path.
    bool validOwner(std::string_view owner);
    std::string instanceOwner(std::string_view instanceId);     // "instance-<id>"
    // A title as the list keeps it: its UTF-8 kept whole, control characters dropped, the spaces (tabs, line breaks)
    // collapsed and trimmed, at most kMaxTitle characters.
    std::string tidyTitle(std::string_view title);

    // A chat as the list shows it.
    struct ChatMeta
    {
        std::string id;
        std::string title;
        bool named = false;                                      // the user named it: its title no longer follows its
                                                                 // first message
        int64_t createdMs = 0, updatedMs = 0;                    // ms since 1970
    };
    // A chat's file as this process last wrote or read it (its time, size and identity): another writer's change (another
    // instance with the same chat open) shows as a file that is not as it was. Each save renames a new file into place,
    // so its identity (juce::File::getFileIdentifier: the inode, Windows' file index) tells a save of the same sealed size
    // within one tick of the kernel's file clock (Linux before 6.13: 1 to 10 ms), where the time alone would not.
    struct Stamp
    {
        int64_t modified = -1, size = -1;
        uint64_t id = 0;
        bool none() const noexcept { return size < 0; }
        friend bool operator==(const Stamp&, const Stamp&) = default;
    };

    juce::File chatsDirectory();                                 // directory()'s chats/
    // An owner's chats, the newest (last changed) first; a chat whose .meta is missing or damaged is listed from its
    // file's time, titled "Chat". Ids not valid and files not sealed here are left out.
    std::vector<ChatMeta> chatsOf(std::string_view owner);
    bool hasChat(std::string_view owner, std::string_view id);
    // Whether an owner's folder is there (none: it never kept a chat, or its project was deleted with its chats), and
    // whether it holds a chat (a file named by a chat's id; none read).
    bool hasOwner(std::string_view owner);
    bool hasChats(std::string_view owner);
    struct Loaded
    {
        ChatMeta meta;
        std::string state;                                       // ChatAgent::saveState's JSON
        Stamp stamp;
    };
    // A chat, opened: nothing, with why, when it is missing, damaged, or was sealed on another Mac or account.
    std::optional<Loaded> loadChat(std::string_view owner, std::string_view id, std::string* why = nullptr);
    // A chat written (the folder made, 0700, the files 0600): `stamp` is what the caller last wrote or read of it (none:
    // it has not), and becomes what it wrote. `conflict`, and nothing written, when the file is there and not as
    // `stamp` says (another writer's: the caller keeps its chat under a new id instead), or there and `stamp` none.
    enum class SaveResult : uint8_t { ok, conflict, failed };
    SaveResult saveChat(std::string_view owner, const ChatMeta& meta, const std::string& state, Stamp& stamp,
                        std::string* why = nullptr);
    // The user's title for a chat (named), its .meta written again; false when there is no such chat.
    bool renameChat(std::string_view owner, std::string_view id, std::string_view title);
    // A chat deleted (both files); a chat moved to another owner's folder (a chat with no project going with the
    // project it is saved as); every chat of an owner deleted, with its folder (a project deleted).
    bool removeChat(std::string_view owner, std::string_view id);
    bool moveChat(std::string_view from, std::string_view to, std::string_view id);
    bool removeOwner(std::string_view owner);
}
