// Source/editor/PresetPicker.cpp: see PresetPicker.h.
#include "editor/PresetPicker.h"

#include "editor/ChatItems.h"
#include "editor/ChatStyle.h"
#include "editor/Dropdown.h"
#include "editor/Items.h"
#include "editor/ProjectGroups.h"
#include "editor/Theme.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace playground::ui
{
    namespace
    {
        constexpr float kBoxW = 320.0f;
        constexpr float kPad = 10.0f;
        constexpr float kFieldH = 30.0f;
        constexpr float kModeW = 78.0f;                  // the field's pill while it names a category
        constexpr float kWhyH = 22.0f;                   // the field's line, under the rows, when it says something
        constexpr int kMostRows = 12, kLeastRows = 3;
        constexpr uint32_t kBoxId = PresetPicker::kA11yBase, kFieldId = kBoxId + 1, kListId = kBoxId + 2,
                           kClearId = kBoxId + 3, kRowId = kBoxId + 0x100;
        constexpr int kApplyItem = 1, kNoCategory = 2, kNewCategory = 3, kChoice = 100;

        using projectgroups::lower;
        using projectgroups::same;

        // The words of a search (spaces apart), in lower case.
        std::vector<std::string> wordsOf(const std::string& search)
        {
            std::vector<std::string> out;
            std::string w;
            for (const char ch : lower(search) + " ")
                if (ch == ' ')
                {
                    if (!w.empty())
                        out.push_back(std::exchange(w, {}));
                }
                else
                    w += ch;
            return out;
        }

        std::string trimmed(const std::string& s)
        {
            const std::size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        }
    }

    std::string presetNotice(const PresetCycle& before, const PresetCycle& after)
    {
        if (after.current < 0 || after.current >= static_cast<int>(after.presets.size()))
            return "PRESET APPLIED";
        std::string out = "PRESET: " + upper(asciiText(after.presets[static_cast<std::size_t>(after.current)].name)) + "  "
                        + std::to_string(after.current + 1) + "/" + std::to_string(after.presets.size());
        if (before.applied && before.edited && before.current >= 0 && before.current < static_cast<int>(before.presets.size()))
            out += "   THE CHANGES TO " + upper(asciiText(before.presets[static_cast<std::size_t>(before.current)].name))
                 + " WERE NOT SAVED";
        return out;
    }

    void PresetPicker::open(const funkgui::Rect& anchor)
    {
        anchor_ = anchor;
        cycle_ = port_.presetCycle();
        field_.set({}, kMaxSearchLength);
        searchKept_.clear();
        categoryMode_ = false;
        categoryOf_.clear();
        why_.clear();
        open_ = true;
        first_ = 0;
        arrange();
        const int current = cycle_.current >= 0 ? rowOf(cycle_.presets[static_cast<std::size_t>(cycle_.current)].name) : -1;
        const int rows = places(width_, height_).rows;
        lit_ = -1;
        step(1, rows);                                   // the first preset, unless one is current
        if (current >= 0)
            light(current, rows);
        ++revision_;
    }

    void PresetPicker::close()
    {
        open_ = false;
        categoryMode_ = false;
        ++revision_;
    }

    void PresetPicker::setAnchor(const funkgui::Rect& anchor)
    {
        if (anchor.x != anchor_.x || anchor.y != anchor_.y || anchor.w != anchor_.w || anchor.h != anchor_.h)
        {
            anchor_ = anchor;
            ++revision_;
        }
    }

    void PresetPicker::sync()
    {
        if (!open_)
            return;
        PresetCycle now = port_.presetCycle();
        if (now == cycle_)
            return;
        if (now.project != cycle_.project)
        {
            close();                                     // another project open: its list is another
            return;
        }
        const int lit = lit_ >= 0 && lit_ < static_cast<int>(rows_.size()) ? rows_[static_cast<std::size_t>(lit_)].preset : -1;
        const std::string litName = lit >= 0 && lit < static_cast<int>(cycle_.presets.size()) ? cycle_.presets[static_cast<std::size_t>(lit)].name : std::string();
        cycle_ = std::move(now);
        arrange();
        const int rows = places(width_, height_).rows;
        if (const int r = litName.empty() ? -1 : rowOf(litName); r >= 0)
            light(r, rows);
        else
        {
            lit_ = -1;
            step(1, rows);
        }
        ++revision_;
    }

    void PresetPicker::arrange()
    {
        groups_.clear();
        rows_.clear();
        const std::vector<std::string> words = categoryMode_ ? std::vector<std::string>() : wordsOf(field_.buffer);
        captions_ = std::any_of(cycle_.presets.begin(), cycle_.presets.end(), [](const PresetEntry& p) { return !p.category.empty(); });
        // Each category's title: its least spelling over every preset of it, whatever the search shows.
        std::map<std::string, std::string> titles;       // lower case -> the least spelling
        for (const PresetEntry& p : cycle_.presets)
            if (!p.category.empty())
            {
                std::string& t = titles[lower(p.category)];
                if (t.empty() || p.category < t)
                    t = p.category;
            }
        std::map<std::string, Group> byKey;
        Group none;
        none.title = "Uncategorised";
        for (std::size_t i = 0; i < cycle_.presets.size(); ++i)
        {
            const PresetEntry& p = cycle_.presets[i];
            const std::string name = lower(p.name), category = lower(p.category);
            const bool shown = std::all_of(words.begin(), words.end(), [&](const std::string& w) {
                return name.find(w) != std::string::npos || category.find(w) != std::string::npos;
            });
            if (!shown)
                continue;
            if (p.category.empty())
                none.members.push_back(static_cast<int>(i));
            else
            {
                Group& g = byKey[category];
                g.title = titles[category];
                g.category = g.title;
                g.members.push_back(static_cast<int>(i));
            }
        }
        for (auto& [key, g] : byKey)                     // by name, case aside (the map's order)
            groups_.push_back(std::move(g));
        if (!none.members.empty())
            groups_.push_back(std::move(none));
        for (std::size_t g = 0; g < groups_.size(); ++g)
        {
            if (captions_)
                rows_.push_back({ static_cast<int>(g), -1 });
            for (const int m : groups_[g].members)
                rows_.push_back({ static_cast<int>(g), m });
        }
        first_ = std::clamp(first_, 0, std::max(0, static_cast<int>(rows_.size()) - 1));
        ++revision_;
    }

    int PresetPicker::rowOf(const std::string& preset) const
    {
        for (std::size_t r = 0; r < rows_.size(); ++r)
            if (const int i = rows_[r].preset; i >= 0 && same(cycle_.presets[static_cast<std::size_t>(i)].name, preset))
                return static_cast<int>(r);
        return -1;
    }

    PresetPicker::Places PresetPicker::places(float width, float height) const
    {
        Places at;
        const float w = std::min(std::max(kBoxW, anchor_.w + 40.0f), std::max(160.0f, width - 24.0f));
        const float x = std::clamp(std::round(anchor_.centreX() - 0.5f * w), 12.0f, std::max(12.0f, width - 12.0f - w));
        const float y = anchor_.bottom() + 6.0f;
        at.field = { x + kPad, y + kPad, w - 2.0f * kPad, kFieldH };
        at.mode = { at.field.x + 4.0f, at.field.y + 4.0f, kModeW, kFieldH - 8.0f };
        at.clear = { at.field.right() - 28.0f, at.field.y + 4.0f, 24.0f, kFieldH - 8.0f };
        const float listTop = at.field.bottom() + 8.0f;
        const float whyH = why_.empty() ? 0.0f : kWhyH;
        const int fit = static_cast<int>(std::floor((height - 16.0f - listTop - 8.0f - whyH) / at.rowH));
        const int count = std::max(1, static_cast<int>(rows_.size()));
        at.rows = std::max(1, std::min({ count, kMostRows, std::max(kLeastRows, fit) }));
        at.list = { x + 6.0f, listTop, w - 12.0f, static_cast<float>(at.rows) * at.rowH };
        at.box = { x, y, w, at.list.bottom() + 8.0f + whyH - y };
        return at;
    }

    funkgui::Rect PresetPicker::rowRect(const Places& at, int row) const
    {
        return { at.list.x, at.list.y + static_cast<float>(row - first_) * at.rowH, at.list.w, at.rowH };
    }

    int PresetPicker::rowAt(funkgui::Point p, const Places& at) const
    {
        if (!at.list.contains(p))
            return -1;
        const int r = first_ + static_cast<int>(std::floor((p.y - at.list.y) / at.rowH));
        return r >= first_ && r < first_ + at.rows && r < static_cast<int>(rows_.size()) ? r : -1;
    }

    funkgui::Point PresetPicker::centreOfRow(int row, float width, float height) const
    {
        const Places at = places(width, height);
        if (row < first_ || row >= first_ + at.rows || row >= static_cast<int>(rows_.size()))
            return { -1.0e4f, -1.0e4f };
        const funkgui::Rect r = rowRect(at, row);
        return { r.centreX(), r.centreY() };
    }

    void PresetPicker::light(int row, int rowsShown)
    {
        if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[static_cast<std::size_t>(row)].preset < 0)
            return;
        lit_ = row;
        // In view, with its caption when it is its group's first.
        const int top = row > 0 && rows_[static_cast<std::size_t>(row - 1)].preset < 0 ? row - 1 : row;
        if (top < first_)
            first_ = top;
        else if (row >= first_ + rowsShown)
            first_ = row - rowsShown + 1;
        first_ = std::clamp(first_, 0, std::max(0, static_cast<int>(rows_.size()) - rowsShown));
        ++revision_;
    }

    void PresetPicker::step(int dir, int rowsShown)
    {
        const int n = static_cast<int>(rows_.size());
        if (n == 0)
        {
            lit_ = -1;
            return;
        }
        int r = lit_ < 0 ? (dir > 0 ? -1 : n) : lit_;
        for (int k = 0; k < n; ++k)
        {
            r += dir > 0 ? 1 : -1;
            if (r < 0 || r >= n)
                return;                                  // the ends: it stays
            if (rows_[static_cast<std::size_t>(r)].preset >= 0)
            {
                light(r, rowsShown);
                return;
            }
        }
    }

    void PresetPicker::apply(int row)
    {
        if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[static_cast<std::size_t>(row)].preset < 0)
            return;
        const std::string name = cycle_.presets[static_cast<std::size_t>(rows_[static_cast<std::size_t>(row)].preset)].name;
        const PresetCycle before = port_.presetCycle();
        std::string why;
        if (!port_.applyPreset(cycle_.project, name, why))
        {
            why_ = "Not applied: " + why + ".";
            ++revision_;
            return;
        }
        notice_ = presetNotice(before, port_.presetCycle());
        close();
    }

    std::vector<std::string> PresetPicker::categoryChoices() const
    {
        std::map<std::string, std::string> titles;
        for (const PresetEntry& p : cycle_.presets)
            if (!p.category.empty())
            {
                std::string& t = titles[lower(p.category)];
                if (t.empty() || p.category < t)
                    t = p.category;
            }
        std::vector<std::string> out;
        for (const auto& [key, title] : titles)
            out.push_back(title);
        return out;
    }

    void PresetPicker::setCategory(const std::string& preset, const std::string& typed)
    {
        // Spelt as a category this project's presets have already, case aside: one group, one spelling.
        std::string category = trimmed(typed);
        for (const std::string& c : categoryChoices())
            if (same(c, category))
                category = c;
        std::string why;
        if (!port_.setPresetCategory(cycle_.project, preset, category, why))
        {
            why_ = "Not set: " + why + ".";
            ++revision_;
            return;
        }
        notice_ = category.empty() ? "PRESET " + upper(asciiText(preset)) + ": NO CATEGORY"
                                   : "PRESET " + upper(asciiText(preset)) + ": " + upper(asciiText(category));
        why_.clear();
        cycle_ = port_.presetCycle();
        arrange();
        if (const int r = rowOf(preset); r >= 0)
            light(r, places(width_, height_).rows);
    }

    void PresetPicker::showMenu(int row, funkgui::Point at)
    {
        if (host_ == nullptr || row < 0 || row >= static_cast<int>(rows_.size()) || rows_[static_cast<std::size_t>(row)].preset < 0)
            return;
        const PresetEntry& p = cycle_.presets[static_cast<std::size_t>(rows_[static_cast<std::size_t>(row)].preset)];
        const std::string name = p.name;
        const bool factory = isFactoryName(cycle_.project);
        const std::vector<std::string> choices = categoryChoices();
        funkgui::MenuRequest request;
        request.items.push_back(dropdown::header(asciiText(name)));
        request.items.push_back({ kApplyItem, "Apply" });
        funkgui::MenuItem category;
        category.label = "Category";
        category.enabled = !factory;                     // a factory project's presets stay as they are
        for (std::size_t i = 0; i < choices.size(); ++i)
            category.submenu.push_back(dropdown::choice(kChoice + static_cast<int>(i), choices[i], same(choices[i], p.category)));
        if (!choices.empty())
            category.submenu.push_back(dropdown::separator());
        category.submenu.push_back(dropdown::choice(kNoCategory, "None (Uncategorised)", p.category.empty()));
        category.submenu.push_back({ kNewCategory, "New category..." });
        request.items.push_back(std::move(category));
        request.anchor = { at.x, at.y, 1.0f, 1.0f };
        request.theme = productTheme(funkgui::Theme::byIndex(host_->themeIndex()));
        light(row, places(width_, height_).rows);
        host_->showMenu(request, [this, name, choices](int id) {
            if (!open_ || id <= 0)
                return;
            if (id == kApplyItem)
            {
                if (const int r = rowOf(name); r >= 0)
                    apply(r);
            }
            else if (id == kNoCategory)
                setCategory(name, {});
            else if (id == kNewCategory)
            {
                searchKept_ = field_.buffer;
                categoryMode_ = true;
                categoryOf_ = name;
                field_.set({}, kMaxCategoryLength);
                why_.clear();
                arrange();
                if (const int r = rowOf(name); r >= 0)
                    light(r, places(width_, height_).rows);
            }
            else if (id >= kChoice && id - kChoice < static_cast<int>(choices.size()))
                setCategory(name, choices[static_cast<std::size_t>(id - kChoice)]);
            ++revision_;
            if (answered_)
                answered_();
        });
    }

    bool PresetPicker::pointerDown(const funkgui::PointerEvent& e, funkgui::HostServices* host, float width, float height)
    {
        host_ = host;
        width_ = width;
        height_ = height;
        const Places at = places(width, height);
        const funkgui::Point p{ e.x, e.y };
        if (!at.box.contains(p))
        {
            close();
            return false;
        }
        if (at.field.contains(p))
        {
            if (!categoryMode_ && !field_.buffer.empty() && at.clear.contains(p))
            {
                field_.set({}, kMaxSearchLength);
                arrange();
                lit_ = -1;
                step(1, places(width, height).rows);
            }
            field_.allSelected = false;
            field_.caret = static_cast<int>(field_.buffer.size());
            ++revision_;
            return true;
        }
        const int row = rowAt(p, at);
        if (row < 0 || rows_[static_cast<std::size_t>(row)].preset < 0)
            return true;                                 // a caption, or under the rows
        if (e.popup)
            showMenu(row, p);
        else
            apply(row);
        return true;
    }

    void PresetPicker::wheel(const funkgui::WheelEvent& e, float width, float height)
    {
        const Places at = places(width, height);
        const float dy = e.reversed ? -e.dy : e.dy;
        const int step = dy > 0.0f ? -1 : (dy < 0.0f ? 1 : 0);
        const int most = std::max(0, static_cast<int>(rows_.size()) - at.rows);
        const int first = std::clamp(first_ + step * std::max(1, static_cast<int>(std::round(std::fabs(dy) * 6.0f))), 0, most);
        if (first != first_)
        {
            first_ = first;
            ++revision_;
        }
    }

    void PresetPicker::key(const funkgui::KeyEvent& e, funkgui::HostServices* host, float width, float height)
    {
        using funkgui::Key;
        host_ = host;
        width_ = width;
        height_ = height;
        const int rows = places(width, height).rows;
        const int most = categoryMode_ ? kMaxCategoryLength : kMaxSearchLength;
        const auto typed = [this, rows] {
            why_.clear();
            if (!categoryMode_)
            {
                arrange();                               // the search filters at once: the first match lit
                lit_ = -1;
                first_ = 0;
                step(1, rows);
            }
            ++revision_;
        };
        if (e.key == Key::escape)
        {
            if (categoryMode_)
            {
                categoryMode_ = false;
                field_.set(searchKept_, kMaxSearchLength);
                field_.allSelected = false;
                why_.clear();
                arrange();
                if (const int r = rowOf(categoryOf_); r >= 0)
                    light(r, rows);
            }
            else if (!field_.buffer.empty())
            {
                field_.set({}, kMaxSearchLength);
                typed();
                if (cycle_.current >= 0)                 // back on the current one
                    if (const int r = rowOf(cycle_.presets[static_cast<std::size_t>(cycle_.current)].name); r >= 0)
                        light(r, rows);
            }
            else
                close();
            ++revision_;
            return;
        }
        if (e.key == Key::enter)
        {
            if (categoryMode_)
            {
                if (trimmed(field_.buffer).empty())
                {
                    why_ = "Name the category first: None (Uncategorised) in a preset's menu clears it.";
                    ++revision_;
                    return;
                }
                const std::string preset = categoryOf_;
                categoryMode_ = false;
                const std::string category = field_.buffer;
                field_.set(searchKept_, kMaxSearchLength);
                field_.allSelected = false;
                setCategory(preset, category);
            }
            else
                apply(lit_);
            ++revision_;
            return;
        }
        if (e.key == Key::up || e.key == Key::down)
        {
            step(e.key == Key::up ? -1 : 1, rows);
            return;
        }
        if (e.key == Key::pageUp || e.key == Key::pageDown)
        {
            for (int k = 0; k < std::max(1, rows - 1); ++k)
                step(e.key == Key::pageUp ? -1 : 1, rows);
            return;
        }
        if (e.key == Key::tab)
            return;                                      // nothing to move to: the list is one field and its rows
        if (e.key == Key::character && e.mods.cmd && (e.ch == 'v' || e.ch == 'V'))
        {
            if (host != nullptr && field_.paste(host->clipboardText(), most))
                typed();
            return;
        }
        if (e.key == Key::character && e.mods.cmd && (e.ch == 'a' || e.ch == 'A'))
        {
            field_.allSelected = !field_.buffer.empty();
            ++revision_;
            return;
        }
        if (field_.key(e, most))
            typed();
    }

    funkgui::Cursor PresetPicker::cursor(funkgui::Point p, float width, float height) const
    {
        const Places at = places(width, height);
        if (!categoryMode_ && !field_.buffer.empty() && at.clear.contains(p))
            return funkgui::Cursor::pointingHand;
        if (const int r = rowAt(p, at); r >= 0 && rows_[static_cast<std::size_t>(r)].preset >= 0)
            return funkgui::Cursor::pointingHand;
        return funkgui::Cursor::normal;
    }

    void PresetPicker::draw(funkgui::Canvas& c, const funkgui::Theme& th, float width, float height, funkgui::Point pointer,
                            bool hasPointer) const
    {
        const chat::Palette p = chat::palette(th);
        const Places at = places(width, height);
        const funkgui::Rect& box = at.box;
        constexpr funkgui::Col black{ 0, 0, 0, 255 };
        for (int k = 4; k >= 1; --k)                     // a soft shadow under it
        {
            const float g = static_cast<float>(k) * 2.0f;
            c.rrect(box.x - g, box.y - g + 4.0f, box.w + 2.0f * g, box.h + 2.0f * g, 12.0f + g, black.withAlpha(0.045f));
        }
        c.rrect(box.x, box.y, box.w, box.h, 12.0f, p.card, 1.0f, p.cardEdge);

        // The field: the search, or the category being named (its pill).
        {
            const funkgui::Rect& f = at.field;
            c.rrect(f.x, f.y, f.w, f.h, f.h * 0.5f, p.composer, 1.0f, funkgui::mix(p.composerEdge, p.accent, 0.6f));
            float tx = f.x + 30.0f;
            if (categoryMode_)
            {
                const funkgui::Rect& m = at.mode;
                c.rrect(m.x, m.y, m.w, m.h, m.h * 0.5f, p.accent.withAlpha(0.16f), 1.0f, p.accent);
                c.text("CATEGORY", m.centreX(), c.capCentreTop(m.centreY(), chat::kSmallStrong), chat::kSmallStrong, p.accent,
                       funkgui::Align::centre);
                tx = m.right() + 8.0f;
            }
            else
                chat::drawIcon(c, chat::Icon::search, f.x + 16.0f, f.centreY(), 12.0f, p.accent);
            const bool clear = !categoryMode_ && !field_.buffer.empty();
            const float room = f.right() - tx - (clear ? 32.0f : 12.0f);
            const float ty = c.capCentreTop(f.centreY(), chat::kBody);
            if (field_.buffer.empty())
                c.text(fitText(c, categoryMode_ ? "Category name" : "Search presets", chat::kSmall, room).c_str(), tx,
                       c.capCentreTop(f.centreY(), chat::kSmall), chat::kSmall, p.faint);
            else
            {
                if (field_.allSelected)
                    c.rrect(tx - 2.0f, f.y + 6.0f, std::min(c.textWidth(field_.buffer.c_str(), chat::kBody), room) + 4.0f, f.h - 12.0f,
                            3.0f, th.accentDim);
                c.text(fitText(c, field_.buffer, chat::kBody, room).c_str(), tx, ty, chat::kBody, p.text);
            }
            if (clear)
                chat::drawIcon(c, chat::Icon::cross, at.clear.centreX(), at.clear.centreY(), 9.0f,
                               hasPointer && at.clear.contains(pointer) ? p.text : p.dim);
            const std::string before = field_.buffer.substr(0, static_cast<std::size_t>(std::clamp(field_.caret, 0, static_cast<int>(field_.buffer.size()))));
            const float cx = field_.buffer.empty() ? tx - 4.0f : tx + std::min(c.textWidth(before.c_str(), chat::kBody), room);
            c.rrect(cx, f.y + 7.0f, 1.5f, f.h - 14.0f, 0.75f, p.accent);
        }

        // The rows.
        const int n = static_cast<int>(rows_.size());
        if (n == 0)
        {
            const std::string empty = cycle_.presets.empty() ? "No presets yet: PROJECTS saves the dials as one."
                                                             : "Nothing matches: Escape clears the search.";
            c.text(fitText(c, empty, chat::kSmall, at.list.w - 16.0f).c_str(), at.list.centreX(),
                   c.capCentreTop(at.list.y + 0.5f * at.rowH, chat::kSmall), chat::kSmall, p.faint, funkgui::Align::centre);
        }
        for (int k = 0; k < at.rows && first_ + k < n; ++k)
        {
            const int r = first_ + k;
            const Row& rw = rows_[static_cast<std::size_t>(r)];
            const funkgui::Rect rr = rowRect(at, r);
            if (rw.preset < 0)
            {
                const Group& g = groups_[static_cast<std::size_t>(rw.group)];
                const std::string many = std::to_string(g.members.size());
                c.text(fitText(c, upper(asciiText(g.title)), chat::kSmallStrong, rr.w - 50.0f).c_str(), rr.x + 10.0f,
                       c.capCentreTop(rr.centreY() + 2.0f, chat::kSmallStrong), chat::kSmallStrong, p.faint);
                c.text(many.c_str(), rr.right() - 10.0f, c.capCentreTop(rr.centreY() + 2.0f, chat::kSmall), chat::kSmall, p.faint,
                       funkgui::Align::right);
                continue;
            }
            const PresetEntry& e = cycle_.presets[static_cast<std::size_t>(rw.preset)];
            const bool current = rw.preset == cycle_.current;
            const bool lit = r == lit_;
            if (lit)
                c.rrect(rr.x, rr.y + 1.0f, rr.w, rr.h - 2.0f, 7.0f, funkgui::mix(p.composer, p.accent, 0.20f));
            else if (hasPointer && rr.contains(pointer))
                c.rrect(rr.x, rr.y + 1.0f, rr.w, rr.h - 2.0f, 7.0f, p.hover);
            if (current)
                c.disc(rr.x + 13.0f, rr.centreY(), 3.0f, p.accent);
            const std::string number = std::to_string(rw.preset + 1);
            const float numberW = c.textWidth(number.c_str(), chat::kSmall);
            const funkgui::TextStyle& st = current ? chat::kStrong : chat::kBody;
            const float nameRoom = rr.w - 26.0f - numberW - 22.0f - (current && cycle_.edited ? 12.0f : 0.0f);
            const std::string shown = fitText(c, asciiText(e.name), st, nameRoom);
            c.text(shown.c_str(), rr.x + 24.0f, c.capCentreTop(rr.centreY(), st), st, p.text);
            if (current && cycle_.edited)                // the modified dot
                c.disc(rr.x + 24.0f + c.textWidth(shown.c_str(), st) + 8.0f, rr.centreY(), 3.0f, p.accent);
            c.text(number.c_str(), rr.right() - 10.0f, c.capCentreTop(rr.centreY(), chat::kSmall), chat::kSmall, p.faint,
                   funkgui::Align::right);
        }
        if (n > at.rows)
        {
            const float track = at.list.h - 8.0f;
            const float thumb = std::max(16.0f, track * static_cast<float>(at.rows) / static_cast<float>(n));
            const float y = at.list.y + 4.0f + (track - thumb) * static_cast<float>(std::min(first_, n - at.rows))
                                                   / static_cast<float>(std::max(1, n - at.rows));
            c.rrect(at.list.right() - 3.0f, y, 3.0f, thumb, 1.5f, p.dim);
        }
        if (!why_.empty())
            c.text(fitText(c, why_, chat::kSmall, box.w - 2.0f * kPad).c_str(), box.x + kPad,
                   c.capCentreTop(at.list.bottom() + 4.0f + 0.5f * kWhyH, chat::kSmall), chat::kSmall, p.danger);
    }

    void PresetPicker::accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const
    {
        const Places at = places(width, height);
        funkgui::A11yItem box;
        box.id = kBoxId;
        box.role = funkgui::a11yrole::group;
        box.bounds = at.box;
        box.title = "Presets";
        box.description = "The presets of " + ownName(cycle_.project);
        out.push_back(box);
        funkgui::A11yItem f;
        f.id = kFieldId;
        f.parent = kBoxId;
        f.role = funkgui::a11yrole::editableText;
        f.bounds = at.field;
        f.title = categoryMode_ ? "Category name" : "Search presets";
        f.value = field_.buffer;
        f.help = categoryMode_ ? "Return sets it, Escape goes back" : "Finds presets by name and category";
        out.push_back(std::move(f));
        if (!categoryMode_ && !field_.buffer.empty())
        {
            funkgui::A11yItem x;
            x.id = kClearId;
            x.parent = kBoxId;
            x.role = funkgui::A11yRole::button;
            x.bounds = at.clear;
            x.title = "Clear the search";
            out.push_back(std::move(x));
        }
        funkgui::A11yItem list;
        list.id = kListId;
        list.parent = kBoxId;
        list.role = funkgui::a11yrole::list;
        list.bounds = at.list;
        list.title = "Presets";
        list.value = std::to_string(cycle_.presets.size()) + (cycle_.presets.size() == 1 ? " preset" : " presets");
        out.push_back(std::move(list));
        const int count = static_cast<int>(cycle_.presets.size());
        for (int k = 0; k < at.rows && first_ + k < static_cast<int>(rows_.size()); ++k)
        {
            const int r = first_ + k;
            const Row& rw = rows_[static_cast<std::size_t>(r)];
            funkgui::A11yItem it;
            it.id = kRowId + static_cast<uint32_t>(r);
            it.parent = kListId;
            it.bounds = rowRect(at, r);
            if (rw.preset < 0)
            {
                const Group& g = groups_[static_cast<std::size_t>(rw.group)];
                it.role = funkgui::A11yRole::staticText;
                it.title = g.title;
                it.value = std::to_string(g.members.size()) + (g.members.size() == 1 ? " preset" : " presets");
            }
            else
            {
                const PresetEntry& e = cycle_.presets[static_cast<std::size_t>(rw.preset)];
                it.role = funkgui::A11yRole::listItem;
                it.title = e.name;
                it.value = std::to_string(rw.preset + 1) + " of " + std::to_string(count)
                         + (rw.preset == cycle_.current ? (cycle_.applied ? ", applied" : ", the dials match it") : "")
                         + (rw.preset == cycle_.current && cycle_.edited ? ", edited" : "")
                         + (e.category.empty() ? "" : ", " + e.category);
                it.checkable = true;
                it.checked = r == lit_;
            }
            out.push_back(std::move(it));
        }
    }

    bool PresetPicker::a11yAction(uint32_t id, funkgui::A11yAction a, funkgui::HostServices* host, float width, float height)
    {
        host_ = host;
        width_ = width;
        height_ = height;
        if (id == kClearId)
        {
            field_.set({}, kMaxSearchLength);
            arrange();
            lit_ = -1;
            step(1, places(width, height).rows);
            return true;
        }
        if (id >= kRowId && id < kRowId + 0x1000u)
        {
            const int r = static_cast<int>(id - kRowId);
            if (a == funkgui::A11yAction::showMenu)
            {
                const funkgui::Point p = centreOfRow(r, width, height);
                showMenu(r, p);
            }
            else
                apply(r);
            return true;
        }
        return id >= kBoxId && id < kRowId + 0x1000u;
    }

    std::string PresetPicker::takeNotice()
    {
        return std::exchange(notice_, {});
    }
}
