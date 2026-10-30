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
//   BYNAME   a version whose dials are listed in another order, with a name the program lacks: each lands on its own
//            parameter, the unknown is ignored
//   SAME     a version saved as "glass bloom": the same project, named so now (one file)
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

#include <cmath>
#include <cstdio>
#include <cstdlib>
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
