// PROBE name=plugin.factory timeout=300
//
// plugin.factory: the factory projects (plugin/Factory.h: factory/<key>/ compiled in, read-only) of this product, and
// the import that writes them (scripts/import-factory.py), in folders of the probe's own:
//   LIST     every one compiled in reads (no problem), at least one; each named "Factory: <its own name>" (its own a
//            name the store keeps), one version whose files are names a tab may have, the program first; the port's
//            factoryProjects() lists them read-only (one version, their presets counted), projects() none of them;
//            what was compiled in is counted (bytes, printed)
//   PRIVATE  no text of one holds a path on someone's machine, an email address or a key's shape (the import refuses
//            them; a hand edit is caught here)
//   RENDER   each compiles as the plugin opens it (its files as they are), with no error; each of its saved dials
//            lands on a parameter of its name; two seconds at its dials (an effect over stereo noise at -12 dBFS, an
//            instrument a chord, C4 E4 G4 at velocity 0.8, released at 1 s) are finite and audible (a peak above
//            -60 dBFS in the first second), and the output guard raised no alert
//   PRESETS  each of its presets (its dials over the version's) lands on parameters of their names and plays half a
//            second finite, with no alert
//   OPEN     a processor opens the first: its files the draft and what runs, project() it (version 1, not edited), a
//            saved dial at its value; a preset applied, its dial at once, project().preset it
//   READONLY savePreset, deletePreset, deleteVersion, deleteProject and setProjectInfo refuse it (why says it is a
//            factory project); the store's writes refuse its name and write no file
//   COPY     saved (Cmd-S's saveVersion of its name): the user's own copy, a new project under its own name, version 1
//            of the editor's files, its note naming the factory's, with the factory one's presets and info; it is the
//            project open; the factory one is as it was. Saved again from the factory one: "<name> 2", the first copy
//            untouched (a user project of the same name stays the user's). A new name typed while a factory project
//            is open: a copy under it. A name of the user's: a version of that project, no copy
//   SESSION  the session keeps a factory project open by its name; one this build lacks loads as none
//   EXPORT   an export of a factory project imports as a project of its own name (no colon)
//   IMPORT   (python3 on the PATH) the import over made-up projects of two versions: the latest only (version 1, its
//            files, tab and dials), presets' "from" 1 for the latest's and 0 for an older one's, the info kept, a field
//            it does not know left behind; factory::fromFolder reads it; imported.txt lists it; run again with another
//            name: the old folder gone, the new one there, a hand-made folder untouched; a project holding an email
//            address, a path in /Users and a key is refused, nothing written; a map naming a hand-made folder refused
#include "Probe.h"

#include "Rig.h"

#include "plugin/EditorPort.h"
#include "plugin/Factory.h"
#include "plugin/ProjectStore.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;
    constexpr double kFs = 48000.0;

    // The slot of the parameter named so, or -1.
    int slotOf(const pg::Controls& cs, const std::string& name)
    {
        for (int c = 0; c < pg::kMaxControls; ++c)
            if (cs[static_cast<std::size_t>(c)].used && name == cs[static_cast<std::size_t>(c)].name)
                return c;
        return -1;
    }

    // `dials` onto the rig's dials by name: how many name no parameter.
    int setByName(probe::Rig& rig, const std::vector<pg::projectstore::Dial>& dials)
    {
        int unknown = 0;
        for (const pg::projectstore::Dial& d : dials)
        {
            const int c = slotOf(rig.controls(), d.name);
            if (c < 0)
            {
                ++unknown;
                continue;
            }
            const pg::Control& k = rig.controls()[static_cast<std::size_t>(c)];
            rig.norm()[static_cast<std::size_t>(c)] = pg::normOf(k, pg::projectstore::valueOf(d, k, rig.panel().options[static_cast<std::size_t>(c)]));
        }
        return unknown;
    }

    double peakDb(const probe::Stereo& s, std::size_t count)
    {
        double p = 0.0;
        for (std::size_t i = 0; i < std::min(count, s.size()); ++i)
            p = std::max({ p, std::fabs(static_cast<double>(s.l[i])), std::fabs(static_cast<double>(s.r[i])) });
        return probe::dbFromLin(p);
    }

    float dialOf(pg::Processor& p, const std::string& name)
    {
        for (int c = 0; c < pg::kHostControls; ++c)
            if (const pg::Control k = p.control(c); k.used && name == k.name)
                return pg::plainOf(k, p.getParameters()[c]->getValue());
        return std::nanf("");
    }

    bool listedMine(pg::EditorPort& port, const std::string& name, pg::ProjectEntry* out = nullptr)
    {
        for (const pg::ProjectEntry& e : port.projects())
            if (e.name == name)
            {
                if (out != nullptr)
                    *out = e;
                return true;
            }
        return false;
    }

    // What the import refuses, said as plainly as the probe needs it (scripts/import-factory.py has the whole list).
    bool looksPrivate(const std::string& text)
    {
        const juce::String t = juce::String::fromUTF8(text.c_str());
        if (t.contains("/Users/") || t.contains("/home/") || t.contains("sk-ant-") || t.contains("PRIVATE KEY-----"))
            return true;
        for (int at = t.indexOfChar('@'); at > 0; at = t.indexOfChar(at + 1, '@'))
        {
            const juce::String after = t.substring(at + 1).initialSectionContainingOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-");
            if (juce::CharacterFunctions::isLetterOrDigit(t[at - 1]) && after.contains(".") && !after.endsWithChar('.'))
                return true;
        }
        return false;
    }

    // A made-up project of this plugin's, as the plugin saves one: two versions (the first's program a little
    // different), two presets (one from each), its info, and a field no build knows.
    juce::var madeUp(const std::string& extra)
    {
        const std::string params = "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n";
        const std::string program = kInstrument
            ? "// Made Up: a test's\nmodule Voice\n{\n" + params + "    bool held;\n    void start(float n, float v) { held = true; }\n"
                  "    void stop() { held = false; }\n    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                  "        for (int i = 0; i < n; ++i) { left[i] += 0.1f * level[i]; right[i] += 0.1f * level[i]; }\n"
                  "        return held;\n    }\n};\n" + extra
            : "// Made Up: a test's\nmodule Effect\n{\n" + params + "    void process(float* restrict left, float* restrict right, int n)\n"
                  "    {\n        for (int i = 0; i < n; ++i) { left[i] *= level[i]; right[i] *= level[i]; }\n    }\n};\n" + extra;
        const auto dials = [](double v) {
            juce::Array<juce::var> a;
            auto* d = new juce::DynamicObject();
            d->setProperty("name", "level");
            d->setProperty("value", v);
            a.add(juce::var(d));
            return juce::var(a);
        };
        const auto version = [&](int number, const std::string& text, double level) {
            auto* v = new juce::DynamicObject();
            v->setProperty("number", number);
            v->setProperty("saved", static_cast<juce::int64>(1762000000000LL + number));
            v->setProperty("note", number == 1 ? "first save" : "program.fm +1 -1");
            v->setProperty("active", 1);
            juce::Array<juce::var> files;
            for (const auto& [name, t] : std::vector<std::pair<std::string, std::string>>{ { "program.fm", text }, { "notes.fm", "// notes\n" } })
            {
                auto* f = new juce::DynamicObject();
                f->setProperty("name", juce::String(name));
                f->setProperty("text", juce::String::fromUTF8(t.c_str()));
                files.add(juce::var(f));
            }
            v->setProperty("files", files);
            v->setProperty("dials", dials(level));
            return juce::var(v);
        };
        auto* root = new juce::DynamicObject();
        root->setProperty("format", "funk-playground-project");
        root->setProperty("version", 1);
        root->setProperty("plugin", pg::product::kName);
        root->setProperty("kind", kInstrument ? "instrument" : "effect");
        root->setProperty("name", "made up");
        juce::Array<juce::var> versions;
        versions.add(version(1, program + "// older\n", 0.2));
        versions.add(version(7, program, 0.6));
        root->setProperty("versions", versions);
        juce::Array<juce::var> presets;
        for (const auto& [name, from] : std::vector<std::pair<const char*, int>>{ { "Newer", 7 }, { "Older", 1 } })
        {
            auto* pr = new juce::DynamicObject();
            pr->setProperty("name", name);
            pr->setProperty("saved", static_cast<juce::int64>(1762000000100LL));
            pr->setProperty("from", from);
            pr->setProperty("dials", dials(from == 7 ? 0.9 : 0.1));
            presets.add(juce::var(pr));
        }
        root->setProperty("presets", presets);
        auto* info = new juce::DynamicObject();
        info->setProperty("description", "A test's own, made up.");
        info->setProperty("author", "Probe");
        info->setProperty("category", "Pad");
        info->setProperty("tags", juce::Array<juce::var>{ juce::var("warm") });
        info->setProperty("genres", juce::Array<juce::var>());
        root->setProperty("info", juce::var(info));
        root->setProperty("chat", "a field the import leaves behind");
        return juce::var(root);
    }

    // Runs the import: its exit code (-1: it did not start), its output in `out`.
    int runImport(const juce::File& source, const juce::File& map, const juce::File& factory, std::string& out)
    {
        const juce::File repo = juce::File(PROBE_SOURCE_DIR);
        juce::ChildProcess p;
        const juce::StringArray args{ "python3", repo.getChildFile("scripts/import-factory.py").getFullPathName(),
                                      "--source", source.getFullPathName(), "--names", map.getFullPathName(),
                                      "--factory", factory.getFullPathName(),
                                      "--products", repo.getChildFile("products").getFullPathName() };
        if (!p.start(args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
            return -1;
        out = p.readAllProcessOutput().toStdString();
        p.waitForProcessToFinish(60000);
        return static_cast<int>(p.getExitCode());
    }

    bool pythonThere()
    {
        juce::ChildProcess p;
        if (!p.start(juce::StringArray{ "python3", "--version" }, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
            return false;
        (void)p.readAllProcessOutput();
        return p.waitForProcessToFinish(20000) && p.getExitCode() == 0;
    }
}

PROBE(plugin, factory)
{
    const funkgui::HeadlessGuiScope gui;
    namespace ps = pg::projectstore;
    namespace fy = pg::factory;
    const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile(juce::String(pg::product::kEnvPrefix) + "factory-probe-" + juce::String(juce::Time::currentTimeMillis()));
    dir.deleteRecursively();
    ps::setDirectoryForTests(dir.getChildFile("projects"));

    // ---- LIST ----------------------------------------------------------------------------------------------------------
    for (const std::string& problem : fy::problems())
        std::printf("problem: %s\n", problem.c_str());
    P.eq("list.no_problems", static_cast<int64_t>(fy::problems().size()), 0);
    const std::vector<ps::Project>& all = fy::projects();
    P.ge("list.count", static_cast<double>(all.size()), 1.0);
    std::printf("%zu factory projects, %zu bytes compiled in\n", all.size(), fy::bytes());
    P.ge("list.bytes", static_cast<double>(fy::bytes()), 1.0);
    int named = 0, oneVersion = 0, filesOk = 0;
    for (const ps::Project& p : all)
    {
        const std::string own = pg::ownName(p.name);
        named += pg::isFactoryName(p.name) && p.name == pg::factoryName(own) && ps::tidyName(own) == own ? 1 : 0;
        oneVersion += p.versions.size() == 1 ? 1 : 0;
        bool ok = !p.latest().files.empty();
        for (const pg::SourceFile& f : p.latest().files)
            ok = ok && pg::validFileName(f.name);
        filesOk += ok ? 1 : 0;
        std::printf("  %s: %zu files, %zu presets, %zu dials%s\n", p.name.c_str(), p.latest().files.size(), p.presets.size(),
                    p.latest().dials.size(), p.info.description.empty() ? "" : (", \"" + p.info.description.substr(0, 60) + "\"").c_str());
    }
    P.eq("list.named", named, static_cast<int64_t>(all.size()));
    P.eq("list.one_version", oneVersion, static_cast<int64_t>(all.size()));
    P.eq("list.files", filesOk, static_cast<int64_t>(all.size()));
    {
        pg::Processor a;
        pg::EditorPort& port = a.editorPort();
        const std::vector<pg::ProjectEntry> entries = port.factoryProjects();
        bool same = entries.size() == all.size();
        for (std::size_t i = 0; same && i < entries.size(); ++i)
            same = entries[i].name == all[i].name && entries[i].factory && entries[i].latest == all[i].latest().number
                && entries[i].versions == 1 && entries[i].presets == static_cast<int>(all[i].presets.size());
        P.eq("list.port", same ? 1 : 0, 1);
        P.eq("list.not_mine", port.projects().empty() ? 1 : 0, 1);
        P.eq("list.find_own_name", !all.empty() && fy::find(pg::ownName(all[0].name)) == &all[0] && fy::find(all[0].name) == &all[0]
                                       ? 1 : 0, 1);
    }

    // ---- PRIVATE -------------------------------------------------------------------------------------------------------
    {
        int leaks = 0;
        for (const ps::Project& p : all)
        {
            std::vector<std::string> texts{ p.name, p.latest().note, p.info.description, p.info.author, p.info.category };
            for (const pg::SourceFile& f : p.latest().files)
                texts.push_back(f.text);
            for (const ps::Preset& pr : p.presets)
                texts.push_back(pr.name);
            for (const std::string& t : texts)
                if (looksPrivate(t))
                {
                    std::printf("  private in %s: %.40s...\n", p.name.c_str(), t.c_str());
                    ++leaks;
                }
        }
        P.eq("private.none", leaks, 0);
        P.eq("private.check_works", looksPrivate("mail me at someone@example.com") && looksPrivate("/Users/someone/x")
                                        && !looksPrivate("x@3 project:Name@2") ? 1 : 0, 1);
    }

    // ---- RENDER and PRESETS ----------------------------------------------------------------------------------------------
    const std::size_t n = static_cast<std::size_t>(2.0 * kFs);
    const probe::Stereo input = kInstrument ? probe::Stereo(n) : probe::noise(n, 0.25);
    std::vector<probe::Note> chord;
    if constexpr (kInstrument)
    {
        for (const int note : { 60, 64, 67 })
            chord.push_back({ 0, pg::EventKind::noteOn, note, 0.8f });
        for (const int note : { 60, 64, 67 })
            chord.push_back({ static_cast<std::size_t>(kFs), pg::EventKind::noteOff, note, 0.0f });
    }
    int index = 0;
    for (const ps::Project& p : all)
    {
        const std::string key = "project" + std::to_string(++index);
        std::printf("%s: %s\n", key.c_str(), p.name.c_str());
        probe::Rig rig(kFs, 512);
        std::string messages;
        const bool ok = rig.load(p.latest().files, &messages);
        if (!messages.empty())
            std::printf("%s", messages.c_str());
        P.eq(key + ".compiles", ok ? 1 : 0, 1);
        if (!ok)
            continue;
        P.eq(key + ".dials_named", setByName(rig, p.latest().dials), 0);
        const probe::Stereo out = rig.run(input, 256, chord);
        P.eq(key + ".finite", probe::allFinite(out.l) && probe::allFinite(out.r) ? 1 : 0, 1);
        const double peak = peakDb(out, static_cast<std::size_t>(kFs));
        std::printf("  peak %.1f dBFS in the first second\n", peak);
        P.ge(key + ".audible_db", peak, -60.0);
        P.eq(key + ".no_alerts", static_cast<int64_t>(rig.engine().takeAlerts()), 0);
        for (const ps::Preset& pr : p.presets)
        {
            const std::string pk = key + ".preset." + juce::String(pr.name).retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789").toLowerCase().toStdString();
            probe::Rig r(kFs, 512);
            if (!r.load(p.latest().files))
                continue;
            (void)setByName(r, p.latest().dials);
            P.eq(pk + ".dials_named", setByName(r, pr.dials), 0);
            const probe::Stereo o = r.run(kInstrument ? probe::Stereo(n / 4) : probe::noise(n / 4, 0.25), 256, chord);
            P.eq(pk + ".finite", probe::allFinite(o.l) && probe::allFinite(o.r) ? 1 : 0, 1);
            P.eq(pk + ".no_alerts", static_cast<int64_t>(r.engine().takeAlerts()), 0);
        }
    }
    if (all.empty())
        return P.finish();
    const ps::Project& first = all.front();
    const std::string fkey = first.name, own = pg::ownName(first.name);

    // ---- OPEN ----------------------------------------------------------------------------------------------------------
    pg::Processor a;
    pg::EditorPort& pa = a.editorPort();
    std::string why;
    P.eq("open.opens", pa.openProject(fkey, 0, why) ? 1 : 0, 1);
    a.settle();
    P.eq("open.draft", pa.draft().files == first.latest().files ? 1 : 0, 1);
    P.eq("open.runs", pa.status().ok && pa.running() == first.latest().files ? 1 : 0, 1);
    {
        const pg::ProjectState st = pa.project();
        P.eq("open.project", st.name == fkey && st.version == 1 && !st.edited ? 1 : 0, 1);
        if (!first.latest().dials.empty())
        {
            const ps::Dial& d = first.latest().dials.front();
            const float v = dialOf(a, d.name);
            P.near("open.dial", std::isfinite(v) ? v : -1.0e9, d.value, 1.0e-3 * std::max(1.0, std::fabs(static_cast<double>(d.value))));
        }
        if (!first.presets.empty())
        {
            const ps::Preset& pr = first.presets.front();
            P.eq("open.preset_applies", pa.applyPreset(fkey, pr.name, why) && pa.project().preset == pr.name ? 1 : 0, 1);
            a.settle();
            if (!pr.dials.empty())
            {
                const float v = dialOf(a, pr.dials.front().name);
                P.near("open.preset_dial", std::isfinite(v) ? v : -1.0e9, pr.dials.front().value,
                       1.0e-3 * std::max(1.0, std::fabs(static_cast<double>(pr.dials.front().value))));
            }
        }
    }

    // ---- READONLY ------------------------------------------------------------------------------------------------------
    {
        const auto says = [&](bool done) { return !done && why.find("factory project") != std::string::npos; };
        P.eq("readonly.save_preset", says(pa.savePreset("Mine", why)) ? 1 : 0, 1);
        P.eq("readonly.delete_preset", first.presets.empty() || says(pa.deletePreset(fkey, first.presets.front().name, why)) ? 1 : 0, 1);
        P.eq("readonly.delete_version", says(pa.deleteVersion(fkey, 1, why)) ? 1 : 0, 1);
        P.eq("readonly.delete_project", says(pa.deleteProject(fkey, why)) ? 1 : 0, 1);
        P.eq("readonly.info", says(pa.setProjectInfo(fkey, { "x", "y", "Pad", {}, {} }, why)) ? 1 : 0, 1);
        P.eq("readonly.store_save", !ps::saveVersion(fkey, first.latest().files, 0, {}, {}, &why).has_value()
                                        && !ps::savePreset(fkey, "Mine", {}, 1, &why) && !ps::setInfo(fkey, {}, &why) ? 1 : 0, 1);
        P.eq("readonly.no_file", pa.projects().empty() && !dir.getChildFile("projects").getChildFile("Factory " + juce::String(own) + ".fmproject").exists()
                                     ? 1 : 0, 1);
        P.eq("readonly.still_listed", fy::find(fkey) != nullptr && fy::find(fkey)->latest().files == first.latest().files ? 1 : 0, 1);
    }

    // ---- COPY ----------------------------------------------------------------------------------------------------------
    {
        (void)pa.openProject(fkey, 0, why);
        a.settle();
        pg::SavedVersion saved;
        P.eq("copy.saved", pa.saveVersion(fkey, saved, why) ? 1 : 0, 1);
        std::printf("copy: %s v%d (%s)\n", saved.name.c_str(), saved.number, why.c_str());
        P.eq("copy.own_name", saved.copied && saved.created && saved.number == 1 && saved.name == own ? 1 : 0, 1);
        const std::optional<ps::Project> mine = ps::load(own);
        P.eq("copy.one_version", mine && mine->versions.size() == 1 && mine->latest().files == first.latest().files ? 1 : 0, 1);
        P.eq("copy.note", mine && mine->latest().note.find("factory") != std::string::npos ? 1 : 0, 1);
        bool presets = mine.has_value() && mine->presets.size() == first.presets.size();
        for (std::size_t i = 0; presets && i < first.presets.size(); ++i)
            presets = mine->presets[i].name == first.presets[i].name && mine->presets[i].dials.size() == first.presets[i].dials.size();
        P.eq("copy.presets", presets ? 1 : 0, 1);
        P.eq("copy.info", mine && mine->info == first.info ? 1 : 0, 1);
        P.eq("copy.open_now", pa.project().name == own && pa.project().version == 1 ? 1 : 0, 1);
        P.eq("copy.factory_unchanged", fy::find(fkey) != nullptr && fy::find(fkey)->latest().files == first.latest().files ? 1 : 0, 1);

        // Again from the factory one: a second copy; the first stays the user's.
        (void)pa.openProject(fkey, 0, why);
        a.settle();
        P.eq("copy.again", pa.saveVersion(fkey, saved, why) && saved.copied && saved.name == own + " 2" ? 1 : 0, 1);
        pg::ProjectEntry e;
        P.eq("copy.first_untouched", listedMine(pa, own, &e) && e.versions == 1 ? 1 : 0, 1);

        // A new name typed while a factory project is open: a copy under it.
        (void)pa.openProject(fkey, 0, why);
        a.settle();
        P.eq("copy.typed", pa.saveVersion("Typed Copy", saved, why) && saved.copied && saved.name == "Typed Copy"
                               && ps::load("Typed Copy").has_value() && ps::load("Typed Copy")->presets.size() == first.presets.size()
                               ? 1 : 0, 1);
        // A name of the user's: a version of that project, no copy.
        (void)pa.openProject(fkey, 0, why);
        a.settle();
        P.eq("copy.into_mine", pa.saveVersion(own, saved, why) && !saved.copied && saved.name == own ? 1 : 0, 1);
        std::printf("into the user's own: v%d%s\n", saved.number, saved.unchanged ? " (unchanged)" : "");
    }

    // ---- SESSION -------------------------------------------------------------------------------------------------------
    {
        (void)pa.openProject(fkey, 0, why);
        a.settle();
        juce::MemoryBlock state;
        a.getStateInformation(state);
        pg::Processor b;
        b.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        P.eq("session.keeps_it", b.editorPort().project().name == fkey && b.editorPort().project().version == 1 ? 1 : 0, 1);
        const std::unique_ptr<juce::XmlElement> xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
        juce::XmlElement* program = xml != nullptr ? xml->getChildByName("PROGRAM") : nullptr;
        if (program != nullptr)
        {
            program->setAttribute("project", "Factory: Not In This Build");
            juce::MemoryBlock other;
            juce::AudioProcessor::copyXmlToBinary(*xml, other);
            pg::Processor c;
            c.setStateInformation(other.getData(), static_cast<int>(other.getSize()));
            P.eq("session.unknown_none", c.editorPort().project().name.empty() ? 1 : 0, 1);
        }
        else
            P.eq("session.unknown_none", 0, 1);
    }

    // ---- EXPORT --------------------------------------------------------------------------------------------------------
    {
        const juce::File pack = dir.getChildFile("export.fmpack");
        pg::Exported out;
        P.eq("export.written", pa.exportProject(fkey, true, true, pack.getFullPathName().toStdString(), out, why) ? 1 : 0, 1);
        std::string name;
        P.eq("export.imports", pa.importProject(pack.getFullPathName().toStdString(), name, why) ? 1 : 0, 1);
        std::printf("export imported as %s\n", name.c_str());
        P.eq("export.own_name", name.rfind(own, 0) == 0 && name.find(':') == std::string::npos ? 1 : 0, 1);
    }

    // ---- IMPORT --------------------------------------------------------------------------------------------------------
    if (!pythonThere())
        std::printf("IMPORT: no python3 on the PATH, so the import is not run here\n");
    else
    {
        const juce::File src = dir.getChildFile("source"), fac = dir.getChildFile("factory"), map = dir.getChildFile("names.tsv");
        const juce::File folder = src.getChildFile(pg::product::kName);
        folder.createDirectory();
        folder.getChildFile("made up.fmproject").replaceWithText(juce::JSON::toString(madeUp("")));
        const std::string keyWord = kInstrument ? "synth" : "effect";
        // The map names the product by its key or its name: this product's key is its kind's here.
        map.replaceWithText("product\tfrom\tto\n# a comment\n" + juce::String(pg::product::kName) + "\tMade Up\tFirst Name\n");
        std::string out;
        int code = runImport(src, map, fac, out);
        std::printf("import: %d\n%s", code, out.c_str());
        P.eq("import.runs", code, 0);
        juce::File product;
        for (const juce::File& f : fac.findChildFiles(juce::File::findDirectories, false))
            product = f;
        std::printf("into %s\n", product.getFileName().toRawUTF8());
        const juce::File written = product.getChildFile("First Name");
        std::string readWhy;
        const std::optional<ps::Project> p = fy::fromFolder(written, &readWhy);
        if (!p)
            std::printf("not read: %s\n", readWhy.c_str());
        P.eq("import.reads", p.has_value() ? 1 : 0, 1);
        const juce::var source = madeUp("");
        if (p)
        {
            const juce::var latest = source["versions"][1];
            P.eq("import.latest_only", p->versions.size() == 1 && p->latest().number == 1 && p->latest().files.size() == 2
                                           && p->latest().files[0].text == latest["files"][0]["text"].toString().toStdString()
                                           && p->latest().active == 1 && p->latest().dials.size() == 1
                                           && std::fabs(p->latest().dials[0].value - 0.6f) < 1.0e-6f ? 1 : 0, 1);
            const ps::Preset* newer = p->preset("Newer");
            const ps::Preset* older = p->preset("Older");
            P.eq("import.presets_from", newer != nullptr && older != nullptr && newer->from == 1 && older->from == 0 ? 1 : 0, 1);
            P.eq("import.info", p->info.description == "A test's own, made up." && p->info.category == "Pad" && p->info.author == "Probe"
                                    ? 1 : 0, 1);
            P.eq("import.named", p->name == pg::factoryName("First Name") ? 1 : 0, 1);
        }
        const juce::String json = written.getChildFile("project.json").loadFileAsString();
        P.eq("import.unknown_left", json.isNotEmpty() && !json.contains("\"chat\"") && !json.contains("\"text\"") ? 1 : 0, 1);
        P.eq("import.listed", product.getChildFile("imported.txt").loadFileAsString().contains("First Name") ? 1 : 0, 1);
        P.eq("import.product_key", product.getFileName().toStdString() == keyWord ? 1 : 0, 1);

        // Again, renamed, with a folder made by hand beside: the old one gone, the new one there, the hand-made kept.
        product.getChildFile("By Hand").createDirectory();
        product.getChildFile("By Hand").getChildFile("project.json").replaceWithText("{}");
        map.replaceWithText(juce::String(keyWord) + "\tmade up\tSecond Name\n");
        code = runImport(src, map, fac, out);
        P.eq("import.again_runs", code, 0);
        P.eq("import.again_replaces", !written.exists() && product.getChildFile("Second Name").getChildFile("project.json").existsAsFile()
                                          && product.getChildFile("By Hand").getChildFile("project.json").existsAsFile() ? 1 : 0, 1);

        // A map naming the hand-made folder: refused, nothing changed.
        map.replaceWithText(juce::String(keyWord) + "\tmade up\tBy Hand\n");
        code = runImport(src, map, fac, out);
        std::printf("hand-made: %d\n%s", code, out.c_str());
        P.eq("import.hand_made_refused", code == 1 && product.getChildFile("Second Name").exists()
                                             && product.getChildFile("By Hand").getChildFile("project.json").loadFileAsString() == "{}" ? 1 : 0, 1);

        // Something private: refused, nothing written.
        folder.getChildFile("leaky.fmproject").replaceWithText(juce::JSON::toString(madeUp(
            "// mailed to someone@example.com from /Users/someone/Desktop\n// sk-ant-api03-0123456789abcdefABCDEF\n")));
        map.replaceWithText(juce::String(keyWord) + "\tleaky\tLeaky\n");
        code = runImport(src, map, fac, out);
        std::printf("private: %d\n%s", code, out.c_str());
        P.eq("import.private_refused", code == 1 && !product.getChildFile("Leaky").exists() && product.getChildFile("Second Name").exists()
                                           ? 1 : 0, 1);
        P.eq("import.private_said", out.find("an email address") != std::string::npos && out.find("a path on someone's machine") != std::string::npos
                                        && out.find("a key or token") != std::string::npos && out.find("someone@") == std::string::npos
                                        ? 1 : 0, 1);
    }

    ps::setDirectoryForTests({});
    dir.deleteRecursively();
    return P.finish();
}
