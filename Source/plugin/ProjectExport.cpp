// Source/plugin/ProjectExport.cpp: see ProjectExport.h.
#include "plugin/ProjectExport.h"

#include "Product.h"
#include "plugin/EditorPort.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>

#if defined(__APPLE__)
#include <compression.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace playground::projectexport
{
    namespace
    {
        constexpr const char* kSourceFormat = "funk-playground-source";
        constexpr const char* kProjectFormat = "funk-playground-project";
        constexpr int kSourceVersion = 1;
        constexpr std::size_t kMaxRaw = 256u * 1024u * 1024u;   // a package that says it holds more is no package
        constexpr juce::int64 kMaxFile = 128 * 1024 * 1024;   // its samples' files (ProjectStore.h) as they are

        constexpr std::array<uint32_t, 256> crcTable()
        {
            std::array<uint32_t, 256> t{};
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1u) != 0u ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
            return t;
        }
        constexpr std::array<uint32_t, 256> kCrc = crcTable();

        juce::String str(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

        void fail(std::string* why, const std::string& what)
        {
            if (why != nullptr)
                *why = what;
        }

        std::string kindName() { return product::kIsInstrument ? "instrument" : "effect"; }
        std::string when(int64_t ms) { return juce::Time(static_cast<juce::int64>(ms)).formatted("%Y-%m-%d %H:%M").toStdString(); }

        std::string joined(const std::vector<std::string>& v)
        {
            std::string out;
            for (const std::string& s : v)
                out += (out.empty() ? "" : ", ") + s;
            return out;
        }

        std::string number(float v)
        {
            juce::String s(static_cast<double>(v), 4);
            while (s.containsChar('.') && (s.endsWithChar('0') || s.endsWithChar('.')))
                s = s.dropLastCharacters(1);
            return s.toStdString();
        }

        // Written beside, then moved over (place: over a file there only when `replace`): the file is never half
        // written.
        bool writeFile(const juce::File& to, const void* data, std::size_t size, std::string* why, bool replace = true)
        {
            const juce::File dir = to.getParentDirectory();
            if (!dir.isDirectory() && !dir.createDirectory())
            {
                fail(why, "cannot make the folder " + dir.getFullPathName().toStdString());
                return false;
            }
            const juce::File temp = dir.getChildFile("." + to.getFileName() + ".writing");
            {
                juce::FileOutputStream out(temp);
                if (!out.openedOk() || !out.setPosition(0) || !out.truncate().wasOk() || !out.write(data, size))
                {
                    temp.deleteFile();
                    fail(why, "cannot write " + to.getFullPathName().toStdString());
                    return false;
                }
                out.flush();
            }
            if (!place(temp, to, replace, why))
            {
                temp.deleteFile();
                return false;
            }
            return true;
        }

        std::string readme(const projectstore::Project& p, int versions)
        {
            const projectstore::Version& v = p.latest();
            const projectstore::Info& in = p.info;
            std::string s = "# " + p.name + "\n\n" + (in.description.empty() ? std::string("No description yet.") : in.description) + "\n\n";
            s += "| | |\n|---|---|\n";
            s += "| Plugin | " + std::string(product::kName) + " (an " + kindName() + ") |\n";
            if (!in.category.empty())
                s += "| Category | " + in.category + " |\n";
            if (!in.genres.empty())
                s += "| Genres | " + joined(in.genres) + " |\n";
            if (!in.tags.empty())
                s += "| Tags | " + joined(in.tags) + " |\n";
            if (!in.author.empty())
                s += "| Author | " + in.author + " |\n";
            s += "| Version | v" + std::to_string(v.number) + ", saved " + when(v.savedMs) + " (" + std::to_string(versions)
               + (versions == 1 ? " version" : " versions") + " in this zip) |\n";
            s += "| Exported | " + when(juce::Time::currentTimeMillis()) + " |\n\n";
            s += "## Files\n\nFMinus source, the program first (the folder's own copies are the newest version's):\n\n";
            for (const SourceFile& f : v.files)
                s += "- `" + f.name + "`\n";
            if (!v.samples.empty())
            {
                s += "\n## Samples\n\nThe audio files the program reads (`#pragma sample`), in `samples/`:\n\n";
                for (const projectstore::SampleRef& r : v.samples)
                    s += "- `" + r.name + "`\n";
            }
            if (!p.presets.empty())
            {
                s += "\n## Presets\n\nDial settings for the program:\n\n";
                for (const projectstore::Preset& pr : p.presets)
                {
                    std::string dials;
                    for (const projectstore::Dial& d : pr.dials)
                        dials += (dials.empty() ? "" : ", ") + d.name + " " + number(d.value);
                    s += "- **" + pr.name + "**" + (pr.from > 0 ? " (from v" + std::to_string(pr.from) + ")" : std::string()) + ": " + dials + "\n";
                }
            }
            if (versions > 1)
            {
                s += "\n## Versions\n\nThe newest first; each older one's files are in `versions/v<N>/`:\n\n";
                for (auto it = p.versions.rbegin(); it != p.versions.rend(); ++it)
                    s += "- v" + std::to_string(it->number) + ", " + when(it->savedMs) + ": " + it->note + "\n";
            }
            s += "\n## Open it again\n\nIn " + std::string(product::kName) + ": PROJECTS, Import..., and choose this .zip. `project.json` "
                 "keeps the dials, the presets and every version's place.\n";
            s += "\nThe project's chats with Claude are not in this zip: they stay on the computer they were had on.\n";
            return s;
        }
    }

    uint32_t crc32(const void* data, std::size_t size)
    {
        uint32_t c = 0xFFFFFFFFu;
        const auto* b = static_cast<const uint8_t*>(data);
        for (std::size_t i = 0; i < size; ++i)
            c = kCrc[(c ^ b[i]) & 0xFFu] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    std::vector<uint8_t> compress(const std::string& raw, Method& method)
    {
        method = Method::stored;
        if (raw.empty())
            return {};
#if defined(__APPLE__)
        {
            std::vector<uint8_t> out(raw.size() + raw.size() / 8 + 4096);
            const std::size_t n = compression_encode_buffer(out.data(), out.size(), reinterpret_cast<const uint8_t*>(raw.data()),
                                                            raw.size(), nullptr, COMPRESSION_LZMA);
            if (n > 0)
            {
                out.resize(n);
                method = Method::xz;
                return out;
            }
        }
#endif
        juce::MemoryOutputStream mem;
        {
            juce::GZIPCompressorOutputStream z(mem, 9);
            z.write(raw.data(), raw.size());
            z.flush();
        }
        method = Method::zlib;
        const auto* d = static_cast<const uint8_t*>(mem.getData());
        return { d, d + mem.getDataSize() };
    }

    std::optional<std::string> decompress(const uint8_t* data, std::size_t size, Method method, std::size_t rawSize, std::string* why)
    {
        if (rawSize > kMaxRaw)
        {
            fail(why, "it says it holds more than a project can");
            return std::nullopt;
        }
        std::string out;
        switch (method)
        {
            case Method::stored:
                out.assign(reinterpret_cast<const char*>(data), size);
                break;
            case Method::zlib:
            {
                juce::MemoryInputStream in(data, size, false);
                juce::GZIPDecompressorInputStream z(&in, false, juce::GZIPDecompressorInputStream::zlibFormat);
                juce::MemoryOutputStream mem;
                mem.writeFromInputStream(z, static_cast<juce::int64>(rawSize) + 1);
                out.assign(static_cast<const char*>(mem.getData()), mem.getDataSize());
                break;
            }
            case Method::xz:
#if defined(__APPLE__)
                out.assign(rawSize, '\0');
                if (rawSize > 0
                    && compression_decode_buffer(reinterpret_cast<uint8_t*>(out.data()), rawSize, data, size, nullptr, COMPRESSION_LZMA)
                           != rawSize)
                    out.clear();
                break;
#else
                fail(why, "it is compressed with xz, which this build does not read");
                return std::nullopt;
#endif
        }
        if (out.size() != rawSize)
        {
            fail(why, "it is damaged (its contents are not the size its header says)");
            return std::nullopt;
        }
        return out;
    }

    bool packageBytes(const projectstore::Project& p, bool allVersions, std::vector<uint8_t>& file, Result& out, std::string*)
    {
        projectstore::Project q = p;
        q.id.clear();                                    // this Mac's: its chats stay here (ProjectExport.h)
        if (!allVersions)
        {
            q.versions = { p.latest() };
            std::vector<projectstore::SampleFile> going;   // only the sample files the version that goes names
            for (const projectstore::SampleRef& r : p.latest().samples)
                if (const projectstore::SampleFile* f = p.sample(r.hash);
                    f != nullptr && std::none_of(going.begin(), going.end(), [&](const auto& g) { return g.hash == r.hash; }))
                    going.push_back(*f);
            q.samples = std::move(going);
        }
        const std::string json = projectstore::toJson(q);
        Method method = Method::stored;
        const std::vector<uint8_t> body = compress(json, method);
        file.assign(kHeaderBytes + body.size(), 0);
        std::memcpy(file.data(), "FMPK", 4);
        file[4] = static_cast<uint8_t>(kPackageFormat);
        file[5] = static_cast<uint8_t>(method);
        const uint64_t size = json.size();
        for (int i = 0; i < 8; ++i)
            file[static_cast<std::size_t>(8 + i)] = static_cast<uint8_t>(size >> (8 * i));
        const uint32_t crc = crc32(json.data(), json.size());
        for (int i = 0; i < 4; ++i)
            file[static_cast<std::size_t>(16 + i)] = static_cast<uint8_t>(crc >> (8 * i));
        std::copy(body.begin(), body.end(), file.begin() + kHeaderBytes);
        out = { static_cast<int64_t>(file.size()), static_cast<int64_t>(json.size()), method, static_cast<int>(q.versions.size()) };
        return true;
    }

    bool writePackage(const projectstore::Project& p, bool allVersions, const juce::File& to, Result& out, std::string* why,
                      bool replace)
    {
        std::vector<uint8_t> file;
        Result r;
        if (!packageBytes(p, allVersions, file, r, why) || !writeFile(to, file.data(), file.size(), why, replace))
            return false;
        out = r;
        return true;
    }

    bool sourceBytes(const projectstore::Project& p, bool allVersions, std::vector<uint8_t>& bytes, Result& out, std::string* why)
    {
        const std::string root = p.name;
        juce::ZipFile::Builder zip;
        int64_t raw = 0;
        const auto add = [&](const std::string& path, const std::string& text, int64_t ms, int level = 9) {
            raw += static_cast<int64_t>(text.size());
            zip.addEntry(new juce::MemoryInputStream(text.data(), text.size(), true), level, str(root + "/" + path),
                         juce::Time(static_cast<juce::int64>(ms)));
        };
        // The manifest: the project's JSON, each version's files by name only, and where they are; never its id.
        projectstore::Project q = p;
        q.id.clear();
        if (!allVersions)
            q.versions = { p.latest() };
        // Its sample files (ProjectStore.h), each once in samples/ under the name its newest version naming it gives it
        // (a name two files would have: the older's hash before it), and only those the versions that go name.
        std::vector<std::pair<std::string, std::string>> sampleFiles;   // hash, path
        for (auto v = q.versions.rbegin(); v != q.versions.rend(); ++v)
            for (const projectstore::SampleRef& r : v->samples)
                if (q.sample(r.hash) != nullptr
                    && std::none_of(sampleFiles.begin(), sampleFiles.end(), [&](const auto& e) { return e.first == r.hash; }))
                {
                    std::string path = "samples/" + r.name;
                    if (std::any_of(sampleFiles.begin(), sampleFiles.end(), [&](const auto& e) { return e.second == path; }))
                        path = "samples/" + r.hash + "-" + r.name;
                    sampleFiles.emplace_back(r.hash, path);
                }
        {
            std::vector<projectstore::SampleFile> going;
            for (const auto& [hash, path] : sampleFiles)
                going.push_back(*q.sample(hash));
            q.samples = std::move(going);
        }
        juce::var manifest = juce::JSON::parse(str(projectstore::toJson(q)));
        juce::DynamicObject* m = manifest.getDynamicObject();
        if (m == nullptr)
        {
            fail(why, "cannot describe the project");
            return false;
        }
        m->setProperty("format", kSourceFormat);
        m->setProperty("version", kSourceVersion);
        m->setProperty("exported", static_cast<juce::int64>(juce::Time::currentTimeMillis()));
        const int newest = p.latest().number;
        if (juce::Array<juce::var>* versions = manifest["versions"].getArray())
            for (juce::var& v : *versions)
            {
                const int n = static_cast<int>(v["number"]);
                const std::string folder = n == newest ? std::string() : "versions/v" + std::to_string(n);
                v.getDynamicObject()->setProperty("folder", str(folder));
                if (juce::Array<juce::var>* files = v["files"].getArray())
                    for (juce::var& f : *files)
                        f.getDynamicObject()->removeProperty("text");
            }
        if (juce::Array<juce::var>* files = manifest["sampleFiles"].getArray())
            for (juce::var& f : *files)
            {
                f.getDynamicObject()->removeProperty("data");
                for (const auto& [hash, path] : sampleFiles)
                    if (f["hash"].toString().toStdString() == hash)
                        f.getDynamicObject()->setProperty("file", str(path));
            }
        add("README.md", readme(q, static_cast<int>(q.versions.size())), juce::Time::currentTimeMillis());
        add("project.json", (juce::JSON::toString(manifest, false) + "\n").toStdString(), juce::Time::currentTimeMillis());
        for (const projectstore::Version& v : q.versions)
        {
            const std::string folder = v.number == newest ? std::string() : "versions/v" + std::to_string(v.number) + "/";
            for (const SourceFile& f : v.files)
                add(folder + f.name, f.text, v.savedMs);
        }
        for (const auto& [hash, path] : sampleFiles)
            add(path, q.sample(hash)->bytes, p.latest().savedMs, 0);   // audio: stored, as deflate gains it little
        juce::MemoryOutputStream mem;
        if (!zip.writeToStream(mem, nullptr))
        {
            fail(why, "cannot make the zip");
            return false;
        }
        const auto* d = static_cast<const uint8_t*>(mem.getData());
        bytes.assign(d, d + mem.getDataSize());
        out = { static_cast<int64_t>(mem.getDataSize()), raw, Method::zlib, static_cast<int>(q.versions.size()) };
        return true;
    }

    bool writeSource(const projectstore::Project& p, bool allVersions, const juce::File& to, Result& out, std::string* why,
                     bool replace)
    {
        std::vector<uint8_t> bytes;
        Result r;
        if (!sourceBytes(p, allVersions, bytes, r, why) || !writeFile(to, bytes.data(), bytes.size(), why, replace))
            return false;
        out = r;
        return true;
    }

    namespace
    {
        // One export's bytes (a .fmpack's or a source .zip's), `label` its file's name and `stem` the name it falls
        // back on.
        std::optional<projectstore::Project> readBytes(const juce::MemoryBlock& bytes, const std::string& label, const std::string& stem,
                                                       std::string* why)
        {
            if (bytes.getSize() < 4)
            {
                fail(why, "cannot read " + label);
                return std::nullopt;
            }
            const auto* b = static_cast<const uint8_t*>(bytes.getData());
            const auto named = [&](const std::string& json) -> std::optional<projectstore::Project> {
                const std::string name = juce::JSON::parse(str(json))["name"].toString().toStdString();
                return projectstore::fromJson(json, name.empty() ? stem : name, why);
            };
            if (std::memcmp(b, "FMPK", 4) == 0)
            {
                if (bytes.getSize() < static_cast<std::size_t>(kHeaderBytes))
                {
                    fail(why, label + " is damaged (its header is cut short)");
                    return std::nullopt;
                }
                if (b[4] > kPackageFormat)
                {
                    fail(why, label + " is from a newer version of the plugin");
                    return std::nullopt;
                }
                if (b[5] > static_cast<uint8_t>(Method::xz))
                {
                    fail(why, label + " is compressed in a way this plugin does not know");
                    return std::nullopt;
                }
                uint64_t size = 0;
                for (int i = 0; i < 8; ++i)
                    size |= static_cast<uint64_t>(b[8 + i]) << (8 * i);
                uint32_t crc = 0;
                for (int i = 0; i < 4; ++i)
                    crc |= static_cast<uint32_t>(b[16 + i]) << (8 * i);
                std::string what;
                const std::optional<std::string> json = decompress(b + kHeaderBytes, bytes.getSize() - kHeaderBytes, static_cast<Method>(b[5]),
                                                                   static_cast<std::size_t>(std::min<uint64_t>(size, kMaxRaw + 1)), &what);
                if (!json)
                {
                    fail(why, label + ": " + what);
                    return std::nullopt;
                }
                if (crc32(json->data(), json->size()) != crc)
                {
                    fail(why, label + " is damaged (its check does not match)");
                    return std::nullopt;
                }
                return named(*json);
            }
            if (b[0] != 'P' || b[1] != 'K')
            {
                fail(why, label + " is neither a package (.fmpack) nor a zip");
                return std::nullopt;
            }
            // A zip: the manifest in its folder, each version's files beside it.
            juce::MemoryInputStream zipIn(bytes, false);
            juce::ZipFile zip(zipIn);
            int at = -1;
            juce::String folder;
            for (int i = 0; i < zip.getNumEntries(); ++i)
            {
                const juce::String n = zip.getEntry(i)->filename;
                if (n.endsWith("/project.json") && n.upToLastOccurrenceOf("/", false, false).indexOfChar('/') < 0)
                {
                    at = i;
                    folder = n.upToLastOccurrenceOf("/", true, false);
                    break;
                }
            }
            if (at < 0)
            {
                fail(why, label + " is not a project's zip (it has no project.json)");
                return std::nullopt;
            }
            const auto text = [&](const juce::String& path) -> std::optional<std::string> {
                const int i = zip.getIndexOfFileName(path);
                if (i < 0)
                    return std::nullopt;
                const std::unique_ptr<juce::InputStream> in(zip.createStreamForEntry(i));
                if (in == nullptr)
                    return std::nullopt;
                return in->readEntireStreamAsString().toStdString();
            };
            const std::optional<std::string> manifestText = text(zip.getEntry(at)->filename);
            juce::var manifest = juce::JSON::parse(str(manifestText.value_or(std::string())));
            if (!manifest.isObject() || manifest["format"].toString() != kSourceFormat)
            {
                fail(why, label + "'s project.json is not a project's (or it is damaged)");
                return std::nullopt;
            }
            if (static_cast<int>(manifest["version"]) > kSourceVersion)
            {
                fail(why, label + " is from a newer version of the plugin");
                return std::nullopt;
            }
            if (juce::Array<juce::var>* versions = manifest["versions"].getArray())
                for (juce::var& v : *versions)
                {
                    juce::String place = v["folder"].toString();
                    if (place.isNotEmpty() && !place.endsWithChar('/'))
                        place += "/";
                    if (juce::Array<juce::var>* files = v["files"].getArray())
                        for (juce::var& f : *files)
                        {
                            const std::optional<std::string> t = text(folder + place + f["name"].toString());
                            if (!t)
                            {
                                fail(why, label + " lacks " + (folder + place + f["name"].toString()).toStdString());
                                return std::nullopt;
                            }
                            f.getDynamicObject()->setProperty("text", str(*t));
                        }
                }
            // Its sample files, each from the file the manifest names (one it lacks, or damaged, is left out: the versions
            // naming it then say so when opened).
            if (juce::Array<juce::var>* files = manifest["sampleFiles"].getArray())
                for (juce::var& f : *files)
                {
                    const int i = zip.getIndexOfFileName(folder + f["file"].toString());
                    const std::unique_ptr<juce::InputStream> in(i >= 0 ? zip.createStreamForEntry(i) : nullptr);
                    juce::MemoryBlock data;
                    if (in != nullptr)
                        in->readIntoMemoryBlock(data);
                    f.getDynamicObject()->setProperty("data", juce::Base64::toBase64(data.getData(), data.getSize()));
                    f.getDynamicObject()->removeProperty("file");
                }
            manifest.getDynamicObject()->setProperty("format", kProjectFormat);
            manifest.getDynamicObject()->setProperty("version", 1);
            return named((juce::JSON::toString(manifest, false) + "\n").toStdString());
        }
    }

    std::optional<projectstore::Project> read(const juce::File& from, std::string* why)
    {
        const std::string label = from.getFileName().toStdString();
        if (!from.existsAsFile())
        {
            fail(why, "there is no file " + from.getFullPathName().toStdString());
            return std::nullopt;
        }
        if (isBundle(from))
        {
            fail(why, label + " is a bundle of several projects: import it whole (each comes in as a project)");
            return std::nullopt;
        }
        if (from.getSize() > kMaxFile)
        {
            fail(why, label + " is too large to be a project");
            return std::nullopt;
        }
        juce::MemoryBlock bytes;
        if (!from.loadFileAsData(bytes))
        {
            fail(why, "cannot read " + label);
            return std::nullopt;
        }
        return readBytes(bytes, label, from.getFileNameWithoutExtension().toStdString(), why);
    }

    bool place(const juce::File& temp, const juce::File& to, bool replace, std::string* why)
    {
        const std::string target = to.getFullPathName().toStdString();
        if (replace)
        {
            if (temp.moveFileTo(to))
                return true;
            fail(why, "cannot replace " + target);
            return false;
        }
        const auto taken = [&] {
            fail(why, target + " is there already (made meanwhile): it is left as it is");
            return false;
        };
#if defined(_WIN32)
        // Without MOVEFILE_REPLACE_EXISTING a file there fails the move.
        if (MoveFileExW(temp.getFullPathName().toWideCharPointer(), to.getFullPathName().toWideCharPointer(), 0) != 0)
            return true;
        if (GetLastError() == ERROR_ALREADY_EXISTS || GetLastError() == ERROR_FILE_EXISTS)
            return taken();
        fail(why, "cannot write " + target);
        return false;
#else
        const char* from = temp.getFullPathName().toRawUTF8();
        const std::string toPath = target;
        // One step that refuses a file there: renameat2's RENAME_NOREPLACE (Linux), renamex_np's RENAME_EXCL (macOS);
        // where the file system takes neither, a hard link (which refuses one too), then the temporary unlinked.
#if defined(__APPLE__)
        if (renamex_np(from, toPath.c_str(), RENAME_EXCL) == 0)
            return true;
#elif defined(RENAME_NOREPLACE)
        if (renameat2(AT_FDCWD, from, AT_FDCWD, toPath.c_str(), RENAME_NOREPLACE) == 0)
            return true;
#else
        errno = EINVAL;
#endif
        if (errno == EEXIST)
            return taken();
        if (::link(from, toPath.c_str()) == 0)
        {
            ::unlink(from);
            return true;
        }
        if (errno == EEXIST)
            return taken();
        // A file system with no hard links either (FAT): looked at, then moved; the one way a file made in between is
        // replaced.
        if (to.exists())
            return taken();
        if (std::rename(from, toPath.c_str()) == 0)
            return true;
        fail(why, "cannot write " + target);
        return false;
#endif
    }

    std::vector<std::string> bundleNames(const std::vector<std::string>& names, bool package)
    {
        std::vector<std::string> out;
        const std::string ext = package ? kPackageExtension : kSourceExtension;
        for (const std::string& n : names)
        {
            std::string base = projectstore::tidyName(ownName(n));
            if (base.empty())
                base = "Project";
            std::string file = base + ext;
            for (int k = 2; std::any_of(out.begin(), out.end(), [&](const std::string& o) { return projectstore::sameName(o, file); })
                            || projectstore::sameName(file, kBundleManifest);
                 ++k)
                file = base + " " + std::to_string(k) + ext;
            out.push_back(file);
        }
        return out;
    }

    namespace
    {
        // A zip of stored entries (each export is compressed already), written as it goes: each entry's local header
        // and bytes, then the central directory. Names in UTF-8 (bit 11). At most 4 GB in all (no zip64): a bundle is
        // kMaxBundleBytes of project files at most, and an export is no larger than its project's file but for a
        // source zip's README.
        class StoredZip
        {
        public:
            explicit StoredZip(juce::OutputStream& out) : out_(out)
            {
                const juce::Time t = juce::Time::getCurrentTime();
                time_ = static_cast<uint16_t>((t.getHours() << 11) | (t.getMinutes() << 5) | (t.getSeconds() / 2));
                date_ = static_cast<uint16_t>(((std::max(1980, t.getYear()) - 1980) << 9) | ((t.getMonth() + 1) << 5) | t.getDayOfMonth());
            }

            bool add(const std::string& name, const uint8_t* data, std::size_t size, std::string* why)
            {
                const uint64_t at = static_cast<uint64_t>(out_.getPosition());
                if (at + size + name.size() + 64 > 0xFFFFFFFFull || size > 0xFFFFFFFFull)
                {
                    fail(why, "the bundle would be larger than 4 GB");
                    return false;
                }
                Entry e{ name, crc32(data, size), static_cast<uint32_t>(size), static_cast<uint32_t>(at) };
                bool ok = u32(0x04034b50) && u16(20) && u16(0x0800) && u16(0) && u16(time_) && u16(date_) && u32(e.crc)
                          && u32(e.size) && u32(e.size) && u16(static_cast<uint16_t>(name.size())) && u16(0)
                          && out_.write(name.data(), name.size()) && (size == 0 || out_.write(data, size));
                entries_.push_back(std::move(e));
                if (!ok)
                    fail(why, "cannot write the bundle");
                return ok;
            }

            bool finish(std::string* why)
            {
                const uint64_t start = static_cast<uint64_t>(out_.getPosition());
                bool ok = true;
                for (const Entry& e : entries_)
                    ok = ok && u32(0x02014b50) && u16(20) && u16(20) && u16(0x0800) && u16(0) && u16(time_) && u16(date_)
                         && u32(e.crc) && u32(e.size) && u32(e.size) && u16(static_cast<uint16_t>(e.name.size())) && u16(0)
                         && u16(0) && u16(0) && u16(0) && u32(0) && u32(e.offset) && out_.write(e.name.data(), e.name.size());
                const uint64_t end = static_cast<uint64_t>(out_.getPosition());
                if (end > 0xFFFFFFFFull)
                    ok = false;
                ok = ok && u32(0x06054b50) && u16(0) && u16(0) && u16(static_cast<uint16_t>(entries_.size()))
                     && u16(static_cast<uint16_t>(entries_.size())) && u32(static_cast<uint32_t>(end - start))
                     && u32(static_cast<uint32_t>(start)) && u16(0);
                out_.flush();
                if (!ok)
                    fail(why, "cannot write the bundle");
                return ok;
            }

        private:
            struct Entry
            {
                std::string name;
                uint32_t crc = 0, size = 0, offset = 0;
            };
            bool u16(uint16_t v) { return out_.writeShort(static_cast<short>(v)); }
            bool u32(uint32_t v) { return out_.writeInt(static_cast<int>(v)); }

            juce::OutputStream& out_;
            std::vector<Entry> entries_;
            uint16_t time_ = 0, date_ = 0;
        };
    }

    bool writeBundle(const std::vector<std::string>& names, const Loader& load, bool package, bool allVersions, const juce::File& to,
                     bool replace, const BundleProgress& progress, BundleResult& out, std::string* why)
    {
        out = {};
        const auto cancelled = [&] { return progress.cancelled && progress.cancelled(); };
        if (names.empty())
        {
            fail(why, "there is no project to export");
            return false;
        }
        if (static_cast<int>(names.size()) > kMaxBundleProjects)
        {
            fail(why, std::to_string(names.size()) + " projects are more than one bundle takes (" + std::to_string(kMaxBundleProjects) + ")");
            return false;
        }
        const juce::File dir = to.getParentDirectory();
        if (!dir.isDirectory() && !dir.createDirectory())
        {
            fail(why, "cannot make the folder " + dir.getFullPathName().toStdString());
            return false;
        }
        const juce::File temp = dir.getChildFile("." + to.getFileName() + ".writing");
        bool ok = true;
        {
            temp.deleteFile();
            juce::FileOutputStream stream(temp);
            if (!stream.openedOk())
            {
                fail(why, "cannot write " + to.getFullPathName().toStdString());
                return false;
            }
            StoredZip zip(stream);
            const std::vector<std::string> files = bundleNames(names, package);
            juce::Array<juce::var> listed;
            int64_t raw = 0;
            for (std::size_t i = 0; i < names.size() && ok; ++i)
            {
                if (cancelled())
                {
                    out.cancelled = true;
                    ok = false;
                    break;
                }
                std::string whyNot;
                std::optional<projectstore::Project> p = load(names[i], &whyNot);
                std::vector<uint8_t> bytes;
                Result r;
                if (p)
                {
                    p->name = ownName(p->name);          // a factory project's: its own name, as a single export
                    if (!(package ? packageBytes(*p, allVersions, bytes, r, &whyNot) : sourceBytes(*p, allVersions, bytes, r, &whyNot)))
                        p.reset();
                }
                if (p)
                {
                    if (!zip.add(files[i], bytes.data(), bytes.size(), why))
                    {
                        ok = false;
                        break;
                    }
                    auto* o = new juce::DynamicObject();
                    o->setProperty("name", str(p->name));
                    o->setProperty("file", str(files[i]));
                    listed.add(juce::var(o));
                    ++out.projects;
                    out.file.versions += r.versions;
                    raw += r.raw;
                }
                else
                    out.failed.push_back(ownName(names[i]) + ": " + whyNot);
                if (progress.step)
                    progress.step(names[i], p.has_value(), whyNot);
            }
            if (ok && out.projects == 0)
            {
                fail(why, out.failed.empty() ? std::string("no project was exported") : out.failed.front());
                ok = false;
            }
            if (ok)
            {
                auto* m = new juce::DynamicObject();
                m->setProperty("format", kBundleFormat);
                m->setProperty("version", kBundleVersion);
                m->setProperty("plugin", product::kName);
                m->setProperty("kind", str(kindName()));
                m->setProperty("projects", listed);
                const std::string json = (juce::JSON::toString(juce::var(m), false) + "\n").toStdString();
                ok = zip.add(kBundleManifest, reinterpret_cast<const uint8_t*>(json.data()), json.size(), why) && zip.finish(why);
                raw += static_cast<int64_t>(json.size());
            }
            out.file.raw = raw;
            out.file.method = Method::stored;
        }
        if (ok && cancelled())
        {
            out.cancelled = true;
            ok = false;
        }
        if (out.cancelled)
            fail(why, "cancelled");
        if (!ok || !place(temp, to, replace, why))
        {
            temp.deleteFile();
            return false;
        }
        out.file.bytes = static_cast<int64_t>(to.getSize());
        return true;
    }

    bool isBundle(const juce::File& from)
    {
        juce::FileInputStream in(from);
        char head[2] = {};
        if (!in.openedOk() || in.read(head, 2) != 2 || head[0] != 'P' || head[1] != 'K')
            return false;
        juce::ZipFile zip(from);
        return zip.getIndexOfFileName(kBundleManifest) >= 0;
    }

    std::optional<std::vector<Item>> readAll(const juce::File& from, std::string* why)
    {
        const std::string label = from.getFileName().toStdString();
        if (!from.existsAsFile())
        {
            fail(why, "there is no file " + from.getFullPathName().toStdString());
            return std::nullopt;
        }
        if (!isBundle(from))
        {
            std::optional<projectstore::Project> p = read(from, why);
            if (!p)
                return std::nullopt;
            std::vector<Item> one(1);
            one[0].name = p->name;
            one[0].file = label;
            one[0].project = std::move(p);
            return one;
        }
        juce::ZipFile zip(from);
        const auto entryBytes = [&](int index, juce::MemoryBlock& data) {
            const std::unique_ptr<juce::InputStream> in(zip.createStreamForEntry(index));
            return in != nullptr && in->readIntoMemoryBlock(data) >= 0;
        };
        juce::MemoryBlock manifestBytes;
        const int at = zip.getIndexOfFileName(kBundleManifest);
        if (at < 0 || zip.getEntry(at)->uncompressedSize > 1024 * 1024 || !entryBytes(at, manifestBytes))
        {
            fail(why, label + "'s bundle.json cannot be read");
            return std::nullopt;
        }
        const juce::var m = juce::JSON::parse(manifestBytes.toString());
        if (!m.isObject() || m["format"].toString() != kBundleFormat)
        {
            fail(why, label + "'s bundle.json is not a bundle's (or it is damaged)");
            return std::nullopt;
        }
        if (static_cast<int>(m["version"]) > kBundleVersion)
        {
            fail(why, label + " is from a newer version of the plugin");
            return std::nullopt;
        }
        std::vector<Item> items;
        if (const juce::Array<juce::var>* projects = m["projects"].getArray())
            for (const juce::var& v : *projects)
            {
                if (static_cast<int>(items.size()) >= kMaxBundleProjects)
                    break;
                Item item;
                item.file = v["file"].toString().toStdString();
                item.name = v["name"].toString().toStdString();
                if (item.name.empty())
                    item.name = juce::File::createLegalFileName(str(item.file)).upToLastOccurrenceOf(".", false, false).toStdString();
                const int i = item.file.empty() || item.file.find('/') != std::string::npos ? -1 : zip.getIndexOfFileName(str(item.file));
                juce::MemoryBlock data;
                if (i < 0)
                    item.why = label + " lacks " + item.file;
                else if (zip.getEntry(i)->uncompressedSize > kMaxFile)
                    item.why = item.file + " is too large to be a project";
                else if (!entryBytes(i, data))
                    item.why = "cannot read " + item.file;
                else
                    item.project = readBytes(data, item.file, item.name, &item.why);
                items.push_back(std::move(item));
            }
        if (items.empty())
        {
            fail(why, label + " holds no project");
            return std::nullopt;
        }
        return items;
    }
}
