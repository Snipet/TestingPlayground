// Source/editor/ProjectBrowser.cpp: see ProjectBrowser.h.
#include "editor/ProjectBrowser.h"

#include "editor/ChatItems.h"
#include "editor/ChatStyle.h"
#include "editor/Dropdown.h"
#include "editor/ExportCard.h"
#include "editor/Items.h"
#include "editor/Theme.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace playground::ui
{
    namespace
    {
        constexpr float kBoxW = 860.0f, kBoxH = 640.0f;
        constexpr float kPad = 24.0f;
        constexpr float kListTop = 100.0f;               // below the title, the folder's line and the lists' captions
        constexpr float kBelowLists = 132.0f;            // the field, its line, the buttons
        constexpr float kGap = 16.0f;                    // between the two columns
        constexpr float kCaptionH = 24.0f;               // the presets' caption, between the versions and them
        constexpr float kModeW = 78.0f;                  // the field's pill: PROJECT, PRESET or CATEGORY
        constexpr float kButtonH = 32.0f;
        constexpr float kStarW = 26.0f;                  // a project row's star, at its right
        constexpr float kDragAfter = 5.0f;               // px a press moves before it is a drag
        constexpr float kTopH = 32.0f;                   // the search field, the tag's and the order's dropdowns
        constexpr float kTagW = 140.0f, kSortW = 150.0f, kSearchX = 150.0f;
        constexpr uint32_t kBoxId = ProjectBrowser::kA11yBase, kListId = kBoxId + 1,   // + the list
                           kFieldId = kBoxId + 4, kSaveId = kBoxId + 5, kOpenId = kBoxId + 6, kRemoveId = kBoxId + 7,
                           kCloseId = kBoxId + 8, kImportId = kBoxId + 9, kExportId = kBoxId + 10, kAbId = kBoxId + 11,
                           kSearchId = kBoxId + 12, kClearId = kBoxId + 13, kTagId = kBoxId + 14, kSortId = kBoxId + 15,
                           kRowId = kBoxId + 0x100,                                     // + the list * 0x1000 + the row
                           kGroupId = kBoxId + 0x4000,                                  // + the group
                           kStarId = kBoxId + 0x5000;                                   // + the row
        // The menus' ids.
        constexpr int kOpenItem = 1, kStarItem = 2, kExportItem = 3, kDeleteItem = 4, kNewCategory = 10, kNoCategory = 11,
                      kFoldAll = 20, kUnfoldAll = 21, kRename = 22, kAnyTag = 1, kByName = 1, kByDate = 2, kChoice = 100;

        using L = ProjectBrowser::List;
        using projectgroups::Kind;
        using projectgroups::same;
        constexpr std::size_t idx(L l) { return static_cast<std::size_t>(l); }

        float pillW(const std::string& label) { return std::max(84.0f, std::round(7.0f * static_cast<float>(label.size()) + 34.0f)); }

        std::string v(int n) { return "v" + std::to_string(n); }
        std::string shortWhen(const std::string& when) { return when.size() > 5 ? when.substr(5) : when; }   // "10-27 14:02"

        bool has(const std::vector<std::string>& names, const std::string& name)
        {
            return std::any_of(names.begin(), names.end(), [&](const std::string& n) { return same(n, name); });
        }

        // A typed category as the store keeps one (projectstore::tidyInfo): printable, the spaces collapsed and trimmed,
        // cut to its length.
        std::string tidyCategory(const std::string& typed)
        {
            std::string out;
            bool space = false;
            for (const char ch : typed)
            {
                const auto c = static_cast<unsigned char>(ch);
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                {
                    space = !out.empty();
                    continue;
                }
                if (space)
                    out += ' ';
                space = false;
                out += c >= 0x20 && c <= 0x7E ? ch : '?';
            }
            if (out.size() > static_cast<std::size_t>(ProjectBrowser::kMaxCategoryLength))
                out.resize(static_cast<std::size_t>(ProjectBrowser::kMaxCategoryLength));
            while (!out.empty() && out.back() == ' ')
                out.pop_back();
            return out;
        }

        // "\"Glass Bloom\"" for one, "3 projects" for more.
        std::string them(const std::vector<std::string>& names)
        {
            return names.size() == 1 ? "\"" + ownName(names[0]) + "\"" : std::to_string(names.size()) + " projects";
        }
    }

    void ProjectBrowser::readProjects()
    {
        readSeen_ = port_.projectsRead();                // first: a read that lands meanwhile reads them again (poll)
        projects_ = port_.projectsPolled();
        const std::vector<ProjectEntry> factory = port_.factoryProjects();
        projects_.insert(projects_.end(), factory.begin(), factory.end());
        arrange();
    }

    void ProjectBrowser::arrange()
    {
        projectgroups::Options o;
        o.favourites = &view_.favourites;
        o.folded = filtering() ? &filterFolded_ : &view_.folded;
        o.search = view_.search;
        o.tag = view_.tag;
        o.order = view_.order;
        o.reversed = view_.reversed;
        groups_ = projectgroups::arrange(projects_, o);
        rows_.clear();
        for (int g = 0; g < static_cast<int>(groups_.size()); ++g)
        {
            rows_.push_back({ g, -1 });
            if (!groups_[static_cast<std::size_t>(g)].folded)
                for (const int m : groups_[static_cast<std::size_t>(g)].members)
                    rows_.push_back({ g, m });
        }
        const auto listed = [&](const std::string& name) {
            return std::any_of(projects_.begin(), projects_.end(), [&](const ProjectEntry& e) { return same(e.name, name); });
        };
        selected_.erase(std::remove_if(selected_.begin(), selected_.end(), [&](const std::string& n) { return !listed(n); }), selected_.end());
        if (!chosenName_.empty() && !listed(chosenName_))
        {
            chosenName_.clear();
            readChosen();
        }
        anchor_ = std::min(anchor_, static_cast<int>(rows_.size()) - 1);
        // The list keeps its place, as far as its rows reach (a group folded, a filter: fewer).
        const int shown = width_ > 0.0f ? places(width_, height_).rows[idx(L::projects)] : 1;
        first_[idx(L::projects)] = std::clamp(first_[idx(L::projects)], 0, std::max(0, static_cast<int>(rows_.size()) - shown));
        ++revision_;
    }

    // What this card changed since it last read or wrote the view, over the view as it is now: another instance's
    // changes meanwhile (a star, a fold) stay. view_ is then that, as kept.
    void ProjectBrowser::keepView()
    {
        const ProjectsView now = port_.projectsView();
        ProjectsView out = now;
        // A list (the stars, the groups folded): what this card took out of it and put in since, over the list now.
        const auto merge = [](std::vector<std::string>& list, const std::vector<std::string>& kept, const std::vector<std::string>& mine) {
            for (const std::string& n : kept)            // taken out here
                if (!has(mine, n))
                    list.erase(std::remove_if(list.begin(), list.end(), [&](const std::string& x) { return same(x, n); }), list.end());
            for (const std::string& n : mine)            // put in here
                if (!has(kept, n) && !has(list, n))
                    list.push_back(n);
        };
        merge(out.favourites, kept_.favourites, view_.favourites);
        merge(out.folded, kept_.folded, view_.folded);
        if (view_.search != kept_.search)
            out.search = view_.search;
        if (view_.tag != kept_.tag)
            out.tag = view_.tag;
        if (view_.order != kept_.order || view_.reversed != kept_.reversed)
        {
            out.order = view_.order;
            out.reversed = view_.reversed;
        }
        std::string why;
        if (out != now && !port_.setProjectsView(out, why))
        {
            why_ = "Not kept: " + why + ".";
            return;
        }
        view_ = kept_ = out;
        if (search_.buffer != view_.search)
            search_.set(view_.search, kMaxSearchLength);
    }

    bool ProjectBrowser::starredName(const std::string& name) const { return has(view_.favourites, name); }

    void ProjectBrowser::star(const std::vector<std::string>& names, bool on)
    {
        for (const std::string& n : names)
        {
            std::vector<std::string>& f = view_.favourites;
            if (on && !has(f, n))
                f.push_back(n);
            else if (!on)
                f.erase(std::remove_if(f.begin(), f.end(), [&](const std::string& x) { return same(x, n); }), f.end());
        }
        keepView();
        arrange();
    }

    void ProjectBrowser::toggleFold(int group)
    {
        if (group < 0 || group >= static_cast<int>(groups_.size()))
            return;
        const std::string key = groups_[static_cast<std::size_t>(group)].key;
        std::vector<std::string>& folded = filtering() ? filterFolded_ : view_.folded;
        if (has(folded, key))
            folded.erase(std::remove_if(folded.begin(), folded.end(), [&](const std::string& k) { return same(k, key); }), folded.end());
        else
            folded.push_back(key);
        if (!filtering())
            keepView();
        arrange();
    }

    void ProjectBrowser::setSearch(const std::string& text)
    {
        if (text == view_.search)
            return;
        const bool was = filtering();
        view_.search = text;
        if (!was && filtering())
            filterFolded_.clear();                       // a filter shows every group with a match
        first_[idx(L::projects)] = 0;
        arrange();                                       // kept as it closes, or with the next change kept
    }

    std::vector<std::string> ProjectBrowser::categoryChoices() const
    {
        std::vector<std::string> out = ExportCard::categories();
        for (const std::string& c : projectgroups::categoriesOf(projects_))
            if (!has(out, c))
                out.push_back(c);
        return out;
    }

    // A category as the projects keep it: tidied as the store tidies one, spelt as the kind's when it is one case aside
    // (so EXPORT lights its pill), else as the projects of it not being set spell it (a group keeps one spelling: the
    // least, as ProjectGroups titles it), else as typed. Those being set are left out, so a rename may change the case.
    std::string ProjectBrowser::spellingOf(const std::string& typed, const std::vector<std::string>& setting) const
    {
        const std::string c = tidyCategory(typed);
        if (c.empty())
            return c;
        for (const std::string& k : ExportCard::categories())
            if (same(k, c))
                return k;
        std::string spelt;
        for (const ProjectEntry& e : projects_)
            if (!e.factory && same(e.category, c) && !has(setting, e.name) && (spelt.empty() || e.category < spelt))
                spelt = e.category;
        return spelt.empty() ? c : spelt;
    }

    void ProjectBrowser::setCategory(const std::vector<std::string>& names, const std::string& typed)
    {
        const std::string category = spellingOf(typed, names);
        int done = 0, factory = 0;
        std::string failed;
        for (const std::string& n : names)
        {
            if (isFactoryName(n))
            {
                ++factory;
                continue;
            }
            std::string why;
            if (port_.setProjectCategory(n, category, why))
                ++done;
            else
                failed = why;
        }
        readProjects();
        std::vector<std::string> mine;
        for (const std::string& n : names)
            if (!isFactoryName(n))
                mine.push_back(n);
        const std::string where = category.empty() ? "Uncategorised" : category;
        why_ = done == 0                           ? std::string()
             : done == 1 && mine.size() == 1 ? them(mine) + " is in " + where + " now."
                                                 : std::to_string(done) + " projects are in " + where + " now.";
        if (factory > 0)
            why_ += std::string(why_.empty() ? "" : " ") + (factory == 1 ? "A factory project's" : "The factory projects'")
                  + " category stays as it is.";
        if (!failed.empty())
            why_ += std::string(why_.empty() ? "" : " ") + "Not set: " + failed + ".";
        ask_ = Ask::none;
        ++revision_;
    }

    void ProjectBrowser::startCategory(const std::vector<std::string>& names, const std::string& now)
    {
        std::vector<std::string> mine;
        for (const std::string& n : names)
            if (!isFactoryName(n))
                mine.push_back(n);
        if (mine.empty())
        {
            why_ = "A factory project's category stays as it is.";
            ++revision_;
            return;
        }
        if (!categoryMode_)
            nameBefore_ = name_.buffer;
        categoryMode_ = true;
        categoryFor_ = mine;
        name_.set(now, kMaxCategoryLength);
        name_.allSelected = !now.empty();
        typed_ = false;
        ask_ = Ask::none;
        searching_ = false;
        why_ = "A category for " + them(mine) + ": Return sets it, Escape goes back.";
        ++revision_;
    }

    void ProjectBrowser::endCategory()
    {
        if (!categoryMode_)
            return;
        categoryMode_ = false;
        categoryFor_.clear();
        name_.set(nameBefore_, kMaxNameLength);
        why_.clear();
        ++revision_;
    }

    void ProjectBrowser::dropOn(int group)
    {
        if (group < 0 || group >= static_cast<int>(groups_.size()) || dragged_.empty())
            return;
        const projectgroups::Group& g = groups_[static_cast<std::size_t>(group)];
        const std::vector<std::string> names = dragged_;
        switch (g.kind)
        {
            case Kind::favourites:
                star(names, true);
                why_ = them(names) + (names.size() == 1 ? " is" : " are") + " among the favourites now.";
                break;
            case Kind::factory:
                why_ = "Factory holds the factory projects: a project of yours keeps a group of its own.";
                break;
            case Kind::category:
                setCategory(names, g.category);
                break;
            case Kind::uncategorised:
                setCategory(names, {});
                break;
        }
        ++revision_;
    }

    std::vector<std::string> ProjectBrowser::targets(int row) const
    {
        const ProjectEntry* e = rowProject(row);
        if (e == nullptr)
            return {};
        if (has(selected_, e->name))
            return selected_;
        return { e->name };
    }

    void ProjectBrowser::showMenu(Menu m, int row, funkgui::Point at)
    {
        if (host_ == nullptr)
            return;
        const Places spots = places(width_, height_);
        funkgui::MenuRequest request;
        std::vector<std::string> names, choices;
        int current = 0;
        switch (m)
        {
            case Menu::row:
            {
                names = targets(row);
                if (names.empty())
                    return;
                const bool one = names.size() == 1;
                const bool mine = std::any_of(names.begin(), names.end(), [](const std::string& n) { return !isFactoryName(n); });
                const bool all = std::all_of(names.begin(), names.end(), [&](const std::string& n) { return starredName(n); });
                request.items.push_back(dropdown::header(one ? ownName(names[0]) : std::to_string(names.size()) + " projects"));
                if (one)
                    request.items.push_back({ kOpenItem, "Open" });
                request.items.push_back({ kStarItem, all ? "Remove from favourites" : "Add to favourites" });
                // The category: a set of choices, the one they share dotted.
                std::string shared;
                bool same1 = true;
                for (const std::string& n : names)
                    for (const ProjectEntry& e : projects_)
                        if (!e.factory && same(e.name, n))
                        {
                            if (shared.empty() && same1)
                                shared = e.category.empty() ? std::string("\n") : e.category;   // "\n": none
                            else if (!same(shared, e.category.empty() ? std::string("\n") : e.category))
                                same1 = false;
                        }
                choices = categoryChoices();
                funkgui::MenuItem category;
                category.label = "Category";
                category.enabled = mine;
                for (std::size_t i = 0; i < choices.size(); ++i)
                    category.submenu.push_back(dropdown::choice(kChoice + static_cast<int>(i), choices[i], same1 && same(shared, choices[i])));
                category.submenu.push_back(dropdown::separator());
                category.submenu.push_back(dropdown::choice(kNoCategory, "None (Uncategorised)", same1 && shared == "\n"));
                category.submenu.push_back({ kNewCategory, "New category..." });
                request.items.push_back(std::move(category));
                if (one)
                {
                    request.items.push_back(dropdown::separator());
                    request.items.push_back({ kExportItem, "Export..." });
                    request.items.push_back({ kDeleteItem, "Delete", !isFactoryName(names[0]) });
                }
                request.anchor = { at.x, at.y, 1.0f, 1.0f };
                break;
            }
            case Menu::header:
            {
                const ProjectBrowser::Row r = row >= 0 && row < static_cast<int>(rows_.size()) ? rows_[static_cast<std::size_t>(row)] : Row{};
                if (r.group < 0)
                    return;
                const projectgroups::Group& g = groups_[static_cast<std::size_t>(r.group)];
                request.items.push_back(dropdown::header(g.title));
                request.items.push_back({ kFoldAll, "Fold every group" });
                request.items.push_back({ kUnfoldAll, "Unfold every group" });
                if (g.kind == Kind::category)
                {
                    // Every project of the category, not only those a search or a tag shows (members): a rename
                    // never splits a group.
                    for (const ProjectEntry& e : projects_)
                        if (!e.factory && same(e.category, g.category))
                            names.push_back(e.name);
                    request.items.push_back(dropdown::separator());
                    request.items.push_back({ kRename, "Rename " + g.title + "..." });
                }
                request.anchor = { at.x, at.y, 1.0f, 1.0f };
                break;
            }
            case Menu::tag:
            {
                request.items.push_back(dropdown::choice(kAnyTag, "Any tag", view_.tag.empty()));
                const auto tags = projectgroups::tagsOf(projects_);
                if (!tags.empty())
                    request.items.push_back(dropdown::header("Tags"));
                for (std::size_t i = 0; i < tags.size(); ++i)
                {
                    choices.push_back(tags[i].first);
                    funkgui::MenuItem it = dropdown::choice(kChoice + static_cast<int>(i), tags[i].first, same(tags[i].first, view_.tag));
                    it.shortcut = std::to_string(tags[i].second);   // how many have it
                    request.items.push_back(std::move(it));
                    if (same(tags[i].first, view_.tag))
                        current = kChoice + static_cast<int>(i);
                }
                if (current == 0)
                    current = kAnyTag;
                dropdown::dropdownMenu(request, spots.tag, current);
                break;
            }
            case Menu::sort:
                request.items.push_back(dropdown::choice(kByName, "By name", view_.order != ProjectOrder::saved));
                request.items.push_back(dropdown::choice(kByDate, "By last saved", view_.order == ProjectOrder::saved));
                dropdown::dropdownMenu(request, spots.sort, view_.order == ProjectOrder::saved ? kByDate : kByName);
                break;
        }
        request.theme = productTheme(funkgui::Theme::byIndex(host_->themeIndex()));
        host_->showMenu(request, [this, m, row, names, choices](int id) {
            if (!open_ || id <= 0)
                return;
            menuChosen(m, row, id, names, choices);
            if (answered_)
                answered_();
        });
    }

    void ProjectBrowser::menuChosen(Menu m, int row, int id, const std::vector<std::string>& names,
                                    const std::vector<std::string>& choices)
    {
        const auto choice = [&](int k) -> const std::string* {
            return k >= kChoice && k - kChoice < static_cast<int>(choices.size()) ? &choices[static_cast<std::size_t>(k - kChoice)] : nullptr;
        };
        switch (m)
        {
            case Menu::row:
                if (id == kOpenItem)
                {
                    focus_ = L::projects;                // its newest version
                    openChosen();
                }
                else if (id == kStarItem)
                {
                    const bool all = std::all_of(names.begin(), names.end(), [&](const std::string& n) { return starredName(n); });
                    star(names, !all);
                }
                else if (id == kExportItem && !names.empty())
                {
                    export_ = names[0];
                    close();
                }
                else if (id == kDeleteItem)
                {
                    focus_ = L::projects;
                    remove();                            // asks first: Delete it does
                }
                else if (id == kNewCategory)
                    startCategory(names, {});
                else if (id == kNoCategory)
                    setCategory(names, {});
                else if (const std::string* c = choice(id))
                    setCategory(names, *c);
                break;
            case Menu::header:
                if (id == kFoldAll || id == kUnfoldAll)
                {
                    std::vector<std::string>& folded = filtering() ? filterFolded_ : view_.folded;
                    folded.clear();
                    if (id == kFoldAll)
                        for (const projectgroups::Group& g : groups_)
                            folded.push_back(g.key);
                    if (!filtering())
                        keepView();
                    arrange();
                }
                else if (id == kRename && row >= 0 && row < static_cast<int>(rows_.size()))
                    startCategory(names, groups_[static_cast<std::size_t>(rows_[static_cast<std::size_t>(row)].group)].title);
                break;
            case Menu::tag:
            {
                const bool was = filtering();
                const std::string* c = choice(id);
                view_.tag = c != nullptr ? *c : std::string();
                if (!was && filtering())
                    filterFolded_.clear();
                first_[idx(L::projects)] = 0;
                keepView();
                arrange();
                break;
            }
            case Menu::sort:
                view_.order = id == kByDate ? ProjectOrder::saved : ProjectOrder::name;
                keepView();
                arrange();
                break;
        }
        ++revision_;
    }

    const ProjectEntry* ProjectBrowser::rowProject(int row) const
    {
        if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[static_cast<std::size_t>(row)].project < 0)
            return nullptr;
        return &projects_[static_cast<std::size_t>(rows_[static_cast<std::size_t>(row)].project)];
    }

    int ProjectBrowser::rowOf(const std::string& project, bool inFavourites) const
    {
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i)
            if (const ProjectEntry* e = rowProject(i); e != nullptr && same(e->name, project)
                && (groups_[static_cast<std::size_t>(rows_[static_cast<std::size_t>(i)].group)].kind == Kind::favourites) == inFavourites)
                return i;
        return -1;
    }

    int ProjectBrowser::headerOf(const std::string& key) const
    {
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i)
            if (rows_[static_cast<std::size_t>(i)].project < 0 && groups_[static_cast<std::size_t>(rows_[static_cast<std::size_t>(i)].group)].key == key)
                return i;
        return -1;
    }

    int ProjectBrowser::chosen(List l) const noexcept
    {
        if (l != L::projects)
            return chosen_[idx(l)];
        if (chosenName_.empty())
            return -1;
        int any = -1;
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i)
            if (const ProjectEntry* e = rowProject(i); e != nullptr && same(e->name, chosenName_))
            {
                if (groups_[static_cast<std::size_t>(rows_[static_cast<std::size_t>(i)].group)].key == chosenGroup_)
                    return i;
                if (any < 0)
                    any = i;
            }
        return any;
    }

    bool ProjectBrowser::factoryChosen() const { return isFactoryName(chosenName_); }

    std::string ProjectBrowser::copyName(const std::string& factory) const
    {
        const std::string base = port_.tidyName(ownName(factory));
        std::string name = base;
        for (int k = 2; std::any_of(projects_.begin(), projects_.end(), [&](const ProjectEntry& p) { return !p.factory && same(p.name, name); }); ++k)
        {
            const std::string suffix = " " + std::to_string(k);
            name = base.substr(0, std::min(base.size(), static_cast<std::size_t>(kMaxNameLength) - suffix.size())) + suffix;
        }
        return name;
    }

    std::string ProjectBrowser::homeName() const
    {
        if (state_.name.empty())
            return title_;
        return isFactoryName(state_.name) ? copyName(state_.name) : state_.name;
    }

    // A project of this Mac's: its name (Save adds a version to it, asked first when it is not the one open). A factory
    // project: its copy's name when it is the one open, else the program open's: Save saves the editor's program, so a
    // factory project's name never goes onto another program.
    std::string ProjectBrowser::fieldFor(const std::string& project) const
    {
        if (!isFactoryName(project))
            return project;
        return same(project, state_.name) ? copyName(project) : homeName();
    }

    void ProjectBrowser::open(const std::string& name, bool preset)
    {
        open_ = true;
        view_ = kept_ = port_.projectsView();
        filterFolded_.clear();
        search_.set(view_.search, kMaxSearchLength);
        searching_ = false;
        categoryMode_ = false;
        categoryFor_.clear();
        dragging_ = false;
        pressRow_ = -1;
        chosen_ = { -1, -1, -1 };
        first_ = { 0, 0, 0 };
        state_ = port_.project();
        title_ = name;
        chosenName_.clear();
        chosenGroup_.clear();
        selected_.clear();
        readProjects();
        for (const ProjectEntry& e : projects_)
            if (!state_.name.empty() && same(e.name, state_.name))
                chosenName_ = e.name;
        if (!chosenName_.empty())
            selected_ = { chosenName_ };
        anchor_ = chosen(L::projects);
        first_[idx(L::projects)] = std::max(0, anchor_ - 2);
        readChosen();
        focus_ = L::projects;
        name_.set(isFactoryName(name) ? copyName(name) : name, kMaxNameLength);   // a factory project's: its copy's
        typed_ = false;
        ask_ = Ask::none;
        why_.clear();
        if (preset)
        {
            if (const int c = chosen_[idx(L::presets)]; c >= 0)
            {
                focus_ = L::presets;
                name_.set(presets_[static_cast<std::size_t>(c)].name, kMaxNameLength);
            }
            else
                startPreset();
        }
        ++revision_;
    }

    void ProjectBrowser::reread(Because why)
    {
        if (!open_)
            return;
        // The rows chosen stay chosen by what they are (a version's number, a preset's name), never by their place: a
        // version Claude added comes first, and the open one moves.
        const int c = chosen_[idx(L::versions)], p = chosen_[idx(L::presets)];
        const int version = c >= 0 && c < static_cast<int>(versions_.size()) ? versions_[static_cast<std::size_t>(c)].number : 0;
        const std::string preset = p >= 0 && p < static_cast<int>(presets_.size()) ? presets_[static_cast<std::size_t>(p)].name : std::string();
        state_ = port_.project();
        readProjects();
        readChosen();
        for (std::size_t i = 0; i < versions_.size(); ++i)
            if (version > 0 && versions_[i].number == version)
                chosen_[idx(L::versions)] = static_cast<int>(i);
        for (std::size_t i = 0; i < presets_.size(); ++i)
            if (!preset.empty() && same(presets_[i].name, preset))
                chosen_[idx(L::presets)] = static_cast<int>(i);
        first_[idx(L::versions)] = std::max(0, chosen_[idx(L::versions)] - 2);
        first_[idx(L::presets)] = std::max(0, chosen_[idx(L::presets)] - 2);
        if (const int row = chosen(L::projects); row >= 0)
            anchor_ = row;
        // A confirmation asked for (Delete, Open anyway, Replace, Add version) is never given over lists that changed
        // under it: the next click asks again. A press on a project's row holds its place: let go (a drag holds its
        // projects by name, and goes on).
        if (ask_ != Ask::none)
        {
            ask_ = Ask::none;
            why_ = why == Because::claude ? "The lists changed (Claude's projects tool): choose again, then confirm."
                 : why == Because::saved  ? "The lists changed (your save finished): choose again, then confirm."
                                          : "The lists changed (a project was saved elsewhere): choose again, then confirm.";
        }
        if (!dragging_)
        {
            pressRow_ = -1;
            chooseOnUp_ = false;
        }
        ++revision_;
    }

    void ProjectBrowser::close()
    {
        if (open_)
            keepView();                                  // the search, kept as it closes
        open_ = false;
        ask_ = Ask::none;
        dragging_ = false;
        pressRow_ = -1;
        ++revision_;
    }

    bool ProjectBrowser::takeStartedSave() { return std::exchange(startedSave_, false); }

    std::string ProjectBrowser::takeNotice()
    {
        std::string n;
        n.swap(notice_);
        return n;
    }

    bool ProjectBrowser::takeImport()
    {
        const bool i = import_;
        import_ = false;
        return i;
    }

    std::string ProjectBrowser::takeExport()
    {
        std::string e;
        e.swap(export_);
        return e;
    }

    std::pair<std::string, int> ProjectBrowser::takeAb()
    {
        std::pair<std::string, int> a;
        a.swap(ab_);
        return a;
    }

    void ProjectBrowser::chooseProjectNamed(const std::string& name, float width, float height)
    {
        readProjects();
        if (name.empty())
            return;
        int row = rowOf(name);
        if (row < 0)
        {
            // Not shown (a filter leaves it out, or its group is folded): the filter off, its group open.
            view_.search.clear();
            view_.tag.clear();
            search_.set({}, kMaxSearchLength);
            for (const ProjectEntry& e : projects_)
                if (same(e.name, name))
                {
                    const std::string key = e.factory          ? std::string(projectgroups::kFactoryKey)
                                          : e.category.empty() ? std::string(projectgroups::kUncategorisedKey)
                                                               : projectgroups::keyOf(Kind::category, e.category);
                    view_.folded.erase(std::remove_if(view_.folded.begin(), view_.folded.end(), [&](const std::string& k) { return same(k, key); }),
                                       view_.folded.end());
                }
            keepView();
            arrange();
            row = rowOf(name);
        }
        if (row >= 0)
            chooseProject(row, places(width, height).rows[idx(L::projects)]);
    }

    bool ProjectBrowser::chosenIsOpen() const { return !state_.name.empty() && same(chosenName_, state_.name); }

    // Whether what it shows (the projects, the chosen one's versions and presets, the project open's name, version and
    // preset) differs from the store's cache and the project open now.
    bool ProjectBrowser::listsChanged() const
    {
        std::vector<ProjectEntry> now = port_.projectsPolled();
        const std::vector<ProjectEntry> factory = port_.factoryProjects();
        now.insert(now.end(), factory.begin(), factory.end());
        const ProjectDetail d = chosenName_.empty() ? ProjectDetail{} : port_.projectPolled(chosenName_);
        const ProjectState open = port_.project();
        bool same = now == projects_ && d.presets == presets_ && d.versions.size() == versions_.size()
                 && (chosenName_.empty() || !d.exists || d.read) == chosenRead_
                 && open.name == state_.name && open.version == state_.version && open.preset == state_.preset;
        for (std::size_t i = 0; same && i < versions_.size(); ++i)
            same = d.versions[i].number == versions_[i].number && d.versions[i].when == versions_[i].when
                && d.versions[i].note == versions_[i].note;
        return !same;
    }

    bool ProjectBrowser::rereadIfChanged(Because why)
    {
        if (!open_ || !listsChanged())
            return false;
        reread(why);
        return true;
    }

    bool ProjectBrowser::poll()
    {
        if (!open_ || port_.projectsRead() == readSeen_)
            return false;
        // Read again only when what it shows changed (the Reader reads other files too: the explorer's, another
        // instance's), so that a confirmation asked for stands over lists that did not.
        const uint64_t seen = port_.projectsRead();
        if (!listsChanged())
        {
            readSeen_ = seen;
            return false;
        }
        reread(Because::elsewhere);
        return true;
    }

    void ProjectBrowser::readChosen()
    {
        versions_.clear();
        presets_.clear();
        chosenRead_ = true;
        chosen_[idx(L::versions)] = chosen_[idx(L::presets)] = -1;
        first_[idx(L::versions)] = first_[idx(L::presets)] = 0;
        const std::string name = chosenName_;
        if (name.empty())
            return;
        const ProjectDetail d = port_.projectPolled(name);
        chosenRead_ = !d.exists || d.read;
        versions_ = d.versions;
        presets_ = d.presets;
        const bool isOpen = chosenIsOpen();
        for (std::size_t i = 0; i < versions_.size(); ++i)
            if (isOpen && versions_[i].number == state_.version)
                chosen_[idx(L::versions)] = static_cast<int>(i);
        if (chosen_[idx(L::versions)] < 0 && !versions_.empty())
            chosen_[idx(L::versions)] = 0;               // the newest
        for (std::size_t i = 0; i < presets_.size(); ++i)
            if (isOpen && same(presets_[i].name, state_.preset))
                chosen_[idx(L::presets)] = static_cast<int>(i);
        first_[idx(L::versions)] = std::max(0, chosen_[idx(L::versions)] - 2);
        first_[idx(L::presets)] = std::max(0, chosen_[idx(L::presets)] - 2);
    }

    int ProjectBrowser::count(List l) const
    {
        switch (l)
        {
            case L::projects: return static_cast<int>(rows_.size());
            case L::versions: return static_cast<int>(versions_.size());
            case L::presets: return static_cast<int>(presets_.size());
        }
        return 0;
    }

    ProjectBrowser::Places ProjectBrowser::places(float width, float height) const
    {
        Places at;
        const float w = std::min(kBoxW, width - 32.0f), h = std::min(kBoxH, height - 32.0f);
        at.box = { std::round((width - w) * 0.5f), std::round((height - h) * 0.45f), w, h };
        const float top = at.box.y + kListTop;
        const float listsH = std::max(3.0f * at.rowH + kCaptionH, h - kListTop - kBelowLists);
        const auto rowsIn = [&](float room) { return std::max(1, static_cast<int>(std::floor((room - 8.0f) / at.rowH))); };
        const float leftW = std::round((w - 2.0f * kPad - kGap) * 0.42f);
        const float rightX = at.box.x + kPad + leftW + kGap, rightW = at.box.right() - kPad - rightX;
        at.rows[idx(L::projects)] = rowsIn(listsH);
        at.projects = { at.box.x + kPad, top, leftW, static_cast<float>(at.rows[idx(L::projects)]) * at.rowH + 8.0f };
        const float versionsRoom = std::round((listsH - kCaptionH) * 0.56f);
        at.rows[idx(L::versions)] = rowsIn(versionsRoom);
        at.versions = { rightX, top, rightW, static_cast<float>(at.rows[idx(L::versions)]) * at.rowH + 8.0f };
        const float presetsTop = at.versions.bottom() + kCaptionH;
        at.rows[idx(L::presets)] = rowsIn(top + listsH - presetsTop);
        at.presets = { rightX, presetsTop, rightW, static_cast<float>(at.rows[idx(L::presets)]) * at.rowH + 8.0f };
        const float fy = top + listsH + 16.0f;
        at.mode = { at.box.x + kPad, fy, kModeW, 34.0f };
        at.field = { at.mode.right() + 8.0f, fy, at.box.right() - kPad - (at.mode.right() + 8.0f), 34.0f };
        const float by = at.box.bottom() - kPad - kButtonH;
        const float saveW = pillW(saveLabel()), openW = pillW(openLabel());
        at.save = { at.box.right() - kPad - saveW, by, saveW, kButtonH };
        at.open = { at.save.x - 8.0f - openW, by, openW, kButtonH };
        at.close = { at.open.x - 8.0f - 84.0f, by, 84.0f, kButtonH };
        at.remove = { at.box.x + kPad, by, pillW(removeLabel()), kButtonH };
        at.importIt = { at.remove.right() + 8.0f, by, 96.0f, kButtonH };
        at.exportIt = { at.importIt.right() + 8.0f, by, 96.0f, kButtonH };
        at.ab = { at.exportIt.right() + 8.0f, by, 72.0f, kButtonH };
        // The top: the search field after the title, then the tag's dropdown and the order's at the right.
        const float ty = at.box.y + 21.0f;
        at.sort = { at.box.right() - kPad - kSortW, ty, kSortW, kTopH };
        at.tag = { at.sort.x - 8.0f - kTagW, ty, kTagW, kTopH };
        at.search = { at.box.x + kPad + kSearchX, ty, std::max(120.0f, at.tag.x - 8.0f - (at.box.x + kPad + kSearchX)), kTopH };
        at.clear = { at.search.right() - 28.0f, ty + 4.0f, 24.0f, kTopH - 8.0f };
        return at;
    }

    funkgui::Rect ProjectBrowser::listRect(const Places& at, List l) const
    {
        return l == L::projects ? at.projects : l == L::versions ? at.versions : at.presets;
    }

    funkgui::Rect ProjectBrowser::rowRect(const Places& at, List l, int i) const
    {
        const funkgui::Rect r = listRect(at, l);
        const bool scrolls = count(l) > at.rows[idx(l)];
        return { r.x + 4.0f, r.y + 4.0f + static_cast<float>(i - first_[idx(l)]) * at.rowH, r.w - 8.0f - (scrolls ? 8.0f : 0.0f), at.rowH };
    }

    funkgui::Rect ProjectBrowser::starRect(const funkgui::Rect& row) { return { row.right() - kStarW, row.y, kStarW, row.h }; }

    int ProjectBrowser::rowAt(funkgui::Point p, const Places& at, List l) const
    {
        const funkgui::Rect r = listRect(at, l);
        if (!r.contains(p))
            return -1;
        const int first = first_[idx(l)];
        const int row = first + static_cast<int>(std::floor((p.y - r.y - 4.0f) / at.rowH));
        return row >= first && row < first + at.rows[idx(l)] && row < count(l) ? row : -1;
    }

    funkgui::Point ProjectBrowser::centreOfRow(List l, int i, float width, float height) const
    {
        const Places at = places(width, height);
        const int first = first_[idx(l)];
        if (i < first || i >= first + at.rows[idx(l)] || i >= count(l))
            return { -1.0e4f, -1.0e4f };
        const funkgui::Rect r = listRect(at, l);
        // A project's: clear of its star.
        const float cx = l == L::projects ? r.x + 4.0f + 0.5f * (r.w - 8.0f - kStarW - 8.0f) : r.centreX();
        return { cx, r.y + 4.0f + (static_cast<float>(i - first) + 0.5f) * at.rowH };
    }

    funkgui::Point ProjectBrowser::centreOfEmpty(List l, float width, float height) const
    {
        const Places at = places(width, height);
        const int shown = std::min(count(l) - first_[idx(l)], at.rows[idx(l)]);
        if (shown >= at.rows[idx(l)])
            return { -1.0e4f, -1.0e4f };
        const funkgui::Rect r = listRect(at, l);
        return { r.centreX(), r.y + 4.0f + (static_cast<float>(shown) + 0.5f) * at.rowH };
    }

    funkgui::Point ProjectBrowser::centreOfStar(int row, float width, float height) const
    {
        const Places at = places(width, height);
        const int first = first_[idx(L::projects)];
        if (rowProject(row) == nullptr || row < first || row >= first + at.rows[idx(L::projects)])
            return { -1.0e4f, -1.0e4f };
        const funkgui::Rect s = starRect(rowRect(at, L::projects, row));
        return { s.centreX(), s.centreY() };
    }

    void ProjectBrowser::chooseProject(int row, int rowsShown)
    {
        const ProjectEntry* e = rowProject(row);
        if (e == nullptr)
            return;
        endCategory();
        chosenName_ = e->name;
        chosenGroup_ = groups_[static_cast<std::size_t>(rows_[static_cast<std::size_t>(row)].group)].key;
        selected_ = { e->name };
        anchor_ = row;
        int& first = first_[idx(L::projects)];
        if (row < first)
            first = row;
        if (row >= first + rowsShown)
            first = row - rowsShown + 1;
        focus_ = L::projects;
        ask_ = Ask::none;
        why_.clear();
        typed_ = false;
        readChosen();
        name_.set(fieldFor(chosenName_), kMaxNameLength);
        if (isFactoryName(chosenName_) && !chosenIsOpen())
            why_ = "\"" + ownName(chosenName_) + "\" is read-only: open it to make it yours. Save saves the program open.";
        ++revision_;
    }

    void ProjectBrowser::choose(List l, int i, int rowsShown, int step)
    {
        const int n = count(l);
        if (n == 0)
            return;
        i = std::clamp(i, 0, n - 1);
        if (l == L::projects)
        {
            // A header is never chosen: the project's row past it in the step's direction, else the one before.
            const int dir = step < 0 ? -1 : 1;
            int j = i;
            while (j >= 0 && j < n && rows_[static_cast<std::size_t>(j)].project < 0)
                j += dir;
            if (j < 0 || j >= n)
                for (j = i; j >= 0 && j < n && rows_[static_cast<std::size_t>(j)].project < 0;)
                    j -= dir;
            if (j >= 0 && j < n)
                chooseProject(j, rowsShown);
            return;
        }
        endCategory();
        int& c = chosen_[idx(l)];
        int& first = first_[idx(l)];
        c = i;
        if (c < first)
            first = c;
        if (c >= first + rowsShown)
            first = c - rowsShown + 1;
        focus_ = l;
        ask_ = Ask::none;
        why_.clear();
        typed_ = false;
        if (l == L::versions)
        {
            name_.set(fieldFor(chosenName_), kMaxNameLength);
            if (isFactoryName(chosenName_) && !chosenIsOpen())
                why_ = "\"" + ownName(chosenName_) + "\" is read-only: open it to make it yours. Save saves the program open.";
        }
        else
            name_.set(presets_[static_cast<std::size_t>(c)].name, kMaxNameLength);
        ++revision_;
    }

    void ProjectBrowser::startPreset()
    {
        endCategory();
        focus_ = L::presets;
        chosen_[idx(L::presets)] = -1;
        ask_ = Ask::none;
        why_.clear();
        typed_ = false;
        std::string name;
        for (int k = static_cast<int>(presets_.size()) + 1;; ++k)
        {
            name = "Preset " + std::to_string(k);
            if (std::none_of(presets_.begin(), presets_.end(), [&](const PresetEntry& p) { return same(p.name, name); }))
                break;
        }
        name_.set(name, kMaxNameLength);
        ++revision_;
    }

    std::string ProjectBrowser::saveLabel() const
    {
        if (categoryMode_)
            return "Set category";
        if (focus_ == L::presets)
            return ask_ == Ask::replace ? "Replace" : "Save preset";
        if (ask_ == Ask::addTo)
            return "Add version";
        const std::string name = port_.tidyName(name_.buffer);
        const bool exists = std::any_of(projects_.begin(), projects_.end(), [&](const ProjectEntry& p) { return !p.factory && same(p.name, name); });
        return exists ? "Save version" : isFactoryName(state_.name) ? "Save as mine" : "Save project";
    }

    std::string ProjectBrowser::openLabel() const
    {
        if (focus_ == L::presets)
            return ask_ == Ask::open ? "Apply anyway" : "Apply";
        if (ask_ == Ask::open)
            return "Open anyway";
        const int c = chosen_[idx(L::versions)];
        if (focus_ == L::versions && c >= 0 && c < static_cast<int>(versions_.size()))
            return "Open " + v(versions_[static_cast<std::size_t>(c)].number);
        return "Open";
    }

    std::string ProjectBrowser::removeLabel() const
    {
        if (ask_ == Ask::remove)
            return "Delete it";
        const int c = chosen_[idx(L::versions)];
        if (focus_ == L::versions && c >= 0 && c < static_cast<int>(versions_.size()))
            return "Delete " + v(versions_[static_cast<std::size_t>(c)].number);
        return "Delete";
    }

    void ProjectBrowser::save()
    {
        if (categoryMode_)
        {
            const std::vector<std::string> names = categoryFor_;
            const std::string category = name_.buffer;
            if (tidyCategory(category).empty())          // as an empty name is refused: clearing is None's
            {
                why_ = "Name the category first: None (Uncategorised) in a project's menu clears it.";
                ++revision_;
                return;
            }
            endCategory();
            setCategory(names, category);
            return;
        }
        const std::string name = port_.tidyName(name_.buffer);
        std::string why;
        if (focus_ == L::presets)
        {
            if (name.empty())
            {
                ask_ = Ask::none;
                why_ = "Name the preset first.";
                return;
            }
            if (state_.name.empty())
            {
                ask_ = Ask::none;
                why_ = "Save the project first: a preset is kept in its project.";
                return;
            }
            if (isFactoryName(state_.name))
            {
                ask_ = Ask::none;
                why_ = "A factory project's presets stay as they are: save the project first (your own copy, its presets with it).";
                return;
            }
            if (!chosenIsOpen())
            {
                ask_ = Ask::none;
                why_ = "A preset is saved in the project open, \"" + state_.name + "\": choose it to save one.";
                return;
            }
            const bool taken = std::any_of(presets_.begin(), presets_.end(), [&](const PresetEntry& p) { return same(p.name, name); });
            if (taken && ask_ != Ask::replace)
            {
                ask_ = Ask::replace;
                why_ = "\"" + name + "\" is a preset already: Replace saves over it.";
                return;
            }
            if (!port_.savePreset(name, why))
            {
                ask_ = Ask::none;
                why_ = "Not saved: " + why + ".";
                return;
            }
            notice_ = "PRESET SAVED: " + upper(name);
            close();
            return;
        }
        if (name.empty())
        {
            ask_ = Ask::none;
            why_ = "Name the project first.";
            return;
        }
        const bool other = !same(name, state_.name)
                        && std::any_of(projects_.begin(), projects_.end(), [&](const ProjectEntry& p) { return !p.factory && same(p.name, name); });
        if (other && ask_ != Ask::addTo)
        {
            ask_ = Ask::addTo;
            why_ = "\"" + name + "\" is another project: Add version saves this program as its newest version.";
            return;
        }
        // The project's file is read and written off the message thread (EditorPort::startSave): the card closes at
        // once, and the Panel says SAVING, then what came of it.
        if (!port_.startSave(name, why))
        {
            ask_ = Ask::none;
            why_ = "Not saved: " + why + ".";
            return;
        }
        startedSave_ = true;
        close();
    }

    void ProjectBrowser::openChosen()
    {
        std::string why;
        if (focus_ == L::presets)
        {
            const int c = chosen_[idx(L::presets)];
            if (c < 0 || c >= static_cast<int>(presets_.size()))
            {
                ask_ = Ask::none;
                why_ = presets_.empty() ? "This project has no presets yet." : "Choose a preset first.";
                return;
            }
            if (!chosenIsOpen() && port_.project().edited && ask_ != Ask::open)
            {
                ask_ = Ask::open;
                why_ = "It opens \"" + chosenName_ + "\": the editor's changes no version holds are lost. Apply anyway does.";
                return;
            }
            const std::string name = presets_[static_cast<std::size_t>(c)].name;
            if (!port_.applyPreset(chosenName_, name, why))
            {
                ask_ = Ask::none;
                why_ = "Not applied: " + why + ".";
                return;
            }
            notice_ = "PRESET: " + upper(name);
            close();
            return;
        }
        if (chosenName_.empty())
        {
            ask_ = Ask::none;
            why_ = projects_.empty() ? "There is no project yet: name this program and Save it." : "Choose a project first.";
            return;
        }
        int version = 0;
        if (const int c = chosen_[idx(L::versions)]; focus_ == L::versions && c >= 0 && c < static_cast<int>(versions_.size()))
            version = versions_[static_cast<std::size_t>(c)].number;
        if (port_.project().edited && ask_ != Ask::open)
        {
            ask_ = Ask::open;
            why_ = "The editor's files have changes no version holds: Open anyway replaces them.";
            return;
        }
        const std::string name = chosenName_;
        int latest = 0;
        for (const ProjectEntry& e : projects_)
            if (same(e.name, name))
                latest = e.latest;
        if (!port_.openProject(name, version, why))
        {
            ask_ = Ask::none;
            why_ = "Not opened: " + why + ".";
            return;
        }
        notice_ = isFactoryName(name) ? "OPENED: " + upper(ownName(name)) + " (FACTORY, READ-ONLY: A SAVE MAKES YOUR OWN COPY)"
                                      : "OPENED: " + upper(name) + " " + upper(v(version > 0 ? version : latest));
        close();
    }

    void ProjectBrowser::remove()
    {
        const int c = focus_ == L::projects ? (chosenName_.empty() ? -1 : 0) : chosen_[idx(focus_)];
        if (c < 0 || (focus_ != L::projects && c >= count(focus_)))
        {
            ask_ = Ask::none;
            why_ = focus_ == L::projects ? "Choose a project first." : focus_ == L::versions ? "Choose a version first." : "Choose a preset first.";
            return;
        }
        const std::string project = chosenName_;
        if (isFactoryName(project))
        {
            ask_ = Ask::none;
            why_ = "\"" + ownName(project) + "\" is a factory project: it stays as it is, its versions and presets too.";
            return;
        }
        if (focus_ == L::projects && selected_.size() > 1)
        {
            ask_ = Ask::none;
            why_ = "Delete takes one project at a time: choose the one alone.";
            return;
        }
        std::string what;
        if (focus_ == L::projects)
            what = "\"" + project + "\", every version, preset and chat,";
        else if (focus_ == L::versions)
            what = v(versions_[static_cast<std::size_t>(c)].number) + " of \"" + project + "\"";
        else
            what = "the preset \"" + presets_[static_cast<std::size_t>(c)].name + "\"";
        if (ask_ != Ask::remove)
        {
            ask_ = Ask::remove;
            why_ = "Delete " + what + " from " + kThisMachine + "? Delete it does.";
            return;
        }
        ask_ = Ask::none;
        std::string why;
        const List from = focus_;
        bool done = false;
        if (from == L::projects)
            done = port_.deleteProject(project, why);
        else if (from == L::versions)
            done = port_.deleteVersion(project, versions_[static_cast<std::size_t>(c)].number, why);
        else
            done = port_.deletePreset(project, presets_[static_cast<std::size_t>(c)].name, why);
        if (!done)
        {
            why_ = "Not deleted: " + why + ".";
            return;
        }
        keepView();                                      // the store unstarred a project deleted: so does the card
        state_ = port_.project();
        if (from == L::projects)
        {
            chosenName_.clear();
            selected_.clear();
        }
        readProjects();
        readChosen();
        focus_ = from;
        what[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(what[0])));
        if (from == L::projects)
            why_ = "Deleted \"" + project + "\".";
        else
            why_ = "Deleted " + what + ".";
        ++revision_;
    }

    void ProjectBrowser::pointerDown(const funkgui::PointerEvent& e, funkgui::HostServices* host, float width, float height)
    {
        host_ = host;
        width_ = width;
        height_ = height;
        const funkgui::Point p{ e.x, e.y };
        const Places at = places(width, height);
        pressRow_ = -1;
        chooseOnUp_ = false;
        dragging_ = false;
        if (at.search.contains(p))
        {
            if (!search_.buffer.empty() && at.clear.contains(p))
            {
                search_.set({}, kMaxSearchLength);
                setSearch({});
            }
            searching_ = true;
            search_.allSelected = false;
            search_.caret = static_cast<int>(search_.buffer.size());
            ++revision_;
            return;
        }
        searching_ = false;
        if (at.tag.contains(p) || at.sort.contains(p))
        {
            showMenu(at.tag.contains(p) ? Menu::tag : Menu::sort, -1, p);
            ++revision_;
            return;
        }
        for (const List l : { L::projects, L::versions, L::presets })
            if (listRect(at, l).contains(p))
            {
                const int row = rowAt(p, at, l);
                if (l == L::projects && row >= 0)
                {
                    if (rows_[static_cast<std::size_t>(row)].project < 0)   // a group's header: folds it
                    {
                        if (e.popup)
                            showMenu(Menu::header, row, p);
                        else
                            toggleFold(rows_[static_cast<std::size_t>(row)].group);
                        return;
                    }
                    const std::string name = rowProject(row)->name;
                    if (e.popup)
                    {
                        if (!has(selected_, name))
                            chooseProject(row, at.rows[idx(l)]);
                        showMenu(Menu::row, row, p);
                        return;
                    }
                    if (starRect(rowRect(at, l, row)).contains(p))
                    {
                        star({ name }, !starredName(name));
                        return;
                    }
                    if (e.mods.cmd)
                    {
                        // One more chosen, or one less (never none): the versions and presets show the one clicked.
                        if (has(selected_, name) && selected_.size() > 1)
                        {
                            selected_.erase(std::remove_if(selected_.begin(), selected_.end(), [&](const std::string& n) { return same(n, name); }),
                                            selected_.end());
                            if (same(chosenName_, name))
                            {
                                chosenName_ = selected_.back();
                                readChosen();
                                name_.set(fieldFor(chosenName_), kMaxNameLength);
                            }
                        }
                        else if (!has(selected_, name))
                        {
                            const std::vector<std::string> before = selected_;
                            chooseProject(row, at.rows[idx(l)]);
                            selected_ = before;
                            selected_.push_back(name);
                        }
                        anchor_ = row;
                        why_ = selected_.size() > 1 ? std::to_string(selected_.size())
                                                          + " projects chosen: right-click, or drag them onto a group, for their category."
                                                    : std::string();
                        ++revision_;
                        return;
                    }
                    if (e.mods.shift && anchor_ >= 0 && anchor_ < static_cast<int>(rows_.size()))
                    {
                        const int from = std::min(anchor_, row), to = std::max(anchor_, row), keep = anchor_;
                        chooseProject(row, at.rows[idx(l)]);
                        selected_.clear();
                        for (int k = from; k <= to; ++k)
                            if (const ProjectEntry* pe = rowProject(k); pe != nullptr && !has(selected_, pe->name))
                                selected_.push_back(pe->name);
                        anchor_ = keep;
                        why_ = selected_.size() > 1 ? std::to_string(selected_.size())
                                                          + " projects chosen: right-click, or drag them onto a group, for their category."
                                                    : std::string();
                        ++revision_;
                        return;
                    }
                    if (has(selected_, name) && selected_.size() > 1)
                        chooseOnUp_ = true;              // a drag takes them all; a click, the row alone
                    else
                        chooseProject(row, at.rows[idx(l)]);
                    pressRow_ = row;
                    pressAt_ = p;
                    focus_ = L::projects;
                    ++revision_;
                    return;
                }
                if (row >= 0)
                    choose(l, row, at.rows[idx(l)]);
                else if (l == L::presets && !chosenName_.empty() && !factoryChosen())
                    startPreset();                       // a factory project's presets stay as they are
                else
                {
                    focus_ = l;
                    ask_ = Ask::none;
                    ++revision_;
                }
                return;
            }
        if (at.save.contains(p))
            save();
        else if (at.open.contains(p))
            openChosen();
        else if (at.remove.contains(p))
            remove();
        else if (at.close.contains(p))
            close();
        else if (at.importIt.contains(p))
        {
            import_ = true;
            close();
        }
        else if (at.exportIt.contains(p))
        {
            if (chosenName_.empty())
            {
                ask_ = Ask::none;
                why_ = projects_.empty() ? "There is no project yet: name this program and Save it." : "Choose a project to export.";
            }
            else
            {
                export_ = chosenName_;
                close();
            }
        }
        else if (at.ab.contains(p))
        {
            if (chosenName_.empty() || versions_.empty())
            {
                ask_ = Ask::none;
                why_ = projects_.empty() ? "There is no project yet: A/B plays the editor's files against a saved version."
                                         : "Choose a project: A/B plays the editor's files against one of its versions.";
            }
            else
            {
                const int c = chosen_[idx(L::versions)];
                ab_ = { chosenName_, c >= 0 && c < static_cast<int>(versions_.size()) ? versions_[static_cast<std::size_t>(c)].number : 0 };
                close();
            }
        }
        else if (at.field.contains(p))
        {
            name_.allSelected = false;
            name_.caret = static_cast<int>(name_.buffer.size());
        }
        ++revision_;
    }

    void ProjectBrowser::pointerDrag(funkgui::Point p, float width, float height)
    {
        width_ = width;
        height_ = height;
        if (pressRow_ < 0)
            return;
        if (!dragging_)
        {
            if (std::hypot(p.x - pressAt_.x, p.y - pressAt_.y) < kDragAfter)
                return;
            dragging_ = true;
            dragged_ = targets(pressRow_);
            chooseOnUp_ = false;
        }
        dragAt_ = p;
        ++revision_;
    }

    void ProjectBrowser::pointerUp(funkgui::Point p, float width, float height)
    {
        width_ = width;
        height_ = height;
        const Places at = places(width, height);
        if (dragging_)
        {
            if (const int row = rowAt(p, at, L::projects); row >= 0)
                dropOn(rows_[static_cast<std::size_t>(row)].group);
            dragging_ = false;
            dragged_.clear();
        }
        else if (chooseOnUp_ && rowProject(pressRow_) != nullptr)
            chooseProject(pressRow_, at.rows[idx(L::projects)]);
        pressRow_ = -1;
        chooseOnUp_ = false;
        ++revision_;
    }

    void ProjectBrowser::doubleClick(funkgui::Point p, float width, float height)
    {
        const Places at = places(width, height);
        for (const List l : { L::projects, L::versions, L::presets })
            if (const int row = rowAt(p, at, l); row >= 0)
            {
                if (l == L::projects)
                {
                    const ProjectEntry* e = rowProject(row);
                    if (e == nullptr || starRect(rowRect(at, l, row)).contains(p))
                        return;                          // a header's (it folded and opened), a star's (twice toggled)
                    if (!same(e->name, chosenName_) || focus_ != l)
                        chooseProject(row, at.rows[idx(l)]);
                    openChosen();
                    return;
                }
                if (row != chosen_[idx(l)] || focus_ != l)
                    choose(l, row, at.rows[idx(l)]);
                openChosen();
                return;
            }
    }

    void ProjectBrowser::wheel(const funkgui::WheelEvent& e, float width, float height)
    {
        const Places at = places(width, height);
        for (const List l : { L::projects, L::versions, L::presets })
            if (listRect(at, l).contains({ e.x, e.y }))
            {
                const int most = std::max(0, count(l) - at.rows[idx(l)]);
                const float dy = e.reversed ? -e.dy : e.dy;
                const int step = dy > 0.0f ? -1 : (dy < 0.0f ? 1 : 0);
                int& first = first_[idx(l)];
                first = std::clamp(first + step * std::max(1, static_cast<int>(std::round(std::fabs(dy) * 6.0f))), 0, most);
                ++revision_;
                return;
            }
    }

    void ProjectBrowser::key(const funkgui::KeyEvent& e, funkgui::HostServices* host, float width, float height)
    {
        using funkgui::Key;
        host_ = host;
        width_ = width;
        height_ = height;
        const Places at = places(width, height);
        const auto edited = [this] {
            typed_ = true;
            ask_ = Ask::none;
            if (!categoryMode_)
                why_.clear();
            ++revision_;
        };
        const bool upDown = e.key == Key::up || e.key == Key::down;
        if (e.key == Key::character && e.mods.cmd && (e.ch == 'f' || e.ch == 'F'))
        {
            searching_ = true;
            search_.allSelected = !search_.buffer.empty();
            search_.caret = static_cast<int>(search_.buffer.size());
            ++revision_;
            return;
        }
        if (searching_ && !upDown)
        {
            // The search field has the keys: each change filters at once.
            if (e.key == Key::escape)
            {
                if (search_.buffer.empty())
                    searching_ = false;
                search_.set({}, kMaxSearchLength);
                setSearch({});
            }
            else if (e.key == Key::enter || e.key == Key::tab)
            {
                searching_ = false;
                if (chosen(L::projects) < 0)
                    choose(L::projects, 0, at.rows[idx(L::projects)]);   // the first match
            }
            else if (e.key == Key::character && e.mods.cmd && (e.ch == 'v' || e.ch == 'V'))
            {
                if (host != nullptr && search_.paste(host->clipboardText(), kMaxSearchLength))
                    setSearch(search_.buffer);
            }
            else if (e.key == Key::character && e.mods.cmd && (e.ch == 'a' || e.ch == 'A'))
                search_.allSelected = !search_.buffer.empty();
            else if (search_.key(e, kMaxSearchLength))
                setSearch(search_.buffer);
            ++revision_;
            return;
        }
        if (e.key == Key::escape)
        {
            if (categoryMode_)
                endCategory();
            else
                close();
        }
        else if (e.key == Key::enter)
        {
            const bool none = focus_ == L::projects ? chosenName_.empty() : chosen_[idx(focus_)] < 0;   // one folded away counts
            if (categoryMode_ || typed_ || none)
                save();
            else
                openChosen();
        }
        else if (upDown)
        {
            const int c = focus_ == L::projects ? chosen(L::projects) : chosen_[idx(focus_)];
            choose(focus_, c < 0 ? 0 : c + (e.key == Key::up ? -1 : 1), at.rows[idx(focus_)], e.key == Key::up ? -1 : 1);
        }
        else if (e.key == Key::tab)
        {
            const List next = static_cast<List>((idx(focus_) + (e.mods.shift ? 2u : 1u)) % 3u);
            const int c = next == L::projects ? chosen(L::projects) : chosen_[idx(next)];
            if (c >= 0)
                choose(next, c, at.rows[idx(next)]);
            else if (next == L::presets && !chosenName_.empty())
                startPreset();
            else
            {
                endCategory();
                focus_ = next;
                ask_ = Ask::none;
                ++revision_;
            }
        }
        else if (e.key == Key::character && e.mods.cmd && (e.ch == 'v' || e.ch == 'V'))
        {
            if (host != nullptr && name_.paste(host->clipboardText(), categoryMode_ ? kMaxCategoryLength : kMaxNameLength))
                edited();
        }
        else if (e.key == Key::character && e.mods.cmd && (e.ch == 'a' || e.ch == 'A'))
            name_.allSelected = !name_.buffer.empty();
        else if (name_.key(e, categoryMode_ ? kMaxCategoryLength : kMaxNameLength))
            edited();
    }

    funkgui::Cursor ProjectBrowser::cursor(funkgui::Point p, float width, float height) const
    {
        const Places at = places(width, height);
        if (dragging_)
        {
            const int row = rowAt(p, at, L::projects);
            if (row < 0)
                return funkgui::Cursor::grabbing;
            return groups_[static_cast<std::size_t>(rows_[static_cast<std::size_t>(row)].group)].kind == Kind::factory
                       ? funkgui::Cursor::notAllowed
                       : funkgui::Cursor::grabbing;
        }
        if (at.save.contains(p) || at.open.contains(p) || at.remove.contains(p) || at.close.contains(p) || at.importIt.contains(p)
            || at.exportIt.contains(p) || at.ab.contains(p) || at.tag.contains(p) || at.sort.contains(p)
            || (!search_.buffer.empty() && at.clear.contains(p)))
            return funkgui::Cursor::pointingHand;
        for (const List l : { L::projects, L::versions, L::presets })
            if (rowAt(p, at, l) >= 0)
                return funkgui::Cursor::pointingHand;
        return funkgui::Cursor::normal;
    }

    void ProjectBrowser::draw(funkgui::Canvas& c, const funkgui::Theme& th, float width, float height, funkgui::Point pointer,
                              bool hasPointer) const
    {
        const chat::Palette p = chat::palette(th);
        const Places at = places(width, height);
        const funkgui::Rect& box = at.box;
        c.rrect(0.0f, 0.0f, width, height, 0.0f, th.ground.withAlpha(0.68f));
        constexpr funkgui::Col black{ 0, 0, 0, 255 };
        for (int k = 3; k >= 1; --k)                     // a soft shadow under the card
        {
            const float g = static_cast<float>(k) * 3.0f;
            c.rrect(box.x - g, box.y - g + 6.0f, box.w + 2.0f * g, box.h + 2.0f * g, 16.0f + g, black.withAlpha(0.07f));
        }
        c.rrect(box.x, box.y, box.w, box.h, 16.0f, p.card, 1.0f, p.cardEdge);

        // The title, the search, the tag and the order; where the projects are, and how many.
        const float x = box.x + kPad;
        c.rrect(x, box.y + 22.0f, 30.0f, 30.0f, 9.0f, p.accent.withAlpha(0.16f));
        chat::drawIcon(c, chat::Icon::list, x + 15.0f, box.y + 37.0f, 15.0f, p.accent);
        c.text("Projects", x + 42.0f, c.capCentreTop(box.y + 37.0f, chat::kH2), chat::kH2, p.text);
        {
            const funkgui::Rect& s = at.search;
            c.rrect(s.x, s.y, s.w, s.h, s.h * 0.5f, p.composer, 1.0f,
                    searching_ ? funkgui::mix(p.composerEdge, p.accent, 0.6f) : p.composerEdge);
            chat::drawIcon(c, chat::Icon::search, s.x + 17.0f, s.centreY(), 12.0f, searching_ ? p.accent : p.dim);
            const float tx = s.x + 32.0f, room = s.w - 32.0f - (search_.buffer.empty() ? 12.0f : 34.0f);
            const float ty = c.capCentreTop(s.centreY(), chat::kBody);
            if (search_.buffer.empty())
                c.text(fitText(c, "Search: name, description, tags", chat::kSmall, room).c_str(), tx,
                       c.capCentreTop(s.centreY(), chat::kSmall), chat::kSmall, p.faint);
            else
            {
                if (search_.allSelected && searching_)
                    c.rrect(tx - 2.0f, s.y + 7.0f, std::min(c.textWidth(search_.buffer.c_str(), chat::kBody), room) + 4.0f, s.h - 14.0f,
                            3.0f, th.accentDim);
                c.text(fitText(c, search_.buffer, chat::kBody, room).c_str(), tx, ty, chat::kBody, p.text);
                const bool over = hasPointer && at.clear.contains(pointer);
                chat::drawIcon(c, chat::Icon::cross, at.clear.centreX(), at.clear.centreY(), 9.0f, over ? p.text : p.dim);
            }
            if (searching_)
            {
                const std::string before = search_.buffer.substr(0, static_cast<std::size_t>(std::clamp(search_.caret, 0, static_cast<int>(search_.buffer.size()))));
                const float cx = tx + std::min(c.textWidth(before.c_str(), chat::kBody), room);
                c.rrect(cx, s.y + 8.0f, 1.5f, s.h - 16.0f, 0.75f, p.accent);
            }
            const auto trigger = [&](const funkgui::Rect& r, const std::string& label, bool enabled) {
                const dropdown::State st{ enabled, enabled && hasPointer && r.contains(pointer), menuOpen_ && menuOpen_(r), false };
                const funkgui::Col ink = dropdown::drawTrigger(c, p, r, dropdown::Base::outlined, st);
                c.text(fitText(c, label, chat::kSmall, dropdown::labelRoom(r, r.x + 14.0f)).c_str(), r.x + 14.0f,
                       c.capCentreTop(r.centreY(), chat::kSmall), chat::kSmall, ink);
            };
            const bool tags = !view_.tag.empty() || std::any_of(projects_.begin(), projects_.end(), [](const ProjectEntry& e) { return !e.tags.empty(); });
            trigger(at.tag, view_.tag.empty() ? "Any tag" : "Tag: " + view_.tag, tags);
            trigger(at.sort, view_.order == ProjectOrder::saved ? "By last saved" : "By name", true);
        }
        const auto mine = static_cast<std::size_t>(std::count_if(projects_.begin(), projects_.end(), [](const ProjectEntry& e) { return !e.factory; }));
        const std::size_t factory = projects_.size() - mine;
        std::string howMany = std::to_string(mine) + (mine == 1 ? " project" : " projects")
                            + (factory > 0 ? ", " + std::to_string(factory) + " factory" : std::string());
        if (filtering())
        {
            std::size_t shown = 0;
            for (const ProjectEntry& e : projects_)
                shown += projectgroups::matches(e, view_.search, view_.tag) ? 1 : 0;
            howMany = std::to_string(shown) + " of " + std::to_string(projects_.size()) + " match";
        }
        const float countW = c.textWidth(howMany.c_str(), chat::kSmall);
        c.text(howMany.c_str(), box.right() - kPad, c.capCentreTop(box.y + 66.0f, chat::kSmall), chat::kSmall, filtering() ? p.accent : p.faint,
               funkgui::Align::right);
        const std::string folder = std::string("Saved on ") + kThisMachine + " in " + port_.projectFolder()
                                 + "   Each save is a version; a project's presets are dial settings for it";
        c.text(fitText(c, folder, chat::kSmall, box.w - 2.0f * kPad - countW - 16.0f).c_str(), x, c.capCentreTop(box.y + 66.0f, chat::kSmall),
               chat::kSmall, p.soft);

        // The lists, each under its caption.
        const std::string of = chosenName_.empty() ? std::string() : " OF " + upper(asciiText(ownName(chosenName_)))
                                                                   + (factoryChosen() ? " (FACTORY)" : "");
        const auto caption = [&](const funkgui::Rect& r, List l, const std::string& text) {
            const funkgui::TextStyle& st = chat::kSmallStrong;
            c.text(fitText(c, text, st, r.w).c_str(), r.x + 2.0f, c.capCentreTop(r.y - 11.0f, st), st, focus_ == l ? p.text : p.faint);
        };
        caption(at.projects, L::projects, selected_.size() > 1 ? "PROJECTS   " + std::to_string(selected_.size()) + " CHOSEN" : "PROJECTS");
        caption(at.versions, L::versions, "VERSIONS" + of);
        caption(at.presets, L::presets, "PRESETS" + of);
        // The group under a drag, to be drawn as its target.
        const int dropRow = dragging_ ? rowAt(dragAt_, at, L::projects) : -1;
        const int dropGroup = dropRow >= 0 ? rows_[static_cast<std::size_t>(dropRow)].group : -1;
        const bool dropRefused = dropGroup >= 0 && groups_[static_cast<std::size_t>(dropGroup)].kind == Kind::factory;
        const auto list = [&](List l, const std::string& empty, auto&& row) {
            const funkgui::Rect r = listRect(at, l);
            c.rrect(r.x, r.y, r.w, r.h, 10.0f, p.composer, 1.0f, focus_ == l ? funkgui::mix(p.composerEdge, p.accent, 0.45f) : p.composerEdge);
            const int n = count(l), rows = at.rows[idx(l)], first = first_[idx(l)];
            if (n == 0 && !empty.empty())
                c.text(fitText(c, empty, chat::kSmall, r.w - 24.0f).c_str(), r.centreX(), c.capCentreTop(r.y + 4.0f + 0.5f * at.rowH, chat::kSmall),
                       chat::kSmall, p.faint, funkgui::Align::centre);
            const bool scrolls = n > rows;
            for (int k = 0; k < rows && first + k < n; ++k)
            {
                const int i = first + k;
                const funkgui::Rect rr = rowRect(at, l, i);
                const bool header = l == L::projects && rows_[static_cast<std::size_t>(i)].project < 0;
                const bool chosen = l == L::projects ? (!header && same(rowProject(i)->name, chosenName_)) : i == chosen_[idx(l)];
                const bool picked = l == L::projects && !header && has(selected_, rowProject(i)->name);
                const bool target = l == L::projects && dropGroup >= 0 && rows_[static_cast<std::size_t>(i)].group == dropGroup;
                if (target)
                    c.rrect(rr.x, rr.y + 1.0f, rr.w, rr.h - 2.0f, 7.0f, dropRefused ? p.danger.withAlpha(0.10f) : p.accent.withAlpha(0.14f));
                if (chosen)
                    c.rrect(rr.x, rr.y + 1.0f, rr.w, rr.h - 2.0f, 7.0f, funkgui::mix(p.composer, p.accent, focus_ == l ? 0.22f : 0.10f));
                else if (picked)
                    c.rrect(rr.x, rr.y + 1.0f, rr.w, rr.h - 2.0f, 7.0f, funkgui::mix(p.composer, p.accent, 0.13f));
                else if (!target && hasPointer && rr.contains(pointer) && !dragging_)
                    c.rrect(rr.x, rr.y + 1.0f, rr.w, rr.h - 2.0f, 7.0f, p.hover);
                row(i, rr, chosen);
            }
            if (scrolls)
            {
                const float track = r.h - 12.0f;
                const float thumb = std::max(18.0f, track * static_cast<float>(rows) / static_cast<float>(n));
                const float y = r.y + 6.0f + (track - thumb) * static_cast<float>(std::min(first, n - rows)) / static_cast<float>(std::max(1, n - rows));
                c.rrect(r.right() - 9.0f, y, 4.0f, thumb, 2.0f, p.dim);
            }
        };
        const auto mark = [&](const funkgui::Rect& rr) { c.disc(rr.x + 12.0f, rr.centreY(), 3.0f, p.accent); };
        const std::string noProjects = projects_.empty() ? "No projects yet: name this program below and Save it."
                                     : filtering()       ? "Nothing matches: Escape in the search clears it."
                                                         : std::string();
        list(L::projects, noProjects, [&](int i, const funkgui::Rect& rr, bool chosen) {
            const Row& rw = rows_[static_cast<std::size_t>(i)];
            const projectgroups::Group& g = groups_[static_cast<std::size_t>(rw.group)];
            if (rw.project < 0)
            {
                // A group's header: its chevron, its name, how many it has.
                const funkgui::TextStyle& st = chat::kSmallStrong;
                chat::drawIcon(c, g.folded ? chat::Icon::chevronRight : chat::Icon::chevronDown, rr.x + 12.0f, rr.centreY(), 8.0f, p.dim);
                float tx = rr.x + 24.0f;
                if (g.kind == Kind::favourites)
                {
                    chat::drawIcon(c, chat::Icon::starFilled, tx + 5.0f, rr.centreY(), 11.0f, p.accent);
                    tx += 16.0f;
                }
                const std::string title = upper(asciiText(g.title)) + (g.kind == Kind::factory ? "   READ-ONLY" : "");
                const std::string n = filtering() && static_cast<int>(g.members.size()) != g.total
                                        ? std::to_string(g.members.size()) + " of " + std::to_string(g.total)
                                        : std::to_string(g.total);
                const float nW = c.textWidth(n.c_str(), chat::kSmall);
                c.text(fitText(c, title, st, rr.right() - 10.0f - nW - 12.0f - tx).c_str(), tx, c.capCentreTop(rr.centreY(), st), st, p.soft);
                c.text(n.c_str(), rr.right() - 10.0f, c.capCentreTop(rr.centreY(), chat::kSmall), chat::kSmall, p.faint, funkgui::Align::right);
                return;
            }
            const ProjectEntry& e = projects_[static_cast<std::size_t>(rw.project)];
            if (same(e.name, state_.name))
                mark(rr);
            const std::string right = !e.factory     ? v(e.latest) + "   " + shortWhen(e.when)
                                    : e.presets > 0 ? std::to_string(e.presets) + (e.presets == 1 ? " preset" : " presets")
                                                    : std::string();
            const float rightW = c.textWidth(right.c_str(), chat::kSmall);
            const funkgui::TextStyle& st = chosen ? chat::kStrong : chat::kBody;
            c.text(fitText(c, asciiText(ownName(e.name)), st, rr.w - rightW - 48.0f - kStarW).c_str(), rr.x + 24.0f, c.capCentreTop(rr.centreY(), st), st,
                   p.text);
            c.text(right.c_str(), rr.right() - kStarW - 4.0f, c.capCentreTop(rr.centreY(), chat::kSmall), chat::kSmall, p.faint, funkgui::Align::right);
            const funkgui::Rect s = starRect(rr);
            const bool overStar = hasPointer && s.contains(pointer) && !dragging_;
            if (starredName(e.name))
                chat::drawIcon(c, chat::Icon::starFilled, s.centreX(), s.centreY(), 12.0f, overStar ? funkgui::mix(p.accent, p.text, 0.25f) : p.accent);
            else if (overStar || (hasPointer && rr.contains(pointer) && !dragging_) || chosen)
                chat::drawIcon(c, chat::Icon::star, s.centreX(), s.centreY(), 12.0f, overStar ? p.text : p.faint);
        });
        list(L::versions, chosenName_.empty() ? "Choose a project." : !chosenRead_ ? "Reading the project..." : "",
             [&](int i, const funkgui::Rect& rr, bool chosen) {
            const VersionEntry& e = versions_[static_cast<std::size_t>(i)];
            if (chosenIsOpen() && e.number == state_.version)
                mark(rr);
            const funkgui::TextStyle& st = chosen ? chat::kStrong : chat::kBody;
            const float mid = rr.centreY();
            c.text(v(e.number).c_str(), rr.x + 24.0f, c.capCentreTop(mid, st), st, p.text);
            const std::string when = shortWhen(e.when);
            c.text(when.c_str(), rr.x + 70.0f, c.capCentreTop(mid, chat::kSmall), chat::kSmall, p.faint);
            const float noteX = rr.x + 78.0f + c.textWidth(when.c_str(), chat::kSmall) + 12.0f;
            c.text(fitText(c, asciiText(e.note), chat::kSmall, std::max(0.0f, rr.right() - 10.0f - noteX)).c_str(), noteX,
                   c.capCentreTop(mid, chat::kSmall), chat::kSmall, p.soft);
        });
        const std::string noPresets = chosenName_.empty() || !chosenRead_    ? std::string()
                                    : chosenIsOpen() && !factoryChosen() ? "No presets yet: click here, name one, Save preset."
                                                                         : "No presets.";
        list(L::presets, noPresets, [&](int i, const funkgui::Rect& rr, bool chosen) {
            const PresetEntry& e = presets_[static_cast<std::size_t>(i)];
            if (chosenIsOpen() && same(e.name, state_.preset))
                mark(rr);
            const std::string right = (e.from > 0 ? "from " + v(e.from) + "   " : std::string()) + shortWhen(e.when);
            const float rightW = c.textWidth(right.c_str(), chat::kSmall);
            const funkgui::TextStyle& st = chosen ? chat::kStrong : chat::kBody;
            c.text(fitText(c, asciiText(e.name), st, rr.w - rightW - 48.0f).c_str(), rr.x + 24.0f, c.capCentreTop(rr.centreY(), st), st, p.text);
            c.text(right.c_str(), rr.right() - 10.0f, c.capCentreTop(rr.centreY(), chat::kSmall), chat::kSmall, p.faint, funkgui::Align::right);
        });

        // The field: what it names (its pill), the name, its caret, and the line under it.
        const bool preset = focus_ == L::presets && !categoryMode_;
        const bool tinted = preset || categoryMode_;
        const funkgui::Rect& m = at.mode;
        c.rrect(m.x, m.y, m.w, m.h, 10.0f, tinted ? p.accent.withAlpha(0.16f) : p.composer, 1.0f, tinted ? p.accent : p.composerEdge);
        c.text(categoryMode_ ? "CATEGORY" : preset ? "PRESET" : "PROJECT", m.centreX(), c.capCentreTop(m.centreY(), chat::kSmallStrong),
               chat::kSmallStrong, tinted ? p.accent : p.soft, funkgui::Align::centre);
        const funkgui::Rect& f = at.field;
        c.rrect(f.x, f.y, f.w, f.h, 10.0f, p.composer, 1.0f, funkgui::mix(p.composerEdge, p.accent, searching_ ? 0.0f : 0.45f));
        const float textTop = c.capCentreTop(f.centreY(), chat::kBody);
        const float maxW = f.w - 28.0f;
        if (name_.buffer.empty())
            c.text(categoryMode_ ? "Category name" : preset ? "Preset name" : "Project name", f.x + 14.0f, textTop, chat::kBody, p.faint);
        else
        {
            const float w = std::min(c.textWidth(name_.buffer.c_str(), chat::kBody), maxW);
            if (name_.allSelected)
                c.rrect(f.x + 12.0f, f.y + 8.0f, w + 4.0f, f.h - 16.0f, 3.0f, th.accentDim);
            c.text(fitText(c, name_.buffer, chat::kBody, maxW).c_str(), f.x + 14.0f, textTop, chat::kBody, p.text);
        }
        if (!searching_)
        {
            const std::string before = name_.buffer.substr(0, static_cast<std::size_t>(std::clamp(name_.caret, 0, static_cast<int>(name_.buffer.size()))));
            const float cx = f.x + 14.0f + std::min(c.textWidth(before.c_str(), chat::kBody), maxW);
            c.rrect(cx, f.y + 9.0f, 1.5f, f.h - 18.0f, 0.75f, p.accent);
        }
        if (!why_.empty())
            c.text(fitText(c, why_, chat::kSmall, box.w - 2.0f * kPad).c_str(), m.x + 2.0f, c.capCentreTop(f.bottom() + 15.0f, chat::kSmall),
                   chat::kSmall, ask_ == Ask::none ? p.soft : p.danger);

        // The buttons: pills (0 primary, 1 outlined, 2 quiet), greyed when they have nothing to act on.
        const auto button = [&](const funkgui::Rect& r, const std::string& label, int look, bool enabled) {
            const bool hover = enabled && hasPointer && r.contains(pointer);
            funkgui::Col fill = p.card.withAlpha(0.0f), edge = p.card.withAlpha(0.0f), ink = enabled ? p.soft : p.faint;
            if (look == 0)
            {
                fill = hover ? funkgui::mix(p.accent, p.text, 0.12f) : p.accent;
                ink = p.onAccent;
            }
            else
            {
                if (look == 1)
                    edge = p.composerEdge;
                if (hover)
                    fill = p.hover;
                if (enabled)
                    ink = look == 2 ? p.danger : (hover ? p.text : p.soft);
            }
            c.rrect(r.x, r.y, r.w, r.h, r.h * 0.5f, fill, look == 1 ? 1.0f : 0.0f, edge);
            const funkgui::TextStyle& st = look == 0 ? chat::kSmallStrong : chat::kSmall;
            c.text(label.c_str(), r.centreX(), c.capCentreTop(r.centreY(), st), st, ink, funkgui::Align::centre);
        };
        const bool canOpen = preset ? chosen_[idx(L::presets)] >= 0 : !chosenName_.empty();
        const bool canRemove = focus_ == L::projects ? !chosenName_.empty() && selected_.size() <= 1 : chosen_[idx(focus_)] >= 0;
        button(at.save, saveLabel(), 0, true);
        button(at.open, openLabel(), 1, canOpen);
        button(at.close, "Close", 1, true);
        button(at.remove, removeLabel(), 2, canRemove && !factoryChosen());
        button(at.importIt, "Import...", 1, true);
        button(at.exportIt, "Export...", 1, !chosenName_.empty());
        button(at.ab, "A/B...", 1, !chosenName_.empty() && !versions_.empty());

        // A drag: what it carries, at the pointer.
        if (dragging_ && !dragged_.empty())
        {
            const std::string what = them(dragged_);
            const float w = c.textWidth(what.c_str(), chat::kSmallStrong) + 24.0f;
            const funkgui::Rect g{ dragAt_.x + 12.0f, dragAt_.y + 6.0f, w, 24.0f };
            c.rrect(g.x, g.y + 2.0f, g.w, g.h, 12.0f, black.withAlpha(0.12f));
            c.rrect(g.x, g.y, g.w, g.h, 12.0f, dropRefused ? p.danger : p.accent);
            c.text(what.c_str(), g.centreX(), c.capCentreTop(g.centreY(), chat::kSmallStrong), chat::kSmallStrong, p.onAccent,
                   funkgui::Align::centre);
        }
    }

    void ProjectBrowser::accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const
    {
        const Places at = places(width, height);
        funkgui::A11yItem box;
        box.id = kBoxId;
        box.role = funkgui::a11yrole::group;
        box.bounds = at.box;
        box.title = "Projects";
        box.description = std::string("Saved on ") + kThisMachine + " in " + port_.projectFolder();
        out.push_back(box);
        {
            funkgui::A11yItem s;
            s.id = kSearchId;
            s.parent = kBoxId;
            s.role = funkgui::a11yrole::editableText;
            s.bounds = at.search;
            s.title = "Search projects";
            s.value = search_.buffer;
            s.help = shortcutText("Finds projects by name, description and tags; Cmd-F");
            out.push_back(std::move(s));
            if (!search_.buffer.empty())
            {
                funkgui::A11yItem x;
                x.id = kClearId;
                x.parent = kBoxId;
                x.role = funkgui::A11yRole::button;
                x.bounds = at.clear;
                x.title = "Clear the search";
                out.push_back(std::move(x));
            }
            const auto combo = [&](uint32_t id, const funkgui::Rect& r, const std::string& title, const std::string& value) {
                funkgui::A11yItem b;
                b.id = id;
                b.parent = kBoxId;
                b.role = funkgui::A11yRole::comboBox;
                b.bounds = r;
                b.title = title;
                b.value = value;
                out.push_back(std::move(b));
            };
            combo(kTagId, at.tag, "Tag", view_.tag.empty() ? "Any tag" : view_.tag);
            combo(kSortId, at.sort, "Order", view_.order == ProjectOrder::saved ? "By last saved" : "By name");
        }
        const std::string of = chosenName_.empty() ? std::string() : " of " + ownName(chosenName_);
        const auto listItem = [&](List l, const std::string& title) {
            funkgui::A11yItem it;
            it.id = kListId + static_cast<uint32_t>(idx(l));
            it.parent = kBoxId;
            it.role = funkgui::a11yrole::list;
            it.bounds = listRect(at, l);
            it.title = title;
            it.value = std::to_string(l == L::projects ? static_cast<int>(projects_.size()) : count(l)) + (focus_ == l ? ", chosen from" : "");
            out.push_back(std::move(it));
        };
        const auto list = [&](List l, const std::string& title, auto&& rowItem) {
            listItem(l, title);
            const int first = first_[idx(l)];
            for (int k = 0; k < at.rows[idx(l)] && first + k < count(l); ++k)
            {
                const int i = first + k;
                funkgui::A11yItem row;
                row.id = kRowId + static_cast<uint32_t>(idx(l)) * 0x1000u + static_cast<uint32_t>(i);
                row.parent = kListId + static_cast<uint32_t>(idx(l));
                row.role = funkgui::A11yRole::listItem;
                row.bounds = rowRect(at, l, i);
                rowItem(i, row);
                out.push_back(std::move(row));
            }
        };
        // The projects, in their groups: each group an item of the list (as far as it is shown), its header a toggle
        // (open or folded) and its rows in it, each row's star a toggle in the row.
        listItem(L::projects, "Projects");
        const int firstRow = first_[idx(L::projects)];
        const int shownRows = std::min(at.rows[idx(L::projects)], count(L::projects) - firstRow);
        for (int k = 0; k < shownRows; ++k)
        {
            const int i = firstRow + k;
            const Row& rw = rows_[static_cast<std::size_t>(i)];
            const projectgroups::Group& g = groups_[static_cast<std::size_t>(rw.group)];
            if (k == 0 || rows_[static_cast<std::size_t>(i - 1)].group != rw.group)
            {
                funkgui::Rect b = rowRect(at, L::projects, i);
                for (int j = k + 1; j < shownRows && rows_[static_cast<std::size_t>(firstRow + j)].group == rw.group; ++j)
                    b.h = rowRect(at, L::projects, firstRow + j).bottom() - b.y;
                funkgui::A11yItem it;
                it.id = kGroupId + static_cast<uint32_t>(rw.group);
                it.parent = kListId + static_cast<uint32_t>(idx(L::projects));
                it.role = funkgui::a11yrole::group;
                it.bounds = b;
                it.title = g.title;
                it.value = std::to_string(g.members.size()) + (g.members.size() == 1 ? " project" : " projects") + (g.folded ? ", folded" : "");
                it.help = g.kind == Kind::favourites ? "The projects starred"
                        : g.kind == Kind::factory    ? "The factory projects, read-only: saving one makes a project of your own"
                        : g.kind == Kind::category   ? "Projects of this category: drag projects here to give them it"
                                                     : "Projects with no category";
                out.push_back(std::move(it));
            }
            funkgui::A11yItem row;
            row.id = kRowId + static_cast<uint32_t>(i);
            row.parent = kGroupId + static_cast<uint32_t>(rw.group);
            row.bounds = rowRect(at, L::projects, i);
            if (rw.project < 0)
            {
                row.role = funkgui::A11yRole::toggleButton;
                row.title = g.title;
                row.checkable = true;
                row.checked = !g.folded;
                row.value = g.folded ? "folded" : "open";
                row.help = "Shows or hides this group's projects";
                out.push_back(std::move(row));
                continue;
            }
            const ProjectEntry& e = projects_[static_cast<std::size_t>(rw.project)];
            const bool chosen = same(e.name, chosenName_);
            std::string value = chosen ? "chosen, " : has(selected_, e.name) ? "among those chosen, " : "";
            value += same(e.name, state_.name) ? "open, " : "";
            value += starredName(e.name) ? "favourite, " : "";
            if (e.factory)
                value += "factory, read-only, " + std::to_string(e.presets) + (e.presets == 1 ? " preset" : " presets");
            else
                value += (e.category.empty() ? std::string("no category, ") : "category " + e.category + ", ") + "version "
                       + std::to_string(e.latest) + ", saved " + e.when;
            row.role = funkgui::A11yRole::listItem;
            row.title = ownName(e.name);
            row.value = value;
            row.help = shortcutText("Chooses it; a double-click opens it; Cmd-click chooses more; the context menu sets the category");
            funkgui::A11yItem s;                         // its star, a toggle in the row
            s.id = kStarId + static_cast<uint32_t>(i);
            s.parent = row.id;
            s.role = funkgui::A11yRole::toggleButton;
            s.bounds = starRect(row.bounds);
            s.title = "Favourite";
            s.checkable = true;
            s.checked = starredName(e.name);
            s.help = "Stars it: the favourites are pinned at the top";
            out.push_back(std::move(row));
            out.push_back(std::move(s));
        }
        list(L::versions, "Versions" + of, [&](int i, funkgui::A11yItem& row) {
            const VersionEntry& e = versions_[static_cast<std::size_t>(i)];
            row.title = "Version " + std::to_string(e.number);
            row.value = (i == chosen_[idx(L::versions)] ? "chosen, " : "") + std::string(chosenIsOpen() && e.number == state_.version ? "open, " : "")
                      + "saved " + e.when + ", " + e.note;
        });
        list(L::presets, "Presets" + of, [&](int i, funkgui::A11yItem& row) {
            const PresetEntry& e = presets_[static_cast<std::size_t>(i)];
            row.title = e.name;
            row.value = (i == chosen_[idx(L::presets)] ? "chosen, " : "") + std::string(chosenIsOpen() && same(e.name, state_.preset) ? "applied, " : "")
                      + "saved " + e.when;
        });
        funkgui::A11yItem field;
        field.id = kFieldId;
        field.parent = kBoxId;
        field.role = funkgui::a11yrole::editableText;
        field.bounds = at.field;
        field.title = categoryMode_ ? "Category name" : focus_ == L::presets ? "Preset name" : "Project name";
        field.value = name_.buffer;
        field.help = why_;
        out.push_back(std::move(field));
        const auto button = [&](uint32_t id, const funkgui::Rect& r, const std::string& title) {
            funkgui::A11yItem b;
            b.id = id;
            b.parent = kBoxId;
            b.role = funkgui::A11yRole::button;
            b.bounds = r;
            b.title = title;
            out.push_back(std::move(b));
        };
        button(kSaveId, at.save, saveLabel());
        button(kOpenId, at.open, openLabel());
        button(kCloseId, at.close, "Close");
        button(kRemoveId, at.remove, removeLabel());
        button(kImportId, at.importIt, "Import...");
        button(kExportId, at.exportIt, "Export...");
        button(kAbId, at.ab, "A/B...");
    }

    bool ProjectBrowser::a11yAction(uint32_t id, funkgui::A11yAction a, funkgui::HostServices* host, float width, float height)
    {
        host_ = host;
        width_ = width;
        height_ = height;
        if (a != funkgui::A11yAction::press && a != funkgui::A11yAction::focus && a != funkgui::A11yAction::toggle
            && a != funkgui::A11yAction::showMenu)
            return false;
        const Places at = places(width, height);
        const auto press = [&](const funkgui::Rect& r) {
            funkgui::PointerEvent e;
            e.x = r.centreX();
            e.y = r.centreY();
            pointerDown(e, host, width, height);
        };
        if (id == kSaveId)
            save();
        else if (id == kOpenId)
            openChosen();
        else if (id == kRemoveId)
            remove();
        else if (id == kCloseId)
            close();
        else if (id == kImportId || id == kExportId || id == kAbId)
            press(id == kImportId ? at.importIt : id == kExportId ? at.exportIt : at.ab);
        else if (id == kSearchId)
        {
            searching_ = true;
            search_.caret = static_cast<int>(search_.buffer.size());
        }
        else if (id == kClearId)
        {
            search_.set({}, kMaxSearchLength);
            setSearch({});
        }
        else if (id == kTagId || id == kSortId)
            showMenu(id == kTagId ? Menu::tag : Menu::sort, -1, {});
        else if (id >= kStarId && id < kStarId + 0x1000u)
        {
            const ProjectEntry* e = rowProject(static_cast<int>(id - kStarId));
            if (e == nullptr)
                return false;
            const std::string name = e->name;
            star({ name }, !starredName(name));
        }
        else if (id >= kRowId && id < kRowId + 3u * 0x1000u)
        {
            const auto l = static_cast<List>((id - kRowId) / 0x1000u);
            const int i = static_cast<int>((id - kRowId) % 0x1000u);
            if (i >= count(l))
                return false;
            if (l == L::projects && rows_[static_cast<std::size_t>(i)].project < 0)
                toggleFold(rows_[static_cast<std::size_t>(i)].group);
            else if (l == L::projects && a == funkgui::A11yAction::showMenu)
            {
                const funkgui::Rect r = rowRect(at, l, i);
                if (!has(selected_, rowProject(i)->name))
                    chooseProject(i, at.rows[idx(l)]);
                showMenu(Menu::row, i, { r.centreX(), r.centreY() });
            }
            else
                choose(l, i, at.rows[idx(l)]);
        }
        else
            return false;
        ++revision_;
        return true;
    }
}
