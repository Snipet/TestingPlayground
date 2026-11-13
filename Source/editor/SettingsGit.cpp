// Source/editor/SettingsGit.cpp: the Projects page's Git rows (SettingsPages.h; package 05, docs/plan/05-git.md):
// which git the projects' git uses and how it signs in. It reads where each thing lives at each layout: the git the user
// chose (EditorPort::gitPathSet, the store's links file) and what the last git job found (EditorPort::gitState). Git is
// looked for only by a press (Find git here, or a git button on the GIT card): never when the page opens.
#include "editor/SettingsPages.h"

#include "plugin/EditorPort.h"

#include <funkgui/panel/HostServices.h>

#include <functional>
#include <string>

namespace playground::ui::settings
{
    namespace
    {
        uint64_t mixIn(uint64_t h, const std::string& s) { return (h ^ std::hash<std::string>{}(s)) * 0x100000001B3ull; }
    }

    void buildGitRows(Builder& b)
    {
        EditorPort& port = b.ctx.port;
        const GitState s = port.gitState();
        const std::string chosen = port.gitPathSet();
        b.heading("GIT");
        std::string found;
        if (s.busy && s.kind == GitKind::find)
            found = "Looking...";
        else if (!s.looked)
            found = "Not looked for yet";
        else if (!s.gitPath.empty())
            found = "git " + s.gitVersion + " at " + s.gitPath;
        else
            found = "Not found";
        b.row("Git", found);
        b.row("Which", chosen.empty() ? std::string("The usual places") : chosen);
        float x = b.ctrlX;
        const auto pill = [&](int code, const std::string& label, const std::string& title, bool enabled) {
            const float w = Builder::pillW(label, false, false);
            b.button(code, { x, b.y, w, kRowH }, label, title, enabled);
            x += w + 8.0f;
        };
        pill(kGitFind, "Find git", "Find git: look for it now", !s.busy);
        pill(kGitChoose, "Choose git...", "Choose the git program to use", b.ctx.host != nullptr);
        if (!chosen.empty())
            pill(kGitDefault, "Use the usual places", "Look for git in the usual places again", true);
        b.y += kRowStep;
        if (s.looked && s.gitPath.empty() && !s.gitWhy.empty())
            b.notes(s.gitWhy, b.ctrlX, b.ctrlW, Line::Kind::error);
        b.notes("A project can be kept in a git repository of yours (PROJECTS, Git...). Git runs only when you press Connect, "
                "Push, Pull, Check or Add from Git, and signs in its own way: an SSH agent, a credential helper. The plugin "
                "keeps no password. A host that keeps its plugins in a sandbox may not let git run: the Standalone app can "
                "push and pull.",
                b.ctrlX, b.ctrlW);
    }

    void actGitRows(const Control& c, Context& ctx)
    {
        if (c.act != Act::item)
            return;
        std::string why;
        if (c.index == kGitFind)
        {
            GitJob job;
            job.kind = GitKind::find;
            ctx.port.startGit(job, why);
        }
        else if (c.index == kGitDefault)
            ctx.port.setGitPath("", why);
        else if (c.index == kGitChoose && ctx.host != nullptr)
        {
            funkgui::FileRequest request;
            request.mode = funkgui::FileRequest::Mode::open;
            request.title = "Choose git";
            EditorPort* port = &ctx.port;
            ctx.host->chooseFiles(request, [port](const std::vector<std::string>& paths) {
                std::string whyNot;
                if (!paths.empty())
                    port->setGitPath(paths.front(), whyNot);
            });
        }
    }

    uint64_t keyGitRows(const Context& ctx)
    {
        const GitState s = ctx.port.gitState();
        uint64_t h = mixIn(0, ctx.port.gitPathSet());
        h = mixIn(h, s.gitPath + "|" + s.gitVersion + "|" + s.gitWhy);
        return (h ^ (s.busy ? 1u : 0u) ^ (s.looked ? 2u : 0u)) * 0x100000001B3ull;
    }
}
