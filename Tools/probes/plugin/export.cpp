// PROBE name=plugin.export timeout=180
//
// plugin.export: a project's info and its two exports (plugin/ProjectExport.h) through the processor's EditorPort, in a
// folder of the probe's own:
//   INFO     setProjectInfo keeps a description, an author, a category, tags and genres, tidied (the tags split at
//            commas, each once case aside, a long description cut), and the project file holds them
//   PACKAGE  a project of 30 versions exported as a .fmpack: "FMPK", format 1, xz (on macOS), the header's size and
//            CRC-32 the JSON's, the body an .xz stream (its magic), far smaller than its text
//   IMPORT   the package imported: a project of its own ("<name> 2", the name being taken), its versions, presets and
//            info the same; opened, the newest version's files and dials
//   NEWEST   exported with the newest version only: one version, numbered as it was
//   SOURCE   a .zip: <Name>/README.md (the description, the tags, the presets), <Name>/project.json (the source
//            format), the newest version's files at the top and each older one's under versions/v<N>/; imported, the
//            same versions and files
//   ZLIB     a package whose body is zlib (as a build without xz writes) imports
//   REFUSE   a package of the other kind (the why names it), one with a byte changed (its check), one cut short, a file
//            that is no export, a zip with no project.json: refused, nothing added
#include "Probe.h"

#include "plugin/EditorPort.h"
#include "plugin/ProjectExport.h"
#include "plugin/ProjectStore.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace tp = testingplayground;

namespace
{
    constexpr bool kInstrument = tp::product::kIsInstrument;

    tp::Sources twoFiles(int edit)
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
        return { { tp::kProgramName, program + "// edit " + std::to_string(edit) + "\n" },
                 { "gain.fm", "float half(float x) { return 0.5f * x; }\n" } };
    }

    float dial(tp::Processor& p, int slot) { return tp::plainOf(p.control(slot), p.getParameters()[slot]->getValue()); }

    void setDial(tp::Processor& p, int slot, float plain)
    {
        p.getParameters()[slot]->setValueNotifyingHost(tp::normOf(p.control(slot), plain));
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
    std::vector<uint8_t> packageOf(const std::string& json, tp::projectexport::Method method)
    {
        std::vector<uint8_t> body;
        if (method == tp::projectexport::Method::zlib)
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
        const uint32_t crc = tp::projectexport::crc32(json.data(), json.size());
        for (int i = 0; i < 4; ++i)
            out[static_cast<std::size_t>(16 + i)] = static_cast<uint8_t>(crc >> (8 * i));
        out.insert(out.end(), body.begin(), body.end());
        return out;
    }

    bool sameVersions(const tp::projectstore::Project& a, const tp::projectstore::Project& b)
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
    namespace ps = tp::projectstore;
    namespace px = tp::projectexport;
    const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("export-probe-" + juce::String(juce::Time::currentTimeMillis()));
    dir.deleteRecursively();
    const juce::File out = dir.getChildFile("out");
    ps::setDirectoryForTests(dir.getChildFile("projects"));

    // A project of 30 versions, each a line more, and two presets.
    tp::Processor a;
    tp::EditorPort& pa = a.editorPort();
    std::string why;
    tp::SavedVersion saved;
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
    tp::ProjectInfo info;
    info.description = "A pad of glass,   slow to open. " + std::string(700, 'x');
    info.author = "Probe";
    info.category = kInstrument ? "Pad" : "Reverb";
    info.tags = { " warm ", "analog", "Warm", "" };
    info.genres = { "Ambient", "Cinematic" };
    P.eq("info.kept", pa.setProjectInfo("Glass Bloom", info, why) ? 1 : 0, 1);
    const tp::ProjectInfo back = pa.projectInfo("Glass Bloom");
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
    tp::Exported ex;
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
        tp::Processor b;
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
        P.eq("refuse.nothing_added", ps::list().size() == before ? 1 : 0, 1);
    }

    ps::setDirectoryForTests({});
    dir.deleteRecursively();
    return P.finish();
}
