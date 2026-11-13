// Source/plugin/ProcessorGit.cpp: the Processor's git job (EditorPort::startGit, gitState(), cancelGit(); package 05,
// docs/plan/05-git.md): one job at a time, on a thread of its own (gitter_), never the message or the audio thread. It
// runs only when the user presses (Connect, Push, Pull, Check, Add from Git, Disconnect, Settings' Find git): nothing
// here starts by itself. The thread finds git (plugin/Git.h's find(), the path the user chose first), runs the operation
// (plugin/GitProjects.h) and leaves its result; poll() (pollGit) joins it, opens a pulled version when the project is
// the one open and the editor holds no unsaved change (as PROJECTS' Open does), and only then publishes the outcome, so
// an editor that sees it sees the version opened too.
#include "plugin/Processor.h"

#include "plugin/GitProjects.h"
#include "plugin/ProjectStore.h"

#include <juce_core/juce_core.h>

namespace playground
{
    namespace
    {
        std::string whenOf(int64_t ms) { return juce::Time(static_cast<juce::int64>(ms)).formatted("%Y-%m-%d %H:%M").toStdString(); }
    }

    GitLinkView Processor::gitLink(const std::string& project) const
    {
        GitLinkView v;
        int latest = 0;
        const std::optional<projectstore::Link> k = projectstore::linkCached(project, &latest);
        if (!k)
            return v;
        v.linked = true;
        v.remote = k->remote;
        v.shortRemote = gitprojects::shortRemote(k->remote);
        v.branch = k->branch;
        v.folder = k->folder;
        v.toPush = std::max(0, k->ahead) + std::max(0, latest - k->committed);
        v.behind = k->behind;
        v.checked = k->checkedMs > 0 ? whenOf(k->checkedMs) : std::string();
        v.diverged = k->diverged;
        for (const projectstore::GitCommit& c : k->log)
            v.log.push_back({ c.sha.substr(0, 7), c.subject, whenOf(c.ms) });
        return v;
    }

    bool Processor::startGit(const GitJob& job, std::string& why)
    {
        {
            const std::lock_guard<std::mutex> lock(gitMutex_);
            if (gitState_.busy)
            {
                why = "git is busy with another job";
                return false;
            }
        }
        const bool needsProject = job.kind != GitKind::find && job.kind != GitKind::clone;
        if (needsProject && job.project.empty())
        {
            why = "no project is chosen";
            return false;
        }
        if ((job.kind == GitKind::connect || job.kind == GitKind::clone) && job.remote.empty())
        {
            why = "give the repository's address";
            return false;
        }
        if (gitter_.joinable())
            gitter_.join();                                      // the last one, landed (busy is false)
        {
            const std::lock_guard<std::mutex> lock(gitMutex_);
            GitState s;
            s.busy = true;
            s.ended = gitState_.ended;
            s.kind = job.kind;
            s.project = job.project;
            s.looked = gitState_.looked;
            s.gitPath = gitState_.gitPath;
            s.gitVersion = gitState_.gitVersion;
            s.gitWhy = gitState_.gitWhy;
            s.step = "Starting git";
            gitState_ = s;
            gitDone_ = false;
        }
        gitCancel_.store(false);
        gitter_ = std::thread([this, job] { runGit(job); });
        return true;
    }

    GitState Processor::gitState() const
    {
        const std::lock_guard<std::mutex> lock(gitMutex_);
        return gitState_;
    }

    void Processor::cancelGit()
    {
        gitCancel_.store(true);
        {
            const std::lock_guard<std::mutex> lock(gitMutex_);   // the gate's waiter sees it (setGitGate)
        }
        gitGateOpen_.notify_all();
    }

    void Processor::setGitGate(bool shut)
    {
        {
            const std::lock_guard<std::mutex> lock(gitMutex_);
            gitGateShut_ = shut;
        }
        gitGateOpen_.notify_all();
    }

    std::string Processor::gitPathSet() const { return projectstore::links().git; }

    bool Processor::setGitPath(const std::string& path, std::string& why)
    {
        if (!path.empty() && !juce::File::isAbsolutePath(juce::String::fromUTF8(path.c_str())))
        {
            why = "git's path must be a full one";
            return false;
        }
        return projectstore::setGitPath(path, &why);
    }

    // The git thread: nothing here touches the processor but gitState_ (under gitMutex_) and the store's own functions.
    void Processor::runGit(const GitJob& job)
    {
        {
            std::unique_lock<std::mutex> lock(gitMutex_);
            gitGateOpen_.wait(lock, [this] { return !gitGateShut_ || gitCancel_.load(); });
        }
        gitprojects::Result r;
        const git::Found found = git::find(projectstore::links().git);
        {
            const std::lock_guard<std::mutex> lock(gitMutex_);
            gitState_.looked = true;
            gitState_.gitPath = found.path;
            gitState_.gitVersion = found.version;
            gitState_.gitWhy = found.found() ? std::string() : found.why;
        }
        if (gitCancel_.load())
        {
            r.why = "Cancelled.";
            r.cancelled = true;
        }
        else if (!found.found())
            r.why = found.why.empty() ? git::notFoundWords() : found.why;
        else
        {
            gitprojects::Setup setup;
            setup.git = found.path;
            setup.cancel = &gitCancel_;
            setup.allowFile = gitFiles_;
            setup.step = [this](const std::string& what) {
                const std::lock_guard<std::mutex> lock(gitMutex_);
                gitState_.step = what;
            };
            switch (job.kind)
            {
                case GitKind::find:
                    r.ok = true;
                    r.line = "Git " + found.version + " at " + found.path + ".";
                    break;
                case GitKind::connect: r = gitprojects::connect(setup, job.project, job.remote, job.branch); break;
                case GitKind::push: r = gitprojects::push(setup, job.project); break;
                case GitKind::pull: r = gitprojects::pull(setup, job.project); break;
                case GitKind::check: r = gitprojects::check(setup, job.project); break;
                case GitKind::clone: r = gitprojects::clone(setup, job.remote, job.branch); break;
                case GitKind::disconnect: r = gitprojects::disconnect(job.project); break;
                case GitKind::keepMine: r = gitprojects::keepMine(setup, job.project); break;
                case GitKind::takeTheirs: r = gitprojects::takeTheirs(setup, job.project); break;
                case GitKind::theirs: r = gitprojects::theirs(setup, job.project); break;
            }
        }
        const std::lock_guard<std::mutex> lock(gitMutex_);
        gitState_.ok = r.ok;
        gitState_.cancelled = r.cancelled;
        gitState_.behind = r.behind;
        gitState_.diverged = r.diverged;
        gitState_.nothing = r.nothing;
        gitState_.merged = r.merged;
        gitState_.theirs = r.theirs.substr(0, 7);
        if (job.kind == GitKind::theirs && r.ok && r.theirsProject)
        {
            gitTheirsOf_ = r.project;
            gitTheirsSha_ = r.theirs;
            gitTheirsVersion_ = r.theirsProject->latest();
        }
        gitState_.version = r.version;
        gitState_.line = r.ok ? r.line : std::string();
        gitState_.why = r.ok ? std::string() : r.why;
        if (!r.project.empty())
            gitState_.project = r.project;
        gitState_.step.clear();
        gitDone_ = true;
    }

    // poll()'s: a job whose thread has ended is joined; a pull that saved a version of the project open, while the editor
    // holds no unsaved change, opens it (PROJECTS' Open's path); then the outcome is out.
    void Processor::pollGit()
    {
        if (!gitter_.joinable())
            return;
        {
            const std::lock_guard<std::mutex> lock(gitMutex_);
            if (!gitDone_)
                return;
        }
        gitter_.join();
        GitState s = gitState();
        if ((s.kind == GitKind::pull || s.kind == GitKind::keepMine || s.kind == GitKind::takeTheirs) && s.ok && s.version > 0)
        {
            const ProjectState open = project();
            if (!open.name.empty() && projectstore::sameName(open.name, s.project) && !open.changed && !open.dialsEdited
                && open.version < s.version)
            {
                std::string why;
                s.opened = openProject(s.project, s.version, why);
                if (s.opened)
                    s.line += " Opened it.";
            }
            else if (!open.name.empty() && projectstore::sameName(open.name, s.project))
                s.line += " The editor has unsaved changes: the new version waits in the versions list.";
        }
        {
            const std::lock_guard<std::mutex> lock(gitMutex_);
            gitState_.opened = s.opened;
            gitState_.line = s.line;
            gitState_.busy = false;
            gitState_.finished = true;
            ++gitState_.ended;
            gitDone_ = false;
        }
        gitLanded_.fetch_add(1, std::memory_order_acq_rel);
    }

    void Processor::finishGit()
    {
        if (!gitter_.joinable())
            return;
        cancelGit();
        gitter_.join();
    }
}
