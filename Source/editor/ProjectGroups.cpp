// Source/editor/ProjectGroups.cpp: see ProjectGroups.h.
#include "editor/ProjectGroups.h"

#include <algorithm>
#include <cctype>

namespace playground::ui::projectgroups
{
    std::string lower(std::string_view s)
    {
        std::string out(s);
        for (char& c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    bool same(std::string_view a, std::string_view b)
    {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
               });
    }

    std::string keyOf(Kind kind, std::string_view category)
    {
        switch (kind)
        {
            case Kind::favourites: return std::string(kFavouritesKey);
            case Kind::factory: return std::string(kFactoryKey);
            case Kind::uncategorised: return std::string(kUncategorisedKey);
            case Kind::category: break;
        }
        return "category:" + lower(category);
    }

    bool starred(const std::vector<std::string>* favourites, std::string_view name)
    {
        return favourites != nullptr && std::any_of(favourites->begin(), favourites->end(), [&](const std::string& f) { return same(f, name); });
    }

    bool matches(const ProjectEntry& e, std::string_view search, std::string_view tag)
    {
        if (!tag.empty() && std::none_of(e.tags.begin(), e.tags.end(), [&](const std::string& t) { return same(t, tag); }))
            return false;
        std::string hay = lower(ownName(e.name)) + "\n" + lower(e.description);
        for (const std::string& t : e.tags)
            hay += "\n" + lower(t);
        const std::string q = lower(search);
        std::size_t at = 0;
        while (at < q.size())
        {
            const std::size_t start = q.find_first_not_of(' ', at);
            if (start == std::string::npos)
                break;
            const std::size_t end = std::min(q.find(' ', start), q.size());
            if (hay.find(q.substr(start, end - start)) == std::string::npos)
                return false;
            at = end;
        }
        return true;
    }

    std::vector<Group> arrange(const std::vector<ProjectEntry>& projects, const Options& o)
    {
        std::vector<Group> groups;
        const auto groupFor = [&](Kind kind, const std::string& category) -> Group& {
            const std::string key = keyOf(kind, category);
            for (Group& g : groups)
                if (g.key == key)
                    return g;
            Group g;
            g.kind = kind;
            g.key = key;
            g.category = kind == Kind::category ? category : std::string();
            g.title = kind == Kind::favourites ? "Favourites"
                    : kind == Kind::factory   ? "Factory"
                    : kind == Kind::category  ? category
                                              : "Uncategorised";
            g.folded = o.folded != nullptr && std::any_of(o.folded->begin(), o.folded->end(), [&](const std::string& f) { return same(f, key); });
            groups.push_back(std::move(g));
            return groups.back();
        };
        // The projects in their order first (by name, or the newest first), so each group's follow it.
        std::vector<int> order(projects.size());
        for (std::size_t i = 0; i < order.size(); ++i)
            order[i] = static_cast<int>(i);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            const ProjectEntry& x = projects[static_cast<std::size_t>(a)];
            const ProjectEntry& y = projects[static_cast<std::size_t>(b)];
            if (o.byDate && (x.savedMs != y.savedMs || x.when != y.when))
                return x.savedMs != y.savedMs ? x.savedMs > y.savedMs : x.when > y.when;
            const std::string nx = lower(ownName(x.name)), ny = lower(ownName(y.name));
            return nx != ny ? nx < ny : x.factory < y.factory;
        });
        const bool filtering = !o.search.empty() || !o.tag.empty();
        const auto add = [&](Group& g, int i, bool shown) {
            ++g.total;
            if (shown)
                g.members.push_back(i);
        };
        for (const int i : order)
        {
            const ProjectEntry& e = projects[static_cast<std::size_t>(i)];
            if (e.name.empty())
                continue;
            const bool shown = !filtering || matches(e, o.search, o.tag);
            if (starred(o.favourites, e.name))
                add(groupFor(Kind::favourites, {}), i, shown);
            if (e.factory)
            {
                if (o.factoryGroup)
                    add(groupFor(Kind::factory, {}), i, shown);
            }
            else if (e.category.empty())
                add(groupFor(Kind::uncategorised, {}), i, shown);
            else
            {
                Group& g = groupFor(Kind::category, e.category);
                if (e.category < g.category)             // one spelling whatever the order or the filter: the least
                    g.category = g.title = e.category;
                add(g, i, shown);
            }
        }
        groups.erase(std::remove_if(groups.begin(), groups.end(), [](const Group& g) { return g.members.empty(); }), groups.end());
        const auto rank = [](Kind k) { return k == Kind::favourites ? 0 : k == Kind::factory ? 1 : k == Kind::category ? 2 : 3; };
        std::stable_sort(groups.begin(), groups.end(), [&](const Group& a, const Group& b) {
            if (rank(a.kind) != rank(b.kind))
                return rank(a.kind) < rank(b.kind);
            return a.key < b.key;
        });
        return groups;
    }

    std::vector<std::pair<std::string, int>> tagsOf(const std::vector<ProjectEntry>& projects)
    {
        std::vector<std::pair<std::string, int>> out;
        for (const ProjectEntry& e : projects)
            for (const std::string& t : e.tags)
            {
                const auto it = std::find_if(out.begin(), out.end(), [&](const auto& o) { return same(o.first, t); });
                if (it != out.end())
                    ++it->second;
                else
                    out.emplace_back(t, 1);
            }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return lower(a.first) < lower(b.first); });
        return out;
    }

    std::vector<std::string> categoriesOf(const std::vector<ProjectEntry>& projects)
    {
        std::vector<std::string> out;
        for (const ProjectEntry& e : projects)
        {
            if (e.factory || e.name.empty() || e.category.empty())
                continue;
            const auto it = std::find_if(out.begin(), out.end(), [&](const std::string& c) { return same(c, e.category); });
            if (it == out.end())
                out.push_back(e.category);
            else if (e.category < *it)                   // spelt as arrange() titles its group: the least
                *it = e.category;
        }
        std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
        return out;
    }
}
