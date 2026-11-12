// Source/editor/DiffView.cpp: see DiffView.h.
#include "editor/DiffView.h"

#include "editor/ChatStyle.h"
#include "editor/Items.h"

#include "dsp/Sources.h"
#include "dsp/TextDiff.h"
#include "fminus/CirDoc.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace playground::ui
{
    namespace
    {
        constexpr float kChangeW = 26.0f;                // a change button

        // A line as the view shows it: ASCII, a tab as four spaces, no carriage return.
        std::string shown(std::string_view line)
        {
            std::string out;
            for (const char ch : asciiText(line))
                if (ch == '\t')
                    out += "    ";
                else if (ch != '\r')
                    out += ch;
            return out;
        }

        float tabW(const std::string& label) { return std::clamp(6.6f * static_cast<float>(label.size()) + 26.0f, 64.0f, 240.0f); }

        bool changedRow(const DiffView::Line& l)
        {
            return l.kind != DiffView::Line::Kind::same && l.kind != DiffView::Line::Kind::gap;
        }
    }

    // ---- the diff ------------------------------------------------------------------------------------------------------
    std::vector<DiffView::FileDiff> DiffView::diffOf(const Sources& a, const Sources& b)
    {
        const auto named = [](const Sources& s, const std::string& name) -> const SourceFile* {
            for (const SourceFile& f : s)
                if (f.name == name)
                    return &f;
            return nullptr;
        };
        std::vector<std::string> names;
        for (const SourceFile& f : a)
            names.push_back(f.name);
        for (const SourceFile& f : b)
            if (named(a, f.name) == nullptr)
                names.push_back(f.name);
        std::vector<FileDiff> out;
        for (const std::string& name : names)
        {
            const SourceFile *fa = named(a, name), *fb = named(b, name);
            FileDiff d;
            d.name = name;
            const auto whole = [&](const SourceFile& f, Line::Kind kind) {
                int n = 0;
                for (const std::string_view l : textdiff::lines(f.text))
                {
                    ++n;
                    if (static_cast<int>(d.lines.size()) < kMaxDiffRows)
                        d.lines.push_back({ kind, kind == Line::Kind::removed ? n : 0, kind == Line::Kind::added ? n : 0, shown(l) });
                }
                (kind == Line::Kind::added ? d.added : d.removed) = n;
                if (n > kMaxDiffRows)
                    d.lines.push_back({ Line::Kind::gap, 0, 0, "... and " + std::to_string(n - kMaxDiffRows) + " more lines" });
            };
            if (fa != nullptr && fb != nullptr && fa->text == fb->text)
                d.change = Change::same;
            else if (fb == nullptr)
            {
                d.change = Change::removed;
                whole(*fa, Line::Kind::removed);
            }
            else if (fa == nullptr)
            {
                d.change = Change::added;
                whole(*fb, Line::Kind::added);
            }
            else
            {
                d.change = Change::changed;
                const std::vector<std::string_view> la = textdiff::lines(fa->text), lb = textdiff::lines(fb->text);
                const std::vector<textdiff::Edit> script = textdiff::diff(la, lb, nullptr, 4000);
                const std::size_t n = script.size();
                // A netlist's drawing lines (fminus/CirDoc.h): layout, not the program. A run of them that changed is one
                // row, and counts in neither +N nor -N.
                const bool netlist = isCircuitFile(name);
                const auto layout = [&](std::size_t i) {
                    const textdiff::Edit& e = script[i];
                    return netlist && e.op != textdiff::Op::same
                        && cir::isLayoutLine(e.op == textdiff::Op::added ? lb[e.b] : la[e.a]);
                };
                std::vector<bool> near(n, false);
                for (std::size_t i = 0; i < n; ++i)
                    if (script[i].op != textdiff::Op::same)
                    {
                        const std::size_t from = i >= static_cast<std::size_t>(kContext) ? i - kContext : 0;
                        for (std::size_t j = from; j < std::min(n, i + kContext + 1); ++j)
                            near[j] = true;
                        if (layout(i))
                            continue;
                        if (script[i].op == textdiff::Op::added)
                            ++d.added;
                        else
                            ++d.removed;
                    }
                bool cut = false;
                for (std::size_t i = 0; i < n && !cut; ++i)
                {
                    if (static_cast<int>(d.lines.size()) >= kMaxDiffRows)
                    {
                        d.lines.push_back({ Line::Kind::gap, 0, 0, "... the rest is cut here" });
                        cut = true;
                        break;
                    }
                    if (!near[i])
                    {
                        std::size_t run = 0;
                        while (i + run < n && !near[i + run])
                            ++run;
                        d.lines.push_back({ Line::Kind::gap, 0, 0,
                                            std::to_string(run) + (run == 1 ? " line" : " lines") + " the same" });
                        i += run - 1;
                        continue;
                    }
                    if (layout(i))
                    {
                        std::size_t run = 0;
                        while (i + run < n && layout(i + run))
                            ++run;
                        const textdiff::Edit& e = script[i];
                        d.lines.push_back({ Line::Kind::drawing, e.op == textdiff::Op::added ? 0 : static_cast<int>(e.a) + 1,
                                            e.op == textdiff::Op::removed ? 0 : static_cast<int>(e.b) + 1,
                                            "the drawing changed (" + std::to_string(run) + (run == 1 ? " line)" : " lines)") });
                        ++d.drawing;
                        i += run - 1;
                        continue;
                    }
                    const textdiff::Edit& e = script[i];
                    Line l;
                    l.kind = e.op == textdiff::Op::same ? Line::Kind::same : e.op == textdiff::Op::added ? Line::Kind::added : Line::Kind::removed;
                    l.a = e.op == textdiff::Op::added ? 0 : static_cast<int>(e.a) + 1;
                    l.b = e.op == textdiff::Op::removed ? 0 : static_cast<int>(e.b) + 1;
                    l.text = shown(e.op == textdiff::Op::added ? lb[e.b] : la[e.a]);
                    d.lines.push_back(std::move(l));
                }
            }
            out.push_back(std::move(d));
        }
        return out;
    }

    DiffView::FileDiff DiffView::dialsDiff(const std::vector<ContentDial>& a, const std::vector<ContentDial>& b,
                                           const std::string& left, const std::string& right)
    {
        FileDiff d;
        d.name = "Dials";
        d.section = Section::dials;
        const auto named = [](const std::vector<ContentDial>& v, const std::string& name) -> const ContentDial* {
            for (const ContentDial& x : v)
                if (x.name == name)
                    return &x;
            return nullptr;
        };
        std::vector<std::string> names;
        for (const ContentDial& x : a)
            names.push_back(x.name);
        for (const ContentDial& x : b)
            if (named(a, x.name) == nullptr)
                names.push_back(x.name);
        for (const std::string& name : names)
        {
            const ContentDial *da = named(a, name), *db = named(b, name);
            const std::string n = shown(name);
            if (da != nullptr && db != nullptr)
            {
                if (da->value == db->value && da->label == db->label)
                    continue;
                d.lines.push_back({ Line::Kind::changed, 0, 0, n + ":  " + shown(da->text) + "  ->  " + shown(db->text) });
                ++d.added;
            }
            else if (da != nullptr)
            {
                d.lines.push_back({ Line::Kind::removed, 0, 0, n + ":  " + shown(da->text) + "  (only in " + left + ")" });
                ++d.removed;
            }
            else
            {
                d.lines.push_back({ Line::Kind::added, 0, 0, n + ":  " + shown(db->text) + "  (only in " + right + ")" });
                ++d.added;
            }
        }
        d.change = d.lines.empty() ? Change::same : Change::changed;
        return d;
    }

    DiffView::FileDiff DiffView::samplesDiff(const std::vector<ContentSample>& a, const std::vector<ContentSample>& b)
    {
        FileDiff d;
        d.name = "Samples";
        d.section = Section::samples;
        std::map<std::string, std::string> ha, hb;
        for (const ContentSample& s : a)
            ha[s.name] = s.hash;
        for (const ContentSample& s : b)
            hb[s.name] = s.hash;
        for (const ContentSample& s : a)
            if (const auto it = hb.find(s.name); it == hb.end())
            {
                d.lines.push_back({ Line::Kind::removed, 0, 0, shown(s.name) + "  removed" });
                ++d.removed;
            }
            else if (it->second != s.hash)
            {
                d.lines.push_back({ Line::Kind::changed, 0, 0, shown(s.name) + "  replaced" });
                ++d.added;
            }
        for (const ContentSample& s : b)
            if (ha.find(s.name) == ha.end())
            {
                d.lines.push_back({ Line::Kind::added, 0, 0, shown(s.name) + "  added" });
                ++d.added;
            }
        d.change = d.lines.empty() ? Change::same : Change::changed;
        return d;
    }

    std::string DiffView::tabLabel(const FileDiff& f)
    {
        if (f.section != Section::file)
            return std::string(f.section == Section::dials ? "DIALS " : "SAMPLES ") + std::to_string(f.lines.size());
        switch (f.change)
        {
            case Change::same: return f.name;
            case Change::added: return f.name + " new";
            case Change::removed: return f.name + " gone";
            case Change::changed: break;
        }
        if (f.added == 0 && f.removed == 0 && f.drawing > 0)
            return f.name + " drawing";
        return f.name + " +" + std::to_string(f.added) + " -" + std::to_string(f.removed);
    }

    // ---- state ---------------------------------------------------------------------------------------------------------
    void DiffView::set(std::vector<FileDiff> diffs, bool keep)
    {
        const std::string was = keep && file_ >= 0 && file_ < static_cast<int>(diffs_.size()) ? diffs_[static_cast<std::size_t>(file_)].name : std::string();
        diffs_ = std::move(diffs);
        file_ = 0;
        if (keep)
        {
            for (std::size_t i = 0; i < diffs_.size(); ++i)
                if (diffs_[i].name == was)
                    file_ = static_cast<int>(i);
            firstTab_ = std::min(firstTab_, file_);
        }
        else
        {
            firstTab_ = 0;
            for (std::size_t i = 0; i < diffs_.size(); ++i)
                if (diffs_[i].change != Change::same)
                {
                    file_ = static_cast<int>(i);
                    break;
                }
        }
        diff_.offset = 0.0f;
        ++revision_;
    }

    void DiffView::setSides(std::string left, std::string right)
    {
        left_ = std::move(left);
        right_ = std::move(right);
        ++revision_;
    }

    void DiffView::showFile(int i)
    {
        if (diffs_.empty())
            return;
        file_ = std::clamp(i, 0, static_cast<int>(diffs_.size()) - 1);
        firstTab_ = std::min(firstTab_, file_);
        diff_.offset = 0.0f;
        ++revision_;
    }

    // ---- places --------------------------------------------------------------------------------------------------------
    DiffView::Places DiffView::places(const funkgui::Rect& area) const
    {
        Places at;
        const float top = area.y;
        const float buttons = changeKeys_ ? 2.0f * kChangeW + 12.0f : 0.0f;
        const float tabsW = area.w - buttons;
        float x = area.x;
        for (int i = std::max(0, firstTab_); i < static_cast<int>(diffs_.size()); ++i)
        {
            const float tw = tabW(tabLabel(diffs_[static_cast<std::size_t>(i)]));
            if (x + tw > area.x + tabsW + 0.5f && !at.tabs.empty())
                break;
            at.tabs.push_back({ x, top, std::min(tw, tabsW), kTabH });
            x += tw + 6.0f;
        }
        if (changeKeys_)
        {
            at.next = { area.right() - kChangeW, top, kChangeW, kTabH };
            at.previous = { at.next.x - 4.0f - kChangeW, top, kChangeW, kTabH };
        }
        at.diff = { area.x, top + kTabH + 8.0f, area.w, area.bottom() - (top + kTabH + 8.0f) };
        at.lines = std::max(1, static_cast<int>(std::floor((at.diff.h - 12.0f) / at.lineH)));
        return at;
    }

    Scroll DiffView::diffScroll(const Places& at) const
    {
        Scroll s = diff_;
        const int n = file_ >= 0 && file_ < static_cast<int>(diffs_.size()) ? static_cast<int>(diffs_[static_cast<std::size_t>(file_)].lines.size()) : 0;
        s.content = static_cast<float>(n) * at.lineH;
        s.view = static_cast<float>(at.lines) * at.lineH;
        s.row = at.lineH;
        s.clamp();
        return s;
    }

    funkgui::Rect DiffView::diffBar(const Places& at) const
    {
        return { at.diff.x, at.diff.y + 4.0f, at.diff.w - 3.0f, at.diff.h - 8.0f };
    }

    int DiffView::scroll() const noexcept { return static_cast<int>(std::ceil(diff_.offset / Places{}.lineH - 1.0e-3f)); }

    funkgui::Point DiffView::centreOfTab(int i, const funkgui::Rect& area) const
    {
        const Places at = places(area);
        const int k = i - firstTab_;
        if (k < 0 || k >= static_cast<int>(at.tabs.size()))
            return { -1.0e4f, -1.0e4f };
        const funkgui::Rect& r = at.tabs[static_cast<std::size_t>(k)];
        return { r.centreX(), r.centreY() };
    }

    // ---- the changes ---------------------------------------------------------------------------------------------------
    std::vector<int> DiffView::runStarts(int file) const
    {
        std::vector<int> out;
        if (file < 0 || file >= static_cast<int>(diffs_.size()))
            return out;
        const std::vector<Line>& lines = diffs_[static_cast<std::size_t>(file)].lines;
        for (std::size_t i = 0; i < lines.size(); ++i)
            if (changedRow(lines[i]) && (i == 0 || !changedRow(lines[i - 1])))
                out.push_back(static_cast<int>(i));
        return out;
    }

    void DiffView::reveal(int row, const funkgui::Rect& area)
    {
        const Places at = places(area);
        Scroll s = diffScroll(at);
        s.moveTo(static_cast<float>(std::max(0, row - 1)) * at.lineH);   // a line of context above it
        diff_ = s;
        ++revision_;
    }

    bool DiffView::step(int dir, const funkgui::Rect& area)
    {
        if (diffs_.empty())
            return false;
        const int top = scroll();
        // In the pill shown: the first run past the row the last step put at the top (one row of context above it).
        const std::vector<int> here = runStarts(file_);
        if (dir > 0)
        {
            for (const int r : here)
                if (std::max(0, r - 1) > top)
                {
                    reveal(r, area);
                    return true;
                }
        }
        else
            for (auto it = here.rbegin(); it != here.rend(); ++it)
                if (std::max(0, *it - 1) < top)
                {
                    reveal(*it, area);
                    return true;
                }
        // On into the pills after (before) it.
        const int n = static_cast<int>(diffs_.size());
        for (int k = 1; k < n; ++k)
        {
            const int f = file_ + dir * k;
            if (f < 0 || f >= n)
                break;
            const std::vector<int> runs = runStarts(f);
            if (runs.empty())
                continue;
            showFile(f);
            while (firstTab_ < file_ && file_ >= firstTab_ + static_cast<int>(places(area).tabs.size()))
                ++firstTab_;
            reveal(dir > 0 ? runs.front() : runs.back(), area);
            return true;
        }
        return false;
    }

    // ---- events --------------------------------------------------------------------------------------------------------
    bool DiffView::pointerDown(funkgui::Point p, const funkgui::Rect& area)
    {
        const Places at = places(area);
        if (Scroll s = diffScroll(at); s.press(p, diffBar(at)))
        {
            diff_ = s;
            ++revision_;
            return true;
        }
        for (std::size_t k = 0; k < at.tabs.size(); ++k)
            if (at.tabs[k].contains(p))
            {
                showFile(firstTab_ + static_cast<int>(k));
                return true;
            }
        if (changeKeys_ && at.previous.contains(p))
        {
            (void)step(-1, area);
            return true;
        }
        if (changeKeys_ && at.next.contains(p))
        {
            (void)step(1, area);
            return true;
        }
        return at.diff.contains(p);
    }

    bool DiffView::pointerDrag(funkgui::Point p, const funkgui::Rect& area)
    {
        if (!diff_.dragging())
            return false;
        const Places at = places(area);
        Scroll s = diffScroll(at);
        s.drag(p, diffBar(at));
        diff_ = s;
        ++revision_;
        return true;
    }

    bool DiffView::pointerUp()
    {
        if (!diff_.dragging())
            return false;
        diff_.release();
        ++revision_;
        return true;
    }

    bool DiffView::wheel(const funkgui::WheelEvent& e, const funkgui::Rect& area)
    {
        // As the code scrolls (Scroll.h): the delta as it came, WheelEvent::reversed never read.
        const Places at = places(area);
        if (!at.diff.contains({ e.x, e.y }))
            return false;
        Scroll s = diffScroll(at);
        s.wheel(e.dy, e.smooth);
        diff_ = s;
        ++revision_;
        return true;
    }

    bool DiffView::key(const funkgui::KeyEvent& e, const funkgui::Rect& area)
    {
        using funkgui::Key;
        const Places at = places(area);
        if (e.key == Key::tab && !diffs_.empty())
        {
            const int n = static_cast<int>(diffs_.size());
            showFile((file_ + (e.mods.shift ? n - 1 : 1)) % n);
            while (firstTab_ < file_ && file_ >= firstTab_ + static_cast<int>(places(area).tabs.size()))
                ++firstTab_;                             // the pill shown in the row
            return true;
        }
        if (e.key == Key::pageUp || e.key == Key::pageDown)
        {
            Scroll s = diffScroll(at);
            s.moveTo(s.offset + static_cast<float>((e.key == Key::pageUp ? -1 : 1) * std::max(1, at.lines - 2)) * at.lineH);
            diff_ = s;
            ++revision_;
            return true;
        }
        if (e.key == Key::home || e.key == Key::end)
        {
            Scroll s = diffScroll(at);
            s.moveTo(e.key == Key::home ? 0.0f : s.max());
            diff_ = s;
            ++revision_;
            return true;
        }
        if (changeKeys_ && e.key == Key::character && !e.mods.cmd && !e.mods.ctrl && !e.mods.alt && (e.ch == ']' || e.ch == '['))
        {
            (void)step(e.ch == ']' ? 1 : -1, area);
            return true;
        }
        return false;
    }

    bool DiffView::hand(funkgui::Point p, const funkgui::Rect& area) const
    {
        const Places at = places(area);
        for (const funkgui::Rect& r : at.tabs)
            if (r.contains(p))
                return true;
        return changeKeys_ && (at.previous.contains(p) || at.next.contains(p));
    }

    // ---- drawing -------------------------------------------------------------------------------------------------------
    void DiffView::draw(funkgui::Canvas& c, const funkgui::Theme& th, const funkgui::Rect& area, funkgui::Point pointer,
                        bool hasPointer) const
    {
        const chat::Palette p = chat::palette(th);
        const Places at = places(area);
        const funkgui::Rect& d = at.diff;
        for (std::size_t k = 0; k < at.tabs.size(); ++k)
        {
            const int i = firstTab_ + static_cast<int>(k);
            const FileDiff& f = diffs_[static_cast<std::size_t>(i)];
            const funkgui::Rect& t = at.tabs[k];
            const bool chosen = i == file_;
            const bool hover = hasPointer && t.contains(pointer);
            c.rrect(t.x, t.y, t.w, t.h, t.h * 0.5f, chosen ? funkgui::mix(p.composer, p.accent, 0.2f) : hover ? p.hover : p.composer,
                    1.0f, chosen ? p.accent : p.composerEdge);
            const funkgui::Col ink = f.change == Change::same ? p.faint : chosen ? p.text : p.soft;
            c.text(fitText(c, tabLabel(f), chat::kSmall, t.w - 20.0f).c_str(), t.centreX(), c.capCentreTop(t.centreY(), chat::kSmall),
                   chat::kSmall, ink, funkgui::Align::centre);
        }
        const float tabsRight = changeKeys_ ? at.previous.x - 8.0f : d.right();
        if (firstTab_ + static_cast<int>(at.tabs.size()) < static_cast<int>(diffs_.size()))
            c.text("...", tabsRight - 4.0f, c.capCentreTop(area.y + 0.5f * kTabH, chat::kSmall), chat::kSmall, p.faint,
                   funkgui::Align::right);
        if (changeKeys_)
            for (const bool next : { false, true })
            {
                const funkgui::Rect& r = next ? at.next : at.previous;
                const bool hover = hasPointer && r.contains(pointer);
                c.rrect(r.x, r.y, r.w, r.h, r.h * 0.5f, hover ? p.hover : p.composer, 1.0f, p.composerEdge);
                chat::drawIcon(c, next ? chat::Icon::chevronDown : chat::Icon::chevronUp, r.centreX(), r.centreY(), 12.0f,
                               hover ? p.text : p.soft);
            }
        c.rrect(d.x, d.y, d.w, d.h, 10.0f, p.code, 1.0f, p.codeEdge);
        if (file_ < 0 || file_ >= static_cast<int>(diffs_.size()))
            return;
        const FileDiff& f = diffs_[static_cast<std::size_t>(file_)];
        if (f.change == Change::same)
        {
            const std::string what = f.section == Section::dials     ? "The dials are the same in " + left_ + " and " + right_ + "."
                                   : f.section == Section::samples   ? "The samples are the same in " + left_ + " and " + right_ + "."
                                                                     : f.name + " is the same in " + left_ + " and " + right_ + ".";
            c.text(fitText(c, what, chat::kBody, d.w - 24.0f).c_str(), d.centreX(), c.capCentreTop(d.y + 28.0f, chat::kBody),
                   chat::kBody, p.faint, funkgui::Align::centre);
        }
        const funkgui::Canvas::ClipScope clip(c, { c.snapX(d.x + 1.0f), c.snapY(d.y + 1.0f), d.w - 2.0f, d.h - 2.0f });
        const float cw = std::max(1.0f, c.textWidth("M", chat::kCodeText));
        const bool numbered = f.section == Section::file;
        const float textX = numbered ? d.x + 96.0f : d.x + 28.0f;
        const auto maxChars = static_cast<std::size_t>(std::max(0.0f, (d.right() - 12.0f - textX) / cw));
        // The lines in view, a line at an edge in part (the clip above).
        const Scroll scroll = diffScroll(at);
        const int from = std::max(0, static_cast<int>(std::floor(scroll.offset / at.lineH)));
        const int to = std::min(static_cast<int>(f.lines.size()), static_cast<int>(std::ceil((scroll.offset + scroll.view) / at.lineH)) + 1);
        for (int i = from; i < to; ++i)
        {
            const Line& l = f.lines[static_cast<std::size_t>(i)];
            const float y = d.y + 6.0f + static_cast<float>(i) * at.lineH - scroll.offset, mid = y + 0.5f * at.lineH;
            if (l.kind == Line::Kind::gap || l.kind == Line::Kind::drawing)
            {
                const bool drawing = l.kind == Line::Kind::drawing;
                if (drawing)
                    c.rrect(d.x + 2.0f, y, d.w - 4.0f, at.lineH, 0.0f, p.accent.withAlpha(0.07f));
                c.hairlineH(d.x + 8.0f, mid, d.w - 16.0f, drawing ? p.accent.withAlpha(0.5f) : p.codeEdge);
                const float tw = c.textWidth(l.text.c_str(), chat::kSmall) + 16.0f;
                c.rrect(d.centreX() - 0.5f * tw, mid - 7.0f, tw, 14.0f, 7.0f, p.code, drawing ? 1.0f : 0.0f, p.accent.withAlpha(0.5f));
                c.text(l.text.c_str(), d.centreX(), c.capCentreTop(mid, chat::kSmall), chat::kSmall, drawing ? p.soft : p.faint,
                       funkgui::Align::centre);
                continue;
            }
            if (l.kind == Line::Kind::changed)
                c.rrect(d.x + 2.0f, y, d.w - 4.0f, at.lineH, 0.0f, p.accent.withAlpha(0.08f));
            else if (l.kind != Line::Kind::same)
                c.rrect(d.x + 2.0f, y, d.w - 4.0f, at.lineH, 0.0f, (l.kind == Line::Kind::removed ? p.danger : p.accent).withAlpha(0.14f));
            if (l.a > 0)
                c.text(std::to_string(l.a).c_str(), d.x + 40.0f, c.capCentreTop(mid, chat::kSmall), chat::kSmall, p.faint,
                       funkgui::Align::right);
            if (l.b > 0)
                c.text(std::to_string(l.b).c_str(), d.x + 74.0f, c.capCentreTop(mid, chat::kSmall), chat::kSmall, p.faint,
                       funkgui::Align::right);
            const float markX = numbered ? d.x + 84.0f : d.x + 14.0f;
            if (l.kind == Line::Kind::removed || l.kind == Line::Kind::added)
                c.text(l.kind == Line::Kind::removed ? "-" : "+", markX, c.capCentreTop(mid, chat::kCodeText), chat::kCodeText,
                       l.kind == Line::Kind::removed ? p.danger : p.accent);
            else if (l.kind == Line::Kind::changed)
                c.text("~", markX, c.capCentreTop(mid, chat::kCodeText), chat::kCodeText, p.accent);
            const std::string text = l.text.size() > maxChars ? l.text.substr(0, maxChars > 3 ? maxChars - 3 : 0) + "..." : l.text;
            c.text(text.c_str(), textX, c.capCentreTop(mid, chat::kCodeText), chat::kCodeText,
                   l.kind == Line::Kind::removed ? p.soft : p.text);
        }
        scroll.draw(c, diffBar(at), th);
    }

    // ---- accessibility -------------------------------------------------------------------------------------------------
    void DiffView::accessibility(std::vector<funkgui::A11yItem>& out, const funkgui::Rect& area, uint32_t parent,
                                 uint32_t tabBase, uint32_t diffId, uint32_t buttonBase) const
    {
        const Places at = places(area);
        for (std::size_t k = 0; k < at.tabs.size(); ++k)
        {
            const int i = firstTab_ + static_cast<int>(k);
            funkgui::A11yItem t;
            t.id = tabBase + static_cast<uint32_t>(i);
            t.parent = parent;
            t.role = funkgui::A11yRole::toggleButton;
            t.bounds = at.tabs[k];
            t.title = tabLabel(diffs_[static_cast<std::size_t>(i)]);
            t.checkable = true;
            t.checked = i == file_;
            out.push_back(std::move(t));
        }
        if (changeKeys_ && buttonBase != 0)
            for (const bool next : { false, true })
            {
                funkgui::A11yItem b;
                b.id = buttonBase + (next ? 1u : 0u);
                b.parent = parent;
                b.role = funkgui::A11yRole::button;
                b.bounds = next ? at.next : at.previous;
                b.title = next ? "Next change" : "Previous change";
                out.push_back(std::move(b));
            }
        funkgui::A11yItem diff;
        diff.id = diffId;
        diff.parent = parent;
        diff.role = funkgui::A11yRole::staticText;
        diff.bounds = at.diff;
        if (file_ >= 0 && file_ < static_cast<int>(diffs_.size()))
        {
            const FileDiff& f = diffs_[static_cast<std::size_t>(file_)];
            diff.title = "Changes in " + f.name;
            if (f.section != Section::file)
            {
                for (const Line& l : f.lines)
                    diff.value += (diff.value.empty() ? "" : "; ") + l.text;
                if (diff.value.empty())
                    diff.value = "the same in " + left_ + " and " + right_;
            }
            else
                diff.value = f.change == Change::same    ? "the same in " + left_ + " and " + right_
                           : f.change == Change::added   ? "new in " + right_ + ", " + std::to_string(f.added) + " lines"
                           : f.change == Change::removed ? "gone in " + right_ + ", " + std::to_string(f.removed) + " lines"
                                                         : std::to_string(f.added) + " lines added, " + std::to_string(f.removed) + " taken away"
                                                               + (f.drawing > 0 ? ", the drawing changed" : "");
        }
        out.push_back(std::move(diff));
    }

    bool DiffView::a11yAction(uint32_t id, const funkgui::Rect& area, uint32_t tabBase, uint32_t buttonBase)
    {
        if (id >= tabBase && id < tabBase + 0x100u)
        {
            if (static_cast<int>(id - tabBase) >= static_cast<int>(diffs_.size()))
                return false;
            showFile(static_cast<int>(id - tabBase));
            return true;
        }
        if (changeKeys_ && buttonBase != 0 && (id == buttonBase || id == buttonBase + 1u))
        {
            (void)step(id == buttonBase ? -1 : 1, area);
            return true;
        }
        return false;
    }
}
