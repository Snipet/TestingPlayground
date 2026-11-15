// PROBE name=plugin.export timeout=180
//
// plugin.export: a project's info and its two exports (plugin/ProjectExport.h) through the processor's EditorPort, in a
// folder of the probe's own:
//   INFO     setProjectInfo keeps a description, an author, a category, tags and genres, tidied (the tags split at
//            commas, each once case aside, a long description cut), and the project file holds them
//   PACKAGE  a project of 30 versions exported as a .fmpack: "FMPK", format 1, xz on macOS (the body an .xz stream,
//            its magic) and zlib elsewhere, the header's size and CRC-32 the JSON's, far smaller than its text
//   IMPORT   the package imported: a project of its own ("<name> 2", the name being taken), its versions, presets and
//            info the same; opened, the newest version's files and dials
//   NEWEST   exported with the newest version only: one version, numbered as it was
//   SOURCE   a .zip: <Name>/README.md (the description, the tags, the presets), <Name>/project.json (the source
//            format), the newest version's files at the top and each older one's under versions/v<N>/; imported, the
//            same versions and files
//   ZLIB     a package whose body is zlib (as a build without xz writes) imports
//   REFUSE   a package of the other kind (the why names it), one with a byte changed (its check), one cut short, a file
//            that is no export, a zip with no project.json, and (but on macOS) one in xz, the why naming it: refused,
//            nothing added
//   THROWN   an export that throws with its temporary written (fminus/Failure.h's seam), one project and a bundle:
//            fails in words, the folder holding nothing new, and the next export is taken
//   BUNDLE   several projects as one bundle (startExport, off the message thread): held before its store's work
//            (setExportGate), the export says it is in flight and no project's file is read on the message thread
//            (parses() still across startExport and exporting()); once let go it writes a .zip of stored entries:
//            bundle.json (its format, kind and each project's name and file) and each project's own .fmpack, a factory
//            project under its own name, two names that differ by case made unique case aside; each member alone a
//            package read() takes; read() refuses the bundle, saying so; imported (importProjects), every project
//            comes in with its versions, presets, info and samples the same; a hand-made bundle with one of the other
//            kind and one of this kind: that one taken, the other refused, naming the kind; a cancel (held, then let
//            go) leaves no file and no temporary; a file is never replaced: one made at the target meanwhile (a bundle,
//            and one project) is left as it is, the export failing in words, and one there as it starts refuses it at
//            once (one project and several), the file untouched; a single export through the job writes the bytes
//            exportProject wrote; 257 projects and a path not full refused at once, 2 GB of project files in all (a
//            sparse file) refused on the job's thread, no file written; a processor that goes with an export held
//            cancels it and waits for it, no file left
#include "Probe.h"

#include "fminus/Failure.h"
#include "plugin/EditorPort.h"
#include "plugin/ProjectExport.h"
#include "plugin/ProjectStore.h"
#include "plugin/Processor.h"

#include "dsp/Assets.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;

    pg::Sources twoFiles(int edit)
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
        return { { pg::kProgramName, program + "// edit " + std::to_string(edit) + "\n" },
                 { "gain.fm", "float half(float x) { return 0.5f * x; }\n" } };
    }

    float dial(pg::Processor& p, int slot) { return pg::plainOf(p.control(slot), p.getParameters()[slot]->getValue()); }

    void setDial(pg::Processor& p, int slot, float plain)
    {
        p.getParameters()[slot]->setValueNotifyingHost(pg::normOf(p.control(slot), plain));
    }

    std::vector<uint8_t> bytesOf(const juce::File& f)
    {
        juce::MemoryBlock b;
        f.loadFileAsData(b);
        const auto* d = static_cast<const uint8_t*>(b.getData());
        return { d, d + b.getSize() };
    }

    void writeBytes(const juce::File& f, const std::vector<uint8_t>& b) { f.replaceWithData(b.data(), b.size()); }

    // A package of `json`, its body compressed by `method` (stored or zlib only).
    std::vector<uint8_t> packageOf(const std::string& json, pg::projectexport::Method method)
    {
        std::vector<uint8_t> body;
        if (method == pg::projectexport::Method::zlib)
        {
            juce::MemoryOutputStream mem;
            {
                juce::GZIPCompressorOutputStream z(mem, 9);
                z.write(json.data(), json.size());
            }
            const auto* d = static_cast<const uint8_t*>(mem.getData());
            body.assign(d, d + mem.getDataSize());
        }
        else
            body.assign(json.begin(), json.end());
        std::vector<uint8_t> out(20, 0);
        std::memcpy(out.data(), "FMPK", 4);
        out[4] = 1;
        out[5] = static_cast<uint8_t>(method);
        for (int i = 0; i < 8; ++i)
            out[static_cast<std::size_t>(8 + i)] = static_cast<uint8_t>(static_cast<uint64_t>(json.size()) >> (8 * i));
        const uint32_t crc = pg::projectexport::crc32(json.data(), json.size());
        for (int i = 0; i < 4; ++i)
            out[static_cast<std::size_t>(16 + i)] = static_cast<uint8_t>(crc >> (8 * i));
        out.insert(out.end(), body.begin(), body.end());
        return out;
    }

    bool sameVersions(const pg::projectstore::Project& a, const pg::projectstore::Project& b)
    {
        if (a.versions.size() != b.versions.size())
            return false;
        for (std::size_t i = 0; i < a.versions.size(); ++i)
        {
            const auto& x = a.versions[i];
            const auto& y = b.versions[i];
            if (x.number != y.number || x.files != y.files || x.active != y.active || x.note != y.note || x.dials.size() != y.dials.size())
                return false;
        }
        return true;
    }
}

PROBE(plugin, export)
{
    const funkgui::HeadlessGuiScope gui;
    namespace ps = pg::projectstore;
    namespace px = pg::projectexport;
    const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile(juce::String(pg::product::kEnvPrefix) + "export-probe-" + juce::String(juce::Time::currentTimeMillis()));
    dir.deleteRecursively();
    const juce::File out = dir.getChildFile("out");
    ps::setDirectoryForTests(dir.getChildFile("projects"));

    // A project of 30 versions, each a line more, and two presets.
    pg::Processor a;
    pg::EditorPort& pa = a.editorPort();
    std::string why;
    pg::SavedVersion saved;
    pa.setDraft({ twoFiles(0), 1 });
    pa.run(twoFiles(0));
    a.settle();
    bool all = true;
    for (int k = 1; k <= 30 && all; ++k)
    {
        pa.setDraft({ twoFiles(k), 1 });
        setDial(a, 0, 0.01f * static_cast<float>(k));
        all = pa.saveVersion("Glass Bloom", saved, why);
    }
    P.eq("setup.versions", all && pa.versions("Glass Bloom").size() == 30 ? 1 : 0, 1);
    setDial(a, 1, 2500.0f);
    P.eq("setup.presets", pa.savePreset("Bright", why) && pa.savePreset("Dark", why) ? 1 : 0, 1);

    // INFO
    pg::ProjectInfo info;
    info.description = "A pad of glass,   slow to open. " + std::string(700, 'x');
    info.author = "Probe";
    info.category = kInstrument ? "Pad" : "Reverb";
    info.tags = { " warm ", "analog", "Warm", "" };
    info.genres = { "Ambient", "Cinematic" };
    P.eq("info.kept", pa.setProjectInfo("Glass Bloom", info, why) ? 1 : 0, 1);
    const pg::ProjectInfo back = pa.projectInfo("Glass Bloom");
    std::printf("info: %zu characters, author %s, category %s, %zu tags, %zu genres\n", back.description.size(), back.author.c_str(),
                back.category.c_str(), back.tags.size(), back.genres.size());
    P.eq("info.tidied", back.description.rfind("A pad of glass, slow to open. x", 0) == 0 && back.description.size() == ps::kMaxDescription
                            && back.tags == std::vector<std::string>{ "warm", "analog" } && back.genres == info.genres && back.author == "Probe"
                            ? 1 : 0, 1);
    {
        const juce::var root = juce::JSON::parse(dir.getChildFile("projects").getChildFile("Glass Bloom.fmproject").loadFileAsString());
        P.eq("info.in_file", root["info"]["category"].toString() == juce::String(info.category) && root["info"]["tags"].size() == 2 ? 1 : 0, 1);
    }
    const std::optional<ps::Project> original = ps::load("Glass Bloom");
    P.eq("setup.loaded", original.has_value() ? 1 : 0, 1);

    // PACKAGE
    const juce::File pack = out.getChildFile("Glass Bloom.fmpack");
    pg::Exported ex;
    P.eq("package.written", pa.exportProject("Glass Bloom", true, true, pack.getFullPathName().toStdString(), ex, why) ? 1 : 0, 1);
    if (!why.empty())
        std::printf("package: %s\n", why.c_str());
    const std::vector<uint8_t> pb = bytesOf(pack);
    std::printf("package: %lld bytes of %lld (%s), %d versions\n", static_cast<long long>(ex.bytes), static_cast<long long>(ex.raw),
                ex.method.c_str(), ex.versions);
    P.eq("package.header", pb.size() > 26 && std::memcmp(pb.data(), "FMPK", 4) == 0 && pb[4] == 1 ? 1 : 0, 1);
#if defined(__APPLE__)
    P.eq("package.xz", pb.size() > 26 && pb[5] == 2 && ex.method == "xz" && pb[20] == 0xFD && pb[21] == '7' && pb[22] == 'z' && pb[23] == 'X'
                           && pb[24] == 'Z' && pb[25] == 0 ? 1 : 0, 1);
#else
    // No xz here (Apple's Compression framework writes it): zlib, its stream's first byte deflate's (0x78).
    P.eq("package.zlib", pb.size() > 26 && pb[5] == 1 && ex.method == "zlib" && pb[20] == 0x78 ? 1 : 0, 1);
#endif
    {
        uint64_t size = 0;
        for (int i = 0; i < 8 && pb.size() >= 20; ++i)
            size |= static_cast<uint64_t>(pb[static_cast<std::size_t>(8 + i)]) << (8 * i);
        uint32_t crc = 0;
        for (int i = 0; i < 4 && pb.size() >= 20; ++i)
            crc |= static_cast<uint32_t>(pb[static_cast<std::size_t>(16 + i)]) << (8 * i);
        const std::optional<std::string> json = pb.size() >= 20 ? px::decompress(pb.data() + 20, pb.size() - 20, static_cast<px::Method>(pb[5]),
                                                                                  static_cast<std::size_t>(size))
                                                                : std::nullopt;
        P.eq("package.size_and_check", json && json->size() == static_cast<std::size_t>(ex.raw) && px::crc32(json->data(), json->size()) == crc
                                           ? 1 : 0, 1);
        P.eq("package.is_the_project", json && json->find("\"funk-playground-project\"") != std::string::npos ? 1 : 0, 1);
    }
    P.le("package.compressed", static_cast<double>(ex.bytes), static_cast<double>(ex.raw) / 8.0);
    P.eq("package.versions", ex.versions, 30);

    // IMPORT
    std::string name;
    P.eq("import.package", pa.importProject(pack.getFullPathName().toStdString(), name, why) ? 1 : 0, 1);
    std::printf("imported as %s\n", name.c_str());
    P.eq("import.own_name", name == "Glass Bloom 2" ? 1 : 0, 1);
    {
        const std::optional<ps::Project> copy = ps::load(name);
        P.eq("import.same", copy && original && sameVersions(*copy, *original) && copy->presets.size() == 2 && copy->info == original->info ? 1 : 0, 1);
        pg::Processor b;
        P.eq("import.opens", b.editorPort().openProject(name, 0, why) ? 1 : 0, 1);
        b.settle();
        P.eq("import.files", b.editorPort().draft().files == twoFiles(30) && b.editorPort().draft().active == 1 ? 1 : 0, 1);
        P.near("import.dial", static_cast<double>(dial(b, 0)), 0.30, 1e-4);
    }

    // NEWEST
    {
        const juce::File newest = out.getChildFile("Newest.fmpack");
        P.eq("newest.written", pa.exportProject("Glass Bloom", true, false, newest.getFullPathName().toStdString(), ex, why) ? 1 : 0, 1);
        const std::optional<ps::Project> p = px::read(newest, &why);
        P.eq("newest.one", p && p->versions.size() == 1 && p->versions[0].number == 30 && ex.versions == 1 ? 1 : 0, 1);
    }

    // SOURCE
    {
        const juce::File zipFile = out.getChildFile("Glass Bloom.zip");
        P.eq("source.written", pa.exportProject("Glass Bloom", false, true, zipFile.getFullPathName().toStdString(), ex, why) ? 1 : 0, 1);
        std::printf("source: %lld bytes of %lld\n", static_cast<long long>(ex.bytes), static_cast<long long>(ex.raw));
        juce::ZipFile zip(zipFile);
        const auto entry = [&](const juce::String& path) -> std::string {
            const int i = zip.getIndexOfFileName(path);
            if (i < 0)
                return {};
            const std::unique_ptr<juce::InputStream> in(zip.createStreamForEntry(i));
            return in != nullptr ? in->readEntireStreamAsString().toStdString() : std::string();
        };
        const std::string readme = entry("Glass Bloom/README.md");
        const std::string manifest = entry("Glass Bloom/project.json");
        P.eq("source.readme", readme.rfind("# Glass Bloom", 0) == 0 && readme.find("A pad of glass") != std::string::npos
                                  && readme.find("warm, analog") != std::string::npos && readme.find("**Bright**") != std::string::npos
                                  ? 1 : 0, 1);
        P.eq("source.manifest", manifest.find("\"funk-playground-source\"") != std::string::npos ? 1 : 0, 1);
        P.eq("source.newest_on_top", entry("Glass Bloom/program.fm") == twoFiles(30)[0].text && entry("Glass Bloom/gain.fm") == twoFiles(30)[1].text
                                         ? 1 : 0, 1);
        P.eq("source.older_in_folders", entry("Glass Bloom/versions/v1/program.fm") == twoFiles(1)[0].text
                                            && entry("Glass Bloom/versions/v29/program.fm") == twoFiles(29)[0].text ? 1 : 0, 1);
        P.eq("source.entries", zip.getNumEntries(), 2 + 30 * 2);
        P.eq("source.import", pa.importProject(zipFile.getFullPathName().toStdString(), name, why) ? 1 : 0, 1);
        if (!why.empty())
            std::printf("source import: %s\n", why.c_str());
        const std::optional<ps::Project> copy = ps::load(name);
        P.eq("source.same", name == "Glass Bloom 3" && copy && original && sameVersions(*copy, *original) && copy->presets.size() == 2
                                && copy->info == original->info ? 1 : 0, 1);
    }

    // ZLIB
    {
        const juce::File z = out.getChildFile("Zlib.fmpack");
        ps::Project p = *original;
        p.name = "Zlib One";
        writeBytes(z, packageOf(ps::toJson(p), px::Method::zlib));
        P.eq("zlib.imports", pa.importProject(z.getFullPathName().toStdString(), name, why) && name == "Zlib One" ? 1 : 0, 1);
    }

    // REFUSE
    {
        const std::size_t before = ps::list().size();
        const auto refused = [&](const juce::File& f, const char* says) {
            why.clear();
            std::string n;
            const bool no = !pa.importProject(f.getFullPathName().toStdString(), n, why);
            std::printf("refused %s: %s\n", f.getFileName().toRawUTF8(), why.c_str());
            return no && why.find(says) != std::string::npos;
        };
        ps::Project other = *original;
        std::string json = ps::toJson(other);
        const std::string mine = std::string("\"kind\": \"") + (kInstrument ? "instrument" : "effect") + "\"";
        const std::size_t at = json.find(mine);
        if (at != std::string::npos)
            json.replace(at, mine.size(), std::string("\"kind\": \"") + (kInstrument ? "effect" : "instrument") + "\"");
        const juce::File kind = out.getChildFile("Other.fmpack");
        writeBytes(kind, packageOf(json, px::Method::stored));
        P.eq("refuse.other_kind", at != std::string::npos && refused(kind, kInstrument ? "effect" : "instrument") ? 1 : 0, 1);
        std::vector<uint8_t> bad = pb;
        bad[bad.size() / 2] ^= 0x40;
        const juce::File damaged = out.getChildFile("Damaged.fmpack");
        writeBytes(damaged, bad);
        P.eq("refuse.damaged", refused(damaged, "damaged") ? 1 : 0, 1);
        const juce::File cut = out.getChildFile("Cut.fmpack");
        writeBytes(cut, { pb.begin(), pb.begin() + 12 });
        P.eq("refuse.cut_short", refused(cut, "cut short") ? 1 : 0, 1);
        const juce::File junk = out.getChildFile("Junk.fmpack");
        junk.replaceWithText("not an export at all");
        P.eq("refuse.not_an_export", refused(junk, "neither") ? 1 : 0, 1);
        const juce::File bare = out.getChildFile("Bare.zip");
        {
            juce::ZipFile::Builder zb;
            zb.addEntry(new juce::MemoryInputStream("x", 1, true), 9, "Bare/program.fm", juce::Time::getCurrentTime());
            juce::FileOutputStream os(bare);
            zb.writeToStream(os, nullptr);
        }
        P.eq("refuse.no_manifest", refused(bare, "project.json") ? 1 : 0, 1);
#if !defined(__APPLE__)
        // A package in xz, as macOS writes it: this build reads none, and says so (its body is never looked at).
        std::vector<uint8_t> xz = packageOf(ps::toJson(*original), px::Method::stored);
        xz[5] = static_cast<uint8_t>(px::Method::xz);
        const juce::File fromMac = out.getChildFile("FromMac.fmpack");
        writeBytes(fromMac, xz);
        P.eq("refuse.xz", refused(fromMac, "xz") ? 1 : 0, 1);
#endif
        P.eq("refuse.nothing_added", ps::list().size() == before ? 1 : 0, 1);
    }

    // BUNDLE
    {
        const auto waitExport = [&](pg::Processor& proc) {
            for (int k = 0; k < 6000; ++k)
            {
                proc.poll();
                if (!proc.editorPort().exporting().busy)
                    return true;
                juce::Thread::sleep(5);
            }
            return false;
        };
        // A project with a sample, added as an import would add it.
        const std::string sampleBytes = "RIFF....WAVEfmt sample bytes for the bundle probe";
        const std::string sampleHash = pg::assets::hashOf(sampleBytes);
        {
            ps::Project s = *original;
            s.name = "Sampled";
            s.versions = { original->latest() };
            s.versions[0].samples = { { "kick.wav", sampleHash } };
            s.samples = { { sampleHash, sampleBytes } };
            P.eq("bundle.sampled_added", ps::add(s, &why).value_or("") == "Sampled" ? 1 : 0, 1);
        }
        const std::vector<ps::Entry> factory = ps::factoryList();
        const std::string factoryOne = factory.empty() ? std::string() : factory.front().name;
        const std::string factoryOwn = pg::ownName(factoryOne);
        // A project of the user's whose name is the factory one's own but for its case.
        std::string caseTwin;
        for (const char c : factoryOwn)
            caseTwin += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!factoryOne.empty())
        {
            ps::Project t = *original;
            t.name = caseTwin;
            t.versions = { original->latest() };
            t.presets.clear();
            (void)ps::add(t, &why);
        }
        std::vector<std::string> names = { "Glass Bloom", "Sampled" };
        if (!factoryOne.empty())
        {
            names.push_back(factoryOne);
            names.push_back(caseTwin);
        }
        const std::vector<std::string> files = px::bundleNames(names, true);
        P.eq("bundle.names_unique", px::bundleNames({ "Pad", "pad", "PAD" }, true)
                                        == std::vector<std::string>{ "Pad.fmpack", "pad 2.fmpack", "PAD 3.fmpack" }
                                        ? 1 : 0, 1);
        const juce::File bundle = out.getChildFile("Several.zip");
        const int parsesBefore = ps::parses();
        a.setExportGate(true);
        const bool started = pa.startExport(names, true, true, bundle.getFullPathName().toStdString(), why);
        const pg::ExportState held = pa.exporting();
        P.eq("bundle.started", started && held.busy && !held.finished && held.total == static_cast<int>(names.size()) && held.done == 0 ? 1 : 0, 1);
        P.eq("bundle.no_parse_on_message_thread", ps::parses() - parsesBefore, 0);
        std::string busyWhy;
        P.eq("bundle.one_at_a_time", pa.startExport(names, true, true, out.getChildFile("Second.zip").getFullPathName().toStdString(), busyWhy)
                                         ? 0 : 1, 1);
        a.setExportGate(false);
        P.eq("bundle.ended", waitExport(a) ? 1 : 0, 1);
        const pg::ExportState done = pa.exporting();
        std::printf("bundle: %s, %d of %d done, %d failed, %lld bytes (%s) %s\n", done.ok ? "written" : "not written", done.done, done.total,
                    done.failed, static_cast<long long>(done.out.bytes), done.out.method.c_str(), done.why.c_str());
        P.eq("bundle.written", done.ok && done.finished && done.done == static_cast<int>(names.size()) && done.failed == 0 && bundle.existsAsFile()
                                   && done.out.method == "bundle" && done.out.bytes == bundle.getSize() ? 1 : 0, 1);
        P.eq("bundle.parsed_on_its_thread", ps::parses() - parsesBefore >= 2 ? 1 : 0, 1);
        {
            juce::ZipFile zip(bundle);
            const auto entry = [&](const juce::String& path) -> std::string {
                const int i = zip.getIndexOfFileName(path);
                if (i < 0)
                    return {};
                const std::unique_ptr<juce::InputStream> in(zip.createStreamForEntry(i));
                return in != nullptr ? in->readEntireStreamAsString().toStdString() : std::string();
            };
            const juce::var m = juce::JSON::parse(juce::String(entry(px::kBundleManifest)));
            bool listed = m["projects"].size() == static_cast<int>(names.size());
            for (int k = 0; listed && k < m["projects"].size(); ++k)
                listed = m["projects"][k]["file"].toString().toStdString() == files[static_cast<std::size_t>(k)]
                      && zip.getIndexOfFileName(m["projects"][k]["file"].toString()) >= 0;
            P.eq("bundle.manifest", m["format"].toString() == px::kBundleFormat && static_cast<int>(m["version"]) == 1
                                        && m["kind"].toString() == (kInstrument ? "instrument" : "effect")
                                        && m["plugin"].toString() == pg::product::kName && listed ? 1 : 0, 1);
            P.eq("bundle.entries", zip.getNumEntries(), static_cast<int>(names.size()) + 1);
            if (!factoryOne.empty())
            {
                P.eq("bundle.factory_own_name", files[2] == factoryOwn + ".fmpack" && m["projects"][2]["name"].toString().toStdString() == factoryOwn
                                                    ? 1 : 0, 1);
                P.eq("bundle.case_unique", files[3] == caseTwin + " 2.fmpack" ? 1 : 0, 1);
            }
            else
                probe::skip(P, "bundle.factory_own_name", "this product has no factory project");
            // Unzipped by hand: a member is an ordinary package.
            const juce::File member = out.getChildFile("Member.fmpack");
            juce::MemoryBlock body;
            if (const int mi = zip.getIndexOfFileName("Glass Bloom.fmpack"); mi >= 0)
                if (const std::unique_ptr<juce::InputStream> in(zip.createStreamForEntry(mi)); in != nullptr)
                    in->readIntoMemoryBlock(body);
            member.replaceWithData(body.getData(), body.getSize());
            const std::optional<ps::Project> alone = px::read(member, &why);
            P.eq("bundle.member_alone", alone && original && sameVersions(*alone, *original) && body.getSize() > 4
                                             && std::memcmp(body.getData(), "FMPK", 4) == 0 ? 1 : 0, 1);
            P.eq("bundle.no_id", alone && alone->id.empty() ? 1 : 0, 1);
        }
        why.clear();
        P.eq("bundle.read_refuses", !px::read(bundle, &why) && why.find("bundle") != std::string::npos ? 1 : 0, 1);
        // In again: every project, each as it went.
        {
            pg::Imported in;
            const bool imported = pa.importProjects(bundle.getFullPathName().toStdString(), in, why);
            std::printf("bundle imported: %zu projects, %zu refused\n", in.names.size(), in.refused.size());
            P.eq("bundle.imported", imported && in.names.size() == names.size() && in.refused.empty() ? 1 : 0, 1);
            const std::optional<ps::Project> glass = in.names.empty() ? std::nullopt : ps::load(in.names[0]);
            P.eq("bundle.same_project", glass && original && sameVersions(*glass, *original) && glass->presets.size() == 2
                                            && glass->info == original->info && glass->createdMs == original->createdMs ? 1 : 0, 1);
            const std::optional<ps::Project> sampled = in.names.size() < 2 ? std::nullopt : ps::load(in.names[1]);
            P.eq("bundle.same_samples", sampled && sampled->samples.size() == 1 && sampled->samples[0].bytes == sampleBytes
                                            && sampled->latest().samples.size() == 1 && sampled->latest().samples[0].name == "kick.wav"
                                            && sampled->latest().samples[0].hash == sampleHash ? 1 : 0, 1);
            if (!factoryOne.empty())
                P.eq("bundle.factory_in_as_own", in.names.size() >= 3 && !pg::isFactoryName(in.names[2])
                                                     && ps::sameName(in.names[2].substr(0, factoryOwn.size()), factoryOwn) ? 1 : 0, 1);
            std::string one;
            P.eq("bundle.import_one_name", pa.importProject(bundle.getFullPathName().toStdString(), one, why) && !one.empty() ? 1 : 0, 1);
        }
        // A bundle with one of the other kind: this kind's taken, the other refused by name.
        {
            ps::Project mine = *original;
            mine.name = "Kind Mine";
            std::string other = ps::toJson(*original);
            const std::string kind = std::string("\"kind\": \"") + (kInstrument ? "instrument" : "effect") + "\"";
            if (const std::size_t at = other.find(kind); at != std::string::npos)
                other.replace(at, kind.size(), std::string("\"kind\": \"") + (kInstrument ? "effect" : "instrument") + "\"");
            const std::vector<uint8_t> a1 = packageOf(ps::toJson(mine), px::Method::stored);
            const std::vector<uint8_t> a2 = packageOf(other, px::Method::stored);
            const std::string manifest = "{\"format\": \"funk-playground-bundle\", \"version\": 1, \"projects\": ["
                                         "{\"name\": \"Kind Mine\", \"file\": \"Kind Mine.fmpack\"}, "
                                         "{\"name\": \"Kind Other\", \"file\": \"Kind Other.fmpack\"}]}";
            juce::ZipFile::Builder zb;
            zb.addEntry(new juce::MemoryInputStream(manifest.data(), manifest.size(), true), 9, "bundle.json", juce::Time::getCurrentTime());
            zb.addEntry(new juce::MemoryInputStream(a1.data(), a1.size(), true), 0, "Kind Mine.fmpack", juce::Time::getCurrentTime());
            zb.addEntry(new juce::MemoryInputStream(a2.data(), a2.size(), true), 0, "Kind Other.fmpack", juce::Time::getCurrentTime());
            const juce::File mixed = out.getChildFile("Mixed.zip");
            {
                juce::FileOutputStream os(mixed);
                zb.writeToStream(os, nullptr);
            }
            pg::Imported in;
            const bool ok = pa.importProjects(mixed.getFullPathName().toStdString(), in, why);
            std::printf("mixed: %s\n", in.refused.empty() ? "nothing refused" : in.refused.front().c_str());
            P.eq("bundle.other_kind_refused", ok && in.names == std::vector<std::string>{ "Kind Mine" } && in.refused.size() == 1
                                                  && in.refused[0].rfind("Kind Other:", 0) == 0
                                                  && in.refused[0].find(kInstrument ? "effect" : "instrument") != std::string::npos
                                                  ? 1 : 0, 1);
        }
        const auto temps = [&] {
            int n = 0;
            for (const juce::File& f : out.findChildFiles(juce::File::findFiles, false, "*.writing"))
                n += f.getFileName().startsWithChar('.') ? 1 : 0;
            return n;
        };
        // A cancel: nothing left.
        {
            const juce::File cancelled = out.getChildFile("Cancelled.zip");
            a.setExportGate(true);
            P.eq("cancel.started", pa.startExport(names, false, true, cancelled.getFullPathName().toStdString(), why) ? 1 : 0, 1);
            pa.cancelExport();
            a.setExportGate(false);
            P.eq("cancel.ended", waitExport(a) ? 1 : 0, 1);
            const pg::ExportState st = pa.exporting();
            P.eq("cancel.no_file", !st.ok && st.cancelled && !cancelled.exists() && temps() == 0 ? 1 : 0, 1);
        }
        // An exception with the temporary written (fminus/Failure.h's seam): the export fails in words, the folder
        // holds nothing new (no file, no temporary), and the next export is taken as any (one project, then a bundle).
        for (const bool single : { true, false })
        {
            const juce::File target = out.getChildFile(single ? "Thrown.fmpack" : "Thrown.zip");
            const int filesBefore = out.getNumberOfChildFiles(juce::File::findFiles | juce::File::ignoreHiddenFiles);
            pg::failure::plant("export", pg::failure::Kind::badAlloc);
            const std::vector<std::string> which = single ? std::vector<std::string>{ "Glass Bloom" } : names;
            const bool go = pa.startExport(which, true, true, target.getFullPathName().toStdString(), why);
            const bool ended = waitExport(a);
            const int fired = pg::failure::fired();
            pg::failure::unplant();
            const pg::ExportState st = pa.exporting();
            std::printf("thrown (%s): fired %d, %s\n", single ? "one" : "bundle", fired, st.why.c_str());
            P.eq(single ? "thrown.single_in_words" : "thrown.bundle_in_words",
                 go && ended && fired == 1 && !st.ok && !st.cancelled
                     && st.why == "the plugin itself failed while exporting: out of memory; this is not a fault in your program" ? 1 : 0, 1);
            P.eq(single ? "thrown.single_nothing_left" : "thrown.bundle_nothing_left",
                 !target.exists() && temps() == 0
                     && out.getNumberOfChildFiles(juce::File::findFiles | juce::File::ignoreHiddenFiles) == filesBefore ? 1 : 0, 1);
            const bool again = pa.startExport(which, true, true, target.getFullPathName().toStdString(), why) && waitExport(a);
            P.eq(single ? "thrown.single_next" : "thrown.bundle_next", again && pa.exporting().ok && target.existsAsFile() ? 1 : 0, 1);
            target.deleteFile();
        }
        // A file made at the target meanwhile: never replaced (a bundle, then one project).
        for (const bool single : { false, true })
        {
            const juce::File target = out.getChildFile(single ? "Taken.fmpack" : "Taken.zip");
            target.deleteFile();
            a.setExportGate(true);
            const std::vector<std::string> which = single ? std::vector<std::string>{ "Glass Bloom" } : names;
            const bool go = pa.startExport(which, true, true, target.getFullPathName().toStdString(), why);
            target.replaceWithText("the user's own file");
            a.setExportGate(false);
            (void)waitExport(a);
            const pg::ExportState st = pa.exporting();
            std::printf("taken (%s): %s\n", single ? "one" : "bundle", st.why.c_str());
            P.eq(single ? "replace.single_kept" : "replace.bundle_kept",
                 go && !st.ok && !st.cancelled && target.loadFileAsString() == "the user's own file" && temps() == 0
                     && st.why.find("there already") != std::string::npos ? 1 : 0, 1);
        }
        // One project through the job: the bytes exportProject wrote.
        {
            const juce::File one = out.getChildFile("One.fmpack");
            P.eq("single.started", pa.startExport({ "Glass Bloom" }, true, true, one.getFullPathName().toStdString(), why) ? 1 : 0, 1);
            P.eq("single.ended", waitExport(a) ? 1 : 0, 1);
            const pg::ExportState st = pa.exporting();
            P.eq("single.same_bytes", st.ok && st.done == 1 && st.out.versions == 30 && bytesOf(one) == pb ? 1 : 0, 1);
            // A file there already: refused at once, for one project and for several, the file as it was.
            const std::vector<uint8_t> was = bytesOf(one);
            std::string w;
            P.eq("replace.single_refused_at_once", !pa.startExport({ "Glass Bloom" }, true, false, one.getFullPathName().toStdString(), w)
                                                       && w.find("there already") != std::string::npos && !pa.exporting().busy
                                                       && bytesOf(one) == was ? 1 : 0, 1);
            P.eq("replace.bundle_refused_at_once", !pa.startExport(names, false, true, bundle.getFullPathName().toStdString(), w)
                                                       && w.find("there already") != std::string::npos && !pa.exporting().busy
                                                       && bundle.existsAsFile() ? 1 : 0, 1);
        }
        // The limits.
        {
            std::vector<std::string> many(static_cast<std::size_t>(px::kMaxBundleProjects) + 1, "Glass Bloom");
            std::string w;
            P.eq("limit.count", !pa.startExport(many, true, true, out.getChildFile("Many.zip").getFullPathName().toStdString(), w)
                                    && w.find("256") != std::string::npos ? 1 : 0, 1);
            P.eq("limit.full_path", pa.startExport({ "Glass Bloom" }, true, true, "relative.zip", w) ? 0 : 1, 1);
            const juce::File huge = dir.getChildFile("projects").getChildFile("Huge.fmproject");
            std::error_code ec;
            {
                juce::FileOutputStream h(huge);
            }
            std::filesystem::resize_file(std::filesystem::path(huge.getFullPathName().toStdString()),
                                         static_cast<std::uintmax_t>(px::kMaxBundleBytes) + 1, ec);
            const juce::File big = out.getChildFile("Big.zip");
            const bool go = !ec && pa.startExport({ "Huge", "Glass Bloom" }, true, true, big.getFullPathName().toStdString(), w);
            (void)waitExport(a);
            const pg::ExportState st = pa.exporting();
            std::printf("too large: %s\n", st.why.c_str());
            P.eq("limit.bytes", go && !st.ok && st.why.find("GB") != std::string::npos && st.done == 0 && !big.exists() ? 1 : 0, 1);
            huge.deleteFile();
        }
        // A processor that goes with an export held: cancelled and waited for, no file.
        {
            const juce::File gone = out.getChildFile("Gone.zip");
            {
                pg::Processor c;
                c.setExportGate(true);
                P.eq("goes.started", c.editorPort().startExport(names, true, true, gone.getFullPathName().toStdString(), why) ? 1 : 0, 1);
            }
            P.eq("goes.no_file", !gone.exists() && temps() == 0 ? 1 : 0, 1);
        }
    }

    ps::setDirectoryForTests({});
    dir.deleteRecursively();
    return P.finish();
}
