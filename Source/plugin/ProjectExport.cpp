// Source/plugin/ProjectExport.cpp: see ProjectExport.h.
#include "plugin/ProjectExport.h"

#include "Product.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cstring>

#if defined(__APPLE__)
#include <compression.h>
#endif

namespace testingplaygroundsynth::projectexport
{
    namespace
    {
        constexpr const char* kSourceFormat = "funk-playground-source";
        constexpr const char* kProjectFormat = "funk-playground-project";
        constexpr int kSourceVersion = 1;
        constexpr std::size_t kMaxRaw = 256u * 1024u * 1024u;   // a package that says it holds more is no package
        constexpr juce::int64 kMaxFile = 64 * 1024 * 1024;

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

        // Written beside, then moved over: the file is never half written.
        bool writeFile(const juce::File& to, const void* data, std::size_t size, std::string* why)
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
            if (!temp.moveFileTo(to))
            {
                temp.deleteFile();
                fail(why, "cannot replace " + to.getFullPathName().toStdString());
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

    bool writePackage(const projectstore::Project& p, bool allVersions, const juce::File& to, Result& out, std::string* why)
    {
        projectstore::Project q = p;
        if (!allVersions)
            q.versions = { p.latest() };
        const std::string json = projectstore::toJson(q);
        Method method = Method::stored;
        const std::vector<uint8_t> body = compress(json, method);
        std::vector<uint8_t> file(kHeaderBytes + body.size(), 0);
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
        if (!writeFile(to, file.data(), file.size(), why))
            return false;
        out = { static_cast<int64_t>(file.size()), static_cast<int64_t>(json.size()), method, static_cast<int>(q.versions.size()) };
        return true;
    }

    bool writeSource(const projectstore::Project& p, bool allVersions, const juce::File& to, Result& out, std::string* why)
    {
        const std::string root = p.name;
        juce::ZipFile::Builder zip;
        int64_t raw = 0;
        const auto add = [&](const std::string& path, const std::string& text, int64_t ms) {
            raw += static_cast<int64_t>(text.size());
            zip.addEntry(new juce::MemoryInputStream(text.data(), text.size(), true), 9, str(root + "/" + path),
                         juce::Time(static_cast<juce::int64>(ms)));
        };
        // The manifest: the project's JSON, each version's files by name only, and where they are.
        projectstore::Project q = p;
        if (!allVersions)
            q.versions = { p.latest() };
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
        add("README.md", readme(q, static_cast<int>(q.versions.size())), juce::Time::currentTimeMillis());
        add("project.json", (juce::JSON::toString(manifest, false) + "\n").toStdString(), juce::Time::currentTimeMillis());
        for (const projectstore::Version& v : q.versions)
        {
            const std::string folder = v.number == newest ? std::string() : "versions/v" + std::to_string(v.number) + "/";
            for (const SourceFile& f : v.files)
                add(folder + f.name, f.text, v.savedMs);
        }
        juce::MemoryOutputStream mem;
        if (!zip.writeToStream(mem, nullptr))
        {
            fail(why, "cannot make the zip");
            return false;
        }
        if (!writeFile(to, mem.getData(), mem.getDataSize(), why))
            return false;
        out = { static_cast<int64_t>(mem.getDataSize()), raw, Method::zlib, static_cast<int>(q.versions.size()) };
        return true;
    }

    std::optional<projectstore::Project> read(const juce::File& from, std::string* why)
    {
        const std::string label = from.getFileName().toStdString();
        if (!from.existsAsFile())
        {
            fail(why, "there is no file " + from.getFullPathName().toStdString());
            return std::nullopt;
        }
        if (from.getSize() > kMaxFile)
        {
            fail(why, label + " is too large to be a project");
            return std::nullopt;
        }
        juce::MemoryBlock bytes;
        if (!from.loadFileAsData(bytes) || bytes.getSize() < 4)
        {
            fail(why, "cannot read " + label);
            return std::nullopt;
        }
        const auto* b = static_cast<const uint8_t*>(bytes.getData());
        const auto named = [&](const std::string& json) -> std::optional<projectstore::Project> {
            const std::string name = juce::JSON::parse(str(json))["name"].toString().toStdString();
            return projectstore::fromJson(json, name.empty() ? from.getFileNameWithoutExtension().toStdString() : name, why);
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
        manifest.getDynamicObject()->setProperty("format", kProjectFormat);
        manifest.getDynamicObject()->setProperty("version", 1);
        return named((juce::JSON::toString(manifest, false) + "\n").toStdString());
    }
}
