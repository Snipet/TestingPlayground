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
    };

    bool starred(const std::vector<std::string>* favourites, std::string_view name);
    bool matches(const ProjectEntry& e, std::string_view search, std::string_view tag);
    std::vector<Group> arrange(const std::vector<ProjectEntry>& projects, const Options& options);

    // The tags of the projects given (case aside, each once with how many have it), by name; and the user's projects'
    // categories, each once (spelt as its group's title), by name.
    std::vector<std::pair<std::string, int>> tagsOf(const std::vector<ProjectEntry>& projects);
    std::vector<std::string> categoriesOf(const std::vector<ProjectEntry>& projects);
}
