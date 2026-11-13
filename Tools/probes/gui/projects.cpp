// PROBE name=gui.projects timeout=180
//
// gui.projects: PROJECTS' groups (editor/ProjectBrowser.h, editor/ProjectGroups.h) and the explorer's PROJECTS grouped
// the same way, headless over a real Processor, in a folder of the probe's own with made-up projects, to
// <build>/probe-out/ (each in GRAPHITE and, .paper.png, in PAPER):
//   gui.projects.png           the groups: FAVOURITES (a project and a factory one starred), FACTORY, a category a
//                              group, UNCATEGORISED last; a group folded; two projects chosen
//   gui.projects.category.png  the field naming a category (the context menu's New category...)
//   gui.projects.drag.png      two projects carried over a category's group
//   gui.projects.search.png    a search typed and a tag filtered on
//   gui.projects.explorer.png  the explorer's PROJECTS in groups, one folded
//   gui.projects.size.png      the projects by size, each row's right-hand column its file's size
//   gui.projects.bulk.png      EXPORT for three projects chosen (.running.png: its export under way; .done.png: what it
//                              came to); .paper.png each, gui.projects.bulk.done.paper.png the last; .least.png in
//                              the least window
// and checks:
//   GROUPS    FACTORY on top, then a category a group by name (told apart case aside: "Pad" and "pad" one group,
//             titled by the least spelling), UNCATEGORISED last; each group's projects by name; a click on a group's
//             header folds it (its rows gone, the header there, nothing chosen) and opens it again; the fold kept
//             (this Mac's view); a fold another instance made while the card is open kept beside one made here, and
//             an unfold here takes out only its own
//   FAVOURITE a click on a row's star stars it: FAVOURITES on top holding it, the project in its own group too; a
//             factory project starred the same; the star again unstars; kept (the view's); a star another instance
//             made while the card is open kept beside one made here
//   CHOOSE    a click chooses one alone; Cmd-click adds one and takes it out again; Shift-click chooses the rows from
//             the last one clicked; a click on one of several chosen chooses it alone on release
//   MENU      a right-click on one of those chosen: a menu for them all ("2 projects"), Add to favourites, Category (the
//             kind's categories and the user's own, None, New category...); a category chosen is theirs (the rest of
//             their info as it was), and None clears it; a factory project's Category and Delete greyed
//   INLINE    New category...: the field names a category (its pill CATEGORY, Save "Set category"), typed and Return
//             set it, the field its project's name again; Escape goes back, nothing set; Return with it empty refused
//             (the category as it was); typed in another case, spelt as the kind's or as the group's it joins; a
//             category group's Rename... moves every project of it, also while a search shows only one
//   DRAG      two chosen carried onto a category's group take its category; one let go outside the lists, nothing;
//             onto UNCATEGORISED, none; onto FAVOURITES, starred; onto FACTORY, refused (it says so), as it was
//   SEARCH    Cmd-F: the projects whose name, description or a tag holds each word typed, a folded group's matches
//             shown, a group of none gone; Escape clears it and the folds are back; the tag filter's menu (Any tag,
//             each tag with how many have it) keeps those with the tag; the search and the tag remembered when
//             PROJECTS opens again; the clear (x) and Any tag end them
//   SORT      the order's menu: Last modified, Date created, Size, Name as choices (Name checked), Reversed under a
//             separator; Last modified: each group's newest first, its title spelt as before; Date created: the newest
//             made first, a project saved again keeping its date; Size: the largest file first, the row's column its
//             size; Reversed turns it around (checked, kept); ties by name either way; the trigger's label each key's;
//             Name again
//   OUTSIDE   a press outside the card closes it as Close does: one on its edge does not, nor a drag that ends outside;
//             with Delete asked, a double-click outside closes it, deletes nothing and reaches nothing (the pill under
//             it opens no card), the question withdrawn after; the focus where Close leaves it
//   A11Y      a group item a group shown (in the projects' list), its header a toggle (checked while open) and its
//             rows in it, each row's star a toggle in its row; the search a text field, the tag and the order combo
//             boxes; a header pressed folds its group and a star pressed stars
//   EXPLORER  the explorer's PROJECTS: a caption a group (FAVOURITES first, the factory's starred one among them,
//             UNCATEGORISED last), the newest first in each; a click on a caption folds the group there and in
//             PROJECTS (one view), another opens it
//   EXPORT    a project's category goes out with its export and comes back with its import (.fmpack and .zip); the
//             factory projects carry theirs (their info's); EXPORT offers a project's own category as a pill, and
//             lights the kind's for one kept in another case (writing nothing)
//   BULK      three chosen, Export...: EXPORT for the three (the bulk card), one .zip asked for under a free name, its
//             progress while the job runs (EXPORTING in the status), then "Exported 3 projects to"; the bundle's path
//             again refused (an export never replaces a file), the file as it was; Cancel while it runs: no file; the
//             context menu's Export takes every one chosen; Import... brings the bundle's three back, counted
//   PREVIEW   the chosen version's panel in a pane over the versions (editor/Preview.h; gui.projects.preview.png and
//             .paper.png; .rows.png, a program with no panel.gui (its dials in rows); .none.png, a panel.gui of which
//             nothing reads and no dials (none, saying why); .reading.png, the choice not at rest yet; .factory.png;
//             .least.png, no pane): nothing asked until the choice rests (the keys through the list ask for none), then
//             drawn and registered; a version's click shows that version's, the one before let go; one drawn a poll,
//             none in a poll that drew Claude's view_panel; twelve kept; none read on the message thread; a factory
//             project's; closed, nothing drawn and the picture held kPictureHoldSeconds of the Panel's clock, then let
//             go; one image to a screen reader; the least window has no pane, its buttons as they were, and a press
//             outside still closes the card
//   READS     PROJECTS and EXPORT read the store's cache: a project another process wrote (a version more, then its
//             author) is read by the store's Reader alone, none on the message thread, and shows once read (its new
//             version first in PROJECTS; EXPORT's fields filled, then nothing written as it closes)
#include "Probe.h"

#include "editor/Explorer.h"
#include "editor/ExportCard.h"
#include "editor/Panel.h"
#include "editor/ProjectBrowser.h"
#include "editor/ProjectGroups.h"
#include "plugin/ChatAgent.h"
#include "plugin/EditorPort.h"
#include "plugin/ProjectStore.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/canvas/Images.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace pg = playground;
namespace gr = playground::ui::projectgroups;
using BL = pg::ui::ProjectBrowser::List;
using S = pg::ui::ExplorerItem::Section;

namespace
{
    constexpr float W = static_cast<float>(pg::ui::kDefaultWidth), H = static_cast<float>(pg::ui::kDefaultHeight);

    std::string program()
    {
        if constexpr (pg::product::kIsInstrument)
            return "// Probe Tone: a sine\nmodule Voice\n{\n    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                   "    float note;\n    void start(float n, float v) { note = n; }\n"
                   "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n";
        else
            return "// Probe Gain: a gain\nmodule Effect\n{\n    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
                   "    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
    }

    void typeText(funkgui::HeadlessHost& host, const std::string& text)
    {
        std::string spec;
        for (const char ch : text)
            spec += (spec.empty() ? "" : ",") + (ch == ' ' ? std::string("space") : std::string(1, ch));
        host.keys(spec.c_str());
    }

    void writePng(funkgui::test::Probe& P, funkgui::HeadlessHost& host, const std::string& name)
    {
        const std::string path = probe::outPath(name);
        host.draw();
        P.eq(name + ".written", host.writePng(path.c_str(), 2) ? 1 : 0, 1);
        std::printf("wrote %s\n", path.c_str());
    }

    funkgui::Mods cmd()
    {
        funkgui::Mods m;
        m.cmd = true;
        return m;
    }
    funkgui::Mods shift()
    {
        funkgui::Mods m;
        m.shift = true;
        return m;
    }
    funkgui::Mods ctrl()                                 // a ctrl-click: the context menu's (a right-click)
    {
        funkgui::Mods m;
        m.ctrl = true;
        return m;
    }

    // PROJECTS driven through its host: a row scrolled wholly into view first (the wheel, a notch's third at a time), then
    // clicked.
    struct Driver
    {
        pg::ui::Panel& panel;
        funkgui::HeadlessHost& host;
        pg::ui::ProjectBrowser& b;

        void show(int row)
        {
            const funkgui::Rect list = b.places(W, H).projects;
            for (int k = 0; k < 64 && row >= 0; ++k)
            {
                if (b.centreOfRow(BL::projects, row, W, H).x > -1.0e3f)
                    return;
                host.wheel(list.centreX(), list.centreY(), row < b.firstShown(BL::projects) ? 1.0f / 6.0f : -1.0f / 6.0f);
                host.settle();
            }
        }
        funkgui::Point at(int row)
        {
            show(row);
            return b.centreOfRow(BL::projects, row, W, H);
        }
        void click(int row, funkgui::Mods m = {})
        {
            const funkgui::Point p = at(row);
            host.click(p.x, p.y, m);
            host.settle();
        }
        void clickName(const std::string& name, funkgui::Mods m = {}) { click(b.rowOf(name), m); }
        void clickRect(const funkgui::Rect& r)
        {
            host.click(r.centreX(), r.centreY());
            host.settle();
        }
        void star(int row)
        {
            show(row);
            const funkgui::Point p = b.centreOfStar(row, W, H);
            host.click(p.x, p.y);
            host.settle();
        }
        // A press on one row, carried to another's centre, not let go (both rows in view first).
        void carry(int from, int to)
        {
            show(std::max(from, to));
            show(std::min(from, to));
            const funkgui::Point p = b.centreOfRow(BL::projects, from, W, H), q = b.centreOfRow(BL::projects, to, W, H);
            host.press(p.x, p.y);
            host.moveTo(q.x, q.y, 8);
            host.settle();
        }
    };

    const gr::Group* groupOf(const pg::ui::ProjectBrowser& b, const std::string& key)
    {
        for (const gr::Group& g : b.groups())
            if (g.key == key)
                return &g;
        return nullptr;
    }

    std::vector<std::string> membersOf(const pg::ui::ProjectBrowser& b, const std::string& key)
    {
        std::vector<std::string> out;
        if (const gr::Group* g = groupOf(b, key))
            for (const int m : g->members)
                out.push_back(b.projectList()[static_cast<std::size_t>(m)].name);
        return out;
    }

    // Starred in this Mac's view (EditorPort::projectsView) now.
    bool starredNow(const pg::EditorPort& port, const std::string& name)
    {
        const pg::ProjectsView v = port.projectsView();
        return gr::starred(&v.favourites, name);
    }

    const funkgui::MenuItem* itemLabelled(const std::vector<funkgui::MenuItem>& items, const std::string& label)
    {
        for (const funkgui::MenuItem& it : items)
            if (it.label == label)
                return &it;
        return nullptr;
    }
}

PROBE(gui, projects)
{
    const funkgui::HeadlessGuiScope gui;
    funkgui::UiPreferences::get().setBackend(funkgui::UiPreferences::memoryBackend());   // never the user's file
    funkgui::UiPreferences::get().setTheme(0);
    const juce::File folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                  .getChildFile(juce::String(pg::product::kEnvPrefix) + "gui-groups-" + juce::String(juce::Time::currentTimeMillis()));
    folder.deleteRecursively();
    pg::projectstore::setDirectoryForTests(folder);

    pg::Processor processor;
    pg::EditorPort& port = processor.editorPort();
    pg::Draft d;
    d.files = { { "program.fm", program() } };
    port.setDraft(d);
    port.run(d.files);
    processor.settle();

    // The made-up projects: three categories of this kind's (one spelt in lower case too), and three with none.
    const std::vector<std::string> cats = pg::ui::ExportCard::categories();
    const std::string c0 = cats[0], c1 = cats[1], c2 = cats[2];
    const std::string k0 = gr::keyOf(gr::Kind::category, c0), k1 = gr::keyOf(gr::Kind::category, c1), k2 = gr::keyOf(gr::Kind::category, c2);
    struct Made
    {
        const char* name;
        std::string category;
        std::vector<std::string> tags;
        const char* description;
    };
    const std::vector<Made> made{
        { "Deep Sub", c0, { "dark", "sub" }, "A sine sub an octave down." },
        { "Acid Line", c0, { "squelch" }, "A resonant saw line." },
        { "Bright Lead", c1, { "bright" }, "A thin square lead." },
        { "Glass Bloom", c2, { "warm", "bright" }, "A slow pad of detuned saws." },
        { "Night Choir", gr::lower(c2), { "warm" }, "Voices under a slow filter." },
        { "Sketch 1", "", {}, "An idea." },
        { "Sketch 2", "", {}, "" },
        { "Sketch 3", "", {}, "" },
    };
    {
        bool ok = true;
        for (const Made& m : made)
        {
            std::string why;
            pg::SavedVersion saved;
            ok = ok && port.saveVersion(m.name, saved, why) && port.setProjectInfo(m.name, { m.description, "Probe", m.category, m.tags, {} }, why);
            std::this_thread::sleep_for(std::chrono::milliseconds(3));   // each its own save time
        }
        P.eq("setup.saved", ok ? 1 : 0, 1);
    }

    pg::ui::Panel panel(processor.ports(), port);
    funkgui::HeadlessHost host(panel, 0, 2.0f);
    host.setZoom({ 100, 125, 150, 200 }, 100);
    host.settle();
    pg::ui::ProjectBrowser& b = panel.projectBrowser();
    Driver drive{ panel, host, b };
    const std::vector<pg::ProjectEntry> factory = port.factoryProjects();
    P.ge("setup.factory", static_cast<double>(factory.size()), 1.0);
    const std::string f0 = factory.empty() ? std::string() : factory.front().name;

    // A Panel in PAPER, PROJECTS opened and `then` done there, for a picture's twin.
    const auto paper = [&](const std::string& name, const std::function<void(Driver&)>& then) {
        funkgui::UiPreferences::get().setTheme(1);
        {
            pg::ui::Panel other(processor.ports(), port);
            funkgui::HeadlessHost otherHost(other, 1, 2.0f);
            otherHost.settle();
            other.openProjects();
            otherHost.settle();
            Driver od{ other, otherHost, other.projectBrowser() };
            if (then)
                then(od);
            writePng(P, otherHost, name);
            if (otherHost.pressed())
            {
                // A drag let go over the card's title, outside the lists: nothing dropped.
                const funkgui::Rect box = other.projectBrowser().places(W, H).box;
                otherHost.moveTo(box.x + 12.0f, box.y + 12.0f, 2);
                otherHost.release();
                otherHost.settle();
            }
            if (other.projectBrowser().isOpen())
                other.projectBrowser().close();
        }
        funkgui::UiPreferences::get().setTheme(0);
    };

    // ---- GROUPS ----------------------------------------------------------------------------------------------------
    panel.openProjects();
    host.settle();
    P.eq("groups.open", b.isOpen() ? 1 : 0, 1);
    {
        std::vector<std::string> keys;
        for (const gr::Group& g : b.groups())
            keys.push_back(g.key);
        std::string all;
        for (const std::string& k : keys)
            all += k + " | ";
        std::printf("groups: %s\n", all.c_str());
        std::vector<std::string> sorted{ k0, k1, k2 };   // the categories' groups by name
        std::sort(sorted.begin(), sorted.end());
        P.eq("groups.order", keys == std::vector<std::string>{ "factory", sorted[0], sorted[1], sorted[2], "uncategorised" } ? 1 : 0, 1);
        P.eq("groups.factory_first", b.headerOf("factory") == 0 && membersOf(b, "factory").size() == factory.size() ? 1 : 0, 1);
        const gr::Group* g2 = groupOf(b, k2);
        P.eq("groups.case_aside", g2 != nullptr && g2->members.size() == 2 && g2->title == c2 ? 1 : 0, 1);
        bool byName = true;
        for (const gr::Group& g : b.groups())
            for (std::size_t i = 1; i < g.members.size(); ++i)
                byName = byName && gr::lower(pg::ownName(b.projectList()[static_cast<std::size_t>(g.members[i - 1])].name))
                                       < gr::lower(pg::ownName(b.projectList()[static_cast<std::size_t>(g.members[i])].name));
        P.eq("groups.by_name", byName ? 1 : 0, 1);
        P.eq("groups.sketches_last", membersOf(b, "uncategorised") == std::vector<std::string>{ "Sketch 1", "Sketch 2", "Sketch 3" } ? 1 : 0, 1);
        const std::string chosen = b.chosenProject();
        drive.click(b.headerOf(k0));
        P.eq("groups.fold", b.rowOf("Deep Sub") < 0 && b.headerOf(k0) >= 0 && b.chosenProject() == chosen ? 1 : 0, 1);
        P.eq("groups.fold_kept", port.projectsView().folded == std::vector<std::string>{ k0 } ? 1 : 0, 1);
        drive.click(b.headerOf(k0));
        P.eq("groups.unfold", b.rowOf("Deep Sub") >= 0 && port.projectsView().folded.empty() ? 1 : 0, 1);
        // Another instance folds a group while this card is open: a fold here keeps both, an unfold here only its own.
        std::string why;
        pg::ProjectsView other = port.projectsView();
        other.folded.push_back(k1);
        P.eq("groups.another_folds", port.setProjectsView(other, why) ? 1 : 0, 1);
        drive.click(b.headerOf(k0));
        const std::vector<std::string> both = port.projectsView().folded;
        P.eq("groups.fold_merged", both.size() == 2 && std::find(both.begin(), both.end(), k0) != both.end()
                                       && std::find(both.begin(), both.end(), k1) != both.end() && groupOf(b, k1) != nullptr
                                       && groupOf(b, k1)->folded ? 1 : 0, 1);
        drive.click(b.headerOf(k0));
        P.eq("groups.unfold_own", port.projectsView().folded == std::vector<std::string>{ k1 } ? 1 : 0, 1);
        drive.click(b.headerOf(k1));
        P.eq("groups.unfold_theirs", port.projectsView().folded.empty() && groupOf(b, k1) != nullptr && !groupOf(b, k1)->folded ? 1 : 0, 1);
    }

    // ---- FAVOURITE -------------------------------------------------------------------------------------------------
    drive.star(b.rowOf("Glass Bloom"));
    P.eq("fav.pinned", !b.groups().empty() && b.groups()[0].kind == gr::Kind::favourites
                           && membersOf(b, "favourites") == std::vector<std::string>{ "Glass Bloom" } ? 1 : 0, 1);
    P.eq("fav.both_rows", b.rowOf("Glass Bloom", true) >= 0 && b.rowOf("Glass Bloom") >= 0 ? 1 : 0, 1);
    P.eq("fav.kept", port.projectsView().favourites == std::vector<std::string>{ "Glass Bloom" } ? 1 : 0, 1);
    drive.star(b.rowOf(f0));
    P.eq("fav.factory", membersOf(b, "favourites").size() == 2 && starredNow(port, f0) ? 1 : 0, 1);
    drive.star(b.rowOf("Glass Bloom", true));            // its star in FAVOURITES
    P.eq("fav.unstar", membersOf(b, "favourites") == std::vector<std::string>{ f0 } ? 1 : 0, 1);
    drive.star(b.rowOf("Glass Bloom"));
    {
        // Another instance stars one while this card is open: a star here keeps both.
        std::string why;
        pg::ProjectsView other = port.projectsView();
        other.favourites.push_back("Sketch 3");
        P.eq("fav.another_kept", port.setProjectsView(other, why) ? 1 : 0, 1);
        drive.star(b.rowOf("Bright Lead"));
        P.eq("fav.merged", starredNow(port, "Sketch 3") && starredNow(port, "Bright Lead") && starredNow(port, "Glass Bloom")
                               && b.rowOf("Sketch 3", true) >= 0 ? 1 : 0, 1);
        drive.star(b.rowOf("Sketch 3"));
        drive.star(b.rowOf("Bright Lead"));
        P.eq("fav.unmerged", !starredNow(port, "Sketch 3") && !starredNow(port, "Bright Lead") && starredNow(port, "Glass Bloom") ? 1 : 0, 1);
    }

    // ---- CHOOSE ----------------------------------------------------------------------------------------------------
    drive.clickName("Sketch 1");
    P.eq("choose.click", b.selection() == std::vector<std::string>{ "Sketch 1" } && b.chosenProject() == "Sketch 1" ? 1 : 0, 1);
    drive.clickName("Sketch 3", cmd());
    P.eq("choose.cmd_adds", b.selection() == std::vector<std::string>{ "Sketch 1", "Sketch 3" } && b.chosenProject() == "Sketch 3" ? 1 : 0, 1);
    drive.clickName("Sketch 3", cmd());
    P.eq("choose.cmd_removes", b.selection() == std::vector<std::string>{ "Sketch 1" } ? 1 : 0, 1);
    drive.clickName("Sketch 1");
    drive.clickName("Sketch 3", shift());
    P.eq("choose.shift_range", b.selection() == std::vector<std::string>{ "Sketch 1", "Sketch 2", "Sketch 3" } ? 1 : 0, 1);
    drive.clickName("Sketch 2");
    P.eq("choose.click_one_of_them", b.selection() == std::vector<std::string>{ "Sketch 2" } && b.chosenProject() == "Sketch 2" ? 1 : 0, 1);
    const auto chooseTwo = [](Driver& dr) {
        dr.clickName("Sketch 1");
        dr.clickName("Sketch 2", cmd());
    };
    chooseTwo(drive);
    drive.click(b.headerOf(k1));                         // folded, for the picture
    P.eq("choose.two", b.selection().size() == 2 ? 1 : 0, 1);
    writePng(P, host, "gui.projects.png");
    paper("gui.projects.paper.png", chooseTwo);
    drive.click(b.headerOf(k1));

    // ---- MENU ------------------------------------------------------------------------------------------------------
    {
        drive.clickName("Sketch 2", ctrl());
        const funkgui::MenuRequest* m = host.pendingMenu();
        P.eq("menu.shown", m != nullptr && !m->items.empty() && m->items[0].header && m->items[0].label == "2 projects" ? 1 : 0, 1);
        const funkgui::MenuItem* category = m != nullptr ? itemLabelled(m->items, "Category") : nullptr;
        P.eq("menu.favourite", m != nullptr && itemLabelled(m->items, "Add to favourites") != nullptr ? 1 : 0, 1);
        const std::vector<std::string> choices = b.categoryChoices();
        bool listed = category != nullptr && category->enabled && category->submenu.size() == choices.size() + 3;
        for (std::size_t i = 0; listed && i < choices.size(); ++i)
            listed = category->submenu[i].label == choices[i] && category->submenu[i].radio;
        P.eq("menu.categories", listed && category->submenu[choices.size() + 1].label == "None (Uncategorised)"
                                    && category->submenu[choices.size() + 2].label == "New category..." ? 1 : 0, 1);
        P.eq("menu.chosen", host.chooseMenuItem(c0) ? 1 : 0, 1);
        host.settle();
        std::printf("menu: %s\n", b.why().c_str());
        P.eq("menu.sets_both", port.projectInfo("Sketch 1").category == c0 && port.projectInfo("Sketch 2").category == c0
                                   && b.why().find("2 projects are in") != std::string::npos ? 1 : 0, 1);
        P.eq("menu.info_kept", port.projectInfo("Sketch 1").description == "An idea." && port.projectInfo("Sketch 1").author == "Probe" ? 1 : 0, 1);
        P.eq("menu.regrouped", membersOf(b, k0).size() == 4 && membersOf(b, "uncategorised") == std::vector<std::string>{ "Sketch 3" } ? 1 : 0, 1);
        drive.clickName("Sketch 2", ctrl());
        P.eq("menu.none", host.chooseMenuItem("None (Uncategorised)") ? 1 : 0, 1);
        host.settle();
        P.eq("menu.none_clears", port.projectInfo("Sketch 1").category.empty() && port.projectInfo("Sketch 2").category.empty() ? 1 : 0, 1);
        drive.clickName(f0, ctrl());
        m = host.pendingMenu();
        const funkgui::MenuItem* fc = m != nullptr ? itemLabelled(m->items, "Category") : nullptr;
        const funkgui::MenuItem* fd = m != nullptr ? itemLabelled(m->items, "Delete") : nullptr;
        P.eq("menu.factory_greyed", fc != nullptr && !fc->enabled && fd != nullptr && !fd->enabled ? 1 : 0, 1);
        host.cancelMenu();
        host.settle();
    }

    // ---- INLINE ----------------------------------------------------------------------------------------------------
    {
        drive.clickName("Deep Sub", ctrl());
        P.eq("inline.menu", host.chooseMenuItem("New category...") ? 1 : 0, 1);
        host.settle();
        P.eq("inline.mode", b.categoryMode() && b.saveLabel() == "Set category" && b.name().empty() ? 1 : 0, 1);
        typeText(host, "Drones");
        host.settle();
        writePng(P, host, "gui.projects.category.png");
        paper("gui.projects.category.paper.png", [](Driver& dr) {
            dr.clickName("Deep Sub", ctrl());
            dr.host.chooseMenuItem("New category...");
            dr.host.settle();
            typeText(dr.host, "Drones");
            dr.host.settle();
        });
        host.keys("return");
        host.settle();
        P.eq("inline.set", port.projectInfo("Deep Sub").category == "Drones" && !b.categoryMode() && b.name() == "Deep Sub"
                               && b.headerOf(gr::keyOf(gr::Kind::category, "Drones")) >= 0 && b.isOpen() ? 1 : 0, 1);
        drive.clickName("Acid Line", ctrl());
        host.chooseMenuItem("New category...");
        host.settle();
        typeText(host, "Nope");
        host.keys("escape");
        host.settle();
        P.eq("inline.escape", !b.categoryMode() && b.isOpen() && port.projectInfo("Acid Line").category == c0 && b.name() == "Acid Line" ? 1 : 0, 1);
        // Return with the field empty is refused, as an empty name is: None clears a category.
        drive.clickName("Acid Line", ctrl());
        host.chooseMenuItem("New category...");
        host.settle();
        host.keys("return");
        host.settle();
        std::printf("inline empty: %s\n", b.why().c_str());
        P.eq("inline.empty_refused", b.categoryMode() && port.projectInfo("Acid Line").category == c0
                                         && b.why().find("Name the category first") != std::string::npos ? 1 : 0, 1);
        host.keys("escape");
        host.settle();
        // Typed in another case (and with spaces round it), a category is spelt as the kind's, or as the group it joins.
        drive.clickName("Sketch 3", ctrl());
        host.chooseMenuItem("New category...");
        host.settle();
        typeText(host, gr::lower(c1));
        host.keys("return");
        host.settle();
        P.eq("inline.kind_spelling", port.projectInfo("Sketch 3").category == c1 && membersOf(b, k1).size() == 2 ? 1 : 0, 1);
        drive.clickName("Sketch 3", ctrl());
        host.chooseMenuItem("New category...");
        host.settle();
        typeText(host, " drones ");
        host.keys("return");
        host.settle();
        const std::string kd = gr::keyOf(gr::Kind::category, "Drones");
        P.eq("inline.group_spelling", port.projectInfo("Sketch 3").category == "Drones" && membersOf(b, kd).size() == 2 ? 1 : 0, 1);
        // Rename... while a search shows one of the group's two: both renamed, the group never split.
        host.keys("cmd+f");
        typeText(host, "sub");
        host.settle();
        const gr::Group* dg = groupOf(b, kd);
        P.eq("inline.filtered", dg != nullptr && dg->members.size() == 1 && dg->total == 2 && b.rowOf("Sketch 3") < 0 ? 1 : 0, 1);
        drive.click(b.headerOf(kd), ctrl());
        const funkgui::MenuRequest* m = host.pendingMenu();
        P.eq("inline.rename_offered", m != nullptr && itemLabelled(m->items, "Rename Drones...") != nullptr ? 1 : 0, 1);
        host.chooseMenuItem("Rename Drones...");
        host.settle();
        P.eq("inline.rename_mode", b.categoryMode() && b.name() == "Drones" ? 1 : 0, 1);
        typeText(host, "Ambient Drones");                // over the name, all of it chosen
        host.keys("return");
        host.settle();
        std::printf("inline rename: %s\n", b.why().c_str());
        P.eq("inline.renamed", port.projectInfo("Deep Sub").category == "Ambient Drones" && port.projectInfo("Sketch 3").category == "Ambient Drones"
                                   && b.why().find("2 projects are in Ambient Drones") != std::string::npos
                                   && b.headerOf(gr::keyOf(gr::Kind::category, "Ambient Drones")) >= 0
                                   && b.headerOf(kd) < 0 ? 1 : 0, 1);
        drive.clickRect(b.places(W, H).clear);           // the search cleared, Sketch 3 back to none
        drive.clickName("Sketch 3", ctrl());
        host.chooseMenuItem("None (Uncategorised)");
        host.settle();
        P.eq("inline.restored", b.search().empty() && port.projectInfo("Sketch 3").category.empty() ? 1 : 0, 1);
    }

    // ---- DRAG ------------------------------------------------------------------------------------------------------
    {
        chooseTwo(drive);
        drive.carry(b.rowOf("Sketch 1"), b.rowOf("Glass Bloom"));
        P.eq("drag.carrying", b.dragging() ? 1 : 0, 1);
        writePng(P, host, "gui.projects.drag.png");
        host.release();
        host.settle();
        P.eq("drag.category", port.projectInfo("Sketch 1").category == c2 && port.projectInfo("Sketch 2").category == c2 && !b.dragging() ? 1 : 0, 1);
        paper("gui.projects.drag.paper.png", [&](Driver& dr) {
            chooseTwo(dr);
            dr.carry(dr.b.rowOf("Sketch 1"), dr.b.headerOf("uncategorised"));
        });
        // The twin let go of its drag outside the lists (its host released it): nothing changed.
        P.eq("drag.outside_nothing", port.projectInfo("Sketch 1").category == c2 ? 1 : 0, 1);
        drive.clickName("Sketch 1");
        drive.carry(b.rowOf("Sketch 1"), b.headerOf("uncategorised"));
        host.release();
        host.settle();
        P.eq("drag.uncategorised", port.projectInfo("Sketch 1").category.empty() && port.projectInfo("Sketch 2").category == c2 ? 1 : 0, 1);
        drive.clickName("Deep Sub");
        drive.carry(b.rowOf("Deep Sub"), b.headerOf("favourites"));
        host.release();
        host.settle();
        P.eq("drag.favourites", starredNow(port, "Deep Sub") ? 1 : 0, 1);
        drive.clickName("Acid Line");
        drive.carry(b.rowOf("Acid Line"), b.headerOf("factory"));
        host.release();
        host.settle();
        P.eq("drag.factory_refused", port.projectInfo("Acid Line").category == c0 && b.why().find("Factory holds") != std::string::npos ? 1 : 0, 1);
    }

    // ---- SEARCH ----------------------------------------------------------------------------------------------------
    {
        drive.click(b.headerOf(k2));                     // Glass Bloom's group folded
        P.eq("search.folded_first", b.rowOf("Glass Bloom") < 0 ? 1 : 0, 1);
        host.keys("cmd+f");
        P.eq("search.cmd_f", b.searching() ? 1 : 0, 1);
        typeText(host, "warm");
        host.settle();
        bool allMatch = true;
        for (int i = 0; i < static_cast<int>(b.rowList().size()); ++i)
            if (const pg::ProjectEntry* e = b.rowProject(i))
                allMatch = allMatch && gr::matches(*e, "warm", "");
        P.eq("search.matches", b.search() == "warm" && allMatch && b.rowOf("Glass Bloom") >= 0 && b.rowOf("Night Choir") >= 0
                                   && b.rowOf("Deep Sub") < 0 ? 1 : 0, 1);
        P.eq("search.group_gone", b.headerOf(k1) < 0 ? 1 : 0, 1);
        host.keys("cmd+a");
        typeText(host, "slow pad");
        host.settle();
        P.eq("search.every_word", b.rowOf("Glass Bloom") >= 0 && b.rowOf("Night Choir") < 0 ? 1 : 0, 1);
        host.keys("escape");
        host.settle();
        P.eq("search.escape_clears", b.search().empty() && b.rowOf("Glass Bloom") < 0 && b.rowOf("Deep Sub") >= 0 ? 1 : 0, 1);
        host.keys("escape");                             // the search let go
        drive.click(b.headerOf(k2));
        drive.clickRect(b.places(W, H).tag);
        const funkgui::MenuRequest* m = host.pendingMenu();
        const funkgui::MenuItem* bright = m != nullptr ? itemLabelled(m->items, "bright") : nullptr;
        P.eq("search.tag_menu", m != nullptr && itemLabelled(m->items, "Any tag") != nullptr && bright != nullptr && bright->radio
                                    && bright->shortcut == "2" ? 1 : 0, 1);
        host.chooseMenuItem("bright");
        host.settle();
        P.eq("search.tag", b.view().tag == "bright" && b.rowOf("Glass Bloom") >= 0 && b.rowOf("Bright Lead") >= 0 && b.rowOf("Night Choir") < 0 ? 1 : 0, 1);
        drive.clickRect(b.places(W, H).search);
        typeText(host, "glass");
        host.settle();
        P.eq("search.with_tag", b.rowOf("Glass Bloom") >= 0 && b.rowOf("Bright Lead") < 0 ? 1 : 0, 1);
        writePng(P, host, "gui.projects.search.png");
        drive.clickRect(b.places(W, H).close);
        P.eq("search.closed", !b.isOpen() ? 1 : 0, 1);
        panel.openProjects();
        host.settle();
        P.eq("search.remembered", b.search() == "glass" && b.view().tag == "bright" && port.projectsView().search == "glass"
                                      && b.rowOf("Night Choir") < 0 ? 1 : 0, 1);
        paper("gui.projects.search.paper.png", {});
        drive.clickRect(b.places(W, H).clear);
        drive.clickRect(b.places(W, H).tag);
        host.chooseMenuItem("Any tag");
        host.settle();
        P.eq("search.cleared", b.search().empty() && b.view().tag.empty() && b.rowOf("Night Choir") >= 0 ? 1 : 0, 1);
    }

    // ---- SORT ------------------------------------------------------------------------------------------------------
    {
        // Night Choir saved again (a change): the newest of its group.
        b.close();
        host.settle();
        pg::Draft edited = port.draft();
        edited.files[0].text += "// again\n";
        port.setDraft(edited);
        std::string why;
        pg::SavedVersion saved;
        P.eq("sort.saved_again", port.saveVersion("Night Choir", saved, why) && saved.number == 2 ? 1 : 0, 1);
        panel.openProjects();
        host.settle();
        drive.clickRect(b.places(W, H).sort);
        {
            // Four keys as a set of choices (Name, the default, checked), then, under a separator, Reversed.
            const funkgui::MenuRequest* m = host.pendingMenu();
            const std::vector<std::string> want{ "Last modified", "Date created", "Size", "Name" };
            bool rows = m != nullptr && m->items.size() == 6;
            for (std::size_t i = 0; rows && i < 4; ++i)
                rows = m->items[i].label == want[i] && m->items[i].radio && m->items[i].checked == (i == 3);
            P.eq("sort.rows", rows && m->items[4].separator && m->items[5].label == "Reversed" && !m->items[5].checked
                                  && !m->items[5].radio && m->selectedId == m->items[3].id ? 1 : 0, 1);
        }
        P.eq("sort.menu", host.pendingMenu() != nullptr && host.chooseMenuItem("Last modified") ? 1 : 0, 1);
        host.settle();
        P.eq("sort.label_saved", b.orderLabel() == "Last modified" ? 1 : 0, 1);
        bool newest = true;
        for (const gr::Group& g : b.groups())
            for (std::size_t i = 1; i < g.members.size(); ++i)
                newest = newest && b.projectList()[static_cast<std::size_t>(g.members[i - 1])].savedMs
                                       >= b.projectList()[static_cast<std::size_t>(g.members[i])].savedMs;
        P.eq("sort.newest_first", b.view().order == pg::ProjectOrder::saved && newest && membersOf(b, k2).front() == "Night Choir" ? 1 : 0, 1);
        // Night Choir, spelt in lower case, first now: the group's title spelt as before (the least spelling).
        P.eq("sort.one_spelling", groupOf(b, k2) != nullptr && groupOf(b, k2)->title == c2 ? 1 : 0, 1);
        // Each key, each group's members in its order (ties by name), the row's right-hand column saying what it is
        // sorted by.
        const auto inOrder = [&](const std::function<int64_t(const pg::ProjectEntry&)>& key, bool reversed) {
            bool ok = true;
            for (const gr::Group& g : b.groups())
                for (std::size_t i = 1; i < g.members.size(); ++i)
                {
                    const pg::ProjectEntry& x = b.projectList()[static_cast<std::size_t>(g.members[i - 1])];
                    const pg::ProjectEntry& y = b.projectList()[static_cast<std::size_t>(g.members[i])];
                    const int64_t kx = key(x), ky = key(y);
                    ok = ok && (kx != ky ? (reversed ? kx < ky : kx > ky) : gr::lower(pg::ownName(x.name)) < gr::lower(pg::ownName(y.name)));
                }
            return ok;
        };
        const auto entry = [&](const std::string& name) {
            for (const pg::ProjectEntry& e : b.projectList())
                if (e.name == name)
                    return e;
            return pg::ProjectEntry{};
        };
        const auto chooseOrder = [&](const char* label) {
            drive.clickRect(b.places(W, H).sort);
            const bool ok = host.chooseMenuItem(label);
            host.settle();
            return ok;
        };
        P.eq("sort.created", chooseOrder("Date created") && b.view().order == pg::ProjectOrder::created
                                 && inOrder([](const pg::ProjectEntry& e) { return e.createdMs; }, false) ? 1 : 0, 1);
        // Night Choir saved again: made before its new version, its made date its first save's.
        P.eq("sort.created_kept", entry("Night Choir").createdMs > 0 && entry("Night Choir").createdMs < entry("Night Choir").savedMs ? 1 : 0, 1);
        {
            // The column: its version, then the date it was made, "MM-DD HH:MM" as a save's date shows.
            const std::string facts = b.rowFacts(entry("Glass Bloom"));
            const std::string date = facts.size() > 5 ? facts.substr(5) : std::string();
            const bool shaped = date.size() == 11 && date[2] == '-' && date[5] == ' ' && date[8] == ':'
                             && std::all_of(date.begin(), date.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)) || ch == '-' || ch == ' ' || ch == ':'; });
            std::printf("sort: Glass Bloom made [%s]\n", facts.c_str());
            P.eq("sort.label_created", b.orderLabel() == "Date created" && facts.rfind("v1   ", 0) == 0 && shaped ? 1 : 0, 1);
        }
        P.eq("sort.size", chooseOrder("Size") && b.view().order == pg::ProjectOrder::size
                              && inOrder([](const pg::ProjectEntry& e) { return e.bytes; }, false) ? 1 : 0, 1);
        {
            const pg::ProjectEntry gb = entry("Glass Bloom");
            const std::string facts = b.rowFacts(gb);
            std::printf("sort: Glass Bloom %lld bytes: [%s]\n", static_cast<long long>(gb.bytes), facts.c_str());
            P.eq("sort.size_column", gb.bytes > 0 && facts.rfind("v1   ", 0) == 0
                                         && (facts.find(" KB") != std::string::npos || facts.find(" bytes") != std::string::npos) ? 1 : 0, 1);
        }
        P.eq("sort.label_size", b.orderLabel() == "Size" ? 1 : 0, 1);
        writePng(P, host, "gui.projects.size.png");
        paper("gui.projects.size.paper.png", {});
        // Reversed: the smallest first; checked in the menu; kept (this Mac's view).
        P.eq("sort.reversed", chooseOrder("Reversed") && b.view().reversed && b.view().order == pg::ProjectOrder::size
                                  && port.projectsView().reversed && inOrder([](const pg::ProjectEntry& e) { return e.bytes; }, true) ? 1 : 0, 1);
        drive.clickRect(b.places(W, H).sort);
        {
            const funkgui::MenuRequest* m = host.pendingMenu();
            P.eq("sort.reversed_checked", m != nullptr && m->items.size() == 6 && m->items[5].checked && m->items[2].checked ? 1 : 0, 1);
            host.cancelMenu();
            host.settle();
        }
        // The explorer's PROJECTS stays newest saved first, whatever the card's order.
        // Ties: equal keys by name, A to Z, either way round (arrange itself, over made-up entries).
        {
            std::vector<pg::ProjectEntry> tied(3);
            tied[0].name = "beta";
            tied[1].name = "Alpha";
            tied[2].name = "gamma";
            for (pg::ProjectEntry& e : tied)
                e.bytes = 100;
            tied[2].bytes = 200;
            bool ties = true;
            for (const bool rev : { false, true })
            {
                gr::Options o;
                o.order = pg::ProjectOrder::size;
                o.reversed = rev;
                const std::vector<gr::Group> g = gr::arrange(tied, o);
                std::vector<std::string> names;
                for (const gr::Group& grp : g)
                    for (const int m : grp.members)
                        names.push_back(tied[static_cast<std::size_t>(m)].name);
                ties = ties && names == (rev ? std::vector<std::string>{ "Alpha", "beta", "gamma" } : std::vector<std::string>{ "gamma", "Alpha", "beta" });
            }
            P.eq("sort.ties_by_name", ties ? 1 : 0, 1);
        }
        P.eq("sort.by_name", chooseOrder("Reversed") && chooseOrder("Name") && b.view().order == pg::ProjectOrder::name && !b.view().reversed
                                 && membersOf(b, k2).front() == "Glass Bloom" ? 1 : 0, 1);
    }

    // ---- A11Y ------------------------------------------------------------------------------------------------------
    {
        drive.show(0);
        const std::vector<funkgui::A11yItem> items = host.accessibility();
        const uint32_t base = pg::ui::ProjectBrowser::kA11yBase, list = base + 1;
        int groups = 0, headers = 0, rowsIn = 0, stars = 0;
        bool search = false, tag = false, order = false, starChecked = false;
        for (const funkgui::A11yItem& it : items)
        {
            if (it.role == funkgui::a11yrole::group && it.parent == list)
                ++groups;
            if (it.role == funkgui::A11yRole::toggleButton && it.checkable && it.title != "Favourite" && it.id >= base + 0x100 && it.id < base + 0x1100)
                headers += it.parent >= base + 0x4000 && it.parent < base + 0x5000 ? 1 : 0;
            if (it.role == funkgui::A11yRole::listItem && it.parent >= base + 0x4000 && it.parent < base + 0x5000)
                ++rowsIn;
            if (it.role == funkgui::A11yRole::toggleButton && it.title == "Favourite" && it.parent >= base + 0x100 && it.parent < base + 0x1100)
            {
                ++stars;
                for (const funkgui::A11yItem& row : items)
                    if (row.id == it.parent && row.title == "Glass Bloom")
                        starChecked = starChecked || it.checked;
            }
            search = search || (it.role == funkgui::a11yrole::editableText && it.title == "Search projects");
            tag = tag || (it.role == funkgui::A11yRole::comboBox && it.title == "Tag");
            order = order || (it.role == funkgui::A11yRole::comboBox && it.title == "Order");
        }
        std::printf("a11y: %d groups, %d headers, %d rows, %d stars\n", groups, headers, rowsIn, stars);
        P.ge("a11y.groups", groups, 3);
        P.eq("a11y.headers", headers, groups);
        P.ge("a11y.rows_in_groups", rowsIn, 4);
        P.eq("a11y.stars", stars, rowsIn);
        P.eq("a11y.star_checked", starChecked ? 1 : 0, 1);
        P.eq("a11y.top", search && tag && order ? 1 : 0, 1);
        const int header = b.headerOf("factory");
        panel.a11yAction(base + 0x100 + static_cast<uint32_t>(header), funkgui::A11yAction::press, 0.0);
        host.settle();
        P.eq("a11y.header_press", groupOf(b, "factory") != nullptr && groupOf(b, "factory")->folded ? 1 : 0, 1);
        panel.a11yAction(base + 0x100 + static_cast<uint32_t>(b.headerOf("factory")), funkgui::A11yAction::press, 0.0);
        host.settle();
        drive.show(b.rowOf("Bright Lead"));
        panel.a11yAction(base + 0x5000 + static_cast<uint32_t>(b.rowOf("Bright Lead")), funkgui::A11yAction::press, 0.0);
        host.settle();
        P.eq("a11y.star_press", starredNow(port, "Bright Lead") ? 1 : 0, 1);
        drive.star(b.rowOf("Bright Lead"));              // unstarred again
    }

    // ---- EXPLORER --------------------------------------------------------------------------------------------------
    {
        b.close();
        host.settle();
        host.tick(4);
        host.settle();
        pg::ui::ExplorerItem& ex = panel.explorer();
        // A row's index (the first of that label at or after `from`), or -1; rowUnder: one under a caption, before
        // the next.
        const auto rowNamed = [&](const std::string& label, int from = 0) {
            for (int i = std::max(0, from); i < ex.rows(S::projects); ++i)
                if (const auto* r = ex.row(S::projects, i); r != nullptr && r->label == label)
                    return i;
            return -1;
        };
        const auto rowUnder = [&](const std::string& caption, const std::string& label) {
            const int c = rowNamed(caption);
            for (int i = c + 1; c >= 0 && i < ex.rows(S::projects); ++i)
            {
                const auto* r = ex.row(S::projects, i);
                if (r == nullptr || r->heading)
                    break;
                if (r->label == label)
                    return i;
            }
            return -1;
        };
        std::vector<std::string> captions;
        for (int i = 0; i < ex.rows(S::projects); ++i)
            if (const auto* r = ex.row(S::projects, i); r != nullptr && r->heading)
                captions.push_back(r->label);
        std::string all;
        for (const std::string& c : captions)
            all += c + " | ";
        std::printf("explorer captions: %s\n", all.c_str());
        const auto* first = ex.row(S::projects, 0);
        P.eq("explorer.favourites_first", first != nullptr && first->heading && first->fold == 1 && first->label == "FAVOURITES" ? 1 : 0, 1);
        P.eq("explorer.uncategorised_last", !captions.empty() && captions.back() == "UNCATEGORISED" ? 1 : 0, 1);
        P.eq("explorer.categories", std::find(captions.begin(), captions.end(), pg::ui::upper(c2)) != captions.end()
                                        && std::find(captions.begin(), captions.end(), "AMBIENT DRONES") != captions.end() ? 1 : 0, 1);
        P.eq("explorer.factory_starred", rowNamed(pg::ownName(f0)) > 0 && rowNamed(pg::ownName(f0)) < rowNamed(captions.size() > 1 ? captions[1] : "") ? 1 : 0, 1);
        P.eq("explorer.newest_first", rowUnder(pg::ui::upper(c2), "Night Choir") >= 0
                                          && rowUnder(pg::ui::upper(c2), "Night Choir") < rowUnder(pg::ui::upper(c2), "Glass Bloom") ? 1 : 0, 1);
        // The caption of c2 in view, then clicked: folded here and in PROJECTS.
        const int cap = rowNamed(pg::ui::upper(c2));
        ex.scrollTo(0.0f);
        for (int k = 0; k < 80 && cap >= 0 && !ex.centreOfRow(S::projects, cap); ++k)
            ex.scrollTo(ex.scroll() + 30.0f);
        host.settle();
        if (const auto at = ex.centreOfRow(S::projects, cap))
            host.click(at->x, at->y);
        host.tick(2);
        host.settle();
        P.eq("explorer.fold", rowUnder(pg::ui::upper(c2), "Night Choir") < 0 && rowNamed(pg::ui::upper(c2)) >= 0 && ex.row(S::projects, rowNamed(pg::ui::upper(c2)))->fold == 2
                                  && std::find(port.projectsView().folded.begin(), port.projectsView().folded.end(), k2) != port.projectsView().folded.end()
                                  ? 1 : 0, 1);
        // The picture: PROJECTS' header at the top of the explorer, a group folded.
        ex.scrollTo(0.0f);
        host.settle();
        if (const auto h = ex.centreOfHeader(S::projects))
            ex.scrollTo(h->y - ex.bounds().y - 20.0f);
        host.settle();
        writePng(P, host, "gui.projects.explorer.png");
        const float scrolled = ex.scroll();
        paper("gui.projects.explorer.paper.png", [&](Driver& dr) {
            dr.b.close();
            dr.host.settle();
            dr.host.tick(4);
            dr.host.settle();
            dr.panel.explorer().scrollTo(scrolled);
            dr.host.settle();
        });
        panel.openProjects();
        host.settle();
        P.eq("explorer.fold_in_projects", groupOf(b, k2) != nullptr && groupOf(b, k2)->folded ? 1 : 0, 1);
        b.close();
        host.settle();
        for (int k = 0; k < 80 && !ex.centreOfRow(S::projects, rowNamed(pg::ui::upper(c2))); ++k)
            ex.scrollTo(ex.scroll() + 30.0f);
        if (const auto at = ex.centreOfRow(S::projects, rowNamed(pg::ui::upper(c2))))
            host.click(at->x, at->y);
        host.tick(2);
        host.settle();
        P.eq("explorer.unfold", rowUnder(pg::ui::upper(c2), "Night Choir") >= 0 && port.projectsView().folded.empty() ? 1 : 0, 1);
    }

    // ---- OUTSIDE: a press outside the card closes it as Close does --------------------------------------------------
    {
        if (!b.isOpen())
        {
            panel.openProjects();
            host.settle();
        }
        const funkgui::Rect box = b.places(W, H).box;
        // On its edge (inside the box, beside every control): it stays.
        host.click(box.x + 3.0f, box.y + box.h * 0.5f);
        host.settle();
        P.eq("outside.edge_keeps", b.isOpen() ? 1 : 0, 1);
        // A drag that began inside and ends outside (a project carried off the card): nothing closes.
        drive.carry(b.rowOf("Sketch 1"), b.rowOf("Sketch 1"));
        host.moveTo(box.x - 20.0f, box.y + 10.0f, 4);
        host.release();
        host.settle();
        P.eq("outside.drag_out_keeps", b.isOpen() && !b.dragging() ? 1 : 0, 1);
        // Delete asked, then a press outside: closed, nothing deleted, the question withdrawn.
        drive.clickName("Sketch 3");
        drive.clickRect(b.places(W, H).remove);
        const bool asked = b.removeLabel() != "Delete";
        const int focusBefore = panel.focusItem();
        // A double-click outside, over the top bar's PROJECTS pill: the first press closes the card, the second, and the
        // double-click, reach nothing (the pill would open the card again).
        const funkgui::Point pill = panel.centreOfProjects();
        host.doubleClick(pill.x, pill.y);
        host.settle();
        std::printf("outside: asked [%s], focus %d -> %d\n", b.removeLabel().c_str(), focusBefore, panel.focusItem());
        P.eq("outside.closes", asked && !b.isOpen() ? 1 : 0, 1);
        P.eq("outside.nothing_deleted", port.projectPolled("Sketch 3").exists ? 1 : 0, 1);
        P.eq("outside.reaches_nothing", !b.isOpen() && !panel.exportCard().isOpen() ? 1 : 0, 1);
        const int focusOutside = panel.focusItem();
        panel.openProjects();
        host.settle();
        P.eq("outside.question_withdrawn", b.isOpen() && b.removeLabel() == "Delete" ? 1 : 0, 1);
        drive.clickRect(b.places(W, H).close);           // Close: the focus where an outside press left it
        P.eq("outside.focus_as_close", !b.isOpen() && focusOutside >= 0 && panel.focusItem() == focusOutside ? 1 : 0, 1);
    }

    // ---- EXPORT ----------------------------------------------------------------------------------------------------
    {
        std::string why;
        for (const bool package : { true, false })
        {
            const juce::File out = folder.getChildFile("out").getChildFile(package ? "Glass Bloom.fmpack" : "Glass Bloom.zip");
            out.getParentDirectory().createDirectory();
            pg::Exported done;
            std::string name;
            const bool ok = port.exportProject("Glass Bloom", package, true, out.getFullPathName().toStdString(), done, why)
                         && port.importProject(out.getFullPathName().toStdString(), name, why);
            std::printf("export %s: %s %s\n", package ? "fmpack" : "zip", name.c_str(), why.c_str());
            P.eq(std::string("export.category_") + (package ? "fmpack" : "zip"), ok && port.projectInfo(name).category == c2 ? 1 : 0, 1);
            (void)port.deleteProject(name, why);
        }
        bool carried = !factory.empty(), any = false;
        for (const pg::ProjectEntry& e : factory)
        {
            carried = carried && e.category == port.projectInfo(e.name).category;
            any = any || !e.category.empty();
        }
        P.eq("export.factory_carry", carried && any ? 1 : 0, 1);
        pg::ui::ExportCard& card = panel.exportCard();
        card.open("Deep Sub");
        host.settle();
        P.eq("export.own_category_pill", card.isOpen() && !card.choices().empty() && card.choices().back() == "Ambient Drones"
                                             && card.info().category == "Ambient Drones" ? 1 : 0, 1);
        card.close();
        host.settle();
        P.eq("export.kept_as_is", port.projectInfo("Deep Sub").category == "Ambient Drones" ? 1 : 0, 1);
        // One of the kind's kept in another case: that category's pill lit, no pill of its own, nothing written.
        card.open("Night Choir");
        host.settle();
        P.eq("export.case_aside_pill", card.isOpen() && card.info().category == c2 && card.choices() == pg::ui::ExportCard::categories() ? 1 : 0, 1);
        card.close();
        host.settle();
        P.eq("export.case_kept", port.projectInfo("Night Choir").category == gr::lower(c2) ? 1 : 0, 1);

        // Several chosen, Export...: the bulk card (no info fields: the count, the names, the versions, the two
        // exports), one .zip asked for under a free name, its progress while the job runs (held at its gate), then
        // its outcome; in GRAPHITE and in PAPER.
        const std::vector<std::string> three{ "Acid Line", "Bright Lead", "Deep Sub" };
        const juce::File bundleOut = folder.getChildFile("bulk");
        bundleOut.createDirectory();
        const auto waitLanded = [&](pg::ui::Panel& pn, funkgui::HeadlessHost& h) {
            for (const auto by = std::chrono::steady_clock::now() + probe::kWorkWait;
                 (pn.exportCard().running() || port.exporting().busy) && std::chrono::steady_clock::now() < by;)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                processor.settle();
                h.tick(1);
            }
            h.settle();
            return !pn.exportCard().running();
        };
        const auto bulk = [&](Driver& dv, const std::string& stem, bool rows) {
            pg::ui::ExportCard& ec = dv.panel.exportCard();
            dv.b.close();
            dv.host.settle();
            dv.panel.openProjects();
            dv.host.settle();
            dv.clickName(three[0]);
            dv.clickName(three[1], cmd());
            dv.clickName(three[2], cmd());
            dv.clickRect(dv.b.places(W, H).exportIt);
            if (rows)
                P.eq("bulk.opens", ec.isOpen() && ec.bulk() && ec.projects().size() == 3 && !dv.b.isOpen() ? 1 : 0, 1);
            writePng(P, dv.host, stem + ".png");
            // The package: one .zip, its name free where the chooser starts.
            dv.clickRect(ec.places(W, H).package);
            const funkgui::FileRequest* asked = dv.host.pendingFiles();
            const std::string suggested = std::string(pg::product::kName) + " Projects.zip";
            if (rows)
                P.eq("bulk.asks_zip", asked != nullptr && asked->suggestedName == suggested && asked->pattern == "*.zip" ? 1 : 0, 1);
            const juce::File to = bundleOut.getChildFile(juce::String(stem.substr(stem.rfind('/') + 1)) + ".zip");
            processor.setExportGate(true);               // held before it reads: the card as it runs
            dv.host.returnFiles({ to.getFullPathName().toStdString() });
            dv.host.settle();
            if (rows)
                P.eq("bulk.running", ec.running() && port.exporting().busy && ec.why().find("Exporting") != std::string::npos
                                         && dv.panel.statusText().find("EXPORTING") != std::string::npos ? 1 : 0, 1);
            writePng(P, dv.host, stem + ".running.png");
            processor.setExportGate(false);
            const bool landed = waitLanded(dv.panel, dv.host);
            std::printf("bulk: %s; status %s\n", ec.why().c_str(), dv.panel.statusText().c_str());
            if (rows)
            {
                P.eq("bulk.landed", landed && to.existsAsFile() ? 1 : 0, 1);
                P.eq("bulk.said", ec.why().find("Exported 3 projects to") != std::string::npos
                                      && dv.panel.statusText().find("EXPORTED 3 PROJECTS") != std::string::npos ? 1 : 0, 1);
            }
            return to;
        };
        b.close();
        host.settle();
        const juce::File bundled = bulk(drive, "gui.projects.bulk", true);
        writePng(P, host, "gui.projects.bulk.done.png");
        paper("gui.projects.bulk.done.paper.png", [&](Driver& dv) { (void)bulk(dv, "gui.projects.bulk.paper", false); });
        // The least window: the bulk card within it, its buttons apart and inside.
        {
            const bool least = host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight);
            host.settle();
            const float lw = static_cast<float>(pg::ui::kMinWidth), lh = static_cast<float>(pg::ui::kMinHeight);
            const pg::ui::ExportCard::Places at = card.places(lw, lh);
            P.eq("bulk.least_fits", least && card.isOpen() && card.bulk() && at.box.x >= 0.0f && at.box.right() <= lw && at.box.bottom() <= lh
                                        && at.done.right() < at.source.x && at.source.right() < at.package.x && at.package.right() <= at.box.right()
                                        && at.versions.bottom() < at.done.y - 30.0f ? 1 : 0, 1);
            writePng(P, host, "gui.projects.bulk.least.png");
            host.resize(pg::ui::kDefaultWidth, pg::ui::kDefaultHeight);
            host.settle();
        }
        // An export never replaces a file: the bundle's path again is refused, the file as it was.
        {
            const juce::int64 before = bundled.getSize();
            card.close();
            host.settle();
            card.open(three);
            host.settle();
            drive.clickRect(card.places(W, H).source);
            host.returnFiles({ bundled.getFullPathName().toStdString() });
            host.settle();
            P.eq("bulk.refused_there_already", !card.running() && card.why().find("is there already: an export never replaces a file") != std::string::npos
                                                   && bundled.getSize() == before ? 1 : 0, 1);
            // Cancel while it runs: no file, said so.
            const juce::File cancelled = bundleOut.getChildFile("cancelled.zip");
            drive.clickRect(card.places(W, H).source);
            processor.setExportGate(true);
            host.returnFiles({ cancelled.getFullPathName().toStdString() });
            host.settle();
            const bool running = card.running();
            drive.clickRect(card.places(W, H).done);    // Done is Cancel while it runs
            processor.setExportGate(false);
            (void)waitLanded(panel, host);
            P.eq("bulk.cancel", running && card.isOpen() && !cancelled.exists() && card.why() == "Cancelled: no file written." ? 1 : 0, 1);
            card.close();
            host.settle();
        }
        // The context menu's Export takes every project chosen.
        {
            panel.openProjects();
            host.settle();
            drive.clickName(three[0]);
            drive.clickName(three[1], cmd());
            drive.clickName(three[1], ctrl());
            const funkgui::MenuRequest* m = host.pendingMenu();
            P.eq("bulk.menu", m != nullptr && itemLabelled(m->items, "Export 2 projects...") != nullptr
                                  && host.chooseMenuItem("Export 2 projects...") ? 1 : 0, 1);
            host.settle();
            P.eq("bulk.menu_opens", card.isOpen() && card.projects() == std::vector<std::string>{ three[0], three[1] } ? 1 : 0, 1);
            card.close();
            host.settle();
        }
        // Import... takes the bundle: every project of it a project of its own, the status counting them.
        {
            panel.openProjects();
            host.settle();
            drive.clickRect(b.places(W, H).importIt);
            host.returnFiles({ bundled.getFullPathName().toStdString() });
            host.settle();
            std::printf("bulk import: %s\n", panel.statusText().c_str());
            bool added = panel.statusText().find("IMPORTED 3 PROJECTS") != std::string::npos;
            for (const std::string& n : three)
                added = added && port.projectPolled(n + " 2").exists;
            P.eq("bulk.imported", added ? 1 : 0, 1);
            for (const std::string& n : three)
                (void)port.deleteProject(n + " 2", why);
            if (b.isOpen())
                b.close();
            host.settle();
        }
    }

    // ---- READS: PROJECTS and EXPORT read the store's cache; a file another process wrote, its Reader -------------------
    {
        // Another process writes "Sketch 3": a version more and its info's author. Neither card reads the file on the
        // message thread (its parses all the Reader's), and each shows it once the Reader has it (poll()).
        const juce::File f = folder.getChildFile("Sketch 3.fmproject");
        const auto rewrite = [&](const std::function<void(juce::var&)>& change) {
            juce::var root = juce::JSON::parse(f.loadFileAsString());
            change(root);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));   // its time moves on a coarse clock too
            return f.replaceWithText(juce::JSON::toString(root));
        };
        int latest = 0;
        const bool wrote = rewrite([&](juce::var& root) {
            if (juce::Array<juce::var>* versions = root["versions"].getArray(); versions != nullptr && !versions->isEmpty())
            {
                juce::var v = versions->getLast().clone();
                latest = static_cast<int>(v["number"]) + 1;
                v.getDynamicObject()->setProperty("number", latest);
                v.getDynamicObject()->setProperty("note", "written elsewhere");
                versions->add(v);
            }
        });
        const auto waitFor = [&](const std::function<bool()>& done) {
            for (int k = 0; k < 2000 && !done(); ++k)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                host.tick(1);
            }
            return done();
        };
        const int parsed0 = pg::projectstore::parses(), reader0 = pg::projectstore::readerParses();
        panel.openProjects();
        host.settle();
        b.chooseProjectNamed("Sketch 3", W, H);               // shown first: a filter off, its group open
        host.settle();
        const bool shown = waitFor([&] {
            return b.chosenRead() && !b.versionList().empty() && b.versionList().front().number == latest;
        });
        const int onThread = (pg::projectstore::parses() - parsed0) - (pg::projectstore::readerParses() - reader0);
        std::printf("reads: PROJECTS, %d file(s) read on the message thread, %d by the Reader\n", onThread,
                    pg::projectstore::readerParses() - reader0);
        P.eq("reads.projects_setup", wrote && latest > 1 ? 1 : 0, 1);
        P.eq("reads.projects_none_on_the_message_thread", onThread, 0);
        P.eq("reads.projects_shown_once_read", shown && b.versionList().front().note == "written elsewhere" ? 1 : 0, 1);
        b.close();
        host.settle();
        const bool wroteInfo = rewrite([](juce::var& root) {
            if (auto* info = root["info"].getDynamicObject())
                info->setProperty("author", "Elsewhere");
        });
        const int parsed1 = pg::projectstore::parses(), reader1 = pg::projectstore::readerParses();
        pg::ui::ExportCard& card = panel.exportCard();
        card.open("Sketch 3");
        host.settle();
        const bool filled = waitFor([&] { return !card.reading() && card.info().author == "Elsewhere"; });
        P.eq("reads.export_none_on_the_message_thread",
             (pg::projectstore::parses() - parsed1) - (pg::projectstore::readerParses() - reader1), 0);
        P.eq("reads.export_filled_once_read", wroteInfo && filled ? 1 : 0, 1);
        card.close();
        host.settle();
        P.eq("reads.export_nothing_written", port.projectPolled("Sketch 3").info.author == "Elsewhere" ? 1 : 0, 1);
    }

    // ---- PREVIEW: the chosen version's panel over the versions (package 04, editor/Preview.h) ---------------------------
    {
        // "Panel Pad": two versions with a panel.gui (v2's a section more), saved as the user saves them.
        const std::string gui1 = "panel {\n    row {\n        section \"LEVEL\" { knob level }\n    }\n}\n";
        const std::string gui2 = "panel {\n    row {\n        section \"LEVEL\" { knob level }\n        section \"AGAIN\" { slider level }\n    }\n}\n";
        std::string why;
        pg::SavedVersion saved;
        const auto save = [&](const std::string& name, const pg::Sources& files) {
            pg::Draft dd;
            dd.files = files;
            port.setDraft(dd);
            port.run(dd.files);
            processor.settle();
            return port.saveVersion(name, saved, why);
        };
        const bool padSaved = save("Panel Pad", { { "program.fm", program() }, { "panel.gui", gui1 } })
                           && save("Panel Pad", { { "program.fm", program() }, { "panel.gui", gui2 } }) && saved.number == 2
                           && save("Panel Two", { { "program.fm", program() }, { "panel.gui", gui1 } })
                           && save("Panel Three", { { "program.fm", program() }, { "panel.gui", gui2 } })
                           && save("Panel Broken", { { "program.fm", pg::product::kIsInstrument
                                                                         ? "module Voice\n{\n    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n"
                                                                         : "module Effect\n{\n    void process(float* restrict left, float* restrict right, int n) {}\n};\n" },
                                                     { "panel.gui", "not a panel\n" } });
        P.eq("preview.setup", padSaved ? 1 : 0, 1);
        using St = pg::PreviewState::Status;
        // The Panel's frames and the processor's polls (a frame a 60th of the Panel's clock), until `done`: a generous
        // deadline, returning as soon as it is done.
        const auto pump = [&](funkgui::HeadlessHost& h, const std::function<bool()>& done) {
            const auto until = std::chrono::steady_clock::now() + probe::kWorkWait;
            while (!done() && std::chrono::steady_clock::now() < until)
            {
                processor.poll();
                h.tick(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return done();
        };
        const auto shown = [&] { return b.previewShown(); };
        const auto settled = [&] { const St s = shown().status; return s == St::ready || s == St::failed; };
        const int parsed0 = pg::projectstore::parses(), reader0 = pg::projectstore::readerParses();
        const uint64_t drawn0 = processor.previewsDrawn();
        panel.openProjects();
        host.settle();
        const pg::ui::ProjectBrowser::Places at = b.places(W, H);
        P.eq("preview.pane", at.preview.w > 200.0f && at.preview.h >= 120.0f && at.preview.bottom() < at.versions.y
                                 ? 1 : 0, 1);
        P.ge("preview.versions_rows", at.rows[1], 4);
        P.ge("preview.presets_rows", at.rows[2], 4);
        // A program with no panel.gui: its dials in rows.
        const std::size_t kept0 = processor.previewsKept();
        b.chooseProjectNamed("Sketch 1", W, H);
        host.tick(2);
        P.eq("preview.rest_asks_nothing", shown().status == St::reading && processor.previewsKept() == kept0 ? 1 : 0, 1);
        P.eq("preview.rows", pump(host, settled) && shown().status == St::ready ? 1 : 0, 1);
        writePng(P, host, "gui.projects.preview.rows.png");
        // A panel.gui of which nothing reads, and no dials: none, saying why.
        b.chooseProjectNamed("Panel Broken", W, H);
        host.tick(1);
        P.eq("preview.none", pump(host, settled) && shown().status == St::failed
                                 && shown().why.rfind("panel.gui does not read", 0) == 0 ? 1 : 0, 1);
        writePng(P, host, "gui.projects.preview.none.png");
        // Panel Pad: its newest version, drawn and registered.
        b.chooseProjectNamed("Panel Pad", W, H);
        host.tick(1);
        writePng(P, host, "gui.projects.preview.reading.png");
        P.eq("preview.ready", pump(host, settled) && shown().status == St::ready && shown().number == 2 ? 1 : 0, 1);
        const uint32_t newest = shown().image;
        P.eq("preview.registered", newest != 0 && funkgui::images::has(newest) ? 1 : 0, 1);
        writePng(P, host, "gui.projects.preview.png");
        paper("gui.projects.preview.paper.png", [&](Driver& dv) {
            dv.b.chooseProjectNamed("Panel Pad", W, H);
            const auto until = std::chrono::steady_clock::now() + probe::kWorkWait;
            while (dv.b.previewShown().status != St::ready && std::chrono::steady_clock::now() < until)
            {
                processor.poll();
                dv.host.tick(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            P.eq("preview.paper_ready", dv.b.previewShown().status == St::ready && dv.b.previewShown().image != newest ? 1 : 0, 1);
        });
        // A version's click: v1's panel.
        {
            const funkgui::Point p = b.centreOfRow(BL::versions, 1, W, H);
            host.click(p.x, p.y);
            host.tick(1);
        }
        P.eq("preview.version", pump(host, [&] { return shown().status == St::ready && shown().number == 1; }) && shown().image != newest ? 1 : 0, 1);
        P.eq("preview.version_registered", funkgui::images::has(shown().image) && !funkgui::images::has(newest) ? 1 : 0, 1);
        // The keys through the projects' list: nothing asked on the way (a frame a key, each well under the rest).
        {
            const std::size_t kept = processor.previewsKept();
            const uint64_t drawn = processor.previewsDrawn();
            b.chooseProjectNamed("Sketch 1", W, H);
            host.tick(1);
            bool askedNone = true;
            for (int k = 0; k < 6; ++k)
            {
                host.keys(k % 2 == 0 ? "up" : "down");
                host.tick(2);
                processor.poll();
                askedNone = askedNone && processor.previewsKept() == kept && processor.previewsDrawn() == drawn;
            }
            P.eq("preview.keys_ask_nothing", askedNone ? 1 : 0, 1);
            P.eq("preview.keys_then_rest", pump(host, settled) ? 1 : 0, 1);
        }
        // One drawn a poll at most, and none in a poll that drew a picture for Claude.
        {
            // Asked until each is read (the store's Reader) and prepared (the processor's thread), no poll between.
            const auto until = std::chrono::steady_clock::now() + probe::kWorkWait;
            const auto ask = [&](const char* name, int theme) {
                while (port.previewPolled(name, 0, theme).status == St::reading && std::chrono::steady_clock::now() < until)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
            };
            ask("Panel Two", 0);
            ask("Panel Three", 0);
            ask("Panel Two", 1);
            while (processor.previewsQueued() > 0 && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            const std::size_t waiting = processor.previewsToDraw();
            P.eq("preview.three_prepared", static_cast<int>(waiting), 3);
            uint64_t before = processor.previewsDrawn();
            processor.poll();
            P.eq("preview.one_a_poll", static_cast<int>(processor.previewsDrawn() - before), 1);
            P.eq("preview.one_a_poll_left", static_cast<int>(processor.previewsToDraw()), 2);
            // Claude's view_panel waits for this thread's poll: that poll draws it and no preview.
            pg::ChatAgent::McpResult viewed;
            std::thread claude([&] { viewed = processor.chatAgent().mcpCall("view_panel", juce::var(new juce::DynamicObject())); });
            const auto deadline = std::chrono::steady_clock::now() + probe::kWorkWait;
            while (!processor.pictureWaiting() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            before = processor.previewsDrawn();
            processor.poll();
            claude.join();
            P.eq("preview.claude_first", processor.previewsDrawn() == before && !viewed.isError ? 1 : 0, 1);
            processor.poll();
            P.eq("preview.after_claude", static_cast<int>(processor.previewsDrawn() - before), 1);
            processor.poll();
        }
        // Twelve kept: the newest asked for. Each asked until the store's Reader has read its version (a version not read
        // yet makes no entry: the card asks again each frame, and so does this), then prepared and drawn.
        {
            const auto until = std::chrono::steady_clock::now() + probe::kWorkWait;
            for (int t = 0; t < 2; ++t)
                for (const char* name : { "Panel Pad", "Panel Two", "Panel Three", "Sketch 1", "Sketch 2", "Sketch 3", "Deep Sub" })
                {
                    while (port.previewPolled(name, 0, t).status == St::reading && std::chrono::steady_clock::now() < until)
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    processor.previewsSettle(static_cast<double>(probe::kWorkWait.count()));
                }
            P.eq("preview.twelve_kept", static_cast<int>(processor.previewsKept()), static_cast<int>(pg::Processor::kPreviewsKept));
        }
        const int onThread = (pg::projectstore::parses() - parsed0) - (pg::projectstore::readerParses() - reader0);
        std::printf("previews: %llu drawn, %d file(s) read on the message thread\n",
                    static_cast<unsigned long long>(processor.previewsDrawn() - drawn0), onThread);
        P.eq("preview.none_read_on_the_message_thread", onThread, 0);
        // A factory project's (the placeholders have no panel.gui: their dials in rows).
        if (!f0.empty())
        {
            b.chooseProjectNamed(f0, W, H);
            host.tick(1);
            P.eq("preview.factory", pump(host, settled) && b.previewWanted().first == f0 && shown().status == St::ready ? 1 : 0, 1);
            writePng(P, host, "gui.projects.preview.factory.png");
        }
        // Closed: nothing asked or drawn; the picture registered kPictureHoldSeconds more (of the Panel's clock).
        b.chooseProjectNamed("Panel Pad", W, H);
        host.tick(1);
        pump(host, [&] { return shown().status == St::ready; });
        const uint32_t last = shown().image;
        b.close();
        host.settle();
        {
            const uint64_t drawn = processor.previewsDrawn();
            for (int k = 0; k < 30; ++k)
            {
                processor.poll();
                host.tick(1);
            }
            P.eq("preview.closed_draws_nothing", processor.previewsDrawn() == drawn ? 1 : 0, 1);
            P.eq("preview.closed_held", funkgui::images::has(last) ? 1 : 0, 1);
            host.tick(static_cast<int>(60.0 * pg::kPictureHoldSeconds) + 30);
            P.eq("preview.closed_let_go", funkgui::images::has(last) ? 0 : 1, 1);
        }
        // Accessibility: one image, titled for the project.
        {
            panel.openProjects();
            host.settle();
            b.chooseProjectNamed("Panel Pad", W, H);
            host.tick(2);
            std::vector<funkgui::A11yItem> items;
            b.accessibility(items, W, H);
            const auto it = std::find_if(items.begin(), items.end(), [](const funkgui::A11yItem& a) { return a.role == funkgui::A11yRole::image; });
            P.eq("preview.a11y", it != items.end() && it->title == "Preview of Panel Pad's panel" ? 1 : 0, 1);
        }
        // The least window: no pane, four rows of each list; the order, Compare... and Export... where they were.
        {
            const bool least = host.resize(pg::ui::kMinWidth, pg::ui::kMinHeight);
            host.settle();
            const float lw = static_cast<float>(pg::ui::kMinWidth), lh = static_cast<float>(pg::ui::kMinHeight);
            const pg::ui::ProjectBrowser::Places small = b.places(lw, lh);
            std::printf("least window: rows %d %d %d, preview %.0f x %.0f\n", small.rows[0], small.rows[1], small.rows[2], small.preview.w, small.preview.h);
            P.eq("preview.least_none", least && small.preview.w == 0.0f ? 1 : 0, 1);
            P.eq("preview.least_buttons", small.sort.w > 0.0f && small.compare.right() <= small.box.right() && small.exportIt.w > 0.0f ? 1 : 0, 1);
            writePng(P, host, "gui.projects.preview.least.png");
            host.click(4.0f, lh - 4.0f);                      // outside the card: it closes
            host.settle();
            P.eq("preview.least_outside_closes", b.isOpen() ? 0 : 1, 1);
            host.resize(pg::ui::kDefaultWidth, pg::ui::kDefaultHeight);
            host.settle();
        }
    }

    pg::projectstore::setDirectoryForTests({});
    folder.deleteRecursively();
    return P.finish();
}
