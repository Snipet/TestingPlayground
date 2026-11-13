// Source/plugin/ProjectTree.cpp: see ProjectTree.h.
#include "plugin/ProjectTree.h"

#include "plugin/EditorPort.h"

#include "Product.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <system_error>

namespace playground::projecttree
{
    namespace fs = std::filesystem;

    namespace
    {
        const char* kindName() { return product::kIsInstrument ? "instrument" : "effect"; }

        void fail(std::string* why, const std::string& text)
        {
            if (why != nullptr)
                *why = text;
        }

        juce::String str(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

        // A JSON string, as JUCE writes one.
        std::string q(const std::string& s) { return juce::JSON::toString(juce::var(str(s))).toStdString(); }

        std::string number(float v)
        {
            if (!std::isfinite(v))
                v = 0.0f;
            char buf[48];
            std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(v));   // a float's every bit, read back
            return buf;
        }

        std::string dialLine(const projectstore::Dial& d)
        {
            std::string s = "{ \"name\": " + q(d.name) + ", \"value\": " + number(d.value);
            if (!d.label.empty())
                s += ", \"label\": " + q(d.label);
            return s + " }";
        }

        std::string dialsInline(const std::vector<projectstore::Dial>& dials)
        {
            std::string s = "[";
            for (std::size_t i = 0; i < dials.size(); ++i)
                s += (i == 0 ? " " : ", ") + dialLine(dials[i]);
            return s + (dials.empty() ? "]" : " ]");
        }

        std::string stringsInline(const std::vector<std::string>& v)
        {
            std::string s = "[";
            for (std::size_t i = 0; i < v.size(); ++i)
                s += (i == 0 ? " " : ", ") + q(v[i]);
            return s + (v.empty() ? "]" : " ]");
        }

        // A list, one item a line.
        std::string listOf(const std::vector<std::string>& items)
        {
            if (items.empty())
                return "[]";
            std::string s = "[\n";
            for (std::size_t i = 0; i < items.size(); ++i)
                s += "    " + items[i] + (i + 1 < items.size() ? ",\n" : "\n");
            return s + "  ]";
        }

        std::string manifest(const projectstore::Project& p, const projectstore::Version& v)
        {
            std::vector<std::string> files, dials, presets, midi, circuits, samples;
            for (const SourceFile& f : v.files)
                files.push_back(q(f.name));
            for (const projectstore::Dial& d : v.dials)
                dials.push_back(dialLine(d));
            std::vector<projectstore::Preset> sorted = p.presets;
            std::sort(sorted.begin(), sorted.end(), [](const projectstore::Preset& a, const projectstore::Preset& b) {
                return str(a.name).compareIgnoreCase(str(b.name)) < 0;
            });
            for (const projectstore::Preset& pr : sorted)
            {
                std::string s = "{ \"name\": " + q(pr.name);
                if (!pr.category.empty())
                    s += ", \"category\": " + q(pr.category);
                s += ", \"dials\": " + dialsInline(pr.dials) + " }";
                presets.push_back(s);
            }
            for (const midilearn::Mapping& m : v.midi)
                midi.push_back("{ \"dial\": " + q(m.dial) + ", \"channel\": " + std::to_string(m.channel) + ", \"cc\": "
                               + std::to_string(m.cc) + (m.fine ? ", \"fine\": true }" : " }"));
            for (const projectstore::Answer& a : v.circuits)
                circuits.push_back("{ \"key\": " + q(a.key) + ", \"text\": " + q(a.text) + " }");
            for (const projectstore::SampleRef& r : v.samples)
                samples.push_back("{ \"name\": " + q(r.name) + ", \"file\": " + q("samples/" + r.name) + " }");
            const projectstore::Info& in = p.info;
            std::string info = "{ \"description\": " + q(in.description) + ", \"author\": " + q(in.author) + ", \"category\": "
                             + q(in.category) + ", \"tags\": " + stringsInline(in.tags) + ", \"genres\": " + stringsInline(in.genres)
                             + " }";
            std::string s = "{\n";
            s += "  \"format\": " + q(kFormat) + ",\n";
            s += "  \"version\": " + std::to_string(kVersion) + ",\n";
            s += "  \"plugin\": " + q(product::kName) + ",\n";
            s += "  \"kind\": " + q(kindName()) + ",\n";
            s += "  \"name\": " + q(ownName(p.name)) + ",\n";
            s += "  \"info\": " + info + ",\n";
            s += "  \"files\": " + listOf(files) + ",\n";
            s += "  \"active\": " + std::to_string(v.active) + ",\n";
            s += "  \"dials\": " + listOf(dials) + ",\n";
            s += "  \"presets\": " + listOf(presets) + ",\n";
            s += "  \"midi\": " + listOf(midi) + ",\n";
            s += "  \"circuits\": " + listOf(circuits) + ",\n";
            s += "  \"samples\": " + listOf(samples) + "\n";
            return s + "}\n";
        }

        std::string readme(const projectstore::Project& p)
        {
            std::string s = "# " + ownName(p.name) + "\n\n";
            if (!p.info.description.empty())
                s += p.info.description + "\n\n";
            s += std::string("A project of ") + product::kName + " (" + kindName() + "), kept in git: each commit is a version.\n\n"
                 "`project.json` holds its dials, presets and the order of its files; `program.fm` and the files beside it are "
                 "its code; `samples/` the audio files it plays. Open it in " + product::kName
                 + ": PROJECTS, Git..., Add from Git..., with this repository's address.\n";
            return s;
        }

        bool validPath(const std::string& path)
        {
            if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos || path.find(':') != std::string::npos)
                return false;
            std::size_t from = 0;
            while (from <= path.size())
            {
                const std::size_t end = std::min(path.find('/', from), path.size());
                const std::string part = path.substr(from, end - from);
                if (part.empty() || part == "." || part == ".." || part.front() == '.')
                    return part == kAttributes && end == path.size();
                from = end + 1;
            }
            return true;
        }

        bool isLink(const fs::path& p)
        {
            std::error_code ec;
            return fs::is_symlink(fs::symlink_status(p, ec));
        }

        // `rel` under `root` passes through no link, and is none itself.
        bool noLinkOnWay(const fs::path& root, const std::string& rel)
        {
            fs::path at = root;
            std::size_t from = 0;
            while (from <= rel.size())
            {
                const std::size_t end = std::min(rel.find('/', from), rel.size());
                at /= rel.substr(from, end - from);
                if (isLink(at))
                    return false;
                from = end + 1;
            }
            return true;
        }

        bool writeBytes(const fs::path& to, const std::string& bytes, std::string* why)
        {
            std::error_code ec;
            fs::create_directories(to.parent_path(), ec);
            static thread_local std::mt19937_64 rng{ std::random_device{}() };
            char tag[24];
            std::snprintf(tag, sizeof tag, ".%016llx", static_cast<unsigned long long>(rng()));
            const fs::path temp = to.parent_path() / (to.filename().string() + tag + ".part");
            {
                std::ofstream f(temp, std::ios::binary | std::ios::trunc);
                f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                f.close();
                if (!f)
                {
                    fs::remove(temp, ec);
                    fail(why, "cannot write " + to.string());
                    return false;
                }
            }
            fs::rename(temp, to, ec);
            if (ec)
            {
                fs::remove(temp, ec);
                fail(why, "cannot write " + to.string());
                return false;
            }
            return true;
        }

        std::optional<std::string> readBytes(const fs::path& from, std::size_t most, std::string* why)
        {
            std::error_code ec;
            if (isLink(from) || !fs::is_regular_file(from, ec))
            {
                fail(why, from.filename().string() + " is not there");
                return std::nullopt;
            }
            if (fs::file_size(from, ec) > most)
            {
                fail(why, from.filename().string() + " is too large");
                return std::nullopt;
            }
            std::ifstream f(from, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }

        std::vector<projectstore::Dial> dialsOf(const juce::var& v)
        {
            std::vector<projectstore::Dial> out;
            if (const juce::Array<juce::var>* a = v.getArray())
                for (const juce::var& d : *a)
                {
                    const std::string name = d["name"].toString().toStdString();
                    if (name.empty())
                        continue;
                    const double value = static_cast<double>(d["value"]);
                    out.push_back({ name, std::isfinite(value) ? static_cast<float>(value) : 0.0f,
                                    d["label"].toString().toStdString() });
                }
            return out;
        }

        std::vector<std::string> stringsOf(const juce::var& v)
        {
            std::vector<std::string> out;
            if (const juce::Array<juce::var>* a = v.getArray())
                for (const juce::var& s : *a)
                    out.push_back(s.toString().toStdString());
            return out;
        }
    }

    std::vector<std::string> filesOf(const projectstore::Version& v)
    {
        std::vector<std::string> out{ kManifest };
        for (const SourceFile& f : v.files)
            out.push_back(f.name);
        for (const projectstore::SampleRef& r : v.samples)
            out.push_back("samples/" + r.name);
        return out;
    }

    bool render(const projectstore::Project& p, const projectstore::Version& v, std::vector<File>& out, std::string* why)
    {
        out.clear();
        out.push_back({ kManifest, manifest(p, v) });
        for (const SourceFile& f : v.files)
        {
            if (!validFileName(f.name) || f.name == kManifest || f.name == kReadme)
            {
                fail(why, "a file's name, \"" + f.name + "\", cannot be one in the repository");
                return false;
            }
            out.push_back({ f.name, f.text });
        }
        for (const projectstore::SampleRef& r : v.samples)
        {
            const projectstore::SampleFile* s = p.sample(r.hash);
            if (s == nullptr || !assets::validName(r.name))
            {
                fail(why, "the sample " + r.name + " is not in the project");
                return false;
            }
            out.push_back({ "samples/" + r.name, s->bytes });
        }
        return true;
    }

    bool write(const projectstore::Project& p, const projectstore::Version& v, const std::string& folder,
               const std::vector<std::string>& before, std::vector<std::string>& written, std::string* why)
    {
        const fs::path root(folder);
        std::error_code ec;
        if (isLink(root) || !fs::is_directory(root, ec))
        {
            fail(why, folder + " is not a folder (or it is a link)");
            return false;
        }
        std::vector<File> files;
        if (!render(p, v, files, why))
            return false;
        // Every path checked before anything is written: none a link, none through one.
        for (const File& f : files)
            if (!validPath(f.path) || !noLinkOnWay(root, f.path))
            {
                fail(why, f.path + " in " + folder + " is a link, or cannot be a file there: nothing was written");
                return false;
            }
        for (const char* once : { kReadme, kAttributes })
            if (!noLinkOnWay(root, once))
            {
                fail(why, std::string(once) + " in " + folder + " is a link: nothing was written");
                return false;
            }
        for (const File& f : files)
            if (!writeBytes(root / fs::path(f.path), f.bytes, why))
                return false;
        if (!fs::exists(fs::symlink_status(root / kReadme, ec)) && !writeBytes(root / kReadme, readme(p), why))
            return false;
        if (!fs::exists(fs::symlink_status(root / kAttributes, ec))
            && !writeBytes(root / kAttributes, "* text=auto eol=lf\nsamples/* binary\n", why))
            return false;
        written.clear();
        for (const File& f : files)
            written.push_back(f.path);
        // What the last write listed and this one has not: removed, each a regular file, by its own name.
        for (const std::string& old : before)
        {
            if (std::find(written.begin(), written.end(), old) != written.end() || !validPath(old) || old == kReadme
                || old == kAttributes || !noLinkOnWay(root, old))
                continue;
            const fs::path at = root / fs::path(old);
            if (fs::is_regular_file(fs::symlink_status(at, ec)))
                fs::remove(at, ec);
        }
        return true;
    }

    Getter folderGetter(const std::string& folder)
    {
        return [root = fs::path(folder)](const std::string& path, std::string* why) -> std::optional<std::string> {
            if (!validPath(path) || !noLinkOnWay(root, path))
            {
                fail(why, path + " cannot be read");
                return std::nullopt;
            }
            return readBytes(root / fs::path(path), path.rfind("samples/", 0) == 0 ? assets::kMaxFileBytes : kMaxTextBytes, why);
        };
    }

    std::optional<Tree> read(const Getter& get, std::string* why)
    {
        std::string whyNot;
        const std::optional<std::string> json = get(kManifest, &whyNot);
        if (!json)
        {
            fail(why, "it is no project of " + std::string(product::kName) + "'s: it has no project.json");
            return std::nullopt;
        }
        if (json->size() > kMaxManifestBytes)
        {
            fail(why, "its project.json is too large");
            return std::nullopt;
        }
        const juce::var root = juce::JSON::parse(str(*json));
        if (!root.isObject() || root["format"].toString() != kFormat)
        {
            fail(why, "its project.json is no project of " + std::string(product::kName) + "'s (or it is damaged)");
            return std::nullopt;
        }
        if (static_cast<int>(root["version"]) > kVersion)
        {
            fail(why, "it is from a newer version of the plugin");
            return std::nullopt;
        }
        const std::string kind = root["kind"].toString().toStdString();
        if (kind != kindName())
        {
            fail(why, "it is a project of the " + kind + " playground (" + root["plugin"].toString().toStdString() + "): this one plays "
                          + kindName() + "s");
            return std::nullopt;
        }
        Tree t;
        t.name = projectstore::tidyName(root["name"].toString().toStdString());
        const juce::var& in = root["info"];
        t.info = projectstore::tidyInfo({ in["description"].toString().toStdString(), in["author"].toString().toStdString(),
                                          in["category"].toString().toStdString(), stringsOf(in["tags"]), stringsOf(in["genres"]) });
        for (const std::string& name : stringsOf(root["files"]))
        {
            if (!validFileName(name) || name == kManifest || findFile(t.version.files, name) != nullptr
                || t.version.files.size() >= kMaxFiles)
            {
                fail(why, "its file \"" + name + "\" cannot be one");
                return std::nullopt;
            }
            const std::optional<std::string> text = get(name, &whyNot);
            if (!text || text->size() > kMaxTextBytes)
            {
                fail(why, "its project.json names " + name + ", which is not there");
                return std::nullopt;
            }
            t.version.files.push_back({ name, *text });
        }
        if (t.version.files.empty())
        {
            fail(why, "it has no program");
            return std::nullopt;
        }
        t.version.active = std::clamp(static_cast<int>(root["active"]), 0, static_cast<int>(t.version.files.size()) - 1);
        t.version.dials = dialsOf(root["dials"]);
        if (const juce::Array<juce::var>* a = root["presets"].getArray())
            for (const juce::var& pr : *a)
            {
                projectstore::Preset preset;
                preset.name = projectstore::tidyName(pr["name"].toString().toStdString());
                if (preset.name.empty())
                    continue;
                preset.category = projectstore::tidyCategory(pr["category"].toString().toStdString());
                preset.dials = dialsOf(pr["dials"]);
                t.presets.push_back(std::move(preset));
            }
        std::vector<midilearn::Mapping> maps;
        if (const juce::Array<juce::var>* a = root["midi"].getArray())
            for (const juce::var& m : *a)
                maps.push_back({ m["dial"].toString().toStdString(), m.hasProperty("channel") ? static_cast<int>(m["channel"]) : 1,
                                 m.hasProperty("cc") ? static_cast<int>(m["cc"]) : -1, static_cast<bool>(m["fine"]) });
        t.version.midi = midilearn::tidy(maps);
        if (const juce::Array<juce::var>* a = root["circuits"].getArray())
            for (const juce::var& c : *a)
                if (t.version.circuits.size() < projectstore::kMaxAnswers)
                    t.version.circuits.push_back({ c["key"].toString().toStdString(), c["text"].toString().toStdString() });
        if (const juce::Array<juce::var>* a = root["samples"].getArray())
            for (const juce::var& s : *a)
            {
                const std::string name = s["name"].toString().toStdString();
                if (!assets::validName(name) || t.version.samples.size() >= assets::kMaxAssets
                    || std::any_of(t.version.samples.begin(), t.version.samples.end(),
                                   [&](const projectstore::SampleRef& r) { return r.name == name; }))
                {
                    fail(why, "its sample \"" + name + "\" cannot be one");
                    return std::nullopt;
                }
                const std::optional<std::string> bytes = get("samples/" + name, &whyNot);
                if (!bytes || bytes->size() > assets::kMaxFileBytes)
                {
                    fail(why, "its project.json names samples/" + name + ", which is not there (or is too large)");
                    return std::nullopt;
                }
                const std::string hash = assets::hashOf(*bytes);
                t.version.samples.push_back({ name, hash });
                if (std::none_of(t.samples.begin(), t.samples.end(), [&](const projectstore::SampleFile& f) { return f.hash == hash; }))
                    t.samples.push_back({ hash, *bytes });
            }
        return t;
    }
}
