// Source/plugin/ChatStore.cpp: see ChatStore.h.
#include "plugin/ChatStore.h"

#include "Product.h"

#include <juce_core/juce_core.h>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#include <CommonCrypto/CommonRandom.h>

#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

namespace playground::chatstore
{
    namespace
    {
        constexpr char kMagic[4] = { 'F', 'P', 'K', '1' };
        constexpr std::size_t kSaltBytes = 16, kIvBytes = 16, kMacBytes = 32, kBlockBytes = 16;   // SHA-256, AES
        constexpr unsigned kRounds = 100000;
        // Another machine, as the user's error names it ("made on another Mac or account").
#if defined(_WIN32)
        constexpr const char* kAnotherMachine = "another computer";
#else
        constexpr const char* kAnotherMachine = "another Mac";
#endif

        // ---- the four primitives the file needs: CommonCrypto on macOS, CNG (bcrypt) on Windows ------------------
#if defined(_WIN32)
        // A CNG algorithm provider, opened once and kept for the process (CNG's providers are thread-safe).
        BCRYPT_ALG_HANDLE provider(const wchar_t* algorithm, ULONG flags)
        {
            BCRYPT_ALG_HANDLE h = nullptr;
            return BCryptOpenAlgorithmProvider(&h, algorithm, nullptr, flags) >= 0 ? h : nullptr;
        }

        bool randomBytes(uint8_t* out, std::size_t n)
        {
            return BCryptGenRandom(nullptr, out, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
        }

        bool pbkdf2(const std::string& secret, const uint8_t* salt, uint8_t* out, std::size_t n)
        {
            static const BCRYPT_ALG_HANDLE alg = provider(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
            if (alg == nullptr)
                return false;
            return BCryptDeriveKeyPBKDF2(alg, reinterpret_cast<PUCHAR>(const_cast<char*>(secret.data())),
                                         static_cast<ULONG>(secret.size()), const_cast<PUCHAR>(salt),
                                         static_cast<ULONG>(kSaltBytes), kRounds, out, static_cast<ULONG>(n), 0)
                >= 0;
        }

        std::array<uint8_t, kMacBytes> hmac(const std::array<uint8_t, 32>& key, const uint8_t* data, std::size_t n)
        {
            static const BCRYPT_ALG_HANDLE alg = provider(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
            std::array<uint8_t, kMacBytes> out{};
            BCRYPT_HASH_HANDLE h = nullptr;
            if (alg != nullptr
                && BCryptCreateHash(alg, &h, nullptr, 0, const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0) >= 0)
            {
                BCryptHashData(h, const_cast<PUCHAR>(data), static_cast<ULONG>(n), 0);
                BCryptFinishHash(h, out.data(), static_cast<ULONG>(out.size()), 0);
                BCryptDestroyHash(h);
            }
            return out;
        }

        // AES-256-CBC with PKCS#7 padding; `out` is sized to what was written. False when it fails (a bad pad).
        bool aesCbc(bool encrypt, const std::array<uint8_t, 32>& key, const uint8_t* iv, const uint8_t* in,
                    std::size_t n, std::vector<uint8_t>& out)
        {
            static const BCRYPT_ALG_HANDLE alg = [] {
                BCRYPT_ALG_HANDLE h = provider(BCRYPT_AES_ALGORITHM, 0);
                if (h != nullptr
                    && BCryptSetProperty(h, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                                         static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_CBC)), 0) < 0)
                    h = nullptr;
                return h;
            }();
            BCRYPT_KEY_HANDLE k = nullptr;
            if (alg == nullptr
                || BCryptGenerateSymmetricKey(alg, &k, nullptr, 0, const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0) < 0)
                return false;
            std::array<uint8_t, kIvBytes> chain{};       // CNG writes the chaining value back into the IV it is given
            std::copy(iv, iv + kIvBytes, chain.begin());
            out.assign(n + kBlockBytes, 0);
            ULONG written = 0;
            const NTSTATUS rc = encrypt
                ? BCryptEncrypt(k, const_cast<PUCHAR>(in), static_cast<ULONG>(n), nullptr, chain.data(), static_cast<ULONG>(chain.size()),
                                out.data(), static_cast<ULONG>(out.size()), &written, BCRYPT_BLOCK_PADDING)
                : BCryptDecrypt(k, const_cast<PUCHAR>(in), static_cast<ULONG>(n), nullptr, chain.data(), static_cast<ULONG>(chain.size()),
                                out.data(), static_cast<ULONG>(out.size()), &written, BCRYPT_BLOCK_PADDING);
            BCryptDestroyKey(k);
            out.resize(rc >= 0 ? written : 0);
            return rc >= 0;
        }

        // The account's part of the secret (macOS's uid): the user's name is in the secret already.
        std::string accountId() { return "win"; }
#else
        bool randomBytes(uint8_t* out, std::size_t n) { return CCRandomGenerateBytes(out, n) == kCCSuccess; }

        bool pbkdf2(const std::string& secret, const uint8_t* salt, uint8_t* out, std::size_t n)
        {
            return CCKeyDerivationPBKDF(kCCPBKDF2, secret.data(), secret.size(), salt, kSaltBytes, kCCPRFHmacAlgSHA256,
                                        kRounds, out, n)
                == kCCSuccess;
        }

        std::array<uint8_t, kMacBytes> hmac(const std::array<uint8_t, 32>& key, const uint8_t* data, std::size_t n)
        {
            static_assert(kMacBytes == CC_SHA256_DIGEST_LENGTH && kBlockBytes == kCCBlockSizeAES128);
            std::array<uint8_t, kMacBytes> out{};
            CCHmac(kCCHmacAlgSHA256, key.data(), key.size(), data, n, out.data());
            return out;
        }

        bool aesCbc(bool encrypt, const std::array<uint8_t, 32>& key, const uint8_t* iv, const uint8_t* in,
                    std::size_t n, std::vector<uint8_t>& out)
        {
            out.assign(n + kBlockBytes, 0);
            std::size_t written = 0;
            const bool ok = CCCrypt(encrypt ? kCCEncrypt : kCCDecrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding,
                                    key.data(), key.size(), iv, in, n, out.data(), out.size(), &written)
                         == kCCSuccess;
            out.resize(ok ? written : 0);
            return ok;
        }

        std::string accountId() { return std::to_string(static_cast<unsigned>(::getuid())); }
#endif

        // The folder private to the user (0700; on Windows, the user's own profile already is).
        void makePrivate([[maybe_unused]] const juce::File& f, [[maybe_unused]] int mode)
        {
#if !defined(_WIN32)
            ::chmod(f.getFullPathName().toRawUTF8(), static_cast<mode_t>(mode));
#endif
        }

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
            makePrivate(dir, 0700);
            return true;
        }

        // The encryption key and the MAC key, from this computer and account and the salt.
        bool deriveKeys(const uint8_t* salt, std::array<uint8_t, 32>& enc, std::array<uint8_t, 32>& mac)
        {
            const std::string secret = "funk-playground-key-v1|" + juce::SystemStats::getUniqueDeviceID().toStdString()
                                     + "|" + juce::SystemStats::getLogonName().toStdString() + "|" + accountId();
            std::array<uint8_t, 64> both{};
            if (!pbkdf2(secret, salt, both.data(), both.size()))
                return false;
            std::copy(both.begin(), both.begin() + 32, enc.begin());
            std::copy(both.begin() + 32, both.end(), mac.begin());
            return true;
        }

        bool sameBytes(const uint8_t* a, const uint8_t* b, std::size_t n)
        {
            uint8_t diff = 0;                        // constant time
            for (std::size_t i = 0; i < n; ++i)
                diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
            return diff == 0;
        }

        // Writes `bytes` to `file` (0600) through a temporary beside it, renamed over it. `unique`: the temporary's name
        // is this write's own (a chat's file, which two processes may write at once).
        bool writePrivate(const juce::File& file, const void* bytes, std::size_t n, std::string* why, bool unique = false)
        {
            std::string suffix = ".tmp";
            if (unique)
            {
                std::array<uint8_t, 8> r{};
                (void)randomBytes(r.data(), r.size());
                suffix = "." + juce::String::toHexString(r.data(), static_cast<int>(r.size()), 0).toStdString() + ".tmp";
            }
            const juce::File temp = file.getSiblingFile(file.getFileName() + juce::String(suffix));
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
            makePrivate(temp, 0600);
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
#if defined(_WIN32)
        // %APPDATA%\Funk\Playground (userApplicationDataDirectory is %APPDATA% there).
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("Funk")
            .getChildFile("Playground");
#else
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("Application Support")
            .getChildFile("Funk")
            .getChildFile("Playground");
#endif
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

    namespace
    {
        struct Keys
        {
            std::array<uint8_t, 32> enc{}, mac{};
        };

        // `secret` sealed under `magic` with `salt` (named in the bytes) and the keys made from it: the bytes of a file.
        bool sealBytes(const char* magic, const uint8_t* salt, const Keys& keys, std::string_view secret,
                       std::vector<uint8_t>& bytes, std::string* why)
        {
            std::array<uint8_t, kIvBytes> iv{};
            if (!randomBytes(iv.data(), iv.size()))
            {
                if (why != nullptr)
                    *why = "no random bytes";
                return false;
            }
            std::vector<uint8_t> cipher;
            if (!aesCbc(true, keys.enc, iv.data(), reinterpret_cast<const uint8_t*>(secret.data()), secret.size(), cipher))
            {
                if (why != nullptr)
                    *why = "the encryption failed";
                return false;
            }
            bytes.clear();
            bytes.reserve(4 + kSaltBytes + kIvBytes + cipher.size() + kMacBytes);
            bytes.insert(bytes.end(), magic, magic + 4);
            bytes.insert(bytes.end(), salt, salt + kSaltBytes);
            bytes.insert(bytes.end(), iv.begin(), iv.end());
            bytes.insert(bytes.end(), cipher.begin(), cipher.end());
            const auto tag = hmac(keys.mac, bytes.data(), bytes.size());
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            return true;
        }

        // What sealed bytes hold, opened (`keysFor` makes the keys of the salt they name); nothing, with why, when they
        // are damaged or were made elsewhere. `what` names them in the reasons ("key").
        std::optional<std::string> unsealBytes(const char* magic, const uint8_t* bytes, std::size_t n,
                                               const std::function<std::optional<Keys>(const uint8_t*)>& keysFor,
                                               const std::string& what, std::string* why)
        {
            const auto fail = [&](const std::string& reason) -> std::optional<std::string> {
                if (why != nullptr)
                    *why = reason;
                return std::nullopt;
            };
            constexpr std::size_t head = 4 + kSaltBytes + kIvBytes;
            if (n < head + kBlockBytes + kMacBytes || !sameBytes(bytes, reinterpret_cast<const uint8_t*>(magic), 4))
                return fail("the saved " + what + " is damaged");
            const std::optional<Keys> keys = keysFor(bytes + 4);
            if (!keys)
                return fail("the key derivation failed");
            const auto tag = hmac(keys->mac, bytes, n - kMacBytes);
            if (!sameBytes(tag.data(), bytes + n - kMacBytes, kMacBytes))
                return fail("the saved " + what + " was made on " + kAnotherMachine + " or account, or is damaged");
            const uint8_t* cipher = bytes + head;
            const std::size_t cipherBytes = n - head - kMacBytes;
            std::vector<uint8_t> plain;
            if (!aesCbc(false, keys->enc, bytes + 4 + kSaltBytes, cipher, cipherBytes, plain))
                return fail("the saved " + what + " does not decrypt");
            std::string secret(reinterpret_cast<const char*>(plain.data()), plain.size());
            std::fill(plain.begin(), plain.end(), uint8_t{ 0 });
            return secret;
        }

        std::optional<Keys> derived(const uint8_t* salt)
        {
            Keys k;
            if (!deriveKeys(salt, k.enc, k.mac))
                return std::nullopt;
            return k;
        }

        // `secret` sealed into `file` (ChatStore.h: FPK1, a fresh salt each time).
        bool seal(const juce::File& file, std::string_view secret, std::string* why)
        {
            if (!ensureDirectory(why))
                return false;
            std::array<uint8_t, kSaltBytes> salt{};
            if (!randomBytes(salt.data(), salt.size()))
            {
                if (why != nullptr)
                    *why = "no random bytes";
                return false;
            }
            const std::optional<Keys> keys = derived(salt.data());
            if (!keys)
            {
                if (why != nullptr)
                    *why = "the key derivation failed";
                return false;
            }
            std::vector<uint8_t> bytes;
            return sealBytes(kMagic, salt.data(), *keys, secret, bytes, why) && writePrivate(file, bytes.data(), bytes.size(), why);
        }

        // What `file` holds, opened; nothing, with why, when it is missing, damaged or made elsewhere. `what` names
        // it in the reasons ("key").
        std::optional<std::string> unseal(const juce::File& file, const std::string& what, std::string* why)
        {
            juce::MemoryBlock block;
            if (!file.loadFileAsData(block))
            {
                if (why != nullptr)
                    *why = "no " + what + " saved";
                return std::nullopt;
            }
            return unsealBytes(kMagic, static_cast<const uint8_t*>(block.getData()), block.getSize(), derived, what, why);
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
        if (!key && why != nullptr && why->find(kAnotherMachine) != std::string::npos)
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

    Choice loadChoice()
    {
        Choice c;
        const juce::var v = juce::JSON::parse(choiceFile());
        if (auto* o = v.getDynamicObject())
        {
            c.model = o->getProperty("model").toString().toStdString();
            c.effort = o->getProperty("effort").toString().toStdString();
            c.fast = static_cast<bool>(o->getProperty("fast"));
            c.web = static_cast<bool>(o->getProperty("web"));
            c.planner = o->getProperty("planner").toString().toStdString();
        }
        return c;
    }

    void saveChoice(const Choice& choice)
    {
        saveChatJson([&](juce::DynamicObject& o) {
            o.setProperty("model", juce::String(choice.model));
            o.setProperty("effort", juce::String(choice.effort));
            o.setProperty("fast", choice.fast);
            o.setProperty("web", choice.web);
            o.setProperty("planner", juce::String(choice.planner));
        });
    }

    void forgetOldSignIn()
    {
        const juce::File dir = directory();
        for (const char* name : { "account-token.bin", "account-token.bin.tmp" })
            if (const juce::File f = dir.getChildFile(name); f.existsAsFile())
                f.deleteFile();
        const juce::var v = juce::JSON::parse(choiceFile());
        const auto* o = v.getDynamicObject();
        if (o == nullptr || (!o->hasProperty("account") && !o->hasProperty("use")))
            return;
        saveChatJson([](juce::DynamicObject& json) {
            json.removeProperty("account");
            json.removeProperty("use");
        });
    }

    // ---- the chats (ChatStore.h) -----------------------------------------------------------------------------------------
    namespace
    {
        constexpr char kChatMagic[4] = { 'F', 'P', 'C', '1' };
        constexpr const char* kChatExtension = ".chat";
        constexpr const char* kMetaExtension = ".meta";
        constexpr const char* kSaltFile = ".salt";

        // The chats' keys by the salt they were made from: PBKDF2's 100,000 rounds once a salt in a process, so a list
        // of every chat of a project (each file sealed under the store's salt) costs one derivation, not one a file.
        std::mutex keysMutex;
        std::map<std::array<uint8_t, kSaltBytes>, Keys> chatKeys;

        std::optional<Keys> keysOf(const uint8_t* salt)
        {
            std::array<uint8_t, kSaltBytes> s{};
            std::copy(salt, salt + kSaltBytes, s.begin());
            const std::lock_guard<std::mutex> lock(keysMutex);
            if (const auto it = chatKeys.find(s); it != chatKeys.end())
                return it->second;
            std::optional<Keys> k = derived(salt);
            if (!k)
                return std::nullopt;
            if (chatKeys.size() >= 16)                   // salts of files written elsewhere: a few at most
                chatKeys.clear();
            chatKeys.emplace(s, *k);
            return k;
        }

        bool privateFolder(const juce::File& dir, std::string* why)
        {
            if (!dir.isDirectory() && !dir.createDirectory())
            {
                if (why != nullptr)
                    *why = "cannot create " + dir.getFullPathName().toStdString();
                return false;
            }
            makePrivate(dir, 0700);
            return true;
        }

        // The store's folders, made private: the chat's folder, chats/, and the owner's.
        bool ownerFolder(std::string_view owner, juce::File& dir, std::string* why)
        {
            if (!validOwner(owner))
            {
                if (why != nullptr)
                    *why = "not a chat's owner";
                return false;
            }
            dir = chatsDirectory().getChildFile(juce::String(std::string(owner)));
            return ensureDirectory(why) && privateFolder(chatsDirectory(), why) && privateFolder(dir, why);
        }

        juce::File ownerPath(std::string_view owner)
        {
            return validOwner(owner) ? chatsDirectory().getChildFile(juce::String(std::string(owner))) : juce::File();
        }

        // The store's salt (chats/.salt, made once, 0600). Two processes that make it at once each seal with their
        // own: either reads, since a file names its salt.
        bool storeSalt(std::array<uint8_t, kSaltBytes>& salt, std::string* why)
        {
            const juce::File f = chatsDirectory().getChildFile(kSaltFile);
            juce::MemoryBlock b;
            if (f.loadFileAsData(b) && b.getSize() == kSaltBytes)
            {
                std::copy(static_cast<const uint8_t*>(b.getData()), static_cast<const uint8_t*>(b.getData()) + kSaltBytes, salt.begin());
                return true;
            }
            if (!randomBytes(salt.data(), salt.size()))
            {
                if (why != nullptr)
                    *why = "no random bytes";
                return false;
            }
            return writePrivate(f, salt.data(), salt.size(), why, true);
        }

        bool sealChat(const juce::File& file, std::string_view text, std::string* why)
        {
            std::array<uint8_t, kSaltBytes> salt{};
            if (!storeSalt(salt, why))
                return false;
            const std::optional<Keys> keys = keysOf(salt.data());
            if (!keys)
            {
                if (why != nullptr)
                    *why = "the key derivation failed";
                return false;
            }
            std::vector<uint8_t> bytes;
            return sealBytes(kChatMagic, salt.data(), *keys, text, bytes, why)
                && writePrivate(file, bytes.data(), bytes.size(), why, true);
        }

        std::optional<std::string> unsealChat(const juce::File& file, std::string* why)
        {
            juce::MemoryBlock block;
            if (!file.loadFileAsData(block))
            {
                if (why != nullptr)
                    *why = "there is no such chat";
                return std::nullopt;
            }
            return unsealBytes(kChatMagic, static_cast<const uint8_t*>(block.getData()), block.getSize(), keysOf, "chat", why);
        }

        Stamp stampOf(const juce::File& f)
        {
            if (!f.existsAsFile())
                return {};
            return { f.getLastModificationTime().toMilliseconds(), f.getSize() };
        }

        juce::File chatFile(const juce::File& dir, std::string_view id) { return dir.getChildFile(juce::String(std::string(id)) + kChatExtension); }
        juce::File metaFile(const juce::File& dir, std::string_view id) { return dir.getChildFile(juce::String(std::string(id)) + kMetaExtension); }

        std::string metaJson(const ChatMeta& m)
        {
            auto* o = new juce::DynamicObject();
            const juce::var v(o);
            o->setProperty("format", "funk-playground-chat");
            o->setProperty("version", 1);
            o->setProperty("id", juce::String(m.id));
            o->setProperty("title", juce::String::fromUTF8(m.title.c_str()));
            o->setProperty("named", m.named);
            o->setProperty("created", static_cast<juce::int64>(m.createdMs));
            o->setProperty("updated", static_cast<juce::int64>(m.updatedMs));
            return juce::JSON::toString(v, true).toStdString();
        }

        // A chat's .meta, or what its .chat file says of it when that is missing or damaged.
        ChatMeta readMeta(const juce::File& dir, std::string_view id)
        {
            ChatMeta m;
            m.id = std::string(id);
            if (const std::optional<std::string> text = unsealChat(metaFile(dir, id), nullptr))
            {
                const juce::var v = juce::JSON::parse(juce::String::fromUTF8(text->c_str()));
                if (v.getDynamicObject() != nullptr && v["format"].toString() == "funk-playground-chat")
                {
                    m.title = tidyTitle(v["title"].toString().toStdString());
                    m.named = static_cast<bool>(v["named"]);
                    m.createdMs = static_cast<int64_t>(static_cast<juce::int64>(v["created"]));
                    m.updatedMs = static_cast<int64_t>(static_cast<juce::int64>(v["updated"]));
                }
            }
            if (m.updatedMs <= 0)
            {
                const juce::File f = chatFile(dir, id);
                m.updatedMs = f.getLastModificationTime().toMilliseconds();
                if (m.createdMs <= 0)
                    m.createdMs = m.updatedMs;
            }
            if (m.title.empty())
                m.title = "Chat";
            return m;
        }

        std::mutex writeMutex;                           // this process's chat writes, one at a time
    }

    std::string newId()
    {
        std::array<uint8_t, 16> r{};
        (void)randomBytes(r.data(), r.size());
        return juce::String::toHexString(r.data(), static_cast<int>(r.size()), 0).toLowerCase().toStdString();
    }

    bool validId(std::string_view id)
    {
        return id.size() == 32 && std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }

    bool validOwner(std::string_view owner)
    {
        if (owner.empty() || owner.size() > 64 || owner.front() == '-')
            return false;
        return std::all_of(owner.begin(), owner.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '-'; });
    }

    std::string instanceOwner(std::string_view instanceId) { return std::string(kInstancePrefix) + std::string(instanceId); }

    std::string tidyTitle(std::string_view title)
    {
        // The spaces (tabs, line breaks) collapsed and trimmed, control characters dropped, at most kMaxTitle characters
        // (UTF-8 kept whole: the panel draws it as asciiText).
        std::string out;
        bool space = false;
        std::size_t chars = 0;
        for (std::size_t i = 0; i < title.size() && chars < kMaxTitle;)
        {
            const auto c = static_cast<unsigned char>(title[i]);
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            {
                space = !out.empty();
                ++i;
                continue;
            }
            if (c < 0x20 || c == 0x7F)
            {
                ++i;
                continue;
            }
            const std::size_t n = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            if (space)
            {
                out += ' ';
                ++chars;
                space = false;
                if (chars >= kMaxTitle)
                    break;
            }
            out.append(title.substr(i, std::min(n, title.size() - i)));
            ++chars;
            i += n;
        }
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        return out;
    }

    juce::File chatsDirectory() { return directory().getChildFile("chats"); }

    std::vector<ChatMeta> chatsOf(std::string_view owner)
    {
        std::vector<ChatMeta> out;
        const juce::File dir = ownerPath(owner);
        if (dir == juce::File() || !dir.isDirectory())
            return out;
        for (const juce::File& f : dir.findChildFiles(juce::File::findFiles, false, juce::String("*") + kChatExtension))
        {
            const std::string id = f.getFileNameWithoutExtension().toStdString();
            if (validId(id))
                out.push_back(readMeta(dir, id));
        }
        std::sort(out.begin(), out.end(), [](const ChatMeta& a, const ChatMeta& b) {
            return a.updatedMs != b.updatedMs ? a.updatedMs > b.updatedMs : a.id < b.id;
        });
        return out;
    }

    bool hasChat(std::string_view owner, std::string_view id)
    {
        const juce::File dir = ownerPath(owner);
        return dir != juce::File() && validId(id) && chatFile(dir, id).existsAsFile();
    }

    bool hasOwner(std::string_view owner)
    {
        const juce::File dir = ownerPath(owner);
        return dir != juce::File() && dir.isDirectory();
    }

    bool hasChats(std::string_view owner)
    {
        const juce::File dir = ownerPath(owner);
        if (dir == juce::File() || !dir.isDirectory())
            return false;
        for (const juce::File& f : dir.findChildFiles(juce::File::findFiles, false, juce::String("*") + kChatExtension))
            if (validId(f.getFileNameWithoutExtension().toStdString()))
                return true;
        return false;
    }

    std::optional<Loaded> loadChat(std::string_view owner, std::string_view id, std::string* why)
    {
        const juce::File dir = ownerPath(owner);
        if (dir == juce::File() || !validId(id))
        {
            if (why != nullptr)
                *why = "there is no such chat";
            return std::nullopt;
        }
        const juce::File f = chatFile(dir, id);
        Loaded l;
        l.stamp = stampOf(f);
        std::optional<std::string> state = unsealChat(f, why);
        if (!state)
            return std::nullopt;
        l.state = std::move(*state);
        l.meta = readMeta(dir, id);
        return l;
    }

    SaveResult saveChat(std::string_view owner, const ChatMeta& meta, const std::string& state, Stamp& stamp, std::string* why)
    {
        juce::File dir;
        if (!validId(meta.id))
        {
            if (why != nullptr)
                *why = "not a chat's id";
            return SaveResult::failed;
        }
        const std::lock_guard<std::mutex> lock(writeMutex);
        if (!ownerFolder(owner, dir, why))
            return SaveResult::failed;
        const juce::File f = chatFile(dir, meta.id);
        if (f.existsAsFile() && (stamp.none() || stampOf(f) != stamp))
            return SaveResult::conflict;
        ChatMeta m = meta;
        m.title = tidyTitle(m.title);
        if (!sealChat(f, state, why) || !sealChat(metaFile(dir, m.id), metaJson(m), why))
            return SaveResult::failed;
        stamp = stampOf(f);
        return SaveResult::ok;
    }

    bool renameChat(std::string_view owner, std::string_view id, std::string_view title)
    {
        const std::lock_guard<std::mutex> lock(writeMutex);
        const juce::File dir = ownerPath(owner);
        if (dir == juce::File() || !validId(id) || !chatFile(dir, id).existsAsFile())
            return false;
        ChatMeta m = readMeta(dir, id);
        m.title = tidyTitle(title);
        m.named = !m.title.empty();
        if (m.title.empty())
            m.title = "Chat";
        return sealChat(metaFile(dir, id), metaJson(m), nullptr);
    }

    bool removeChat(std::string_view owner, std::string_view id)
    {
        const std::lock_guard<std::mutex> lock(writeMutex);
        const juce::File dir = ownerPath(owner);
        if (dir == juce::File() || !validId(id))
            return false;
        const juce::File c = chatFile(dir, id), m = metaFile(dir, id);
        const bool had = c.existsAsFile();
        c.deleteFile();
        m.deleteFile();
        return had && !c.existsAsFile();
    }

    bool moveChat(std::string_view from, std::string_view to, std::string_view id)
    {
        const std::lock_guard<std::mutex> lock(writeMutex);
        const juce::File src = ownerPath(from);
        juce::File dst;
        if (src == juce::File() || !validId(id) || !chatFile(src, id).existsAsFile() || !ownerFolder(to, dst, nullptr))
            return false;
        if (chatFile(dst, id).existsAsFile())
            return false;
        if (!chatFile(src, id).moveFileTo(chatFile(dst, id)))
            return false;
        if (metaFile(src, id).existsAsFile())
            metaFile(src, id).moveFileTo(metaFile(dst, id));
        if (src.getNumberOfChildFiles(juce::File::findFilesAndDirectories) == 0)
            src.deleteFile();                            // an empty owner's folder goes
        return true;
    }

    bool removeOwner(std::string_view owner)
    {
        const std::lock_guard<std::mutex> lock(writeMutex);
        const juce::File dir = ownerPath(owner);
        return dir == juce::File() || !dir.exists() || dir.deleteRecursively();
    }
}
