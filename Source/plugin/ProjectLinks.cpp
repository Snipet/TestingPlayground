// Source/plugin/ProjectLinks.cpp: ProjectStore.h's links to git remotes (package 05): the folder's .git-links.json.
#include "plugin/ProjectStore.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace playground::projectstore
{
    namespace
    {
        constexpr const char* kLinksFormat = "funk-playground-git-links";

        struct LinksFile
        {
            std::string git;
            std::vector<Link> links;
        };

        // The file as last read or written (its path, time and size), so links() reads it only when it changed; one
        // writer at a time in this process.
        std::mutex linksMutex;
        std::string cachedPath;
        juce::int64 cachedModified = -1, cachedSize = -1;
        LinksFile cached;
        std::atomic<uint64_t> revision{ 1 };

        juce::String str(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

        void fail(std::string* why, const std::string& text)
        {
            if (why != nullptr)
                *why = text;
        }

        juce::File linksFile() { return directory().getChildFile(kLinksFile); }

        // A working folder's name: one part, no link out of the Git folder.
        bool validFolder(const std::string& f)
        {
            return !f.empty() && f.size() <= 120 && f.front() != '.' && f.find_first_of("/\\:") == std::string::npos;
        }

        juce::var stringsVar(const std::vector<std::string>& v)
        {
            juce::Array<juce::var> a;
            for (const std::string& s : v)
                a.add(str(s));
            return a;
        }

        LinksFile parse(const juce::var& root)
        {
            LinksFile out;
            if (!root.isObject() || root["format"].toString() != kLinksFormat)
                return out;
            out.git = root["git"].toString().toStdString();
            if (const juce::Array<juce::var>* a = root["links"].getArray())
                for (const juce::var& v : *a)
                {
                    Link l;
                    l.id = v["id"].toString().toStdString();
                    l.folder = v["folder"].toString().toStdString();
                    l.remote = v["remote"].toString().toStdString();
                    l.branch = v["branch"].toString().toStdString();
                    l.committed = static_cast<int>(v["committed"]);
                    l.commit = v["commit"].toString().toStdString();
                    l.syncedMs = static_cast<juce::int64>(v["syncedMs"]);
                    l.ahead = v.hasProperty("ahead") ? static_cast<int>(v["ahead"]) : -1;
                    l.behind = v.hasProperty("behind") ? static_cast<int>(v["behind"]) : -1;
                    l.checkedMs = static_cast<juce::int64>(v["checkedMs"]);
                    if (const juce::Array<juce::var>* w = v["written"].getArray())
                        for (const juce::var& s : *w)
                            l.written.push_back(s.toString().toStdString());
                    if (const juce::Array<juce::var>* r = v["recent"].getArray())
                        for (const juce::var& c : *r)
                            if (l.recent.size() < kRecentCommits)
                                l.recent.push_back({ c["sha"].toString().toStdString(), c["subject"].toString().toStdString(),
                                                     static_cast<juce::int64>(c["ms"]) });
                    l.theirs = v["theirs"].toString().toStdString();
                    if (!validProjectId(l.id) || !validFolder(l.folder) || l.branch.empty()
                        || std::any_of(out.links.begin(), out.links.end(), [&](const Link& o) { return o.id == l.id; }))
                        continue;
                    out.links.push_back(std::move(l));
                }
            return out;
        }

        juce::var varOf(const LinksFile& f)
        {
            auto* root = new juce::DynamicObject();
            root->setProperty("format", kLinksFormat);
            root->setProperty("version", 1);
            if (!f.git.empty())
                root->setProperty("git", str(f.git));
            juce::Array<juce::var> links;
            for (const Link& l : f.links)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty("id", str(l.id));
                o->setProperty("folder", str(l.folder));
                o->setProperty("remote", str(l.remote));
                o->setProperty("branch", str(l.branch));
                o->setProperty("committed", l.committed);
                o->setProperty("commit", str(l.commit));
                o->setProperty("syncedMs", static_cast<juce::int64>(l.syncedMs));
                o->setProperty("ahead", l.ahead);
                o->setProperty("behind", l.behind);
                o->setProperty("checkedMs", static_cast<juce::int64>(l.checkedMs));
                o->setProperty("written", stringsVar(l.written));
                juce::Array<juce::var> recent;
                for (const LinkCommit& c : l.recent)
                {
                    auto* r = new juce::DynamicObject();
                    r->setProperty("sha", str(c.sha));
                    r->setProperty("subject", str(c.subject));
                    r->setProperty("ms", static_cast<juce::int64>(c.ms));
                    recent.add(juce::var(r));
                }
                o->setProperty("recent", recent);
                if (!l.theirs.empty())
                    o->setProperty("theirs", str(l.theirs));
                links.add(juce::var(o));
            }
            root->setProperty("links", links);
            return juce::var(root);
        }

        // The file as it is now (the caller holds linksMutex).
        LinksFile readLocked()
        {
            const juce::File f = linksFile();
            const std::string path = f.getFullPathName().toStdString();
            const bool there = f.existsAsFile();
            const juce::int64 modified = there ? f.getLastModificationTime().toMilliseconds() : 0, size = there ? f.getSize() : 0;
            if (cachedPath == path && cachedModified == modified && cachedSize == size)
                return cached;
            LinksFile read;
            if (there && size < 4 * 1024 * 1024)
                read = parse(juce::JSON::parse(f.loadFileAsString()));
            cachedPath = path;
            cachedModified = modified;
            cachedSize = size;
            cached = read;
            revision.fetch_add(1, std::memory_order_acq_rel);
            return read;
        }

        bool writeLocked(LinksFile f, std::string* why)
        {
            for (Link& l : f.links)
                if (l.recent.size() > kRecentCommits)
                    l.recent.resize(kRecentCommits);
            const juce::File dir = directory();
            if (!dir.isDirectory() && !dir.createDirectory())
            {
                fail(why, "cannot make the folder " + dir.getFullPathName().toStdString());
                return false;
            }
            const juce::File target = linksFile();
            const juce::File temp = dir.getChildFile(juce::String(kLinksFile) + ".saving");
            if (!temp.replaceWithText(juce::JSON::toString(varOf(f)), false, false, "\n") || !temp.moveFileTo(target))
            {
                temp.deleteFile();
                fail(why, "cannot write " + target.getFullPathName().toStdString());
                return false;
            }
            cachedPath = target.getFullPathName().toStdString();
            cachedModified = target.getLastModificationTime().toMilliseconds();
            cachedSize = target.getSize();
            cached = std::move(f);
            revision.fetch_add(1, std::memory_order_acq_rel);
            return true;
        }
    }

    juce::File gitFolder() { return directory().getChildFile(kGitFolderName); }

    std::vector<Link> links()
    {
        const std::lock_guard<std::mutex> lock(linksMutex);
        return readLocked().links;
    }

    std::optional<Link> linkOf(std::string_view id)
    {
        if (id.empty())
            return std::nullopt;
        const std::lock_guard<std::mutex> lock(linksMutex);
        for (const Link& l : readLocked().links)
            if (l.id == id)
                return l;
        return std::nullopt;
    }

    bool setLink(const Link& link, std::string* why)
    {
        if (!validProjectId(link.id) || !validFolder(link.folder) || link.branch.empty())
        {
            fail(why, "a link needs a project's id, a folder and a branch");
            return false;
        }
        const std::lock_guard<std::mutex> lock(linksMutex);
        LinksFile f = readLocked();
        const auto it = std::find_if(f.links.begin(), f.links.end(), [&](const Link& l) { return l.id == link.id; });
        if (it != f.links.end())
        {
            if (*it == link)
                return true;
            *it = link;
        }
        else
            f.links.push_back(link);
        return writeLocked(std::move(f), why);
    }

    bool removeLink(std::string_view id, std::string* why)
    {
        const std::lock_guard<std::mutex> lock(linksMutex);
        LinksFile f = readLocked();
        const auto it = std::find_if(f.links.begin(), f.links.end(), [&](const Link& l) { return l.id == id; });
        if (it == f.links.end())
            return true;
        f.links.erase(it);
        return writeLocked(std::move(f), why);
    }

    uint64_t linksRevision()
    {
        const std::lock_guard<std::mutex> lock(linksMutex);
        readLocked();
        return revision.load(std::memory_order_acquire);
    }

    std::string gitPath()
    {
        const std::lock_guard<std::mutex> lock(linksMutex);
        return readLocked().git;
    }

    bool setGitPath(const std::string& path, std::string* why)
    {
        const std::lock_guard<std::mutex> lock(linksMutex);
        LinksFile f = readLocked();
        if (f.git == path)
            return true;
        f.git = path;
        return writeLocked(std::move(f), why);
    }
}
