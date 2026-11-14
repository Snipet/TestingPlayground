// Source/plugin/ProcessorGit.cpp: the Processor's git job (package 05; EditorPort's gitLink, startGit, gitState,
// cancelGit, disconnectGit): one job at a time on a thread of its own (gitThread_), started only by the user's press,
// running plugin/GitProjects.h's operations; never on the audio or the message thread. The message thread reads the
// links file and the store's cache alone (gitLink), never a repository; poll() joins a job that ended and opens a
// version a pull saved when its project is open with no change.
#include "plugin/Processor.h"

#include "fminus/Failure.h"

#include "plugin/GitProjects.h"
#include "plugin/ProjectStore.h"

#include "Product.h"

#include <juce_core/juce_core.h>

#include <cctype>
#include <cstdlib>
#include <exception>

namespace playground
{
    namespace
    {
        std::string whenOf(int64_t ms) { return juce::Time(static_cast<juce::int64>(ms)).formatted("%Y-%m-%d %H:%M").toStdString(); }

        // file:// remotes only under a probe's sandbox (CTest sets <ENV_PREFIX>PREFS_DIR for each, cmake/Probes.cmake).
        bool probeSandbox()
        {
            const char* v = std::getenv((std::string(product::kEnvPrefix) + "PREFS_DIR").c_str());
            return v != nullptr && *v != '\0';
        }
    }

    GitLink Processor::gitLink(const std::string& project) const
    {
        GitLink out;
        if (project.empty() || isFactoryName(project))
            return out;
        std::vector<std::string> unread;
        std::string id;
        for (const projectstore::Entry& e : projectstore::listCached(unread))
            if (projectstore::sameName(e.name, project))
                id = e.id;
        std::vector<int> numbers;
        if (const auto d = projectstore::detailCached(project, unread))
            for (const projectstore::VersionSummary& v : d->versions)
                numbers.push_back(v.number);
        if (!unread.empty())
            reader_->request(unread);
        const std::optional<projectstore::Link> l = projectstore::linkOf(id);
        if (!l)
            return out;
        out.linked = true;
        out.remote = l->remote;
        out.shortRemote = gitprojects::shortRemote(l->remote);
        out.branch = l->branch;
        out.folder = gitprojects::folderOf(*l);
        out.toPush = gitprojects::toPush(*l, numbers);
        out.ahead = l->ahead;
        out.behind = l->behind;
        out.checked = l->checkedMs > 0 ? whenOf(l->checkedMs) : std::string();
        for (const projectstore::LinkCommit& c : l->recent)
            out.recent.push_back({ c.sha, c.subject, whenOf(c.ms) });
        out.diverged = !l->theirs.empty();
        return out;
    }

    bool Processor::startGit(const GitJob& job, std::string& why)
    {
        {
            const std::lock_guard<std::mutex> lock(gitMutex_);
            if (gitState_.busy)
            {
                why = "git is busy with the last thing asked: wait for it, or cancel it";
                return false;
            }
        }
        if (job.op != GitOp::find && job.op != GitOp::add && job.project.empty())
        {
            why = "there is no project to do it for";
            return false;
        }
        if (job.op != GitOp::find && job.op != GitOp::add && isFactoryName(job.project))
        {
            why = "a factory project is not linked to git: save your own copy, and link that";
            return false;
        }
        if (gitThread_.joinable())
            gitThread_.join();                                   // the last one, ended (busy is false)
        {
            const std::lock_guard<std::mutex> lock(gitMutex_);
            gitState_.busy = true;
            gitState_.op = job.op;
            gitState_.project = job.project;
            gitState_.doing = job.op == GitOp::find ? "Looking for git..." : "Starting git...";
            gitState_.ok = gitState_.cancelled = gitState_.behind = gitState_.diverged = false;
            gitState_.words.clear();
            gitState_.added.clear();
        }
        gitCancel_.store(false);
        gitRevision_.fetch_add(1);
        gitThread_ = std::thread([this, job] { runGit(job); });
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
        gitRevision_.fetch_add(1);
    }

    uint64_t Processor::gitPolled() const { return gitRevision_.load() + projectstore::linksRevision(); }

    bool Processor::disconnectGit(const std::string& project, std::string& why)
    {
        {
            const std::lock_guard<std::mutex> lock(gitMutex_);
            if (gitState_.busy)
            {
                why = "git is busy with the last thing asked: wait for it, or cancel it";
                return false;
            }
        }
        const gitprojects::Outcome o = gitprojects::disconnect(project);
        gitRevision_.fetch_add(1);
        if (!o.ok)
        {
            why = o.words;
            return false;
        }
        why = o.words;                                           // what stays, in words
        return true;
    }

    std::string Processor::gitChosen() const { return projectstore::gitPath(); }

    bool Processor::setGitChosen(const std::string& path, std::string& why)
    {
        const bool ok = projectstore::setGitPath(path, &why);
        gitRevision_.fetch_add(1);
        return ok;
    }

    std::string Processor::gitFolder() const { return projectstore::gitFolder().getFullPathName().toStdString(); }

    // The git thread: nothing here touches the processor but gitState_ and gitOpen_ (under gitMutex_), the store's own
    // functions and git. Whatever its work throws fails the job in words.
    void Processor::runGit(const GitJob& job)
    {
        GitState ended;
        std::pair<std::string, int> open;
        try
        {
            failure::at("git");
            const git::Found found = git::find(projectstore::gitPath());
            ended.looked = true;
            ended.gitPath = found.path;
            ended.gitVersion = found.version;
            ended.gitWhy = found.why;
            gitprojects::Outcome o;
            if (job.op == GitOp::find)
            {
                o.ok = found.found();
                o.words = found.found() ? "Git " + found.version + " at " + found.path + "." : found.why;
            }
            else if (!found.found())
                o.words = found.why;
            else
            {
                gitprojects::Env env;
                env.git = found.path;
                env.cancel = &gitCancel_;
                env.allowFile = probeSandbox();
                env.doing = [this](const std::string& words) {
                    {
                        const std::lock_guard<std::mutex> lock(gitMutex_);
                        gitState_.doing = words;
                    }
                    gitRevision_.fetch_add(1);
                };
                switch (job.op)
                {
                    case GitOp::connect: o = gitprojects::connect(env, job.project, job.remote, job.branch); break;
                    case GitOp::push: o = gitprojects::push(env, job.project); break;
                    case GitOp::pull: o = gitprojects::pull(env, job.project); break;
                    case GitOp::check: o = gitprojects::check(env, job.project); break;
                    case GitOp::add: o = gitprojects::add(env, job.remote, job.branch); break;
                    case GitOp::find: break;
                }
                if (o.cancelled || gitCancel_.load())
                {
                    o.ok = false;
                    o.cancelled = true;
                    if (o.words.empty() || o.words == "Cancelled.")
                        o.words = job.op == GitOp::connect ? "Cancelled: nothing was linked."
                                : job.op == GitOp::add     ? "Cancelled: nothing was added."
                                                           : "Cancelled.";
                }
            }
            ended.ok = o.ok;
            ended.cancelled = o.cancelled;
            ended.behind = o.behind;
            ended.diverged = o.diverged;
            ended.words = o.words;
            ended.added = o.project;
            if (job.op == GitOp::pull && o.ok && o.version > 0)
                open = { job.project, o.version };
        }
        catch (...)
        {
            // fminus/Failure.h's words, as every thread of the plugin says a failure; the card shows a sentence.
            ended = GitState{};
            open = {};
            try
            {
                std::string words = failure::sentence(job.op == GitOp::find      ? "looking for git"
                                                      : job.op == GitOp::connect ? "linking the project to git"
                                                      : job.op == GitOp::push    ? "pushing the project"
                                                      : job.op == GitOp::pull    ? "pulling the project"
                                                      : job.op == GitOp::check   ? "checking the remote"
                                                      : job.op == GitOp::add     ? "adding a project from git"
                                                                                 : "running git");
                if (!words.empty())
                    words[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(words[0])));
                ended.words = words + ".";
            }
            catch (...)
            {
                ended.words.clear();
            }
        }
        {
            const std::lock_guard<std::mutex> lock(gitMutex_);
            gitState_.ok = ended.ok;
            gitState_.cancelled = ended.cancelled;
            gitState_.behind = ended.behind;
            gitState_.diverged = ended.diverged;
            gitState_.words = ended.words;
            gitState_.added = ended.added;
            gitState_.looked = ended.looked || gitState_.looked;
            if (ended.looked)
            {
                gitState_.gitPath = ended.gitPath;
                gitState_.gitVersion = ended.gitVersion;
                gitState_.gitWhy = ended.gitWhy;
            }
            gitState_.doing.clear();
            gitOpen_ = open;
            gitState_.busy = false;
            ++gitState_.ended;
        }
        gitRevision_.fetch_add(1);
    }

    // poll()'s: a job that ended has its thread joined; a version a pull saved opens when its project is the one open
    // with no change the editor would lose (else it waits in the versions list).
    void Processor::pollGit()
    {
        std::pair<std::string, int> open;
        {
            const std::lock_guard<std::mutex> lock(gitMutex_);
            if (gitState_.busy)
                return;
            open.swap(gitOpen_);
        }
        if (gitThread_.joinable())
            gitThread_.join();
        if (open.first.empty())
            return;
        const ProjectState st = project();
        if (!projectstore::sameName(st.name, open.first) || st.edited || st.changed || st.dialsEdited)
            return;
        std::string why;
        if (openProject(open.first, open.second, why))
            gitRevision_.fetch_add(1);
    }

    void Processor::finishGit()
    {
        if (!gitThread_.joinable())
            return;
        cancelGit();
        gitThread_.join();
    }
}
