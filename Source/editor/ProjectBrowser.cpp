// Source/editor/ProjectBrowser.cpp: see ProjectBrowser.h.
#include "editor/ProjectBrowser.h"

#include "editor/ChatItems.h"
#include "editor/ChatStyle.h"
#include "editor/Items.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace playground::ui
{
    namespace
    {
        constexpr float kBoxW = 860.0f, kBoxH = 560.0f;
        constexpr float kPad = 24.0f;
        constexpr float kListTop = 100.0f;               // below the title, the folder's line and the lists' captions
        constexpr float kBelowLists = 132.0f;            // the field, its line, the buttons
        constexpr float kGap = 16.0f;                    // between the two columns
        constexpr float kCaptionH = 24.0f;               // the presets' caption, between the versions and them
        constexpr float kModeW = 78.0f;                  // the field's pill: PROJECT or PRESET
        constexpr float kButtonH = 32.0f;
        constexpr uint32_t kBoxId = ProjectBrowser::kA11yBase, kListId = kBoxId + 1,   // + the list
                           kFieldId = kBoxId + 4, kSaveId = kBoxId + 5, kOpenId = kBoxId + 6, kRemoveId = kBoxId + 7,
                           kCloseId = kBoxId + 8, kImportId = kBoxId + 9, kExportId = kBoxId + 10,
                           kRowId = kBoxId + 0x100;                                     // + the list * 0x1000 + the row

        using L = ProjectBrowser::List;
        constexpr std::size_t idx(L l) { return static_cast<std::size_t>(l); }

        bool same(const std::string& a, const std::string& b)
        {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                       return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
                   });
        }

        float pillW(const std::string& label) { return std::max(84.0f, std::round(7.0f * static_cast<float>(label.size()) + 34.0f)); }

        std::string v(int n) { return "v" + std::to_string(n); }
        std::string shortWhen(const std::string& when) { return when.size() > 5 ? when.substr(5) : when; }   // "10-27 14:02"
    }

    void ProjectBrowser::open(const std::string& name, bool preset)
    {
        open_ = true;
        projects_ = port_.projects();
        state_ = port_.project();
        chosen_ = { -1, -1, -1 };
        first_ = { 0, 0, 0 };
        for (std::size_t i = 0; i < projects_.size(); ++i)
            if (same(projects_[i].name, state_.name))
                chosen_[idx(L::projects)] = static_cast<int>(i);
        first_[idx(L::projects)] = std::max(0, chosen_[idx(L::projects)] - 2);
        readChosen();
        focus_ = L::projects;
        name_.set(name, kMaxNameLength);
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

    void ProjectBrowser::close()
    {
        open_ = false;
        ask_ = Ask::none;
        ++revision_;
    }

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

    void ProjectBrowser::chooseProjectNamed(const std::string& name, float width, float height)
    {
        projects_ = port_.projects();
        for (std::size_t i = 0; i < projects_.size(); ++i)
            if (same(projects_[i].name, name))
                choose(L::projects, static_cast<int>(i), places(width, height).rows[idx(L::projects)]);
    }

    std::string ProjectBrowser::chosenProject() const
    {
        const int c = chosen_[idx(L::projects)];
        return c >= 0 && c < static_cast<int>(projects_.size()) ? projects_[static_cast<std::size_t>(c)].name : std::string();
    }

    bool ProjectBrowser::chosenIsOpen() const { return !state_.name.empty() && same(chosenProject(), state_.name); }

    void ProjectBrowser::readChosen()
    {
        versions_.clear();
        presets_.clear();
        chosen_[idx(L::versions)] = chosen_[idx(L::presets)] = -1;
        first_[idx(L::versions)] = first_[idx(L::presets)] = 0;
        const std::string name = chosenProject();
        if (name.empty())
            return;
        versions_ = port_.versions(name);
        presets_ = port_.presets(name);
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
            case L::projects: return static_cast<int>(projects_.size());
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
        return at;
    }

    funkgui::Rect ProjectBrowser::listRect(const Places& at, List l) const
    {
        return l == L::projects ? at.projects : l == L::versions ? at.versions : at.presets;
    }

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
        return { r.centreX(), r.y + 4.0f + (static_cast<float>(i - first) + 0.5f) * at.rowH };
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

    void ProjectBrowser::choose(List l, int i, int rowsShown)
    {
        const int n = count(l);
        if (n == 0)
            return;
        int& c = chosen_[idx(l)];
        int& first = first_[idx(l)];
        c = std::clamp(i, 0, n - 1);
        if (c < first)
            first = c;
        if (c >= first + rowsShown)
            first = c - rowsShown + 1;
        focus_ = l;
        ask_ = Ask::none;
        why_.clear();
        typed_ = false;
        if (l == L::projects)
        {
            readChosen();
            name_.set(projects_[static_cast<std::size_t>(c)].name, kMaxNameLength);
        }
        else if (l == L::versions)
            name_.set(chosenProject(), kMaxNameLength);
        else
            name_.set(presets_[static_cast<std::size_t>(c)].name, kMaxNameLength);
        ++revision_;
    }

    void ProjectBrowser::startPreset()
    {
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
        if (focus_ == L::presets)
            return ask_ == Ask::replace ? "Replace" : "Save preset";
        if (ask_ == Ask::addTo)
            return "Add version";
        const std::string name = port_.tidyName(name_.buffer);
        const bool exists = std::any_of(projects_.begin(), projects_.end(), [&](const ProjectEntry& p) { return same(p.name, name); });
        return exists ? "Save version" : "Save project";
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
                        && std::any_of(projects_.begin(), projects_.end(), [&](const ProjectEntry& p) { return same(p.name, name); });
        if (other && ask_ != Ask::addTo)
        {
            ask_ = Ask::addTo;
            why_ = "\"" + name + "\" is another project: Add version saves this program as its newest version.";
            return;
        }
        SavedVersion saved;
        if (!port_.saveVersion(name, saved, why))
        {
            ask_ = Ask::none;
            why_ = "Not saved: " + why + ".";
            return;
        }
        const std::string at = upper(name) + " " + upper(v(saved.number));
        notice_ = saved.unchanged ? "NOTHING NEW TO SAVE: " + at + " HAS IT" : saved.created ? "PROJECT SAVED: " + upper(name) : "SAVED: " + at;
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
                why_ = "It opens \"" + chosenProject() + "\": the editor's changes no version holds are lost. Apply anyway does.";
                return;
            }
            const std::string name = presets_[static_cast<std::size_t>(c)].name;
            if (!port_.applyPreset(chosenProject(), name, why))
            {
                ask_ = Ask::none;
                why_ = "Not applied: " + why + ".";
                return;
            }
            notice_ = "PRESET: " + upper(name);
            close();
            return;
        }
        const int p = chosen_[idx(L::projects)];
        if (p < 0)
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
        const std::string name = chosenProject();
        if (!port_.openProject(name, version, why))
        {
            ask_ = Ask::none;
            why_ = "Not opened: " + why + ".";
            return;
        }
        notice_ = "OPENED: " + upper(name) + " " + upper(v(version > 0 ? version : projects_[static_cast<std::size_t>(p)].latest));
        close();
    }

    void ProjectBrowser::remove()
    {
        const int c = chosen_[idx(focus_)];
        if (c < 0 || c >= count(focus_))
        {
            ask_ = Ask::none;
            why_ = focus_ == L::projects ? "Choose a project first." : focus_ == L::versions ? "Choose a version first." : "Choose a preset first.";
            return;
        }
        const std::string project = chosenProject();
        std::string what;
        if (focus_ == L::projects)
            what = "\"" + project + "\", every version and preset,";
        else if (focus_ == L::versions)
            what = v(versions_[static_cast<std::size_t>(c)].number) + " of \"" + project + "\"";
        else
            what = "the preset \"" + presets_[static_cast<std::size_t>(c)].name + "\"";
        if (ask_ != Ask::remove)
        {
            ask_ = Ask::remove;
            why_ = "Delete " + what + " from this Mac? Delete it does.";
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
        projects_ = port_.projects();
        state_ = port_.project();
        int keep = -1;
        if (from != L::projects)
            for (std::size_t i = 0; i < projects_.size(); ++i)
                if (same(projects_[i].name, project))
                    keep = static_cast<int>(i);
        chosen_[idx(L::projects)] = keep;
        first_[idx(L::projects)] = std::min(first_[idx(L::projects)], std::max(0, static_cast<int>(projects_.size()) - 1));
        readChosen();
        focus_ = from == L::projects ? L::projects : from;
        what[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(what[0])));
        if (from == L::projects)
            why_ = "Deleted \"" + project + "\".";
        else
            why_ = "Deleted " + what + ".";
        ++revision_;
    }

    void ProjectBrowser::pointerDown(funkgui::Point p, float width, float height)
    {
        const Places at = places(width, height);
        for (const List l : { L::projects, L::versions, L::presets })
            if (listRect(at, l).contains(p))
            {
                if (const int row = rowAt(p, at, l); row >= 0)
                    choose(l, row, at.rows[idx(l)]);
                else if (l == L::presets && !chosenProject().empty())
                    startPreset();
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
            if (chosenProject().empty())
            {
                ask_ = Ask::none;
                why_ = projects_.empty() ? "There is no project yet: name this program and Save it." : "Choose a project to export.";
            }
            else
            {
                export_ = chosenProject();
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

    void ProjectBrowser::doubleClick(funkgui::Point p, float width, float height)
    {
        const Places at = places(width, height);
        for (const List l : { L::projects, L::versions, L::presets })
            if (const int row = rowAt(p, at, l); row >= 0)
            {
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
        const Places at = places(width, height);
        const auto edited = [this] {
            typed_ = true;
            ask_ = Ask::none;
            why_.clear();
            ++revision_;
        };
        if (e.key == Key::escape)
            close();
        else if (e.key == Key::enter)
        {
            const int c = chosen_[idx(focus_)];
            if (typed_ || c < 0)
                save();
            else
                openChosen();
        }
        else if (e.key == Key::up || e.key == Key::down)
        {
            const int c = chosen_[idx(focus_)];
            choose(focus_, c < 0 ? 0 : c + (e.key == Key::up ? -1 : 1), at.rows[idx(focus_)]);
        }
        else if (e.key == Key::tab)
        {
            const List next = static_cast<List>((idx(focus_) + (e.mods.shift ? 2u : 1u)) % 3u);
            if (const int c = chosen_[idx(next)]; c >= 0)
                choose(next, c, at.rows[idx(next)]);
            else if (next == L::presets && !chosenProject().empty())
                startPreset();
            else
            {
                focus_ = next;
                ask_ = Ask::none;
                ++revision_;
            }
        }
        else if (e.key == Key::character && e.mods.cmd && (e.ch == 'v' || e.ch == 'V'))
        {
            if (host != nullptr && name_.paste(host->clipboardText(), kMaxNameLength))
                edited();
        }
        else if (e.key == Key::character && e.mods.cmd && (e.ch == 'a' || e.ch == 'A'))
            name_.allSelected = !name_.buffer.empty();
        else if (name_.key(e, kMaxNameLength))
            edited();
    }

    funkgui::Cursor ProjectBrowser::cursor(funkgui::Point p, float width, float height) const
    {
        const Places at = places(width, height);
        if (at.save.contains(p) || at.open.contains(p) || at.remove.contains(p) || at.close.contains(p) || at.importIt.contains(p)
            || at.exportIt.contains(p))
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

        // The title, and where the projects are.
        const float x = box.x + kPad;
        c.rrect(x, box.y + 22.0f, 30.0f, 30.0f, 9.0f, p.accent.withAlpha(0.16f));
        chat::drawIcon(c, chat::Icon::list, x + 15.0f, box.y + 37.0f, 15.0f, p.accent);
        c.text("Projects", x + 42.0f, c.capCentreTop(box.y + 37.0f, chat::kH2), chat::kH2, p.text);
        const std::string howMany = std::to_string(projects_.size()) + (projects_.size() == 1 ? " project" : " projects");
        c.text(howMany.c_str(), box.right() - kPad, c.capCentreTop(box.y + 37.0f, chat::kSmall), chat::kSmall, p.faint,
               funkgui::Align::right);
        const std::string folder = "Saved on this Mac in " + port_.projectFolder()
                                 + "   Each save is a version; a project's presets are dial settings for it";
        c.text(fitText(c, folder, chat::kSmall, box.w - 2.0f * kPad).c_str(), x, c.capCentreTop(box.y + 62.0f, chat::kSmall),
               chat::kSmall, p.soft);

        // The lists, each under its caption.
        const std::string of = chosenProject().empty() ? std::string() : " OF " + upper(asciiText(chosenProject()));
        const auto caption = [&](const funkgui::Rect& r, List l, const std::string& text) {
            const funkgui::TextStyle& st = chat::kSmallStrong;
            c.text(fitText(c, text, st, r.w).c_str(), r.x + 2.0f, c.capCentreTop(r.y - 11.0f, st), st, focus_ == l ? p.text : p.faint);
        };
        caption(at.projects, L::projects, "PROJECTS");
        caption(at.versions, L::versions, "VERSIONS" + of);
        caption(at.presets, L::presets, "PRESETS" + of);
        const auto list = [&](List l, const std::string& empty, auto&& row) {
            const funkgui::Rect r = listRect(at, l);
            c.rrect(r.x, r.y, r.w, r.h, 10.0f, p.composer, 1.0f, focus_ == l ? funkgui::mix(p.composerEdge, p.accent, 0.45f) : p.composerEdge);
            const int n = count(l), rows = at.rows[idx(l)], first = first_[idx(l)];
            if (n == 0 && !empty.empty())
                c.text(fitText(c, empty, chat::kSmall, r.w - 24.0f).c_str(), r.centreX(), c.capCentreTop(r.y + 4.0f + 0.5f * at.rowH, chat::kSmall),
                       chat::kSmall, p.faint, funkgui::Align::centre);
            const bool scrolls = n > rows;
            const float rowW = r.w - 8.0f - (scrolls ? 8.0f : 0.0f);
            for (int k = 0; k < rows && first + k < n; ++k)
            {
                const int i = first + k;
                const funkgui::Rect rr{ r.x + 4.0f, r.y + 4.0f + static_cast<float>(k) * at.rowH, rowW, at.rowH };
                const bool chosen = i == chosen_[idx(l)];
                if (chosen)
                    c.rrect(rr.x, rr.y + 1.0f, rr.w, rr.h - 2.0f, 7.0f, funkgui::mix(p.composer, p.accent, focus_ == l ? 0.22f : 0.10f));
                else if (hasPointer && rr.contains(pointer))
                    c.rrect(rr.x, rr.y + 1.0f, rr.w, rr.h - 2.0f, 7.0f, p.hover);
                row(i, rr, chosen);
            }
            if (scrolls)
            {
                const float track = r.h - 12.0f;
                const float thumb = std::max(18.0f, track * static_cast<float>(rows) / static_cast<float>(n));
                const float y = r.y + 6.0f + (track - thumb) * static_cast<float>(first) / static_cast<float>(std::max(1, n - rows));
                c.rrect(r.right() - 9.0f, y, 4.0f, thumb, 2.0f, p.dim);
            }
        };
        const auto mark = [&](const funkgui::Rect& rr) { c.disc(rr.x + 12.0f, rr.centreY(), 3.0f, p.accent); };
        list(L::projects, "No projects yet: name this program below and Save it.", [&](int i, const funkgui::Rect& rr, bool chosen) {
            const ProjectEntry& e = projects_[static_cast<std::size_t>(i)];
            if (same(e.name, state_.name))
                mark(rr);
            const std::string right = v(e.latest) + "   " + shortWhen(e.when);
            const float rightW = c.textWidth(right.c_str(), chat::kSmall);
            const funkgui::TextStyle& st = chosen ? chat::kStrong : chat::kBody;
            c.text(fitText(c, asciiText(e.name), st, rr.w - rightW - 48.0f).c_str(), rr.x + 24.0f, c.capCentreTop(rr.centreY(), st), st, p.text);
            c.text(right.c_str(), rr.right() - 10.0f, c.capCentreTop(rr.centreY(), chat::kSmall), chat::kSmall, p.faint, funkgui::Align::right);
        });
        list(L::versions, chosenProject().empty() ? "Choose a project." : "", [&](int i, const funkgui::Rect& rr, bool chosen) {
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
        const std::string noPresets = chosenProject().empty() ? std::string()
                                    : chosenIsOpen()         ? "No presets yet: click here, name one, Save preset."
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
        const bool preset = focus_ == L::presets;
        const funkgui::Rect& m = at.mode;
        c.rrect(m.x, m.y, m.w, m.h, 10.0f, preset ? p.accent.withAlpha(0.16f) : p.composer, 1.0f, preset ? p.accent : p.composerEdge);
        c.text(preset ? "PRESET" : "PROJECT", m.centreX(), c.capCentreTop(m.centreY(), chat::kSmallStrong), chat::kSmallStrong,
               preset ? p.accent : p.soft, funkgui::Align::centre);
        const funkgui::Rect& f = at.field;
        c.rrect(f.x, f.y, f.w, f.h, 10.0f, p.composer, 1.0f, funkgui::mix(p.composerEdge, p.accent, 0.45f));
        const float textTop = c.capCentreTop(f.centreY(), chat::kBody);
        const float maxW = f.w - 28.0f;
        if (name_.buffer.empty())
            c.text(preset ? "Preset name" : "Project name", f.x + 14.0f, textTop, chat::kBody, p.faint);
        else
        {
            const float w = std::min(c.textWidth(name_.buffer.c_str(), chat::kBody), maxW);
            if (name_.allSelected)
                c.rrect(f.x + 12.0f, f.y + 8.0f, w + 4.0f, f.h - 16.0f, 3.0f, th.accentDim);
            c.text(fitText(c, name_.buffer, chat::kBody, maxW).c_str(), f.x + 14.0f, textTop, chat::kBody, p.text);
        }
        const std::string before = name_.buffer.substr(0, static_cast<std::size_t>(std::clamp(name_.caret, 0, static_cast<int>(name_.buffer.size()))));
        const float cx = f.x + 14.0f + std::min(c.textWidth(before.c_str(), chat::kBody), maxW);
        c.rrect(cx, f.y + 9.0f, 1.5f, f.h - 18.0f, 0.75f, p.accent);
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
        const bool canOpen = preset ? chosen_[idx(L::presets)] >= 0 : chosen_[idx(L::projects)] >= 0;
        button(at.save, saveLabel(), 0, true);
        button(at.open, openLabel(), 1, canOpen);
        button(at.close, "Close", 1, true);
        button(at.remove, removeLabel(), 2, chosen_[idx(focus_)] >= 0);
        button(at.importIt, "Import...", 1, true);
        button(at.exportIt, "Export...", 1, chosen_[idx(L::projects)] >= 0);
    }

    void ProjectBrowser::accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const
    {
        const Places at = places(width, height);
        funkgui::A11yItem box;
        box.id = kBoxId;
        box.role = funkgui::a11yrole::group;
        box.bounds = at.box;
        box.title = "Projects";
        box.description = "Saved on this Mac in " + port_.projectFolder();
        out.push_back(box);
        const std::string of = chosenProject().empty() ? std::string() : " of " + chosenProject();
        const auto list = [&](List l, const std::string& title, auto&& rowText) {
            funkgui::A11yItem it;
            it.id = kListId + static_cast<uint32_t>(idx(l));
            it.parent = kBoxId;
            it.role = funkgui::a11yrole::list;
            it.bounds = listRect(at, l);
            it.title = title;
            it.value = std::to_string(count(l)) + (focus_ == l ? ", chosen from" : "");
            out.push_back(it);
            const int first = first_[idx(l)];
            for (int k = 0; k < at.rows[idx(l)] && first + k < count(l); ++k)
            {
                const int i = first + k;
                funkgui::A11yItem row;
                row.id = kRowId + static_cast<uint32_t>(idx(l)) * 0x1000u + static_cast<uint32_t>(i);
                row.parent = it.id;
                row.role = funkgui::A11yRole::listItem;
                const funkgui::Rect r = listRect(at, l);
                row.bounds = { r.x + 4.0f, r.y + 4.0f + static_cast<float>(k) * at.rowH, r.w - 8.0f, at.rowH };
                std::string value;
                row.title = rowText(i, value);
                row.value = (i == chosen_[idx(l)] ? "chosen, " : "") + value;
                out.push_back(std::move(row));
            }
        };
        list(L::projects, "Projects", [&](int i, std::string& value) {
            const ProjectEntry& e = projects_[static_cast<std::size_t>(i)];
            value = (same(e.name, state_.name) ? "open, " : "") + std::string("version ") + std::to_string(e.latest) + ", saved " + e.when;
            return e.name;
        });
        list(L::versions, "Versions" + of, [&](int i, std::string& value) {
            const VersionEntry& e = versions_[static_cast<std::size_t>(i)];
            value = (chosenIsOpen() && e.number == state_.version ? "open, " : "") + std::string("saved ") + e.when + ", " + e.note;
            return "Version " + std::to_string(e.number);
        });
        list(L::presets, "Presets" + of, [&](int i, std::string& value) {
            const PresetEntry& e = presets_[static_cast<std::size_t>(i)];
            value = (chosenIsOpen() && same(e.name, state_.preset) ? "applied, " : "") + std::string("saved ") + e.when;
            return e.name;
        });
        funkgui::A11yItem field;
        field.id = kFieldId;
        field.parent = kBoxId;
        field.role = funkgui::a11yrole::editableText;
        field.bounds = at.field;
        field.title = focus_ == L::presets ? "Preset name" : "Project name";
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
    }

    bool ProjectBrowser::a11yAction(uint32_t id, funkgui::A11yAction a, float width, float height)
    {
        if (a != funkgui::A11yAction::press && a != funkgui::A11yAction::focus)
            return false;
        if (id == kSaveId)
            save();
        else if (id == kOpenId)
            openChosen();
        else if (id == kRemoveId)
            remove();
        else if (id == kCloseId)
            close();
        else if (id == kImportId || id == kExportId)
        {
            const Places at = places(width, height);
            const funkgui::Rect& r = id == kImportId ? at.importIt : at.exportIt;
            pointerDown({ r.centreX(), r.centreY() }, width, height);
        }
        else if (id >= kRowId && id < kRowId + 3u * 0x1000u)
        {
            const auto l = static_cast<List>((id - kRowId) / 0x1000u);
            const int i = static_cast<int>((id - kRowId) % 0x1000u);
            if (i >= count(l))
                return false;
            choose(l, i, places(width, height).rows[idx(l)]);
        }
        else
            return false;
        return true;
    }
}
