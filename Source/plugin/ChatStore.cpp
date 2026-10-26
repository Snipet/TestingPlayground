// Source/plugin/ChatStore.cpp: see ChatStore.h.
#include "plugin/ChatStore.h"

#include "Product.h"

#include <juce_core/juce_core.h>

#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#include <CommonCrypto/CommonRandom.h>

#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <vector>

namespace testingplaygroundsynth::chatstore
{
    namespace
    {
        constexpr char kMagic[4] = { 'F', 'P', 'K', '1' };
        constexpr std::size_t kSaltBytes = 16, kIvBytes = 16, kMacBytes = CC_SHA256_DIGEST_LENGTH;
        constexpr unsigned kRounds = 100000;

        std::mutex overrideMutex;
        juce::File overrideDir;                      // empty: the default

        juce::File keyFile() { return directory().getChildFile("anthropic-key.bin"); }
        juce::File tokenFile() { return directory().getChildFile("account-token.bin"); }
        juce::File choiceFile() { return directory().getChildFile("chat.json"); }

        bool ensureDirectory(std::string* why)
        {
            const juce::File dir = directory();
            if (!dir.isDirectory() && !dir.createDirectory())
            {
                if (why != nullptr)
                    *why = "cannot create " + dir.getFullPathName().toStdString();
                return false;
            }
            ::chmod(dir.getFullPathName().toRawUTF8(), 0700);
            return true;
        }

        // The encryption key and the MAC key, from this Mac and account and the salt.
        bool deriveKeys(const uint8_t* salt, std::array<uint8_t, 32>& enc, std::array<uint8_t, 32>& mac)
        {
            const std::string secret = "funk-playground-key-v1|" + juce::SystemStats::getUniqueDeviceID().toStdString()
                                     + "|" + juce::SystemStats::getLogonName().toStdString() + "|"
                                     + std::to_string(static_cast<unsigned>(::getuid()));
            std::array<uint8_t, 64> both{};
            const int rc = CCKeyDerivationPBKDF(kCCPBKDF2, secret.data(), secret.size(), salt, kSaltBytes,
                                                kCCPRFHmacAlgSHA256, kRounds, both.data(), both.size());
            if (rc != kCCSuccess)
                return false;
            std::copy(both.begin(), both.begin() + 32, enc.begin());
            std::copy(both.begin() + 32, both.end(), mac.begin());
            return true;
        }

        std::array<uint8_t, kMacBytes> hmac(const std::array<uint8_t, 32>& key, const uint8_t* data, std::size_t n)
        {
            std::array<uint8_t, kMacBytes> out{};
            CCHmac(kCCHmacAlgSHA256, key.data(), key.size(), data, n, out.data());
            return out;
        }

        bool sameBytes(const uint8_t* a, const uint8_t* b, std::size_t n)
        {
            uint8_t diff = 0;                        // constant time
            for (std::size_t i = 0; i < n; ++i)
                diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
            return diff == 0;
        }

        // Writes `bytes` to `file` (0600) through a temporary beside it, renamed over it.
        bool writePrivate(const juce::File& file, const void* bytes, std::size_t n, std::string* why)
        {
            const juce::File temp = file.getSiblingFile(file.getFileName() + ".tmp");
            temp.deleteFile();
            {
                juce::FileOutputStream out(temp);
                if (!out.openedOk() || !out.write(bytes, n))
                {
                    if (why != nullptr)
                        *why = "cannot write " + temp.getFullPathName().toStdString();
                    return false;
                }
                out.flush();
            }
            ::chmod(temp.getFullPathName().toRawUTF8(), 0600);
            if (!temp.moveFileTo(file))
            {
                temp.deleteFile();
                if (why != nullptr)
                    *why = "cannot replace " + file.getFullPathName().toStdString();
                return false;
            }
            return true;
        }
    }

    juce::File directory()
    {
        {
            const std::lock_guard<std::mutex> lock(overrideMutex);
            if (overrideDir != juce::File())
                return overrideDir;
        }
        // A probe's sandbox (CTest sets <ENV_PREFIX>PREFS_DIR for each, cmake/Probes.cmake): never the user's folder.
        const std::string sandbox = std::string(product::kEnvPrefix) + "PREFS_DIR";
        if (const char* dir = std::getenv(sandbox.c_str()); dir != nullptr && *dir != '\0')
            return juce::File(juce::String::fromUTF8(dir)).getChildFile("chat");
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("Application Support")
            .getChildFile("Funk")
            .getChildFile("Playground");
    }

    void setDirectoryForTests(const juce::File& dir)
    {
        const std::lock_guard<std::mutex> lock(overrideMutex);
        overrideDir = dir;
    }

    bool plausibleKey(std::string_view key)
    {
        constexpr std::string_view prefix = "sk-ant-";
        if (key.size() < prefix.size() + 20 || key.size() > prefix.size() + 300 || key.substr(0, prefix.size()) != prefix)
            return false;
        for (const char c : key)
        {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'
                         || c == '_';
            if (!ok)
                return false;
        }
        return true;
    }

    bool plausibleToken(std::string_view token)
    {
        constexpr std::string_view prefix = "tpb_";
        if (token.size() < prefix.size() + 20 || token.size() > prefix.size() + 200
            || token.substr(0, prefix.size()) != prefix)
            return false;
        for (const char c : token)
        {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'
                         || c == '_';
            if (!ok)
                return false;
        }
        return true;
    }

    namespace
    {
        // `secret` sealed into `file` (ChatStore.h: FPK1).
        bool seal(const juce::File& file, std::string_view secret, std::string* why)
        {
            if (!ensureDirectory(why))
                return false;
            std::array<uint8_t, kSaltBytes> salt{};
            std::array<uint8_t, kIvBytes> iv{};
            if (CCRandomGenerateBytes(salt.data(), salt.size()) != kCCSuccess
                || CCRandomGenerateBytes(iv.data(), iv.size()) != kCCSuccess)
            {
                if (why != nullptr)
                    *why = "no random bytes";
                return false;
            }
            std::array<uint8_t, 32> enc{}, mac{};
            if (!deriveKeys(salt.data(), enc, mac))
            {
                if (why != nullptr)
                    *why = "the key derivation failed";
                return false;
            }
            std::vector<uint8_t> cipher(secret.size() + kCCBlockSizeAES128);
            std::size_t written = 0;
            if (CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, enc.data(), enc.size(), iv.data(),
                        secret.data(), secret.size(), cipher.data(), cipher.size(), &written)
                != kCCSuccess)
            {
                if (why != nullptr)
                    *why = "the encryption failed";
                return false;
            }
            cipher.resize(written);
            std::vector<uint8_t> bytes;
            bytes.insert(bytes.end(), kMagic, kMagic + 4);
            bytes.insert(bytes.end(), salt.begin(), salt.end());
            bytes.insert(bytes.end(), iv.begin(), iv.end());
            bytes.insert(bytes.end(), cipher.begin(), cipher.end());
            const auto tag = hmac(mac, bytes.data(), bytes.size());
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            return writePrivate(file, bytes.data(), bytes.size(), why);
        }

        // What `file` holds, opened; nothing, with why, when it is missing, damaged or made elsewhere. `what` names
        // it in the reasons ("key", "sign-in").
        std::optional<std::string> unseal(const juce::File& file, const std::string& what, std::string* why)
        {
            const auto fail = [&](const std::string& reason) -> std::optional<std::string> {
                if (why != nullptr)
                    *why = reason;
                return std::nullopt;
            };
            juce::MemoryBlock block;
            if (!file.loadFileAsData(block))
                return fail("no " + what + " saved");
            const auto* bytes = static_cast<const uint8_t*>(block.getData());
            const std::size_t n = block.getSize();
            constexpr std::size_t head = 4 + kSaltBytes + kIvBytes;
            if (n < head + kCCBlockSizeAES128 + kMacBytes
                || !sameBytes(bytes, reinterpret_cast<const uint8_t*>(kMagic), 4))
                return fail("the saved " + what + " is damaged");
            std::array<uint8_t, 32> enc{}, mac{};
            if (!deriveKeys(bytes + 4, enc, mac))
                return fail("the key derivation failed");
            const auto tag = hmac(mac, bytes, n - kMacBytes);
            if (!sameBytes(tag.data(), bytes + n - kMacBytes, kMacBytes))
                return fail("the saved " + what + " was made on another Mac or account, or is damaged");
            const uint8_t* cipher = bytes + head;
            const std::size_t cipherBytes = n - head - kMacBytes;
            std::vector<char> plain(cipherBytes + kCCBlockSizeAES128);
            std::size_t written = 0;
            if (CCCrypt(kCCDecrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, enc.data(), enc.size(),
                        bytes + 4 + kSaltBytes, cipher, cipherBytes, plain.data(), plain.size(), &written)
                != kCCSuccess)
                return fail("the saved " + what + " does not decrypt");
            std::string secret(plain.data(), written);
            std::fill(plain.begin(), plain.end(), '\0');
            return secret;
        }

        // chat.json as an object (empty when it is missing or damaged), and back with `edit` applied.
        juce::var loadChatJson()
        {
            const juce::var v = juce::JSON::parse(choiceFile());
            if (v.getDynamicObject() != nullptr)
                return v;
            return juce::var(new juce::DynamicObject());
        }

        void saveChatJson(const std::function<void(juce::DynamicObject&)>& edit)
        {
            if (!ensureDirectory(nullptr))
                return;
            juce::var v = loadChatJson();
            edit(*v.getDynamicObject());
            const std::string text = juce::JSON::toString(v, true).toStdString() + "\n";
            writePrivate(choiceFile(), text.data(), text.size(), nullptr);
        }
    }

    bool hasKey() { return keyFile().existsAsFile(); }

    bool saveKey(std::string_view key, std::string* why)
    {
        if (!plausibleKey(key))
        {
            if (why != nullptr)
                *why = "that is not an Anthropic API key (they start with sk-ant-)";
            return false;
        }
        return seal(keyFile(), key, why);
    }

    std::optional<std::string> loadKey(std::string* why)
    {
        std::optional<std::string> key = unseal(keyFile(), "key", why);
        if (key && !plausibleKey(*key))
        {
            if (why != nullptr)
                *why = "the saved key is damaged";
            return std::nullopt;
        }
        if (!key && why != nullptr && why->find("another Mac") != std::string::npos)
            *why += ": add it again";
        if (!key && why != nullptr && *why == "no key saved")
            *why = "no API key saved";
        return key;
    }

    bool forgetKey()
    {
        const juce::File f = keyFile();
        return !f.existsAsFile() || f.deleteFile();
    }

    bool hasToken() { return tokenFile().existsAsFile(); }

    bool saveToken(std::string_view token, std::string* why)
    {
        if (!plausibleToken(token))
        {
            if (why != nullptr)
                *why = "the server answered with something that is not a sign-in token";
            return false;
        }
        return seal(tokenFile(), token, why);
    }

    std::optional<std::string> loadToken(std::string* why)
    {
        std::optional<std::string> token = unseal(tokenFile(), "sign-in", why);
        if (token && !plausibleToken(*token))
        {
            if (why != nullptr)
                *why = "the saved sign-in is damaged";
            return std::nullopt;
        }
        if (!token && why != nullptr && why->find("another Mac") != std::string::npos)
            *why += ": sign in again";
        return token;
    }

    bool forgetToken()
    {
        const juce::File f = tokenFile();
        return !f.existsAsFile() || f.deleteFile();
    }

    Choice loadChoice()
    {
        Choice c;
        const juce::var v = juce::JSON::parse(choiceFile());
        if (auto* o = v.getDynamicObject())
        {
            c.model = o->getProperty("model").toString().toStdString();
            c.effort = o->getProperty("effort").toString().toStdString();
            c.fast = static_cast<bool>(o->getProperty("fast"));
        }
        return c;
    }

    void saveChoice(const Choice& choice)
    {
        saveChatJson([&](juce::DynamicObject& o) {
            o.setProperty("model", juce::String(choice.model));
            o.setProperty("effort", juce::String(choice.effort));
            o.setProperty("fast", choice.fast);
        });
    }

    Account loadAccount()
    {
        Account a;
        const juce::var v = juce::JSON::parse(choiceFile());
        if (auto* o = v.getDynamicObject())
        {
            a.username = o->getProperty("account").toString().toStdString();
            if (o->hasProperty("use"))
            {
                a.useAccount = o->getProperty("use").toString() != "key";
                return a;
            }
        }
        // Never chosen (a key saved before sign-in existed): the key, when one is stored and no sign-in is.
        a.useAccount = hasToken() || !hasKey();
        return a;
    }

    void saveAccount(const Account& account)
    {
        saveChatJson([&](juce::DynamicObject& o) {
            o.setProperty("account", juce::String::fromUTF8(account.username.c_str()));
            o.setProperty("use", account.useAccount ? "account" : "key");
        });
    }
}
