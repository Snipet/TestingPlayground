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
//   CHOICE  the model, thinking level and fast mode round-trip; a damaged chat.json loads as no choice
//   OLD     what a build from before 2026-10-28 kept of a sign-in to a server (account-token.bin, sealed as the key's
//           file is, a temporary beside it, and chat.json's "account" and "use"): forgetOldSignIn deletes the files and
//           the two fields and keeps the choice and the key; with nothing of it there it changes nothing (no chat.json
//           made); a damaged chat.json, or a token's file that is not FPK1, is no trouble
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

    // OLD: a sign-in an earlier build kept. Its token's file was sealed as the key's is: a copy of the key's file
    // stands in for it.
    {
        const juce::File tokenFile = dir.getChildFile("account-token.bin");
        const juce::File tokenTemp = dir.getChildFile("account-token.bin.tmp");
        const juce::File chatJson = dir.getChildFile("chat.json");
        store::saveKey(key);
        P.eq("old.sealed_like_the_key", keyFile.copyFileTo(tokenFile) ? 1 : 0, 1);
        tokenTemp.replaceWithText("FPK1");
        chatJson.replaceWithText("{ \"model\": \"claude-sonnet-5-5\", \"effort\": \"high\", \"fast\": true, "
                                 "\"account\": \"alice\", \"use\": \"account\" }\n");
        store::forgetOldSignIn();
        P.eq("old.token_gone", tokenFile.exists() || tokenTemp.exists() ? 1 : 0, 0);
        const juce::String json = chatJson.loadFileAsString();
        P.eq("old.fields_gone", json.contains("\"account\"") || json.contains("\"use\"") || json.contains("alice") ? 1 : 0, 0);
        const store::Choice kept = store::loadChoice();
        P.eq("old.choice_kept", kept.model == "claude-sonnet-5-5" && kept.effort == "high" && kept.fast ? 1 : 0, 1);
        P.eq("old.key_kept", store::loadKey().value_or("") == key ? 1 : 0, 1);
        P.eq("old.chat_json_0600", mode(chatJson), 0600);
        const juce::String before = chatJson.loadFileAsString();
        store::forgetOldSignIn();
        P.eq("old.again_nothing", chatJson.loadFileAsString() == before ? 1 : 0, 1);
        chatJson.deleteFile();
        store::forgetOldSignIn();
        P.eq("old.none_no_chat_json", chatJson.exists() ? 1 : 0, 0);
        // Damaged: chat.json not JSON, a token's file of a few bytes.
        chatJson.replaceWithText("{ \"account\": ");
        tokenFile.replaceWithText("tpb");
        store::forgetOldSignIn();
        P.eq("old.damaged_token_gone", tokenFile.exists() ? 1 : 0, 0);
        P.eq("old.damaged_json_left", chatJson.loadFileAsString() == "{ \"account\": " ? 1 : 0, 1);
        P.eq("old.damaged_json_no_choice", store::loadChoice().model.empty() ? 1 : 0, 1);
        chatJson.deleteFile();
        store::forgetKey();
    }

    store::setDirectoryForTests(juce::File());
    dir.getParentDirectory().deleteRecursively();
    return P.finish();
}
