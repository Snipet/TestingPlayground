// Source/plugin/ProjectStore.cpp: see ProjectStore.h.
#include "plugin/ProjectStore.h"

#include "Product.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <unordered_map>

namespace testingplaygroundsynth::projectstore
{
    namespace
    {
        std::mutex overrideMutex;
        juce::File overrideDir;                          // empty: the default
        constexpr const char* kFormat = "funk-playground-project";
        constexpr const char* kLegacyFormat = "funk-playground-preset";
        constexpr int kFormatVersion = 1;
        constexpr juce::int64 kMaxBytes = 64 * 1024 * 1024;   // a project larger than this is no project

        // The listing's cache: a file read again only when its size or its time changes.
        struct Cached
        {
            juce::int64 modified = 0, size = 0;
            Entry entry;
        };
        std::mutex cacheMutex;
        std::map<std::string, Cached> cache;             // by the file's full path

        std::string kindName(bool instrument) { return instrument ? "instrument" : "effect"; }

        juce::String str(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

        // The file of the project named so (case aside), or none.
        juce::File fileOf(std::string_view name, const char* extension = kExtension)
        {
            const std::string tidy = tidyName(name);
            if (tidy.empty())
                return {};
            for (const juce::File& f : directory().findChildFiles(juce::File::findFiles, false, juce::String("*") + extension))
                if (sameName(f.getFileNameWithoutExtension().toStdString(), tidy))
                    return f;
            return {};
        }

        juce::var dialsVar(const std::vector<Dial>& dials)
        {
            juce::Array<juce::var> out;
            for (const Dial& d : dials)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty("name", str(d.name));
                o->setProperty("value", std::isfinite(d.value) ? static_cast<double>(d.value) : 0.0);
                out.add(juce::var(o));
            }
            return out;
        }

        std::vector<Dial> dialsOf(const juce::var& v)
        {
            std::vector<Dial> out;
            if (const juce::Array<juce::var>* a = v.getArray())
                for (const juce::var& d : *a)
                    if (d.isObject())
                    {
                        const double value = static_cast<double>(d["value"]);
                        out.push_back({ d["name"].toString().toStdString(), std::isfinite(value) ? static_cast<float>(value) : 0.0f });
                    }
            return out;
        }

        juce::var filesVar(const Sources& files)
        {
            juce::Array<juce::var> out;
            for (const SourceFile& f : files)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty("name", str(f.name));
                o->setProperty("text", str(f.text));
                out.add(juce::var(o));
            }
            return out;
        }

        Sources filesOf(const juce::var& v)
        {
            Sources out;
            if (const juce::Array<juce::var>* a = v.getArray())
                for (const juce::var& f : *a)
                    if (f.isObject())
                        out.push_back({ f["name"].toString().toStdString(), f["text"].toString().toStdString() });
            return out;
        }

        bool sameDials(const std::vector<Dial>& a, const std::vector<Dial>& b)
        {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const Dial& x, const Dial& y) {
                       return x.name == y.name && x.value == y.value;
                   });
        }

        void fail(std::string* why, const std::string& what)
        {
            if (why != nullptr)
                *why = what;
        }

        // The project in `f`: nothing, with why, when it is not one of this plugin's or it is damaged.
        std::optional<Project> parse(const juce::File& f, std::string* why)
        {
            if (f.getSize() > kMaxBytes)
            {
                fail(why, f.getFileName().toStdString() + " is too large to be a project");
                return std::nullopt;
            }
            const juce::var root = juce::JSON::parse(f.loadFileAsString());
            if (!root.isObject() || root["format"].toString() != kFormat)
            {
                fail(why, f.getFileName().toStdString() + " is not a project (or it is damaged)");
                return std::nullopt;
            }
            if (static_cast<int>(root["version"]) > kFormatVersion)
            {
                fail(why, f.getFileName().toStdString() + " is from a newer version of the plugin");
                return std::nullopt;
            }
            const std::string kind = root["kind"].toString().toStdString();
            if (kind != kindName(product::kIsInstrument))
            {
                fail(why, "\"" + f.getFileNameWithoutExtension().toStdString() + "\" is a project of the " + kind + " playground ("
                              + root["plugin"].toString().toStdString() + "): this one plays " + kindName(product::kIsInstrument) + "s");
                return std::nullopt;
            }
            Project p;
            p.name = f.getFileNameWithoutExtension().toStdString();
            if (const juce::Array<juce::var>* versions = root["versions"].getArray())
                for (const juce::var& v : *versions)
                {
                    if (!v.isObject())
                        continue;
                    Version ver;
                    ver.number = static_cast<int>(v["number"]);
                    ver.savedMs = static_cast<int64_t>(static_cast<juce::int64>(v["saved"]));
                    ver.note = v["note"].toString().toStdString();
                    ver.active = static_cast<int>(v["active"]);
                    ver.files = filesOf(v["files"]);
                    ver.dials = dialsOf(v["dials"]);
                    if (!ver.files.empty() && ver.number > 0)
                        p.versions.push_back(std::move(ver));
                }
            std::sort(p.versions.begin(), p.versions.end(), [](const Version& a, const Version& b) { return a.number < b.number; });
            if (p.versions.empty())
            {
                fail(why, "\"" + p.name + "\" has no program");
                return std::nullopt;
            }
            if (const juce::Array<juce::var>* presets = root["presets"].getArray())
                for (const juce::var& v : *presets)
                {
                    if (!v.isObject())
                        continue;
                    Preset pr;
                    pr.name = tidyName(v["name"].toString().toStdString());
                    pr.savedMs = static_cast<int64_t>(static_cast<juce::int64>(v["saved"]));
                    pr.from = static_cast<int>(v["from"]);
                    pr.dials = dialsOf(v["dials"]);
                    if (!pr.name.empty() && p.preset(pr.name) == nullptr)
                        p.presets.push_back(std::move(pr));
                }
            return p;
        }

        void sortPresets(std::vector<Preset>& presets)
        {
            std::sort(presets.begin(), presets.end(), [](const Preset& a, const Preset& b) {
                const int c = str(a.name).compareIgnoreCase(str(b.name));
                return c != 0 ? c < 0 : a.name < b.name;
            });
        }

        // Writes `p` under p.name: beside, then moved over, so a project is never half written; one of the name in
        // another case goes.
        bool write(Project p, std::string* why)
        {
            p.name = tidyName(p.name);
            if (p.name.empty())
            {
                fail(why, "a project needs a name");
                return false;
            }
            const juce::File dir = directory();
            if (!dir.isDirectory() && !dir.createDirectory())
            {
                fail(why, "cannot make the folder " + dir.getFullPathName().toStdString());
                return false;
            }
            sortPresets(p.presets);
            auto* root = new juce::DynamicObject();
            root->setProperty("format", kFormat);
            root->setProperty("version", kFormatVersion);
            root->setProperty("plugin", product::kName);
            root->setProperty("kind", str(kindName(product::kIsInstrument)));
            root->setProperty("name", str(p.name));
            juce::Array<juce::var> versions;
            for (const Version& v : p.versions)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty("number", v.number);
                o->setProperty("saved", static_cast<juce::int64>(v.savedMs));
                o->setProperty("note", str(v.note));
                o->setProperty("active", v.active);
                o->setProperty("files", filesVar(v.files));
                o->setProperty("dials", dialsVar(v.dials));
                versions.add(juce::var(o));
            }
            root->setProperty("versions", versions);
            juce::Array<juce::var> presets;
            for (const Preset& pr : p.presets)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty("name", str(pr.name));
                o->setProperty("saved", static_cast<juce::int64>(pr.savedMs));
                o->setProperty("from", pr.from);
                o->setProperty("dials", dialsVar(pr.dials));
                presets.add(juce::var(o));
            }
            root->setProperty("presets", presets);
            const juce::String json = juce::JSON::toString(juce::var(root), false) + "\n";

            const juce::File target = dir.getChildFile(str(p.name) + kExtension);
            const juce::File temp = dir.getChildFile("." + str(p.name) + ".saving");
            if (!temp.replaceWithText(json, false, false, "\n"))
            {
                fail(why, "cannot write " + temp.getFullPathName().toStdString());
                return false;
            }
            const juce::File old = fileOf(p.name);
            if (old != juce::File() && old.getFileName() != target.getFileName())
                old.deleteFile();
            if (!temp.moveFileTo(target))
            {
                temp.deleteFile();
                fail(why, "cannot replace " + target.getFullPathName().toStdString());
                return false;
            }
            return true;
        }

        // The playground's presets before projects: each a project of one version, the old file then moved aside.
        void migrate()
        {
            const juce::File dir = directory();
            if (!dir.isDirectory())
                return;
            for (const juce::File& f : dir.findChildFiles(juce::File::findFiles, false, juce::String("*") + kLegacyExtension))
            {
                const std::string name = f.getFileNameWithoutExtension().toStdString();
                if (tidyName(name) != name || name.empty() || f.getSize() > kMaxBytes)
                    continue;
                const juce::var root = juce::JSON::parse(f.loadFileAsString());
                if (!root.isObject() || root["format"].toString() != kLegacyFormat
                    || root["kind"].toString().toStdString() != kindName(product::kIsInstrument))
                    continue;                            // not this plugin's, or damaged: left as it is
                if (fileOf(name) == juce::File())
                {
                    Project p;
                    p.name = name;
                    Version v;
                    v.number = 1;
                    v.savedMs = f.getLastModificationTime().toMilliseconds();
                    v.note = "the preset saved " + juce::Time(v.savedMs).formatted("%Y-%m-%d %H:%M").toStdString();
                    v.files = filesOf(root["files"]);
                    v.active = std::clamp(static_cast<int>(root["active"]), 0, std::max(0, static_cast<int>(v.files.size()) - 1));
                    v.dials = dialsOf(root["dials"]);
                    if (v.files.empty())
                        continue;
                    p.versions.push_back(std::move(v));
                    if (!write(std::move(p), nullptr))
                        continue;
                }
                const juce::File aside = dir.getChildFile(kMigratedFolder);
                if (!aside.isDirectory() && !aside.createDirectory())
                    continue;
                juce::File to = aside.getChildFile(f.getFileName());
                for (int k = 2; to.exists() && k < 1000; ++k)
                    to = aside.getChildFile(f.getFileNameWithoutExtension() + " " + juce::String(k) + kLegacyExtension);
                f.moveFileTo(to);
            }
        }

        // A project read, changed and written again.
        template <typename Change>
        bool update(std::string_view name, std::string* why, Change&& change)
        {
            std::optional<Project> p = load(name, why);
            if (!p)
                return false;
            if (!change(*p))
                return false;
            return write(std::move(*p), why);
        }

        // Lines `a` has that `b` lacks, as many times as it has them more, whatever their order.
        int linesMore(const std::string& a, const std::string& b)
        {
            std::unordered_map<std::string_view, int> count;
            const auto each = [](const std::string& s, auto&& f) {
                std::size_t at = 0;
                while (at <= s.size())
                {
                    const std::size_t end = std::min(s.find('\n', at), s.size());
                    if (!(end == s.size() && at == s.size()))
                        f(std::string_view(s).substr(at, end - at));
                    at = end + 1;
                }
            };
            each(b, [&](std::string_view line) { ++count[line]; });
            int more = 0;
            each(a, [&](std::string_view line) {
                if (int& c = count[line]; c > 0)
                    --c;
                else
                    ++more;
            });
            return more;
        }
    }

    const Version* Project::version(int number) const
    {
        for (const Version& v : versions)
            if (v.number == number)
                return &v;
        return nullptr;
    }

    const Preset* Project::preset(std::string_view wanted) const
    {
        for (const Preset& p : presets)
            if (sameName(p.name, wanted))
                return &p;
        return nullptr;
    }

    juce::File directory()
    {
        {
            const std::lock_guard<std::mutex> lock(overrideMutex);
            if (overrideDir != juce::File())
                return overrideDir;
        }
        // A probe's sandbox (CTest sets <ENV_PREFIX>PREFS_DIR for each, cmake/Probes.cmake): never the user's folder.
        const std::string sandbox = std::string(product::kEnvPrefix) + "PREFS_DIR";
        if (const char* dir = std::getenv(sandbox.c_str()); dir != nullptr && *dir != '\0')
            return juce::File(juce::String::fromUTF8(dir)).getChildFile("presets");
        return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
            .getChildFile("Library")
            .getChildFile("Audio")
            .getChildFile("Presets")
            .getChildFile(product::kCompany)
            .getChildFile(product::kName);
    }

    void setDirectoryForTests(const juce::File& dir)
    {
        const std::lock_guard<std::mutex> lock(overrideMutex);
        overrideDir = dir;
    }

    std::string tidyName(std::string_view name)
    {
        std::string out;
        bool space = false;
        for (const char ch : name)
        {
            const auto c = static_cast<unsigned char>(ch);
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            {
                space = !out.empty();
                continue;
            }
            if (c < 0x21 || c > 0x7E || std::string_view("/\\:*?\"<>|").find(ch) != std::string_view::npos)
                continue;
            if (out.empty() && ch == '.')
                continue;
            if (space)
                out += ' ';
            space = false;
            out += ch;
        }
        if (out.size() > kMaxName)
            out.resize(kMaxName);
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        return out;
    }

    bool sameName(std::string_view a, std::string_view b)
    {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
               });
    }

    std::vector<Entry> list()
    {
        migrate();
        std::vector<Entry> out;
        std::map<std::string, Cached> seen;
        {
            const std::lock_guard<std::mutex> lock(cacheMutex);
            for (const juce::File& f : directory().findChildFiles(juce::File::findFiles, false, juce::String("*") + kExtension))
            {
                const std::string name = f.getFileNameWithoutExtension().toStdString();
                if (tidyName(name) != name || name.empty())
                    continue;
                const std::string path = f.getFullPathName().toStdString();
                const juce::int64 modified = f.getLastModificationTime().toMilliseconds(), size = f.getSize();
                Cached c;
                if (auto it = cache.find(path); it != cache.end() && it->second.modified == modified && it->second.size == size)
                    c = it->second;
                else
                {
                    std::optional<Project> p = parse(f, nullptr);
                    if (!p)
                        continue;
                    c.modified = modified;
                    c.size = size;
                    c.entry.name = p->name;
                    c.entry.savedMs = p->latest().savedMs;
                    c.entry.latest = p->latest().number;
                    c.entry.versions = static_cast<int>(p->versions.size());
                    for (const Preset& pr : p->presets)
                        c.entry.presets.push_back(pr.name);
                }
                out.push_back(c.entry);
                seen.emplace(path, std::move(c));
            }
            cache.swap(seen);
        }
        std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
            const int c = str(a.name).compareIgnoreCase(str(b.name));
            return c != 0 ? c < 0 : a.name < b.name;
        });
        return out;
    }

    bool exists(std::string_view name)
    {
        migrate();
        return fileOf(name) != juce::File();
    }

    std::optional<Project> load(std::string_view name, std::string* why)
    {
        migrate();
        const juce::File f = fileOf(name);
        if (f == juce::File())
        {
            fail(why, "there is no project named \"" + std::string(name) + "\"");
            return std::nullopt;
        }
        return parse(f, why);
    }

    std::optional<Saved> saveVersion(std::string_view name, const Sources& files, int active, const std::vector<Dial>& dials,
                                     std::string* why)
    {
        const std::string tidy = tidyName(name);
        if (tidy.empty())
        {
            fail(why, "a project needs a name");
            return std::nullopt;
        }
        if (files.empty())
        {
            fail(why, "there is no program to save");
            return std::nullopt;
        }
        Saved saved;
        Project p;
        if (exists(tidy))
        {
            std::optional<Project> old = load(tidy, why);
            if (!old)
                return std::nullopt;
            p = std::move(*old);
            const Version& last = p.latest();
            if (last.files == files && last.active == active && sameDials(last.dials, dials))
            {
                saved.number = last.number;
                saved.unchanged = true;
                if (p.name != tidy)                      // the same project, named in another case now
                {
                    p.name = tidy;
                    if (!write(std::move(p), why))
                        return std::nullopt;
                }
                return saved;
            }
        }
        else
            saved.created = true;
        p.name = tidy;
        Version v;
        v.number = p.versions.empty() ? 1 : p.latest().number + 1;
        v.savedMs = juce::Time::currentTimeMillis();
        v.note = describeChange(p.versions.empty() ? nullptr : &p.latest(), files, dials);
        v.files = files;
        v.active = active;
        v.dials = dials;
        p.versions.push_back(std::move(v));
        if (p.versions.size() > static_cast<std::size_t>(kMaxVersions))
            p.versions.erase(p.versions.begin(), p.versions.end() - kMaxVersions);
        saved.number = p.latest().number;
        if (!write(std::move(p), why))
            return std::nullopt;
        return saved;
    }

    bool savePreset(std::string_view project, std::string_view preset, const std::vector<Dial>& dials, int from, std::string* why)
    {
        const std::string name = tidyName(preset);
        if (name.empty())
        {
            fail(why, "a preset needs a name");
            return false;
        }
        return update(project, why, [&](Project& p) {
            Preset pr{ name, juce::Time::currentTimeMillis(), from, dials };
            for (Preset& old : p.presets)
                if (sameName(old.name, name))
                {
                    old = std::move(pr);
                    return true;
                }
            p.presets.push_back(std::move(pr));
            return true;
        });
    }

    bool removePreset(std::string_view project, std::string_view preset, std::string* why)
    {
        return update(project, why, [&](Project& p) {
            const auto it = std::find_if(p.presets.begin(), p.presets.end(), [&](const Preset& pr) { return sameName(pr.name, preset); });
            if (it == p.presets.end())
            {
                fail(why, "\"" + p.name + "\" has no preset named \"" + std::string(preset) + "\"");
                return false;
            }
            p.presets.erase(it);
            return true;
        });
    }

    bool removeVersion(std::string_view project, int number, std::string* why)
    {
        return update(project, why, [&](Project& p) {
            const auto it = std::find_if(p.versions.begin(), p.versions.end(), [&](const Version& v) { return v.number == number; });
            if (it == p.versions.end())
            {
                fail(why, "\"" + p.name + "\" keeps no version " + std::to_string(number));
                return false;
            }
            if (p.versions.size() == 1)
            {
                fail(why, "it is the project's only version (delete the project instead)");
                return false;
            }
            p.versions.erase(it);
            return true;
        });
    }

    bool remove(std::string_view name, std::string* why)
    {
        const juce::File f = fileOf(name);
        if (f == juce::File() || !f.deleteFile())
        {
            fail(why, "cannot delete the project \"" + std::string(name) + "\"");
            return false;
        }
        return true;
    }

    std::string describeChange(const Version* before, const Sources& files, const std::vector<Dial>& dials)
    {
        if (before == nullptr)
            return "first save";
        std::vector<std::string> parts;
        for (const SourceFile& f : files)
        {
            const auto old = std::find_if(before->files.begin(), before->files.end(), [&](const SourceFile& o) { return o.name == f.name; });
            if (old == before->files.end())
                parts.push_back(f.name + " new");
            else if (old->text != f.text)
            {
                const int added = linesMore(f.text, old->text), taken = linesMore(old->text, f.text);
                parts.push_back(f.name + " +" + std::to_string(added) + " -" + std::to_string(taken));
            }
        }
        for (const SourceFile& o : before->files)
            if (std::none_of(files.begin(), files.end(), [&](const SourceFile& f) { return f.name == o.name; }))
                parts.push_back(o.name + " gone");
        int moved = 0;
        for (const Dial& d : dials)
        {
            const auto old = std::find_if(before->dials.begin(), before->dials.end(), [&](const Dial& o) { return o.name == d.name; });
            moved += old == before->dials.end() || old->value != d.value ? 1 : 0;
        }
        std::string out;
        const std::size_t shown = std::min<std::size_t>(parts.size(), 3);
        for (std::size_t i = 0; i < shown; ++i)
            out += (i > 0 ? ", " : "") + parts[i];
        if (parts.size() > shown)
            out += ", " + std::to_string(parts.size() - shown) + " more files";
        if (moved > 0)
            out += (out.empty() ? "" : ", ") + std::to_string(moved) + (moved == 1 ? " dial" : " dials");
        return out.empty() ? "the tab shown" : out;
    }
}
