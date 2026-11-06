// Source/editor/AbCard.cpp: see AbCard.h.
#include "editor/AbCard.h"

#include "editor/ChatStyle.h"
#include "editor/Items.h"

#include "dsp/TextDiff.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace playground::ui
{
    namespace
    {
        constexpr float kBoxW = 980.0f, kBoxH = 620.0f;
        constexpr float kPad = 24.0f;
        constexpr float kListTop = 100.0f;               // below the title, its line and the lists' captions
        constexpr float kGap = 16.0f;                    // between the versions and the changes
        constexpr float kTabH = 26.0f;
        constexpr float kPlayH = 40.0f;                  // A and B
        constexpr float kPlayW = 140.0f;
        constexpr float kButtonH = 32.0f;
        constexpr float kChip = 22.0f;                   // a row's A and B chips
        constexpr uint32_t kBoxId = AbCard::kA11yBase, kListId = kBoxId + 1, kDiffId = kBoxId + 2, kPlayAId = kBoxId + 3,
                           kPlayBId = kBoxId + 4, kSameId = kBoxId + 5, kOwnId = kBoxId + 6, kMatchId = kBoxId + 7,
                           kCloseId = kBoxId + 8, kKeepAId = kBoxId + 9, kKeepBId = kBoxId + 10, kLoudId = kBoxId + 11,
                           kTabId = kBoxId + 0x100,       // + the file
                           kRowId = kBoxId + 0x1000;      // + the row * 4 + 0 (its A chip), 1 (its B chip), 2 (the row)

        const char* letter(int side) { return side == 0 ? "A" : "B"; }

        std::string fixed(const char* format, double v)
        {
            char buf[48];
            std::snprintf(buf, sizeof buf, format, v);
            return buf;
        }

        std::string shortWhen(const std::string& when) { return when.size() > 5 ? when.substr(5) : when; }   // "10-27 14:02"

        // A line as the card shows it: ASCII, a tab as four spaces, no carriage return.
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
    }

    // ---- the diff ------------------------------------------------------------------------------------------------------
    std::vector<AbCard::FileDiff> AbCard::diffOf(const Sources& a, const Sources& b)
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
                std::vector<bool> near(n, false);
                for (std::size_t i = 0; i < n; ++i)
                    if (script[i].op != textdiff::Op::same)
                    {
                        const std::size_t from = i >= static_cast<std::size_t>(kContext) ? i - kContext : 0;
                        for (std::size_t j = from; j < std::min(n, i + kContext + 1); ++j)
                            near[j] = true;
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

    std::string AbCard::tabLabel(const FileDiff& f)
    {
        switch (f.change)
        {
            case Change::same: return f.name;
            case Change::added: return f.name + " new";
            case Change::removed: return f.name + " gone";
            case Change::changed: break;
        }
        return f.name + " +" + std::to_string(f.added) + " -" + std::to_string(f.removed);
    }

    // ---- opening, following ------------------------------------------------------------------------------------------
    bool AbCard::open(const std::string& project, int version)
    {
        versions_ = project.empty() ? std::vector<VersionEntry>{} : port_.versions(project);
        const int b = version > 0 ? version : versions_.empty() ? 0 : versions_.front().number;
        std::string why;
        why_.clear();
        if (!port_.startAb(project, 0, b, why))
        {
            why_ = "Not started: " + why + ".";
            ++revision_;
            return false;
        }
        return attach();
    }

    bool AbCard::attach()
    {
        const AbState s = port_.ab();
        if (!s.on)
            return false;
        versions_ = s.project.empty() ? std::vector<VersionEntry>{} : port_.versions(s.project);
        open_ = true;
        why_.clear();
        diffA_.clear();
        diffB_.clear();
        diffs_.clear();
        file_ = firstTab_ = scroll_ = 0;
        firstRow_ = 0;
        follow();
        // The first file that changed shows first.
        for (std::size_t i = 0; i < diffs_.size(); ++i)
            if (diffs_[i].change != Change::same)
            {
                showFile(static_cast<int>(i));
                break;
            }
        if (const int row = rowOfVersion(state_.sides[1].version); row >= 0)
            firstRow_ = std::max(0, row - 3);
        ++revision_;
        return true;
    }

    void AbCard::follow()
    {
        state_ = port_.ab();
        if (!state_.on)
        {
            if (open_ && !state_.ended.empty())
                notice_ = "A/B ENDED: ANOTHER PROGRAM RUNS";
            open_ = false;
            ++revision_;
            return;
        }
        if (!(state_.sides[0].files == diffA_) || !(state_.sides[1].files == diffB_))
        {
            const std::string was = file_ >= 0 && file_ < static_cast<int>(diffs_.size()) ? diffs_[static_cast<std::size_t>(file_)].name : std::string();
            diffA_ = state_.sides[0].files;
            diffB_ = state_.sides[1].files;
            diffs_ = diffOf(diffA_, diffB_);
            file_ = 0;
            for (std::size_t i = 0; i < diffs_.size(); ++i)
                if (diffs_[i].name == was)
                    file_ = static_cast<int>(i);
            firstTab_ = std::min(firstTab_, file_);
            scroll_ = 0;
        }
        ++revision_;
    }

    void AbCard::tick()
    {
        if (open_ && port_.abRevision() != state_.revision)
            follow();
    }

    void AbCard::close()
    {
        if (open_)
        {
            port_.endAb();
            notice_ = "A/B CLOSED: WHAT PLAYED BEFORE PLAYS AGAIN";
        }
        open_ = false;
        state_ = port_.ab();
        ++revision_;
    }

    void AbCard::keep(int side)
    {
        if (!open_ || side < 0 || side > 1)
            return;
        const AbSide kept = state_.sides[static_cast<std::size_t>(side)];
        std::string why;
        if (!port_.keepAb(side, why))
        {
            why_ = "Not kept: " + why + ".";
            ++revision_;
            return;
        }
        notice_ = std::string("KEPT ") + letter(side) + ": " + upper(kept.label);
        open_ = false;
        state_ = port_.ab();
        ++revision_;
    }

    void AbCard::play(int side)
    {
        if (!open_)
            return;
        port_.playAb(side);
        follow();
    }

    std::string AbCard::takeNotice()
    {
        std::string n;
        n.swap(notice_);
        return n;
    }

    int AbCard::versionOfRow(int row) const
    {
        return row <= 0 || row > static_cast<int>(versions_.size()) ? 0 : versions_[static_cast<std::size_t>(row - 1)].number;
    }

    int AbCard::rowOfVersion(int version) const
    {
        if (version <= 0)
            return 0;
        for (std::size_t i = 0; i < versions_.size(); ++i)
            if (versions_[i].number == version)
                return static_cast<int>(i) + 1;
        return -1;
    }

    void AbCard::setSide(int side, int row)
    {
        if (row < 0 || row >= rowCount())
            return;
        std::array<int, 2> v{ state_.sides[0].version, state_.sides[1].version };
        const int want = versionOfRow(row);
        if (v[static_cast<std::size_t>(side)] == want)
            return;
        v[static_cast<std::size_t>(side)] = want;
        std::string why;
        if (!port_.setAbSides(v[0], v[1], why))
            why_ = "Not changed: " + why + ".";
        else
            why_.clear();
        follow();
    }

    void AbCard::showFile(int i)
    {
        if (diffs_.empty())
            return;
        file_ = std::clamp(i, 0, static_cast<int>(diffs_.size()) - 1);
        firstTab_ = std::min(firstTab_, file_);
        scroll_ = 0;
        ++revision_;
    }

    // ---- places --------------------------------------------------------------------------------------------------------
    AbCard::Places AbCard::places(float width, float height) const
    {
        Places at;
        const float w = std::min(kBoxW, width - 32.0f), h = std::min(kBoxH, height - 32.0f);
        at.box = { std::round((width - w) * 0.5f), std::round((height - h) * 0.45f), w, h };
        const funkgui::Rect& box = at.box;
        const float top = box.y + kListTop;
        const float by = box.bottom() - kPad - kButtonH;
        const float cy = by - 16.0f - kPlayH;
        const float listH = std::max(3.0f * at.rowH + 8.0f, cy - 22.0f - top);
        const float leftW = std::round((w - 2.0f * kPad - kGap) * 0.34f);
        at.rows = std::max(1, static_cast<int>(std::floor((listH - 8.0f) / at.rowH)));
        at.list = { box.x + kPad, top, leftW, static_cast<float>(at.rows) * at.rowH + 8.0f };
        const float rightX = at.list.right() + kGap, rightW = box.right() - kPad - rightX;
        float x = rightX;
        for (int i = std::max(0, firstTab_); i < static_cast<int>(diffs_.size()); ++i)
        {
            const float tw = tabW(tabLabel(diffs_[static_cast<std::size_t>(i)]));
            if (x + tw > rightX + rightW + 0.5f && !at.tabs.empty())
                break;
            at.tabs.push_back({ x, top, std::min(tw, rightW), kTabH });
            x += tw + 6.0f;
        }
        at.diff = { rightX, top + kTabH + 8.0f, rightW, at.list.bottom() - (top + kTabH + 8.0f) };
        at.lines = std::max(1, static_cast<int>(std::floor((at.diff.h - 12.0f) / at.lineH)));
        at.playA = { box.x + kPad, cy, kPlayW, kPlayH };
        at.playB = { at.playA.right() + 8.0f, cy, kPlayW, kPlayH };
        at.same = { at.playB.right() + 72.0f, cy + 6.0f, 64.0f, 28.0f };
        at.own = { at.same.right() + 4.0f, cy + 6.0f, 108.0f, 28.0f };
        at.match = { at.own.right() + 24.0f, cy + 6.0f, 146.0f, 28.0f };
        at.keepB = { box.right() - kPad - 96.0f, by, 96.0f, kButtonH };
        at.keepA = { at.keepB.x - 8.0f - 96.0f, by, 96.0f, kButtonH };
        at.close = { at.keepA.x - 8.0f - 84.0f, by, 84.0f, kButtonH };
        return at;
    }

    float AbCard::chipX(const funkgui::Rect& row, int chip) const { return row.x + 6.0f + static_cast<float>(chip) * (kChip + 4.0f); }

    funkgui::Point AbCard::centreOfRow(int row, int chip, float width, float height) const
    {
        const Places at = places(width, height);
        if (row < firstRow_ || row >= firstRow_ + at.rows || row >= rowCount())
            return { -1.0e4f, -1.0e4f };
        const funkgui::Rect rr{ at.list.x + 4.0f, at.list.y + 4.0f + static_cast<float>(row - firstRow_) * at.rowH, at.list.w - 8.0f, at.rowH };
        if (chip == 0 || chip == 1)
            return { chipX(rr, chip) + 0.5f * kChip, rr.centreY() };
        return { rr.x + 0.6f * rr.w, rr.centreY() };
    }

    funkgui::Point AbCard::centreOfTab(int i, float width, float height) const
    {
        const Places at = places(width, height);
        const int k = i - firstTab_;
        if (k < 0 || k >= static_cast<int>(at.tabs.size()))
            return { -1.0e4f, -1.0e4f };
        const funkgui::Rect& r = at.tabs[static_cast<std::size_t>(k)];
        return { r.centreX(), r.centreY() };
    }

    int AbCard::maxScroll(const Places& at) const
    {
        if (file_ < 0 || file_ >= static_cast<int>(diffs_.size()))
            return 0;
        return std::max(0, static_cast<int>(diffs_[static_cast<std::size_t>(file_)].lines.size()) - at.lines);
    }

    // ---- events --------------------------------------------------------------------------------------------------------
    void AbCard::pointerDown(funkgui::Point p, float width, float height)
    {
        const Places at = places(width, height);
        if (at.list.contains(p))
        {
            const int row = firstRow_ + static_cast<int>(std::floor((p.y - at.list.y - 4.0f) / at.rowH));
            if (row >= firstRow_ && row < firstRow_ + at.rows && row < rowCount())
            {
                const funkgui::Rect rr{ at.list.x + 4.0f, 0.0f, at.list.w - 8.0f, at.rowH };
                const bool chipA = p.x >= chipX(rr, 0) - 2.0f && p.x < chipX(rr, 0) + kChip + 2.0f;
                setSide(chipA ? 0 : 1, row);
            }
            return;
        }
        for (std::size_t k = 0; k < at.tabs.size(); ++k)
            if (at.tabs[k].contains(p))
            {
                showFile(firstTab_ + static_cast<int>(k));
                return;
            }
        if (at.playA.contains(p))
            play(0);
        else if (at.playB.contains(p))
            play(1);
        else if (at.same.contains(p) || at.own.contains(p))
        {
            port_.setAbOwnDials(at.own.contains(p));
            follow();
        }
        else if (at.match.contains(p))
        {
            port_.setAbMatch(!state_.match);
            follow();
        }
        else if (at.close.contains(p))
            close();
        else if (at.keepA.contains(p))
            keep(0);
        else if (at.keepB.contains(p))
            keep(1);
    }

    void AbCard::wheel(const funkgui::WheelEvent& e, float width, float height)
    {
        const Places at = places(width, height);
        const float dy = e.reversed ? -e.dy : e.dy;
        const int step = dy > 0.0f ? -1 : (dy < 0.0f ? 1 : 0);
        const int by = step * std::max(1, static_cast<int>(std::round(std::fabs(dy) * 6.0f)));
        if (at.list.contains({ e.x, e.y }))
            firstRow_ = std::clamp(firstRow_ + by, 0, std::max(0, rowCount() - at.rows));
        else if (at.diff.contains({ e.x, e.y }))
            scroll_ = std::clamp(scroll_ + 3 * by, 0, maxScroll(at));
        else
            return;
        ++revision_;
    }

    void AbCard::key(const funkgui::KeyEvent& e, float width, float height)
    {
        using funkgui::Key;
        const Places at = places(width, height);
        if (e.key == Key::escape)
            close();
        else if (e.key == Key::enter)
        {
            if (state_.playing >= 0 && state_.pending < 0)
                keep(state_.playing);
        }
        else if (e.key == Key::space)
            play(state_.pending >= 0 ? 1 - state_.pending : state_.playing == 0 ? 1 : 0);
        else if (e.key == Key::character && !e.mods.cmd && !e.mods.ctrl && (e.ch == 'a' || e.ch == 'A'))
            play(0);
        else if (e.key == Key::character && !e.mods.cmd && !e.mods.ctrl && (e.ch == 'b' || e.ch == 'B'))
            play(1);
        else if (e.key == Key::up || e.key == Key::down)
        {
            const int side = e.mods.shift ? 0 : 1;
            const int row = rowOfVersion(state_.sides[static_cast<std::size_t>(side)].version);
            const int next = std::clamp((row < 0 ? 0 : row) + (e.key == Key::up ? -1 : 1), 0, rowCount() - 1);
            if (next < firstRow_)
                firstRow_ = next;
            if (next >= firstRow_ + at.rows)
                firstRow_ = next - at.rows + 1;
            setSide(side, next);
        }
        else if (e.key == Key::tab && !diffs_.empty())
        {
            const int n = static_cast<int>(diffs_.size());
            showFile((file_ + (e.mods.shift ? n - 1 : 1)) % n);
            while (firstTab_ < file_ && file_ >= firstTab_ + static_cast<int>(places(width, height).tabs.size()))
                ++firstTab_;                             // the tab shown in the row
        }
        else if (e.key == Key::pageUp || e.key == Key::pageDown)
        {
            scroll_ = std::clamp(scroll_ + (e.key == Key::pageUp ? -1 : 1) * std::max(1, at.lines - 2), 0, maxScroll(at));
            ++revision_;
        }
        else if (e.key == Key::home || e.key == Key::end)
        {
            scroll_ = e.key == Key::home ? 0 : maxScroll(at);
            ++revision_;
        }
    }

    funkgui::Cursor AbCard::cursor(funkgui::Point p, float width, float height) const
    {
        const Places at = places(width, height);
        for (const funkgui::Rect& r : { at.playA, at.playB, at.same, at.own, at.match, at.close, at.keepA, at.keepB })
            if (r.contains(p))
                return funkgui::Cursor::pointingHand;
        for (const funkgui::Rect& r : at.tabs)
            if (r.contains(p))
                return funkgui::Cursor::pointingHand;
        if (at.list.contains(p))
        {
            const int row = firstRow_ + static_cast<int>(std::floor((p.y - at.list.y - 4.0f) / at.rowH));
            if (row >= firstRow_ && row < firstRow_ + at.rows && row < rowCount())
                return funkgui::Cursor::pointingHand;
        }
        return funkgui::Cursor::normal;
    }

    // ---- drawing -------------------------------------------------------------------------------------------------------
    void AbCard::draw(funkgui::Canvas& c, const funkgui::Theme& th, float width, float height, funkgui::Point pointer,
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

        // The title: A/B of the project, and which side plays.
        const float x = box.x + kPad;
        c.rrect(x, box.y + 22.0f, 30.0f, 30.0f, 9.0f, p.accent.withAlpha(0.16f));
        chat::drawIcon(c, chat::Icon::wave, x + 15.0f, box.y + 37.0f, 15.0f, p.accent);
        c.text("A/B", x + 42.0f, c.capCentreTop(box.y + 37.0f, chat::kH2), chat::kH2, p.text);
        const float titleW = c.textWidth("A/B", chat::kH2);
        if (!state_.project.empty())
            c.text(fitText(c, "of " + asciiText(state_.project), chat::kBody, box.w * 0.45f).c_str(), x + 52.0f + titleW,
                   c.capCentreTop(box.y + 37.0f, chat::kBody), chat::kBody, p.soft);
        std::string now;
        if (state_.pending >= 0)
            now = std::string(letter(state_.pending)) + " (" + state_.sides[static_cast<std::size_t>(state_.pending)].label + ") on its way";
        else if (state_.playing >= 0)
            now = std::string("Playing ") + letter(state_.playing) + " (" + state_.sides[static_cast<std::size_t>(state_.playing)].label + ")";
        else
            now = "Neither plays yet";
        c.text(now.c_str(), box.right() - kPad, c.capCentreTop(box.y + 37.0f, chat::kSmallStrong), chat::kSmallStrong,
               state_.pending >= 0 ? p.soft : p.accent, funkgui::Align::right);
        c.text(fitText(c, "Two versions in turn while you play, at matched loudness. The editor keeps its files until you "
                          "keep one; Close goes back to what played before.", chat::kSmall, box.w - 2.0f * kPad).c_str(),
               x, c.capCentreTop(box.y + 62.0f, chat::kSmall), chat::kSmall, p.soft);

        // The versions, each row's A and B chips.
        {
            const funkgui::Rect& r = at.list;
            const funkgui::TextStyle& cap = chat::kSmallStrong;
            c.text(fitText(c, state_.project.empty() ? std::string("VERSIONS") : "VERSIONS OF " + upper(asciiText(state_.project)), cap, r.w).c_str(),
                   r.x + 2.0f, c.capCentreTop(r.y - 11.0f, cap), cap, p.faint);
            c.rrect(r.x, r.y, r.w, r.h, 10.0f, p.composer, 1.0f, p.composerEdge);
            const int n = rowCount();
            const bool scrolls = n > at.rows;
            const float rowW = r.w - 8.0f - (scrolls ? 8.0f : 0.0f);
            for (int k = 0; k < at.rows && firstRow_ + k < n; ++k)
            {
                const int row = firstRow_ + k;
                const int version = versionOfRow(row);
                const funkgui::Rect rr{ r.x + 4.0f, r.y + 4.0f + static_cast<float>(k) * at.rowH, rowW, at.rowH };
                const bool isA = state_.sides[0].version == version, isB = state_.sides[1].version == version;
                if (isB)
                    c.rrect(rr.x, rr.y + 1.0f, rr.w, rr.h - 2.0f, 7.0f, funkgui::mix(p.composer, p.accent, 0.14f));
                else if (isA)
                    c.rrect(rr.x, rr.y + 1.0f, rr.w, rr.h - 2.0f, 7.0f, funkgui::mix(p.composer, p.text, 0.07f));
                else if (hasPointer && rr.contains(pointer))
                    c.rrect(rr.x, rr.y + 1.0f, rr.w, rr.h - 2.0f, 7.0f, p.hover);
                for (int chip = 0; chip < 2; ++chip)
                {
                    const bool on = chip == 0 ? isA : isB;
                    const float cx = chipX(rr, chip);
                    const funkgui::Rect cr{ cx, rr.centreY() - 10.0f, kChip, 20.0f };
                    const bool hover = hasPointer && cr.contains(pointer);
                    const funkgui::Col tone = chip == 0 ? p.soft : p.accent;
                    if (on)
                        c.rrect(cr.x, cr.y, cr.w, cr.h, 6.0f, tone);
                    else
                        c.rrect(cr.x, cr.y, cr.w, cr.h, 6.0f, hover ? p.hover : p.composer, 1.0f, hover ? tone : p.composerEdge);
                    c.text(letter(chip), cr.centreX(), c.capCentreTop(cr.centreY(), chat::kSmallStrong), chat::kSmallStrong,
                           on ? p.onAccent : (hover ? tone : p.faint), funkgui::Align::centre);
                }
                const float tx = chipX(rr, 1) + kChip + 12.0f;
                const funkgui::TextStyle& st = isA || isB ? chat::kStrong : chat::kBody;
                if (version == 0)
                {
                    c.text("Current", tx, c.capCentreTop(rr.centreY(), st), st, p.text);
                    c.text(fitText(c, "the editor's files", chat::kSmall, std::max(0.0f, rr.right() - 10.0f - (tx + 70.0f))).c_str(),
                           tx + 70.0f, c.capCentreTop(rr.centreY(), chat::kSmall), chat::kSmall, p.soft);
                    continue;
                }
                const VersionEntry& v = versions_[static_cast<std::size_t>(row - 1)];
                c.text(("v" + std::to_string(v.number)).c_str(), tx, c.capCentreTop(rr.centreY(), st), st, p.text);
                const std::string when = shortWhen(v.when);
                c.text(when.c_str(), tx + 44.0f, c.capCentreTop(rr.centreY(), chat::kSmall), chat::kSmall, p.faint);
                const float noteX = tx + 52.0f + c.textWidth(when.c_str(), chat::kSmall) + 8.0f;
                c.text(fitText(c, asciiText(v.note), chat::kSmall, std::max(0.0f, rr.right() - 10.0f - noteX)).c_str(), noteX,
                       c.capCentreTop(rr.centreY(), chat::kSmall), chat::kSmall, p.soft);
            }
            if (scrolls)
            {
                const float track = r.h - 12.0f;
                const float thumb = std::max(18.0f, track * static_cast<float>(at.rows) / static_cast<float>(n));
                const float y = r.y + 6.0f + (track - thumb) * static_cast<float>(firstRow_) / static_cast<float>(std::max(1, n - at.rows));
                c.rrect(r.right() - 9.0f, y, 4.0f, thumb, 2.0f, p.dim);
            }
        }

        // The changes: a tab a file, the chosen one's diff.
        {
            const funkgui::Rect& d = at.diff;
            const funkgui::TextStyle& cap = chat::kSmallStrong;
            const std::string caption = "CHANGES FROM A (" + upper(state_.sides[0].label) + ") TO B (" + upper(state_.sides[1].label) + ")";
            c.text(fitText(c, caption, cap, d.w).c_str(), d.x + 2.0f, c.capCentreTop(at.list.y - 11.0f, cap), cap, p.faint);
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
            if (firstTab_ + static_cast<int>(at.tabs.size()) < static_cast<int>(diffs_.size()))
                c.text("...", d.right() - 4.0f, c.capCentreTop(at.list.y + 0.5f * kTabH, chat::kSmall), chat::kSmall, p.faint,
                       funkgui::Align::right);
            c.rrect(d.x, d.y, d.w, d.h, 10.0f, p.code, 1.0f, p.codeEdge);
            if (file_ >= 0 && file_ < static_cast<int>(diffs_.size()))
            {
                const FileDiff& f = diffs_[static_cast<std::size_t>(file_)];
                if (f.change == Change::same)
                    c.text(fitText(c, f.name + " is the same in A and B.", chat::kBody, d.w - 24.0f).c_str(), d.centreX(),
                           c.capCentreTop(d.y + 28.0f, chat::kBody), chat::kBody, p.faint, funkgui::Align::centre);
                const funkgui::Canvas::ClipScope clip(c, { c.snapX(d.x + 1.0f), c.snapY(d.y + 1.0f), d.w - 2.0f, d.h - 2.0f });
                const float cw = std::max(1.0f, c.textWidth("M", chat::kCodeText));
                const float textX = d.x + 96.0f;
                const auto maxChars = static_cast<std::size_t>(std::max(0.0f, (d.right() - 12.0f - textX) / cw));
                for (int k = 0; k < at.lines; ++k)
                {
                    const int i = scroll_ + k;
                    if (i >= static_cast<int>(f.lines.size()))
                        break;
                    const Line& l = f.lines[static_cast<std::size_t>(i)];
                    const float y = d.y + 6.0f + static_cast<float>(k) * at.lineH, mid = y + 0.5f * at.lineH;
                    if (l.kind == Line::Kind::gap)
                    {
                        c.hairlineH(d.x + 8.0f, mid, d.w - 16.0f, p.codeEdge);
                        const float tw = c.textWidth(l.text.c_str(), chat::kSmall) + 16.0f;
                        c.rrect(d.centreX() - 0.5f * tw, mid - 7.0f, tw, 14.0f, 7.0f, p.code);
                        c.text(l.text.c_str(), d.centreX(), c.capCentreTop(mid, chat::kSmall), chat::kSmall, p.faint, funkgui::Align::centre);
                        continue;
                    }
                    if (l.kind != Line::Kind::same)
                        c.rrect(d.x + 2.0f, y, d.w - 4.0f, at.lineH, 0.0f,
                                (l.kind == Line::Kind::removed ? p.danger : p.accent).withAlpha(0.14f));
                    if (l.a > 0)
                        c.text(std::to_string(l.a).c_str(), d.x + 40.0f, c.capCentreTop(mid, chat::kSmall), chat::kSmall, p.faint,
                               funkgui::Align::right);
                    if (l.b > 0)
                        c.text(std::to_string(l.b).c_str(), d.x + 74.0f, c.capCentreTop(mid, chat::kSmall), chat::kSmall, p.faint,
                               funkgui::Align::right);
                    if (l.kind != Line::Kind::same)
                        c.text(l.kind == Line::Kind::removed ? "-" : "+", d.x + 84.0f, c.capCentreTop(mid, chat::kCodeText), chat::kCodeText,
                               l.kind == Line::Kind::removed ? p.danger : p.accent);
                    const std::string text = l.text.size() > maxChars ? l.text.substr(0, maxChars > 3 ? maxChars - 3 : 0) + "..." : l.text;
                    c.text(text.c_str(), textX, c.capCentreTop(mid, chat::kCodeText), chat::kCodeText,
                           l.kind == Line::Kind::removed ? p.soft : p.text);
                }
                const int rows = static_cast<int>(f.lines.size());
                if (rows > at.lines)
                {
                    const float track = d.h - 12.0f;
                    const float thumb = std::max(18.0f, track * static_cast<float>(at.lines) / static_cast<float>(rows));
                    const float y = d.y + 6.0f + (track - thumb) * static_cast<float>(scroll_) / static_cast<float>(std::max(1, rows - at.lines));
                    c.rrect(d.right() - 9.0f, y, 4.0f, thumb, 2.0f, p.dim);
                }
            }
        }

        // A and B: the side that plays filled, the one on its way outlined.
        for (int side = 0; side < 2; ++side)
        {
            const funkgui::Rect& r = side == 0 ? at.playA : at.playB;
            const bool plays = state_.playing == side && state_.pending < 0;
            const bool coming = state_.pending == side;
            const bool hover = hasPointer && r.contains(pointer);
            c.rrect(r.x, r.y, r.w, r.h, 12.0f, plays ? p.accent : hover ? p.hover : p.composer, coming ? 2.0f : 1.0f,
                    plays || coming ? p.accent : p.composerEdge);
            const funkgui::Col ink = plays ? p.onAccent : coming ? p.accent : p.text;
            c.text(letter(side), r.x + 18.0f, c.capCentreTop(r.centreY(), chat::kH1), chat::kH1, ink);
            const std::string label = state_.sides[static_cast<std::size_t>(side)].label + (coming ? "..." : "");
            c.text(fitText(c, label, chat::kSmall, r.w - 52.0f).c_str(), r.x + 40.0f, c.capCentreTop(r.centreY(), chat::kSmall),
                   chat::kSmall, plays ? p.onAccent : p.soft);
            if (plays)
                chat::drawIcon(c, chat::Icon::play, r.right() - 18.0f, r.centreY(), 12.0f, p.onAccent);
        }

        // The dials: the same for both, or each its own.
        c.text("DIALS", at.same.x - 10.0f, c.capCentreTop(at.same.centreY(), chat::kSmallStrong), chat::kSmallStrong, p.faint,
               funkgui::Align::right);
        for (const bool own : { false, true })
        {
            const funkgui::Rect& r = own ? at.own : at.same;
            const bool on = state_.ownDials == own;
            const bool hover = hasPointer && r.contains(pointer);
            c.rrect(r.x, r.y, r.w, r.h, 9.0f, on ? funkgui::mix(p.composer, p.accent, 0.2f) : hover ? p.hover : p.composer, 1.0f,
                    on ? p.accent : p.composerEdge);
            c.text(own ? "Each its own" : "Same", r.centreX(), c.capCentreTop(r.centreY(), chat::kSmall), chat::kSmall,
                   on ? p.text : p.soft, funkgui::Align::centre);
        }

        // Match loudness, and what it found.
        {
            const funkgui::Rect& r = at.match;
            const bool hover = hasPointer && r.contains(pointer);
            c.rrect(r.x, r.y, r.w, r.h, 9.0f, hover ? p.hover : p.composer, 1.0f, state_.match ? p.accent : p.composerEdge);
            const funkgui::Rect box2{ r.x + 9.0f, r.centreY() - 7.0f, 14.0f, 14.0f };
            c.rrect(box2.x, box2.y, box2.w, box2.h, 4.0f, state_.match ? p.accent : p.composer, 1.0f, state_.match ? p.accent : p.dim);
            if (state_.match)
                chat::drawIcon(c, chat::Icon::check, box2.centreX(), box2.centreY(), 11.0f, p.onAccent);
            c.text("Match loudness", r.x + 31.0f, c.capCentreTop(r.centreY(), chat::kSmall), chat::kSmall, state_.match ? p.text : p.soft);
            std::string line;
            funkgui::Col ink = p.soft;
            if (!state_.match)
                line = "Off: each side at its own level";
            else if (state_.measuring)
                line = "Measuring the loudness...";
            else if (!state_.unmatched.empty())
            {
                line = state_.unmatched;
                ink = p.danger;
            }
            else if (std::isfinite(state_.lufs[0]) && std::isfinite(state_.lufs[1]))
            {
                const int down = state_.trimDb[0] < state_.trimDb[1] ? 0 : 1;
                const float by = -state_.trimDb[static_cast<std::size_t>(down)];
                line = (by > 0.0f ? std::string(letter(down)) + " turned down " + fixed("%.1f dB", by) : std::string("As loud as each other"))
                     + "   A " + fixed("%.1f", state_.lufs[0]) + ", B " + fixed("%.1f", state_.lufs[1]) + " LUFS";
            }
            c.text(fitText(c, line, chat::kSmall, std::max(0.0f, box.right() - kPad - (r.right() + 12.0f))).c_str(), r.right() + 12.0f,
                   c.capCentreTop(r.centreY(), chat::kSmall), chat::kSmall, ink);
        }

        // The line at the foot, and the buttons.
        const std::string foot = !why_.empty() ? why_ : state_.why;
        const float footW = std::max(0.0f, at.close.x - 12.0f - x);
        if (!foot.empty())
            c.text(fitText(c, foot, chat::kSmall, footW).c_str(), x, c.capCentreTop(at.close.centreY(), chat::kSmall), chat::kSmall, p.danger);
        else
            c.text(fitText(c, "A and B (or Space) switch; Up and Down choose B, Shift A; Return keeps the side that plays.", chat::kSmall, footW).c_str(),
                   x, c.capCentreTop(at.close.centreY(), chat::kSmall), chat::kSmall, p.faint);
        const auto button = [&](const funkgui::Rect& r, const std::string& label, bool primary) {
            const bool hover = hasPointer && r.contains(pointer);
            funkgui::Col fill = p.card.withAlpha(0.0f), ink = hover ? p.text : p.soft;
            if (primary)
            {
                fill = hover ? funkgui::mix(p.accent, p.text, 0.12f) : p.accent;
                ink = p.onAccent;
            }
            else if (hover)
                fill = p.hover;
            c.rrect(r.x, r.y, r.w, r.h, r.h * 0.5f, fill, primary ? 0.0f : 1.0f, p.composerEdge);
            const funkgui::TextStyle& st = primary ? chat::kSmallStrong : chat::kSmall;
            c.text(label.c_str(), r.centreX(), c.capCentreTop(r.centreY(), st), st, ink, funkgui::Align::centre);
        };
        button(at.close, "Close", false);
        button(at.keepA, "Keep A", state_.playing == 0 && state_.pending < 0);
        button(at.keepB, "Keep B", state_.playing != 0 || state_.pending >= 0);
    }

    // ---- accessibility -------------------------------------------------------------------------------------------------
    void AbCard::accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const
    {
        const Places at = places(width, height);
        funkgui::A11yItem box;
        box.id = kBoxId;
        box.role = funkgui::a11yrole::group;
        box.bounds = at.box;
        box.title = "A/B";
        box.description = state_.project.empty() ? std::string("The editor's files") : "Versions of " + state_.project;
        out.push_back(box);
        funkgui::A11yItem list;
        list.id = kListId;
        list.parent = kBoxId;
        list.role = funkgui::a11yrole::list;
        list.bounds = at.list;
        list.title = "Versions";
        list.value = std::to_string(rowCount());
        out.push_back(list);
        for (int k = 0; k < at.rows && firstRow_ + k < rowCount(); ++k)
        {
            const int row = firstRow_ + k;
            const int version = versionOfRow(row);
            const funkgui::Rect rr{ at.list.x + 4.0f, at.list.y + 4.0f + static_cast<float>(k) * at.rowH, at.list.w - 8.0f, at.rowH };
            const std::string name = version == 0 ? std::string("Current") : "Version " + std::to_string(version);
            funkgui::A11yItem it;
            it.id = kRowId + static_cast<uint32_t>(row) * 4u + 2u;
            it.parent = kListId;
            it.role = funkgui::A11yRole::listItem;
            it.bounds = rr;
            it.title = name;
            const bool isA = state_.sides[0].version == version, isB = state_.sides[1].version == version;
            it.value = std::string(isA && isB ? "A and B" : isA ? "A" : isB ? "B" : "")
                     + (version == 0 ? std::string(isA || isB ? ", " : "") + "the editor's files"
                                     : std::string(isA || isB ? ", " : "") + "saved " + versions_[static_cast<std::size_t>(row - 1)].when
                                           + ", " + versions_[static_cast<std::size_t>(row - 1)].note);
            out.push_back(it);
            for (int chip = 0; chip < 2; ++chip)
            {
                funkgui::A11yItem b;
                b.id = kRowId + static_cast<uint32_t>(row) * 4u + static_cast<uint32_t>(chip);
                b.parent = kListId;
                b.role = funkgui::A11yRole::radioButton;
                b.bounds = { chipX(rr, chip), rr.centreY() - 10.0f, kChip, 20.0f };
                b.title = name + " as " + letter(chip);
                b.checkable = true;
                b.checked = chip == 0 ? isA : isB;
                out.push_back(std::move(b));
            }
        }
        for (std::size_t k = 0; k < at.tabs.size(); ++k)
        {
            const int i = firstTab_ + static_cast<int>(k);
            funkgui::A11yItem t;
            t.id = kTabId + static_cast<uint32_t>(i);
            t.parent = kBoxId;
            t.role = funkgui::A11yRole::toggleButton;
            t.bounds = at.tabs[k];
            t.title = tabLabel(diffs_[static_cast<std::size_t>(i)]);
            t.checkable = true;
            t.checked = i == file_;
            out.push_back(std::move(t));
        }
        funkgui::A11yItem diff;
        diff.id = kDiffId;
        diff.parent = kBoxId;
        diff.role = funkgui::A11yRole::staticText;
        diff.bounds = at.diff;
        if (file_ >= 0 && file_ < static_cast<int>(diffs_.size()))
        {
            const FileDiff& f = diffs_[static_cast<std::size_t>(file_)];
            diff.title = "Changes in " + f.name;
            diff.value = f.change == Change::same    ? "the same in A and B"
                       : f.change == Change::added   ? "new in B, " + std::to_string(f.added) + " lines"
                       : f.change == Change::removed ? "gone in B, " + std::to_string(f.removed) + " lines"
                                                     : std::to_string(f.added) + " lines added, " + std::to_string(f.removed) + " taken away";
        }
        out.push_back(std::move(diff));
        const auto button = [&](uint32_t id, const funkgui::Rect& r, const std::string& title, bool checkable, bool checked,
                                funkgui::A11yRole role = funkgui::A11yRole::button) {
            funkgui::A11yItem b;
            b.id = id;
            b.parent = kBoxId;
            b.role = checkable ? funkgui::A11yRole::toggleButton : role;
            b.bounds = r;
            b.title = title;
            b.checkable = checkable;
            b.checked = checked;
            out.push_back(std::move(b));
        };
        button(kPlayAId, at.playA, "Play A, " + state_.sides[0].label, true, state_.playing == 0 && state_.pending < 0);
        button(kPlayBId, at.playB, "Play B, " + state_.sides[1].label, true, state_.playing == 1 && state_.pending < 0);
        button(kSameId, at.same, "The same dials", true, !state_.ownDials);
        button(kOwnId, at.own, "Each its own dials", true, state_.ownDials);
        button(kMatchId, at.match, "Match loudness", true, state_.match);
        funkgui::A11yItem loud;
        loud.id = kLoudId;
        loud.parent = kBoxId;
        loud.role = funkgui::A11yRole::staticText;
        loud.bounds = { at.match.right() + 12.0f, at.match.y, std::max(0.0f, at.box.right() - kPad - at.match.right() - 12.0f), at.match.h };
        loud.title = "Loudness";
        if (state_.measuring)
            loud.value = "measuring";
        else if (!state_.unmatched.empty())
            loud.value = state_.unmatched;
        else if (std::isfinite(state_.lufs[0]) && std::isfinite(state_.lufs[1]))
            loud.value = "A " + fixed("%.1f", state_.lufs[0]) + " LUFS, B " + fixed("%.1f", state_.lufs[1]) + " LUFS, A turned by "
                       + fixed("%.1f", state_.trimDb[0]) + " dB, B by " + fixed("%.1f", state_.trimDb[1]) + " dB";
        out.push_back(std::move(loud));
        button(kCloseId, at.close, "Close", false, false);
        button(kKeepAId, at.keepA, "Keep A", false, false);
        button(kKeepBId, at.keepB, "Keep B", false, false);
    }

    bool AbCard::a11yAction(uint32_t id, funkgui::A11yAction a, float width, float height)
    {
        if (a != funkgui::A11yAction::press && a != funkgui::A11yAction::toggle && a != funkgui::A11yAction::focus)
            return false;
        const Places at = places(width, height);
        const auto press = [&](const funkgui::Rect& r) { pointerDown({ r.centreX(), r.centreY() }, width, height); };
        if (id == kPlayAId)
            play(0);
        else if (id == kPlayBId)
            play(1);
        else if (id == kSameId)
            press(at.same);
        else if (id == kOwnId)
            press(at.own);
        else if (id == kMatchId)
            press(at.match);
        else if (id == kCloseId)
            close();
        else if (id == kKeepAId)
            keep(0);
        else if (id == kKeepBId)
            keep(1);
        else if (id >= kTabId && id < kTabId + 0x100u)
        {
            if (static_cast<int>(id - kTabId) >= static_cast<int>(diffs_.size()))
                return false;
            showFile(static_cast<int>(id - kTabId));
        }
        else if (id >= kRowId && id < kRowId + 0x8000u)
        {
            const int row = static_cast<int>((id - kRowId) / 4u), part = static_cast<int>((id - kRowId) % 4u);
            if (row >= rowCount() || part > 2)
                return false;
            setSide(part == 0 ? 0 : 1, row);
        }
        else
            return false;
        return true;
    }
}
