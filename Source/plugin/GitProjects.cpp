// Source/plugin/GitProjects.cpp: see GitProjects.h.
#include "plugin/GitProjects.h"

#include "plugin/EditorPort.h"
#include "plugin/ProjectExport.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <memory>

namespace playground::gitprojects
{
    namespace
    {
        juce::String str(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
        bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

        Result failed(std::string why)
        {
            Result r;
            r.why = std::move(why);
            return r;
        }

        // Git's refusal, with what to do when it looks like a sandboxed host's or an SSH key's that needs a passphrase.
        std::string wordsOf(const git::Outcome& o)
        {
            std::string w = o.why.empty() ? std::string("git failed") : o.why;
            if (o.ended == run::Ended::notStarted || has(w, "Operation not permitted") || has(w, "sandbox"))
                w += "\nA host that keeps its plugins in a sandbox may not let git run or reach the network: the Standalone "
                     "app can push and pull.";
            else if (has(w, "Permission denied (publickey") || has(w, "passphrase") || has(w, "Host key verification failed"))
                w += "\nGit's SSH sign-in did not go through: add your key to the SSH agent (ssh-add), or use an https "
                     "address with git's credential helper.";
            else if (has(w, "could not read Username") || has(w, "Authentication failed") || has(w, "terminal prompts disabled"))
                w += "\nThe remote asked for a sign-in git could not give: set up git's credential helper (or an SSH key) in "
                     "a terminal first.";
            return w;
        }

        Result gitFailed(const git::Outcome& o, const std::string& doing)
        {
            Result r = failed(doing + ": " + wordsOf(o));
            r.cancelled = o.ended == run::Ended::cancelled;
            if (r.cancelled)
                r.why = "Cancelled.";
            return r;
        }

        void say(const Setup& s, const std::string& what)
        {
            if (s.step)
                s.step(what);
        }

        git::Context contextOf(const Setup& s, const std::string& folder)
        {
            git::Context c;
            c.git = s.git;
            c.folder = folder;
            c.cancel = s.cancel;
            c.allowFile = s.allowFile;
            return c;
        }

        bool cancelled(const Setup& s) { return s.cancel != nullptr && s.cancel->load(); }

        std::string plural(int n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

        // The project, its id (made when it has none) and its link. Refusals in words.
        struct Linked
        {
            projectstore::Project project;
            std::string id;
            std::optional<projectstore::Link> link;
        };
        std::optional<Linked> linked(const std::string& name, std::string& why, bool needLink)
        {
            if (isFactoryName(name))
            {
                why = "A factory project cannot be kept in git: save your own copy of it first.";
                return std::nullopt;
            }
            std::optional<projectstore::Project> p = projectstore::load(name, &why);
            if (!p)
                return std::nullopt;
            Linked l;
            l.id = projectstore::idOf(*p, &why);
            if (l.id.empty())
                return std::nullopt;
            p->id = l.id;
            l.project = std::move(*p);
            l.link = projectstore::linkOf(l.id);
            if (needLink && !l.link)
            {
                why = "\"" + l.project.name + "\" is not linked to a git repository.";
                return std::nullopt;
            }
            if (needLink && !juce::File(str(l.link->folder)).getChildFile(".git").exists())
            {
                why = "Its working folder is gone (" + l.link->folder + "): Disconnect, then Connect again.";
                return std::nullopt;
            }
            return l;
        }

        // A commit for each kept version newer than `committed`, oldest first. Its count in `made`.
        bool commitVersions(const Setup& s, const git::Context& c, const projectstore::Project& p, int& committed, int& made,
                            Result& r)
        {
            std::vector<const projectstore::Version*> due;
            for (const projectstore::Version& v : p.versions)
                if (v.number > committed)
                    due.push_back(&v);
            const juce::File folder(str(c.folder));
            for (std::size_t i = 0; i < due.size(); ++i)
            {
                if (cancelled(s))
                {
                    r = failed("Cancelled.");
                    r.cancelled = true;
                    return false;
                }
                const projectstore::Version& v = *due[i];
                say(s, "Committing version " + std::to_string(v.number) + " (" + std::to_string(i + 1) + " of "
                           + std::to_string(due.size()) + ")");
                std::string why;
                if (!projectexport::writeTree(p, v.number, folder, &why))
                {
                    r = failed("Cannot write version " + std::to_string(v.number) + " into " + c.folder + ": " + why);
                    return false;
                }
                const git::Outcome o = git::commitAll(c, messageOf(v.number, v.note), v.savedMs,
                                                      { { kTrailer, std::to_string(v.number) } }, nullptr, true);
                if (!o.ok)
                {
                    r = gitFailed(o, "Version " + std::to_string(v.number) + " was not committed");
                    return false;
                }
                committed = v.number;
                ++made;
            }
            return true;
        }

        // The link's commits and HEAD, as git has them now.
        void readLog(const git::Context& c, projectstore::Link& k)
        {
            std::vector<git::Commit> commits;
            if (git::log(c, static_cast<int>(projectstore::kMaxLog), commits).ok)
            {
                k.log.clear();
                for (const git::Commit& g : commits)
                    k.log.push_back({ g.sha, g.subject, g.timeMs });
            }
            std::string sha;
            if (git::head(c, sha).ok)
                k.commit = sha;
        }

        std::string sha7(const std::string& sha) { return sha.substr(0, 7); }

        // The tree read and saved as a new version of `name` (with its presets and info), `lead` before its note.
        std::optional<int> adopt(const std::string& name, const projectstore::Project& tree, const std::string& lead, std::string& why)
        {
            const projectstore::Version& v = tree.latest();
            auto set = std::make_shared<assets::Set>();
            for (const projectstore::SampleRef& r : v.samples)
                if (const projectstore::SampleFile* f = tree.sample(r.hash))
                    set->items.push_back(std::make_shared<const assets::Asset>(r.name, f->bytes, assets::Audio{}));
            const std::optional<projectstore::Saved> saved = projectstore::saveVersion(
                name, v.files, v.active, v.dials, v.circuits, { v.midi, false }, set.get(), &why, lead);
            if (!saved)
                return std::nullopt;
            // Its presets: one it lacks, or of other dials, saved; one only this side has stays (nothing is deleted).
            if (const std::optional<projectstore::Project> now = projectstore::load(name, &why))
            {
                for (const projectstore::Preset& pr : tree.presets)
                {
                    const projectstore::Preset* mine = now->preset(pr.name);
                    if (mine == nullptr || mine->dials != pr.dials)
                        projectstore::savePreset(name, pr.name, pr.dials, saved->number, &why);
                    if ((mine == nullptr || mine->category != pr.category) && !pr.category.empty())
                        projectstore::setPresetCategory(name, pr.name, pr.category, &why);
                }
                if (!(now->info == tree.info) && !tree.info.empty())
                    projectstore::setInfo(name, tree.info, &why);
            }
            return saved->number;
        }

        int uncommitted(const projectstore::Project& p, int committed)
        {
            return static_cast<int>(std::count_if(p.versions.begin(), p.versions.end(),
                                                  [&](const projectstore::Version& v) { return v.number > committed; }));
        }

        // The remote's branch as last fetched: its commit.
        std::string theirSha(const git::Context& c, const std::string& branch)
        {
            const git::Outcome o = git::call(c, { "rev-parse", "--verify", "-q", "refs/remotes/origin/" + branch });
            std::string sha = o.ok ? o.out : std::string();
            while (!sha.empty() && (sha.back() == '\n' || sha.back() == '\r'))
                sha.pop_back();
            return sha;
        }

        // Their tip (the remote's branch as last fetched) read as a project: checked out beside the working folder, read,
        // and the checkout taken away again. None, with why, when it holds no project of this plugin's.
        std::optional<projectstore::Project> readTheirs(const git::Context& c, const projectstore::Link& k, std::string& why)
        {
            const juce::File root = juce::File(str(k.folder)).getParentDirectory();
            const juce::File there = root.getNonexistentChildFile(".theirs", "", false);
            const git::Outcome o = git::checkoutInto(c, "origin", k.branch, there.getFullPathName().toStdString());
            if (!o.ok)
            {
                why = wordsOf(o);
                there.deleteRecursively();                // what the checkout made, if anything
                return std::nullopt;
            }
            std::optional<projectstore::Project> p = projectexport::readTree(there, &why);
            git::worktreeRemove(c, there.getFullPathName().toStdString());
            there.deleteRecursively();
            return p;
        }

        // A settled link: what git says now, HEAD's commits and the counts as last fetched, no longer diverged.
        void settled(const git::Context& c, projectstore::Link& k, int committed)
        {
            k.committed = committed;
            k.diverged = false;
            readLog(c, k);
            git::Counts n;
            if (git::counts(c, "origin", k.branch, n).ok)
            {
                k.ahead = n.ahead;
                k.behind = n.behind;
            }
            projectstore::setLink(k, nullptr);
        }

        // keepMine and takeTheirs: the remote fetched again, the versions not yet committed committed, their tip read.
        struct Settle
        {
            Linked l;
            projectstore::Link k;
            git::Context c;
            std::string sha;
            projectstore::Project theirs;
        };
        std::optional<Settle> prepareSettle(const Setup& s, const std::string& project, Result& r)
        {
            std::string why;
            std::optional<Linked> l = linked(project, why, true);
            if (!l)
            {
                r = failed(why);
                return std::nullopt;
            }
            Settle st{ std::move(*l), {}, {}, {}, {} };
            st.k = *st.l.link;
            st.c = contextOf(s, st.k.folder);
            say(s, "Fetching from " + shortRemote(st.k.remote));
            if (const git::Outcome o = git::fetch(st.c, "origin", st.k.branch); !o.ok)
            {
                r = gitFailed(o, "Nothing was changed");
                return std::nullopt;
            }
            git::Counts n;
            if (const git::Outcome o = git::counts(st.c, "origin", st.k.branch, n); !o.ok)
            {
                r = gitFailed(o, "Nothing was changed");
                return std::nullopt;
            }
            if (n.behind == 0)
            {
                st.k.diverged = false;
                projectstore::setLink(st.k, nullptr);
                r = failed("There is nothing to settle: " + shortRemote(st.k.remote) + " has nothing this project lacks.");
                return std::nullopt;
            }
            int made = 0;
            if (!commitVersions(s, st.c, st.l.project, st.k.committed, made, r))
                return std::nullopt;
            if (made > 0)
                projectstore::setLink(st.k, nullptr);
            st.sha = theirSha(st.c, st.k.branch);
            say(s, "Reading theirs (" + sha7(st.sha) + ")");
            std::optional<projectstore::Project> t = readTheirs(st.c, st.k, why);
            if (!t)
            {
                r = failed("Their side is not a project of this plugin's: " + why + " Nothing was changed.");
                return std::nullopt;
            }
            st.theirs = std::move(*t);
            return st;
        }
    }

    std::string messageOf(int number, const std::string& note)
    {
        return "Version " + std::to_string(number) + (note.empty() ? std::string() : ": " + note);
    }

    std::string shortRemote(const std::string& address)
    {
        std::string a = address;
        for (const char* scheme : { "https://", "http://", "ssh://", "git://" })
            if (a.rfind(scheme, 0) == 0)
            {
                a = a.substr(std::char_traits<char>::length(scheme));
                break;
            }
        if (a.rfind("file://", 0) == 0)
            return a.substr(7);
        if (const std::size_t at = a.find('@'); at != std::string::npos && at < a.find('/'))
            a = a.substr(at + 1);
        if (const std::size_t colon = a.find(':'); colon != std::string::npos && colon < a.find('/'))
        {
            // host:path (scp's form), or host:port/path
            std::string rest = a.substr(colon + 1);
            const std::size_t slash = rest.find('/');
            const bool port = slash != std::string::npos && slash > 0
                           && std::all_of(rest.begin(), rest.begin() + static_cast<std::ptrdiff_t>(slash), [](char ch) { return ch >= '0' && ch <= '9'; });
            a = a.substr(0, colon) + "/" + (port ? rest.substr(slash + 1) : rest);
        }
        if (a.size() > 4 && a.compare(a.size() - 4, 4, ".git") == 0)
            a.resize(a.size() - 4);
        while (!a.empty() && a.back() == '/')
            a.pop_back();
        return a;
    }

    juce::File folderFor(const std::string& project, const std::string& id)
    {
        const juce::File root = projectstore::directory().getChildFile(kGitFolder);
        std::string base = projectstore::tidyName(ownName(project));
        if (base.empty())
            base = "Project";
        base += "-" + id.substr(0, 8);
        juce::File f = root.getChildFile(str(base));
        for (int k = 2; f.exists() && k < 1000; ++k)
            f = root.getChildFile(str(base + " " + std::to_string(k)));
        return f;
    }

    Result connect(const Setup& s, const std::string& project, const std::string& remote, const std::string& branchGiven)
    {
        const std::string branch = branchGiven.empty() ? std::string("main") : branchGiven;
        if (const std::string w = git::addressRefusal(remote, s.allowFile); !w.empty())
            return failed("That address cannot be used: " + w);
        if (const std::string w = git::branchRefusal(branch); !w.empty())
            return failed("That branch's name cannot be used: " + w);
        std::string why;
        std::optional<Linked> l = linked(project, why, false);
        if (!l)
            return failed(why);
        if (l->link)
            return failed("\"" + l->project.name + "\" is linked to " + shortRemote(l->link->remote) + " already: Disconnect it first.");
        const juce::File folder = folderFor(l->project.name, l->id);
        const git::Context c = contextOf(s, folder.getFullPathName().toStdString());
        // The folder this call makes, and takes away again when the remote refuses (it holds nothing but its own work).
        const auto undo = [&folder] { folder.deleteRecursively(); };
        if (!folder.getParentDirectory().isDirectory() && !folder.getParentDirectory().createDirectory())
            return failed("Cannot make the folder " + folder.getParentDirectory().getFullPathName().toStdString());
        say(s, "Making the repository");
        if (const git::Outcome o = git::init(c, branch); !o.ok)
        {
            undo();
            return gitFailed(o, "The repository was not made");
        }
        if (const git::Outcome o = git::addRemote(c, "origin", remote); !o.ok)
        {
            undo();
            return gitFailed(o, "The remote was not added");
        }
        say(s, "Looking at " + shortRemote(remote));
        bool there = false;
        if (const git::Outcome o = git::remoteHas(c, "origin", branch, there); !o.ok)
        {
            undo();
            return gitFailed(o, "The remote could not be reached");
        }
        if (there)
        {
            undo();
            return failed("This repository is not empty (its branch " + branch + " has commits). Use Add from Git to make a "
                          "project of it, or give an empty repository.");
        }
        int committed = 0, made = 0;
        Result r;
        if (!commitVersions(s, c, l->project, committed, made, r))
        {
            undo();
            return r;
        }
        projectstore::Link k;
        k.id = l->id;
        k.folder = folder.getFullPathName().toStdString();
        k.remote = remote;
        k.branch = branch;
        k.committed = committed;
        readLog(c, k);
        say(s, "Pushing to " + shortRemote(remote));
        const git::Outcome pushed = git::push(c, "origin", branch, true);
        const int64_t now = juce::Time::currentTimeMillis();
        if (pushed.ok)
        {
            k.syncedMs = k.checkedMs = now;
            k.ahead = k.behind = 0;
        }
        else
            k.ahead = made;
        if (!projectstore::setLink(k, &why))
            return failed("The repository is made in " + k.folder + ", but the link was not kept: " + why);
        if (!pushed.ok)
        {
            Result f = gitFailed(pushed, "Linked, with " + plural(made, "version", "versions") + " committed in " + k.folder
                                             + ", but the push failed (press Push to try again)");
            f.project = l->project.name;
            return f;
        }
        r = {};
        r.ok = true;
        r.project = l->project.name;
        r.line = "Linked to " + shortRemote(remote) + " (" + branch + "): " + plural(made, "version", "versions") + " pushed.";
        return r;
    }

    Result push(const Setup& s, const std::string& project)
    {
        std::string why;
        std::optional<Linked> l = linked(project, why, true);
        if (!l)
            return failed(why);
        projectstore::Link k = *l->link;
        const git::Context c = contextOf(s, k.folder);
        int made = 0;
        Result r;
        const bool committedAll = commitVersions(s, c, l->project, k.committed, made, r);
        readLog(c, k);
        if (made > 0)
            projectstore::setLink(k, nullptr);           // what was committed stays known, whatever the push does
        if (!committedAll)
            return r;
        say(s, "Pushing to " + shortRemote(k.remote));
        const git::Outcome o = git::push(c, "origin", k.branch);
        if (!o.ok && o.behind)
        {
            // The remote moved: how far, from a fetch (the user pressed Push: it reaches the remote anyway).
            if (git::fetch(c, "origin", k.branch).ok)
            {
                git::Counts n;
                if (git::counts(c, "origin", k.branch, n).ok)
                {
                    k.ahead = n.ahead;
                    k.behind = n.behind;
                    k.checkedMs = juce::Time::currentTimeMillis();
                }
            }
            projectstore::setLink(k, nullptr);
            Result b = failed("The remote has commits this project does not have yet: pull first. Nothing was pushed"
                              + std::string(made > 0 ? " (the new versions are committed here)." : "."));
            b.behind = true;
            b.project = l->project.name;
            return b;
        }
        if (!o.ok)
            return gitFailed(o, "Nothing was pushed");
        k.syncedMs = k.checkedMs = juce::Time::currentTimeMillis();
        k.ahead = k.behind = 0;
        if (!projectstore::setLink(k, &why))
            return failed("Pushed, but the link was not kept: " + why);
        r = {};
        r.ok = true;
        r.project = l->project.name;
        r.line = made == 0 ? "Pushed: " + shortRemote(k.remote) + " has every version."
                           : "Pushed " + plural(made, "version", "versions") + " to " + shortRemote(k.remote) + ".";
        return r;
    }

    Result pull(const Setup& s, const std::string& project)
    {
        std::string why;
        std::optional<Linked> l = linked(project, why, true);
        if (!l)
            return failed(why);
        projectstore::Link k = *l->link;
        const git::Context c = contextOf(s, k.folder);
        say(s, "Fetching from " + shortRemote(k.remote));
        if (const git::Outcome o = git::fetch(c, "origin", k.branch); !o.ok)
            return gitFailed(o, "Nothing was pulled");
        git::Counts n;
        if (const git::Outcome o = git::counts(c, "origin", k.branch, n); !o.ok)
            return gitFailed(o, "Nothing was pulled");
        const int mine = uncommitted(l->project, k.committed);
        k.checkedMs = juce::Time::currentTimeMillis();
        k.ahead = n.ahead;
        k.behind = n.behind;
        Result r;
        r.project = l->project.name;
        if (n.behind == 0)
        {
            k.diverged = false;
            projectstore::setLink(k, nullptr);
            r.ok = r.nothing = true;
            r.line = "Nothing new on " + shortRemote(k.remote) + "."
                   + (n.ahead + mine > 0 ? " " + plural(n.ahead + mine, "version", "versions") + " to push." : std::string());
            return r;
        }
        if (n.ahead > 0 || mine > 0)
        {
            // Both sides moved: this side's versions committed first, then git's merge, kept only when it is clean and
            // its tree reads as a project; else taken back whole, the link marked diverged for the user to settle.
            int made = 0;
            if (!commitVersions(s, c, l->project, k.committed, made, r))
                return r;
            if (git::Counts after; git::counts(c, "origin", k.branch, after).ok)
            {
                k.ahead = after.ahead;                    // the commits just made count too
                k.behind = after.behind;
            }
            readLog(c, k);
            projectstore::setLink(k, nullptr);
            const std::string sha = theirSha(c, k.branch);
            r.theirs = sha;
            say(s, "Joining " + plural(n.behind, "commit", "commits") + " from " + shortRemote(k.remote));
            bool conflicts = false;
            const git::Outcome merged = git::merge(c, "origin", k.branch, conflicts);
            if (!merged.ok && !conflicts)
                return gitFailed(merged, "Nothing was pulled");
            std::optional<projectstore::Project> tree;
            if (merged.ok)
                tree = projectexport::readTree(juce::File(str(k.folder)), &why);
            if (!tree)
            {
                git::mergeAbort(c);
                k.diverged = true;
                readLog(c, k);
                projectstore::setLink(k, nullptr);
                r.diverged = true;
                r.why = "Both sides have changed: " + shortRemote(k.remote) + " has " + plural(n.behind, "commit", "commits")
                      + " this project lacks, and this project has " + plural(std::max(k.ahead, 0), "commit", "commits")
                      + " the remote lacks, in ways git cannot join. Nothing was changed: Keep mine, Take theirs, or Compare "
                        "them first (each way keeps both sides as versions).";
                return r;
            }
            const std::string message = "Merged with " + sha7(sha);
            if (const git::Outcome o = git::commitAll(c, message, 0, {}); !o.ok)
            {
                git::mergeAbort(c);
                return gitFailed(o, "The merge was not committed; nothing was changed");
            }
            const std::optional<int> version = adopt(l->project.name, *tree, message, why);
            if (!version)
                return failed("Merged in " + k.folder + ", but the new version was not saved: " + why);
            settled(c, k, *version);
            k.syncedMs = k.checkedMs;
            projectstore::setLink(k, nullptr);
            r.ok = r.merged = true;
            r.version = *version;
            r.line = "Joined " + plural(n.behind, "commit", "commits") + " from " + shortRemote(k.remote) + " with this side's "
                   + "changes as version " + std::to_string(*version) + ". Push to send it.";
            return r;
        }
        say(s, "Bringing in " + plural(n.behind, "commit", "commits"));
        if (const git::Outcome o = git::fastForward(c, "origin", k.branch); !o.ok)
            return gitFailed(o, "Nothing was pulled");
        std::optional<projectstore::Project> tree = projectexport::readTree(juce::File(str(k.folder)), &why);
        if (!tree)
        {
            // Pulled from a tree that is no project's (edited by hand): the store is as it was; the repository has moved,
            // so what is committed stays known.
            readLog(c, k);
            projectstore::setLink(k, nullptr);
            return failed("What was pulled is not a project of this plugin's: " + why + " The project is as it was.");
        }
        readLog(c, k);
        const std::string subject = k.log.empty() ? std::string() : k.log.front().subject;
        const std::string lead = "Pulled " + sha7(k.commit) + (subject.empty() ? std::string() : ": " + subject)
                               + (n.behind > 1 ? " and " + plural(n.behind - 1, "more commit", "more commits") : std::string());
        say(s, "Saving the new version");
        const std::optional<int> version = adopt(l->project.name, *tree, lead, why);
        if (!version)
            return failed("Pulled into " + k.folder + ", but the new version was not saved: " + why);
        k.committed = *version;
        k.ahead = k.behind = 0;
        k.diverged = false;
        k.syncedMs = k.checkedMs;
        projectstore::setLink(k, nullptr);
        r.ok = true;
        r.version = *version;
        r.line = "Pulled " + plural(n.behind, "commit", "commits") + " from " + shortRemote(k.remote) + " as version "
               + std::to_string(*version) + ".";
        return r;
    }

    Result check(const Setup& s, const std::string& project)
    {
        std::string why;
        std::optional<Linked> l = linked(project, why, true);
        if (!l)
            return failed(why);
        projectstore::Link k = *l->link;
        const git::Context c = contextOf(s, k.folder);
        say(s, "Fetching from " + shortRemote(k.remote));
        if (const git::Outcome o = git::fetch(c, "origin", k.branch); !o.ok)
            return gitFailed(o, "Not checked");
        git::Counts n;
        if (const git::Outcome o = git::counts(c, "origin", k.branch, n); !o.ok)
            return gitFailed(o, "Not checked");
        k.ahead = n.ahead;
        k.behind = n.behind;
        k.checkedMs = juce::Time::currentTimeMillis();
        readLog(c, k);
        projectstore::setLink(k, nullptr);
        const int ahead = n.ahead + uncommitted(l->project, k.committed);
        Result r;
        r.ok = true;
        r.project = l->project.name;
        r.line = ahead == 0 && n.behind == 0 ? "Up to date with " + shortRemote(k.remote) + "."
               : std::string(ahead > 0 ? plural(ahead, "version", "versions") + " to push" : std::string())
                     + (ahead > 0 && n.behind > 0 ? ", " : "")
                     + (n.behind > 0 ? plural(n.behind, "commit", "commits") + " to pull" : std::string()) + ".";
        return r;
    }

    Result clone(const Setup& s, const std::string& remote, const std::string& branch)
    {
        if (const std::string w = git::addressRefusal(remote, s.allowFile); !w.empty())
            return failed("That address cannot be used: " + w);
        if (!branch.empty())
            if (const std::string w = git::branchRefusal(branch); !w.empty())
                return failed("That branch's name cannot be used: " + w);
        const juce::File root = projectstore::directory().getChildFile(kGitFolder);
        if (!root.isDirectory() && !root.createDirectory())
            return failed("Cannot make the folder " + root.getFullPathName().toStdString());
        const juce::File temp = root.getNonexistentChildFile(".cloning", "", false);
        const git::Context c = contextOf(s, temp.getFullPathName().toStdString());
        say(s, "Cloning " + shortRemote(remote));
        if (const git::Outcome o = git::clone(c, remote, temp.getFullPathName().toStdString(), branch); !o.ok)
        {
            temp.deleteRecursively();                     // what the clone made, if anything
            return gitFailed(o, "Nothing was added");
        }
        std::string why;
        std::optional<projectstore::Project> tree = projectexport::readTree(temp, &why);
        if (!tree)
        {
            temp.deleteRecursively();
            return failed("Nothing was added: the repository does not hold a project of this plugin's (" + why + ").");
        }
        if (tree->name.empty())
            tree->name = shortRemote(remote).substr(shortRemote(remote).rfind('/') + 1);
        say(s, "Adding the project");
        const std::optional<std::string> name = projectstore::add(*tree, &why);
        if (!name)
        {
            temp.deleteRecursively();
            return failed("Nothing was added: " + why);
        }
        const std::string id = projectstore::idOf(*name, &why);
        const juce::File folder = folderFor(*name, id.empty() ? std::string("00000000") : id);
        projectstore::Link k;
        k.id = id;
        k.folder = temp.moveFileTo(folder) ? folder.getFullPathName().toStdString() : temp.getFullPathName().toStdString();
        k.remote = remote;
        const git::Context in = contextOf(s, k.folder);
        k.branch = branch;
        if (k.branch.empty())
        {
            const git::Outcome o = git::call(in, { "rev-parse", "--abbrev-ref", "HEAD" });
            std::string b = o.out;
            while (!b.empty() && (b.back() == '\n' || b.back() == '\r'))
                b.pop_back();
            k.branch = o.ok && git::branchRefusal(b).empty() ? b : std::string("main");
        }
        if (const std::optional<projectstore::Project> p = projectstore::load(*name, &why))
            k.committed = p->latest().number;
        readLog(in, k);
        k.ahead = k.behind = 0;
        k.syncedMs = k.checkedMs = juce::Time::currentTimeMillis();
        Result r;
        r.project = *name;
        r.version = k.committed;
        if (id.empty() || !projectstore::setLink(k, &why))
        {
            r.why = "\"" + *name + "\" was added, but not linked: " + why;
            return r;
        }
        r.ok = true;
        r.line = "Added \"" + *name + "\" from " + shortRemote(remote) + " (" + k.branch + ").";
        return r;
    }

    Result keepMine(const Setup& s, const std::string& project)
    {
        Result r;
        std::optional<Settle> st = prepareSettle(s, project, r);
        if (!st)
            return r;
        std::string why;
        const std::string name = st->l.project.name;
        say(s, "Keeping theirs as a version");
        const std::optional<int> theirs = adopt(name, st->theirs, "Theirs, " + sha7(st->sha), why);
        if (!theirs)
            return failed("Their side was not saved: " + why + " Nothing was changed.");
        say(s, "Joining their history, keeping this side");
        bool conflicts = false;
        if (const git::Outcome o = git::merge(st->c, "origin", st->k.branch, conflicts, true); !o.ok)
        {
            git::mergeAbort(st->c);
            return gitFailed(o, "Their side is version " + std::to_string(*theirs) + ", but the merge was not made");
        }
        const std::string message = "Kept mine over " + sha7(st->sha);
        if (const git::Outcome o = git::commitAll(st->c, message, 0, {}); !o.ok)
        {
            git::mergeAbort(st->c);
            return gitFailed(o, "Their side is version " + std::to_string(*theirs) + ", but the merge was not committed");
        }
        std::optional<projectstore::Project> mine = projectexport::readTree(juce::File(str(st->k.folder)), &why);
        const std::optional<int> top = mine ? adopt(name, *mine, message, why) : std::nullopt;
        if (!top)
            return failed("Merged, but this side was not saved on top again: " + why);
        settled(st->c, st->k, *top);
        r = {};
        r.ok = true;
        r.project = name;
        r.version = *top;
        r.theirs = st->sha;
        r.line = "Kept this side as version " + std::to_string(*top) + "; theirs is version " + std::to_string(*theirs)
               + ". Push to send it.";
        return r;
    }

    Result takeTheirs(const Setup& s, const std::string& project)
    {
        Result r;
        std::optional<Settle> st = prepareSettle(s, project, r);
        if (!st)
            return r;
        std::string why;
        const std::string name = st->l.project.name;
        const int mineVersion = st->l.project.latest().number;
        say(s, "Joining their history, taking their side");
        bool conflicts = false;
        if (const git::Outcome o = git::merge(st->c, "origin", st->k.branch, conflicts, true); !o.ok)
        {
            git::mergeAbort(st->c);
            return gitFailed(o, "Nothing was changed");
        }
        // The merge keeps this side's tree; their tree written over it (what this side's tree wrote taken away).
        if (!projectexport::writeTree(st->theirs, st->theirs.latest().number, juce::File(str(st->k.folder)), &why))
        {
            git::mergeAbort(st->c);
            return failed("Their side could not be written into " + st->k.folder + ": " + why + " Nothing was changed.");
        }
        const std::string message = "Took theirs, " + sha7(st->sha);
        if (const git::Outcome o = git::commitAll(st->c, message, 0, {}); !o.ok)
        {
            git::mergeAbort(st->c);
            return gitFailed(o, "The merge was not committed; nothing was changed");
        }
        const std::optional<int> version = adopt(name, st->theirs, message, why);
        if (!version)
            return failed("Merged, but their side was not saved as a version: " + why);
        settled(st->c, st->k, *version);
        r.ok = true;
        r.project = name;
        r.version = *version;
        r.theirs = st->sha;
        r.line = "Took their side as version " + std::to_string(*version) + "; this side stays version " + std::to_string(mineVersion)
               + ". Push to send it.";
        return r;
    }

    Result theirs(const Setup& s, const std::string& project)
    {
        std::string why;
        std::optional<Linked> l = linked(project, why, true);
        if (!l)
            return failed(why);
        const git::Context c = contextOf(s, l->link->folder);
        Result r;
        r.project = l->project.name;
        r.theirs = theirSha(c, l->link->branch);
        if (r.theirs.empty())
            return failed("Nothing was fetched from " + shortRemote(l->link->remote) + " yet: press Check first.");
        say(s, "Reading theirs (" + sha7(r.theirs) + ")");
        r.theirsProject = readTheirs(c, *l->link, why);
        if (!r.theirsProject)
            return failed("Their side is not a project of this plugin's: " + why);
        r.ok = true;
        r.line = "Their side, " + sha7(r.theirs) + ", against this one.";
        return r;
    }

    Result disconnect(const std::string& project)
    {
        std::string why;
        std::optional<Linked> l = linked(project, why, false);
        if (!l)
            return failed(why);
        if (!l->link)
            return failed("\"" + l->project.name + "\" is not linked to a git repository.");
        if (!projectstore::removeLink(l->id, &why))
            return failed("Not disconnected: " + why);
        Result r;
        r.ok = true;
        r.project = l->project.name;
        r.line = "Disconnected from " + shortRemote(l->link->remote) + ". The working folder stays at " + l->link->folder + ".";
        return r;
    }
}
