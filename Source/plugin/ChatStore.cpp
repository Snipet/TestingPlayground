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

    bool hasKey() { return keyFile().existsAsFile(); }

    bool saveKey(std::string_view key, std::string* why)
    {
        if (!plausibleKey(key))
        {
            if (why != nullptr)
                *why = "that is not an Anthropic API key (they start with sk-ant-)";
            return false;
        }
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
        std::vector<uint8_t> cipher(key.size() + kCCBlockSizeAES128);
        std::size_t written = 0;
        if (CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, enc.data(), enc.size(), iv.data(), key.data(),
                    key.size(), cipher.data(), cipher.size(), &written)
            != kCCSuccess)
        {
            if (why != nullptr)
                *why = "the encryption failed";
            return false;
        }
        cipher.resize(written);
        std::vector<uint8_t> file;
        file.insert(file.end(), kMagic, kMagic + 4);
        file.insert(file.end(), salt.begin(), salt.end());
        file.insert(file.end(), iv.begin(), iv.end());
        file.insert(file.end(), cipher.begin(), cipher.end());
        const auto tag = hmac(mac, file.data(), file.size());
        file.insert(file.end(), tag.begin(), tag.end());
        return writePrivate(keyFile(), file.data(), file.size(), why);
    }

    std::optional<std::string> loadKey(std::string* why)
    {
        juce::MemoryBlock block;
        if (!keyFile().loadFileAsData(block))
        {
            if (why != nullptr)
                *why = "no API key saved";
            return std::nullopt;
        }
        const auto* bytes = static_cast<const uint8_t*>(block.getData());
        const std::size_t n = block.getSize();
        constexpr std::size_t head = 4 + kSaltBytes + kIvBytes;
        if (n < head + kCCBlockSizeAES128 + kMacBytes || !sameBytes(bytes, reinterpret_cast<const uint8_t*>(kMagic), 4))
        {
            if (why != nullptr)
                *why = "the saved key is damaged";
            return std::nullopt;
        }
        std::array<uint8_t, 32> enc{}, mac{};
        if (!deriveKeys(bytes + 4, enc, mac))
        {
            if (why != nullptr)
                *why = "the key derivation failed";
            return std::nullopt;
        }
        const auto tag = hmac(mac, bytes, n - kMacBytes);
        if (!sameBytes(tag.data(), bytes + n - kMacBytes, kMacBytes))
        {
            if (why != nullptr)
                *why = "the saved key was made on another Mac or account, or is damaged: add it again";
            return std::nullopt;
        }
        const uint8_t* cipher = bytes + head;
        const std::size_t cipherBytes = n - head - kMacBytes;
        std::vector<char> plain(cipherBytes + kCCBlockSizeAES128);
        std::size_t written = 0;
        if (CCCrypt(kCCDecrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, enc.data(), enc.size(), bytes + 4 + kSaltBytes,
                    cipher, cipherBytes, plain.data(), plain.size(), &written)
            != kCCSuccess)
        {
            if (why != nullptr)
                *why = "the saved key does not decrypt";
            return std::nullopt;
        }
        std::string key(plain.data(), written);
        std::fill(plain.begin(), plain.end(), '\0');
        if (!plausibleKey(key))
        {
            if (why != nullptr)
                *why = "the saved key is damaged";
            return std::nullopt;
        }
        return key;
    }

    bool forgetKey()
    {
        const juce::File f = keyFile();
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
        if (!ensureDirectory(nullptr))
            return;
        auto* o = new juce::DynamicObject();
        o->setProperty("model", juce::String(choice.model));
        o->setProperty("effort", juce::String(choice.effort));
        o->setProperty("fast", choice.fast);
        const std::string text = juce::JSON::toString(juce::var(o), true).toStdString() + "\n";
        writePrivate(choiceFile(), text.data(), text.size(), nullptr);
    }
}
