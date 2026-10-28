// Source/plugin/ChatStore.h: what the chat keeps on this Mac, shared by both playground plugins (the effect and the
// synth, in any host and in their Standalones): the Anthropic API key, encrypted, and the choices (the model, thinking
// level, fast mode, web search and planner chosen last). They live in ~/Library/Application Support/Funk/Playground/
// (0700): anthropic-key.bin (0600) and chat.json.
//
// The key's file is "FPK1", a 16-byte salt, a 16-byte IV, the key encrypted with AES-256-CBC, and an HMAC-SHA256 of
// all that before it (CommonCrypto). The two keys come from PBKDF2-HMAC-SHA256 (100,000 rounds, the salt) over this
// Mac's device ID (juce::SystemStats::getUniqueDeviceID) and the user's account: the file is useless on another Mac or
// to another account, and nothing on disk shows the key in clear. Any program running as this user on this Mac can
// derive the same keys, as it could read anything else of the user's: the encryption protects the key at rest
// (backups, a copied folder, a stray search), not from such a program.
//
// Message thread or any other; the functions do their own I/O and hold no state but the directory override.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace juce
{
    class File;
}

namespace playground::chatstore
{
    // The folder (tests point it elsewhere; an empty path restores the default). Under a probe's preferences sandbox
    // (<ENV_PREFIX>PREFS_DIR set) it is that folder's chat/.
    juce::File directory();
    void setDirectoryForTests(const juce::File& dir);

    // The API key: whether one is saved, the key (nothing, with why, when it is missing or does not decrypt), save
    // (false with why: not a plausible key, or the file cannot be written), forget (the file deleted).
    bool hasKey();
    std::optional<std::string> loadKey(std::string* why = nullptr);
    bool saveKey(std::string_view key, std::string* why = nullptr);
    bool forgetKey();

    // Whether `key` looks like an Anthropic API key: "sk-ant-" then 20 to 300 letters, digits, '-' and '_'.
    bool plausibleKey(std::string_view key);

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

    // History: until 2026-10-28 the chat could also sign in to a TestingPlayground server instead of using a key, and
    // kept that sign-in here (account-token.bin, and chat.json's "account" and "use"). Deletes what is left of it: the
    // token's file (and a temporary beside it), and those two fields (chat.json's others stay). Nothing there, nothing
    // changes; a file that cannot be read or deleted is left as it is. The agent calls it once, when it starts.
    void forgetOldSignIn();
}
