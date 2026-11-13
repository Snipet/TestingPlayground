// Source/editor/ProjectBrowser.h: PROJECTS, a card over the panel, as the key dialog is: the projects saved on this Mac
// (EditorPort::projectsPolled, projectPolled: the store's cache, a file it lacks read on its Reader and shown once read,
// poll()), each a program and its dials kept as versions, with presets (dial settings for it).
//
//   the top     the search field (Cmd-F: the name, the description and the tags, each word; Escape clears it), the
//               tag filter (a dropdown: any tag, or one of those the projects have) and the order (a dropdown: Last
//               modified, the newest save first; Date created, the newest first; Size, the largest file first; Name,
//               A to Z; and Reversed, checked while it turns the order around; the trigger names it, an arrow when
//               reversed): ProjectGroups.h, within each group. A row's right-hand column shows what it is sorted by:
//               the date saved (Last modified and Name), the date made, or the file's size
//   PROJECTS    in groups that fold (a click on a group's header; ProjectGroups.h): FAVOURITES (the projects starred,
//               pinned at the top), FACTORY (the factory projects, EditorPort::factoryProjects: read-only, nothing of
//               them deleted, their category theirs), one a category (the user's projects' info's), UNCATEGORISED
//               last. A row a project: its name, its newest version and when it was saved, the one open marked, and
//               its star (a click stars it or not). A click chooses one (its name goes into the field, its versions
//               and presets show at the right), Cmd-click adds one to the projects chosen or takes it out, Shift-click
//               chooses the rows from the last one clicked; a double-click opens its newest version. The projects
//               chosen take a category: by the context menu (a right-click: Category, its list and New category...,
//               None), by a drag onto a group (a category's, UNCATEGORISED for none, FAVOURITES stars them), or typed
//               (the menu's New category..., or a category group's Rename...: every project of it, whatever a filter
//               shows; the field then names the category, its pill saying CATEGORY; Return sets it, an empty field
//               refused (None clears one), Escape goes back). A category set is spelt as the kind's or as the group it
//               joins spells it, case aside (spellingOf). A factory project's category stays its own. The one open,
//               chosen, puts the name its copy would take into the field ("Warm Pad", "Warm Pad 2" when a project of
//               this Mac's has that); another leaves the field at the program open's (homeName: Save saves the
//               editor's program, never the chosen one's) and says to open it first
//   VERSIONS    the chosen project's, the newest first, each with when and what changed; the one open marked. A click
//               chooses one, a double-click opens it
//   PRESETS     the chosen project's dial settings; the one applied marked. A click chooses one (its name goes into the
//               field, which then names a preset), a click under the rows starts a new one, a double-click applies it
//   the field   the name Save saves under: a project's, or, while the presets are chosen from, a preset's, or a category
//               being named (its pill says which)
//   Save        a project: the editor's files and dials as a new version of the project named (the one open, or a new
//               one; another's, a second click: Add version); a factory project open, a new name saves the user's own
//               copy (EditorPort::saveVersion). A preset: the dials as a preset of the project open (one of that name, a
//               second click: Replace); never a factory project's. A category: Set category
//   Open        the chosen project's newest version, or the chosen version; Apply, the chosen preset (its project
//               opened first when it is not the one open). When the editor's files have changes no version holds, a
//               second click: Open anyway
//   Delete      the chosen project, version or preset (a second click: Delete it)
//   Import...   a .zip or a .fmpack another playground exported, a bundle too (the Panel's chooser), each project of
//               it a project of its own here
//   Export...   the chosen project's info and its export (the Panel's EXPORT card, ExportCard.h); with several chosen,
//               every one of them as one bundle (the context menu's Export too)
//   Git...      the chosen project kept in a git repository (the Panel's GIT card, GitCard.h); none chosen: Add from
//               Git. A linked project's row shows the mark after its name (GitMark.h)
//   A/B...      the editor's files against the chosen version (the newest, unless one is chosen), played in turn while
//               the host plays, with what changed between them (the Panel's A/B card, AbCard.h)
//   Compare...  what changed between two of the chosen project's versions, or a version and the editor (the Panel's
//               COMPARE card, DiffCard.h): the chosen version (the newest, unless one is chosen) against the one before
//               it (the first version: against the editor); the project open with the editor's changes, the chosen
//               version (the one open) against the editor. A version row's menu (a right-click): Compare with the version
//               before, Compare with the editor
//   Close       or Escape, or a press outside the card (the press goes no further; nothing saved or opened, a
//               confirmation asked for withdrawn; a drag that began inside and ends outside closes nothing). Return
//               saves when a name was typed, else opens (applies); Up and Down choose in the list
//               chosen from, Tab moves to the next list
// Each list scrolls as the code does (Scroll.h: the wheel over it, its thumb dragged, a press on its track), by px:
// a row at its edge shows in part.
// The stars, the groups folded, the search, the tag and the order are this Mac's (EditorPort::projectsView): kept as
// they change, the same in every instance and the explorer's PROJECTS (which groups as this does). While a search or
// a tag filters, every group with a match shows its matches (a fold then is the filter's alone, the groups folded kept
// for after it). While it is open it takes every pointer event and key. What it did, it says once (takeNotice) for the
// status line. Its menus are the host's (the context menu, the tag filter's, the order's); a menu's answer calls the
// Panel's hook (setAnswered) as an event does.
#pragma once

#include "editor/ProjectGroups.h"
#include "editor/Scroll.h"
#include "plugin/EditorPort.h"

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>
#include <funkgui/text/LineEdit.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace playground::ui
{
    class ProjectBrowser
    {
    public:
        static constexpr int kMaxNameLength = 40;        // projectstore::kMaxName
        static constexpr int kMaxCategoryLength = 24;    // projectstore::kMaxTag
        static constexpr int kMaxSearchLength = 60;      // projectstore::kMaxSearch
        static constexpr uint32_t kA11yBase = 0x7FFE0000u;   // its accessibility ids, past every item's

        enum class List : uint8_t { projects, versions, presets };

        explicit ProjectBrowser(EditorPort& port) : port_(port) {}

        bool isOpen() const noexcept { return open_; }
        // The lists read afresh, the project open chosen; the field `name` (a project's), or, with `preset`, the
        // presets chosen from and the field the preset applied or a new preset's name.
        void open(const std::string& name, bool preset);
        void setSize(float width, float height) noexcept { width_ = width; height_ = height; }   // the panel's, before open()
        void close();
        // While open: the lists read again after a change made elsewhere (Claude's projects tool saved, opened or applied
        // one), the project chosen kept, its versions and presets read afresh, the version and the preset chosen kept by
        // number and name; a confirmation asked for is withdrawn (the next click asks again, the words saying what
        // changed them: `why`), a press let go.
        enum class Because : uint8_t
        {
            claude,                                      // Claude's projects tool
            elsewhere,                                   // a file the store's Reader read (poll)
            saved                                        // the editor's own save, landed (Cmd-S here or in the code window)
        };
        void reread(Because why = Because::claude);
        // While open: the lists read again (reread) only when what it shows differs from the store's cache or the project
        // open, so that a confirmation asked for stands over lists that did not change. Whether it read them.
        bool rereadIfChanged(Because why);
        // While open, each frame: the lists read again once the store's Reader has read a file (EditorPort::projectsRead):
        // the card reads the store's cache alone (projectsPolled, projectPolled), never a project's file whole on the
        // message thread, so one it lacks shows when the Reader has it. Whether it read them.
        bool poll();
        bool chosenRead() const noexcept { return chosenRead_; }   // the chosen project's file read (else: reading)
        // ---- Package 04: the preview (Preview.h; EditorPort::previewPolled) -------------------------------------------
        // While open, each frame (the Panel's refresh): the chosen project's version (the one chosen, else its newest)
        // asked for kPreviewRest seconds after the choice rests (the keys through a list ask for nothing on the way), in
        // FunkGui's theme `theme`. Whether what it shows changed. previewShown(): what the pane shows (the Panel
        // registers its picture, Panel::syncPreview).
        static constexpr double kPreviewRest = 0.15;
        bool pollPreview(double nowSeconds, int theme);
        const PreviewState& previewShown() const noexcept { return preview_; }
        std::pair<std::string, int> previewWanted() const;      // the project and the number (0: newest) it shows
        // Whether its Save started a save of the editor's (EditorPort::startSave), once: the Panel then says SAVING and,
        // when it has ended, what came of it.
        bool takeStartedSave();

        // Its places, for the panel's size.
        struct Places
        {
            funkgui::Rect box, projects, versions, presets, mode, field, save, open, remove, close, importIt, exportIt, ab;
            funkgui::Rect compare;                       // Compare..., after A/B...
            funkgui::Rect git;                           // Git..., after Export... (package 05: GitCard.h)
            funkgui::Rect search, clear, tag, sort;      // the top's: the search field, its clear (x), the tag, the order
            funkgui::Rect preview;                       // the chosen version's panel, over the versions (w 0: none, a
                                                         // window too short for it and four rows of each list)
            float rowH = 28.0f;
            std::array<int, 3> rows{};                   // how many rows each list shows
        };
        Places places(float width, float height) const;

        // Events, the panel's size given. The pointer goes nowhere else while it is open. `host` shows its menus.
        void pointerDown(const funkgui::PointerEvent& e, funkgui::HostServices* host, float width, float height);
        void pointerDrag(funkgui::Point p, float width, float height);
        void pointerUp(funkgui::Point p, float width, float height);
        void doubleClick(funkgui::Point p, float width, float height);
        void wheel(const funkgui::WheelEvent& e, float width, float height);
        void key(const funkgui::KeyEvent& e, funkgui::HostServices* host, float width, float height);
        funkgui::Cursor cursor(funkgui::Point p, float width, float height) const;
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, float width, float height, funkgui::Point pointer,
                  bool hasPointer) const;
        void accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const;
        bool a11yAction(uint32_t id, funkgui::A11yAction a, funkgui::HostServices* host, float width, float height);
        uint32_t revision() const noexcept { return revision_; }   // bumps when its items change
        // The Panel's: a menu's answer has acted (afterProjects), and whether a menu opens at a rect (its trigger then
        // draws open).
        void setAnswered(std::function<void()> answered) { answered_ = std::move(answered); }
        void setMenuOpen(std::function<bool(const funkgui::Rect&)> menuOpen) { menuOpen_ = std::move(menuOpen); }

        // What it did last ("SAVED: GLASS BLOOM V3"), once; "" for nothing new.
        std::string takeNotice();
        // What the Panel does next, once: Import's chooser; EXPORT for that project ("" for none); A/B for that project
        // and version (0: its newest; the project "" for none).
        bool takeImport();
        std::vector<std::string> takeExport();           // every project to export (several: a bundle); none: {}
        // Git... pressed (package 05: the Panel opens GitCard.h): the chosen project ("": none, Add from Git).
        std::pair<bool, std::string> takeGit();
        std::pair<std::string, int> takeAb();
        std::tuple<std::string, int, int> takeCompare(); // COMPARE of that project's sides (left, right; 0 the editor)
        void chooseProjectNamed(const std::string& name, float width, float height);
        // The name a factory project's copy takes: its own, else "<own> 2", ... (case aside, as the store's add()).
        std::string copyName(const std::string& factory) const;

        // For probes. The projects (this Mac's, then the factory's), and the rows of the projects' list: a group's
        // header (its project -1) or a project of it.
        struct Row
        {
            int group = -1;                              // in groups()
            int project = -1;                            // in projectList(); -1: the group's header
        };
        const std::vector<ProjectEntry>& projectList() const noexcept { return projects_; }
        const std::vector<projectgroups::Group>& groups() const noexcept { return groups_; }
        const std::vector<Row>& rowList() const noexcept { return rows_; }
        const ProjectEntry* rowProject(int row) const;   // none for a header
        int rowOf(const std::string& project, bool inFavourites = false) const;   // its row (-1: not shown)
        int headerOf(const std::string& key) const;      // a group's header row (-1: none)
        const std::vector<VersionEntry>& versionList() const noexcept { return versions_; }
        const std::vector<PresetEntry>& presetList() const noexcept { return presets_; }
        int chosen(List l) const noexcept;               // the projects': the chosen project's row (-1: none shown)
        int firstShown(List l) const;                    // the first row wholly shown
        float scrolled(List l) const;                    // px (Scroll::offset), clamped to the list as it is
        Scroll scrollOf(const Places& at, List l) const; // its scroll: content and view as the places have them
        funkgui::Rect barArea(const Places& at, List l) const;   // where its bar is (Scroll's area)
        std::string chosenProject() const { return chosenName_; }
        const std::vector<std::string>& selection() const noexcept { return selected_; }   // the projects chosen
        const ProjectsView& view() const noexcept { return view_; }
        bool searching() const noexcept { return searching_; }   // keys type the search
        const std::string& search() const noexcept { return search_.buffer; }
        bool categoryMode() const noexcept { return categoryMode_; }
        bool dragging() const noexcept { return dragging_; }
        List focus() const noexcept { return focus_; }
        bool presetMode() const noexcept { return focus_ == List::presets && !categoryMode_; }
        const std::string& name() const noexcept { return name_.buffer; }
        const std::string& why() const noexcept { return why_; }
        std::string saveLabel() const;
        std::string orderLabel() const;                  // the order's trigger: "Last modified", "Size" (an arrow drawn
                                                         // after it while reversed: view().reversed)
        std::string rowFacts(const ProjectEntry& e) const;   // a row's right-hand column: its version and what the list is
                                                         // sorted by (the date saved or made, the size)
        std::string openLabel() const;
        std::string removeLabel() const;
        funkgui::Point centreOfRow(List l, int i, float width, float height) const;   // a row wholly shown, else far away
        funkgui::Point centreOfEmpty(List l, float width, float height) const;        // under its rows, else far away
        funkgui::Point centreOfStar(int row, float width, float height) const;        // a project row's star
        // The categories its menus offer: the kind's (ExportCard::categories()), then the user's others, by name.
        std::vector<std::string> categoryChoices() const;

    private:
        enum class Ask : uint8_t { none, addTo, replace, open, remove };
        enum class Menu : uint8_t { row, header, tag, sort, version };

        funkgui::Rect listRect(const Places& at, List l) const;
        funkgui::Rect rowRect(const Places& at, List l, int i) const;   // a row (its index), scrolled
        funkgui::Rect rowsArea(const Places& at, List l) const;         // the rows' part of the list: their clip
        bool wholly(const Places& at, List l, int i) const;             // a row wholly in view
        int firstWhole(const Places& at, List l) const;                 // the first row wholly in view
        void reveal(List l, int row, int rowsShown);     // the least scroll that shows the row (the keys, a choice)
        void scrollToRow(List l, int row);               // the row at the top, as far as the list scrolls
        static funkgui::Rect starRect(const funkgui::Rect& row);
        int count(List l) const;
        int rowAt(funkgui::Point p, const Places& at, List l) const;
        void readProjects();                             // this Mac's, then the factory's; arranged
        void arrange();                                  // the groups and the rows, from the projects and the view
        bool filtering() const noexcept { return !view_.search.empty() || !view_.tag.empty(); }
        void keepView();                                 // what changed of the view to the port's (this Mac's), merged
                                                         // (the stars and the folds item by item)
        bool starredName(const std::string& name) const;
        void star(const std::vector<std::string>& names, bool on);
        void toggleFold(int group);
        void setSearch(const std::string& text);
        std::string spellingOf(const std::string& typed, const std::vector<std::string>& setting) const;
        void setCategory(const std::vector<std::string>& names, const std::string& typed);   // as spellingOf spells it
        void startCategory(const std::vector<std::string>& names, const std::string& now);   // the field names one
        void endCategory();
        void dropOn(int group);                          // the projects dragged, onto a group
        void showMenu(Menu m, int row, funkgui::Point at);
        void menuChosen(Menu m, int row, int id, const std::vector<std::string>& names, const std::vector<std::string>& choices);
        std::vector<std::string> targets(int row) const; // the projects a row's menu or drag acts on (the chosen ones,
                                                         // when the row is one of them)
        std::string homeName() const;                    // the field for the program open: its project's (copy's), else its title
        std::string fieldFor(const std::string& project) const;   // the field when `project` is chosen
        bool factoryChosen() const;                      // the chosen project is a factory one
        void readChosen();                               // the chosen project's versions and presets
        void chooseProject(int row, int rowsShown);      // a project's row: it alone chosen
        void choose(List l, int i, int rowsShown, int step = 1);   // a header skipped in the step's direction
        void startPreset();                              // the presets chosen from, the field a new preset's name
        void save();
        void openChosen();
        void remove();
        bool chosenIsOpen() const;                       // the chosen project is the one open
        bool listsChanged() const;                       // rereadIfChanged's and poll()'s test

        EditorPort& port_;
        bool open_ = false;
        std::vector<ProjectEntry> projects_;
        std::vector<projectgroups::Group> groups_;
        std::vector<Row> rows_;
        ProjectsView view_;                              // as the port had it, and as it changed since
        ProjectsView kept_;                              // as it last read or wrote it (what changed since: view_'s)
        std::vector<std::string> filterFolded_;          // the groups folded while a filter is on
        std::vector<VersionEntry> versions_;
        std::vector<PresetEntry> presets_;
        bool chosenRead_ = true;                         // the chosen project's detail read (else the Reader reads it)
        bool startedSave_ = false;                       // takeStartedSave
        uint64_t readSeen_ = 0;                          // EditorPort::projectsRead as the lists were last read
        ProjectState state_;                             // EditorPort::project when it opened, or after it acted
        std::string title_;                              // the name it opened with (the program's title when none is open)
        std::string chosenName_;                         // the chosen project ("" for none)
        std::string chosenGroup_;                        // the key of the group it was chosen in (a favourite has two)
        std::vector<std::string> selected_;              // the projects chosen, the chosen one among them
        int anchor_ = -1;                                // the row a Shift-click chooses from
        std::array<int, 3> chosen_{ -1, -1, -1 };        // the versions' and the presets' (the projects': chosenName_)
        std::array<Scroll, 3> scroll_{};                 // each list's (its offset kept; content and view: scrollOf)
        int barList_ = -1;                               // the list whose thumb is dragged
        List focus_ = List::projects;
        funkgui::text::LineEdit name_;
        funkgui::text::LineEdit search_;
        bool searching_ = false;
        bool typed_ = false;                             // a name typed since it opened, or since a row was chosen
        bool categoryMode_ = false;                      // the field names a category for categoryFor_
        std::vector<std::string> categoryFor_;
        std::string nameBefore_;                         // the field before it named a category
        Ask ask_ = Ask::none;                            // what the next click on that button does
        std::string why_;                                // the line under the field
        std::string notice_;
        bool import_ = false;
        std::vector<std::string> export_;
        bool git_ = false;                               // package 05: takeGit
        std::string gitProject_;
        std::pair<std::string, int> ab_;
        std::tuple<std::string, int, int> compare_{ "", 0, 0 };
        // Compare...'s sides for the chosen project: `how` 0 as the button does (DiffCard.h), 1 a version row's "with
        // the version before", 2 its "with the editor"; `row` the version row (-1: the chosen one).
        void compareChosen(int how, int row = -1);
        // A press on a project's row, and the drag it may become.
        int pressRow_ = -1;
        funkgui::Point pressAt_{};
        bool dragging_ = false;
        bool chooseOnUp_ = false;                        // a press on one of several chosen: the row alone on release
        funkgui::Point dragAt_{};
        std::vector<std::string> dragged_;
        float width_ = 0.0f, height_ = 0.0f;             // the panel's, at the last event
        funkgui::HostServices* host_ = nullptr;
        std::function<void()> answered_;
        std::function<bool(const funkgui::Rect&)> menuOpen_;
        uint32_t revision_ = 0;
        // Package 04's preview: what was chosen and when, and what the port last said of it.
        std::pair<std::string, int> previewWant_{ "", 0 };
        double previewSince_ = 0.0, previewNow_ = 0.0;
        bool previewMoved_ = true;                       // the choice changed since previewSince_ (the clock starts at
                                                         // the next pollPreview)
        PreviewState preview_;
    };
}
