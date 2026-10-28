// PROBE name=plugin.chatstore timeout=60
//
// plugin.chatstore: the chat's key and choices on disk (ChatStore.h), in a folder of the probe's own.
//   SHAPE   plausibleKey takes "sk-ant-" and 20 to 300 letters, digits, '-' and '_', nothing else
//   NONE    with no file: no key, and loadKey says why
//   SAVE    a key that is not one is refused (with why) and writes nothing; a key is saved: the file is "FPK1", salt,
//           IV, whole AES blocks and an HMAC; it holds neither the key nor any 12 characters of it; the file is 0600
//           and the folder 0700; it loads back the same; saved twice it is written differently (a fresh salt and IV)
//   TAMPER  one byte changed anywhere (magic, salt, IV, ciphertext, HMAC), or the file cut short: the key does not
//           load, and loadKey says why
//   FORGET  forgetKey deletes the file: no key
//   TOKEN   the server's sign-in token, as the key: plausibleToken takes "tpb_" and 20 to 200 letters, digits, '-' and
//           '_'; a token that is not one is refused and writes nothing; saved, account-token.bin is "FPK1", salt, IV,
//           whole AES blocks and an HMAC, holds no 12 characters of the token, is 0600, and loads back the same; one
//           byte changed, it does not load (with why); forgetToken deletes it, and leaves the key alone
//   CHOICE  the model, thinking level and fast mode round-trip; a damaged chat.json loads as no choice
//   ACCOUNT the username and which access the chat uses round-trip; saving the choice keeps them and saving them keeps
//           the choice; never chosen, the chat uses the account unless only a key is stored
//   SANDBOX under CTest (<ENV_PREFIX>PREFS_DIR set) the default folder is the sandbox's chat/, never the user's
#include "Probe.h"

#include "plugin/ChatStore.h"

#include "Product.h"

#include "FakeClaude.h"

#include <juce_core/juce_core.h>

#include <sys/stat.h>

#include <cstdlib>
#include <string>

namespace pg = playground;
namespace store = playground::chatstore;

namespace
{
    juce::MemoryBlock readAll(const juce::File& f)
    {
        juce::MemoryBlock m;
        f.loadFileAsData(m);
        return m;
    }

    bool contains(const juce::MemoryBlock& m, const std::string& needle)
    {
        const std::string hay(static_cast<const char*>(m.getData()), m.getSize());
        return hay.find(needle) != std::string::npos;
    }

    int mode(const juce::File& f)
    {
        struct stat st {};
        if (::stat(f.getFullPathName().toRawUTF8(), &st) != 0)
            return -1;
        return static_cast<int>(st.st_mode & 0777);
    }
}

PROBE(plugin, chatstore)
{
    // SANDBOX, before the probe's own folder replaces it.
    {
        const std::string var = std::string(pg::product::kEnvPrefix) + "PREFS_DIR";
        if (const char* dir = std::getenv(var.c_str()); dir != nullptr && *dir != '\0')
            P.eq("sandbox.default_in_sandbox",
                 store::directory() == juce::File(juce::String::fromUTF8(dir)).getChildFile("chat") ? 1 : 0, 1);
    }

    const juce::File dir = juce::File(juce::String(probe::outPath("chatstore"))).getChildFile("Playground");
    dir.getParentDirectory().deleteRecursively();
    store::setDirectoryForTests(dir);
    const juce::File keyFile = dir.getChildFile("anthropic-key.bin");
    const std::string key = fake::fakeKey();

    // SHAPE
    P.eq("shape.fake_key", store::plausibleKey(key) ? 1 : 0, 1);
    P.eq("shape.prefix_only", store::plausibleKey("sk-ant-") ? 1 : 0, 0);
    P.eq("shape.short", store::plausibleKey("sk-ant-abc123") ? 1 : 0, 0);
    P.eq("shape.other_prefix", store::plausibleKey("sk-proj-0123456789abcdefghijklmnop") ? 1 : 0, 0);
    P.eq("shape.space", store::plausibleKey("sk-ant-0123456789 abcdefghijklmnop") ? 1 : 0, 0);
    P.eq("shape.long", store::plausibleKey("sk-ant-" + std::string(301, 'a')) ? 1 : 0, 0);
    P.eq("shape.longest", store::plausibleKey("sk-ant-" + std::string(300, 'a')) ? 1 : 0, 1);

    // NONE
    P.eq("none.has_key", store::hasKey() ? 1 : 0, 0);
    std::string why;
    P.eq("none.loads", store::loadKey(&why).has_value() ? 1 : 0, 0);
    P.eq("none.why_said", why.empty() ? 0 : 1, 1);

    // SAVE
    why.clear();
    P.eq("save.refuses_non_key", store::saveKey("hello", &why) ? 1 : 0, 0);
    P.eq("save.refusal_why", why.empty() ? 0 : 1, 1);
    P.eq("save.refusal_writes_nothing", keyFile.exists() ? 1 : 0, 0);
    P.eq("save.saves", store::saveKey(key, &why) ? 1 : 0, 1);
    P.eq("save.has_key", store::hasKey() ? 1 : 0, 1);
    const juce::MemoryBlock first = readAll(keyFile);
    P.eq("save.magic", first.getSize() >= 4 && std::string(static_cast<const char*>(first.getData()), 4) == "FPK1" ? 1 : 0, 1);
    const int64_t cipher = static_cast<int64_t>(first.getSize()) - 4 - 16 - 16 - 32;
    P.eq("save.whole_blocks", cipher > 0 && cipher % 16 == 0 ? 1 : 0, 1);
    P.ge("save.covers_key", static_cast<double>(cipher), static_cast<double>(key.size()));
    int leaks = 0;
    for (std::size_t p = 0; p + 12 <= key.size(); ++p)
        leaks += contains(first, key.substr(p, 12)) ? 1 : 0;
    P.eq("save.no_plaintext", leaks, 0);
    P.eq("save.file_0600", mode(keyFile), 0600);
    P.eq("save.folder_0700", mode(dir), 0700);
    const auto loaded = store::loadKey(&why);
    P.eq("save.round_trip", loaded.has_value() && *loaded == key ? 1 : 0, 1);
    P.eq("save.again", store::saveKey(key, &why) ? 1 : 0, 1);
    const juce::MemoryBlock second = readAll(keyFile);
    P.eq("save.fresh_salt_iv", second == first ? 0 : 1, 1);
    P.eq("save.again_loads", store::loadKey().value_or("") == key ? 1 : 0, 1);

    // TAMPER: each part, one byte flipped; then cut short.
    const struct
    {
        const char* name;
        std::size_t at;
    } spots[] = { { "magic", 0 },
                  { "salt", 4 + 3 },
                  { "iv", 20 + 5 },
                  { "cipher", 36 + 7 },
                  { "hmac", second.getSize() - 9 } };
    for (const auto& s : spots)
    {
        juce::MemoryBlock bad = second;
        static_cast<char*>(bad.getData())[s.at] ^= 0x20;
        keyFile.replaceWithData(bad.getData(), bad.getSize());
        why.clear();
        P.eq(std::string("tamper.") + s.name + "_refused", store::loadKey(&why).has_value() ? 1 : 0, 0);
        P.eq(std::string("tamper.") + s.name + "_why", why.empty() ? 0 : 1, 1);
    }
    keyFile.replaceWithData(second.getData(), second.getSize() - 20);
    P.eq("tamper.short_refused", store::loadKey().has_value() ? 1 : 0, 0);
    keyFile.replaceWithData(second.getData(), second.getSize());
    P.eq("tamper.restored_loads", store::loadKey().value_or("") == key ? 1 : 0, 1);

    // FORGET
    P.eq("forget.forgets", store::forgetKey() ? 1 : 0, 1);
    P.eq("forget.file_gone", keyFile.exists() ? 1 : 0, 0);
    P.eq("forget.has_key", store::hasKey() ? 1 : 0, 0);

    // TOKEN
    {
        const juce::File tokenFile = dir.getChildFile("account-token.bin");
        const std::string token = fake::fakeToken();
        P.eq("token.shape_fake", store::plausibleToken(token) ? 1 : 0, 1);
        P.eq("token.shape_prefix_only", store::plausibleToken("tpb_") ? 1 : 0, 0);
        P.eq("token.shape_short", store::plausibleToken("tpb_abc") ? 1 : 0, 0);
        P.eq("token.shape_key", store::plausibleToken(key) ? 1 : 0, 0);
        P.eq("token.shape_slash", store::plausibleToken("tpb_0123456789/abcdefghijklmnop") ? 1 : 0, 0);
        P.eq("token.shape_longest", store::plausibleToken("tpb_" + std::string(200, 'a')) ? 1 : 0, 1);
        P.eq("token.shape_long", store::plausibleToken("tpb_" + std::string(201, 'a')) ? 1 : 0, 0);
        P.eq("token.none", store::hasToken() || store::loadToken().has_value() ? 1 : 0, 0);
        why.clear();
        P.eq("token.refuses_non_token", store::saveToken("hello", &why) ? 1 : 0, 0);
        P.eq("token.refusal_why", why.empty() ? 0 : 1, 1);
        P.eq("token.refusal_writes_nothing", tokenFile.exists() ? 1 : 0, 0);
        store::saveKey(key);
        P.eq("token.saves", store::saveToken(token, &why) ? 1 : 0, 1);
        const juce::MemoryBlock t = readAll(tokenFile);
        P.eq("token.magic", t.getSize() >= 4 && std::string(static_cast<const char*>(t.getData()), 4) == "FPK1" ? 1 : 0, 1);
        const int64_t tc = static_cast<int64_t>(t.getSize()) - 4 - 16 - 16 - 32;
        P.eq("token.whole_blocks", tc > 0 && tc % 16 == 0 ? 1 : 0, 1);
        int tokenLeaks = 0;
        for (std::size_t p = 0; p + 12 <= token.size(); ++p)
            tokenLeaks += contains(t, token.substr(p, 12)) ? 1 : 0;
        P.eq("token.no_plaintext", tokenLeaks, 0);
        P.eq("token.file_0600", mode(tokenFile), 0600);
        P.eq("token.round_trip", store::loadToken().value_or("") == token ? 1 : 0, 1);
        juce::MemoryBlock bad = t;
        static_cast<char*>(bad.getData())[40] ^= 0x20;
        tokenFile.replaceWithData(bad.getData(), bad.getSize());
        why.clear();
        P.eq("token.tamper_refused", store::loadToken(&why).has_value() ? 1 : 0, 0);
        P.eq("token.tamper_why", why.empty() ? 0 : 1, 1);
        tokenFile.replaceWithData(t.getData(), t.getSize());
        P.eq("token.forgets", store::forgetToken() && !tokenFile.exists() && !store::hasToken() ? 1 : 0, 1);
        P.eq("token.key_left_alone", store::loadKey().value_or("") == key ? 1 : 0, 1);
        store::forgetKey();
    }

    // CHOICE
    store::saveChoice({ "claude-sonnet-5-5", "high", true });
    const store::Choice c = store::loadChoice();
    P.eq("choice.model", c.model == "claude-sonnet-5-5" ? 1 : 0, 1);
    P.eq("choice.effort", c.effort == "high" ? 1 : 0, 1);
    P.eq("choice.fast", c.fast ? 1 : 0, 1);
    store::saveChoice({ "claude-opus-5-5", "medium", false });
    P.eq("choice.fast_off", store::loadChoice().fast ? 1 : 0, 0);
    dir.getChildFile("chat.json").replaceWithText("{ not json");
    const store::Choice damaged = store::loadChoice();
    P.eq("choice.damaged_is_none", damaged.model.empty() && damaged.effort.empty() && !damaged.fast ? 1 : 0, 1);

    // ACCOUNT
    {
        dir.getChildFile("chat.json").deleteFile();
        P.eq("account.never_chosen_nothing", store::loadAccount().useAccount ? 1 : 0, 1);
        store::saveKey(key);
        P.eq("account.never_chosen_key_only", store::loadAccount().useAccount ? 1 : 0, 0);
        store::saveToken(fake::fakeToken());
        P.eq("account.never_chosen_both", store::loadAccount().useAccount ? 1 : 0, 1);
        store::saveAccount({ "alice", false });
        P.eq("account.round_trip", store::loadAccount().username == "alice" && !store::loadAccount().useAccount ? 1 : 0, 1);
        store::saveChoice({ "claude-haiku-5-5", "low", false });
        P.eq("account.choice_keeps_it", store::loadAccount().username == "alice" && !store::loadAccount().useAccount ? 1 : 0, 1);
        store::saveAccount({ "bob", true });
        P.eq("account.keeps_choice", store::loadChoice().model == "claude-haiku-5-5" && store::loadChoice().effort == "low" ? 1 : 0, 1);
        P.eq("account.chosen", store::loadAccount().username == "bob" && store::loadAccount().useAccount ? 1 : 0, 1);
        store::forgetKey();
        store::forgetToken();
    }

    store::setDirectoryForTests(juce::File());
    dir.getParentDirectory().deleteRecursively();
    return P.finish();
}
