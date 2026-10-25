// Source/editor/PresetBrowser.cpp: see PresetBrowser.h.
#include "editor/PresetBrowser.h"

#include "editor/ChatStyle.h"
#include "editor/Items.h"

#include <funkgui/canvas/Tags.h>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace testingplaygroundsynth::ui
{
    namespace
    {
        constexpr float kBoxW = 600.0f, kBoxH = 480.0f;
        constexpr float kPad = 24.0f;
        constexpr float kListTop = 78.0f;                // below the title and the folder's line
        constexpr float kBelowList = 132.0f;             // the field, its line, the buttons
        constexpr float kButtonH = 32.0f;
        constexpr uint32_t kBoxId = PresetBrowser::kA11yBase, kListId = kBoxId + 1, kFieldId = kBoxId + 2,
                           kSaveId = kBoxId + 3, kLoadId = kBoxId + 4, kRemoveId = kBoxId + 5, kCloseId = kBoxId + 6,
                           kRowId = kBoxId + 16;         // + the row's index

        bool sameName(const std::string& a, const std::string& b)
        {
            return a.size() == b.size()
                && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                       return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
                   });
        }

        float pillW(const funkgui::Canvas* c, const std::string& label)
        {
            const float w = c != nullptr ? c->textWidth(label.c_str(), chat::kSmallStrong) : 7.0f * static_cast<float>(label.size());
            return std::max(84.0f, std::round(w + 34.0f));
        }
    }

    void PresetBrowser::open(const std::string& name)
    {
        open_ = true;
        list_ = port_.presets();
        current_ = port_.presetName();
        chosen_ = -1;
        first_ = 0;
        for (std::size_t i = 0; i < list_.size(); ++i)
            if (sameName(list_[i].name, current_))
                chosen_ = static_cast<int>(i);
        if (chosen_ >= 0)
            first_ = std::max(0, chosen_ - 2);
        name_.set(name, kMaxNameLength);
        typed_ = false;
        ask_ = Ask::none;
        why_.clear();
        ++revision_;
    }

    void PresetBrowser::close()
    {
        open_ = false;
        ask_ = Ask::none;
        ++revision_;
    }

    std::string PresetBrowser::takeNotice()
    {
        std::string n;
        n.swap(notice_);
        return n;
    }

    PresetBrowser::Places PresetBrowser::places(float width, float height) const
    {
        Places at;
        const float w = std::min(kBoxW, width - 32.0f), h = std::min(kBoxH, height - 32.0f);
        at.box = { std::round((width - w) * 0.5f), std::round((height - h) * 0.45f), w, h };
        const float listH = std::max(at.rowH, h - kListTop - kBelowList);
        at.rows = std::max(1, static_cast<int>(std::floor((listH - 8.0f) / at.rowH)));
        at.list = { at.box.x + kPad, at.box.y + kListTop, w - 2.0f * kPad, static_cast<float>(at.rows) * at.rowH + 8.0f };
        at.field = { at.box.x + kPad, at.list.bottom() + 16.0f, w - 2.0f * kPad, 34.0f };
        const float by = at.box.bottom() - kPad - kButtonH;
        const float saveW = pillW(nullptr, saveLabel()), loadW = pillW(nullptr, loadLabel());
        at.save = { at.box.right() - kPad - saveW, by, saveW, kButtonH };
        at.load = { at.save.x - 8.0f - loadW, by, loadW, kButtonH };
        at.close = { at.load.x - 8.0f - 84.0f, by, 84.0f, kButtonH };
        at.remove = { at.box.x + kPad, by, pillW(nullptr, removeLabel()), kButtonH };
        return at;
    }

    int PresetBrowser::rowAt(funkgui::Point p, const Places& at) const
    {
        if (!at.list.contains(p))
            return -1;
        const int row = first_ + static_cast<int>(std::floor((p.y - at.list.y - 4.0f) / at.rowH));
        return row >= first_ && row < first_ + at.rows && row < static_cast<int>(list_.size()) ? row : -1;
    }

    funkgui::Point PresetBrowser::centreOfRow(int i, float width, float height) const
    {
        const Places at = places(width, height);
        if (i < first_ || i >= first_ + at.rows || i >= static_cast<int>(list_.size()))
            return { -1.0e4f, -1.0e4f };
        return { at.list.centreX(), at.list.y + 4.0f + (static_cast<float>(i - first_) + 0.5f) * at.rowH };
    }

    void PresetBrowser::choose(int i, int rowsShown)
    {
        if (list_.empty())
            return;
        chosen_ = std::clamp(i, 0, static_cast<int>(list_.size()) - 1);
        if (chosen_ < first_)
            first_ = chosen_;
        if (chosen_ >= first_ + rowsShown)
            first_ = chosen_ - rowsShown + 1;
        name_.set(list_[static_cast<std::size_t>(chosen_)].name, kMaxNameLength);
        typed_ = false;
        ask_ = Ask::none;
        why_.clear();
        ++revision_;
    }

    void PresetBrowser::save()
    {
        const std::string name = port_.tidyPresetName(name_.buffer);
        if (name.empty())
        {
            ask_ = Ask::none;
            why_ = "Name the preset first.";
            return;
        }
        bool taken = false;
        for (const PresetEntry& e : list_)
            taken = taken || sameName(e.name, name);
        if (taken && ask_ != Ask::replace)
        {
            ask_ = Ask::replace;
            why_ = "\"" + name + "\" is saved already: Replace saves over it.";
            return;
        }
        std::string why;
        if (!port_.savePreset(name, why))
        {
            ask_ = Ask::none;
            why_ = "Not saved: " + why + ".";
            return;
        }
        notice_ = "PRESET SAVED: " + upper(name);
        close();
    }

    void PresetBrowser::load()
    {
        if (chosen_ < 0 || chosen_ >= static_cast<int>(list_.size()))
        {
            ask_ = Ask::none;
            why_ = list_.empty() ? "There is no preset yet: name this program and Save it." : "Choose a preset first.";
            return;
        }
        const std::string name = list_[static_cast<std::size_t>(chosen_)].name;
        if (port_.presetEdited() && ask_ != Ask::load)
        {
            ask_ = Ask::load;
            why_ = "The editor's files have changes no preset holds: Load anyway replaces them.";
            return;
        }
        std::string why;
        if (!port_.loadPreset(name, why))
        {
            ask_ = Ask::none;
            why_ = "Not loaded: " + why + ".";
            return;
        }
        notice_ = "PRESET: " + upper(name);
        close();
    }

    void PresetBrowser::remove()
    {
        if (chosen_ < 0 || chosen_ >= static_cast<int>(list_.size()))
        {
            ask_ = Ask::none;
            why_ = "Choose a preset first.";
            return;
        }
        const std::string name = list_[static_cast<std::size_t>(chosen_)].name;
        if (ask_ != Ask::remove)
        {
            ask_ = Ask::remove;
            why_ = "Delete \"" + name + "\" from this Mac? Delete it does.";
            return;
        }
        std::string why;
        ask_ = Ask::none;
        if (!port_.deletePreset(name, why))
        {
            why_ = "Not deleted: " + why + ".";
            return;
        }
        list_ = port_.presets();
        current_ = port_.presetName();
        chosen_ = -1;
        first_ = std::min(first_, std::max(0, static_cast<int>(list_.size()) - 1));
        why_ = "Deleted \"" + name + "\".";
        ++revision_;
    }

    void PresetBrowser::pointerDown(funkgui::Point p, float width, float height)
    {
        const Places at = places(width, height);
        if (const int row = rowAt(p, at); row >= 0)
            choose(row, at.rows);
        else if (at.save.contains(p))
            save();
        else if (at.load.contains(p))
            load();
        else if (at.remove.contains(p))
            remove();
        else if (at.close.contains(p))
            close();
        else if (at.field.contains(p))
        {
            name_.allSelected = false;
            name_.caret = static_cast<int>(name_.buffer.size());
        }
    }

    void PresetBrowser::doubleClick(funkgui::Point p, float width, float height)
    {
        const Places at = places(width, height);
        if (const int row = rowAt(p, at); row >= 0)
        {
            if (row != chosen_)
                choose(row, at.rows);
            load();
        }
    }

    void PresetBrowser::wheel(const funkgui::WheelEvent& e, float width, float height)
    {
        const Places at = places(width, height);
        if (!at.list.contains({ e.x, e.y }))
            return;
        const int most = std::max(0, static_cast<int>(list_.size()) - at.rows);
        const float dy = e.reversed ? -e.dy : e.dy;
        const int step = dy > 0.0f ? -1 : (dy < 0.0f ? 1 : 0);
        first_ = std::clamp(first_ + step * std::max(1, static_cast<int>(std::round(std::fabs(dy) * 6.0f))), 0, most);
        ++revision_;
    }

    void PresetBrowser::key(const funkgui::KeyEvent& e, funkgui::HostServices* host, float width, float height)
    {
        using funkgui::Key;
        const Places at = places(width, height);
        if (e.key == Key::escape)
            close();
        else if (e.key == Key::enter)
        {
            if (typed_ || chosen_ < 0)
                save();
            else
                load();
        }
        else if (e.key == Key::up || e.key == Key::down)
            choose(chosen_ < 0 ? 0 : chosen_ + (e.key == Key::up ? -1 : 1), at.rows);
        else if (e.key == Key::character && e.mods.cmd && (e.ch == 'v' || e.ch == 'V'))
        {
            if (host != nullptr && name_.paste(host->clipboardText(), kMaxNameLength))
            {
                typed_ = true;
                ask_ = Ask::none;
                why_.clear();
            }
        }
        else if (e.key == Key::character && e.mods.cmd && (e.ch == 'a' || e.ch == 'A'))
            name_.allSelected = !name_.buffer.empty();
        else if (name_.key(e, kMaxNameLength))
        {
            typed_ = true;
            ask_ = Ask::none;
            why_.clear();
        }
    }

    funkgui::Cursor PresetBrowser::cursor(funkgui::Point p, float width, float height) const
    {
        const Places at = places(width, height);
        if (at.save.contains(p) || at.load.contains(p) || at.remove.contains(p) || at.close.contains(p) || rowAt(p, at) >= 0)
            return funkgui::Cursor::pointingHand;
        return funkgui::Cursor::normal;
    }

    void PresetBrowser::draw(funkgui::Canvas& c, const funkgui::Theme& th, float width, float height, funkgui::Point pointer,
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

        // The title, and where the presets are.
        const float x = box.x + kPad;
        c.rrect(x, box.y + 22.0f, 30.0f, 30.0f, 9.0f, p.accent.withAlpha(0.16f));
        chat::drawIcon(c, chat::Icon::file, x + 15.0f, box.y + 37.0f, 15.0f, p.accent);
        c.text("Presets", x + 42.0f, c.capCentreTop(box.y + 37.0f, chat::kH2), chat::kH2, p.text);
        const std::string count = std::to_string(list_.size()) + (list_.size() == 1 ? " preset" : " presets");
        c.text(count.c_str(), box.right() - kPad, c.capCentreTop(box.y + 37.0f, chat::kSmall), chat::kSmall, p.faint,
               funkgui::Align::right);
        const std::string folder = "Saved on this Mac in " + port_.presetFolder();
        c.text(fitText(c, folder, chat::kSmall, box.w - 2.0f * kPad).c_str(), x, c.capCentreTop(box.y + 62.0f, chat::kSmall),
               chat::kSmall, p.soft);

        // The list.
        const funkgui::Rect& l = at.list;
        c.rrect(l.x, l.y, l.w, l.h, 10.0f, p.composer, 1.0f, p.composerEdge);
        if (list_.empty())
            c.text("No presets yet: name this program below and Save it.", l.centreX(),
                   c.capCentreTop(l.y + 4.0f + 0.5f * at.rowH, chat::kSmall), chat::kSmall, p.faint, funkgui::Align::centre);
        const bool scrolls = static_cast<int>(list_.size()) > at.rows;
        const float rowW = l.w - 8.0f - (scrolls ? 8.0f : 0.0f);
        for (int r = 0; r < at.rows && first_ + r < static_cast<int>(list_.size()); ++r)
        {
            const int i = first_ + r;
            const PresetEntry& e = list_[static_cast<std::size_t>(i)];
            const funkgui::Rect row{ l.x + 4.0f, l.y + 4.0f + static_cast<float>(r) * at.rowH, rowW, at.rowH };
            const bool hover = hasPointer && row.contains(pointer);
            if (i == chosen_)
                c.rrect(row.x, row.y + 1.0f, row.w, row.h - 2.0f, 7.0f, funkgui::mix(p.composer, p.accent, 0.22f));
            else if (hover)
                c.rrect(row.x, row.y + 1.0f, row.w, row.h - 2.0f, 7.0f, p.hover);
            const bool current = sameName(e.name, current_);
            if (current)
                c.disc(row.x + 12.0f, row.centreY(), 3.0f, p.accent);
            const float textTop = c.capCentreTop(row.centreY(), chat::kBody);
            const float whenW = c.textWidth(e.when.c_str(), chat::kSmall);
            c.text(fitText(c, e.name, i == chosen_ ? chat::kStrong : chat::kBody, row.w - whenW - 56.0f).c_str(),
                   row.x + 24.0f, textTop, i == chosen_ ? chat::kStrong : chat::kBody, p.text);
            c.text(e.when.c_str(), row.right() - 12.0f, c.capCentreTop(row.centreY(), chat::kSmall), chat::kSmall, p.faint,
                   funkgui::Align::right);
        }
        if (scrolls)
        {
            const float track = l.h - 12.0f;
            const float thumb = std::max(18.0f, track * static_cast<float>(at.rows) / static_cast<float>(list_.size()));
            const int most = static_cast<int>(list_.size()) - at.rows;
            const float y = l.y + 6.0f + (track - thumb) * static_cast<float>(first_) / static_cast<float>(std::max(1, most));
            c.rrect(l.right() - 9.0f, y, 4.0f, thumb, 2.0f, p.dim);
        }

        // The field: the name, its caret, and the line under it.
        const funkgui::Rect& f = at.field;
        c.rrect(f.x, f.y, f.w, f.h, 10.0f, p.composer, 1.0f, funkgui::mix(p.composerEdge, p.accent, 0.45f));
        const float textTop = c.capCentreTop(f.centreY(), chat::kBody);
        const float maxW = f.w - 28.0f;
        if (name_.buffer.empty())
            c.text("Name", f.x + 14.0f, textTop, chat::kBody, p.faint);
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
            c.text(fitText(c, why_, chat::kSmall, f.w).c_str(), f.x + 2.0f, c.capCentreTop(f.bottom() + 15.0f, chat::kSmall),
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
        const bool chose = chosen_ >= 0 && chosen_ < static_cast<int>(list_.size());
        button(at.save, saveLabel(), 0, true);
        button(at.load, loadLabel(), 1, chose);
        button(at.close, "Close", 1, true);
        button(at.remove, removeLabel(), 2, chose);
    }

    void PresetBrowser::accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const
    {
        const Places at = places(width, height);
        funkgui::A11yItem box;
        box.id = kBoxId;
        box.role = funkgui::a11yrole::group;
        box.bounds = at.box;
        box.title = "Presets";
        box.description = "Saved on this Mac in " + port_.presetFolder();
        out.push_back(box);
        funkgui::A11yItem list;
        list.id = kListId;
        list.parent = kBoxId;
        list.role = funkgui::a11yrole::list;
        list.bounds = at.list;
        list.title = "Saved presets";
        list.value = std::to_string(list_.size()) + " presets";
        out.push_back(list);
        for (int r = 0; r < at.rows && first_ + r < static_cast<int>(list_.size()); ++r)
        {
            const int i = first_ + r;
            funkgui::A11yItem row;
            row.id = kRowId + static_cast<uint32_t>(i);
            row.parent = kListId;
            row.role = funkgui::A11yRole::listItem;
            row.bounds = { at.list.x + 4.0f, at.list.y + 4.0f + static_cast<float>(r) * at.rowH, at.list.w - 8.0f, at.rowH };
            row.title = list_[static_cast<std::size_t>(i)].name;
            row.value = (i == chosen_ ? "chosen, " : "") + std::string("saved ") + list_[static_cast<std::size_t>(i)].when;
            out.push_back(std::move(row));
        }
        funkgui::A11yItem field;
        field.id = kFieldId;
        field.parent = kBoxId;
        field.role = funkgui::a11yrole::editableText;
        field.bounds = at.field;
        field.title = "Preset name";
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
        button(kLoadId, at.load, loadLabel());
        button(kCloseId, at.close, "Close");
        button(kRemoveId, at.remove, removeLabel());
    }

    bool PresetBrowser::a11yAction(uint32_t id, funkgui::A11yAction a, float width, float height)
    {
        if (a != funkgui::A11yAction::press && a != funkgui::A11yAction::focus)
            return false;
        if (id == kSaveId)
            save();
        else if (id == kLoadId)
            load();
        else if (id == kRemoveId)
            remove();
        else if (id == kCloseId)
            close();
        else if (id >= kRowId && id < kRowId + static_cast<uint32_t>(list_.size()))
            choose(static_cast<int>(id - kRowId), places(width, height).rows);
        else
            return false;
        return true;
    }
}
