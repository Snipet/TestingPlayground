// Source/plugin/PresetStore.cpp: see PresetStore.h.
#include "plugin/PresetStore.h"

#include "Product.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <mutex>

namespace testingplayground::presetstore
{
    namespace
    {
        std::mutex overrideMutex;
        juce::File overrideDir;                          // empty: the default
        constexpr const char* kFormat = "funk-playground-preset";
        constexpr int kVersion = 1;
        constexpr juce::int64 kMaxBytes = 16 * 1024 * 1024;   // a preset larger than this is no preset

        bool sameName(std::string_view a, std::string_view b)
        {
            return a.size() == b.size()
                && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                       return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
                   });
        }

        // The file of the preset named so (case aside), or none.
        juce::File fileOf(std::string_view name)
        {
            const std::string tidy = tidyName(name);
            if (tidy.empty())
                return {};
            for (const juce::File& f : directory().findChildFiles(juce::File::findFiles, false, juce::String("*") + kExtension))
                if (sameName(f.getFileNameWithoutExtension().toStdString(), tidy))
                    return f;
            return {};
        }

        std::string kindName(bool instrument) { return instrument ? "instrument" : "effect"; }
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

    std::vector<Entry> list()
    {
        std::vector<Entry> out;
        for (const juce::File& f : directory().findChildFiles(juce::File::findFiles, false, juce::String("*") + kExtension))
        {
            const std::string name = f.getFileNameWithoutExtension().toStdString();
            if (tidyName(name) == name && !name.empty())
                out.push_back({ name, f.getLastModificationTime().toMilliseconds() });
        }
        std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
            const juce::String x(a.name), y(b.name);
            const int c = x.compareIgnoreCase(y);
            return c != 0 ? c < 0 : a.name < b.name;
        });
        return out;
    }

    bool exists(std::string_view name) { return fileOf(name) != juce::File(); }

    bool save(const Preset& p, std::string* why)
    {
        const std::string name = tidyName(p.name);
        if (name.empty())
        {
            if (why != nullptr)
                *why = "a preset needs a name";
            return false;
        }
        const juce::File dir = directory();
        if (!dir.isDirectory() && !dir.createDirectory())
        {
            if (why != nullptr)
                *why = "cannot make the folder " + dir.getFullPathName().toStdString();
            return false;
        }
        auto* root = new juce::DynamicObject();
        root->setProperty("format", kFormat);
        root->setProperty("version", kVersion);
        root->setProperty("plugin", product::kName);
        root->setProperty("kind", juce::String(kindName(product::kIsInstrument)));
        root->setProperty("name", juce::String::fromUTF8(name.c_str()));
        root->setProperty("active", p.active);
        juce::Array<juce::var> files;
        for (const SourceFile& f : p.files)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("name", juce::String::fromUTF8(f.name.c_str()));
            o->setProperty("text", juce::String::fromUTF8(f.text.c_str()));
            files.add(juce::var(o));
        }
        root->setProperty("files", files);
        juce::Array<juce::var> dials;
        for (const Dial& d : p.dials)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("name", juce::String::fromUTF8(d.name.c_str()));
            o->setProperty("value", std::isfinite(d.value) ? static_cast<double>(d.value) : 0.0);
            dials.add(juce::var(o));
        }
        root->setProperty("dials", dials);
        const juce::String json = juce::JSON::toString(juce::var(root), false) + "\n";

        // Written beside, then moved over: a preset is never half written. One of the name in another case goes.
        const juce::File target = dir.getChildFile(juce::String::fromUTF8(name.c_str()) + kExtension);
        const juce::File temp = dir.getChildFile("." + juce::String::fromUTF8(name.c_str()) + ".saving");
        if (!temp.replaceWithText(json, false, false, "\n"))
        {
            if (why != nullptr)
                *why = "cannot write " + temp.getFullPathName().toStdString();
            return false;
        }
        const juce::File old = fileOf(name);
        if (old != juce::File() && old.getFileName() != target.getFileName())
            old.deleteFile();
        if (!temp.moveFileTo(target))
        {
            temp.deleteFile();
            if (why != nullptr)
                *why = "cannot replace " + target.getFullPathName().toStdString();
            return false;
        }
        return true;
    }

    std::optional<Preset> load(std::string_view name, std::string* why)
    {
        const auto fail = [why](const std::string& what) -> std::optional<Preset> {
            if (why != nullptr)
                *why = what;
            return std::nullopt;
        };
        const juce::File f = fileOf(name);
        if (f == juce::File())
            return fail("there is no preset named \"" + std::string(name) + "\"");
        if (f.getSize() > kMaxBytes)
            return fail(f.getFileName().toStdString() + " is too large to be a preset");
        const juce::var root = juce::JSON::parse(f.loadFileAsString());
        if (!root.isObject() || root["format"].toString() != kFormat)
            return fail(f.getFileName().toStdString() + " is not a preset (or it is damaged)");
        if (static_cast<int>(root["version"]) > kVersion)
            return fail(f.getFileName().toStdString() + " is from a newer version of the plugin");
        const std::string kind = root["kind"].toString().toStdString();
        if (kind != kindName(product::kIsInstrument))
            return fail("\"" + f.getFileNameWithoutExtension().toStdString() + "\" is a preset of the " + kind + " playground ("
                        + root["plugin"].toString().toStdString() + "): this one plays " + kindName(product::kIsInstrument)
                        + "s");
        Preset p;
        p.name = f.getFileNameWithoutExtension().toStdString();
        p.active = static_cast<int>(root["active"]);
        if (const juce::Array<juce::var>* files = root["files"].getArray())
            for (const juce::var& v : *files)
                if (v.isObject())
                    p.files.push_back({ v["name"].toString().toStdString(), v["text"].toString().toStdString() });
        if (p.files.empty())
            return fail("\"" + p.name + "\" has no program");
        if (const juce::Array<juce::var>* dials = root["dials"].getArray())
            for (const juce::var& v : *dials)
                if (v.isObject())
                {
                    const double value = static_cast<double>(v["value"]);
                    p.dials.push_back({ v["name"].toString().toStdString(), std::isfinite(value) ? static_cast<float>(value) : 0.0f });
                }
        return p;
    }

    bool remove(std::string_view name, std::string* why)
    {
        const juce::File f = fileOf(name);
        if (f == juce::File() || !f.deleteFile())
        {
            if (why != nullptr)
                *why = "cannot delete the preset \"" + std::string(name) + "\"";
            return false;
        }
        return true;
    }
}
