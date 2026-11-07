// PROBE name=plugin.projects timeout=180
//
// plugin.projects: the projects (plugin/ProjectStore.h) through the processor's EditorPort, in a folder of the probe's
// own (and never the user's: under CTest the default folder is the probe's sandbox):
//   NAMES    a name tidied: spaces collapsed and trimmed, a slash, a colon, a quote and a leading dot gone, cut to 40;
//            nothing left is ""
//   FOLDER   under CTest the default folder is the sandbox's presets/
//   SAVE     a processor's two files (the second tab shown) and its dials, saved: a new project, version 1, listed
//            with a date; the file is JSON that names this plugin and its kind; project() is it, not edited
//   VERSIONS saved again unchanged, no version added; after an edit (noticed) version 2, its note naming the file's
//            lines; after a dial moved (noticed) version 3, its note "1 dial"; the versions the newest first; another
//            processor opens version 1 (its files, its tab, its dials, not edited) and the newest (its dial); saving
//            from version 1 makes it the newest again (version 4)
//   PRESETS  the dials saved as a preset of the project open (from its version), project().preset it; a dial moved
//            (noticed) and the preset applied: the dial back at once, not edited; a processor with no project open
//            applies it: the project's newest version opens and the preset's dials land after the compile; a preset
//            with no project open is refused; a name taken is replaced (case aside)
//   POLL     a project's presets as the explorer asks for them (ProjectStore's presetsOf): as load() has them; asked
//            again, its file is not read (ProjectStore's parses()), the name in another case the same; after a preset
//            is saved, not read at all (the save kept what it wrote in the cache); a preset more written by another
//            (the file rewritten) seen at the next ask, read once; a change list() read is not read again (one cache);
//            a project that is not there: nothing, and why; the listing has a file whose extension is in capitals, not
//            a folder named as a project; that file's presets, asked by its name spelt as on no file, come from the
//            listing's entry, poll after poll; every write of the store (a version saved, unchanged too, a preset
//            saved or removed, a version removed, the info set, a project added) leaves the cache as the file is, and
//            so does a load(); a damaged file is read once, not at every listing
//   CACHED   the explorer's own poll (listCached, presetsCached, the Reader): a file another wrote is not read by
//            the listing but listed as it was and handed to the Reader, which reads it once on its thread (its
//            revision moves) and the next listing has it; a project never read is listed once the Reader has read it
//   BYNAME   a version whose dials are listed in another order, with a name the program lacks: each lands on its own
//            parameter, the unknown is ignored
//   SAME     a version saved as "glass bloom": the same project, named so now (one file)
//   LABEL    an int whose values have names (a panel.gui's buttons) keeps its value's name with its number: a version
//            saved on PITCH (KEYS CAR PITCH) has "label": "PITCH" in its JSON (a float none), and so does a preset;
//            after TRUCK is inserted before PITCH (the run keeps PITCH: 3) and the dial moved to KEYS, the preset
//            applies PITCH (3, never TRUCK), not edited; version 1 opens on PITCH (2) and a version saved since on its
//            own (3); a project from before labels (the field taken out) lands on the number, as it did, and its
//            latest version saved again unchanged adds no version but gains its label
//   SESSION  the session keeps the project, its version and the preset; a session from before projects names a preset,
//            now the project of that name
//   MIGRATE  a preset file from before projects becomes a project of one version (its files, tab and dials, a note of
//            its date), and the old file moves into "Presets before projects"
//   CAP      a project keeps its newest 100 versions, the numbers going on
//   REFUSE   a project of the other kind is refused (the why names it), a damaged file and a missing name too, and the
//            draft is untouched
//   DELETE   a preset deleted (no longer applied); a version deleted, never the only one; a project deleted: gone from
//            the list, project() none
//   CIRCUITS a program with a netlist whose configuration is the self-test's (AutoClipper.h), saved before its test (no
//            "circuits" in the version), refined (the tests off, Compiler::selfTestNow) and saved again unchanged: no
//            version added, and version 1 now keeps the answer (its "circuits": key and text), as an export's JSON
//            does; opened by a fresh processor with the store emptied, the answer is restored before it compiles (no
//            self-test wanted, no note); a project from before circuits (the field taken out) opens as before, its
//            files, playing the provisional configuration
#include "Probe.h"

#include "AutoClipper.h"

#include "dsp/Compiler.h"
#include "fminus/Circuits.h"
#include "plugin/EditorPort.h"
#include "plugin/ProjectStore.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;

    // A program of two files: its two dials, level and tone, and a function from gain.fm.
    pg::Sources twoFiles()
    {
        const std::string params = "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                                   "    param float tone [[fminus::range(100, 10000)]] [[fminus::default(1000)]];\n";
        const std::string program = kInstrument
            ? "#include \"gain.fm\"\nmodule Voice\n{\n" + params
                  + "    bool held;\n    void start(float n, float v) { held = true; }\n    void stop() { held = false; }\n"
                    "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                    "        for (int i = 0; i < n; ++i) { left[i] += half(level[i]); right[i] += half(level[i]); }\n"
                    "        return held;\n    }\n};\n"
            : "#include \"gain.fm\"\nmodule Effect\n{\n" + params
                  + "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                    "        for (int i = 0; i < n; ++i) { left[i] = half(left[i]) * level[i]; right[i] = half(right[i]) * level[i]; }\n"
                    "    }\n};\n";
        return { { pg::kProgramName, program }, { "gain.fm", "float half(float x) { return 0.5f * x; }\n" } };
    }

    // A dial's value in the program's units.
    float dial(pg::Processor& p, int slot)
    {
        return pg::plainOf(p.control(slot), p.getParameters()[slot]->getValue());
    }

    void setDial(pg::Processor& p, int slot, float plain)
    {
        p.getParameters()[slot]->setValueNotifyingHost(pg::normOf(p.control(slot), plain));
    }

    std::optional<pg::ProjectEntry> listed(pg::EditorPort& port, const std::string& name)
    {
        for (const pg::ProjectEntry& e : port.projects())
            if (e.name == name)
                return e;
        return std::nullopt;
    }

    // The project's file as JSON, changed by `change`, written as `as`.
    template <typename Change>
    void rewrite(const juce::File& dir, const std::string& from, const std::string& as, Change&& change)
    {
        juce::var root = juce::JSON::parse(dir.getChildFile(juce::String(from) + ".fmproject").loadFileAsString());
        change(root);
        dir.getChildFile(juce::String(as) + ".fmproject").replaceWithText(juce::JSON::toString(root));
    }

    // LABEL's program: drive, an int whose values `names` (quoted texts) name through a panel.gui's buttons, and level.
    pg::Sources labelled(const std::string& names, int count)
    {
        const std::string params = "    param int drive [[fminus::range(0, " + std::to_string(count - 1) + ")]];\n"
                                   "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n";
        const std::string program = kInstrument
            ? "module Voice\n{\n" + params + "    bool held;\n    void start(float n, float v) { held = true; }\n"
                  "    void stop() { held = false; }\n    bool process(float* restrict left, float* restrict right, int n) { return held; }\n};\n"
            : "module Effect\n{\n" + params + "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                  "        for (int i = 0; i < n; ++i) { left[i] = left[i] * level[i]; right[i] = right[i] * level[i]; }\n    }\n};\n";
        return { { pg::kProgramName, program }, { "panel.gui", "row {\n    buttons drive " + names + "\n    knob level\n}\n" } };
    }

    juce::var dialVar(const char* name, double value)
    {
        auto* d = new juce::DynamicObject();
        d->setProperty("name", name);
        d->setProperty("value", value);
        return juce::var(d);
    }
}

PROBE(plugin, projects)
{
    const funkgui::HeadlessGuiScope gui;
    namespace ps = pg::projectstore;

    // FOLDER: the sandbox under CTest (the probe is never run outside it).
    const std::string sandbox = std::string(pg::product::kEnvPrefix) + "PREFS_DIR";
    const char* prefs = std::getenv(sandbox.c_str());
    P.eq("folder.sandbox", prefs != nullptr && *prefs != '\0'
                               && ps::directory() == juce::File(juce::String::fromUTF8(prefs)).getChildFile("presets")
                               ? 1 : 0, 1);
    // A folder of the probe's own for the rest.
    const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile(juce::String(pg::product::kEnvPrefix) + "projects-probe-" + juce::String(juce::Time::currentTimeMillis()));
    dir.deleteRecursively();
    ps::setDirectoryForTests(dir);

    // NAMES
    P.eq("names.tidy", ps::tidyName("  Glass   Bloom / 2: \"x\"  ") == "Glass Bloom 2 x" ? 1 : 0, 1);
    P.eq("names.leading_dot", ps::tidyName("..hidden") == "hidden" ? 1 : 0, 1);
    P.eq("names.cut", ps::tidyName(std::string(60, 'a')).size() == ps::kMaxName ? 1 : 0, 1);
    P.eq("names.nothing_left", ps::tidyName(" /:: ").empty() ? 1 : 0, 1);

    // SAVE
    const pg::Sources files = twoFiles();
    pg::Processor a;
    pg::EditorPort& pa = a.editorPort();
    pa.setDraft({ files, 1 });
    pa.run(files);
    a.settle();
    P.eq("save.program_runs", pa.status().ok && pa.running() == files ? 1 : 0, 1);
    setDial(a, 0, 0.8f);
    setDial(a, 1, 2500.0f);
    std::string why;
    pg::SavedVersion saved;
    P.eq("save.saved", pa.saveVersion("Glass Bloom", saved, why) && saved.created && saved.number == 1 ? 1 : 0, 1);
    if (!why.empty())
        std::printf("save: %s\n", why.c_str());
    const std::optional<pg::ProjectEntry> entry = listed(pa, "Glass Bloom");
    P.eq("save.listed", entry.has_value() && entry->latest == 1 && entry->versions == 1 && pa.projects().size() == 1 ? 1 : 0, 1);
    const std::string when = entry ? entry->when : std::string();
    std::printf("listed: Glass Bloom, %s\n", when.c_str());
    P.eq("save.dated", when.size() == 16 && when[4] == '-' && when[13] == ':' ? 1 : 0, 1);
    const juce::File file = dir.getChildFile("Glass Bloom.fmproject");
    const juce::var json = juce::JSON::parse(file.loadFileAsString());
    P.eq("save.json", json["format"].toString() == "funk-playground-project" && json["plugin"].toString() == pg::product::kName
                          && json["kind"].toString() == pg::product::kKind && json["versions"].size() == 1 ? 1 : 0, 1);
    pg::ProjectState st = pa.project();
    P.eq("save.open_now", st.name == "Glass Bloom" && st.version == 1 && st.latest == 1 && !st.edited && !st.dialsEdited ? 1 : 0, 1);

    // VERSIONS
    P.eq("versions.unchanged", pa.saveVersion("Glass Bloom", saved, why) && saved.unchanged && saved.number == 1
                                   && pa.versions("Glass Bloom").size() == 1 ? 1 : 0, 1);
    pg::Draft edited = pa.draft();
    edited.files[0].text += "// an edit\n";
    pa.setDraft(edited);
    P.eq("versions.edit_noticed", pa.project().edited ? 1 : 0, 1);
    P.eq("versions.second", pa.saveVersion("Glass Bloom", saved, why) && !saved.unchanged && saved.number == 2 ? 1 : 0, 1);
    P.eq("versions.saved_not_edited", !pa.project().edited ? 1 : 0, 1);
    setDial(a, 0, 0.3f);
    P.eq("versions.dial_noticed", pa.project().dialsEdited ? 1 : 0, 1);
    P.eq("versions.third", pa.saveVersion("Glass Bloom", saved, why) && saved.number == 3 ? 1 : 0, 1);
    {
        const std::vector<pg::VersionEntry> vs = pa.versions("Glass Bloom");
        for (const pg::VersionEntry& v : vs)
            std::printf("v%d  %s  %s\n", v.number, v.when.c_str(), v.note.c_str());
        P.eq("versions.newest_first", vs.size() == 3 && vs[0].number == 3 && vs[1].number == 2 && vs[2].number == 1 ? 1 : 0, 1);
        P.eq("versions.notes", vs.size() == 3 && vs[2].note == "first save" && vs[1].note == "program.fm +1 -0" && vs[0].note == "1 dial"
                                   ? 1 : 0, 1);
    }
    pg::Processor b;
    pg::EditorPort& pb = b.editorPort();
    const uint64_t before = pb.draftRevision();
    P.eq("versions.open_first", pb.openProject("Glass Bloom", 1, why) ? 1 : 0, 1);
    P.eq("versions.first_draft", pb.draft().files == files && pb.draft().active == 1 && pb.draftRevision() > before ? 1 : 0, 1);
    b.settle();
    P.eq("versions.first_runs", pb.status().ok && pb.running() == files ? 1 : 0, 1);
    std::printf("version 1's dials: level %.4f, tone %.2f\n", static_cast<double>(dial(b, 0)), static_cast<double>(dial(b, 1)));
    P.near("versions.first_level", static_cast<double>(dial(b, 0)), 0.8, 1e-4);
    P.near("versions.first_tone", static_cast<double>(dial(b, 1)), 2500.0, 0.05);
    st = pb.project();
    P.eq("versions.first_open", st.name == "Glass Bloom" && st.version == 1 && st.latest == 3 && !st.edited && !st.dialsEdited ? 1 : 0, 1);
    P.eq("versions.open_newest", pb.openProject("Glass Bloom", 0, why) ? 1 : 0, 1);
    b.settle();
    P.eq("versions.newest", pb.project().version == 3 && pb.draft().files == edited.files ? 1 : 0, 1);
    P.near("versions.newest_level", static_cast<double>(dial(b, 0)), 0.3, 1e-4);
    P.eq("versions.open_again", pb.openProject("Glass Bloom", 1, why) ? 1 : 0, 1);
    b.settle();
    P.eq("versions.older_saved_newest", pb.saveVersion("Glass Bloom", saved, why) && saved.number == 4 && pb.project().version == 4
                                            && pb.versions("Glass Bloom").front().note == "program.fm +0 -1, 1 dial" ? 1 : 0, 1);

    // PRESETS: a is at version 3 (level 0.3, tone 2500).
    P.eq("presets.saved", pa.savePreset("Bright", why) ? 1 : 0, 1);
    {
        const std::vector<pg::PresetEntry> ps = pa.presets("Glass Bloom");
        P.eq("presets.listed", ps.size() == 1 && ps[0].name == "Bright" && ps[0].from == 3 ? 1 : 0, 1);
    }
    P.eq("presets.applied_now", pa.project().preset == "Bright" && !pa.project().dialsEdited ? 1 : 0, 1);
    setDial(a, 0, 0.9f);
    P.eq("presets.dial_noticed", pa.project().dialsEdited ? 1 : 0, 1);
    P.eq("presets.apply", pa.applyPreset("Glass Bloom", "bright", why) ? 1 : 0, 1);
    P.near("presets.dial_back", static_cast<double>(dial(a, 0)), 0.3, 1e-4);
    P.eq("presets.not_edited", !pa.project().dialsEdited && pa.project().preset == "Bright" ? 1 : 0, 1);
    {
        pg::Processor c;
        pg::EditorPort& pc = c.editorPort();
        why.clear();
        P.eq("presets.needs_a_project", !pc.savePreset("Loose", why) && why.find("save the project first") != std::string::npos ? 1 : 0, 1);
        P.eq("presets.apply_opens", pc.applyPreset("Glass Bloom", "Bright", why) ? 1 : 0, 1);
        c.settle();
        st = pc.project();
        std::printf("applied in another: %s v%d, preset %s, level %.4f\n", st.name.c_str(), st.version, st.preset.c_str(),
                    static_cast<double>(dial(c, 0)));
        P.eq("presets.apply_opened", st.name == "Glass Bloom" && st.version == 4 && st.preset == "Bright" && pc.draft().files == files
                                         ? 1 : 0, 1);
        P.near("presets.apply_dial", static_cast<double>(dial(c, 0)), 0.3, 1e-4);
        P.eq("presets.apply_not_edited", !st.edited && !st.dialsEdited ? 1 : 0, 1);
    }
    setDial(a, 1, 4000.0f);
    P.eq("presets.replace", pa.savePreset("BRIGHT", why) && pa.presets("Glass Bloom").size() == 1
                                && pa.presets("Glass Bloom")[0].name == "BRIGHT" ? 1 : 0, 1);
    P.eq("presets.second", pa.savePreset("Dark", why) && pa.presets("Glass Bloom").size() == 2 ? 1 : 0, 1);

    // POLL: the presets as the explorer asks for them every few seconds (ProjectStore's presetsOf), the file read whole
    // only when it changed.
    {
        const auto same = [](const std::optional<std::vector<ps::PresetSummary>>& got, const std::optional<ps::Project>& p) {
            if (!got || !p || got->size() != p->presets.size())
                return false;
            for (std::size_t k = 0; k < got->size(); ++k)
                if ((*got)[k].name != p->presets[k].name || (*got)[k].savedMs != p->presets[k].savedMs
                    || (*got)[k].from != p->presets[k].from)
                    return false;
            return true;
        };
        const auto first = ps::presetsOf("Glass Bloom");
        P.eq("poll.as_load", same(first, ps::load("Glass Bloom")) ? 1 : 0, 1);
        const int p0 = ps::parses();
        for (int k = 0; k < 5; ++k)
            (void)pa.presets("Glass Bloom");
        P.eq("poll.unchanged_reads_nothing", ps::parses() - p0, 0);
        P.eq("poll.case_aside", ps::presetsOf("glass bloom") == first ? 1 : 0, 1);
        // A preset saved (by the store: the processor's preset applied stays): the next ask reads the file once, then
        // not again.
        P.eq("poll.saved", ps::savePreset("Glass Bloom", "Warm", { { "level", 0.3f } }, 3, &why) ? 1 : 0, 1);
        const int p1 = ps::parses();
        const auto warm = ps::presetsOf("Glass Bloom");
        const auto warmAgain = ps::presetsOf("Glass Bloom");
        P.eq("poll.save_kept", ps::parses() - p1, 0);
        P.eq("poll.change_listed", same(warm, ps::load("Glass Bloom")) && warm->size() == first->size() + 1
                                       && warmAgain == warm ? 1 : 0, 1);
        // Written by another (a preset more in its JSON, the file rewritten in place): seen at the next ask.
        const auto addPreset = [](const char* name) {
            return [name](juce::var& root) {
                juce::Array<juce::var>* presets = root["presets"].getArray();
                juce::var copy = presets->getFirst().clone();
                copy.getDynamicObject()->setProperty("name", name);
                presets->add(copy);
            };
        };
        rewrite(dir, "Glass Bloom", "Glass Bloom", addPreset("Zed"));
        const int p2 = ps::parses();
        const auto zed = ps::presetsOf("Glass Bloom");
        P.eq("poll.other_writer", ps::parses() - p2 == 1 && zed && zed->size() == warm->size() + 1 && zed->back().name == "Zed"
                                      ? 1 : 0, 1);
        // One cache with the listing: a change list() read is not read again here.
        rewrite(dir, "Glass Bloom", "Glass Bloom", addPreset("Zz"));
        const int p3 = ps::parses();
        (void)ps::list();
        const auto zz = ps::presetsOf("Glass Bloom");
        P.eq("poll.shared_with_list", ps::parses() - p3 == 1 && zz && zz->back().name == "Zz" ? 1 : 0, 1);
        why.clear();
        P.eq("poll.none", !ps::presetsOf("No Such Project", &why) && why.find("no project") != std::string::npos ? 1 : 0, 1);
        // The listing (readdir, one look a file): a project's file whose extension is in capitals is listed, a folder
        // named as a project is not.
        {
            const juce::File shouty = dir.getChildFile("Shouty.FMPROJECT"), folder = dir.getChildFile("Folder.fmproject");
            dir.getChildFile("Glass Bloom.fmproject").copyFileTo(shouty);
            folder.createDirectory();
            bool listed = false, folderListed = false;
            for (const ps::Entry& e : ps::list())
            {
                listed = listed || e.name == "Shouty";
                folderListed = folderListed || e.name == "Folder";
            }
            P.eq("poll.list_extension_case", listed ? 1 : 0, 1);
            P.eq("poll.list_no_folder", folderListed ? 0 : 1, 1);
            // Its presets asked as the explorer asks, poll after poll: the name spelt as no file is (Shouty.fmproject
            // asked, Shouty.FMPROJECT listed; shouty), the same file, so the listing's entry serves them and the file
            // is not read again.
            const int p4 = ps::parses();
            const auto s1 = ps::presetsOf("Shouty");
            (void)ps::list();
            const auto s2 = ps::presetsOf("Shouty");
            const auto s3 = ps::presetsOf("shouty");
            P.eq("poll.spelling_served", ps::parses() - p4 == 0 && s1 && s1 == zz && s2 == s1 && s3 == s1 ? 1 : 0, 1);
            shouty.deleteFile();
            folder.deleteRecursively();
        }
        // Every write of the store leaves the cache as the file now is, and so does a load(): the listing and the
        // presets after it read no file whole.
        {
            const auto kept = [&](const std::string& row, auto&& write) {
                (void)ps::list();
                P.eq(row + "_written", write() ? 1 : 0, 1);
                const int parsesBefore = ps::parses();
                (void)ps::list();
                (void)ps::presetsOf("Glass Bloom");
                P.eq(row, ps::parses() - parsesBefore, 0);
            };
            const std::optional<ps::Project> gb = ps::load("Glass Bloom");
            pg::Sources keptFiles = gb ? gb->latest().files : pg::Sources{};
            if (!keptFiles.empty())
                keptFiles[0].text += "\n// kept\n";
            const std::vector<ps::Dial> keptDials = gb ? gb->latest().dials : std::vector<ps::Dial>{};
            int added = 0;
            kept("poll.kept_version", [&] {
                const auto r = ps::saveVersion("Glass Bloom", keptFiles, 0, keptDials);
                added = r ? r->number : 0;
                return r.has_value() && !r->unchanged;
            });
            kept("poll.kept_unchanged", [&] { return ps::saveVersion("Glass Bloom", keptFiles, 0, keptDials).has_value(); });
            kept("poll.kept_preset", [&] { return ps::savePreset("Glass Bloom", "Kept", { { "level", 0.4f } }, added); });
            kept("poll.kept_preset_removed", [&] { return ps::removePreset("Glass Bloom", "Kept"); });
            kept("poll.kept_version_removed", [&] { return ps::removeVersion("Glass Bloom", added); });
            const ps::Info info = ps::load("Glass Bloom") ? ps::load("Glass Bloom")->info : ps::Info{};
            kept("poll.kept_info", [&] { return ps::setInfo("Glass Bloom", info); });
            std::string copy;
            kept("poll.kept_added", [&] {
                const std::optional<std::string> n = gb ? ps::add(*gb) : std::nullopt;
                copy = n.value_or("");
                return n.has_value();
            });
            P.eq("poll.kept_added_removed", !copy.empty() && ps::remove(copy) ? 1 : 0, 1);
            rewrite(dir, "Glass Bloom", "Glass Bloom", addPreset("Loaded"));
            kept("poll.kept_load", [&] { return ps::load("Glass Bloom").has_value(); });
            P.eq("poll.kept_load_tidy", ps::removePreset("Glass Bloom", "Loaded") ? 1 : 0, 1);
            // A damaged file: read once, not at every listing (and never listed).
            const juce::File damaged = dir.getChildFile("Damaged.fmproject");
            damaged.replaceWithText("{ not a project");
            const int d0 = ps::parses();
            (void)ps::list();
            (void)ps::list();
            bool shown = false;
            for (const ps::Entry& e : ps::list())
                shown = shown || e.name == "Damaged";
            P.eq("poll.damaged_read_once", ps::parses() - d0, 1);
            P.eq("poll.damaged_not_listed", shown ? 0 : 1, 1);
            damaged.deleteFile();
        }

        // CACHED: the explorer's poll. A file another wrote is listed as it was, read on the Reader's thread once.
        {
            const std::shared_ptr<ps::Reader> reader = ps::reader();
            const auto waitReader = [&] {
                for (int k = 0; k < 2000 && !reader->idle(); ++k)
                    juce::Thread::sleep(5);
                return reader->idle();
            };
            const auto presetsOfEntry = [](const std::vector<ps::Entry>& list, const std::string& name) {
                for (const ps::Entry& e : list)
                    if (e.name == name)
                        return static_cast<int>(e.presets.size());
                return -1;
            };
            (void)ps::list();
            std::vector<std::string> unread;
            const std::vector<ps::Entry> held0 = ps::listCached(unread);
            P.eq("cached.all_held", unread.empty() && presetsOfEntry(held0, "Glass Bloom") >= 0 ? 1 : 0, 1);
            rewrite(dir, "Glass Bloom", "Glass Bloom", addPreset("Other"));
            const int c0 = ps::parses(), r0 = ps::readerParses();
            const uint64_t rev0 = reader->revision();
            unread.clear();
            const std::vector<ps::Entry> stale = ps::listCached(unread);
            std::vector<std::string> unreadPresets;
            const auto stalePresets = ps::presetsCached("Glass Bloom", unreadPresets);
            P.eq("cached.not_read_here", ps::parses() - c0, 0);
            P.eq("cached.listed_as_was", presetsOfEntry(stale, "Glass Bloom") == presetsOfEntry(held0, "Glass Bloom")
                                             && stalePresets && static_cast<int>(stalePresets->size()) == presetsOfEntry(held0, "Glass Bloom")
                                             ? 1 : 0, 1);
            P.eq("cached.handed_on", unread.size() == 1 && unreadPresets == unread ? 1 : 0, 1);
            reader->request(unread);
            reader->request(unreadPresets);              // the same file: read once
            P.eq("cached.reader_done", waitReader() ? 1 : 0, 1);
            P.eq("cached.read_once_there", ps::parses() - c0 == 1 && ps::readerParses() - r0 == 1 ? 1 : 0, 1);
            P.eq("cached.revision", reader->revision() == rev0 + 1 ? 1 : 0, 1);
            unread.clear();
            const std::vector<ps::Entry> fresh = ps::listCached(unread);
            unreadPresets.clear();
            const auto freshPresets = ps::presetsCached("Glass Bloom", unreadPresets);
            P.eq("cached.then_listed", unread.empty() && unreadPresets.empty()
                                           && presetsOfEntry(fresh, "Glass Bloom") == presetsOfEntry(held0, "Glass Bloom") + 1
                                           && freshPresets && freshPresets->size() == static_cast<std::size_t>(presetsOfEntry(fresh, "Glass Bloom"))
                                           ? 1 : 0, 1);
            // A project no one read yet (another wrote it): not listed until the Reader has read it.
            const juce::File newcomer = dir.getChildFile("Newcomer.fmproject");
            dir.getChildFile("Glass Bloom.fmproject").copyFileTo(newcomer);
            unread.clear();
            const std::vector<ps::Entry> newcomer0 = ps::listCached(unread);
            P.eq("cached.new_not_yet", presetsOfEntry(newcomer0, "Newcomer") < 0 && unread.size() == 1 ? 1 : 0, 1);
            reader->request(unread);
            P.eq("cached.new_reader_done", waitReader() ? 1 : 0, 1);
            unread.clear();
            const std::vector<ps::Entry> second = ps::listCached(unread);
            P.eq("cached.new_listed", presetsOfEntry(second, "Newcomer") >= 0 && unread.empty() ? 1 : 0, 1);
            newcomer.deleteFile();
            P.eq("cached.tidy", ps::removePreset("Glass Bloom", "Other") ? 1 : 0, 1);
        }
        // Back as it was (the presets above as saved): Zed and Zz go.
        P.eq("poll.tidy", ps::removePreset("Glass Bloom", "Zed") && ps::removePreset("Glass Bloom", "Zz")
                              && ps::removePreset("Glass Bloom", "Warm") ? 1 : 0, 1);
    }

    // BYNAME: version 1's dials listed tone first, and one the program lacks.
    {
        rewrite(dir, "Glass Bloom", "Reordered", [](juce::var& root) {
            root.getDynamicObject()->setProperty("name", "Reordered");
            juce::var v1 = root["versions"][0];
            v1.getDynamicObject()->setProperty("dials", juce::Array<juce::var>{ dialVar("tone", 4000.0), dialVar("nosuch", 7.0),
                                                                                dialVar("level", 0.25) });
            root.getDynamicObject()->setProperty("versions", juce::Array<juce::var>{ v1 });
        });
        pg::Processor c;
        P.eq("byname.opened", c.editorPort().openProject("Reordered", 0, why) ? 1 : 0, 1);
        c.settle();
        P.near("byname.level", static_cast<double>(dial(c, 0)), 0.25, 1e-4);
        P.near("byname.tone", static_cast<double>(dial(c, 1)), 4000.0, 0.05);
        P.eq("byname.removed", c.editorPort().deleteProject("Reordered", why) ? 1 : 0, 1);
    }

    // SAME: one name, case aside.
    P.eq("same.saved", pa.saveVersion("glass bloom", saved, why) && saved.number == 5 ? 1 : 0, 1);
    P.eq("same.one_project", listed(pa, "glass bloom").has_value() && !listed(pa, "Glass Bloom").has_value() && pa.projects().size() == 1
                                 ? 1 : 0, 1);
    P.eq("same.open", pa.project().name == "glass bloom" && pa.project().version == 5 ? 1 : 0, 1);

    // SESSION
    {
        juce::MemoryBlock state;
        a.getStateInformation(state);
        pg::Processor d;
        d.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        st = d.editorPort().project();
        std::printf("session: %s v%d, preset %s\n", st.name.c_str(), st.version, st.preset.c_str());
        P.eq("session.keeps_it", st.name == "glass bloom" && st.version == 5 && st.preset == "Dark" && !st.edited ? 1 : 0, 1);
        // A session from before projects: <PROGRAM preset="...">.
        const std::unique_ptr<juce::XmlElement> xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
        juce::XmlElement* program = xml != nullptr ? xml->getChildByName("PROGRAM") : nullptr;
        P.eq("session.old_form", program != nullptr ? 1 : 0, 1);
        if (program != nullptr)
        {
            program->removeAttribute("project");
            program->removeAttribute("projectVersion");
            program->removeAttribute("projectPreset");
            program->setAttribute("preset", "Old Sound");
            juce::MemoryBlock old;
            juce::AudioProcessor::copyXmlToBinary(*xml, old);
            pg::Processor e;
            e.setStateInformation(old.getData(), static_cast<int>(old.getSize()));
            P.eq("session.old_preset_a_project", e.editorPort().project().name == "Old Sound" && e.editorPort().project().version == 0
                                                     ? 1 : 0, 1);
        }
    }

    // MIGRATE: a preset file from before projects.
    {
        auto* root = new juce::DynamicObject();
        root->setProperty("format", "funk-playground-preset");
        root->setProperty("version", 1);
        root->setProperty("plugin", pg::product::kName);
        root->setProperty("kind", pg::product::kKind);
        root->setProperty("name", "Old Sound");
        root->setProperty("active", 1);
        juce::Array<juce::var> fs;
        for (const pg::SourceFile& f : files)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("name", juce::String(f.name));
            o->setProperty("text", juce::String(f.text));
            fs.add(juce::var(o));
        }
        root->setProperty("files", fs);
        root->setProperty("dials", juce::Array<juce::var>{ dialVar("level", 0.6), dialVar("tone", 3000.0) });
        const juce::File legacy = dir.getChildFile("Old Sound.fmpreset");
        legacy.replaceWithText(juce::JSON::toString(juce::var(root)));
        const std::optional<pg::ProjectEntry> old = listed(pa, "Old Sound");
        P.eq("migrate.listed", old.has_value() && old->latest == 1 && old->versions == 1 ? 1 : 0, 1);
        P.eq("migrate.moved_aside", !legacy.exists() && dir.getChildFile(ps::kMigratedFolder).getChildFile("Old Sound.fmpreset").exists()
                                        ? 1 : 0, 1);
        const std::vector<pg::VersionEntry> vs = pa.versions("Old Sound");
        std::printf("migrated: %s\n", vs.empty() ? "-" : vs[0].note.c_str());
        P.eq("migrate.note", vs.size() == 1 && vs[0].note.rfind("the preset saved ", 0) == 0 ? 1 : 0, 1);
        pg::Processor c;
        P.eq("migrate.opens", c.editorPort().openProject("Old Sound", 0, why) ? 1 : 0, 1);
        c.settle();
        P.eq("migrate.files", c.editorPort().draft().files == files && c.editorPort().draft().active == 1 ? 1 : 0, 1);
        P.near("migrate.level", static_cast<double>(dial(c, 0)), 0.6, 1e-4);
        P.near("migrate.tone", static_cast<double>(dial(c, 1)), 3000.0, 0.05);
    }

    // LABEL
    {
        const pg::Sources three = labelled("\"KEYS\" \"CAR\" \"PITCH\"", 3);
        const pg::Sources four = labelled("\"KEYS\" \"CAR\" \"TRUCK\" \"PITCH\"", 4);
        pg::Processor c;
        pg::EditorPort& pc = c.editorPort();
        pc.setDraft({ three, 0 });
        pc.run(three);
        c.settle();
        setDial(c, 0, 2.0f);                                    // PITCH
        P.eq("label.saved", pc.saveVersion("Gears", saved, why) && pc.savePreset("On pitch", why) ? 1 : 0, 1);
        const juce::var root = juce::JSON::parse(dir.getChildFile("Gears.fmproject").loadFileAsString());
        const juce::var d0 = root["versions"][0]["dials"][0], d1 = root["versions"][0]["dials"][1];
        P.eq("label.in_json", d0["name"].toString() == "drive" && static_cast<int>(d0["value"]) == 2 && d0["label"].toString() == "PITCH"
                                  && !d1.hasProperty("label") && root["presets"][0]["dials"][0]["label"].toString() == "PITCH" ? 1 : 0, 1);
        pc.setDraft({ four, 0 });
        pc.run(four);
        c.settle();
        P.near("label.run_keeps_pitch", static_cast<double>(dial(c, 0)), 3.0, 0.0);
        setDial(c, 0, 0.0f);                                    // KEYS
        P.eq("label.preset_applied", pc.applyPreset("Gears", "On pitch", why) ? 1 : 0, 1);
        std::printf("label: the preset applied after TRUCK came: %s\n", c.getParameters()[0]->getCurrentValueAsText().toRawUTF8());
        P.near("label.preset_pitch", static_cast<double>(dial(c, 0)), 3.0, 0.0);
        P.eq("label.preset_not_edited", c.getParameters()[0]->getCurrentValueAsText() == "PITCH" && !pc.project().dialsEdited ? 1 : 0, 1);
        P.eq("label.second_saved", pc.saveVersion("Gears", saved, why) && saved.number == 2 ? 1 : 0, 1);
        pg::Processor e;
        P.eq("label.first_opens", e.editorPort().openProject("Gears", 1, why) ? 1 : 0, 1);
        e.settle();
        P.near("label.first_on_pitch", static_cast<double>(dial(e, 0)), 2.0, 0.0);
        P.eq("label.second_opens", e.editorPort().openProject("Gears", 2, why) ? 1 : 0, 1);
        e.settle();
        P.near("label.second_on_pitch", static_cast<double>(dial(e, 0)), 3.0, 0.0);
        P.eq("label.second_text", e.getParameters()[0]->getCurrentValueAsText() == "PITCH" ? 1 : 0, 1);
        // A project from before labels: the number, as before (2 is TRUCK now).
        rewrite(dir, "Gears", "Old gears", [](juce::var& old) {
            old.getDynamicObject()->setProperty("name", "Old gears");
            for (const char* list : { "versions", "presets" })
                for (juce::var& item : *old[list].getArray())
                    for (juce::var& d : *item["dials"].getArray())
                        d.getDynamicObject()->removeProperty("label");
        });
        P.eq("label.old_applied", e.editorPort().applyPreset("Old gears", "On pitch", why) ? 1 : 0, 1);
        e.settle();
        P.near("label.old_by_number", static_cast<double>(dial(e, 0)), 2.0, 0.0);
        // Its latest version opened (on 3, PITCH: the number) and saved again unchanged: no version added, the version
        // given its values' names.
        P.eq("label.old_opens", e.editorPort().openProject("Old gears", 2, why) ? 1 : 0, 1);
        e.settle();
        const bool same = e.editorPort().saveVersion("Old gears", saved, why) && saved.unchanged && saved.number == 2;
        const juce::var old = juce::JSON::parse(dir.getChildFile("Old gears.fmproject").loadFileAsString());
        const juce::var v1 = old["versions"][0]["dials"][0], v2 = old["versions"][1]["dials"][0];
        std::printf("label: an old version saved again: unchanged %d, number %d, versions %d\n", saved.unchanged ? 1 : 0, saved.number,
                    old["versions"].size());
        P.eq("label.old_unchanged_no_version", same && old["versions"].size() == 2 ? 1 : 0, 1);
        P.eq("label.old_gains_label", v2["label"].toString() == "PITCH" && static_cast<int>(v2["value"]) == 3 && !v1.hasProperty("label")
                                          ? 1 : 0, 1);
        P.eq("label.removed", pc.deleteProject("Gears", why) && pc.deleteProject("Old gears", why) ? 1 : 0, 1);
    }

    // CAP: the newest 100 versions.
    {
        pg::Processor c;
        pg::EditorPort& pc = c.editorPort();
        bool all = true;
        pg::Draft d{ files, 0 };
        for (int k = 0; k < 101 && all; ++k)
        {
            d.files[1].text = "float half(float x) { return 0.5f * x; }\n// " + std::to_string(k) + "\n";
            pc.setDraft(d);
            all = pc.saveVersion("Capped", saved, why);
        }
        const std::vector<pg::VersionEntry> vs = pc.versions("Capped");
        P.eq("cap.saved", all ? 1 : 0, 1);
        P.eq("cap.kept", static_cast<int64_t>(vs.size()), static_cast<int64_t>(ps::kMaxVersions));
        P.eq("cap.numbers_go_on", !vs.empty() && vs.front().number == 101 && vs.back().number == 2 ? 1 : 0, 1);
    }

    // REFUSE
    {
        rewrite(dir, "glass bloom", "Other Kind", [](juce::var& root) {
            root.getDynamicObject()->setProperty("kind", kInstrument ? "effect" : "instrument");
            root.getDynamicObject()->setProperty("plugin", "OtherPlayground");
        });
        dir.getChildFile("Broken.fmproject").replaceWithText("{ not json");
        pg::Processor e;
        const pg::Draft untouched = e.editorPort().draft();
        why.clear();
        P.eq("refuse.other_kind", !e.editorPort().openProject("Other Kind", 0, why) && why.find(kInstrument ? "effect" : "instrument")
                                                                                       != std::string::npos ? 1 : 0, 1);
        std::printf("other kind: %s\n", why.c_str());
        P.eq("refuse.damaged", !e.editorPort().openProject("Broken", 0, why) && why.find("damaged") != std::string::npos ? 1 : 0, 1);
        P.eq("refuse.missing", !e.editorPort().openProject("Nothing Here", 0, why) ? 1 : 0, 1);
        P.eq("refuse.no_version", !e.editorPort().openProject("glass bloom", 77, why) ? 1 : 0, 1);
        P.eq("refuse.draft_untouched", e.editorPort().draft() == untouched ? 1 : 0, 1);
        P.eq("refuse.not_listed", !listed(pa, "Other Kind") && !listed(pa, "Broken") ? 1 : 0, 1);
    }

    // DELETE
    P.eq("delete.preset", pa.deletePreset("glass bloom", "dark", why) && pa.presets("glass bloom").size() == 1 ? 1 : 0, 1);
    P.eq("delete.preset_unapplied", pa.project().name == "glass bloom" && pa.project().preset.empty() ? 1 : 0, 1);
    P.eq("delete.version", pa.deleteVersion("glass bloom", 2, why) && pa.versions("glass bloom").size() == 4 ? 1 : 0, 1);
    P.eq("delete.old_only", pa.deleteVersion("Old Sound", 1, why) ? 0 : 1, 1);
    std::printf("only version: %s\n", why.c_str());
    P.eq("delete.project", pa.deleteProject("glass bloom", why) && !listed(pa, "glass bloom") ? 1 : 0, 1);
    P.eq("delete.none_open", pa.project().name.empty() ? 1 : 0, 1);

    // CIRCUITS
    {
        pg::circuits::Answers& store = pg::circuits::answers();
        store.clearForTests();
        const pg::Sources pedal = { { pg::kProgramName, probe::clipperProgram("clipper.cir", "Clipper", false) },
                                    { "clipper.cir", probe::kAutoClipper } };
        const std::vector<std::string> keys = pg::circuits::keysOf(pedal);
        const std::string key = keys.empty() ? std::string() : keys.front();
        const juce::File pedalFile = dir.getChildFile("Circuit Pedal.fmproject");
        const auto keptOf = [](const juce::File& f) {
            return juce::JSON::parse(f.loadFileAsString())["versions"][0]["circuits"];
        };
        pg::Processor c;
        c.compiler().setSelfTesting(false);
        pg::EditorPort& pc = c.editorPort();
        pc.setDraft({ pedal, 0 });
        pc.run(pedal);
        c.settle();
        const pg::CompileStatus provisional = pc.status();
        P.eq("circuits.saved_provisional", provisional.ok && !provisional.notes.empty() && pc.saveVersion("Circuit Pedal", saved, why)
                                               && saved.created && keptOf(pedalFile).isVoid() ? 1 : 0, 1);
        const bool refined = c.compiler().selfTestNow();
        c.settle();
        const std::string answer = store.find(key).value_or("");
        P.eq("circuits.refined", refined && pc.status().ok && !answer.empty() ? 1 : 0, 1);
        P.eq("circuits.unchanged_keeps_answer", pc.saveVersion("Circuit Pedal", saved, why) && saved.unchanged && saved.number == 1
                                                    && pc.versions("Circuit Pedal").size() == 1 ? 1 : 0, 1);
        const juce::var kept = keptOf(pedalFile);
        std::printf("circuits: version 1 keeps %s\n", juce::JSON::toString(kept, true).toRawUTF8());
        P.eq("circuits.version_field", kept.size() == 1 && kept[0]["key"].toString().toStdString() == key
                                           && kept[0]["text"].toString().toStdString() == answer ? 1 : 0, 1);
        {
            const std::optional<ps::Project> p = ps::load("Circuit Pedal", &why);
            const std::optional<ps::Project> back = p ? ps::fromJson(ps::toJson(*p), "Circuit Pedal", &why) : std::nullopt;
            P.eq("circuits.json_round_trip", back && back->versions.size() == 1 && back->versions[0].circuits.size() == 1
                                                 && back->versions[0].circuits[0].key == key && back->versions[0].circuits[0].text == answer
                                                 ? 1 : 0, 1);
        }
        store.clearForTests();
        pg::Processor d;
        d.compiler().setSelfTesting(false);
        P.eq("circuits.opens", d.editorPort().openProject("Circuit Pedal", 0, why) ? 1 : 0, 1);
        d.settle();
        const pg::CompileStatus opened = d.editorPort().status();
        std::printf("circuits: opened: %s%s\n", opened.errors.c_str(), opened.notes.empty() ? "no messages" : opened.notes.c_str());
        P.eq("circuits.restored_on_open", store.find(key) == answer && opened.ok && opened.notes.empty() && !d.compiler().selfTesting()
                                              && d.editorPort().running() == pedal ? 1 : 0, 1);

        // A project from before circuits: the version without the field.
        rewrite(dir, "Circuit Pedal", "Older Pedal", [](juce::var& root) {
            root.getDynamicObject()->setProperty("name", "Older Pedal");
            root["versions"][0].getDynamicObject()->removeProperty("circuits");
        });
        store.clearForTests();
        pg::Processor e;
        e.compiler().setSelfTesting(false);
        P.eq("circuits.older_opens", e.editorPort().openProject("Older Pedal", 0, why) ? 1 : 0, 1);
        e.settle();
        P.eq("circuits.older_plays_provisional", e.editorPort().draft().files == pedal && e.editorPort().status().ok
                                                     && e.editorPort().status().notes == provisional.notes && store.size() == 0
                                                     && e.compiler().selfTesting() ? 1 : 0, 1);
        store.clearForTests();
    }

    ps::setDirectoryForTests({});
    dir.deleteRecursively();
    return P.finish();
}
