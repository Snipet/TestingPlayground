// Source/editor/DiffCard.cpp: see DiffCard.h.
#include "editor/DiffCard.h"

#include "editor/ChatStyle.h"
#include "editor/Dropdown.h"
#include "editor/Items.h"
#include "editor/Theme.h"

#include <algorithm>
#include <cmath>

namespace playground::ui
{
    namespace
    {
        constexpr float kBoxW = 900.0f, kBoxH = 640.0f;
        constexpr float kPad = 24.0f;
        constexpr float kSideW = 250.0f, kSideH = 30.0f;
        constexpr float kSwapW = 40.0f;
        constexpr float kButtonH = 32.0f;
        constexpr int kChoice = 100;                     // a dropdown's row ids: kChoice + its row (0: Editor)
        constexpr uint32_t kBoxId = DiffCard::kA11yBase, kLeftId = kBoxId + 1, kRightId = kBoxId + 2, kSwapId = kBoxId + 3,
                           kSummaryId = kBoxId + 4, kDiffId = kBoxId + 5, kOpenLeftId = kBoxId + 6, kOpenRightId = kBoxId + 7,
                           kAbId = kBoxId + 8, kCloseId = kBoxId + 9, kNoteLeftId = kBoxId + 10, kNoteRightId = kBoxId + 11,
                           kChangeId = kBoxId + 0x20,     // + 0 previous, 1 next
                           kTabId = kBoxId + 0x100;       // + the pill

        std::string shortWhen(const std::string& when) { return when.size() > 5 ? when.substr(5) : when; }   // "10-27 14:02"

        std::string plural(std::size_t n, const char* one, const char* many)
        {
            return std::to_string(n) + " " + (n == 1 ? one : many);
        }
    }

    // ---- opening, reading ----------------------------------------------------------------------------------------------
    bool DiffCard::open(const std::string& project, int left, int right)
    {
        why_.clear();
        if (project.empty())
            return false;
        project_ = project;
        const auto side = [](int v) { return v == kTheirs ? v : std::max(0, v); };   // package 05: their side kept
        sides_ = { side(left), side(right) };
        content_ = {};
        ask_ = -1;
        open_ = true;
        notice_.clear();
        std::get<0>(ab_).clear();
        readVersions();
        readSide(0);
        readSide(1);
        draftSeen_ = port_.draftRevision();
        view_.setChangeKeys(true);
        view_.set({}, false);
        remake();
        ++revision_;
        return true;
    }

    void DiffCard::close()
    {
        open_ = false;
        ask_ = -1;
        ++revision_;
    }

    void DiffCard::readVersions()
    {
        readSeen_ = port_.projectsRead();
        const ProjectDetail d = port_.projectPolled(project_);   // the store's cache: no file read whole here
        versions_ = d.versions;
    }

    void DiffCard::readSide(int k)
    {
        const int v = sides_[static_cast<std::size_t>(k)];
        content_[static_cast<std::size_t>(k)] = v == kEditor   ? port_.editorContent()
                                              : v == kTheirs ? port_.gitTheirs(project_)   // package 05: from memory
                                                             : port_.versionPolled(project_, v);
    }

    bool DiffCard::reading() const { return !content_[0].read || !content_[1].read; }

    std::string DiffCard::sideLabel(int k) const
    {
        const int v = sides_[static_cast<std::size_t>(k)];
        return v == kEditor ? std::string("Editor") : v == kTheirs ? std::string("Theirs") : "v" + std::to_string(v);
    }

    void DiffCard::remake()
    {
        if (reading())
        {
            summary_.clear();
            ++revision_;
            return;
        }
        for (int k = 0; k < 2; ++k)
            if (content_[static_cast<std::size_t>(k)].missing)
            {
                summary_ = "Version " + std::to_string(sides_[static_cast<std::size_t>(k)]) + " is not kept any more.";
                view_.set({}, false);
                ++revision_;
                return;
            }
        const std::string a = sideLabel(0), b = sideLabel(1);
        std::vector<DiffView::FileDiff> diffs = DiffView::diffOf(content_[0].files, content_[1].files);
        std::size_t files = 0;
        for (const DiffView::FileDiff& f : diffs)
            files += f.change != DiffView::Change::same ? 1 : 0;
        DiffView::FileDiff dials = DiffView::dialsDiff(content_[0].dials, content_[1].dials, a, b);
        DiffView::FileDiff samples = DiffView::samplesDiff(content_[0].samples, content_[1].samples);
        const std::size_t nd = dials.lines.size(), ns = samples.lines.size();
        if (nd > 0)
            diffs.push_back(std::move(dials));
        if (ns > 0)
            diffs.push_back(std::move(samples));
        std::string s;
        const auto add = [&](std::size_t n, const char* one, const char* many) {
            if (n > 0)
                s += (s.empty() ? "" : ", ") + plural(n, one, many);
        };
        add(files, "file", "files");
        add(nd, "dial", "dials");
        add(ns, "sample", "samples");
        summary_ = s.empty() ? std::string("The same") : s;
        view_.setSides(a, b);
        view_.set(std::move(diffs), true);
        // The pill kept is the same on these sides: the first that changed instead.
        if (const auto& fs = view_.files(); !fs.empty() && fs[static_cast<std::size_t>(view_.file())].change == DiffView::Change::same)
            for (std::size_t i = 0; i < fs.size(); ++i)
                if (fs[i].change != DiffView::Change::same)
                {
                    view_.showFile(static_cast<int>(i));
                    break;
                }
        ++revision_;
    }

    void DiffCard::tick()
    {
        if (!open_)
            return;
        bool again = false;
        if (const uint64_t r = port_.projectsRead(); r != readSeen_)
        {
            const std::vector<VersionEntry> before = versions_;
            readVersions();
            for (int k = 0; k < 2; ++k)
                if (sides_[static_cast<std::size_t>(k)] != kEditor && sides_[static_cast<std::size_t>(k)] != kTheirs)
                {
                    const VersionContent was = content_[static_cast<std::size_t>(k)];
                    readSide(k);
                    const VersionContent& now = content_[static_cast<std::size_t>(k)];
                    again = again || was.read != now.read || was.missing != now.missing || !(was.files == now.files)
                         || was.dials != now.dials || was.samples != now.samples;
                }
            if (versions_.size() != before.size())
                ++revision_;
        }
        if (const uint64_t d = port_.draftRevision(); d != draftSeen_)
        {
            draftSeen_ = d;
            for (int k = 0; k < 2; ++k)
                if (sides_[static_cast<std::size_t>(k)] == kEditor)
                {
                    readSide(k);
                    again = true;
                }
        }
        if (again)
            remake();
    }

    void DiffCard::setSide(int side, int version)
    {
        if (side < 0 || side > 1)
            return;
        sides_[static_cast<std::size_t>(side)] = std::max(0, version);
        ask_ = -1;
        why_.clear();
        readSide(side);
        remake();
    }

    void DiffCard::swap()
    {
        std::swap(sides_[0], sides_[1]);
        std::swap(content_[0], content_[1]);
        ask_ = -1;
        why_.clear();
        remake();
    }

    std::tuple<std::string, int, int> DiffCard::takeAb()
    {
        std::tuple<std::string, int, int> a{ "", 0, 0 };
        a.swap(ab_);
        return a;
    }

    std::string DiffCard::takeNotice()
    {
        std::string n;
        n.swap(notice_);
        return n;
    }

    std::vector<std::string> DiffCard::choices() const
    {
        std::vector<std::string> out{ "Editor" };
        for (const VersionEntry& v : versions_)
            out.push_back("v" + std::to_string(v.number) + "  " + shortWhen(v.when) + (v.note.empty() ? "" : "  " + asciiText(v.note)));
        return out;
    }

    void DiffCard::openSide(int k)
    {
        const int v = sides_[static_cast<std::size_t>(k)];
        if (v == kEditor)
            return;
        if (port_.project().changed && ask_ != k)
        {
            ask_ = k;
            why_ = "The editor's files have changes no version holds: Open anyway replaces them.";
            ++revision_;
            return;
        }
        std::string why;
        if (!port_.openProject(project_, v, why))
        {
            ask_ = -1;
            why_ = "Not opened: " + why + ".";
            ++revision_;
            return;
        }
        notice_ = isFactoryName(project_) ? "OPENED: " + upper(ownName(project_)) + " (FACTORY, READ-ONLY: A SAVE MAKES YOUR OWN COPY)"
                                          : "OPENED: " + upper(project_) + " V" + std::to_string(v);
        close();
    }

    // ---- places --------------------------------------------------------------------------------------------------------
    DiffCard::Places DiffCard::places(float width, float height) const
    {
        Places at;
        const float w = std::min(kBoxW, width - 32.0f), h = std::min(kBoxH, height - 32.0f);
        at.box = { std::round((width - w) * 0.5f), std::round((height - h) * 0.45f), w, h };
        const funkgui::Rect& box = at.box;
        const float sideW = std::min(kSideW, std::floor((w - 2.0f * kPad - kSwapW - 24.0f) * 0.5f));
        const float sy = box.y + 76.0f;
        at.left = { box.x + kPad, sy, sideW, kSideH };
        at.swap = { at.left.right() + 12.0f, sy, kSwapW, kSideH };
        at.right = { at.swap.right() + 12.0f, sy, sideW, kSideH };
        const float by = box.bottom() - kPad - kButtonH;
        const float viewTop = sy + kSideH + 68.0f;
        at.view = { box.x + kPad, viewTop, w - 2.0f * kPad, std::max(80.0f, by - 30.0f - viewTop) };
        at.close = { box.right() - kPad - 84.0f, by, 84.0f, kButtonH };
        at.ab = { at.close.x - 8.0f - 76.0f, by, 76.0f, kButtonH };
        at.openLeft = { box.x + kPad, by, 128.0f, kButtonH };
        at.openRight = { at.openLeft.right() + 8.0f, by, 128.0f, kButtonH };
        return at;
    }

    // ---- events --------------------------------------------------------------------------------------------------------
    void DiffCard::showMenu(int side, funkgui::HostServices* host, float width, float height)
    {
        if (host == nullptr || side < 0 || side > 1)
            return;
        const Places at = places(width, height);
        funkgui::MenuRequest request;
        const std::vector<std::string> rows = choices();
        int current = 0;
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            const int version = i == 0 ? kEditor : versions_[i - 1].number;
            const bool on = version == sides_[static_cast<std::size_t>(side)];
            if (i == 1)
                request.items.push_back(dropdown::header("Versions"));
            std::string label = rows[i];
            if (label.size() > 64)
                label = label.substr(0, 61) + "...";
            request.items.push_back(dropdown::choice(kChoice + static_cast<int>(i), label, on));
            if (on)
                current = kChoice + static_cast<int>(i);
        }
        dropdown::dropdownMenu(request, side == 0 ? at.left : at.right, current);
        request.theme = productTheme(funkgui::Theme::byIndex(host->themeIndex()));
        const std::vector<VersionEntry> versions = versions_;
        host->showMenu(request, [this, side, versions](int id) {
            if (!open_ || id < kChoice)
                return;
            const int row = id - kChoice;
            if (row > static_cast<int>(versions.size()))
                return;
            setSide(side, row == 0 ? kEditor : versions[static_cast<std::size_t>(row - 1)].number);
            if (answered_)
                answered_();
        });
    }

    void DiffCard::pointerDown(funkgui::Point p, funkgui::HostServices* host, float width, float height)
    {
        const Places at = places(width, height);
        if (!at.box.contains(p))
        {
            close();                                     // a click outside the card
            return;
        }
        if (at.left.contains(p) || at.right.contains(p))
            showMenu(at.left.contains(p) ? 0 : 1, host, width, height);
        else if (at.swap.contains(p))
            swap();
        else if (at.close.contains(p))
            close();
        else if (at.ab.contains(p) && !theirsShown())
        {
            ab_ = { project_, sides_[0], sides_[1] };
            close();
        }
        else if (at.openLeft.contains(p) && sides_[0] != kEditor && sides_[0] != kTheirs)
            openSide(0);
        else if (at.openRight.contains(p) && sides_[1] != kEditor && sides_[1] != kTheirs)
            openSide(1);
        else if (!reading())
            (void)view_.pointerDown(p, at.view);
        ++revision_;
    }

    void DiffCard::pointerDrag(funkgui::Point p, float width, float height) { (void)view_.pointerDrag(p, places(width, height).view); }

    void DiffCard::pointerUp() { (void)view_.pointerUp(); }

    void DiffCard::wheel(const funkgui::WheelEvent& e, float width, float height)
    {
        if (!reading())
            (void)view_.wheel(e, places(width, height).view);
    }

    void DiffCard::key(const funkgui::KeyEvent& e, funkgui::HostServices* host, float width, float height)
    {
        (void)host;
        if (e.key == funkgui::Key::escape)
        {
            close();
            return;
        }
        if (!reading())
            (void)view_.key(e, places(width, height).view);   // ] [, Tab, Page Up and Down, Home and End
    }

    funkgui::Cursor DiffCard::cursor(funkgui::Point p, float width, float height) const
    {
        const Places at = places(width, height);
        for (const funkgui::Rect& r : { at.left, at.right, at.swap, at.ab, at.close })
            if (r.contains(p))
                return funkgui::Cursor::pointingHand;
        if ((at.openLeft.contains(p) && sides_[0] != kEditor) || (at.openRight.contains(p) && sides_[1] != kEditor))
            return funkgui::Cursor::pointingHand;
        if (!reading() && view_.hand(p, at.view))
            return funkgui::Cursor::pointingHand;
        return funkgui::Cursor::normal;
    }

    // ---- drawing -------------------------------------------------------------------------------------------------------
    void DiffCard::draw(funkgui::Canvas& c, const funkgui::Theme& th, float width, float height, funkgui::Point pointer,
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

        // The title: Compare, of the project.
        const float x = box.x + kPad;
        c.rrect(x, box.y + 22.0f, 30.0f, 30.0f, 9.0f, p.accent.withAlpha(0.16f));
        chat::drawIcon(c, chat::Icon::code, x + 15.0f, box.y + 37.0f, 15.0f, p.accent);
        c.text("Compare", x + 42.0f, c.capCentreTop(box.y + 37.0f, chat::kH2), chat::kH2, p.text);
        const float titleW = c.textWidth("Compare", chat::kH2);
        c.text(fitText(c, "versions of " + asciiText(ownName(project_)), chat::kBody, box.w * 0.5f).c_str(), x + 52.0f + titleW,
               c.capCentreTop(box.y + 37.0f, chat::kBody), chat::kBody, p.soft);

        // The sides: two dropdowns, the swap between them, and under each its time and note.
        for (int k = 0; k < 2; ++k)
        {
            const funkgui::Rect& r = k == 0 ? at.left : at.right;
            const dropdown::State st{ true, hasPointer && r.contains(pointer), menuOpen_ && menuOpen_(r), false };
            const funkgui::Col ink = dropdown::drawTrigger(c, p, r, dropdown::Base::field, st);
            const int v = sides_[static_cast<std::size_t>(k)];
            std::string label = sideLabel(k);
            for (const VersionEntry& e : versions_)
                if (e.number == v)
                    label += "   " + shortWhen(e.when);
            c.text(fitText(c, label, chat::kStrong, dropdown::labelRoom(r, r.x + 14.0f)).c_str(), r.x + 14.0f,
                   c.capCentreTop(r.centreY(), chat::kStrong), chat::kStrong, ink);
            const VersionContent& vc = content_[static_cast<std::size_t>(k)];
            std::string when, note;
            if (v == kEditor)
            {
                when = "Now";
                note = "The editor's files, dials and samples as they are";
            }
            else
            {
                for (const VersionEntry& e : versions_)
                    if (e.number == v)
                    {
                        when = "Saved " + e.when;
                        note = e.note;
                    }
                if (when.empty() && vc.read && !vc.missing)
                {
                    when = "Saved " + vc.when;
                    note = vc.note;
                }
            }
            const float noteW = k == 0 ? at.swap.x - 12.0f - r.x : box.right() - kPad - r.x;
            c.text(fitText(c, when, chat::kSmall, noteW).c_str(), r.x + 2.0f, c.capCentreTop(r.bottom() + 16.0f, chat::kSmall),
                   chat::kSmall, p.faint);
            c.text(fitText(c, asciiText(note), chat::kSmall, noteW).c_str(), r.x + 2.0f, c.capCentreTop(r.bottom() + 33.0f, chat::kSmall),
                   chat::kSmall, p.soft);
        }
        {
            const funkgui::Rect& r = at.swap;
            const bool hover = hasPointer && r.contains(pointer);
            c.rrect(r.x, r.y, r.w, r.h, r.h * 0.5f, hover ? p.hover : p.composer, 1.0f, p.composerEdge);
            chat::drawIcon(c, chat::Icon::chevronLeft, r.centreX() - 6.0f, r.centreY(), 10.0f, hover ? p.text : p.soft);
            chat::drawIcon(c, chat::Icon::chevronRight, r.centreX() + 6.0f, r.centreY(), 10.0f, hover ? p.text : p.soft);
        }

        // What differs, and the view (or what is being read).
        const float lineY = at.view.y - 16.0f;
        c.text(summary_.empty() ? "" : summary_.c_str(), x, c.capCentreTop(lineY, chat::kSmallStrong), chat::kSmallStrong,
               summary_ == "The same" ? p.faint : p.text);
        if (reading())
        {
            const funkgui::Rect& d = at.view;
            c.rrect(d.x, d.y + DiffView::kTabH + 8.0f, d.w, d.h - DiffView::kTabH - 8.0f, 10.0f, p.code, 1.0f, p.codeEdge);
            const int k = !content_[0].read ? 0 : 1;
            const std::string what = "Reading version " + std::to_string(sides_[static_cast<std::size_t>(k)]) + "...";
            c.text(what.c_str(), d.centreX(), c.capCentreTop(d.y + DiffView::kTabH + 44.0f, chat::kBody), chat::kBody, p.faint,
                   funkgui::Align::centre);
        }
        else if (!content_[0].missing && !content_[1].missing)
            view_.draw(c, th, at.view, pointer, hasPointer);

        // The line at the foot, and the buttons.
        const std::string foot = !why_.empty() ? why_
                               : "] and [ the next and the previous change; Tab the next file; the arrow between the sides swaps them.";
        c.text(fitText(c, foot, chat::kSmall, box.w - 2.0f * kPad).c_str(), x, c.capCentreTop(at.close.y - 14.0f, chat::kSmall),
               chat::kSmall, why_.empty() ? p.faint : p.danger);
        const auto button = [&](const funkgui::Rect& r, const std::string& label, bool primary, bool enabled) {
            const bool hover = enabled && hasPointer && r.contains(pointer);
            funkgui::Col fill = p.card.withAlpha(0.0f), ink = !enabled ? p.faint : hover ? p.text : p.soft;
            if (primary && enabled)
            {
                fill = hover ? funkgui::mix(p.accent, p.text, 0.12f) : p.accent;
                ink = p.onAccent;
            }
            else if (hover)
                fill = p.hover;
            c.rrect(r.x, r.y, r.w, r.h, r.h * 0.5f, fill, primary && enabled ? 0.0f : 1.0f, p.composerEdge);
            const funkgui::TextStyle& st = primary ? chat::kSmallStrong : chat::kSmall;
            c.text(fitText(c, label, st, r.w - 16.0f).c_str(), r.centreX(), c.capCentreTop(r.centreY(), st), st, ink, funkgui::Align::centre);
        };
        for (int k = 0; k < 2; ++k)
        {
            const bool editor = sides_[static_cast<std::size_t>(k)] == kEditor || sides_[static_cast<std::size_t>(k)] == kTheirs;
            button(k == 0 ? at.openLeft : at.openRight, askingOpen(k) ? std::string("Open anyway") : editor ? std::string("Open") : "Open " + sideLabel(k),
                   askingOpen(k), !editor);
        }
        button(at.ab, "A/B...", false, !theirsShown());   // their side (package 05): from memory, nothing plays
        button(at.close, "Close", false, true);
    }

    // ---- accessibility -------------------------------------------------------------------------------------------------
    void DiffCard::accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const
    {
        const Places at = places(width, height);
        funkgui::A11yItem box;
        box.id = kBoxId;
        box.role = funkgui::a11yrole::group;
        box.bounds = at.box;
        box.title = "Compare";
        box.description = "Versions of " + ownName(project_);
        out.push_back(box);
        for (int k = 0; k < 2; ++k)
        {
            funkgui::A11yItem it;
            it.id = k == 0 ? kLeftId : kRightId;
            it.parent = kBoxId;
            it.role = funkgui::A11yRole::comboBox;
            it.bounds = k == 0 ? at.left : at.right;
            it.title = k == 0 ? "Left side" : "Right side";
            it.value = sideLabel(k);
            out.push_back(std::move(it));
            funkgui::A11yItem note;
            note.id = k == 0 ? kNoteLeftId : kNoteRightId;
            note.parent = kBoxId;
            note.role = funkgui::A11yRole::staticText;
            const funkgui::Rect& r = k == 0 ? at.left : at.right;
            note.bounds = { r.x, r.bottom() + 6.0f, r.w, 36.0f };
            note.title = sideLabel(k);
            const int v = sides_[static_cast<std::size_t>(k)];
            if (v == kEditor)
                note.value = "the editor's files, dials and samples as they are now";
            else if (v == kTheirs)
                note.value = "the other side, read from the remote's commit: " + content_[static_cast<std::size_t>(k)].note;
            else
                for (const VersionEntry& e : versions_)
                    if (e.number == v)
                        note.value = "saved " + e.when + (e.note.empty() ? "" : ", " + e.note);
            out.push_back(std::move(note));
        }
        const auto button = [&](uint32_t id, const funkgui::Rect& r, const std::string& title, bool enabled = true) {
            funkgui::A11yItem b;
            b.id = id;
            b.parent = kBoxId;
            b.role = funkgui::A11yRole::button;
            b.bounds = r;
            b.title = title;
            b.enabled = enabled;
            out.push_back(std::move(b));
        };
        button(kSwapId, at.swap, "Swap the sides");
        funkgui::A11yItem sum;
        sum.id = kSummaryId;
        sum.parent = kBoxId;
        sum.role = funkgui::A11yRole::staticText;
        sum.bounds = { at.view.x, at.view.y - 26.0f, at.view.w, 20.0f };
        sum.title = "What differs";
        sum.value = reading() ? "Reading..." : summary_;
        out.push_back(std::move(sum));
        if (!reading())
            view_.accessibility(out, at.view, kBoxId, kTabId, kDiffId, kChangeId);
        for (int k = 0; k < 2; ++k)
        {
            const bool editor = sides_[static_cast<std::size_t>(k)] == kEditor || sides_[static_cast<std::size_t>(k)] == kTheirs;
            button(k == 0 ? kOpenLeftId : kOpenRightId, k == 0 ? at.openLeft : at.openRight,
                   askingOpen(k) ? std::string("Open anyway") : "Open " + sideLabel(k), !editor);
        }
        button(kAbId, at.ab, "A/B", !theirsShown());
        button(kCloseId, at.close, "Close");
    }

    bool DiffCard::a11yAction(uint32_t id, funkgui::A11yAction a, funkgui::HostServices* host, float width, float height)
    {
        if (a != funkgui::A11yAction::press && a != funkgui::A11yAction::toggle && a != funkgui::A11yAction::focus
            && a != funkgui::A11yAction::showMenu)
            return false;
        const Places at = places(width, height);
        const auto press = [&](const funkgui::Rect& r) { pointerDown({ r.centreX(), r.centreY() }, host, width, height); };
        if (id == kLeftId || id == kRightId)
            showMenu(id == kLeftId ? 0 : 1, host, width, height);
        else if (id == kSwapId)
            swap();
        else if (id == kOpenLeftId)
            press(at.openLeft);
        else if (id == kOpenRightId)
            press(at.openRight);
        else if (id == kAbId)
            press(at.ab);
        else if (id == kCloseId)
            close();
        else
            return !reading() && view_.a11yAction(id, at.view, kTabId, kChangeId);
        ++revision_;
        return true;
    }
}
