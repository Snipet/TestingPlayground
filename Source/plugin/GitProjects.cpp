// Source/plugin/GitProjects.cpp: see GitProjects.h.
#include "plugin/GitProjects.h"

#include "plugin/EditorPort.h"
#include "plugin/ProjectTree.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <random>
#include <system_error>

namespace playground::gitprojects
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr const char* kRemote = "origin";
        constexpr const char* kTrailer = "Playground-Version";

        bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

        bool isLink(const fs::path& p)
        {
            std::error_code ec;
            return fs::is_symlink(fs::symlink_status(p, ec));
        }

        bool present(const fs::path& p)
        {
            std::error_code ec;
            return fs::exists(fs::symlink_status(p, ec));
        }

        std::string plural(int n, const char* one, const char* many)
        {
            return std::to_string(n) + " " + (n == 1 ? one : many);
        }

        std::string sha7(const std::string& sha) { return sha.substr(0, 7); }

        int64_t now() { return juce::Time::currentTimeMillis(); }

        git::Context contextOf(const Env& env, const std::string& folder)
        {
            git::Context c;
            c.git = env.git;
            c.folder = folder;
            c.cancel = env.cancel;
            c.allowFile = env.allowFile;
            c.networkMs = env.networkMs;
            c.localMs = env.localMs;
            c.outLimit = assets::kMaxFileBytes + 1;    // a sample's bytes, read through git show
            return c;
        }

        void say(const Env& env, const std::string& words)
        {
            if (env.doing)
                env.doing(words);
        }

        bool cancelled(const Env& env) { return env.cancel != nullptr && env.cancel->load(); }

        Outcome failed(const std::string& words)
        {
            Outcome o;
            o.words = words;
            return o;
        }

        // A git failure in words: git's own, and what may help.
        Outcome gitFailed(const Env& env, const git::Outcome& g, const std::string& lead = {})
        {
            Outcome o;
            o.cancelled = g.ended == run::Ended::cancelled || cancelled(env);
            if (o.cancelled)
            {
                o.words = "Cancelled.";
                return o;
            }
            // Git that did not start (its words begin "git ..."), not a refusal of this code's before any git ran.
            const bool notStarted = g.ended == run::Ended::notStarted && g.why.rfind("git ", 0) == 0;
            o.words = (lead.empty() ? std::string() : lead + "\n") + g.why + advice(g.why, notStarted);
            return o;
        }

        // <projects>/Git, made when missing; refused when it is a link or no folder.
        bool gitRoot(fs::path& root, std::string& why)
        {
            root = fs::path(projectstore::gitFolder().getFullPathName().toStdString());
            std::error_code ec;
            if (isLink(root))
            {
                why = root.string() + " is a link: the working folders go only into a real folder there. Nothing was made.";
                return false;
            }
            if (present(root) && !fs::is_directory(root, ec))
            {
                why = root.string() + " is not a folder. Nothing was made.";
                return false;
            }
            fs::create_directories(root, ec);
            if (!fs::is_directory(root, ec))
            {
                why = "cannot make the folder " + root.string();
                return false;
            }
            return true;
        }

        // A folder of `base`'s name under root that was not there, made by this call: "<base>", "<base>-2", ...
        bool freshFolder(const fs::path& root, const std::string& base, fs::path& made)
        {
            for (int k = 1; k < 1000; ++k)
            {
                const fs::path p = root / (k == 1 ? base : base + "-" + std::to_string(k));
                if (present(p))
                    continue;
                std::error_code ec;
                if (fs::create_directory(p, ec) && !ec)
                {
                    made = p;
                    return true;
                }
            }
            return false;
        }

        // A folder this very call made, gone again (never one that was there before, never through a link).
        void removeMade(const fs::path& made)
        {
            std::error_code ec;
            if (made.empty() || isLink(made) || !fs::is_directory(made, ec))
                return;
            fs::remove_all(made, ec);
        }

        std::string folderBase(const std::string& name, const std::string& id)
        {
            std::string base;
            for (const char ch : ownName(name))
                base += (ch == '/' || ch == '\\' || ch == ':' || static_cast<unsigned char>(ch) < 0x20) ? '-' : ch;
            while (!base.empty() && (base.front() == '.' || base.front() == ' '))
                base.erase(base.begin());
            if (base.empty())
                base = "Project";
            return base + "-" + id.substr(0, 8);
        }

        // The working folder of a project's link, checked: a real folder holding a repository.
        bool workingFolder(const projectstore::Link& link, fs::path& folder, std::string& why)
        {
            const fs::path root(projectstore::gitFolder().getFullPathName().toStdString());
            folder = root / link.folder;
            std::error_code ec;
            if (isLink(root) || isLink(folder) || !fs::is_directory(folder, ec) || !present(folder / ".git"))
            {
                why = "The working folder " + folder.string()
                    + " is gone, or is a link, or holds no repository. Disconnect, then Connect again (or Add from Git).";
                return false;
            }
            return true;
        }

        struct Linked
        {
            projectstore::Project project;
            projectstore::Link link;
            fs::path folder;
        };

        bool linkedProject(const std::string& name, Linked& out, std::string& why)
        {
            if (isFactoryName(name))
            {
                why = "A factory project is not linked to git: save your own copy, and link that.";
                return false;
            }
            std::optional<projectstore::Project> p = projectstore::load(name, &why);
            if (!p)
                return false;
            const std::string id = p->id.empty() ? projectstore::idOf(*p, &why) : p->id;
            const std::optional<projectstore::Link> l = projectstore::linkOf(id);
            if (!l)
            {
                why = "\"" + ownName(name) + "\" is not linked to a git repository.";
                return false;
            }
            out.project = std::move(*p);
            out.link = *l;
            return workingFolder(out.link, out.folder, why);
        }

        void recentOf(const git::Context& c, projectstore::Link& link)
        {
            std::vector<git::Commit> commits;
            if (git::log(c, static_cast<int>(projectstore::kRecentCommits), commits).ok)
            {
                link.recent.clear();
                for (const git::Commit& k : commits)
                    link.recent.push_back({ k.sha, k.subject, k.timeMs });
            }
        }

        std::string trackingRef(const std::string& branch) { return std::string("refs/remotes/") + kRemote + "/" + branch; }

        // The project's versions newer than the link's last commit, written and committed, oldest first; the link brought
        // up to date after each (so a failure later commits none twice).
        bool commitNew(const Env& env, const git::Context& c, const projectstore::Project& p, projectstore::Link& link, int& made,
                       std::string& why, git::Outcome& failure)
        {
            made = 0;
            std::vector<const projectstore::Version*> todo;
            for (const projectstore::Version& v : p.versions)
                if (v.number > link.committed)
                    todo.push_back(&v);
            for (std::size_t i = 0; i < todo.size(); ++i)
            {
                const projectstore::Version& v = *todo[i];
                if (cancelled(env))
                {
                    failure.ok = false;
                    failure.ended = run::Ended::cancelled;
                    failure.why = "cancelled";
                    return false;
                }
                say(env, "Committing version " + std::to_string(v.number) + " (" + std::to_string(i + 1) + " of "
                             + std::to_string(todo.size()) + ")...");
                std::vector<std::string> written;
                if (!projecttree::write(p, v, c.folder, link.written, written, &why))
                    return false;
                std::string sha;
                const std::string message = "Version " + std::to_string(v.number) + (v.note.empty() ? std::string() : ": " + v.note);
                failure = git::commitAll(c, message, v.savedMs, { { kTrailer, std::to_string(v.number) } }, &sha);
                if (!failure.ok)
                    return false;
                link.written = std::move(written);
                link.committed = v.number;
                link.commit = sha;
                ++made;
            }
            return true;
        }

        projecttree::Getter commitGetter(const git::Context& c, const std::string& rev)
        {
            return [c, rev](const std::string& path, std::string* why) -> std::optional<std::string> {
                std::string bytes;
                const git::Outcome o = git::show(c, rev, path, bytes);
                if (!o.ok)
                {
                    if (why != nullptr)
                        *why = o.why;
                    return std::nullopt;
                }
                return bytes;
            };
        }

        int64_t commitTime(const git::Context& c, const std::string& rev)
        {
            std::vector<git::Commit> one;
            return git::log(c, 1, one, rev).ok && !one.empty() ? one.front().timeMs : 0;
        }
        // ---- Part 3: both sides moved ---------------------------------------------------------------------------------------
        std::string listed(const std::vector<std::string>& names)
        {
            std::string out;
            for (std::size_t i = 0; i < names.size() && i < 4; ++i)
                out += (i == 0 ? "" : ", ") + names[i];
            if (names.size() > 4)
                out += " and " + std::to_string(names.size() - 4) + " more";
            return out;
        }

        // The project's version `v` as a tree's version, its sample files from the project (to save it again).
        std::vector<projectstore::SampleFile> filesOfVersion(const projectstore::Project& p, const projectstore::Version& v)
        {
            std::vector<projectstore::SampleFile> out;
            for (const projectstore::SampleRef& r : v.samples)
                if (const projectstore::SampleFile* f = p.sample(r.hash))
                    out.push_back(*f);
            return out;
        }

        // A commit of `tree` over HEAD and `theirs` (a merge commit made in the object store, its trailer the version it
        // is), then HEAD, the index and the folder fast-forwarded to it. "" with `failure` when it could not.
        std::string mergeCommit(const git::Context& c, const std::string& tree, const std::string& theirs, const std::string& message,
                                int version, git::Outcome& failure)
        {
            const std::string head = git::resolve(c, "HEAD");
            std::string sha;
            failure = git::commitTree(c, tree, { head, theirs },
                                      message + "\n\n" + kTrailer + ": " + std::to_string(version), now(), sha);
            if (!failure.ok)
                return {};
            failure = git::fastForwardTo(c, sha);
            return failure.ok ? sha : std::string();
        }

        // Pull's last case: both sides have commits. The versions not committed yet are committed, then the two merged in
        // git's object store (git merge-tree: the folder untouched): clean, the merge saved as a new version and made a
        // merge commit; else the remote's commit kept in the link (Link::theirs) for the card's three ways.
        Outcome mergeSides(const Env& env, const git::Context& c, Linked& k, const std::string& theirs)
        {
            const std::string where = shortRemote(k.link.remote);
            if (git::mergeUnderWay(c))
                return failed("A merge is under way in " + k.folder.string()
                              + " (made there, not by the plugin): finish it or abort it there first. Nothing was pulled.");
            std::string why;
            int made = 0;
            git::Outcome g;
            g.ok = true;
            const bool committed = commitNew(env, c, k.project, k.link, made, why, g);
            if (made > 0)
                projectstore::setLink(k.link);
            if (!committed)
                return !g.ok ? gitFailed(env, g, "Not pulled:") : failed("Not pulled: " + why + ".");
            say(env, "Merging with " + where + "...");
            git::Merged m;
            if (git::Outcome o = git::mergeTree(c, "HEAD", theirs, m); !o.ok)
                return gitFailed(env, o, "Not merged:");
            std::optional<projecttree::Tree> tree;
            if (m.clean)
            {
                tree = projecttree::read(commitGetter(c, m.tree), &why);   // a merge that is no project: the user chooses
                if (!tree)
                    m.conflicts = { "project.json" };
            }
            if (!m.clean || !tree)
            {
                k.link.theirs = theirs;
                git::Counts n;
                if (git::counts(c, kRemote, k.link.branch, n).ok)
                {
                    k.link.ahead = n.ahead;
                    k.link.behind = n.behind;
                }
                recentOf(c, k.link);
                projectstore::setLink(k.link);
                Outcome o;
                o.diverged = true;
                o.words = "Both sides changed " + (m.conflicts.empty() ? std::string("the same lines") : "the same lines of " + listed(m.conflicts))
                        + ", so git cannot merge them. Choose: Keep mine, Take theirs, or Compare... to see both. Either way both sides "
                          "stay, as versions.";
                return o;
            }
            say(env, "Saving the merge as a version...");
            const std::string lead = "Merged with " + sha7(theirs);
            const std::optional<projectstore::Saved> saved = projectstore::addVersion(
                k.project.name, tree->version, tree->samples, now(), lead,
                tree->presets, &tree->info, &why);
            if (!saved)
                return failed("Merged, but not saved as a version: " + why + ". Nothing was changed in the folder.");
            git::Outcome f;
            const std::string sha = mergeCommit(c, m.tree, theirs, lead + " (version " + std::to_string(saved->number) + ")",
                                                saved->number, f);
            if (sha.empty())
                return gitFailed(env, f, "Saved as version " + std::to_string(saved->number) + ", but the merge commit was not made:");
            k.link.committed = saved->number;
            k.link.commit = sha;
            k.link.written = projecttree::filesOf(tree->version);
            k.link.theirs.clear();
            git::Counts n;
            if (git::counts(c, kRemote, k.link.branch, n).ok)
            {
                k.link.ahead = n.ahead;
                k.link.behind = n.behind;
            }
            k.link.syncedMs = now();
            recentOf(c, k.link);
            projectstore::setLink(k.link);
            Outcome o;
            o.ok = true;
            o.version = saved->unchanged ? 0 : saved->number;
            o.words = "Both sides had changed, in different places: merged with " + where + " (" + sha7(theirs) + ") and saved as version "
                    + std::to_string(saved->number) + ". Push to send the merge.";
            return o;
        }
    }

    std::string shortRemote(const std::string& remote)
    {
        std::string s = remote;
        for (const char* scheme : { "https://", "http://", "ssh://", "file://" })
            if (s.rfind(scheme, 0) == 0)
            {
                s = s.substr(std::string(scheme).size());
                break;
            }
        if (const std::size_t at = s.find('@'); at != std::string::npos && at < s.find('/'))
            s = s.substr(at + 1);
        if (const std::size_t colon = s.find(':'); colon != std::string::npos && colon < s.find('/'))
            s = s.substr(0, colon) + "/" + s.substr(colon + 1);
        if (s.size() > 4 && s.compare(s.size() - 4, 4, ".git") == 0)
            s.resize(s.size() - 4);
        while (!s.empty() && s.back() == '/')
            s.pop_back();
        return s.empty() ? remote : s;
    }

    std::string folderOf(const projectstore::Link& link)
    {
        if (link.folder.empty())
            return {};
        return projectstore::gitFolder().getChildFile(juce::String::fromUTF8(link.folder.c_str())).getFullPathName().toStdString();
    }

    int toPush(const projectstore::Link& link, const std::vector<int>& versions)
    {
        return static_cast<int>(std::count_if(versions.begin(), versions.end(), [&](int n) { return n > link.committed; }));
    }

    std::string advice(const std::string& why, bool notStarted)
    {
        std::string more;
        if (notStarted || has(why, "Operation not permitted") || has(why, "sandbox"))
            more += "\nIf this plugin runs in a host that keeps plugins in a sandbox, git may not run here: the Standalone app can "
                    "push and pull (open the project there).";
        if (has(why, "Permission denied (publickey") || has(why, "Host key verification failed")
            || has(why, "passphrase"))
            more += "\nAdd your SSH key to the agent (ssh-add), or use an https address with a credential helper.";
        if (has(why, "could not read Username") || has(why, "terminal prompts disabled") || has(why, "Authentication failed"))
            more += "\nGit could not sign in: set up a credential helper or an SSH key in the agent (the plugin asks for no "
                    "password).";
        return more;
    }

    Outcome connect(const Env& env, const std::string& project, const std::string& remote, const std::string& wantedBranch)
    {
        if (isFactoryName(project))
            return failed("A factory project is not linked to git: save your own copy, and link that.");
        if (const std::string why = git::addressRefusal(remote, env.allowFile); !why.empty())
            return failed(why);
        const std::string branch = wantedBranch.empty() ? std::string("main") : wantedBranch;
        if (const std::string why = git::branchRefusal(branch); !why.empty())
            return failed(why);
        std::string why;
        std::optional<projectstore::Project> p = projectstore::load(project, &why);
        if (!p)
            return failed("Not linked: " + why + ".");
        const std::string id = p->id.empty() ? projectstore::idOf(*p, &why) : p->id;
        if (id.empty())
            return failed("Not linked: " + why + ".");
        if (const auto l = projectstore::linkOf(id))
            return failed("\"" + ownName(project) + "\" is linked to " + shortRemote(l->remote) + " already.");
        fs::path root, folder;
        if (!gitRoot(root, why))
            return failed("Not linked: " + why);
        // The remote's branch looked at by its address before any folder is made: one with commits is refused, and so is
        // a remote that cannot be reached, with nothing made.
        say(env, "Looking at " + shortRemote(remote) + "...");
        {
            const git::Outcome h = git::call(contextOf(env, root.string()), { "ls-remote", "--heads", "--", remote, "refs/heads/" + branch },
                                             true);
            if (!h.ok)
                return gitFailed(env, h, "Not linked: " + shortRemote(remote) + " could not be reached.");
            if (h.out.find("refs/heads/" + branch) != std::string::npos)
                return failed("This repository is not empty (its branch " + branch
                              + " has commits). Use Add from Git to make a project of it, or give an empty repository.");
        }
        if (!freshFolder(root, folderBase(project, id), folder))
            return failed("Not linked: no folder could be made in " + root.string() + ".");
        // From here, every way out that fails removes the folder this call made.
        const auto undo = [&](Outcome o) {
            removeMade(folder);
            return o;
        };
        const git::Context c = contextOf(env, folder.string());
        say(env, "Making the repository...");
        if (git::Outcome g = git::init(c, branch); !g.ok)
            return undo(gitFailed(env, g, "Not linked:"));
        projectstore::Link link;
        link.id = id;
        link.folder = folder.filename().string();
        link.remote = remote;
        link.branch = branch;
        int made = 0;
        git::Outcome g;
        g.ok = true;
        if (!commitNew(env, c, *p, link, made, why, g))
            return undo(!g.ok ? gitFailed(env, g, "Not linked:") : failed("Not linked: " + why + "."));
        if (git::Outcome a = git::addRemote(c, kRemote, remote); !a.ok)
            return undo(gitFailed(env, a, "Not linked:"));
        say(env, "Pushing to " + shortRemote(remote) + "...");
        if (git::Outcome s = git::push(c, kRemote, branch, true); !s.ok)
            return undo(gitFailed(env, s, "Not linked: the push to " + shortRemote(remote) + " failed."));
        link.ahead = link.behind = 0;
        link.syncedMs = link.checkedMs = now();
        recentOf(c, link);
        if (!projectstore::setLink(link, &why))
            return undo(failed("Not linked: " + why + "."));
        Outcome o;
        o.ok = true;
        o.words = "Linked to " + shortRemote(remote) + " (" + branch + "): " + plural(made, "version", "versions") + " pushed.";
        return o;
    }

    Outcome push(const Env& env, const std::string& project)
    {
        Linked k;
        std::string why;
        if (!linkedProject(project, k, why))
            return failed(why);
        if (!k.link.theirs.empty())
            return failed("Both sides have changed since they last met: settle that first (Pull).");
        const git::Context c = contextOf(env, k.folder.string());
        int made = 0;
        git::Outcome g;
        g.ok = true;
        const bool committed = commitNew(env, c, k.project, k.link, made, why, g);
        if (made > 0)
            projectstore::setLink(k.link);                 // what was committed stays committed, whatever comes next
        if (!committed)
            return !g.ok ? gitFailed(env, g, "Not pushed:") : failed("Not pushed: " + why + ".");
        const std::string where = shortRemote(k.link.remote);
        git::Counts before;                              // a merge commit to send counts as something pushed
        git::counts(c, kRemote, k.link.branch, before);
        say(env, "Pushing to " + where + "...");
        const git::Outcome s = git::push(c, kRemote, k.link.branch);
        if (!s.ok)
        {
            Outcome o = gitFailed(env, s, "Not pushed:");
            if (s.behind)
            {
                o.behind = true;
                say(env, "Fetching from " + where + "...");
                if (git::fetch(c, kRemote, k.link.branch).ok)
                {
                    git::Counts n;
                    if (git::counts(c, kRemote, k.link.branch, n).ok)
                    {
                        k.link.ahead = n.ahead;
                        k.link.behind = n.behind;
                        k.link.checkedMs = now();
                    }
                }
                o.words = where + " has " + (k.link.behind > 0 ? plural(k.link.behind, "commit", "commits") : std::string("commits"))
                        + " this project has not: pull first. Nothing was pushed, nothing forced.";
            }
            recentOf(c, k.link);
            projectstore::setLink(k.link);
            return o;
        }
        k.link.ahead = k.link.behind = 0;
        k.link.syncedMs = k.link.checkedMs = now();
        recentOf(c, k.link);
        projectstore::setLink(k.link);
        Outcome o;
        o.ok = true;
        o.words = made > 0           ? "Pushed " + plural(made, "version", "versions") + " to " + where + "."
                : before.ahead > 0 ? "Pushed to " + where + ": it has every version now."
                                   : "Nothing new to push: " + where + " has every version.";
        return o;
    }

    Outcome check(const Env& env, const std::string& project)
    {
        Linked k;
        std::string why;
        if (!linkedProject(project, k, why))
            return failed(why);
        const git::Context c = contextOf(env, k.folder.string());
        const std::string where = shortRemote(k.link.remote);
        say(env, "Fetching from " + where + "...");
        if (git::Outcome f = git::fetch(c, kRemote, k.link.branch); !f.ok)
            return gitFailed(env, f, "Not checked:");
        git::Counts n;
        if (git::Outcome f = git::counts(c, kRemote, k.link.branch, n); !f.ok)
            return gitFailed(env, f, "Not checked:");
        k.link.ahead = n.ahead;
        k.link.behind = n.behind;
        k.link.checkedMs = now();
        recentOf(c, k.link);
        projectstore::setLink(k.link);
        std::vector<int> numbers;
        for (const projectstore::Version& v : k.project.versions)
            numbers.push_back(v.number);
        const int pending = toPush(k.link, numbers) + n.ahead;
        Outcome o;
        o.ok = true;
        if (n.behind == 0 && pending == 0)
            o.words = "Up to date with " + where + ".";
        else
            o.words = (n.behind > 0 ? plural(n.behind, "commit", "commits") + " to pull" : std::string())
                    + (n.behind > 0 && pending > 0 ? ", " : "") + (pending > 0 ? plural(pending, "version", "versions") + " to push" : "")
                    + " (" + where + ").";
        return o;
    }

    Outcome pull(const Env& env, const std::string& project)
    {
        Linked k;
        std::string why;
        if (!linkedProject(project, k, why))
            return failed(why);
        const git::Context c = contextOf(env, k.folder.string());
        const std::string where = shortRemote(k.link.remote);
        say(env, "Fetching from " + where + "...");
        if (git::Outcome f = git::fetch(c, kRemote, k.link.branch); !f.ok)
            return gitFailed(env, f, "Not pulled:");
        const std::string theirs = git::resolve(c, trackingRef(k.link.branch));
        const std::string head = git::resolve(c, "HEAD");
        git::Counts n;
        if (git::Outcome f = git::counts(c, kRemote, k.link.branch, n); !f.ok)
            return gitFailed(env, f, "Not pulled:");
        k.link.ahead = n.ahead;
        k.link.behind = n.behind;
        k.link.checkedMs = now();
        std::vector<int> numbers;
        for (const projectstore::Version& v : k.project.versions)
            numbers.push_back(v.number);
        const int pending = toPush(k.link, numbers);
        // A fast-forward whose save did not land (or a commit made by hand in the folder): HEAD is past what was synced.
        const bool unsaved = n.behind == 0 && !head.empty() && head != k.link.commit && pending == 0 && n.ahead == 0;
        if (n.behind == 0 && !unsaved)
        {
            recentOf(c, k.link);
            projectstore::setLink(k.link);
            Outcome o;
            o.ok = true;
            o.words = "Nothing new on " + where + "."
                    + (pending + n.ahead > 0 ? " " + plural(pending + n.ahead, "version", "versions") + " to push." : std::string());
            return o;
        }
        if (!unsaved && (n.ahead > 0 || pending > 0))
            return mergeSides(env, c, k, theirs);       // both sides moved (part 3)
        const std::string rev = unsaved ? head : theirs;
        say(env, "Reading what " + where + " brought...");
        const std::optional<projecttree::Tree> tree = projecttree::read(commitGetter(c, rev), &why);
        if (!tree)
        {
            recentOf(c, k.link);
            projectstore::setLink(k.link);
            return failed("Not pulled: what " + where + " has (" + sha7(rev) + ") is not a project to open here: " + why
                          + ". The folder and the project are as they were.");
        }
        std::vector<git::Commit> brought;
        git::log(c, 50, brought, unsaved ? (k.link.commit.empty() ? head : k.link.commit + ".." + head) : "HEAD.." + theirs);
        if (!unsaved)
        {
            say(env, "Moving the folder to " + sha7(theirs) + "...");
            if (git::Outcome f = git::fastForward(c, kRemote, k.link.branch); !f.ok)
                return gitFailed(env, f, "Not pulled:");
        }
        const std::string subject = brought.empty() ? std::string() : brought.front().subject;
        const int more = static_cast<int>(brought.size()) - 1;
        const std::string lead = "Pulled " + sha7(rev) + (subject.empty() ? std::string() : ": " + subject)
                               + (more > 0 ? " and " + plural(more, "more commit", "more commits") : std::string());
        say(env, "Saving it as a version...");
        const std::optional<projectstore::Saved> saved =
            projectstore::addVersion(project, tree->version, tree->samples, commitTime(c, rev), lead, tree->presets, &tree->info, &why);
        if (!saved)
        {
            recentOf(c, k.link);
            projectstore::setLink(k.link);
            return failed("Pulled into the folder, but not saved as a version: " + why + ". Pull again to save it.");
        }
        k.link.committed = saved->number;
        k.link.commit = rev;
        k.link.written = projecttree::filesOf(tree->version);
        k.link.ahead = k.link.behind = 0;
        k.link.syncedMs = now();
        recentOf(c, k.link);
        projectstore::setLink(k.link);
        Outcome o;
        o.ok = true;
        o.version = saved->unchanged ? 0 : saved->number;
        const int count = std::max(1, static_cast<int>(brought.size()));
        o.words = saved->unchanged ? "Pulled " + plural(count, "commit", "commits") + " from " + where + ": the program is the same, no version added."
                                   : "Pulled " + plural(count, "commit", "commits") + " from " + where + ": saved as version "
                                         + std::to_string(saved->number) + ".";
        return o;
    }

    Outcome keepMine(const Env& env, const std::string& project)
    {
        Linked k;
        std::string why;
        if (!linkedProject(project, k, why))
            return failed(why);
        if (k.link.theirs.empty())
            return failed("Nothing to settle: Pull first.");
        const git::Context c = contextOf(env, k.folder.string());
        if (git::mergeUnderWay(c))
            return failed("A merge is under way in " + k.folder.string() + " (made there, not by the plugin): finish it or abort it there first.");
        const std::string theirs = k.link.theirs, where = shortRemote(k.link.remote);
        // Yours: the version last committed (the pull that found the two sides apart committed every one first), so a
        // second press after one cut off between its saves keeps yours, not the side it saved before.
        const projectstore::Version* committed = k.project.version(k.link.committed);
        const projectstore::Version mine = committed != nullptr ? *committed : k.project.latest();
        const std::vector<projectstore::SampleFile> mineFiles = filesOfVersion(k.project, mine);
        say(env, "Reading " + where + "'s side...");
        const std::optional<projecttree::Tree> tree = projecttree::read(commitGetter(c, theirs), &why);
        int theirsNumber = 0;
        if (tree)
        {
            const std::optional<projectstore::Saved> t = projectstore::addVersion(project, tree->version, tree->samples, commitTime(c, theirs),
                                                                                  "Theirs, " + sha7(theirs), tree->presets, nullptr, &why);
            if (!t)
                return failed("Not settled: their side could not be saved as a version: " + why + ".");
            theirsNumber = t->unchanged ? 0 : t->number;
        }
        const std::optional<projectstore::Saved> m = projectstore::addVersion(project, mine, mineFiles, now(), "Mine, kept over " + sha7(theirs),
                                                                              {}, nullptr, &why);
        if (!m)
            return failed("Not settled: yours could not be saved again: " + why + ".");
        say(env, "Making the merge commit...");
        git::Outcome f;
        const std::string sha = mergeCommit(c, git::treeOf(c, "HEAD"), theirs, "Kept mine over " + sha7(theirs), m->number, f);
        if (sha.empty())
            return gitFailed(env, f, "Saved, but the merge commit was not made (Keep mine again to make it):");
        k.link.committed = m->number;
        k.link.commit = sha;
        k.link.theirs.clear();
        git::Counts n;
        if (git::counts(c, kRemote, k.link.branch, n).ok)
        {
            k.link.ahead = n.ahead;
            k.link.behind = n.behind;
        }
        k.link.syncedMs = now();
        recentOf(c, k.link);
        projectstore::setLink(k.link);
        Outcome o;
        o.ok = true;
        o.words = "Kept yours: " + (theirsNumber > 0 ? where + "'s side saved as version " + std::to_string(theirsNumber) + ", y"
                                                     : std::string("y"))
                + "ours again as version " + std::to_string(m->number) + ". Push to send it.";
        return o;
    }

    Outcome takeTheirs(const Env& env, const std::string& project)
    {
        Linked k;
        std::string why;
        if (!linkedProject(project, k, why))
            return failed(why);
        if (k.link.theirs.empty())
            return failed("Nothing to settle: Pull first.");
        const git::Context c = contextOf(env, k.folder.string());
        if (git::mergeUnderWay(c))
            return failed("A merge is under way in " + k.folder.string() + " (made there, not by the plugin): finish it or abort it there first.");
        const std::string theirs = k.link.theirs, where = shortRemote(k.link.remote);
        const int mine = k.project.latest().number;
        say(env, "Reading " + where + "'s side...");
        const std::optional<projecttree::Tree> tree = projecttree::read(commitGetter(c, theirs), &why);
        if (!tree)
            return failed("Their side is not a project to open here: " + why + ". Keep mine keeps yours.");
        const std::optional<projectstore::Saved> t = projectstore::addVersion(project, tree->version, tree->samples, commitTime(c, theirs),
                                                                              "Took theirs, " + sha7(theirs), tree->presets, &tree->info, &why);
        if (!t)
            return failed("Not settled: their side could not be saved as a version: " + why + ".");
        say(env, "Making the merge commit...");
        git::Outcome f;
        const std::string sha = mergeCommit(c, git::treeOf(c, theirs), theirs, "Took theirs, " + sha7(theirs), t->number, f);
        if (sha.empty())
            return gitFailed(env, f, "Saved, but the merge commit was not made (Take theirs again to make it):");
        k.link.committed = t->number;
        k.link.commit = sha;
        k.link.written = projecttree::filesOf(tree->version);
        k.link.theirs.clear();
        git::Counts n;
        if (git::counts(c, kRemote, k.link.branch, n).ok)
        {
            k.link.ahead = n.ahead;
            k.link.behind = n.behind;
        }
        k.link.syncedMs = now();
        recentOf(c, k.link);
        projectstore::setLink(k.link);
        Outcome o;
        o.ok = true;
        o.version = t->unchanged ? 0 : t->number;
        o.words = "Took " + where + "'s side: saved as version " + std::to_string(t->number) + "; yours stays version "
                + std::to_string(mine) + ". Push to send the merge.";
        return o;
    }

    int64_t commitTimeOf(const Env& env, const std::string& project, const std::string& sha)
    {
        Linked k;
        std::string why;
        return linkedProject(project, k, why) ? commitTime(contextOf(env, k.folder.string()), sha) : 0;
    }

    std::optional<projecttree::Tree> theirsOf(const Env& env, const std::string& project, std::string& sha, std::string& why)
    {
        Linked k;
        if (!linkedProject(project, k, why))
            return std::nullopt;
        if (k.link.theirs.empty())
        {
            why = "Nothing to compare: the two sides have not moved apart.";
            return std::nullopt;
        }
        sha = k.link.theirs;
        say(env, "Reading " + shortRemote(k.link.remote) + "'s side...");
        return projecttree::read(commitGetter(contextOf(env, k.folder.string()), sha), &why);
    }

    Outcome add(const Env& env, const std::string& remote, const std::string& branch)
    {
        if (const std::string why = git::addressRefusal(remote, env.allowFile); !why.empty())
            return failed(why);
        if (!branch.empty())
            if (const std::string why = git::branchRefusal(branch); !why.empty())
                return failed(why);
        fs::path root, folder;
        std::string why;
        if (!gitRoot(root, why))
            return failed("Not added: " + why);
        static thread_local std::mt19937 rng{ std::random_device{}() };
        char tag[16];
        std::snprintf(tag, sizeof tag, "%08x", static_cast<unsigned>(rng()));
        if (!freshFolder(root, std::string("added-") + tag, folder))
            return failed("Not added: no folder could be made in " + root.string() + ".");
        const auto undo = [&](Outcome o) {
            removeMade(folder);
            return o;
        };
        const std::string where = shortRemote(remote);
        git::Context c = contextOf(env, folder.string());
        say(env, "Cloning " + where + "...");
        if (git::Outcome g = git::clone(c, remote, folder.string(), branch); !g.ok)
            return undo(gitFailed(env, g, "Not added:"));
        const std::string head = git::resolve(c, "HEAD");
        const std::string branchNow = branch.empty() ? git::currentBranch(c) : branch;
        if (head.empty() || branchNow.empty())
            return undo(failed("Not added: " + where + " is empty. Connect a project to it instead."));
        say(env, "Reading the project...");
        const std::optional<projecttree::Tree> tree = projecttree::read(commitGetter(c, "HEAD"), &why);
        if (!tree)
            return undo(failed("Not added: " + where + " is not a project to open here: " + why + "."));
        std::vector<git::Commit> tip;
        git::log(c, 1, tip, "HEAD");
        projectstore::Project np;
        np.name = tree->name.empty() ? std::string("From git") : tree->name;
        np.info = tree->info;
        np.presets = tree->presets;
        for (projectstore::Preset& pr : np.presets)
            pr.savedMs = now();
        projectstore::Version v = tree->version;
        v.number = 1;
        v.savedMs = tip.empty() ? now() : tip.front().timeMs;
        v.note = "Added from " + where + " at " + sha7(head) + (tip.empty() ? std::string() : ": " + tip.front().subject);
        np.versions.push_back(std::move(v));
        np.samples = tree->samples;
        np.createdMs = now();
        const std::optional<std::string> name = projectstore::add(std::move(np), &why);
        if (!name)
            return undo(failed("Not added: " + why + "."));
        const std::string id = projectstore::idOf(*name, &why);
        if (id.empty())
            return failed("Added as \"" + *name + "\", but not linked: " + why + ".");
        // The folder named after the project now that it has one (left at its first name when it cannot be).
        fs::path named;
        for (int k = 1; k < 100 && named.empty(); ++k)
        {
            const fs::path p = root / (k == 1 ? folderBase(*name, id) : folderBase(*name, id) + "-" + std::to_string(k));
            if (present(p))
                continue;
            std::error_code ec;
            fs::rename(folder, p, ec);
            if (!ec)
                named = p;
            else
                break;
        }
        if (!named.empty())
            folder = named;
        c.folder = folder.string();
        projectstore::Link link;
        link.id = id;
        link.folder = folder.filename().string();
        link.remote = remote;
        link.branch = branchNow;
        link.committed = 1;
        link.commit = head;
        link.written = projecttree::filesOf(tree->version);
        link.ahead = link.behind = 0;
        link.syncedMs = link.checkedMs = now();
        recentOf(c, link);
        if (!projectstore::setLink(link, &why))
            return failed("Added as \"" + *name + "\", but not linked: " + why + ".");
        Outcome o;
        o.ok = true;
        o.project = *name;
        o.version = 1;
        o.words = "Added \"" + *name + "\" from " + where + ": version 1 is its newest commit (" + sha7(head) + ").";
        return o;
    }

    Outcome disconnect(const std::string& project)
    {
        std::string why;
        std::optional<projectstore::Project> p = projectstore::load(project, &why);
        if (!p)
            return failed("Not disconnected: " + why + ".");
        const std::optional<projectstore::Link> l = p->id.empty() ? std::nullopt : projectstore::linkOf(p->id);
        if (!l)
            return failed("\"" + ownName(project) + "\" is not linked to a git repository.");
        if (!projectstore::removeLink(l->id, &why))
            return failed("Not disconnected: " + why + ".");
        Outcome o;
        o.ok = true;
        o.words = "Disconnected from " + shortRemote(l->remote) + ". The project stays, and so does its folder: " + folderOf(*l) + ".";
        return o;
    }
}
