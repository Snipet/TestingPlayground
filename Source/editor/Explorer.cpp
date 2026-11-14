// Source/editor/Explorer.cpp: see Explorer.h.
#include "editor/Explorer.h"

#include "editor/ChatItems.h"                        // asciiText

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace playground::ui
{
    using funkgui::Point;
    using funkgui::Rect;

    namespace
    {
        constexpr float kHeaderH = 28.0f;            // a section's header
        constexpr float kRowH = 24.0f;
        constexpr float kHeadingH = 20.0f;           // a library category's caption
        constexpr float kGroupGap = 6.0f;
        constexpr float kPadX = 6.0f;                // the rows' inset from the item's edges
        constexpr float kIconX = 18.0f;              // a row's icon's centre, from its left
        constexpr float kTextX = 32.0f;              // its label
        constexpr float kSide = 22.0f;               // the cross's and the header button's square
        constexpr uint32_t kStride = 512;            // a11y ids a group

        constexpr funkgui::TextStyle kHeaderStyle{ 9.5f, 1.4f, 0.06f, false };   // tracked caps, as the panel's titles
    }

    ExplorerItem::ExplorerItem(uint32_t a11yBase, Hooks hooks) : base_(a11yBase), hooks_(std::move(hooks)) {}

    void ExplorerItem::setBounds(Rect r)
    {
        bounds_ = r;
        layoutList();                                // the rows' width follows the item's
    }

    void ExplorerItem::setContent(std::vector<Group> groups)
    {
        groups_ = std::move(groups);
        layoutList();
    }

    void ExplorerItem::setSectionOpen(Section s, bool open)
    {
        open_[static_cast<std::size_t>(s)] = open;
        layoutList();
    }

    int ExplorerItem::groupOf(Section s) const
    {
        for (int g = 0; g < static_cast<int>(groups_.size()); ++g)
            if (groups_[static_cast<std::size_t>(g)].section == s)
                return g;
        return -1;
    }

    // Every header, its button, row and cross, top to bottom (y from the list's top).
    void ExplorerItem::layoutList()
    {
        hits_.clear();
        rowY_.assign(groups_.size(), 0.0f);
        const float w = bounds_.w - 2.0f * kPadX;
        float y = 4.0f;
        for (int g = 0; g < static_cast<int>(groups_.size()); ++g)
        {
            const Group& grp = groups_[static_cast<std::size_t>(g)];
            if (g > 0)
                y += kGroupGap;
            rowY_[static_cast<std::size_t>(g)] = y;
            hits_.push_back({ Part::header, g, -1, { kPadX, y, w, kHeaderH } });
            if (grp.action != chat::Icon::none)
                hits_.push_back({ Part::action, g, -1, { kPadX + w - kSide - 2.0f, y + 0.5f * (kHeaderH - kSide), kSide, kSide } });
            y += kHeaderH;
            if (!open_[static_cast<std::size_t>(grp.section)])
                continue;
            if (grp.rows.empty() && !grp.empty.empty())
                y += kRowH;                          // the line that says it is empty
            for (int r = 0; r < static_cast<int>(grp.rows.size()); ++r)
            {
                const Row& row = grp.rows[static_cast<std::size_t>(r)];
                const float h = row.heading ? kHeadingH : kRowH;
                hits_.push_back({ Part::row, g, r, { kPadX, y, w, h } });
                if (row.closable)
                    hits_.push_back({ Part::close, g, r, { kPadX + w - kSide - 2.0f, y + 0.5f * (h - kSide), kSide, kSide } });
                y += h;
            }
        }
        contentH_ = y + 8.0f;
        scroll_.content = contentH_;
        scroll_.view = viewHeight();
        scroll_.row = kRowH;
        scroll_.clamp();
        ++revision_;
    }

    void ExplorerItem::scrollTo(float y)
    {
        if (scroll_.moveTo(y))
            ++revision_;
    }

    Rect ExplorerItem::onScreen(const Rect& r) const { return { bounds_.x + r.x, bounds_.y + r.y - scroll_.offset, r.w, r.h }; }

    const ExplorerItem::Hit* ExplorerItem::hitAt(Point p) const
    {
        if (!bounds_.contains(p) || scroll_.hits(p, bounds_))
            return nullptr;                          // the scroll bar's
        // The small buttons first: they sit on their row or header.
        for (const Hit& h : hits_)
            if ((h.part == Part::close || h.part == Part::action) && onScreen(h.r).contains(p))
                return &h;
        for (const Hit& h : hits_)
            if ((h.part == Part::row || h.part == Part::header) && onScreen(h.r).contains(p))
            {
                if (h.part == Part::row)
                    if (const Row& row = groups_[static_cast<std::size_t>(h.group)].rows[static_cast<std::size_t>(h.row)];
                        row.heading && row.spec.empty() && row.fold == 0)
                        return nullptr;                  // a caption: nothing (one with a spec line says it, no more)
                return &h;
            }
        return nullptr;
    }

    void ExplorerItem::act(const Hit& h, bool twice)
    {
        const Group& g = groups_[static_cast<std::size_t>(h.group)];
        switch (h.part)
        {
            case Part::header:
                setSectionOpen(g.section, !open_[static_cast<std::size_t>(g.section)]);
                return;
            case Part::action:
                if (hooks_.action)
                    hooks_.action(g.section);
                return;
            case Part::row:
            {
                const Row row = g.rows[static_cast<std::size_t>(h.row)];   // a copy: the hook may change the content
                if (row.heading && row.fold == 0)
                    return;
                if (row.heading)                         // a group's caption: folds it or opens it, once
                {
                    if (!twice && hooks_.open)
                        hooks_.open(g.section, row);
                    return;
                }
                if (twice && hooks_.rename)
                    hooks_.rename(g.section, row);
                else if (!twice && hooks_.open)
                    hooks_.open(g.section, row);
                return;
            }
            case Part::close:
            {
                const Row row = g.rows[static_cast<std::size_t>(h.row)];
                if (hooks_.close)
                    hooks_.close(g.section, row);
                return;
            }
        }
    }

    // ---- input -----------------------------------------------------------------------------------------------------
    void ExplorerItem::tick(float, bool, bool, Point pointer) { pointer_ = pointer; }

    funkgui::Cursor ExplorerItem::cursorAt(Point p) const
    {
        const Hit* h = hitAt(p);
        if (h == nullptr)
            return funkgui::Cursor::normal;
        if (h->part == Part::row)
            if (const Row& row = groups_[static_cast<std::size_t>(h->group)].rows[static_cast<std::size_t>(h->row)]; row.heading && row.fold == 0)
                return funkgui::Cursor::normal;
        return funkgui::Cursor::pointingHand;
    }

    void ExplorerItem::pointerDown(const funkgui::PointerEvent& e, funkgui::GestureController&, funkgui::HostServices&)
    {
        if (!e.popup && scroll_.press({ e.x, e.y }, bounds_))   // the thumb drags; the track jumps, then drags
        {
            ++revision_;
            return;
        }
        if (const Hit* h = hitAt({ e.x, e.y }))
            act(*h, false);
    }

    void ExplorerItem::pointerDrag(const funkgui::PointerEvent& e, funkgui::GestureController&)
    {
        if (!scroll_.dragging())
            return;
        const float was = scroll_.offset;
        scroll_.drag({ e.x, e.y }, bounds_);
        if (scroll_.offset != was)
            ++revision_;
    }

    void ExplorerItem::pointerUp(const funkgui::PointerEvent&, funkgui::GestureController&) { scroll_.release(); }

    void ExplorerItem::doubleClick(const funkgui::PointerEvent& e, funkgui::GestureController&)
    {
        // The first click of the two has acted already (a file shown); the second renames it.
        if (const Hit* h = hitAt({ e.x, e.y }); h != nullptr && h->part == Part::row)
            act(*h, true);
    }

    bool ExplorerItem::wheel(const funkgui::WheelEvent& e, funkgui::GestureController&, double)
    {
        // As the code scrolls (Scroll.h): the delta as it came, WheelEvent::reversed never read.
        const float was = scroll_.offset;
        if (!scroll_.wheel(e.dy, e.smooth))
            return false;
        if (scroll_.offset != was)
            ++revision_;
        return true;
    }

    // ---- drawing -----------------------------------------------------------------------------------------------------
    void ExplorerItem::draw(funkgui::Canvas& c, const funkgui::Theme& th, bool) const
    {
        if (hidden_)
            return;
        const chat::Palette p = chat::palette(th);
        const funkgui::Canvas::ClipScope clip(c, bounds_);
        const Hit* hover = hitAt(pointer_);
        for (const Hit& h : hits_)
        {
            const Rect r = onScreen(h.r);
            if (r.bottom() < bounds_.y || r.y > bounds_.bottom())
                continue;
            const Group& g = groups_[static_cast<std::size_t>(h.group)];
            const bool hovered = hover == &h;
            switch (h.part)
            {
                case Part::header:
                {
                    const bool open = open_[static_cast<std::size_t>(g.section)];
                    if (hovered)
                        c.rrect(r.x, r.y + 2.0f, r.w, r.h - 4.0f, 6.0f, p.hover);
                    chat::drawIcon(c, open ? chat::Icon::chevronDown : chat::Icon::chevronRight, r.x + 10.0f, r.centreY(), 8.0f,
                                   hovered ? p.text : p.dim);
                    std::string title = g.title;
                    if (g.total >= 0 && g.counted)
                        title += "  " + std::to_string(g.total);
                    else if (!g.rows.empty() && g.counted)
                    {
                        int n = 0;
                        for (const Row& row : g.rows)
                            n += row.heading ? 0 : 1;
                        title += "  " + std::to_string(n);
                    }
                    c.text(title.c_str(), r.x + 22.0f, c.capCentreTop(r.centreY(), kHeaderStyle), kHeaderStyle,
                           hovered ? p.text : p.soft);
                    break;
                }
                case Part::action:
                {
                    if (hovered)
                        c.rrect(r.x, r.y, r.w, r.h, 6.0f, p.raised, 1.0f, p.cardEdge);
                    chat::drawIcon(c, g.action, r.centreX(), r.centreY(), 10.0f, hovered ? p.text : p.dim);
                    break;
                }
                case Part::row:
                {
                    const Row& row = g.rows[static_cast<std::size_t>(h.row)];
                    if (row.heading)
                    {
                        float right = r.w - 30.0f;
                        if (row.fold != 0)
                        {
                            // A group's caption: its chevron, and how many it holds at the right.
                            if (hovered)
                                c.rrect(r.x, r.y + 1.0f, r.w, r.h - 2.0f, 5.0f, p.hover);
                            chat::drawIcon(c, row.fold == 2 ? chat::Icon::chevronRight : chat::Icon::chevronDown, r.x + kTextX - 13.0f,
                                           r.centreY() + 1.0f, 6.0f, hovered ? p.text : p.faint);
                            if (!row.detail.empty())
                            {
                                c.text(asciiText(row.detail).c_str(), r.right() - 8.0f, c.capCentreTop(r.centreY() + 2.0f, funkgui::type::kMicro),
                                       funkgui::type::kMicro, p.faint, funkgui::Align::right);
                                right -= c.textWidth(row.detail.c_str(), funkgui::type::kMicro) + 8.0f;
                            }
                        }
                        c.text(fitText(c, asciiText(row.label), funkgui::type::kMicro, right).c_str(), r.x + kTextX - 6.0f,
                               c.capCentreTop(r.centreY() + 2.0f, funkgui::type::kMicro), funkgui::type::kMicro,
                               row.fold != 0 && hovered ? p.text : p.faint);
                        break;
                    }
                    if (row.active)
                    {
                        c.rrect(r.x, r.y + 1.0f, r.w, r.h - 2.0f, 6.0f, p.raised, 1.0f, p.cardEdge);
                        c.rrect(r.x + 1.0f, r.y + 6.0f, 2.5f, r.h - 12.0f, 1.25f, p.accent);
                    }
                    else if (hovered)
                        c.rrect(r.x, r.y + 1.0f, r.w, r.h - 2.0f, 6.0f, p.hover);
                    chat::drawIcon(c, row.icon, r.x + kIconX, r.centreY(), 11.0f, row.active ? p.accent : p.dim);
                    // At the right: the cross under the pointer, else the tick, the error's dot or the detail.
                    const bool crossShown = row.closable && (hovered || (hover != nullptr && hover->part == Part::close
                                                                         && hover->group == h.group && hover->row == h.row));
                    float right = r.right() - 8.0f;
                    if (crossShown)
                        right -= kSide;
                    else if (row.marked)
                    {
                        chat::drawIcon(c, chat::Icon::check, right - 5.0f, r.centreY(), 10.0f, p.accent);
                        right -= 16.0f;
                    }
                    if (row.error && !crossShown)
                    {
                        c.disc(right - 3.0f, r.centreY(), 3.0f, p.danger);
                        right -= 12.0f;
                    }
                    const float labelX = r.x + kTextX;
                    std::string detail = row.detail.empty() ? std::string() : asciiText(row.detail);
                    const float detailW = detail.empty() ? 0.0f : c.textWidth(detail.c_str(), chat::kSmall);
                    const float labelRoom = right - labelX - (detail.empty() ? 0.0f : 10.0f);
                    const std::string label = asciiText(row.label);
                    // Package 05: a linked project's git mark after its name (its room taken before the label's).
                    const float markW = row.git ? chat::branchMarkWidth(c, row.ahead, row.behind, chat::kSmall) + 6.0f : 0.0f;
                    const float labelW = c.textWidth(label.c_str(), chat::kBody) + markW;
                    // The detail only where the label leaves it room; the label is cut before the detail goes.
                    const bool detailShown = !detail.empty() && labelW + 10.0f + detailW <= right - labelX;
                    const std::string shown =
                        fitText(c, label, chat::kBody, (detailShown ? labelRoom - detailW : right - labelX) - markW);
                    c.text(shown.c_str(), labelX, c.capCentreTop(r.centreY(), chat::kBody), chat::kBody,
                           row.active ? p.text : (hovered ? p.text : p.soft));
                    if (row.git)
                        chat::drawBranchMark(c, labelX + c.textWidth(shown.c_str(), chat::kBody) + 6.0f, r.centreY(), row.ahead,
                                             row.behind, chat::kSmall, p.dim);
                    if (detailShown)
                        c.text(detail.c_str(), right, c.capCentreTop(r.centreY(), chat::kSmall), chat::kSmall, p.faint,
                               funkgui::Align::right);
                    break;
                }
                case Part::close:
                {
                    const bool rowHovered = hovered || (hover != nullptr && hover->part == Part::row && hover->group == h.group
                                                        && hover->row == h.row);
                    if (!rowHovered)
                        break;
                    if (hovered)
                        c.rrect(r.x + 2.0f, r.y + 2.0f, r.w - 4.0f, r.h - 4.0f, 5.0f, p.hover);
                    chat::drawIcon(c, chat::Icon::cross, r.centreX(), r.centreY(), 9.0f, hovered ? p.text : p.dim);
                    break;
                }
            }
        }
        // A section with nothing in it says so.
        for (int g = 0; g < static_cast<int>(groups_.size()); ++g)
        {
            const Group& grp = groups_[static_cast<std::size_t>(g)];
            if (!open_[static_cast<std::size_t>(grp.section)] || !grp.rows.empty() || grp.empty.empty())
                continue;
            const Rect r = onScreen({ kPadX, rowY_[static_cast<std::size_t>(g)] + kHeaderH, bounds_.w - 2.0f * kPadX, kRowH });
            c.text(fitText(c, asciiText(grp.empty), chat::kSmall, r.w - kTextX).c_str(), r.x + kTextX,
                   c.capCentreTop(r.centreY(), chat::kSmall), chat::kSmall, p.faint);
        }
        // The scroll bar while the list is taller than the card.
        scroll_.draw(c, bounds_, th);
    }

    // ---- probes --------------------------------------------------------------------------------------------------------
    int ExplorerItem::rows(Section s) const
    {
        const int g = groupOf(s);
        return g < 0 ? 0 : static_cast<int>(groups_[static_cast<std::size_t>(g)].rows.size());
    }

    const ExplorerItem::Row* ExplorerItem::row(Section s, int i) const
    {
        const int g = groupOf(s);
        if (g < 0 || i < 0 || i >= static_cast<int>(groups_[static_cast<std::size_t>(g)].rows.size()))
            return nullptr;
        return &groups_[static_cast<std::size_t>(g)].rows[static_cast<std::size_t>(i)];
    }

    namespace
    {
        std::optional<Point> visibleCentre(const Rect& r, const Rect& view)
        {
            if (r.y < view.y || r.bottom() > view.bottom())
                return std::nullopt;
            return Point{ r.centreX(), r.centreY() };
        }
    }

    std::optional<Point> ExplorerItem::centreOfRow(Section s, int row) const
    {
        const int g = groupOf(s);
        for (const Hit& h : hits_)
            if (h.part == Part::row && h.group == g && h.row == row)
            {
                const Rect r = onScreen(h.r);
                // Clear of the cross at its right.
                return visibleCentre({ r.x, r.y, r.w - kSide - 8.0f, r.h }, bounds_);
            }
        return std::nullopt;
    }

    std::optional<Point> ExplorerItem::centreOfHeader(Section s) const
    {
        const int g = groupOf(s);
        for (const Hit& h : hits_)
            if (h.part == Part::header && h.group == g)
            {
                const Rect r = onScreen(h.r);
                return visibleCentre({ r.x, r.y, r.w * 0.5f, r.h }, bounds_);
            }
        return std::nullopt;
    }

    std::optional<Point> ExplorerItem::centreOfAction(Section s) const
    {
        const int g = groupOf(s);
        for (const Hit& h : hits_)
            if (h.part == Part::action && h.group == g)
                return visibleCentre(onScreen(h.r), bounds_);
        return std::nullopt;
    }

    std::optional<Point> ExplorerItem::centreOfClose(Section s, int row) const
    {
        const int g = groupOf(s);
        for (const Hit& h : hits_)
            if (h.part == Part::close && h.group == g && h.row == row)
                return visibleCentre(onScreen(h.r), bounds_);
        return std::nullopt;
    }

    // ---- accessibility -------------------------------------------------------------------------------------------------
    // ids: base_ the list; then per group g, base_ + 1 + g * kStride its header, + 1 its button, + 2 + 2r a row, + 3 + 2r
    // its cross.
    void ExplorerItem::accessibility(std::vector<funkgui::A11yItem>& out) const
    {
        if (hidden_)
            return;
        funkgui::A11yItem list;
        list.id = base_;
        list.role = funkgui::a11yrole::list;
        list.bounds = bounds_;
        list.title = "Explorer";
        list.help = "The program's files and its LLVM IR, the presets, the projects, the library and the examples";
        out.push_back(list);
        for (const Hit& h : hits_)
        {
            const Rect r = onScreen(h.r);
            if (r.y < bounds_.y || r.bottom() > bounds_.bottom())
                continue;                            // out of view
            const Group& g = groups_[static_cast<std::size_t>(h.group)];
            funkgui::A11yItem it;
            it.parent = base_;
            it.bounds = r;
            const uint32_t gid = base_ + 1 + static_cast<uint32_t>(h.group) * kStride;
            switch (h.part)
            {
                case Part::header:
                    it.id = gid;
                    it.role = funkgui::A11yRole::toggleButton;
                    it.title = g.title;
                    it.checkable = true;
                    it.checked = open_[static_cast<std::size_t>(g.section)];
                    it.help = "Shows or hides this section";
                    break;
                case Part::action:
                    it.id = gid + 1;
                    it.role = funkgui::A11yRole::button;
                    it.title = g.actionTitle;
                    break;
                case Part::row:
                {
                    const Row& row = g.rows[static_cast<std::size_t>(h.row)];
                    it.id = gid + 2 + 2 * static_cast<uint32_t>(h.row);
                    it.role = row.fold != 0 ? funkgui::A11yRole::toggleButton
                            : row.heading   ? funkgui::A11yRole::staticText
                                            : funkgui::A11yRole::button;
                    it.title = asciiText(row.label);
                    it.value = asciiText(row.detail);
                    if (row.git)
                        it.description = "linked to " + row.remote;   // package 05: the git mark's words
                    it.checkable = row.active || row.marked || row.fold != 0;
                    it.checked = row.active || row.marked || row.fold == 1;
                    it.help = !row.help.empty()                ? row.help
                            : row.fold != 0                    ? "Shows or hides this group's projects"
                            : g.section == Section::files      ? "Shows this file; a double click renames it"
                            : g.section == Section::ir         ? "Shows the program's LLVM IR, as FMinus compiled it, in a read-only tab"
                            : g.section == Section::library    ? (row.marked ? "Shows this library file" : "Adds this library file to the program")
                            : g.section == Section::examples   ? "Puts this example in the program and runs it"
                            : g.section == Section::presets    ? "Applies this preset's dials"
                            : g.section == Section::assets     ? "A sample: #pragma sample reads it"
                            : g.section == Section::factory    ? "Opens this factory project (read-only: saving it makes a project of your own)"
                                                               : "Opens this project's newest version";
                    if (row.error)
                        it.description = "Has errors";
                    break;
                }
                case Part::close:
                    it.id = gid + 3 + 2 * static_cast<uint32_t>(h.row);
                    it.role = funkgui::A11yRole::button;
                    it.title = "Close " + asciiText(g.rows[static_cast<std::size_t>(h.row)].label);
                    break;
            }
            out.push_back(std::move(it));
        }
    }

    bool ExplorerItem::a11yAction(uint32_t id, funkgui::A11yAction, double, funkgui::GestureController&)
    {
        if (id <= base_ || id >= base_ + 1 + static_cast<uint32_t>(kSections) * kStride)
            return false;
        const uint32_t k = id - base_ - 1;
        const int g = static_cast<int>(k / kStride);
        const uint32_t j = k % kStride;
        for (const Hit& h : hits_)
        {
            if (h.group != g)
                continue;
            const bool match = (j == 0 && h.part == Part::header) || (j == 1 && h.part == Part::action)
                            || (j >= 2 && (j - 2) % 2 == 0 && h.part == Part::row && h.row == static_cast<int>((j - 2) / 2))
                            || (j >= 3 && (j - 3) % 2 == 0 && h.part == Part::close && h.row == static_cast<int>((j - 3) / 2));
            if (match)
            {
                if (h.part == Part::row)
                    if (const Row& row = groups_[static_cast<std::size_t>(g)].rows[static_cast<std::size_t>(h.row)]; row.heading && row.fold == 0)
                        return false;
                act(h, false);
                return true;
            }
        }
        return false;
    }

    void ExplorerItem::specLine(char* out, std::size_t n) const
    {
        const Hit* h = hitAt(pointer_);
        if (h == nullptr)
        {
            std::snprintf(out, n, "%s", "EXPLORER   THE PROGRAM'S FILES, ITS FOLDER AND ITS IR, THE PRESETS, THE PROJECTS, THE FACTORY'S, THE LIBRARY, THE EXAMPLES");
            return;
        }
        const Section s = groups_[static_cast<std::size_t>(h->group)].section;
        if (h->part == Part::row)
            if (const Row& row = groups_[static_cast<std::size_t>(h->group)].rows[static_cast<std::size_t>(h->row)]; !row.spec.empty())
            {
                std::snprintf(out, n, "%s", row.spec.c_str());
                return;
            }
        if (s == Section::ir && h->part == Part::row)
        {
            const int view = groups_[static_cast<std::size_t>(h->group)].rows[static_cast<std::size_t>(h->row)].index;
            std::snprintf(out, n, "%s", view == 1 ? "LLVM IR, LOWERED   AS FMINUS'S CODE GENERATOR WROTE IT, BEFORE ANY PASS   A READ-ONLY TAB, NOT A FILE"
                                      : view == 2 ? "LLVM IR, FROZEN   THE COPY WITH THE DIALS AS CONSTANTS, OPTIMISED   A READ-ONLY TAB, NOT A FILE"
                                                  : "LLVM IR, OPTIMISED   WHAT THE PROGRAM'S MACHINE CODE CAME FROM   A READ-ONLY TAB, NOT A FILE");
            return;
        }
        const char* line = h->part == Part::header ? "SECTION   SHOW OR HIDE IT"
                         : h->part == Part::close  ? (s == Section::assets ? "REMOVE   THE SAMPLE GOES (A PROGRAM THAT READS IT HEARS SILENCE FROM ITS NEXT RUN)"
                                                                           : "CLOSE   THE FILE'S TAB GOES (IT ASKS FIRST WHEN THE FILE HAS TEXT)")
                         : h->part == Part::action ? (s == Section::files     ? "NEW FILE   A TAB OF ITS OWN, ITS NAME TO TYPE"
                                                      : s == Section::folder  ? "STOP   THE FOLDER STAYS AS IT IS; NOTHING SYNCS UNTIL IT IS ON AGAIN"
                                                      : s == Section::presets ? "NEW PRESET   THE DIALS AS THEY ARE, UNDER A NAME, IN THE PROJECT OPEN"
                                                      : s == Section::assets  ? "ADD A SAMPLE   A WAV, AIFF OR FLAC FILE (OR DROP ONE ON THE PANEL)"
                                                                              : "PROJECTS   SAVE A VERSION, OPEN AN OLDER ONE, DELETE")
                         : s == Section::files     ? "FILE   SHOW IT   DOUBLE-CLICK RENAMES IT"
                         : s == Section::library   ? "LIBRARY   ADD IT AS A TAB (THE PROGRAM INCLUDES IT), OR SHOW IT"
                         : s == Section::examples  ? "EXAMPLE   START AFRESH FROM IT, THE DIALS AT THEIR DEFAULTS"
                         : s == Section::presets   ? "PRESET   ITS DIALS, AT ONCE (NO CODE CHANGES)"
                         : s == Section::factory   ? "FACTORY PROJECT   OPEN IT: ITS FILES AND ITS DIALS   READ-ONLY: SAVING MAKES A PROJECT OF YOUR OWN"
                                                   : "PROJECT   OPEN ITS NEWEST VERSION: ITS FILES AND ITS DIALS";
        std::snprintf(out, n, "%s", line);
    }
}
