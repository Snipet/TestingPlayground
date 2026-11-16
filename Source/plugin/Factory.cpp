// Source/plugin/Factory.cpp: see Factory.h.
#include "plugin/Factory.h"

#include "plugin/EditorPort.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace playground::factory
{
    namespace
    {
        void fail(std::string* why, const std::string& what)
        {
            if (why != nullptr)
                *why = what;
        }

        // The set a FactoryStore published (none: the empty one is current) and whose it is, under one lock that is
        // held only to copy a pointer, so a reader on any thread never waits on the store's work.
        std::mutex publishedMutex;
        Snapshot published;
        const void* publisher = nullptr;
        std::atomic<uint64_t> generationNow{ 0 };

        Snapshot empty()
        {
            static const Snapshot none = std::make_shared<const Set>();
            return none;
        }

#if defined(FP_PROBES)
        const char testPublisher = 0;                    // publishForTests' owner: no store's
#endif
    }

    const projectstore::Project* Set::find(std::string_view name) const
    {
        const std::string own = ownName(name);
        for (const projectstore::Project& p : projects)
            if (projectstore::sameName(ownName(p.name), own))
                return &p;
        return nullptr;
    }

    Snapshot current()
    {
        {
            const std::lock_guard<std::mutex> lock(publishedMutex);
            if (published != nullptr)
                return published;
        }
        return empty();
    }

    uint64_t generation() { return generationNow.load(std::memory_order_acquire); }

    std::shared_ptr<const projectstore::Project> find(std::string_view name)
    {
        Snapshot set = current();
        const projectstore::Project* p = set->find(name);
        if (p == nullptr)
            return nullptr;
        return std::shared_ptr<const projectstore::Project>(std::move(set), p);   // the snapshot lives while it does
    }

    Snapshot setOf(std::vector<projectstore::Project> projects, int64_t sequence)
    {
        auto out = std::make_shared<Set>();
        out->projects = std::move(projects);
        out->sequence = sequence;
        std::sort(out->projects.begin(), out->projects.end(), [](const projectstore::Project& a, const projectstore::Project& b) {
            const int c = juce::String(a.name).compareIgnoreCase(juce::String(b.name));
            return c != 0 ? c < 0 : a.name < b.name;
        });
        return out;
    }

    void publish(const void* owner, Snapshot set)
    {
        Snapshot old;                                    // released outside the lock
        {
            const std::lock_guard<std::mutex> lock(publishedMutex);
            if (set == nullptr && publisher != owner)
                return;                                  // another store's set (a later one's): not this one's to take
            old = std::move(published);
            published = std::move(set);
            publisher = published != nullptr ? owner : nullptr;
            generationNow.fetch_add(1, std::memory_order_acq_rel);
        }
    }

#if defined(FP_PROBES)
    void publishForTests(Snapshot set) { publish(&testPublisher, std::move(set)); }
#endif

    std::optional<projectstore::Project> fromParts(std::string_view own, const std::string& json, const Sources& files,
                                                   std::string* why)
    {
        const std::string name(own);
        if (name.empty() || projectstore::tidyName(name) != name)
        {
            fail(why, "\"" + name + "\" is no project's name (a name keeps printable ASCII, no / \\ : * ? \" < > |)");
            return std::nullopt;
        }
        juce::var root = juce::JSON::parse(juce::String::fromUTF8(json.c_str()));
        juce::DynamicObject* o = root.getDynamicObject();
        const juce::var versions = o != nullptr ? o->getProperty("versions") : juce::var();
        const juce::Array<juce::var>* vs = versions.getArray();
        if (vs == nullptr || vs->size() != 1 || (*vs)[0].getDynamicObject() == nullptr)
        {
            fail(why, "its project.json is not a project of one version (or it is damaged)");
            return std::nullopt;
        }
        // Its files' texts, from beside it, into its version as a project file holds them.
        const juce::var named = (*vs)[0].getDynamicObject()->getProperty("files");
        const juce::Array<juce::var>* fs = named.getArray();
        if (fs == nullptr || fs->isEmpty())
        {
            fail(why, "its version names no file");
            return std::nullopt;
        }
        for (const juce::var& f : *fs)
        {
            juce::DynamicObject* fo = f.getDynamicObject();
            const std::string file = fo != nullptr ? fo->getProperty("name").toString().toStdString() : std::string();
            const SourceFile* text = findFile(files, file);
            if (text == nullptr)
            {
                fail(why, "its version names " + (file.empty() ? std::string("a file with no name") : file) + ", which is not beside it");
                return std::nullopt;
            }
            fo->setProperty("text", juce::String::fromUTF8(text->text.c_str()));
        }
        std::optional<projectstore::Project> p = projectstore::fromJson(juce::JSON::toString(root, true).toStdString(), name, why);
        if (!p)
            return std::nullopt;
        if (p->versions.size() != 1 || p->latest().files.size() != static_cast<std::size_t>(fs->size()))
        {
            fail(why, "its version's files do not read as a program's (a name a tab may not have, or one twice)");
            return std::nullopt;
        }
        p->name = factoryName(name);
        return p;
    }

    std::optional<projectstore::Project> fromFolder(const juce::File& folder, std::string* why)
    {
        const juce::File json = folder.getChildFile("project.json");
        if (!json.existsAsFile())
        {
            fail(why, folder.getFileName().toStdString() + " has no project.json");
            return std::nullopt;
        }
        const std::string text = json.loadFileAsString().toStdString();
        Sources files;
        if (const juce::var root = juce::JSON::parse(juce::String::fromUTF8(text.c_str())); root.isObject())
            if (const juce::Array<juce::var>* vs = root["versions"].getArray(); vs != nullptr && vs->size() == 1)
                if (const juce::Array<juce::var>* fs = (*vs)[0]["files"].getArray(); fs != nullptr)
                    for (const juce::var& f : *fs)
                    {
                        const juce::String name = f["name"].toString();
                        const juce::File file = folder.getChildFile(name);
                        if (name.isNotEmpty() && validFileName(name.toStdString()) && file.existsAsFile())
                            files.push_back({ name.toStdString(), file.loadFileAsString().toStdString() });
                    }
        return fromParts(folder.getFileName().toStdString(), text, files, why);
    }
}
