// PROBE name=plugin.saves timeout=180
//
// plugin.saves: Claude's projects tool (ChatProjects.cpp, ProcessorSaves.cpp) through a real Processor, in a folder of
// the probe's own (never the user's), over MCP (Claude Desktop's calls, each on a thread of its own as the link's worker
// runs it, this thread the message thread that polls) and through the plugin's own chat (a turn against a scripted API:
// nothing reaches the network):
//   TOOL     the tool is the chat's and MCP's: its title in words, destructive (replace and discard take something
//            away), its MCP description the first sentence (only when the user asks), five actions; its notes in the
//            guide's projects part, with when to save (never silently), and start names that part before a save
//   LIST     with no project open: says so, the user's none yet, the factory's (read-only) named; an action it does not
//            take (delete) refused, nothing changed
//   VERSION  save_version with no project open and no name refused; with a name a new project (version 1, its note
//            "Claude: <note> (first save)"), the project open; set_dial and save_version back to back on one worker
//            with no poll() between (the job waits, the dial not yet set): version 2 at the dial set (the first kept as
//            it was); again unchanged: nothing added; a category given: the project's, with nothing new to save too
//            (said, and a notice); a job no poll() takes: withdrawn after its wait, nothing saved, a poll() after it
//            doing nothing
//   PRESET   save_preset with a category: the preset (its dials, its category), applied; a name it has refused without
//            replace (the preset as it was); with replace (and a category of blanks, none): its old values in the
//            result, the new ones kept, its category kept; no project open: refused
//   APPLY    apply_preset while a dial moved off the preset: refused, naming the dial, nothing applied; with discard:
//            applied (its value back); a preset the project lacks: refused, naming its presets
//   OPEN     open of another project with nothing unsaved: its files the editor's, its dials once it compiled (the
//            next tool measures them); the editor's file changed: open refused (unsaved files), with discard: Name@N
//            opened (that version's files and dials); a version not kept refused, naming those kept
//   FACTORY  a factory project opened (read-only, said); save_preset refused (save_version first); save_version: the
//            user's own copy (a new project, its presets with it), the factory's as it was
//   AB       while an A/B is on, save_version, save_preset and apply_preset refused (the user compares A and B: Keep or
//            Close first), nothing saved, no notice; closed, a save goes through
//   NOTICE   each save, open and preset applied: the editor's status says it once (EditorPort::claudeNotice: "CLAUDE
//            SAVED VERSION 2 OF GLASS SONG"); a refusal and a list say nothing
//   JOURNAL  Claude Desktop's saves are not journalled (it knows); the plugin's chat's are ("the plugin's own chat
//            saved version 3 of ...", "... saved preset ...")
//   CHAT     a turn: list, save_version with a note, save_preset with a category, then the answer: the tool is in the
//            request (22 tools), its results no errors, the chat's lines say what it saved, the version's note is
//            Claude's; a second turn's open over moved dials comes back an error, nothing opened; with a check set that
//            fails, a turn whose only tool applies a preset ends with its answer (no nudge, the check kept), and one
//            that opens another project clears the check, its result saying so, and ends with its answer too
#include "FakeClaude.h"
#include "Probe.h"

#include "plugin/ChatAgent.h"
#include "plugin/EditorPort.h"
#include "plugin/ProjectStore.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;
    constexpr double kFs = 48000.0;
    constexpr int kBlock = 256;

    // Two dials, level and tone.
    pg::Sources program(const std::string& comment = {})
    {
        const std::string params = "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                   "    param float tone [[fminus::range(100, 10000)]] [[fminus::default(1000)]];\n";
        const std::string text = (comment.empty() ? std::string() : "// " + comment + "\n")
            + (kInstrument
                   ? "module Voice\n{\n" + params
                         + "    bool held;\n    void start(float n, float v) { held = true; }\n    void stop() { held = false; }\n"
                           "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                           "        for (int i = 0; i < n; ++i) { left[i] += level[i]; right[i] += level[i]; }\n"
                           "        return held;\n    }\n};\n"
                   : "module Effect\n{\n" + params
                         + "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                           "        for (int i = 0; i < n; ++i) { left[i] = level[i]; right[i] = level[i]; }\n    }\n};\n");
        return { { pg::kProgramName, text } };
    }

    float dial(pg::Processor& p, int slot) { return pg::plainOf(p.control(slot), p.getParameters()[slot]->getValue()); }

    void setDial(pg::Processor& p, int slot, float plain)
    {
        p.getParameters()[slot]->setValueNotifyingHost(pg::normOf(p.control(slot), plain));
    }

    juce::var object(std::initializer_list<std::pair<const char*, juce::var>> kv)
    {
        auto* o = new juce::DynamicObject();
        for (const auto& [k, v] : kv)
            o->setProperty(k, v);
        return juce::var(o);
    }

    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    std::string textOf(const pg::ChatAgent::McpResult& r)
    {
        std::string all;
        if (const juce::Array<juce::var>* c = r.content.getArray())
            for (const juce::var& b : *c)
                all += b["text"].toString().toStdString();
        return all;
    }

    std::string tokenOf(const std::string& status)
    {
        const std::size_t at = status.rfind("Token: ");
        return at == std::string::npos ? std::string() : status.substr(at + 7, status.find(' ', at + 7) - at - 7);
    }

    // An MCP call as the link makes it, on a worker of its own, this (the message) thread polling meanwhile.
    pg::ChatAgent::McpResult call(pg::Processor& p, const std::string& tool, const juce::var& args)
    {
        pg::ChatAgent::McpResult r;
        std::atomic<bool> done{ false };
        std::thread t([&] {
            r = p.chatAgent().mcpCall(tool, args);
            done.store(true);
        });
        for (int k = 0; k < 20000 && !done.load(); ++k)
        {
            p.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        t.join();
        p.settle();
        return r;
    }

    pg::ChatAgent::McpResult projects(pg::Processor& p, std::initializer_list<std::pair<const char*, juce::var>> kv)
    {
        return call(p, "projects", object(kv));
    }

    // The chat's turn, this thread polling until it is over.
    void turn(pg::Processor& p, fake::Script& script, int posts, const std::string& message)
    {
        const int before = script.postCount();
        p.chatAgent().send(message);
        for (int k = 0; k < 20000 && (p.chatAgent().chatState().busy || script.postCount() < before + posts); ++k)
        {
            p.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        p.chatAgent().waitIdle();
        p.settle();
    }

    // The last request's tool results, by tool_use id: their text and whether they are errors.
    struct Result
    {
        std::string text;
        bool error = false;
    };
    Result resultOf(const std::string& body, const std::string& id)
    {
        const juce::var b = pg::claude::parseJson(body);
        const juce::var all = b.getProperty("messages", {});
        const juce::Array<juce::var>* messages = all.getArray();
        if (messages == nullptr)
            return {};
        for (const juce::var& m : *messages)
            if (const juce::var parts = m.getProperty("content", {}); const juce::Array<juce::var>* content = parts.getArray())
                for (const juce::var& c : *content)
                    if (c.getProperty("type", "").toString() == "tool_result" && c.getProperty("tool_use_id", "").toString() == juce::String(id))
                        return { c.getProperty("content", "").toString().toStdString(), static_cast<bool>(c.getProperty("is_error", false)) };
        return {};
    }

    std::vector<std::string> toolEntries(const pg::ChatState& s)
    {
        std::vector<std::string> out;
        for (const pg::ChatEntry& e : s.entries)
            if (e.kind == pg::ChatEntry::Kind::tool)
                out.push_back(e.text);
        return out;
    }

    bool hasEntry(const pg::ChatState& s, const std::string& part)
    {
        const std::vector<std::string> es = toolEntries(s);
        return std::any_of(es.begin(), es.end(), [&](const std::string& e) { return has(e, part); });
    }
}

PROBE(plugin, saves)
{
    const funkgui::HeadlessGuiScope gui;
    namespace ps = pg::projectstore;
    const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile(juce::String(pg::product::kEnvPrefix) + "saves-probe-" + juce::String(juce::Time::currentTimeMillis()));
    dir.deleteRecursively();
    ps::setDirectoryForTests(dir);

    pg::Processor a;
    pg::EditorPort& pa = a.editorPort();
    a.setRateAndBufferSizeDetails(kFs, kBlock);
    a.prepareToPlay(kFs, kBlock);
    pa.setDraft({ program(), 0 });
    pa.run(program());
    a.settle();
    P.eq("setup.runs", pa.status().ok ? 1 : 0, 1);
    std::string why;

    // ---- TOOL ------------------------------------------------------------------------------------------------------------
    {
        juce::var tool;
        const juce::var tools = pg::ChatAgent::mcpTools();
        if (const juce::Array<juce::var>* t = tools.getArray())
            for (const juce::var& x : *t)
                if (x["name"].toString() == "projects")
                    tool = x;
        P.eq("tool.mcp", tool["title"].toString() == "Projects and presets"
                             && tool["description"].toString() == "List the user's projects and presets; save, open or apply one only when the user asks."
                             && static_cast<bool>(tool["annotations"]["destructiveHint"]) && !static_cast<bool>(tool["annotations"]["readOnlyHint"])
                         ? 1 : 0, 1);
        const juce::var actionEnum = tool["inputSchema"]["properties"]["action"]["enum"];
        const juce::Array<juce::var>* actions = actionEnum.getArray();
        P.eq("tool.actions", actions != nullptr ? actions->size() : 0, 5);
        bool chatHas = false;
        const juce::var defs = pg::ChatAgent::toolDefinitions();
        if (const juce::Array<juce::var>* t = defs.getArray())
            for (const juce::var& x : *t)
                if (x["name"].toString() == "projects")
                    chatHas = x["description"].toString().contains("replace: true") && x["description"].toString().contains("discard: true");
        P.eq("tool.chat", chatHas ? 1 : 0, 1);
        bool found = false;
        const std::string part = pg::ChatAgent::mcpGuidePart("projects", found);
        P.eq("tool.guide_part", found && has(part, "<tool name=\"projects\"") && has(part, "never silently") && has(part, "discard: true") ? 1 : 0, 1);
        const std::string start = pg::ChatAgent::mcpGuidePart("start", found);
        P.eq("tool.guide_start", has(start, "read it before you save, open or apply one") && !has(start, "<tool name=\"projects\"") ? 1 : 0, 1);
    }

    // ---- LIST ------------------------------------------------------------------------------------------------------------
    const std::vector<ps::Entry> factory = ps::factoryList();
    P.eq("setup.factory", factory.empty() ? 0 : 1, 1);
    const std::string factoryName = factory.empty() ? std::string() : factory.front().name;
    {
        const pg::ChatAgent::McpResult r = projects(a, { { "action", "list" } });
        const std::string t = textOf(r);
        std::printf("list (none open):\n%s\n", t.c_str());
        P.eq("list.none_open", !r.isError && has(t, "No project is open") && has(t, "The user has no projects") ? 1 : 0, 1);
        P.eq("list.factory", has(t, "Factory projects (read-only") && has(t, factoryName) ? 1 : 0, 1);
        const pg::ChatAgent::McpResult bad = projects(a, { { "action", "delete" }, { "name", juce::String(factoryName) } });
        P.eq("list.no_delete", bad.isError && has(textOf(bad), "action is list, save_version, save_preset, apply_preset or open") ? 1 : 0, 1);
    }
    uint64_t notices = pa.claudeNotice().count;
    P.eq("notice.none_yet", static_cast<int64_t>(notices), 0);

    // ---- VERSION -------------------------------------------------------------------------------------------------------
    {
        const pg::ChatAgent::McpResult none = projects(a, { { "action", "save_version" } });
        P.eq("version.needs_name", none.isError && has(textOf(none), "No project is open") ? 1 : 0, 1);
        const std::string token = tokenOf(textOf(a.chatAgent().mcpCall("get_status", object({}))));
        setDial(a, 0, 0.3f);
        const pg::ChatAgent::McpResult r = projects(a, { { "action", "save_version" }, { "name", "Glass Song" },
                                                         { "note", "a level and a tone" }, { "category", "Pads" } });
        std::printf("save_version (new): %s\n", textOf(r).c_str());
        const std::optional<ps::Project> p = ps::load("Glass Song");
        P.eq("version.new", !r.isError && p && p->versions.size() == 1 && p->latest().note == "Claude: a level and a tone (first save)" ? 1 : 0, 1);
        P.eq("version.open", pa.project().name == "Glass Song" && pa.project().version == 1 ? 1 : 0, 1);
        P.eq("version.category", p && p->info.category == "Pads" && has(textOf(r), "Its category: Pads") ? 1 : 0, 1);
        P.eq("notice.new", pa.claudeNotice().count == notices + 1 && pa.claudeNotice().text == "CLAUDE SAVED GLASS SONG (A NEW PROJECT)" ? 1 : 0, 1);
        notices = pa.claudeNotice().count;
        const std::string since = textOf(a.chatAgent().mcpCall("get_status", object({ { "since", juce::String(token) } })));
        P.eq("journal.desktop_not", has(since, "saved version") || has(since, "saved \"Glass Song\"") ? 0 : 1, 1);

        // A dial moved by Claude's set_dial and a save straight after it, on one worker with no poll() between them (as
        // the link runs Claude Desktop's calls): this thread polls only once the save's job waits, so the dial and the
        // job meet in one poll(), which applies the dial first. Saved again: version 2, at the dial set.
        pg::ChatAgent::McpResult r2;
        {
            std::atomic<bool> done{ false };
            std::thread t([&] {
                (void)a.chatAgent().mcpCall("set_dial", object({ { "name", "level" }, { "value", 0.7 } }));
                r2 = a.chatAgent().mcpCall("projects", object({ { "action", "save_version" } }));
                done.store(true);
            });
            for (int k = 0; k < 5000 && !a.claudeJobWaiting() && !done.load(); ++k)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            P.eq("version.job_waits", a.claudeJobWaiting() ? 1 : 0, 1);
            P.near("version.dial_not_yet", static_cast<double>(dial(a, 0)), 0.3, 1e-4);   // no poll() since the set_dial
            for (int k = 0; k < 20000 && !done.load(); ++k)
            {
                a.poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            t.join();
            a.settle();
        }
        const std::optional<ps::Project> p2 = ps::load("Glass Song");
        std::printf("save_version (again): %s\n", textOf(r2).c_str());
        P.eq("version.second", !r2.isError && p2 && p2->versions.size() == 2 && p2->latest().number == 2
                                   && has(textOf(r2), "Saved version 2 of \"Glass Song\"") ? 1 : 0, 1);
        P.near("version.second_dial", p2 && !p2->latest().dials.empty() ? static_cast<double>(p2->latest().dials[0].value) : -1.0, 0.7, 1e-4);
        P.eq("version.first_kept", p2 && p2->versions.front().number == 1 && p2->versions.front().files == p->latest().files
                                       && p2->versions.front().dials == p->latest().dials ? 1 : 0, 1);
        P.eq("version.note_plain", p2 && p2->latest().note.rfind("Claude (", 0) == 0 ? 1 : 0, 1);
        P.eq("notice.second", pa.claudeNotice().count == notices + 1 && pa.claudeNotice().text == "CLAUDE SAVED VERSION 2 OF GLASS SONG" ? 1 : 0, 1);
        notices = pa.claudeNotice().count;
        const pg::ChatAgent::McpResult r3 = projects(a, { { "action", "save_version" } });
        P.eq("version.unchanged", !r3.isError && has(textOf(r3), "Nothing new to save") && ps::load("Glass Song")->versions.size() == 2 ? 1 : 0, 1);
        P.eq("notice.unchanged_none", static_cast<int64_t>(pa.claudeNotice().count), static_cast<int64_t>(notices));
        // A category given with nothing new to save: set all the same (it is how Claude sets one), and said.
        const pg::ChatAgent::McpResult cat = projects(a, { { "action", "save_version" }, { "category", "Keys" } });
        std::printf("save_version (unchanged, a category): %s\n", textOf(cat).c_str());
        const std::optional<ps::Project> keys = ps::load("Glass Song");
        P.eq("version.category_unchanged", !cat.isError && has(textOf(cat), "Nothing new to save") && has(textOf(cat), "Its category: Keys (was Pads)")
                                               && keys && keys->info.category == "Keys" && keys->versions.size() == 2 ? 1 : 0, 1);
        P.eq("notice.category", pa.claudeNotice().count == notices + 1 && pa.claudeNotice().text == "CLAUDE PUT GLASS SONG IN KEYS" ? 1 : 0, 1);
        notices = pa.claudeNotice().count;

        // A job poll() never takes (a host that holds the message thread): withdrawn once its wait is over, nothing
        // saved, and a poll() after it does nothing.
        {
            setDial(a, 1, 5000.0f);
            a.setClaudeJobWaitForTests(150);
            pg::ChatAgent::McpResult w;
            std::thread t([&] { w = a.chatAgent().mcpCall("projects", object({ { "action", "save_version" } })); });
            t.join();                                        // nothing polls meanwhile
            a.setClaudeJobWaitForTests(0);
            const bool waiting = a.claudeJobWaiting();
            a.poll();
            std::printf("save_version (never taken): %s\n", textOf(w).c_str());
            const std::optional<ps::Project> after = ps::load("Glass Song");
            P.eq("version.withdrawn", w.isError && has(textOf(w), "did not get to it in time") && has(textOf(w), "nothing was done")
                                          && !waiting && after && after->versions.size() == 2 ? 1 : 0, 1);
            P.eq("notice.withdrawn_none", static_cast<int64_t>(pa.claudeNotice().count), static_cast<int64_t>(notices));
            setDial(a, 1, 1000.0f);                          // back where version 2 has it
            a.settle();
        }
    }

    // ---- PRESET --------------------------------------------------------------------------------------------------------
    {
        setDial(a, 1, 2000.0f);
        const pg::ChatAgent::McpResult r = projects(a, { { "action", "save_preset" }, { "name", "Warm" }, { "category", "Soft" } });
        std::printf("save_preset: %s\n", textOf(r).c_str());
        const auto presets = ps::presetsOf("Glass Song");
        const auto warm = [&]() -> std::optional<ps::PresetSummary> {
            if (const auto ps2 = ps::presetsOf("Glass Song"))
                for (const ps::PresetSummary& s : *ps2)
                    if (s.name == "Warm")
                        return s;
            return std::nullopt;
        };
        P.eq("preset.saved", !r.isError && warm() && warm()->category == "Soft" && pa.project().preset == "Warm" ? 1 : 0, 1);
        P.eq("notice.preset", pa.claudeNotice().text == "CLAUDE SAVED PRESET WARM IN GLASS SONG" ? 1 : 0, 1);
        notices = pa.claudeNotice().count;
        setDial(a, 0, 0.2f);
        const pg::ChatAgent::McpResult again = projects(a, { { "action", "save_preset" }, { "name", "warm" } });
        P.eq("preset.refused", again.isError && has(textOf(again), "has a preset \"Warm\" already") && has(textOf(again), "replace: true") ? 1 : 0, 1);
        P.near("preset.kept", warm() && !warm()->dials.empty() ? static_cast<double>(warm()->dials[0].value) : -1.0, 0.7, 1e-4);
        P.eq("notice.refused_none", static_cast<int64_t>(pa.claudeNotice().count), static_cast<int64_t>(notices));
        // Replaced, with a category that tidies to nothing (none given): the preset keeps its own.
        const pg::ChatAgent::McpResult over = projects(a, { { "action", "save_preset" }, { "name", "Warm" }, { "replace", true },
                                                            { "category", "  " } });
        std::printf("save_preset (replace): %s\n", textOf(over).c_str());
        P.eq("preset.replaced", !over.isError && has(textOf(over), "Replaced the preset \"Warm\"") && has(textOf(over), "Its old values: level 0.7")
                                    && has(textOf(over), "(now 0.2") && has(textOf(over), "Its category stays Soft") ? 1 : 0, 1);
        P.near("preset.replaced_value", warm() && !warm()->dials.empty() ? static_cast<double>(warm()->dials[0].value) : -1.0, 0.2, 1e-4);
        P.eq("preset.category_kept", warm() && warm()->category == "Soft" ? 1 : 0, 1);
        P.eq("notice.replaced", pa.claudeNotice().text == "CLAUDE REPLACED PRESET WARM IN GLASS SONG" ? 1 : 0, 1);
        (void)presets;
    }

    // ---- APPLY ---------------------------------------------------------------------------------------------------------
    {
        setDial(a, 0, 0.9f);                                 // off Warm's 0.2: unsaved
        notices = pa.claudeNotice().count;
        const pg::ChatAgent::McpResult r = projects(a, { { "action", "apply_preset" }, { "name", "Warm" } });
        std::printf("apply_preset (moved): %s\n", textOf(r).c_str());
        P.eq("apply.refused", r.isError && has(textOf(r), "unsaved changes (1 dial moved (level) from the preset \"Warm\")") ? 1 : 0, 1);
        P.near("apply.refused_dial", static_cast<double>(dial(a, 0)), 0.9, 1e-4);
        P.eq("notice.apply_refused_none", static_cast<int64_t>(pa.claudeNotice().count), static_cast<int64_t>(notices));
        const pg::ChatAgent::McpResult d = projects(a, { { "action", "apply_preset" }, { "name", "Warm" }, { "discard", true } });
        P.eq("apply.discarded", !d.isError && has(textOf(d), "Applied the preset \"Warm\"") && has(textOf(d), "dropped, as the user asked") ? 1 : 0, 1);
        P.near("apply.value", static_cast<double>(dial(a, 0)), 0.2, 1e-4);
        P.eq("notice.applied", pa.claudeNotice().text == "CLAUDE APPLIED PRESET WARM" ? 1 : 0, 1);
        const pg::ChatAgent::McpResult missing = projects(a, { { "action", "apply_preset" }, { "name", "Cold" } });
        P.eq("apply.missing", missing.isError && has(textOf(missing), "has no preset \"Cold\". Its presets: Warm") ? 1 : 0, 1);
    }

    // ---- OPEN ----------------------------------------------------------------------------------------------------------
    {
        // Another project, saved by the user (its level 0.6), then back on Glass Song version 2.
        const pg::Sources other = program("Other");
        pa.setDraft({ other, 0 });
        pa.run(other);
        a.settle();
        setDial(a, 0, 0.6f);
        pg::SavedVersion saved;
        P.eq("open.setup", pa.saveVersion("Other Song", saved, why) && pa.openProject("Glass Song", 2, why) ? 1 : 0, 1);
        a.settle();
        const pg::ChatAgent::McpResult r = projects(a, { { "action", "open" }, { "name", "Other Song" } });
        std::printf("open: %s\n", textOf(r).c_str());
        P.eq("open.opened", !r.isError && has(textOf(r), "Opened version 1 of \"Other Song\"") && has(textOf(r), "It compiled and plays")
                                && pa.project().name == "Other Song" && pa.draft().files == other ? 1 : 0, 1);
        P.near("open.dials", static_cast<double>(dial(a, 0)), 0.6, 1e-4);
        P.eq("notice.opened", pa.claudeNotice().text == "CLAUDE OPENED OTHER SONG V1" ? 1 : 0, 1);
        // The editor's file changed: refused; with discard, Glass Song's version 1.
        pa.setDraft({ program("Other, edited"), 0 });
        const pg::ChatAgent::McpResult refused = projects(a, { { "action", "open" }, { "name", "Glass Song@1" } });
        P.eq("open.refused", refused.isError && has(textOf(refused), "the editor's files (changed since version 1)")
                                 && pa.project().name == "Other Song" ? 1 : 0, 1);
        const pg::ChatAgent::McpResult v1 = projects(a, { { "action", "open" }, { "name", "Glass Song@1" }, { "discard", true } });
        const std::optional<ps::Project> g = ps::load("Glass Song");
        P.eq("open.version", !v1.isError && pa.project().name == "Glass Song" && pa.project().version == 1 && g
                                 && pa.draft().files == g->versions.front().files ? 1 : 0, 1);
        P.near("open.version_dial", static_cast<double>(dial(a, 0)), 0.3, 1e-4);
        const pg::ChatAgent::McpResult gone = projects(a, { { "action", "open" }, { "name", "@7" } });
        P.eq("open.not_kept", gone.isError && has(textOf(gone), "keeps no version 7. It keeps 1, 2") ? 1 : 0, 1);
    }

    // ---- FACTORY -------------------------------------------------------------------------------------------------------
    {
        const std::optional<ps::Project> before = ps::load(factoryName);
        const pg::ChatAgent::McpResult r = projects(a, { { "action", "open" }, { "name", juce::String(factoryName) } });
        std::printf("open (factory): %s\n", textOf(r).c_str());
        P.eq("factory.opened", !r.isError && has(textOf(r), "a factory project, read-only") && pa.project().name == factoryName ? 1 : 0, 1);
        const pg::ChatAgent::McpResult pr = projects(a, { { "action", "save_preset" }, { "name", "Mine" } });
        P.eq("factory.preset_refused", pr.isError && has(textOf(pr), "is a factory project, read-only: save_version first") ? 1 : 0, 1);
        const pg::ChatAgent::McpResult s = projects(a, { { "action", "save_version" } });
        std::printf("save_version (factory): %s\n", textOf(s).c_str());
        const std::string own = pg::ownName(factoryName);
        const std::optional<ps::Project> copy = ps::load(own);
        P.eq("factory.copy", !s.isError && copy && copy->versions.size() == 1 && before && copy->presets.size() == before->presets.size()
                                 && pa.project().name == own && has(textOf(s), "the user's own copy") ? 1 : 0, 1);
        const std::optional<ps::Project> after = ps::load(factoryName);
        P.eq("factory.untouched", before && after && after->versions.size() == before->versions.size()
                                      && after->latest().files == before->latest().files && after->presets.size() == before->presets.size() ? 1 : 0, 1);
        P.eq("notice.copy", has(pa.claudeNotice().text, "YOUR OWN COPY") ? 1 : 0, 1);
    }

    // ---- AB: while the user compares two versions, what plays is a side's: no save, no preset ---------------------------
    {
        const std::optional<ps::Project> before = ps::load(pa.project().name);
        notices = pa.claudeNotice().count;
        P.eq("ab.started", pa.startAb("Glass Song", 1, 2, why) && pa.ab().on ? 1 : 0, 1);
        const pg::ChatAgent::McpResult v = projects(a, { { "action", "save_version" }, { "note", "during the A/B" } });
        const pg::ChatAgent::McpResult pr = projects(a, { { "action", "save_preset" }, { "name", "During" } });
        const pg::ChatAgent::McpResult ap = projects(a, { { "action", "apply_preset" }, { "name", "Warm" } });
        std::printf("save_version (A/B on): %s\n", textOf(v).c_str());
        const std::optional<ps::Project> after = ps::load(pa.project().name);
        P.eq("ab.refused", v.isError && has(textOf(v), "comparing A and B") && pr.isError && has(textOf(pr), "Nothing was saved")
                               && ap.isError && has(textOf(ap), "Not applied") && pa.ab().on ? 1 : 0, 1);
        P.eq("ab.nothing_saved", before && after && after->versions.size() == before->versions.size()
                                     && after->presets.size() == before->presets.size() ? 1 : 0, 1);
        P.eq("notice.ab_none", static_cast<int64_t>(pa.claudeNotice().count), static_cast<int64_t>(notices));
        pa.endAb();
        a.settle();
        const pg::ChatAgent::McpResult again = projects(a, { { "action", "save_version" } });
        P.eq("ab.closed_saves", !pa.ab().on && !again.isError ? 1 : 0, 1);
    }

    // ---- CHAT: the plugin's own chat, a turn against a scripted API --------------------------------------------------
    {
        pg::ChatAgent& agent = a.chatAgent();
        auto script = std::make_shared<fake::Script>();
        script->models = fake::modelsBody({ { "claude-opus-5-5", "Claude Opus 5.5", true, { "low", "medium", "high", "xhigh", "max" } } });
        agent.setTransport(std::make_unique<fake::Transport>(script));
        P.eq("chat.key", agent.saveKey(fake::fakeKey(), why) ? 1 : 0, 1);
        // Glass Song's version 2 open, a dial moved: the turn saves version 3 with a note, and a preset.
        P.eq("chat.setup", pa.openProject("Glass Song", 2, why) ? 1 : 0, 1);
        a.settle();
        setDial(a, 1, 4000.0f);
        const std::string token = tokenOf(textOf(agent.mcpCall("get_status", object({}))));
        script->add(fake::Response::ok(fake::Sse()
                                           .start()
                                           .tool("toolu_p1", "projects", object({ { "action", "list" } }))
                                           .tool("toolu_p2", "projects", object({ { "action", "save_version" }, { "note", "a brighter tone" } }))
                                           .tool("toolu_p3", "projects", object({ { "action", "save_preset" }, { "name", "Chat Bright" },
                                                                                  { "category", "Leads" } }))
                                           .end("tool_use")));
        script->add(fake::Response::ok(fake::Sse().start().text("Saved version 3 of Glass Song and the preset Chat Bright.").end()));
        turn(a, *script, 2, "save this as a new version, and as a preset called Chat Bright");
        const std::string last = script->body(script->postCount() - 1);
        int toolsSent = 0;
        bool projectsSent = false;
        const juce::var sent = pg::claude::parseJson(script->body(0)).getProperty("tools", {});   // held: getArray() points in
        if (const juce::Array<juce::var>* t = sent.getArray())
            for (const juce::var& x : *t)
            {
                ++toolsSent;
                projectsSent = projectsSent || x["name"].toString() == "projects";
            }
        std::printf("chat: %d tools sent%s\n", toolsSent, projectsSent ? ", projects among them" : "");
        P.eq("chat.tool_sent", projectsSent && toolsSent == 22 ? 1 : 0, 1);
        const Result list = resultOf(last, "toolu_p1"), ver = resultOf(last, "toolu_p2"), pre = resultOf(last, "toolu_p3");
        std::printf("chat: %s\n      %s\n      %s\n", list.text.substr(0, list.text.find('\n')).c_str(), ver.text.c_str(), pre.text.c_str());
        P.eq("chat.results", !list.error && !ver.error && !pre.error && has(list.text, "Open: \"Glass Song\", version 2 of 2")
                                 && has(ver.text, "Saved version 3 of \"Glass Song\"") && has(pre.text, "Saved the preset \"Chat Bright\"") ? 1 : 0, 1);
        const pg::ChatState st = agent.chatState();
        P.eq("chat.entries", hasEntry(st, "saved version 3 of \"Glass Song\"") && hasEntry(st, "saved preset \"Chat Bright\" of \"Glass Song\"")
                                 && hasEntry(st, "looked at the projects") ? 1 : 0, 1);
        const std::optional<ps::Project> g = ps::load("Glass Song");
        P.eq("chat.note", g && g->latest().number == 3 && g->latest().note.rfind("Claude: a brighter tone (", 0) == 0 ? 1 : 0, 1);
        const std::string since = textOf(agent.mcpCall("get_status", object({ { "since", juce::String(token) } })));
        std::printf("chat since:\n%s\n", since.c_str());
        P.eq("journal.chat", has(since, "the plugin's own chat saved version 3 of \"Glass Song\"")
                                 && has(since, "the plugin's own chat saved preset \"Chat Bright\"") ? 1 : 0, 1);
        P.eq("notice.chat", pa.claudeNotice().text == "CLAUDE SAVED PRESET CHAT BRIGHT IN GLASS SONG" ? 1 : 0, 1);

        // A second turn: open over a moved dial, refused.
        setDial(a, 0, 0.05f);
        script->add(fake::Response::ok(fake::Sse().start().tool("toolu_p4", "projects", object({ { "action", "open" }, { "name", "Other Song" } })).end("tool_use")));
        script->add(fake::Response::ok(fake::Sse().start().text("You have unsaved changes: shall I save them first?").end()));
        turn(a, *script, 2, "open Other Song");
        const Result open = resultOf(script->body(script->postCount() - 1), "toolu_p4");
        P.eq("chat.open_refused", open.error && has(open.text, "unsaved changes") && pa.project().name == "Glass Song" ? 1 : 0, 1);

        // Checks Claude set for the program (one that fails) never judge what the user asked to open or apply: a turn
        // whose only tool applies a preset ends with its answer, the checks kept; one that opens another project clears
        // them (they were the program's before it), its result saying so, and ends with its answer too.
        juce::Array<juce::var> impossible;
        impossible.add(object({ { "label", "Impossibly loud" }, { "metric", kInstrument ? "rms_db" : "sine_rms_db" }, { "min", 30.0 } }));
        const std::string set = textOf(call(a, "set_checks", object({ { "checks", juce::var(impossible) } })));
        P.eq("checks.set_failing", agent.chatState().checks && has(set, "FAIL") ? 1 : 0, 1);
        script->add(fake::Response::ok(fake::Sse().start().tool("toolu_p5", "projects", object({ { "action", "apply_preset" }, { "name", "Warm" },
                                                                                                  { "discard", true } })).end("tool_use")));
        script->add(fake::Response::ok(fake::Sse().start().text("Warm is on.").end()));
        const int b5 = script->postCount();
        turn(a, *script, 2, "try the Warm preset, my changes can go");
        const Result applied = resultOf(script->body(script->postCount() - 1), "toolu_p5");
        std::printf("checks, apply_preset turn: %d posts, %s\n", script->postCount() - b5, applied.text.c_str());
        P.eq("checks.apply_no_nudge", !applied.error && script->postCount() - b5 == 2 && agent.chatState().checks
                                          && pa.project().preset == "Warm" ? 1 : 0, 1);
        script->add(fake::Response::ok(fake::Sse().start().tool("toolu_p6", "projects", object({ { "action", "open" }, { "name", "Other Song" },
                                                                                                  { "discard", true } })).end("tool_use")));
        script->add(fake::Response::ok(fake::Sse().start().text("Other Song is open.").end()));
        const int b6 = script->postCount();
        turn(a, *script, 2, "open Other Song");
        const Result opened = resultOf(script->body(script->postCount() - 1), "toolu_p6");
        std::printf("checks, open turn: %d posts, %s\n", script->postCount() - b6, opened.text.c_str());
        P.eq("checks.open_cleared", !opened.error && has(opened.text, "Opened version 1 of \"Other Song\"")
                                        && has(opened.text, "The check you set was for the program before it, so the plugin cleared it")
                                        && script->postCount() - b6 == 2 && !agent.chatState().checks
                                        && pa.project().name == "Other Song" ? 1 : 0, 1);
    }

    ps::setDirectoryForTests({});
    dir.deleteRecursively();
    return P.finish();
}
