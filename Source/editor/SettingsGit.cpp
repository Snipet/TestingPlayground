// Source/editor/SettingsGit.cpp: the Settings card's Projects page's Git rows (package 05; SettingsPages.h's gitRows):
// the git the plugin runs (the one the user chose, else found as plugin/Git.h finds it), Look for git (git --version
// alone, on the processor's git job: nothing is run until it is pressed), Choose git... and Use the one found, and the
// folder the working folders go in. Each reads where its setting lives (EditorPort::gitState, gitChosen, gitFolder) at
// each layout; the page's key mixes EditorPort::gitPolled, so a look that ends draws again.
#include "editor/SettingsPages.h"

#include "editor/ChatStyle.h"

#include <funkgui/panel/HostServices.h>

namespace playground::ui::settings
{
    void gitRows(Builder& b)
    {
        EditorPort& port = b.ctx.port;
        const GitState st = port.gitState();
        const std::string chosen = port.gitChosen();
        b.heading("GIT");
        b.label("Git");
        std::string now;
        Line::Kind kind = Line::Kind::value;
        if (st.busy && st.op == GitOp::find)
            now = "Looking...";
        else if (!st.looked)
            now = chosen.empty() ? "Not looked for yet" : chosen + " (not looked at yet)";
        else if (!st.gitPath.empty())
            now = "git " + st.gitVersion + " at " + st.gitPath;
        else
        {
            now = st.gitWhy;
            kind = Line::Kind::error;
        }
        b.line(kind, now, { b.ctrlX, b.y, b.ctrlW, kRowH });
        b.y += kRowStep;
        float x = b.ctrlX;
        const auto button = [&](int code, const std::string& word, const std::string& title, bool enabled) {
            const float w = Builder::pillW(word, false, false);
            b.button(code, { x, b.y, w, kRowH }, word, title, enabled);
            x += w + 8.0f;
        };
        button(kLookForGit, "Look for git", "Look for git: runs git --version, nothing else", !st.busy);
        button(kChooseGit, "Choose git...", "Choose the git program to run", true);
        if (!chosen.empty())
            button(kDefaultGit, "Use the one found", "Forget the git chosen: find it as the plugin does", true);
        b.y += kRowStep;
        b.label("Folders");
        b.line(Line::Kind::value, port.gitFolder(), { b.ctrlX, b.y, b.ctrlW, kRowH });
        b.y += kRowStep;
        const std::string word = showFolderWord();
        b.button(kShowGitFolder, { b.ctrlX, b.y, Builder::pillW(word, false, false), kRowH }, word,
                 std::string(word) + ": the git working folders");
        b.y += kRowStep;
        b.notes("A project's Git... links it to a repository of yours. Git runs only when you press Connect, Push, Pull, "
                "Check or Add from Git (and Look for git here), and signs in its own way (an SSH agent, a credential "
                "helper): the plugin keeps no password or token. A host that keeps plugins in a sandbox may not let git "
                "run: the Standalone app can push and pull.",
                b.ctrlX, b.ctrlW);
    }

    bool gitAct(const Control& c, Context& ctx)
    {
        if (c.act != Act::item)
            return false;
        std::string why;
        switch (c.index)
        {
            case kLookForGit: ctx.port.startGit({ GitOp::find, {}, {}, {} }, why); return true;
            case kDefaultGit: ctx.port.setGitChosen({}, why); return true;
            case kShowGitFolder: ctx.port.showPath(ctx.port.gitFolder(), why); return true;
            case kChooseGit:
            {
                if (ctx.host == nullptr)
                    return true;
                funkgui::FileRequest r;
                r.mode = funkgui::FileRequest::Mode::open;
                r.title = "Choose git";
                EditorPort* port = &ctx.port;
                ctx.host->chooseFiles(r, [port](const std::vector<std::string>& paths) {
                    std::string whyNot;
                    if (!paths.empty() && port->setGitChosen(paths.front(), whyNot))
                        port->startGit({ GitOp::find, {}, {}, {} }, whyNot);   // looked at at once: the user chose it
                });
                return true;
            }
            default: return false;
        }
    }

    uint64_t gitKey(const Context& ctx) { return ctx.port.gitPolled(); }
}
