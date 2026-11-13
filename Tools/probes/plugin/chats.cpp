// PROBE name=plugin.chats timeout=240
//
// plugin.chats: the chats, kept on this Mac only (plugin/ChatStore.h's chats; ChatAgent.h, "The chats"; CLAUDE.md, "The
// chats: on this Mac only"), over real processors whose chat talks to a scripted API (FakeClaude.h): nothing reaches
// the network, and the key's, the chats' and the projects' folders are the probe's own.
//   STORE    a chat written: its .chat and .meta sealed ("FPC1"), 0600 in 0700 folders, the store's salt 0600, no word
//            of it in clear anywhere under the folder; it loads back; a byte changed: refused, saying why; another
//            writer's change since it was read, of the same size and time (only the file's identity tells it): a
//            conflict, nothing written; files not sealed here (a chat and its .meta, a chat alone) are not listed, a
//            chat sealed here without its .meta is, titled "Chat"; an owner or an id that is no folder's
//            name (../, upper case, a slash) is refused before it reaches a path
//   ID       a project gets a UUID when it is first saved, kept by its next save; renamed in the Finder it keeps it
//            (load, idOf), and nameOfId finds it by it; a copy made in the Finder gets an id of its own when opened,
//            the original keeping its; a project from before ids gets one when opened, written into its file; a
//            project read before a save rewrote it (one from before ids, a Finder copy) gets its id written into the
//            file as it is now (the save's own id, or one of its own), the version saved kept
//   SWITCH   a chat in project A; opening B (no chats yet) shows a new chat, A's kept; a chat in B; opening A again
//            brings A's back (its words, its history), and its next request sends A's thinking block unchanged
//            (preserved thinking); B's comes back with B
//   CARRY    a chat with no project open (an example started afresh), the editor saved as project C: the chat goes
//            with C (none left with the instance); C saved as D: D gets a copy, C keeps its own
//   LIST     a new chat: the list has it first (not written yet) and the one before; written, the newest first;
//            renamed (the list and the file say so), another opened, the name cleared (its first message's words
//            again), deleted (the newest other one shows, the files gone), an id that is none refused; a chat listed
//            that cannot be read: opening it changes nothing, and the chat shown says why
//   SESSION  the session holds no word of the chat and no "json", only the instance, the owner and the chat's id;
//            another processor loading it shows the same chat; on another Mac (an empty store) a new, empty chat;
//            a project renamed in the Finder since the session named it is found by its id, its chat with it
//   MIGRATE  a session from before (its conversation in <CHAT json>): loaded, the chat shows and is kept on this Mac
//            (sealed, the instance's), the next session holds none of it; loaded again, the same instance and chat,
//            kept once; old sessions whose chat was empty or a lone note: nothing adopted, an instance of its own each,
//            a chat started in one in none of the others, and still in its own when its session is loaded again
//   RENAME   a project renamed in the Finder since the session named it: found by its id, its chat with it
//   EXPORT   a .fmpack and a .zip of a project with chats hold none of them, nor its id; the zip's README says the
//            chats stay; the package imported is a project with an id of its own and no chat
//   DELETE   a project deleted: its chats with it
//   MCP      Claude Desktop's get_status holds no word of the chat
//   COPIES   one state in several instances: a template's (from an instance that kept no chat) gives each instance its
//            own, and a later one none of theirs; a state naming its instance's chat, in two live instances (a track
//            duplicated): the first is that instance, the second shows a copy as its own (its thinking with it), and
//            loaded again neither holds or lists the other's words; a new chat shown at the save: shown so again, the
//            instance's earlier chat in the list, never in its place
//   SHARED   one project's chat in two instances, both writing: two chats, neither's words lost
//   SAVED    the project open's file deleted in the Finder, then a save: a new project with a new id, the chat moved to
//            it (none left with the old id), the session naming the new id, and found by it when loaded
//   GONE     a project deleted in another instance: this one's chat of it is not written back (the chat says why);
//            saved as a project, it goes on with that one, and the chat says it is kept again
//   PICTURES a view_panel picture (fake, SoftRaster): kept, sealed, in the chat's file; the project left and opened
//            again, the next request still carries the signed thinking and the picture
//   STOPWAIT a project opened while view_panel waits for the message thread (this one, not polling): back at once
//   KEEPFAIL the instance's folder cannot be made: the chat says so once, nothing kept; made possible, the next save
//            keeps it and the chat says so
//   CLOSE    a change not kept yet (Claude Desktop's work) is kept when the processor goes
#include "Probe.h"

#include "FakeClaude.h"

#include "editor/Picture.h"
#include "plugin/ChatAgent.h"
#include "plugin/ChatStore.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"
#include "plugin/ProjectExport.h"
#include "plugin/ProjectStore.h"

#include "Examples.h"
#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <juce_core/juce_core.h>

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace pg = playground;
namespace cs = playground::chatstore;
namespace ps = playground::projectstore;

namespace
{
    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    std::string bytesOf(const juce::File& f)
    {
        juce::MemoryBlock m;
        f.loadFileAsData(m);
        return std::string(static_cast<const char*>(m.getData()), m.getSize());
    }

    std::string bytesOf(const juce::MemoryBlock& m) { return std::string(static_cast<const char*>(m.getData()), m.getSize()); }

    [[maybe_unused]] int mode(const juce::File& f)
    {
        struct stat st {};
        if (::stat(f.getFullPathName().toRawUTF8(), &st) != 0)
            return -1;
        return static_cast<int>(st.st_mode & 0777);
    }

    // Every file under `dir` that holds `phrase` in its bytes.
    int filesHolding(const juce::File& dir, const std::string& phrase)
    {
        int n = 0;
        for (const juce::File& f : dir.findChildFiles(juce::File::findFiles, true, "*"))
            n += has(bytesOf(f), phrase) ? 1 : 0;
        return n;
    }

    // What the chat says, every entry's text.
    std::string said(const pg::ChatState& s)
    {
        std::string out;
        for (const pg::ChatEntry& e : s.entries)
            out += e.text + "\n";
        return out;
    }

    std::shared_ptr<fake::Script> wire(pg::Processor& p)
    {
        auto script = std::make_shared<fake::Script>();
        script->models = fake::modelsBody({ { "claude-opus-5-5", "Claude Opus 5.5", true, { "low", "medium", "high", "xhigh", "max" } } });
        p.chatAgent().setTransport(std::make_unique<fake::Transport>(script));
        return script;
    }

    // One turn: the user's words, Claude's thinking (signed) and its answer.
    void turn(pg::Processor& p, fake::Script& s, const std::string& words, const std::string& thought, const std::string& signature,
              const std::string& answer)
    {
        s.add(fake::Response::ok(fake::Sse().start().thinking(thought, signature).text(answer).end()));
        p.chatAgent().send(words);
        p.chatAgent().waitIdle();
    }

    std::string idInFile(const std::string& name)
    {
        const juce::var v = juce::JSON::parse(ps::directory().getChildFile(juce::String(name) + ps::kExtension));
        return v["id"].toString().toStdString();
    }

    pg::Sources program(const std::string& comment) { return { { "program.fm", std::string(pg::kExamples[0].source) + "\n// " + comment + "\n" } }; }

    // A session as a build from before the chats wrote it: a fresh processor's state, its <CHAT> holding `json` (the
    // conversation, as that build's saveState wrote it).
    juce::MemoryBlock oldSession(const std::string& json)
    {
        pg::Processor maker;
        juce::MemoryBlock fresh;
        maker.getStateInformation(fresh);
        std::unique_ptr<juce::XmlElement> xml = juce::AudioProcessor::getXmlFromBinary(fresh.getData(), static_cast<int>(fresh.getSize()));
        if (juce::XmlElement* c = xml->getChildByName("CHAT"))
            xml->removeChildElement(c, true);
        xml->createNewChildElement("CHAT")->setAttribute("json", juce::String::fromUTF8(json.c_str()));
        juce::MemoryBlock out;
        juce::AudioProcessor::copyXmlToBinary(*xml, out);
        return out;
    }

    juce::MemoryBlock stateOf(pg::Processor& p)
    {
        juce::MemoryBlock m;
        p.getStateInformation(m);
        return m;
    }

    void load(pg::Processor& p, const juce::MemoryBlock& m)
    {
        p.setStateInformation(m.getData(), static_cast<int>(m.getSize()));
        p.settle();
    }

    // Whether any chat of `owner`'s list (its titles) or any chat of its store holds `phrase`.
    bool listHolds(const pg::ChatList& l, const std::string& phrase)
    {
        for (const pg::ChatListEntry& e : l.chats)
            if (has(e.title, phrase))
                return true;
        return false;
    }

    bool storeHolds(const std::string& owner, const std::string& phrase)
    {
        for (const cs::ChatMeta& m : cs::chatsOf(owner))
            if (const auto l = cs::loadChat(owner, m.id); l && has(l->state, phrase))
                return true;
        return false;
    }

    // The last entry of a kind's text.
    std::string lastOf(const pg::ChatState& s, pg::ChatEntry::Kind kind)
    {
        for (auto it = s.entries.rbegin(); it != s.entries.rend(); ++it)
            if (it->kind == kind)
                return it->text;
        return {};
    }
}

PROBE(plugin, chats)
{
    const funkgui::HeadlessGuiScope gui;
    const juce::File root = juce::File(juce::String(probe::outPath("chats")));
    root.deleteRecursively();
    const juce::File keys = root.getChildFile("Playground"), projects = root.getChildFile("Projects");
    cs::setDirectoryForTests(keys);
    ps::setDirectoryForTests(projects);
    const juce::File chats = keys.getChildFile("chats");
    std::string why;
    P.eq("setup.key", cs::saveKey(fake::fakeKey(), &why) ? 1 : 0, 1);

    // ---- STORE ------------------------------------------------------------------------------------------------------
    {
        const std::string owner = cs::instanceOwner(cs::newId());
        cs::ChatMeta meta{ cs::newId(), "Quiet tape echo", false, 1000, 2000 };
        cs::Stamp stamp;
        const std::string state = R"({"version":1,"messages":[],"entries":[{"k":"user","t":"STORE-PHRASE-SECRET"}]})";
        P.eq("store.written", cs::saveChat(owner, meta, state, stamp) == cs::SaveResult::ok ? 1 : 0, 1);
        const juce::File dir = chats.getChildFile(juce::String(owner));
        const juce::File chatFile = dir.getChildFile(juce::String(meta.id) + ".chat"), metaFile = dir.getChildFile(juce::String(meta.id) + ".meta");
        P.eq("store.two_files", chatFile.existsAsFile() && metaFile.existsAsFile() ? 1 : 0, 1);
        P.eq("store.sealed", bytesOf(chatFile).rfind("FPC1", 0) == 0 && bytesOf(metaFile).rfind("FPC1", 0) == 0 ? 1 : 0, 1);
        P.eq("store.nothing_in_clear", filesHolding(keys, "STORE-PHRASE") + filesHolding(keys, "Quiet tape"), 0);
#if !defined(_WIN32)
        P.eq("store.chat_0600", mode(chatFile), 0600);
        P.eq("store.meta_0600", mode(metaFile), 0600);
        P.eq("store.owner_0700", mode(dir), 0700);
        P.eq("store.chats_0700", mode(chats), 0700);
        P.eq("store.salt_0600", mode(chats.getChildFile(".salt")), 0600);
#endif
        const auto loaded = cs::loadChat(owner, meta.id, &why);
        P.eq("store.loads", loaded && loaded->state == state && loaded->meta.title == "Quiet tape echo" && !loaded->meta.named
                                && loaded->meta.createdMs == 1000 && loaded->meta.updatedMs == 2000 ? 1 : 0, 1);
        const std::vector<cs::ChatMeta> list = cs::chatsOf(owner);
        P.eq("store.listed", list.size() == 1 && list[0].id == meta.id ? 1 : 0, 1);
        // Files not sealed here (a copied Library: each byte as another machine's keys sealed it fails this one's MAC,
        // as a byte changed does) are left out of the list; a chat sealed here whose .meta is gone is listed, "Chat".
        {
            const std::string good = bytesOf(chatFile), goodMeta = bytesOf(metaFile);
            const auto flipped = [](std::string b) {
                b[40] = static_cast<char>(b[40] ^ 0x10);
                return b;
            };
            const std::string elsewhere = cs::newId(), bare = cs::newId(), noMeta = cs::newId();
            const auto put = [&](const std::string& id, const char* ext, const std::string& bytes) {
                dir.getChildFile(juce::String(id) + ext).replaceWithData(bytes.data(), bytes.size());
            };
            put(elsewhere, ".chat", flipped(good));
            put(elsewhere, ".meta", flipped(goodMeta));
            put(bare, ".chat", flipped(good));
            put(noMeta, ".chat", good);
            const auto listed = [&](const std::string& id) {
                for (const cs::ChatMeta& m : cs::chatsOf(owner))
                    if (m.id == id)
                        return 1;
                return 0;
            };
            P.eq("store.elsewhere_left_out", listed(elsewhere), 0);
            P.eq("store.elsewhere_bare_left_out", listed(bare), 0);
            bool chatTitled = false;
            for (const cs::ChatMeta& m : cs::chatsOf(owner))
                chatTitled = chatTitled || (m.id == noMeta && m.title == "Chat");
            P.eq("store.no_meta_listed", chatTitled ? 1 : 0, 1);
            P.eq("store.listed_again_same", listed(elsewhere) + listed(bare), 0);   // the answers kept, the same
            for (const std::string& id : { elsewhere, bare, noMeta })
                cs::removeChat(owner, id);
            P.eq("store.only_its_own_left", cs::chatsOf(owner).size() == 1 ? 1 : 0, 1);
        }
        // A byte changed: refused, saying why.
        {
            const std::string good = bytesOf(chatFile);
            std::string bad = good;
            bad[40] = static_cast<char>(bad[40] ^ 0x10);
            chatFile.replaceWithData(bad.data(), bad.size());
            why.clear();
            P.eq("store.tamper_refused", cs::loadChat(owner, meta.id, &why).has_value() ? 1 : 0, 0);
            P.eq("store.tamper_why", why.empty() ? 0 : 1, 1);
            chatFile.replaceWithData(good.data(), good.size());
            P.eq("store.restored_loads", cs::loadChat(owner, meta.id).has_value() ? 1 : 0, 1);
        }
        // Another writer: a stamp that is not the file's now is a conflict, and nothing is written.
        {
            const auto now = cs::loadChat(owner, meta.id);   // as the file is now (restored above)
            cs::Stamp mine = now ? now->stamp : cs::Stamp{}, theirs = mine;
            std::string whyNot;
            const cs::Stamp was = theirs;
            const cs::SaveResult written = cs::saveChat(owner, meta, state + " ", theirs, &whyNot);
            if (written != cs::SaveResult::ok)
                std::printf("INFO     store: theirs %d (%s), stamp %lld %lld, file %lld %lld\n", static_cast<int>(written), whyNot.c_str(),
                            static_cast<long long>(was.modified), static_cast<long long>(was.size),
                            static_cast<long long>(chatFile.getLastModificationTime().toMilliseconds()), static_cast<long long>(chatFile.getSize()));
            P.eq("store.theirs_written", written == cs::SaveResult::ok ? 1 : 0, 1);
            // Their save given back the time mine read: of the same sealed size, it then differs from mine by its
            // identity alone (each save renames a new file in), as two saves within one tick of a file clock do (JUCE
            // reads whole seconds on Linux), so the conflict below rests on that alone.
            chatFile.setLastModificationTime(juce::Time(mine.modified));
            P.eq("store.theirs_alike", chatFile.getLastModificationTime().toMilliseconds() == mine.modified && chatFile.getSize() == mine.size
                                           && chatFile.getFileIdentifier() != mine.id && chatFile.getFileIdentifier() != 0 ? 1 : 0, 1);
            const std::string after = bytesOf(chatFile);
            P.eq("store.conflict", cs::saveChat(owner, meta, state, mine) == cs::SaveResult::conflict ? 1 : 0, 1);
            P.eq("store.conflict_writes_nothing", bytesOf(chatFile) == after ? 1 : 0, 1);
            cs::Stamp none;
            P.eq("store.new_over_existing", cs::saveChat(owner, meta, state, none) == cs::SaveResult::conflict ? 1 : 0, 1);
        }
        // Names that are no folder's.
        P.eq("store.owner_dotdot", cs::validOwner("../x") || cs::validOwner("..") || cs::validOwner("a/b") ? 1 : 0, 0);
        P.eq("store.owner_upper", cs::validOwner("Instance-ABC") ? 1 : 0, 0);
        P.eq("store.owner_ok", cs::validOwner(owner) && cs::validOwner("8c1f4e2a-0b3c-4d5e-8f90-123456789abc") ? 1 : 0, 1);
        P.eq("store.id_shape", cs::validId(meta.id) && !cs::validId("../../etc") && !cs::validId(std::string(32, 'G')) ? 1 : 0, 1);
        cs::Stamp s2;
        P.eq("store.bad_owner_refused", cs::saveChat("../escape", { cs::newId(), "x", false, 1, 1 }, state, s2) == cs::SaveResult::failed
                                            && !root.getChildFile("escape").exists() ? 1 : 0, 1);
        P.eq("store.bad_load_refused", cs::loadChat("../Playground", meta.id).has_value() ? 1 : 0, 0);
        P.eq("store.removed", cs::removeChat(owner, meta.id) && !chatFile.exists() && !metaFile.exists() ? 1 : 0, 1);
    }

    // ---- ID ---------------------------------------------------------------------------------------------------------
    {
        P.eq("id.saved", ps::saveVersion("Alpha", program("alpha"), 0, {}).has_value() ? 1 : 0, 1);
        const std::string id = idInFile("Alpha");
        P.eq("id.made", ps::validProjectId(id) ? 1 : 0, 1);
        P.eq("id.kept_by_a_save", ps::saveVersion("Alpha", program("alpha 2"), 0, {}).has_value() && idInFile("Alpha") == id ? 1 : 0, 1);
        P.eq("id.id_of", ps::idOf("Alpha") == id ? 1 : 0, 1);
        // A copy made in the Finder: its own id when opened, the original's kept.
        const juce::File alpha = projects.getChildFile("Alpha.fmproject");
        P.eq("id.copied", alpha.copyFileTo(projects.getChildFile("Alpha copy.fmproject")) ? 1 : 0, 1);
        const std::string copied = ps::idOf("Alpha copy");
        P.eq("id.copy_gets_its_own", ps::validProjectId(copied) && copied != id && idInFile("Alpha copy") == copied ? 1 : 0, 1);
        P.eq("id.original_keeps", ps::idOf("Alpha") == id ? 1 : 0, 1);
        // Renamed in the Finder: the same id, and found by it.
        P.eq("id.renamed", alpha.moveFileTo(projects.getChildFile("Alpha Renamed.fmproject")) ? 1 : 0, 1);
        const auto renamed = ps::load("Alpha Renamed");
        P.eq("id.rename_keeps", renamed && renamed->id == id && ps::idOf("Alpha Renamed") == id ? 1 : 0, 1);
        P.eq("id.found_by_id", ps::nameOfId(id) == "Alpha Renamed" ? 1 : 0, 1);
        // A project from before ids: one made when it is asked for, written into its file.
        juce::var old = juce::JSON::parse(projects.getChildFile("Alpha copy.fmproject"));
        old.getDynamicObject()->removeProperty("id");
        old.getDynamicObject()->setProperty("name", "Older");
        projects.getChildFile("Older.fmproject").replaceWithText(juce::JSON::toString(old));
        P.eq("id.older_has_none", idInFile("Older").empty() ? 1 : 0, 1);
        const std::string older = ps::idOf("Older");
        P.eq("id.older_gets_one", ps::validProjectId(older) && idInFile("Older") == older ? 1 : 0, 1);
        // A project read before a save rewrote its file (the editor's save runs on a thread of its own): the id made
        // from that read goes into the file as it is now, the version saved kept. One from before ids: the id the save
        // made is its id.
        old.getDynamicObject()->setProperty("name", "Oldest");
        projects.getChildFile("Oldest.fmproject").replaceWithText(juce::JSON::toString(old));
        const std::optional<ps::Project> stale = ps::load("Oldest");
        const std::size_t staleVersions = stale ? stale->versions.size() : 0;
        P.eq("id.stale_setup", stale && stale->id.empty() && ps::saveVersion("Oldest", program("oldest 2"), 0, {}).has_value() ? 1 : 0, 1);
        const std::string saveMade = idInFile("Oldest");
        const std::string fromStale = stale ? ps::idOf(*stale) : std::string();
        const std::optional<ps::Project> oldest = ps::load("Oldest");
        P.eq("id.stale_read_keeps_the_save", ps::validProjectId(saveMade) && fromStale == saveMade && oldest
                                                 && oldest->versions.size() == staleVersions + 1 ? 1 : 0, 1);
        // A copy made in the Finder, read, then a version saved into it: an id of its own, the version kept.
        P.eq("id.gamma_saved", ps::saveVersion("Gamma", program("gamma"), 0, {}).has_value()
                                   && projects.getChildFile("Gamma.fmproject").copyFileTo(projects.getChildFile("Gamma copy.fmproject")) ? 1 : 0, 1);
        const std::string gamma = idInFile("Gamma");
        const std::optional<ps::Project> staleCopy = ps::load("Gamma copy");
        const bool copySaved = ps::saveVersion("Gamma copy", program("gamma 2"), 0, {}).has_value();
        const std::string ownId = staleCopy ? ps::idOf(*staleCopy) : std::string();
        const std::optional<ps::Project> gammaCopy = ps::load("Gamma copy");
        P.eq("id.stale_copy_keeps_the_save", copySaved && staleCopy && staleCopy->id == gamma && ps::validProjectId(ownId) && ownId != gamma
                                                 && idInFile("Gamma copy") == ownId && idInFile("Gamma") == gamma && gammaCopy
                                                 && gammaCopy->versions.size() == staleCopy->versions.size() + 1 ? 1 : 0, 1);
    }

    // The processors go before the folders are given back (each keeps its chat as it goes).
    {
        // ---- SWITCH -------------------------------------------------------------------------------------------------
        pg::Processor a;
        pg::EditorPort& port = a.editorPort();
        pg::ChatAgent& agent = a.chatAgent();
        const auto script = wire(a);
        std::string idA, idB;
        pg::SavedVersion saved;
        {
            port.setDraft({ program("song a"), 0 });
            P.eq("switch.a_saved", port.saveVersion("Song A", saved, why) ? 1 : 0, 1);
            idA = idInFile("Song A");
            P.eq("switch.a_owner", agent.chatOwner() == idA && agent.chatState().owner == "Song A" ? 1 : 0, 1);
            turn(a, *script, "PHRASE-A warm pad please", "Plan A: a pad.", "sig-a", "Answer A: a pad.");
            P.eq("switch.a_title", agent.chatState().title == "PHRASE-A warm pad please" ? 1 : 0, 1);
            P.eq("switch.a_kept", cs::chatsOf(idA).size() == 1 ? 1 : 0, 1);
            P.eq("switch.a_sealed", filesHolding(keys, "PHRASE-A"), 0);
            const std::string historyA = agent.saveState();
            const std::string wordsA = said(agent.chatState());

            P.eq("switch.b_made", ps::saveVersion("Song B", program("song b"), 0, {}).has_value() ? 1 : 0, 1);
            P.eq("switch.b_opened", port.openProject("Song B", 0, why) ? 1 : 0, 1);
            a.settle();
            idB = idInFile("Song B");
            const pg::ChatState b = agent.chatState();
            P.eq("switch.b_new_chat", b.entries.empty() && b.owner == "Song B" && agent.chatOwner() == idB ? 1 : 0, 1);
            P.eq("switch.a_still_kept", cs::chatsOf(idA).size() == 1 ? 1 : 0, 1);
            turn(a, *script, "PHRASE-B a bass", "Plan B.", "sig-b", "Answer B.");
            const std::string wordsB = said(agent.chatState());

            P.eq("switch.a_opened", port.openProject("Song A", 0, why) ? 1 : 0, 1);
            a.settle();
            P.eq("switch.a_back", said(agent.chatState()) == wordsA && agent.chatState().owner == "Song A" ? 1 : 0, 1);
            P.eq("switch.a_history", agent.saveState() == historyA ? 1 : 0, 1);
            const int before = script->postCount();
            turn(a, *script, "and brighter", "Plan A2.", "sig-a2", "Brighter.");
            const std::string body = script->body(before);
            P.eq("switch.thinking_preserved", has(body, "sig-a") && has(body, "Plan A: a pad.") && has(body, "PHRASE-A") ? 1 : 0, 1);
            P.eq("switch.b_opened_again", port.openProject("Song B", 0, why) ? 1 : 0, 1);
            a.settle();
            P.eq("switch.b_back", said(agent.chatState()) == wordsB ? 1 : 0, 1);
            P.eq("switch.two_projects", cs::chatsOf(idA).size() == 1 && cs::chatsOf(idB).size() == 1 ? 1 : 0, 1);
        }

        // ---- CARRY --------------------------------------------------------------------------------------------------
        {
            port.startAfresh(pg::singleFile(pg::kExamples[0].source), "the example \"Init\"");
            a.settle();
            const std::string instance = agent.chatOwner();
            P.eq("carry.instance_owner", instance.rfind(cs::kInstancePrefix, 0) == 0 && agent.chatState().owner.empty()
                                             && agent.chatState().entries.empty() ? 1 : 0, 1);
            turn(a, *script, "PHRASE-C before a project", "Plan C.", "sig-c", "Answer C.");
            P.eq("carry.kept_with_instance", cs::chatsOf(instance).size() == 1 ? 1 : 0, 1);
            const std::string wordsC = said(agent.chatState());
            port.setDraft({ program("song c"), 0 });
            P.eq("carry.c_saved", port.saveVersion("Song C", saved, why) ? 1 : 0, 1);
            const std::string idC = idInFile("Song C");
            P.eq("carry.goes_with_c", agent.chatOwner() == idC && said(agent.chatState()) == wordsC && cs::chatsOf(idC).size() == 1 ? 1 : 0, 1);
            P.eq("carry.none_left", cs::chatsOf(instance).empty() ? 1 : 0, 1);
            P.eq("carry.d_saved", port.saveVersion("Song D", saved, why) ? 1 : 0, 1);
            const std::string idD = idInFile("Song D");
            const auto c = cs::chatsOf(idC), d = cs::chatsOf(idD);
            P.eq("carry.d_copy", agent.chatOwner() == idD && c.size() == 1 && d.size() == 1 && c[0].id != d[0].id
                                     && said(agent.chatState()) == wordsC ? 1 : 0, 1);
        }

        // ---- LIST ---------------------------------------------------------------------------------------------------
        {
            P.eq("list.a_opened", port.openProject("Song A", 0, why) ? 1 : 0, 1);
            a.settle();
            const std::string firstId = agent.chatList().chats.at(0).id;
            agent.newChat();
            pg::ChatList l = agent.chatList();
            P.eq("list.new_first", l.chats.size() == 2 && l.chats[0].current && l.chats[0].title == "New chat" && l.chats[0].when.empty()
                                       && l.chats[1].id == firstId && l.owner == "Song A" ? 1 : 0, 1);
            P.eq("list.new_not_written", cs::chatsOf(idA).size() == 1 ? 1 : 0, 1);
            juce::Thread::sleep(5);
            turn(a, *script, "PHRASE-L second chat", "Plan L.", "sig-l", "Answer L.");
            l = agent.chatList();
            P.eq("list.newest_first", l.chats.size() == 2 && l.chats[0].title == "PHRASE-L second chat" && l.chats[0].current
                                          && l.chats[1].id == firstId && !l.chats[1].current && !l.chats[1].when.empty() ? 1 : 0, 1);
            const std::string secondId = l.chats[0].id;
            agent.renameChat("Bass ideas");
            l = agent.chatList();
            const auto metas = cs::chatsOf(idA);
            bool named = false;
            for (const cs::ChatMeta& m : metas)
                named = named || (m.id == secondId && m.named && m.title == "Bass ideas");
            P.eq("list.renamed", agent.chatState().title == "Bass ideas" && l.chats[0].title == "Bass ideas" && named ? 1 : 0, 1);
            P.eq("list.opened", agent.openChat(firstId) && has(said(agent.chatState()), "PHRASE-A") ? 1 : 0, 1);
            l = agent.chatList();
            P.eq("list.marks_shown", l.chats.size() == 2 && ((l.chats[0].id == firstId && l.chats[0].current && !l.chats[1].current)
                                                             || (l.chats[1].id == firstId && l.chats[1].current && !l.chats[0].current)) ? 1 : 0, 1);
            P.eq("list.open_again", agent.openChat(secondId) && agent.chatState().title == "Bass ideas" ? 1 : 0, 1);
            agent.renameChat("");
            P.eq("list.name_cleared", agent.chatState().title == "PHRASE-L second chat" ? 1 : 0, 1);
            agent.deleteChat();
            P.eq("list.deleted", cs::chatsOf(idA).size() == 1 && !cs::hasChat(idA, secondId) && agent.chatList().chats.size() == 1
                                     && has(said(agent.chatState()), "PHRASE-A") ? 1 : 0, 1);
            P.eq("list.bad_id", agent.openChat("not-an-id") ? 1 : 0, 0);
            // A chat its .meta lists that cannot be read (damaged since): opening it changes nothing and says why.
            {
                const juce::File dir = chats.getChildFile(juce::String(idA));
                const std::string shown = agent.chatRef().id, dead = cs::newId();
                std::string bad = bytesOf(dir.getChildFile(juce::String(shown) + ".chat"));
                const std::string meta = bytesOf(dir.getChildFile(juce::String(shown) + ".meta"));
                bad[40] = static_cast<char>(bad[40] ^ 0x10);
                dir.getChildFile(juce::String(dead) + ".chat").replaceWithData(bad.data(), bad.size());
                dir.getChildFile(juce::String(dead) + ".meta").replaceWithData(meta.data(), meta.size());
                bool listed = false;
                for (const pg::ChatListEntry& e : agent.chatList().chats)
                    listed = listed || e.id == dead;
                const std::string before = said(agent.chatState());
                const bool opened = agent.openChat(dead);
                const pg::ChatState after = agent.chatState();
                P.eq("list.unreadable_listed", listed ? 1 : 0, 1);
                P.eq("list.unreadable_refused", opened ? 1 : 0, 0);
                P.eq("list.unreadable_says", agent.chatRef().id == shown && has(said(after), before)
                                                 && has(lastOf(after, pg::ChatEntry::Kind::note), "That chat could not be opened") ? 1 : 0, 1);
                cs::removeChat(idA, dead);
            }
        }

        // ---- SESSION ------------------------------------------------------------------------------------------------
        juce::MemoryBlock session;
        {
            a.getStateInformation(session);
            const std::string text = bytesOf(session);
            P.eq("session.no_words", has(text, "PHRASE") || has(text, "Answer A") || has(text, "Plan A") ? 1 : 0, 0);
            P.eq("session.no_json", has(text, "json=") ? 1 : 0, 0);
            P.eq("session.references", has(text, "<CHAT") && has(text, "owner=\"" + idA + "\"") && has(text, "instance=")
                                           && has(text, "chat=\"" + agent.chatRef().id + "\"") && has(text, "projectId=\"" + idA + "\"") ? 1 : 0, 1);
            pg::Processor b;
            b.setStateInformation(session.getData(), static_cast<int>(session.getSize()));
            b.settle();
            P.eq("session.same_chat", said(b.chatAgent().chatState()) == said(agent.chatState())
                                          && b.chatAgent().chatRef().id == agent.chatRef().id ? 1 : 0, 1);
            // On another Mac: the chat is not there, nor anything of it.
            const juce::File other = root.getChildFile("Another Mac");
            cs::setDirectoryForTests(other);
            pg::Processor elsewhere;
            elsewhere.setStateInformation(session.getData(), static_cast<int>(session.getSize()));
            elsewhere.settle();
            P.eq("session.elsewhere_empty", elsewhere.chatAgent().chatState().entries.empty() ? 1 : 0, 1);
            cs::setDirectoryForTests(keys);
        }

        // ---- MIGRATE ------------------------------------------------------------------------------------------------
        {
            // A session from before: its <CHAT json="..."> holds the conversation (a chat's saveState, as that build wrote it).
            std::string json;
            {
                pg::Processor maker;
                const auto makerScript = wire(maker);
                turn(maker, *makerScript, "PHRASE-M an older session", "Plan M.", "sig-m", "Answer M.");
                json = maker.chatAgent().saveState();
                maker.chatAgent().deleteChat();             // nothing of it in the store but what the migration keeps
            }
            const juce::MemoryBlock old = oldSession(json);
            P.eq("migrate.old_holds_it", has(bytesOf(old), "PHRASE-M") ? 1 : 0, 1);

            std::string owner, chatId;
            {
                pg::Processor c;
                load(c, old);
                owner = c.chatAgent().chatOwner();
                chatId = c.chatAgent().chatRef().id;
                P.eq("migrate.shown", has(said(c.chatAgent().chatState()), "PHRASE-M") ? 1 : 0, 1);
                const auto kept = cs::chatsOf(owner);
                const std::optional<cs::Loaded> loaded = kept.empty() ? std::optional<cs::Loaded>{} : cs::loadChat(owner, kept[0].id);
                P.eq("migrate.kept_here", kept.size() == 1 && loaded && has(loaded->state, "PHRASE-M") ? 1 : 0, 1);
                P.eq("migrate.sealed", filesHolding(keys, "PHRASE-M"), 0);
                const juce::MemoryBlock next = stateOf(c);
                P.eq("migrate.next_session_none", has(bytesOf(next), "PHRASE-M") || has(bytesOf(next), "json=") ? 1 : 0, 0);
            }
            {
                // The same session opened again (unsaved since): the same instance, the same chat, kept once.
                pg::Processor d;
                load(d, old);
                P.eq("migrate.again_kept_once", d.chatAgent().chatOwner() == owner && cs::chatsOf(owner).size() == 1
                                                    && d.chatAgent().chatRef().id == chatId
                                                    && has(said(d.chatAgent().chatState()), "PHRASE-M") ? 1 : 0, 1);
            }

            // Old sessions whose chat was empty (every untouched instance's alike), or held only a note of the chat's own
            // (the key saved): no chat to adopt, and an instance of its own each, so a chat started in one is not another's.
            std::string empty, noted;
            {
                pg::Processor fresh;
                empty = fresh.chatAgent().saveState();
                juce::var v = juce::JSON::parse(juce::String::fromUTF8(empty.c_str()));
                juce::Array<juce::var> entries;
                entries.add(pg::claude::object({ { "k", "note" }, { "t", "Your API key is saved." } }));
                v.getDynamicObject()->setProperty("entries", entries);
                noted = juce::JSON::toString(v, true).toStdString();
            }
            P.eq("migrate.not_adoptable", !pg::ChatAgent::adoptable(empty) && !pg::ChatAgent::adoptable(noted)
                                              && pg::ChatAgent::adoptable(json) ? 1 : 0, 1);
            const juce::MemoryBlock song1 = oldSession(empty), song2 = oldSession(empty), song3 = oldSession(noted);
            std::string owner1;
            juce::MemoryBlock saved1;
            {
                pg::Processor e1;
                const auto s1 = wire(e1);
                load(e1, song1);
                owner1 = e1.chatAgent().chatOwner();
                P.eq("migrate.empty_nothing_kept", cs::chatsOf(owner1).empty() && e1.chatAgent().chatState().entries.empty() ? 1 : 0, 1);
                turn(e1, *s1, "PHRASE-E1 in the first song", "Plan E1.", "sig-e1", "Answer E1.");
                saved1 = stateOf(e1);
            }
            {
                pg::Processor e2, e3;
                load(e2, song2);
                load(e3, song3);
                const std::string owner2 = e2.chatAgent().chatOwner(), owner3 = e3.chatAgent().chatOwner();
                P.eq("migrate.empty_own_instances", owner1 != owner2 && owner1 != owner3 && owner2 != owner3
                                                        && owner2.rfind(cs::kInstancePrefix, 0) == 0 ? 1 : 0, 1);
                P.eq("migrate.empty_not_shared", !has(said(e2.chatAgent().chatState()), "PHRASE-E1")
                                                     && !listHolds(e2.chatAgent().chatList(), "PHRASE-E1")
                                                     && !has(said(e3.chatAgent().chatState()), "PHRASE-E1")
                                                     && !listHolds(e3.chatAgent().chatList(), "PHRASE-E1") ? 1 : 0, 1);
                P.eq("migrate.note_nothing_kept", cs::chatsOf(owner3).empty() ? 1 : 0, 1);
                pg::Processor e1again;
                load(e1again, saved1);
                P.eq("migrate.empty_first_keeps_its", e1again.chatAgent().chatOwner() == owner1
                                                          && has(said(e1again.chatAgent().chatState()), "PHRASE-E1") ? 1 : 0, 1);
            }
        }

        // ---- RENAME (a project renamed in the Finder, after the session above named it) -----------------------------
        {
            P.eq("rename.moved", projects.getChildFile("Song A.fmproject").moveFileTo(projects.getChildFile("Song A Renamed.fmproject")) ? 1 : 0, 1);
            pg::Processor r;
            r.setStateInformation(session.getData(), static_cast<int>(session.getSize()));
            r.settle();
            P.eq("rename.session_finds_it", r.editorPort().project().name == "Song A Renamed" && r.chatAgent().chatOwner() == idA
                                                && has(said(r.chatAgent().chatState()), "PHRASE-A") ? 1 : 0, 1);
            P.eq("rename.label", r.chatAgent().chatState().owner == "Song A Renamed" ? 1 : 0, 1);
            P.eq("rename.opened", port.openProject("Song B", 0, why) && port.openProject("Song A Renamed", 0, why) ? 1 : 0, 1);
            a.settle();
            P.eq("rename.its_chats", agent.chatOwner() == idA && has(said(agent.chatState()), "PHRASE-A") ? 1 : 0, 1);
        }

        // ---- EXPORT -------------------------------------------------------------------------------------------------
        {
            const juce::File pack = root.getChildFile("Song A.fmpack"), zip = root.getChildFile("Song A.zip");
            pg::Exported out;
            P.eq("export.package", port.exportProject("Song A Renamed", true, true, pack.getFullPathName().toStdString(), out, why) ? 1 : 0, 1);
            P.eq("export.source", port.exportProject("Song A Renamed", false, true, zip.getFullPathName().toStdString(), out, why) ? 1 : 0, 1);
            const auto read = pg::projectexport::read(pack, &why);
            P.eq("export.package_no_id", read && read->id.empty() ? 1 : 0, 1);
            const std::string json = read ? ps::toJson(*read) : std::string();
            P.eq("export.package_no_chat", !json.empty() && !has(json, "PHRASE") && !has(json, idA) ? 1 : 0, 1);
            std::string all, manifest;
            juce::ZipFile z(zip);
            for (int i = 0; i < z.getNumEntries(); ++i)
                if (std::unique_ptr<juce::InputStream> in{ z.createStreamForEntry(i) })
                {
                    const std::string text = in->readEntireStreamAsString().toStdString();
                    all += text;
                    if (z.getEntry(i)->filename.endsWith("/project.json"))
                        manifest = text;
                }
            P.eq("export.zip_no_chat", !all.empty() && !has(all, "PHRASE") && !has(all, idA) ? 1 : 0, 1);
            P.eq("export.zip_no_id", !manifest.empty() && !has(manifest, "\"id\"") ? 1 : 0, 1);
            P.eq("export.zip_says_so", has(all, "chats with Claude are not in this zip") ? 1 : 0, 1);
            std::string name;
            P.eq("export.imported", port.importProject(pack.getFullPathName().toStdString(), name, why) ? 1 : 0, 1);
            const std::string imported = ps::idOf(name);
            P.eq("export.import_new_id", ps::validProjectId(imported) && imported != idA && cs::chatsOf(imported).empty() ? 1 : 0, 1);
            P.eq("export.import_opens_empty", port.openProject(name, 0, why) && agent.chatState().entries.empty() ? 1 : 0, 1);
            a.settle();
        }

        // ---- DELETE -------------------------------------------------------------------------------------------------
        {
            P.eq("delete.has_chats", cs::chatsOf(idB).size() == 1 ? 1 : 0, 1);
            P.eq("delete.deleted", port.deleteProject("Song B", why) ? 1 : 0, 1);
            P.eq("delete.chats_gone", cs::chatsOf(idB).empty() && !chats.getChildFile(juce::String(idB)).exists() ? 1 : 0, 1);
        }

        // ---- MCP ----------------------------------------------------------------------------------------------------
        {
            P.eq("mcp.opened", port.openProject("Song A Renamed", 0, why) ? 1 : 0, 1);
            a.settle();
            const pg::ChatAgent::McpResult r = agent.mcpCall("get_status", juce::var(new juce::DynamicObject()));
            const std::string text = juce::JSON::toString(r.content).toStdString();
            P.eq("mcp.status_said", text.size() > 20 ? 1 : 0, 1);
            P.eq("mcp.no_chat", has(text, "PHRASE") || has(text, "Answer A") ? 1 : 0, 0);
        }

        // ---- COPIES (one state in several instances: a template, a default preset, a track duplicated) -------------
        {
            // A template's state, from an instance that never kept a chat: each instance made from it gets its own.
            juce::MemoryBlock tmpl;
            {
                pg::Processor t;
                tmpl = stateOf(t);
            }
            juce::MemoryBlock s1, s2;
            std::string o1, o2;
            {
                pg::Processor p1, p2;
                const auto w1 = wire(p1), w2 = wire(p2);
                load(p1, tmpl);
                load(p2, tmpl);
                o1 = p1.chatAgent().chatOwner();
                o2 = p2.chatAgent().chatOwner();
                P.eq("copies.template_own_instances", o1 != o2 && o1.rfind(cs::kInstancePrefix, 0) == 0 ? 1 : 0, 1);
                turn(p1, *w1, "PHRASE-T1 a song from the template", "Plan T1.", "sig-t1", "Answer T1.");
                turn(p2, *w2, "PHRASE-T2 another song from it", "Plan T2.", "sig-t2", "Answer T2.");
                s1 = stateOf(p1);
                s2 = stateOf(p2);
            }
            {
                pg::Processor q1, q2, q3;
                load(q1, s1);
                load(q2, s2);
                load(q3, tmpl);                                // a later song made from it
                const std::string w1 = said(q1.chatAgent().chatState()), w2 = said(q2.chatAgent().chatState());
                P.eq("copies.template_apart", has(w1, "PHRASE-T1") && !has(w1, "PHRASE-T2") && !listHolds(q1.chatAgent().chatList(), "PHRASE-T2")
                                                  && has(w2, "PHRASE-T2") && !has(w2, "PHRASE-T1")
                                                  && !listHolds(q2.chatAgent().chatList(), "PHRASE-T1") ? 1 : 0, 1);
                P.eq("copies.template_later_song", q3.chatAgent().chatState().entries.empty()
                                                       && !listHolds(q3.chatAgent().chatList(), "PHRASE-T")
                                                       && q3.chatAgent().chatOwner() != o1 && q3.chatAgent().chatOwner() != o2 ? 1 : 0, 1);
            }
            // A state naming its instance's chat, in two live instances (a track duplicated): the first is that instance,
            // the second gets a copy of the chat as its own; each goes on apart.
            juce::MemoryBlock withChat;
            std::string x0;
            {
                pg::Processor x;
                const auto w = wire(x);
                turn(x, *w, "PHRASE-X0 the track's chat", "Plan X0.", "sig-x0", "Answer X0.");
                withChat = stateOf(x);
                x0 = x.chatAgent().chatOwner();
            }
            juce::MemoryBlock r1, r2;
            {
                pg::Processor p1, p2;
                const auto w1 = wire(p1), w2 = wire(p2);
                load(p1, withChat);
                load(p2, withChat);
                P.eq("copies.duplicate_first_is_it", p1.chatAgent().chatOwner() == x0 && has(said(p1.chatAgent().chatState()), "PHRASE-X0") ? 1 : 0, 1);
                P.eq("copies.duplicate_own_copy", p2.chatAgent().chatOwner() != x0 && has(said(p2.chatAgent().chatState()), "PHRASE-X0")
                                                      && cs::chatsOf(p2.chatAgent().chatOwner()).size() == 1 ? 1 : 0, 1);
                turn(p1, *w1, "PHRASE-X1 on the first track", "Plan X1.", "sig-x1", "Answer X1.");
                turn(p2, *w2, "PHRASE-X2 on the copy", "Plan X2.", "sig-x2", "Answer X2.");
                P.eq("copies.duplicate_thinking", has(w2->body(w2->postCount() - 1), "sig-x0") ? 1 : 0, 1);
                r1 = stateOf(p1);
                r2 = stateOf(p2);
            }
            {
                pg::Processor q1, q2;
                load(q1, r1);
                load(q2, r2);
                const std::string w1 = said(q1.chatAgent().chatState()), w2 = said(q2.chatAgent().chatState());
                P.eq("copies.duplicate_apart", has(w1, "PHRASE-X0") && has(w1, "PHRASE-X1") && !has(w1, "PHRASE-X2")
                                                   && !listHolds(q1.chatAgent().chatList(), "PHRASE-X2") && has(w2, "PHRASE-X0")
                                                   && has(w2, "PHRASE-X2") && !has(w2, "PHRASE-X1")
                                                   && !listHolds(q2.chatAgent().chatList(), "PHRASE-X1") ? 1 : 0, 1);
                P.eq("copies.duplicate_kept_once_each", cs::chatsOf(x0).size() == 1 && cs::chatsOf(q2.chatAgent().chatOwner()).size() == 1 ? 1 : 0, 1);
            }
            // A new chat shown when the session was saved: the instance's own, its earlier chat in the list, never shown in
            // the new one's place.
            {
                juce::MemoryBlock afterNew;
                std::string o;
                {
                    pg::Processor n;
                    const auto w = wire(n);
                    turn(n, *w, "PHRASE-N1 before a new chat", "Plan N1.", "sig-n1", "Answer N1.");
                    n.chatAgent().newChat();
                    afterNew = stateOf(n);
                    o = n.chatAgent().chatOwner();
                }
                pg::Processor m;
                load(m, afterNew);
                P.eq("copies.new_chat_shown", m.chatAgent().chatOwner() == o && m.chatAgent().chatState().entries.empty()
                                                  && listHolds(m.chatAgent().chatList(), "PHRASE-N1") ? 1 : 0, 1);
            }
        }

        // ---- SHARED (one project's chat in two instances, both writing) ---------------------------------------------
        {
            P.eq("shared.made", ps::saveVersion("Shared Song", program("shared"), 0, {}).has_value() ? 1 : 0, 1);
            const std::string idS = idInFile("Shared Song");
            pg::Processor h1, h2;
            const auto w1 = wire(h1), w2 = wire(h2);
            P.eq("shared.opened", h1.editorPort().openProject("Shared Song", 0, why) ? 1 : 0, 1);
            h1.settle();
            turn(h1, *w1, "PHRASE-H0 the project's chat", "Plan H0.", "sig-h0", "Answer H0.");
            load(h2, stateOf(h1));
            P.eq("shared.same_chat", h2.chatAgent().chatOwner() == idS && h2.chatAgent().chatRef().id == h1.chatAgent().chatRef().id ? 1 : 0, 1);
            turn(h1, *w1, "PHRASE-H1 from the first", "Plan H1.", "sig-h1", "Answer H1.");
            turn(h2, *w2, "PHRASE-H2 from the second", "Plan H2.", "sig-h2", "Answer H2.");
            P.eq("shared.neither_lost", cs::chatsOf(idS).size() == 2 && h1.chatAgent().chatRef().id != h2.chatAgent().chatRef().id
                                            && storeHolds(idS, "PHRASE-H1") && storeHolds(idS, "PHRASE-H2") ? 1 : 0, 1);
        }

        // ---- SAVED (the project's file gone since it was opened: a save makes a new project, the chat goes with it) ---
        {
            P.eq("saved.made", ps::saveVersion("Engine", program("engine"), 0, {}).has_value() ? 1 : 0, 1);
            const std::string x = idInFile("Engine");
            pg::Processor g;
            const auto w = wire(g);
            P.eq("saved.opened", g.editorPort().openProject("Engine", 0, why) ? 1 : 0, 1);
            g.settle();
            turn(g, *w, "PHRASE-G before its file went", "Plan G.", "sig-g", "Answer G.");
            P.eq("saved.kept", cs::chatsOf(x).size() == 1 ? 1 : 0, 1);
            P.eq("saved.file_deleted", projects.getChildFile("Engine.fmproject").deleteFile() ? 1 : 0, 1);
            pg::SavedVersion sv;
            P.eq("saved.again", g.editorPort().saveVersion("Engine", sv, why) && sv.created ? 1 : 0, 1);
            const std::string y = idInFile("Engine");
            P.eq("saved.new_id", ps::validProjectId(y) && y != x && g.chatAgent().chatOwner() == y ? 1 : 0, 1);
            P.eq("saved.chat_moved", cs::chatsOf(y).size() == 1 && storeHolds(y, "PHRASE-G") && cs::chatsOf(x).empty() ? 1 : 0, 1);
            const std::string named = bytesOf(stateOf(g));
            P.eq("saved.session_names_it", has(named, "projectId=\"" + y + "\"") && has(named, "owner=\"" + y + "\"") ? 1 : 0, 1);
            pg::Processor g2;
            load(g2, stateOf(g));
            P.eq("saved.reload_finds_it", g2.chatAgent().chatOwner() == y && has(said(g2.chatAgent().chatState()), "PHRASE-G") ? 1 : 0, 1);
        }

        // ---- GONE (a project deleted in another instance, with its chats: not written back by this one) -------------
        {
            P.eq("gone.made", ps::saveVersion("Elsewhere", program("elsewhere"), 0, {}).has_value() ? 1 : 0, 1);
            const std::string e = idInFile("Elsewhere");
            pg::Processor k1, k2;
            const auto w = wire(k1);
            P.eq("gone.opened", k1.editorPort().openProject("Elsewhere", 0, why) && k2.editorPort().openProject("Elsewhere", 0, why) ? 1 : 0, 1);
            k1.settle();
            k2.settle();
            turn(k1, *w, "PHRASE-K1 kept", "Plan K1.", "sig-k1", "Answer K1.");
            P.eq("gone.kept", cs::chatsOf(e).size() == 1 ? 1 : 0, 1);
            P.eq("gone.deleted_there", k2.editorPort().deleteProject("Elsewhere", why) && !cs::hasOwner(e) ? 1 : 0, 1);
            turn(k1, *w, "PHRASE-K2 after the delete", "Plan K2.", "sig-k2", "Answer K2.");
            const std::string note = lastOf(k1.chatAgent().chatState(), pg::ChatEntry::Kind::note);
            std::printf("INFO     gone: %s\n", note.c_str());
            P.eq("gone.not_written_back", !cs::hasOwner(e) && has(note, "could not be kept on this Mac") && has(note, "deleted") ? 1 : 0, 1);
            pg::SavedVersion sv;
            P.eq("gone.saved_as", k1.editorPort().saveVersion("Elsewhere Again", sv, why) ? 1 : 0, 1);
            const std::string e2 = idInFile("Elsewhere Again");
            P.eq("gone.goes_on_with_it", k1.chatAgent().chatOwner() == e2 && storeHolds(e2, "PHRASE-K2") && !cs::hasOwner(e)
                                             && has(lastOf(k1.chatAgent().chatState(), pg::ChatEntry::Kind::note), "kept on this Mac again") ? 1 : 0, 1);
        }

        // ---- PICTURES (a chat's panel pictures kept with it: opened again, it keeps its thinking) --------------------
        {
            P.eq("pictures.made", ps::saveVersion("Pictures", program("pictures"), 0, {}).has_value()
                                      && ps::saveVersion("Pictures B", program("pictures b"), 0, {}).has_value() ? 1 : 0, 1);
            const std::string idV = idInFile("Pictures");
            pg::Processor v;
            const auto w = wire(v);
            P.eq("pictures.opened", v.editorPort().openProject("Pictures", 0, why) ? 1 : 0, 1);
            v.settle();
            w->add(fake::Response::ok(fake::Sse().start().thinking("Plan V: look first.", "sig-v")
                                          .tool("toolu_p1", "view_panel", juce::var(new juce::DynamicObject())).end("tool_use")));
            w->add(fake::Response::ok(fake::Sse().start().text("It looks right.").end()));
            pg::picture::forceSoftRasterForTests(true);
            const int from = w->postCount();
            v.chatAgent().send("PHRASE-V how does it look?");
            // The picture is drawn on the message thread (poll): here, this one.
            for (int k = 0; k < 4000 && (v.chatAgent().chatState().busy || w->postCount() < from + 2); ++k)
            {
                v.poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            v.chatAgent().waitIdle();
            pg::picture::forceSoftRasterForTests(false);
            P.eq("pictures.looked", has(w->body(from + 1), "iVBORw0KGgo") ? 1 : 0, 1);
            P.eq("pictures.kept_sealed", storeHolds(idV, "iVBORw0KGgo") && !storeHolds(idV, "pictures_cut")
                                             && filesHolding(keys, "iVBORw0KGgo") == 0 ? 1 : 0, 1);
            P.eq("pictures.away_and_back", v.editorPort().openProject("Pictures B", 0, why) && v.editorPort().openProject("Pictures", 0, why) ? 1 : 0, 1);
            v.settle();
            const int before = w->postCount();
            turn(v, *w, "and now?", "Plan V2.", "sig-v2", "Now.");
            const std::string body = w->body(before);
            P.eq("pictures.thinking_kept", has(body, "sig-v") && has(body, "Plan V: look first.") && has(body, "iVBORw0KGgo") ? 1 : 0, 1);
        }

        // ---- STOPWAIT (a project opened while Claude's view_panel waits for the message thread, which opens it) ------
        {
            P.eq("stopwait.made", ps::saveVersion("Waits", program("waits"), 0, {}).has_value()
                                      && ps::saveVersion("Waits B", program("waits b"), 0, {}).has_value() ? 1 : 0, 1);
            pg::Processor sw;
            const auto w = wire(sw);
            P.eq("stopwait.opened", sw.editorPort().openProject("Waits", 0, why) ? 1 : 0, 1);
            sw.settle();
            w->add(fake::Response::ok(fake::Sse().start().tool("toolu_w1", "view_panel", juce::var(new juce::DynamicObject())).end("tool_use")));
            sw.chatAgent().send("PHRASE-W look at the panel");
            // Nothing polls: the picture waits for this thread.
            for (int k = 0; k < 2500 && !has(sw.chatAgent().chatState().activity, "Looking at the panel"); ++k)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const auto t0 = std::chrono::steady_clock::now();
            const bool opened = sw.editorPort().openProject("Waits B", 0, why);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            P.eq("stopwait.switched", opened && sw.chatAgent().chatOwner() == idInFile("Waits B") ? 1 : 0, 1);
            P.le("stopwait.ms", ms, 3000.0);
            sw.chatAgent().waitIdle();
            sw.settle();
        }

        // ---- KEEPFAIL (the store cannot be written: the chat says so, once, and when it is kept again) ---------------
        {
            pg::Processor f;
            const auto w = wire(f);
            const std::string owner = f.chatAgent().chatOwner();
            const juce::File blocker = chats.getChildFile(juce::String(owner));
            P.eq("keepfail.blocked", blocker.replaceWithText("not a folder") ? 1 : 0, 1);    // its folder cannot be made
            turn(f, *w, "PHRASE-F the folder is a file", "Plan F.", "sig-f", "Answer F.");
            (void)stateOf(f);                                  // a session's save: tried again, said no more
            int notes = 0;
            for (const pg::ChatEntry& e : f.chatAgent().chatState().entries)
                notes += e.kind == pg::ChatEntry::Kind::note && has(e.text, "could not be kept on this Mac") ? 1 : 0;
            std::printf("INFO     keepfail: %s\n", lastOf(f.chatAgent().chatState(), pg::ChatEntry::Kind::note).c_str());
            P.eq("keepfail.said_once", notes, 1);
            P.eq("keepfail.nothing_kept", cs::chatsOf(owner).empty() ? 1 : 0, 1);
            P.eq("keepfail.unblocked", blocker.deleteFile() ? 1 : 0, 1);
            (void)stateOf(f);                                  // the next save keeps it
            P.eq("keepfail.kept_after", storeHolds(owner, "PHRASE-F")
                                            && has(lastOf(f.chatAgent().chatState(), pg::ChatEntry::Kind::note), "kept on this Mac again") ? 1 : 0, 1);
        }

        // ---- CLOSE (a change not kept yet is kept when the plugin closes) -------------------------------------------
        {
            std::string owner;
            {
                pg::Processor z;
                const auto w = wire(z);
                turn(z, *w, "PHRASE-Z before closing", "Plan Z.", "sig-z", "Answer Z.");
                owner = z.chatAgent().chatOwner();
                (void)z.chatAgent().mcpCall("run_program", juce::var(new juce::DynamicObject()));   // Claude Desktop's work
                P.eq("close.changed_not_kept_yet", has(said(z.chatAgent().chatState()), "Claude Desktop is working")
                                                       && !storeHolds(owner, "Claude Desktop is working") ? 1 : 0, 1);
            }
            P.eq("close.kept_when_closed", storeHolds(owner, "Claude Desktop is working") && storeHolds(owner, "PHRASE-Z") ? 1 : 0, 1);
        }
    }

    cs::setDirectoryForTests(juce::File());
    ps::setDirectoryForTests(juce::File());
    return P.finish();
}
