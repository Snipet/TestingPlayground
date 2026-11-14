// PROBE name=gui.git timeout=300
//
// gui.git: GIT's card (editor/GitCard.h, package 05) headless over a real Processor whose projects are in a folder of the
// probe's own, its remotes bare repositories there reached as file:// addresses, git's configuration the probe's files
// (GIT_CONFIG_GLOBAL, GIT_CONFIG_SYSTEM, HOME) and the committer set by environment: nothing reaches a network or the
// user's git. Each theme in turn (graphite, then paper: ".paper" in the names), to <build>/probe-out/<key>/:
//   gui.git.png               the card, not linked: REMOTE and BRANCH, Connect waiting for an address
//   gui.git.filled.png        ... an address typed
//   gui.git.running.png       a Connect under way (a git that takes its time): what it does, Cancel
//   gui.git.cancelled.png     ... cancelled: nothing linked, nothing left
//   gui.git.linked.png        linked: the remote, the state, the folder, the commits, Push, Pull, Check, Show folder
//   gui.git.behind.png        a push refused: the remote moved (pull first), in git's words
//   gui.git.diverged.png      (part 3) both sides changed the same lines: Keep mine, Take theirs, Compare...
//   gui.git.compare.png       ... COMPARE, the newest version against their side (no A/B, no Open for it)
//   gui.git.disconnect.png    the Disconnect question: what stays and what goes
//   gui.git.nogit.png         a chosen git that is none: said
//   gui.git.add.png           Add from Git
//   gui.git.mark.png          the mark after the project's name in the explorer's PROJECTS and the top bar's pill
//   gui.git.projects.png      ... and in PROJECTS' rows, with Git... among its buttons
//   gui.git.settings.png      the Settings card's Projects page: its Git rows
//   gui.git.small.png         the least window, linked (graphite only), and gui.git.small.add.png
// and checks:
//   OPEN     PROJECTS' Git... opens it for the chosen project (none chosen: Add from Git), the pill's menu's Git... too;
//            Escape and a click outside close it
//   CONNECT  Connect waits for an address; Return presses it; the card says what runs, then the outcome; linked after
//   CANCEL   Cancel kills git: nothing linked, the folder it made gone
//   FAIL     a push refused: behind, "pull first", the line in the danger colour's place (failed)
//   DIVERGED (part 3) a pull that cannot merge: the three ways; Compare... opens COMPARE on their side (A/B off); Keep
//            mine settles it and Push sends it; their side's moved dial worded as the panel says it; COMPARE closed brings
//            the Git card back, diverged
//   ASK      Disconnect asks first (its line names the folder that stays), Escape withdraws it, the second click
//            disconnects: the project and its folder left
//   MARK     the explorer's row and the pill carry the mark; their accessibility says "linked to"
//   KEYS     Tab moves between the fields; the card's accessibility: the fields, the buttons, its ids its own
//   NOGIT    a chosen git that is no git: the card says so, nothing else changes
//   WHATSNEW What's new never opens over the card, nor the card over What's new; while it is up a click is its own
#include "Probe.h"

#include "editor/DiffCard.h"
#include "editor/GitCard.h"
#include "editor/Panel.h"
#include "editor/ProjectBrowser.h"
#include "editor/SettingsCard.h"
#include "plugin/EditorPort.h"
#include "plugin/Git.h"
#include "plugin/ProjectStore.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>

#include <juce_core/juce_core.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace pg = playground;
namespace fs = std::filesystem;
using GC = pg::ui::GitCard;

namespace
{
    void writeFile(const fs::path& p, const std::string& text)
    {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary);
        f << text;
    }

    void writePng(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const std::string& name)
    {
        const std::string path = probe::outPath(name);
        host.draw();
        P.eq(name + ".written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
    }

    bool pump(pg::Processor& p, funkgui::HeadlessHost& host, const std::function<bool()>& done)
    {
        const auto until = std::chrono::steady_clock::now() + probe::kWorkWait;
        while (std::chrono::steady_clock::now() < until)
        {
            p.poll();
            host.tick(1);
            if (done())
            {
                host.settle();
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        host.settle();
        return done();
    }

    void clickAt(funkgui::HeadlessHost& host, funkgui::Point p, funkgui::Mods m = {})
    {
        host.click(p.x, p.y, m);
        host.tick(2);
        host.settle();
    }

    funkgui::Mods ctrl()
    {
        funkgui::Mods m;
        m.ctrl = true;
        return m;
    }

    std::string fileUrl(const fs::path& p)
    {
        const std::string s = p.generic_string();
        return "file://" + std::string(s.front() == '/' ? "" : "/") + s;
    }

    std::string readText(const fs::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }

    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    // Text typed a key at a time (HeadlessHost::keys: one character a token, commas between).
    void typeText(funkgui::HeadlessHost& host, const std::string& text)
    {
        std::string spec;
        for (const char ch : text)
            spec += (spec.empty() ? "" : ",") + (ch == ' ' ? std::string("space") : std::string(1, ch));
        host.keys(spec.c_str());
    }
}

PROBE(gui, git)
{
    const std::string prefix = pg::product::kEnvPrefix;
    const char* sandboxVar = std::getenv((prefix + "PREFS_DIR").c_str());
    if (sandboxVar == nullptr || *sandboxVar == '\0')
    {
        probe::skip(P, "git", "no sandbox (" + prefix + "PREFS_DIR): run the probes through ctest");
        return P.finish();
    }
    const pg::git::Found found = pg::git::find();
    if (!found.found())
    {
        probe::skip(P, "git", "no git on this machine (" + prefix + "GIT): " + found.why);
        return P.finish();
    }
    const fs::path root = fs::path(sandboxVar) / "gui-git";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "home", ec);
    writeFile(root / "gitconfig", "[user]\n\tuseConfigOnly = true\n[init]\n\tdefaultBranch = main\n");
    writeFile(root / "gitconfig-system", "");
    setenv("GIT_CONFIG_GLOBAL", (root / "gitconfig").string().c_str(), 1);
    setenv("GIT_CONFIG_SYSTEM", (root / "gitconfig-system").string().c_str(), 1);
    setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
    setenv("HOME", (root / "home").string().c_str(), 1);
    setenv("XDG_CONFIG_HOME", (root / "home" / ".config").string().c_str(), 1);
    for (const char* v : { "GIT_AUTHOR_NAME", "GIT_COMMITTER_NAME" })
        setenv(v, "Probe", 1);
    for (const char* v : { "GIT_AUTHOR_EMAIL", "GIT_COMMITTER_EMAIL" })
        setenv(v, "probe@example.invalid", 1);
    const auto context = [&](const fs::path& folder) {
        pg::git::Context c;
        c.git = found.path;
        c.folder = folder.string();
        c.allowFile = true;
        return c;
    };
    const auto bareRemote = [&](const std::string& name) {
        const fs::path bare = root / "remotes" / name;
        fs::create_directories(bare, ec);
        pg::git::call(context(bare), { "init", "--bare", "-q" });
        pg::git::call(context(bare), { "symbolic-ref", "HEAD", "refs/heads/main" });
        return bare;
    };
#if !defined(_WIN32)
    // A git that takes its time (the running card's picture, the cancel).
    const fs::path slowGit = root / "slow-git";
    writeFile(slowGit, "#!/bin/sh\nsleep 3\nexec '" + found.path + "' \"$@\"\n");
    fs::permissions(slowGit, fs::perms::owner_all, ec);
#endif
    const fs::path notGit = root / "not-git.txt";
    writeFile(notGit, "just text\n");

    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    for (const int theme : { 0, 1 })
    {
        const std::string sfx = theme == 0 ? "" : ".paper";
        const std::string row = theme == 0 ? "" : "paper.";
        funkgui::UiPreferences::get().setTheme(theme);
        const fs::path store = root / ("store" + sfx);
        pg::projectstore::setDirectoryForTests(juce::File(store.string()));
        pg::Processor processor;
        processor.compiler().setFreezing(false);
        pg::EditorPort& port = processor.editorPort();
        std::string why;
        processor.setFolderLauncher([](pg::FolderApp, const std::string&, std::string&) { return true; });
        pg::SavedVersion saved;
        port.saveVersion("Glass Bloom", saved, why);
        pg::Draft d = port.draft();
        d.files.front().text += "// a second version\n";
        port.setDraft(d);
        port.run(d.files);
        processor.settle();
        port.saveVersion("Glass Bloom", saved, why);
        processor.settle();

        pg::ui::Panel panel(processor.ports(), port);
        funkgui::HeadlessHost host(panel, theme, 2.0f);
        host.settle();
        float W = static_cast<float>(pg::ui::kDefaultWidth), H = static_cast<float>(pg::ui::kDefaultHeight);
        GC& card = panel.gitCard();
        const auto settled = [&] { return pump(processor, host, [&] { return !port.gitState().busy && !card.running(); }); };
        const auto press = [&](GC::Button b) { clickAt(host, card.centreOf(b, W, H)); };
        std::string movedDial, movedText;              // part 3: a dial the other machine moved
        float movedValue = 0.0f;

        // ---- OPEN: PROJECTS' Git... with the project chosen ------------------------------------------------------------
        panel.openProjects();
        host.settle();
        pg::ui::ProjectBrowser& browser = panel.projectBrowser();
        pump(processor, host, [&] { return browser.rowOf("Glass Bloom") >= 0; });
        clickAt(host, browser.centreOfRow(pg::ui::ProjectBrowser::List::projects, browser.rowOf("Glass Bloom"), W, H));
        const pg::ui::ProjectBrowser::Places bp = browser.places(W, H);
        std::printf("open: projects open %d, row %d, chosen \"%s\", git at %.0f,%.0f\n", browser.isOpen() ? 1 : 0,
                    browser.rowOf("Glass Bloom"), browser.chosenProject().c_str(), bp.git.centreX(), bp.git.centreY());
        clickAt(host, { bp.git.centreX(), bp.git.centreY() });
        P.eq(row + "open.from_projects", card.isOpen() && card.mode() == GC::Mode::project && card.project() == "Glass Bloom"
                                             && !card.link().linked && !browser.isOpen() ? 1 : 0, 1);
        P.eq(row + "connect.waits_for_address", card.label(GC::Button::primary) == "Connect" && !card.enabled(GC::Button::primary) ? 1 : 0, 1);
        writePng(P, host, "gui.git" + sfx + ".png");

        // ---- KEYS: typing, Tab -------------------------------------------------------------------------------------------
        const fs::path glass = bareRemote("glass" + sfx + ".git");
        std::printf("open: card open %d, mode %d\n", card.isOpen() ? 1 : 0, static_cast<int>(card.mode()));
        const std::string url = fileUrl(glass);
        typeText(host, url);
        host.settle();
        host.keys("tab");
        host.settle();
        P.eq(row + "keys.typed_tab", card.remoteText() == url && card.focus() == GC::Field::branch && card.branchText() == "main" ? 1 : 0, 1);
        writePng(P, host, "gui.git.filled" + sfx + ".png");

        // ---- CANCEL: a git that takes its time ---------------------------------------------------------------------------
#if !defined(_WIN32)
        port.setGitChosen(slowGit.string(), why);
        host.keys("return");                         // Return presses Connect
        host.tick(2);
        host.settle();
        pump(processor, host, [&] { return card.running() && has(port.gitState().doing, "Committing"); });
        P.eq(row + "cancel.running", card.running() && card.label(GC::Button::close) == "Cancel" && !card.enabled(GC::Button::primary) ? 1 : 0, 1);
        writePng(P, host, "gui.git.running" + sfx + ".png");
        press(GC::Button::close);
        settled();
        std::printf("cancel: \"%s\"\n", card.line().c_str());
        const int folders = [&] {
            int n = 0;
            for (const auto& e : fs::directory_iterator(store / "Git", ec))
                n += e.is_directory(ec) ? 1 : 0;
            return n;
        }();
        P.eq(row + "cancel.nothing_left", card.isOpen() && !card.link().linked && has(card.line(), "Cancelled") && folders == 0 ? 1 : 0, 1);
        writePng(P, host, "gui.git.cancelled" + sfx + ".png");
        port.setGitChosen({}, why);
#else
        probe::skip(P, row + "cancel", "the slow git is a shell script (POSIX)");
#endif

        // ---- CONNECT -----------------------------------------------------------------------------------------------------
        press(GC::Button::primary);
        settled();
        std::printf("connect: \"%s\"\n", card.line().c_str());
        P.eq(row + "connect.linked", card.link().linked && card.link().recent.size() == 2 && card.label(GC::Button::primary) == "Push"
                                         && has(card.line(), "Linked") && !card.failed() ? 1 : 0, 1);
        writePng(P, host, "gui.git.linked" + sfx + ".png");

        // ---- FAIL: another machine pushed first ----------------------------------------------------------------------------
        {
            const fs::path other = root / ("other" + sfx);
            pg::git::clone(context(root), url, other.string(), "main");
            writeFile(other / "program.fm", "// from elsewhere\n");
            // Part 3: one of its real dials moved there too (COMPARE words their side's dials as a version's).
            for (int k = 0; k < pg::kMaxControls && movedDial.empty(); ++k)
                if (const pg::Control& ctl = processor.control(k); ctl.used && ctl.type == pg::ControlType::real)
                {
                    std::string json = readText(other / "project.json");
                    const std::string key = "{ \"name\": \"" + std::string(ctl.name) + "\", \"value\": ";
                    if (const std::size_t at = json.find(key); at != std::string::npos)
                    {
                        const std::size_t from = at + key.size(), end = json.find_first_of(",}", from);
                        movedValue = ctl.max;
                        char buf[32];
                        std::snprintf(buf, sizeof buf, "%.9g ", static_cast<double>(movedValue));
                        json.replace(from, end - from, buf);
                        writeFile(other / "project.json", json);
                        movedDial = ctl.name;
                        movedText = pg::dialText(ctl, movedValue);
                    }
                }
            pg::git::commitAll(context(other), "From elsewhere", 0, {});
            pg::git::push(context(other), "origin", "main");
            d = port.draft();
            d.files.front().text += "// a third version\n";
            port.setDraft(d);
            port.run(d.files);
            processor.settle();
            port.saveVersion("Glass Bloom", saved, why);
            processor.settle();
            press(GC::Button::primary);
            settled();
            std::printf("fail: \"%s\"\n", card.line().c_str());
            P.eq(row + "fail.behind", card.failed() && has(card.line(), "pull first") && card.state().behind ? 1 : 0, 1);
            writePng(P, host, "gui.git.behind" + sfx + ".png");
        }

        // ---- DIVERGED (part 3): Pull finds both sides changed the same lines -----------------------------------------------
        press(GC::Button::pull);
        settled();
        std::printf("diverged: \"%s\"\n", card.line().c_str());
        P.eq(row + "diverged.three_ways", card.link().diverged && card.label(GC::Button::primary) == "Keep mine"
                                              && card.label(GC::Button::pull) == "Take theirs" && card.label(GC::Button::check) == "Compare..."
                                              && has(card.line(), "Keep mine") ? 1 : 0, 1);
        {
            const GC::Places dp = card.places(W, H);
            P.eq(row + "diverged.buttons_apart", dp.disconnect.right() + 8.0f <= dp.check.x && dp.check.right() <= dp.pull.x
                                                     && dp.pull.right() <= dp.primary.x ? 1 : 0, 1);
        }
        writePng(P, host, "gui.git.diverged" + sfx + ".png");
        press(GC::Button::check);                     // Compare...: their side read into memory, COMPARE opened on it
        pump(processor, host, [&] { return panel.compareCard().isOpen() && !panel.compareCard().reading(); });
        {
            const pg::ui::DiffCard& dc = panel.compareCard();
            std::vector<funkgui::A11yItem> out;
            dc.accessibility(out, W, H);
            bool abOff = false;
            for (const funkgui::A11yItem& it : out)
                abOff = abOff || (it.title == "A/B" && !it.enabled);
            P.eq(row + "diverged.compare_theirs", dc.isOpen() && dc.side(1) == pg::ui::DiffCard::kTheirs && dc.sideLabel(1) == "Theirs"
                                                      && !card.isOpen() && abOff ? 1 : 0, 1);
            // Their side's moved dial worded as the panel says it, as a version's dials are.
            std::string said;
            for (const pg::ContentDial& cd : port.gitTheirs("Glass Bloom").dials)
                if (cd.name == movedDial)
                    said = cd.text;
            std::printf("diverged: their %s says \"%s\" (the panel's \"%s\")\n", movedDial.c_str(), said.c_str(), movedText.c_str());
            P.eq(row + "diverged.dials_worded", !movedDial.empty() && said == movedText ? 1 : 0, 1);
            writePng(P, host, "gui.git.compare" + sfx + ".png");
        }
        host.keys("escape");                          // COMPARE closed: the Git card back, the choice still owed
        host.settle();
        pump(processor, host, [&] { return card.isOpen(); });
        P.eq(row + "diverged.card_back", card.isOpen() && card.project() == "Glass Bloom" && card.link().diverged
                                             && card.label(GC::Button::primary) == "Keep mine" ? 1 : 0, 1);
        press(GC::Button::primary);                   // Keep mine
        settled();
        std::printf("diverged: keep mine \"%s\"\n", card.line().c_str());
        P.eq(row + "diverged.keep_mine", !card.link().diverged && has(card.line(), "Kept yours") ? 1 : 0, 1);
        press(GC::Button::primary);                   // and Push
        settled();
        std::printf("diverged: pushed \"%s\"\n", card.line().c_str());
        P.eq(row + "diverged.pushed", !card.failed() && has(card.line(), "Pushed") ? 1 : 0, 1);

        // ---- ASK: Disconnect's question ----------------------------------------------------------------------------------
        press(GC::Button::disconnect);
        P.eq(row + "ask.asked", card.asking() && has(card.line(), "stays") && has(card.line(), card.link().folder)
                                    && card.label(GC::Button::disconnect) == "Disconnect it" ? 1 : 0, 1);
        writePng(P, host, "gui.git.disconnect" + sfx + ".png");
        host.keys("escape");
        host.settle();
        P.eq(row + "ask.escape_withdraws", card.isOpen() && !card.asking() && card.link().linked ? 1 : 0, 1);

        // ---- NOGIT -------------------------------------------------------------------------------------------------------
        port.setGitChosen(notGit.string(), why);
        press(GC::Button::check);
        settled();
        std::printf("nogit: \"%s\"\n", card.line().c_str());
        P.eq(row + "nogit.said", card.failed() && (has(card.line(), "not git") || has(card.line(), "could not be run")) && card.link().linked ? 1 : 0, 1);
        writePng(P, host, "gui.git.nogit" + sfx + ".png");
        port.setGitChosen({}, why);

        // ---- A11Y ----------------------------------------------------------------------------------------------------------
        {
            std::vector<funkgui::A11yItem> out;
            card.accessibility(out, W, H);
            bool own = !out.empty(), push = false, commits = false;
            for (const funkgui::A11yItem& it : out)
            {
                own = own && it.id >= GC::kA11yBase && it.id < GC::kA11yBase + 0x10000u;
                push = push || (it.role == funkgui::A11yRole::button && it.title == "Push");
                commits = commits || it.title == "Recent commits";
            }
            P.eq(row + "a11y.items", own && push && commits ? 1 : 0, 1);
        }

        // ---- CLOSE, the mark ----------------------------------------------------------------------------------------------
        host.keys("escape");
        host.settle();
        P.eq(row + "open.escape_closes", card.isOpen() ? 0 : 1, 1);
        pump(processor, host, [&] { return false; });  // the explorer's poll, the marks (a few frames)
        writePng(P, host, "gui.git.mark" + sfx + ".png");
        {
            std::vector<funkgui::A11yItem> out;
            panel.accessibility(out);
            int linked = 0;
            for (const funkgui::A11yItem& it : out)
                linked += has(it.description, "linked to") ? 1 : 0;
            std::printf("mark: %d items say linked\n", linked);
            P.ge(row + "mark.said", static_cast<double>(linked), 2.0);   // the explorer's row and the pill
        }
        panel.openProjects();
        host.settle();
        pump(processor, host, [&] { return browser.rowOf("Glass Bloom") >= 0; });
        clickAt(host, browser.centreOfRow(pg::ui::ProjectBrowser::List::projects, browser.rowOf("Glass Bloom"), W, H));
        writePng(P, host, "gui.git.projects" + sfx + ".png");
        host.keys("escape");
        host.settle();

        // ---- OPEN from the pill's menu; a click outside closes ------------------------------------------------------------
        clickAt(host, panel.centreOfProjects(), ctrl());
        const bool chosen = host.chooseMenuItem("Git...");
        host.tick(2);
        host.settle();
        P.eq(row + "open.from_pill", chosen && card.isOpen() && card.project() == "Glass Bloom" ? 1 : 0, 1);
        clickAt(host, { 4.0f, H - 4.0f });
        P.eq(row + "open.outside_closes", card.isOpen() ? 0 : 1, 1);

        // ---- WHATSNEW: What's new never opens over the card, nor the card over What's new (openGit refuses while any card
        // is up: a menu's late answer); while What's new is up a click is its own (it closes it and goes no further) -----
        panel.openGit("Glass Bloom");
        host.settle();
        panel.openWhatsNew();
        host.tick(2);
        P.eq(row + "whatsnew.not_over_git", card.isOpen() && !panel.whatsNewCard().isOpen() ? 1 : 0, 1);
        host.keys("escape");
        host.settle();
        panel.whatsNewCard().open(std::string());
        host.tick(2);
        clickAt(host, panel.centreOfProjects(), ctrl());
        const bool menuUnder = host.chooseMenuItem("Git...");
        host.tick(2);
        host.settle();
        P.eq(row + "whatsnew.click_its_own", !menuUnder && !card.isOpen() && !panel.whatsNewCard().isOpen() ? 1 : 0, 1);
        panel.whatsNewCard().open(std::string());
        panel.openGit("Glass Bloom");                    // a menu's late answer: refused while any card is up
        host.tick(2);
        P.eq(row + "whatsnew.git_not_over_it", !card.isOpen() && panel.whatsNewCard().isOpen() ? 1 : 0, 1);
        host.keys("escape");
        host.settle();
        P.eq(row + "whatsnew.then_closes", panel.whatsNewCard().isOpen() ? 0 : 1, 1);

        // ---- ASK: the second click disconnects ---------------------------------------------------------------------------
        panel.openGit("Glass Bloom");
        host.settle();
        const std::string folder = card.link().folder;
        press(GC::Button::disconnect);
        press(GC::Button::disconnect);
        P.eq(row + "ask.disconnected", !card.link().linked && fs::is_directory(folder, ec) && pg::projectstore::exists("Glass Bloom") ? 1 : 0, 1);
        host.keys("escape");
        host.settle();

        // ---- ADD ---------------------------------------------------------------------------------------------------------
        panel.openGit("");
        host.settle();
        P.eq(row + "open.add", card.isOpen() && card.mode() == GC::Mode::add && card.label(GC::Button::primary) == "Add" ? 1 : 0, 1);
        writePng(P, host, "gui.git.add" + sfx + ".png");
        card.setFields(url, "");
        press(GC::Button::primary);
        settled();
        std::printf("add: \"%s\"\n", card.line().c_str());
        P.eq(row + "add.linked", card.mode() == GC::Mode::project && card.link().linked && has(card.line(), "Added") ? 1 : 0, 1);
        host.keys("escape");
        host.settle();

        // ---- SETTINGS: the Projects page's Git rows -----------------------------------------------------------------------
        panel.openSettings("projects");
        host.settle();
        port.startGit({ pg::GitOp::find, {}, {}, {} }, why);
        pump(processor, host, [&] { return !port.gitState().busy; });
        writePng(P, host, "gui.git.settings" + sfx + ".png");
        panel.settingsCard().close();
        host.settle();

        // ---- The least window ---------------------------------------------------------------------------------------------
        if (theme == 0)
        {
            const bool least = host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight);
            host.settle();
            W = static_cast<float>(pg::ui::kMinWidth);
            H = static_cast<float>(pg::ui::kMinHeight);
            panel.openGit(card.project());
            host.settle();
            const GC::Places at = card.places(W, H);
            P.eq("small.fits", least && at.box.x >= 0.0f && at.box.bottom() <= H && at.primary.right() <= at.box.right()
                                   && at.disconnect.right() < at.show.x ? 1 : 0, 1);
            writePng(P, host, "gui.git.small.png");
            press(GC::Button::other);
            host.keys("escape");
            host.settle();
            panel.openGit("");
            host.settle();
            writePng(P, host, "gui.git.small.add.png");
            host.keys("escape");
            host.settle();
            const pg::ui::ProjectBrowser::Places small = browser.places(W, H);
            P.eq("small.projects_git_fits", small.git.right() <= small.box.right() && small.git.bottom() <= small.versions.y
                                                  && small.git.y >= small.box.y ? 1 : 0, 1);
        }
    }
    pg::projectstore::setDirectoryForTests({});
    return P.finish();
}
