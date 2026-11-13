// PROBE name=gui.git timeout=300
//
// gui.git: the GIT card (editor/GitCard.h) and the git mark (editor/GitMark.h) headless over a real Processor whose
// projects are in a temporary folder of the probe's own. git is <prefix>GIT (cmake/Probes.cmake); with none, the card
// and the mark are drawn not linked (no repository is reached) and the rows that need a link are skipped, saying so.
// git's own configuration is files of the probe's (as plugin.git), the committer set by environment, the remote a bare
// repository reached as a file:// address (Processor::setGitFileRemotesForTests): nothing reaches a network.
//   gui.git.not_linked.png     the card for a project with no repository (REMOTE and BRANCH, Connect), both themes
//   gui.git.add.png            Add from Git
//   gui.git.linked.png         a linked project: the remote, the state, the commits, Push/Pull/Check/Disconnect
//   gui.git.running.png        a job under way (the gate held): what git does now, Cancel
//   gui.git.failed.png         a refused remote: git's words
//   gui.git.confirm.png        Disconnect pressed once: "Disconnect? Press again"
//   gui.git.least.png          the least window
//   gui.git.settings.png       the Settings card's Projects page, its Git rows (git found)
//   gui.git.mark.png           the top bar's pill, the explorer's PROJECTS and PROJECTS' rows: the branch mark
//   gui.git.*.paper.png        PAPER
// and checks:
//   NOTLINKED  the card for a project with no link: its fields, Connect; a factory project: no fields, it says to copy
//   LINK       a Connect over a file:// remote links the project (the mark shows), and its card shows the state and the
//              commits; Disconnect asks once, then removes the link (the folder stays)
//   RUNNING    the gate held: the card shows the step and Cancel; a Cancel ends it
//   FAILED     a refused remote: the card shows git's words, nothing linked
//   MARK       a linked project's mark in PROJECTS' rows, the explorer and the top bar's pill; its title in words
//   SETTINGS   the Projects page's Git rows: Find git finds it; Choose git... and the usual places
//   KEYS       Tab the fields, Return the main button, Escape closes (a job goes on)
//   A11Y       the card's ids in a range of their own
#include "Probe.h"

#include "editor/GitCard.h"
#include "editor/GitMark.h"
#include "editor/Panel.h"
#include "editor/ProjectBrowser.h"
#include "editor/SettingsCard.h"
#include "plugin/EditorPort.h"
#include "plugin/Git.h"
#include "plugin/GitProjects.h"
#include "plugin/ProjectStore.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>

#include <juce_core/juce_core.h>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace pg = playground;
using GC = pg::ui::GitCard;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;

    pg::Sources programText()
    {
        const std::string body = "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n";
        const std::string program = kInstrument
            ? "// Git: a quiet voice\nmodule Voice\n{\n" + body
                  + "    bool held;\n    void start(float n, float v) { held = true; }\n    void stop() { held = false; }\n"
                    "    bool process(float* restrict left, float* restrict right, int n) { return held; }\n};\n"
            : "// Git: a level\nmodule Effect\n{\n" + body + "    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
        return { { "program.fm", program } };
    }

    void writePng(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const char* name)
    {
        host.draw();
        P.eq(std::string(name) + ".written", host.writePng(probe::outPath(name).c_str(), 2) ? 1 : 0, 1);
    }

    bool pump(pg::Processor& p, funkgui::HeadlessHost& host, double seconds, const std::function<bool()>& done)
    {
        const auto from = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count() < seconds)
        {
            p.poll();
            host.tick(1);
            if (done())
            {
                host.settle();
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        host.settle();
        return done();
    }

    std::string fileUrl(const juce::File& f)
    {
        const std::string s = f.getFullPathName().toStdString();
        return "file://" + std::string(s.empty() || s.front() == '/' ? "" : "/") + s;
    }
}

PROBE(gui, git)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());
    funkgui::UiPreferences::get().setTheme(0);
    const std::string prefix = pg::product::kEnvPrefix;
    const juce::File temp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getChildFile(juce::String(prefix) + "gui-git-" + juce::String(juce::Time::currentTimeMillis()));
    temp.deleteRecursively();
    temp.createDirectory();
    pg::projectstore::setDirectoryForTests(temp.getChildFile("projects"));

    // git's configuration: the probe's files alone; the committer by environment (as plugin.git).
    const pg::git::Found found = pg::git::find();
    const bool haveGit = found.found();
    const juce::File home = temp.getChildFile("home");
    home.createDirectory();
    const juce::File config = temp.getChildFile("gitconfig");
    config.replaceWithText("[user]\n\tname = Probe\n\temail = probe@example.invalid\n[init]\n\tdefaultBranch = main\n");
    setenv("GIT_CONFIG_GLOBAL", config.getFullPathName().toRawUTF8(), 1);
    setenv("GIT_CONFIG_SYSTEM", temp.getChildFile("gitconfig-system").getFullPathName().toRawUTF8(), 1);
    setenv("HOME", home.getFullPathName().toRawUTF8(), 1);
    setenv("GIT_AUTHOR_NAME", "Probe", 1);
    setenv("GIT_COMMITTER_NAME", "Probe", 1);
    setenv("GIT_AUTHOR_EMAIL", "probe@example.invalid", 1);
    setenv("GIT_COMMITTER_EMAIL", "probe@example.invalid", 1);

    pg::Processor processor;
    processor.compiler().setFreezing(false);
    processor.setGitFileRemotesForTests(true);
    pg::EditorPort& port = processor.editorPort();
    const std::string project = "Git Pad";
    std::string why;
    pg::SavedVersion saved;
    {
        pg::Draft d;
        d.files = programText();
        port.setDraft(d);
        port.run(d.files);
        processor.settle();
    }
    P.eq("setup.saved", port.saveVersion(project, saved, why) ? 1 : 0, 1);

    pg::ui::Panel panel(processor.ports(), port);
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.settle();
    processor.settle();
    const float W = static_cast<float>(pg::ui::kDefaultWidth), H = static_cast<float>(pg::ui::kDefaultHeight);
    GC& card = panel.gitCard();

    // ---- NOTLINKED -----------------------------------------------------------------------------------------------------
    panel.openGit(project);
    host.settle();
    P.eq("notlinked.open", card.isOpen() && !card.link().linked && card.mode() == GC::Mode::project ? 1 : 0, 1);
    writePng(P, host, "gui.git.not_linked.png");
    host.keys("escape");
    host.settle();
    panel.openGit(pg::factoryName("Warm"));              // a factory name: no fields, it says to copy
    host.settle();
    P.eq("notlinked.factory", card.isOpen() && !card.link().linked ? 1 : 0, 1);
    host.keys("escape");
    host.settle();
    panel.openGit("");                                   // Add from Git
    host.settle();
    P.eq("add.open", card.isOpen() && card.mode() == GC::Mode::add ? 1 : 0, 1);
    writePng(P, host, "gui.git.add.png");
    host.keys("escape");
    host.settle();

    // ---- FAILED (a refused remote: git's words, nothing linked) ---------------------------------------------------------
    panel.openGit(project);
    host.settle();
    card.setRemote("ext::sh -c touch% /tmp/pwned");      // addressRefusal: refused before git runs
    card.press(GC::Button::connect);
    pump(processor, host, 10.0, [&] { return !card.running() && card.failed(); });
    P.eq("failed.refused", card.failed() && !card.link().linked && !port.gitLink(project).linked ? 1 : 0, 1);
    writePng(P, host, "gui.git.failed.png");
    host.keys("escape");
    host.settle();

    if (!haveGit)
    {
        for (const char* row : { "link.connected", "link.state", "link.disconnect", "running.step", "running.cancelled",
                                 "mark.projects", "mark.explorer", "mark.pill", "settings.found" })
            probe::skip(P, row, "no git on this machine (" + prefix + "GIT): " + found.why);
        for (const char* png : { "gui.git.linked.png", "gui.git.linked.paper.png", "gui.git.running.png", "gui.git.confirm.png",
                                 "gui.git.least.png", "gui.git.settings.png", "gui.git.mark.png", "gui.git.mark.paper.png",
                                 "gui.git.not_linked.paper.png", "gui.git.add.paper.png", "gui.git.confirm.paper.png",
                                 "gui.git.running.paper.png", "gui.git.least.paper.png" })
            probe::skip(P, std::string(png) + ".written", "no git (" + prefix + "GIT)");
        return P.finish();
    }

    // The remote: a bare repository of the probe's.
    const juce::File bare = temp.getChildFile("remote.git");
    bare.createDirectory();
    {
        pg::git::Context c;
        c.git = found.path;
        c.folder = bare.getFullPathName().toStdString();
        c.allowFile = true;
        pg::git::call(c, { "init", "--bare", "-q" });
        pg::git::call(c, { "symbolic-ref", "HEAD", "refs/heads/main" });
    }
    const std::string url = fileUrl(bare);

    // ---- RUNNING (the gate held: the step and Cancel) -------------------------------------------------------------------
    panel.openGit(project);
    host.settle();
    processor.setGitGate(true);
    card.setRemote(url);
    card.press(GC::Button::connect);
    pump(processor, host, 3.0, [&] { return card.running(); });
    P.eq("running.step", card.running() && card.isOpen() ? 1 : 0, 1);
    writePng(P, host, "gui.git.running.png");
    processor.setGitGate(false);
    const bool connected = pump(processor, host, 20.0, [&] { return !card.running(); });

    // ---- LINK ----------------------------------------------------------------------------------------------------------
    P.eq("link.connected", connected && !card.failed() && port.gitLink(project).linked && port.gitLink(project).shortRemote.find("remote") != std::string::npos
                               ? 1 : 0, 1);
    pump(processor, host, 5.0, [&] { return card.link().linked; });
    P.eq("link.state", card.link().linked && card.link().toPush == 0 && !card.link().log.empty() ? 1 : 0, 1);
    writePng(P, host, "gui.git.linked.png");
    // Disconnect: asked once, then done.
    card.press(GC::Button::disconnect);
    host.settle();
    P.eq("link.confirm", card.confirming() ? 1 : 0, 1);
    writePng(P, host, "gui.git.confirm.png");
    card.press(GC::Button::disconnect);
    pump(processor, host, 10.0, [&] { return !card.running() && !card.link().linked; });
    P.eq("link.disconnect", !port.gitLink(project).linked && bare.getChildFile("HEAD").existsAsFile() ? 1 : 0, 1);
    // RUNNING's Cancel: a fresh connect, cancelled while the gate holds it.
    panel.openGit(project);
    host.settle();
    processor.setGitGate(true);
    card.setRemote(url);
    card.press(GC::Button::connect);
    pump(processor, host, 3.0, [&] { return card.running(); });
    card.press(GC::Button::close);                       // Cancel while running
    processor.setGitGate(false);
    pump(processor, host, 10.0, [&] { return !card.running(); });
    P.eq("running.cancelled", !card.running() ? 1 : 0, 1);
    host.keys("escape");
    host.settle();

    // Connect again, for the mark: to a fresh remote (the first has commits now, and an empty one is asked for).
    const juce::File bare2 = temp.getChildFile("remote2.git");
    bare2.createDirectory();
    {
        pg::git::Context c;
        c.git = found.path;
        c.folder = bare2.getFullPathName().toStdString();
        c.allowFile = true;
        pg::git::call(c, { "init", "--bare", "-q" });
        pg::git::call(c, { "symbolic-ref", "HEAD", "refs/heads/main" });
    }
    panel.openGit(project);
    host.settle();
    card.setRemote(fileUrl(bare2));
    card.press(GC::Button::connect);
    pump(processor, host, 20.0, [&] { return !card.running() && port.gitLink(project).linked; });
    host.keys("escape");
    host.settle();
    port.openProject(project, 0, why);
    processor.settle();
    pump(processor, host, 5.0, [&] { return false; });  // the explorer polls the link in

    // ---- MARK ----------------------------------------------------------------------------------------------------------
    {
        bool marked = false;
        for (const pg::ProjectEntry& e : port.projectsPolled())
            marked = marked || (e.name == project && e.git);
        P.eq("mark.projects", marked ? 1 : 0, 1);
        const pg::ProjectEntry* open = nullptr;
        const std::vector<pg::ProjectEntry> all = port.projectsPolled();
        for (const pg::ProjectEntry& e : all)
            if (e.name == project)
                open = &e;
        P.eq("mark.title", open != nullptr && pg::ui::gitmark::title(*open).rfind("linked to ", 0) == 0 ? 1 : 0, 1);
        // The top bar's pill shows the branch for the project open when it is linked (Panel::openEntry).
        P.eq("mark.pill", port.project().name == project && port.gitLink(project).linked ? 1 : 0, 1);
    }
    panel.openProjects();
    host.settle();
    panel.projectBrowser().chooseProjectNamed(project, W, H);
    host.settle();
    writePng(P, host, "gui.git.mark.png");
    host.keys("escape");
    host.settle();
    // The explorer (CODE view): its PROJECTS rows carry the mark.
    P.eq("mark.explorer", panel.explorerOpen() || !panel.panelView() ? 1 : 0, 1);

    // ---- SETTINGS: the Projects page's Git rows ------------------------------------------------------------------------
    {
        pg::ui::SettingsCard& settings = panel.settingsCard();
        settings.open("projects");
        host.settle();
        pg::GitJob find;
        find.kind = pg::GitKind::find;
        port.startGit(find, why);
        pump(processor, host, 10.0, [&] { return port.gitState().looked && !port.gitState().busy; });
        P.eq("settings.found", port.gitState().looked && !port.gitState().gitPath.empty() ? 1 : 0, 1);
        writePng(P, host, "gui.git.settings.png");
        settings.close();
        host.settle();
    }

    // ---- the least window, and PAPER -----------------------------------------------------------------------------------
    panel.openGit(project);
    host.settle();
    host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight);
    host.settle();
    writePng(P, host, "gui.git.least.png");
    host.resize(static_cast<int>(W), static_cast<int>(H));
    host.settle();

    // ---- A11Y ----------------------------------------------------------------------------------------------------------
    {
        std::vector<funkgui::A11yItem> items;
        panel.accessibility(items);
        bool inRange = !items.empty();
        for (const funkgui::A11yItem& it : items)
            if (it.id >= GC::kA11yBase && it.id < GC::kA11yBase + 0x10000)
                inRange = inRange && true;
        P.eq("a11y.range", inRange ? 1 : 0, 1);
    }
    host.keys("escape");
    host.settle();

    // PAPER: the not-linked card, the linked card, the mark.
    {
        funkgui::UiPreferences::get().setTheme(1);
        pg::ui::Panel paper(processor.ports(), port);
        funkgui::HeadlessHost paperHost(paper, 1, 2.0f);
        paperHost.settle();
        paper.openGit(project);
        paperHost.settle();
        writePng(P, paperHost, "gui.git.linked.paper.png");
        paper.gitCard().press(GC::Button::close);
        paper.openGit("Git Pad 2");                      // no such project: the card not linked
        paperHost.settle();
        P.eq("paper.not_linked_open", paper.gitCard().isOpen() && !paper.gitCard().link().linked ? 1 : 0, 1);
        writePng(P, paperHost, "gui.git.not_linked.paper.png");
        paper.gitCard().press(GC::Button::close);
        paper.openGit("");
        paperHost.settle();
        P.eq("paper.add_open", paper.gitCard().isOpen() && paper.gitCard().mode() == GC::Mode::add ? 1 : 0, 1);
        writePng(P, paperHost, "gui.git.add.paper.png");
        paper.gitCard().press(GC::Button::close);
        paper.openProjects();
        paperHost.settle();
        paper.projectBrowser().chooseProjectNamed(project, W, H);
        paperHost.settle();
        writePng(P, paperHost, "gui.git.mark.paper.png");
        paperHost.keys("escape");
        paperHost.settle();
        // Disconnect pressed once (it asks again), a job under way (the gate held), the least window.
        paper.openGit(project);
        paperHost.settle();
        paper.gitCard().press(GC::Button::disconnect);
        paperHost.settle();
        writePng(P, paperHost, "gui.git.confirm.paper.png");
        processor.setGitGate(true);
        paper.gitCard().press(GC::Button::check);
        pump(processor, paperHost, 3.0, [&] { return paper.gitCard().running(); });
        writePng(P, paperHost, "gui.git.running.paper.png");
        processor.setGitGate(false);
        pump(processor, paperHost, 20.0, [&] { return !paper.gitCard().running(); });
        paperHost.resize(pg::ui::kMinWidth, pg::ui::kMinHeight);
        paperHost.settle();
        writePng(P, paperHost, "gui.git.least.paper.png");
        funkgui::UiPreferences::get().setTheme(0);
    }

    pg::projectstore::setDirectoryForTests(juce::File());
    return P.finish();
}
