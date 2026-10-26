// Source/plugin/ChatStore.h: what the chat keeps on this Mac, shared by both playground plugins (the effect and the
// synth, in any host and in their Standalones): the Anthropic API key and the TestingPlayground server's sign-in token,
// each encrypted, and the choices (the model, thinking level and fast mode chosen last; the account's username and
// whether the chat goes through it or the key). They live in ~/Library/Application Support/Funk/Playground/ (0700):
// anthropic-key.bin and account-token.bin (0600), and chat.json.
//
// A secret's file is "FPK1", a 16-byte salt, a 16-byte IV, the secret encrypted with AES-256-CBC, and an HMAC-SHA256 of
// all that before it (CommonCrypto). The two keys come from PBKDF2-HMAC-SHA256 (100,000 rounds, the salt) over this
// Mac's device ID (juce::SystemStats::getUniqueDeviceID) and the user's account: the file is useless on another Mac or
// to another account, and nothing on disk shows the secret in clear. Any program running as this user on this Mac can
// derive the same keys, as it could read anything else of the user's: the encryption protects the secret at rest
// (backups, a copied folder, a stray search), not from such a program. The password typed to sign in is never stored:
// the server answers it with the token.
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

namespace testingplayground::chatstore
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

    // The server's sign-in token, as the key: whether one is saved, the token, save (false with why), forget.
    bool hasToken();
    std::optional<std::string> loadToken(std::string* why = nullptr);
    bool saveToken(std::string_view token, std::string* why = nullptr);
    bool forgetToken();

    // Whether `token` looks like the server's: "tpb_" then 20 to 200 letters, digits, '-' and '_'.
    bool plausibleToken(std::string_view token);

    // The model, thinking level and fast mode chosen last ("" and off when none was).
    struct Choice
    {
        std::string model;
        std::string effort;
        bool fast = false;
    };
    Choice loadChoice();
    void saveChoice(const Choice& choice);           // the account's fields stay as they are

    // The account: the username signed in last (kept after signing out, for the sign-in form) and which of the two
    // the chat uses, the account or the API key (the one set up last; never chosen, the account unless only a key is
    // stored).
    struct Account
    {
        std::string username;
        bool useAccount = true;
    };
    Account loadAccount();
    void saveAccount(const Account& account);       // the choice's fields stay as they are
}
