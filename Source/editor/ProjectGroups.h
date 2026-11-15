// Source/editor/ProjectGroups.h: the projects in groups, one way for PROJECTS (ProjectBrowser.h) and the explorer's
// PROJECTS (Explorer.h):
//
//   FAVOURITES     the projects starred (ProjectsView::favourites), the factory's among them: pinned at the top. A
//                  project starred is in its own group too
//   FACTORY        the factory projects (plugin/Factory.h), read-only, whatever their category
//   <a category>   the user's projects of that category (their info's: ProjectInfo::category), one group a category
//                  told apart case aside (titled by the least of its spellings, whatever the order), the groups by name
//   UNCATEGORISED  the user's projects with none, last
//
// Each group's projects in the order asked (Options::order: by name, A to Z, case aside; the newest save first; the
// newest made first; the largest file first; `reversed` turning it around, ties by name, A to Z, case aside), and a
// group of none is left out. A filter (a search, a tag) keeps the projects it matches: each of the search's words in
// the name, the description or a tag (case aside), and the tag one of the project's; a group then left with none is
// left out. A group's key (keyOf: "favourites", "factory", "uncategorised", "category:<its name in lower case>") is
// what the view keeps of a group folded. Plain C++, no JUCE.
//
// While no factory set is in use (EditorPort::factoryNote), FACTORY is kept with no project in it and one row that is
// no project (Group::note; noteRow's words): never chosen, starred, dragged, counted, opened, exported or given a menu,
// and left out while a search or a tag filters (it is no result). Where the store's state calls for one, the row is an
// action (Get them, Try again: pressNote), else text alone.
#pragma once

#include "plugin/EditorPort.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace playground::ui::projectgroups
{
    enum class Kind : uint8_t { favourites, factory, category, uncategorised };

    inline constexpr std::string_view kFavouritesKey = "favourites";
    inline constexpr std::string_view kFactoryKey = "factory";
    inline constexpr std::string_view kUncategorisedKey = "uncategorised";

    std::string lower(std::string_view s);
    bool same(std::string_view a, std::string_view b);   // case aside
    std::string keyOf(Kind kind, std::string_view category = {});

    struct Group
    {
        Kind kind = Kind::uncategorised;
        std::string key;                                 // keyOf's
        std::string title;                               // "Favourites", "Factory", "Bass", "Uncategorised"
        std::string category;                            // a category group's ("" for the others)
        std::vector<int> members;                        // the projects shown, as indexes into the list given, in order
        int total = 0;                                   // the group's projects before the filter
        bool folded = false;
        bool note = false;                               // FACTORY with no set: no project, the note's row instead
    };

    struct Options
    {
        const std::vector<std::string>* favourites = nullptr;   // the projects starred (by name, case aside)
        const std::vector<std::string>* folded = nullptr;       // the groups folded (keys, case aside)
        std::string search;
        std::string tag;
        ProjectOrder order = ProjectOrder::name;
        bool reversed = false;
        bool factoryGroup = true;                        // false: a factory project only among the favourites
        bool factoryNote = false;                        // no set in use: FACTORY kept, its note in place of projects
    };

    bool starred(const std::vector<std::string>* favourites, std::string_view name);
    bool matches(const ProjectEntry& e, std::string_view search, std::string_view tag);
    std::vector<Group> arrange(const std::vector<ProjectEntry>& projects, const Options& options);

    // The FACTORY group's row while no set is in use: what it says, and its action ("": text alone), and both in one
    // line for a screen reader and the status line.
    struct NoteRow
    {
        std::string text;                                // "The factory projects are not on this Mac."
        std::string action;                              // "Get them", "Try again", or ""
        std::string spoken;                              // the two together
    };
    NoteRow noteRow(const FactoryNote& note);
    // The row's action, pressed (the message thread: it only tells the store): Get them, a yes; Try again, a fetch now.
    // What it did, for the status line ("" nothing).
    std::string pressNote(EditorPort& port, const FactoryNote& note);

    // The tags of the projects given (case aside, each once with how many have it), by name; and the user's projects'
    // categories, each once (spelt as its group's title), by name.
    std::vector<std::pair<std::string, int>> tagsOf(const std::vector<ProjectEntry>& projects);
    std::vector<std::string> categoriesOf(const std::vector<ProjectEntry>& projects);
}
