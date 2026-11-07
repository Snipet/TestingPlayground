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
// and checks:
//   GROUPS    FACTORY on top, then a category a group by name (told apart case aside: "Pad" and "pad" one group,
//             spelt as its first project), UNCATEGORISED last; each group's projects by name; a click on a group's
//             header folds it (its rows gone, the header there, nothing chosen) and opens it again; the fold kept
//             (this Mac's view)
//   FAVOURITE a click on a row's star stars it: FAVOURITES on top holding it, the project in its own group too; a
//             factory project starred the same; the star again unstars; kept (the view's); a star another instance
//             made while the card is open kept beside one made here
//   CHOOSE    a click chooses one alone; Cmd-click adds one and takes it out again; Shift-click chooses the rows from
//             the last one clicked; a click on one of several chosen chooses it alone on release
//   MENU      a right-click on one of those chosen: a menu for them all ("2 projects"), Add to favourites, Category (the
//             kind's categories and the user's own, None, New category...); a category chosen is theirs (the rest of
//             their info as it was), and None clears it; a factory project's Category and Delete greyed
//   INLINE    New category...: the field names a category (its pill CATEGORY, Save "Set category"), typed and Return
//             set it, the field its project's name again; Escape goes back, nothing set; a category group's Rename...
//             moves every project of it
//   DRAG      two chosen carried onto a category's group take its category; one let go outside the lists, nothing;
//             onto UNCATEGORISED, none; onto FAVOURITES, starred; onto FACTORY, refused (it says so), as it was
//   SEARCH    Cmd-F: the projects whose name, description or a tag holds each word typed, a folded group's matches
//             shown, a group of none gone; Escape clears it and the folds are back; the tag filter's menu (Any tag,
//             each tag with how many have it) keeps those with the tag; the search and the tag remembered when
//             PROJECTS opens again; the clear (x) and Any tag end them
//   SORT      By last saved: each group's newest first; By name: by name again
//   A11Y      a group item a group shown (in the projects' list), its header a toggle (checked while open) and its
//             rows in it, each row's star a toggle in its row; the search a text field, the tag and the order combo
//             boxes; a header pressed folds its group and a star pressed stars
//   EXPLORER  the explorer's PROJECTS: a caption a group (FAVOURITES first, the factory's starred one among them,
//             UNCATEGORISED last), the newest first in each; a click on a caption folds the group there and in
//             PROJECTS (one view), another opens it
//   EXPORT    a project's category goes out with its export and comes back with its import (.fmpack and .zip); the
//             factory projects carry theirs (their info's); EXPORT offers a project's own category as a pill
#include "Probe.h"

#include "editor/Explorer.h"
#include "editor/ExportCard.h"
#include "editor/Panel.h"
#include "editor/ProjectBrowser.h"
#include "editor/ProjectGroups.h"
#include "plugin/EditorPort.h"
#include "plugin/ProjectStore.h"
#include "plugin/Processor.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/prefs/UiPreferences.h>

#include <juce_core/juce_core.h>

#include <algorithm>
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

    // PROJECTS driven through its host: a row scrolled into view first, then clicked.
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
                const int first = b.firstShown(BL::projects), rows = b.places(W, H).rows[0];
                if (row >= first && row < first + rows)
                    return;
                host.wheel(list.centreX(), list.centreY(), row < first ? 1.0f / 6.0f : -1.0f / 6.0f);
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
        drive.click(b.headerOf(gr::keyOf(gr::Kind::category, "Drones")), ctrl());
        const funkgui::MenuRequest* m = host.pendingMenu();
        P.eq("inline.rename_offered", m != nullptr && itemLabelled(m->items, "Rename Drones...") != nullptr ? 1 : 0, 1);
        host.chooseMenuItem("Rename Drones...");
        host.settle();
        P.eq("inline.rename_mode", b.categoryMode() && b.name() == "Drones" ? 1 : 0, 1);
        typeText(host, "Ambient Drones");                // over the name, all of it chosen
        host.keys("return");
        host.settle();
        P.eq("inline.renamed", port.projectInfo("Deep Sub").category == "Ambient Drones"
                                   && b.headerOf(gr::keyOf(gr::Kind::category, "Ambient Drones")) >= 0
                                   && b.headerOf(gr::keyOf(gr::Kind::category, "Drones")) < 0 ? 1 : 0, 1);
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
        P.eq("sort.menu", host.pendingMenu() != nullptr && host.chooseMenuItem("By last saved") ? 1 : 0, 1);
        host.settle();
        bool newest = true;
        for (const gr::Group& g : b.groups())
            for (std::size_t i = 1; i < g.members.size(); ++i)
                newest = newest && b.projectList()[static_cast<std::size_t>(g.members[i - 1])].savedMs
                                       >= b.projectList()[static_cast<std::size_t>(g.members[i])].savedMs;
        P.eq("sort.newest_first", b.view().byDate && newest && membersOf(b, k2).front() == "Night Choir" ? 1 : 0, 1);
        drive.clickRect(b.places(W, H).sort);
        host.chooseMenuItem("By name");
        host.settle();
        P.eq("sort.by_name", !b.view().byDate && membersOf(b, k2).front() == "Glass Bloom" ? 1 : 0, 1);
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
    }

    pg::projectstore::setDirectoryForTests({});
    folder.deleteRecursively();
    return P.finish();
}
