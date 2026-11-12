// Source/editor/SettingsCard.cpp: see SettingsCard.h. The card: its box, its rail, the page's body, Done, the focus,
// the keys, the scroll and the drawing. What a page holds is SettingsPages.h's.
#include "editor/SettingsCard.h"

#include "editor/ChatItems.h"
#include "editor/ChatStyle.h"
#include "editor/Dropdown.h"
#include "editor/Items.h"
#include "editor/SettingsPages.h"
#include "editor/Theme.h"

#include "Product.h"

#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace playground::ui
{
    namespace
    {
        constexpr float kBoxW = 780.0f, kBoxH = 560.0f;  // at most: the window less 16 px a side
        constexpr float kRailW = 176.0f;                 // the pages' names
        constexpr float kPad = 24.0f;
        constexpr float kHeaderH = 84.0f;                // the page's title and its line, over the body
        constexpr float kFooterH = 66.0f;                // Done, under it
        constexpr float kButtonH = 32.0f;
        constexpr float kRailTop = 76.0f;                // the first page's name, from the card's top
        constexpr float kRailRowH = 30.0f, kRailStep = 32.0f;
        constexpr uint32_t kLineIds = SettingsCard::kA11yBase + 0x30000u;

        bool meet(const funkgui::Rect& a, const funkgui::Rect& b)
        {
            return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
        }

        // Each control's id: Done and the rail's pages apart, a page's controls by page, act and index.
        uint32_t idOf(int page, SettingsCard::Act act, int index)
        {
            using Act = SettingsCard::Act;
            if (act == Act::done)
                return SettingsCard::kA11yBase + 1u;
            if (act == Act::page)
                return SettingsCard::kA11yBase + 0x10u + static_cast<uint32_t>(index);
            return SettingsCard::kA11yBase + 0x1000u * (static_cast<uint32_t>(page) + 1u) + 0x80u * static_cast<uint32_t>(act)
                + static_cast<uint32_t>(std::clamp(index, 0, 0x7F));
        }

        // Outside the body's scroll: the rail's pages and Done.
        bool fixed(const SettingsCard::Control& c)
        {
            return c.act == SettingsCard::Act::done || c.act == SettingsCard::Act::page;
        }

        const char* sectionName(DeviceSection s)
        {
            switch (s)
            {
                case DeviceSection::output: return "Output";
                case DeviceSection::input:  return "Input";
                case DeviceSection::audio:  return "Audio";
                case DeviceSection::midi:   return "MIDI";
            }
            return "";
        }
    }

    SettingsCard::~SettingsCard()
    {
        if (viewer_)
            port_.removeDebugViewer();
    }

    int SettingsCard::shownPage() const
    {
        const auto all = settings::pages();
        settings::Context ctx{ port_, host_, const_cast<SettingsCard&>(*this) };
        const auto listed = [&](int i) {
            return i >= 0 && i < static_cast<int>(all.size()) && (all[static_cast<std::size_t>(i)].shown == nullptr
                                                                   || all[static_cast<std::size_t>(i)].shown(ctx));
        };
        if (listed(page_))
            return page_;
        for (int i = 0; i < static_cast<int>(all.size()); ++i)
            if (listed(i))
                return i;
        return 0;
    }

    std::string SettingsCard::pageId() const
    {
        const auto all = settings::pages();
        return all.empty() ? std::string() : std::string(all[static_cast<std::size_t>(shownPage())].id);
    }

    bool SettingsCard::open(std::string_view id)
    {
        sync();
        open_ = true;
        focus_ = {};
        ring_ = false;
        if (!id.empty())
        {
            const auto all = settings::pages();
            for (int i = 0; i < static_cast<int>(all.size()); ++i)
                if (all[static_cast<std::size_t>(i)].id == id)
                    page_ = i;
        }
        page_ = shownPage();
        pageChanged();
        return true;
    }

    void SettingsCard::close()
    {
        open_ = false;
        focus_ = {};
        countViewer();
        ++revision_;
    }

    bool SettingsCard::showPage(std::string_view id)
    {
        const auto all = settings::pages();
        settings::Context ctx{ port_, host_, *this };
        for (int i = 0; i < static_cast<int>(all.size()); ++i)
        {
            const settings::Page& p = all[static_cast<std::size_t>(i)];
            if (p.id != id || (p.shown != nullptr && !p.shown(ctx)))
                continue;
            if (!open_)
                return false;
            if (i != page_)
            {
                page_ = i;
                pageChanged();
            }
            return true;
        }
        return false;
    }

    void SettingsCard::pageChanged()
    {
        scroll_ = Scroll{};
        pageKey_ = 0;
        countViewer();
        ++revision_;
    }

    void SettingsCard::countViewer()
    {
        const bool want = open_ && pageId() == settings::kDebugPage.id;
        if (want == viewer_)
            return;
        viewer_ = want;
        if (want)
        {
            port_.addDebugViewer();
            gatherDebug();
            debugAt_ = -1.0;
        }
        else
        {
            port_.removeDebugViewer();
            debug_.clear();
        }
    }

    void SettingsCard::gatherDebug()
    {
        debug_ = port_.debugRows();
        if (hooks_.graphics)
            debug_.insert(debug_.begin() + std::min<std::ptrdiff_t>(1, static_cast<std::ptrdiff_t>(debug_.size())), hooks_.graphics());
        ++revision_;
    }

    std::string SettingsCard::debugText() const
    {
        std::string out = std::string(product::kName) + " " + product::kVersion + ": settings, Debug\n";
        for (const DebugSection& s : debug_)
        {
            out += "\n" + s.title + "\n";
            for (const DebugRow& r : s.rows)
                out += "  " + r.name + ": " + r.value + "\n";
        }
        return out;
    }

    bool SettingsCard::sync(funkgui::HostServices* host)
    {
        if (host != nullptr)
            host_ = host;
        bool moved = false;
        DevicePort* d = port_.devices();
        if (d == nullptr)
        {
            if (has_)
            {
                const bool theirs = open_ && pageId() == settings::kAudioPage.id;
                has_ = false;                            // the window let go of its devices
                setup_ = {};
                if (theirs)
                    close();                             // their page went with them
                else
                    page_ = shownPage();
                ++revision_;
                moved = true;
            }
        }
        else if (const uint64_t serial = d->serial(); !has_ || serial != seen_)
        {
            setup_ = d->setup();
            seen_ = serial;
            has_ = true;
            ++revision_;
            moved = true;
        }
        if (!open_)
            return moved;
        // What the page reads that changes with no input of the card's.
        const auto all = settings::pages();
        const settings::Page& p = all[static_cast<std::size_t>(shownPage())];
        settings::Context ctx{ port_, host, *this };
        if (p.key != nullptr)
            if (const uint64_t k = p.key(ctx); k != pageKey_)
            {
                pageKey_ = k;
                ++revision_;
                moved = true;
            }
        // The Debug page: read again once a second while it shows.
        if (viewer_ && host != nullptr)
        {
            const double now = host->nowSeconds();
            if (debugAt_ < 0.0)
                debugAt_ = now;
            else if (now - debugAt_ >= 1.0)
            {
                debugAt_ = now;
                gatherDebug();
                moved = true;
            }
        }
        return moved;
    }

    void SettingsCard::changed()
    {
        sync();
        ++revision_;
    }

    SettingsCard::Places SettingsCard::places(float width, float height) const
    {
        Places at;
        const auto all = settings::pages();
        at.page = shownPage();
        const float w = std::min(kBoxW, width - 32.0f);
        const float h = std::min(kBoxH, height - 32.0f);
        at.box = { std::round((width - w) * 0.5f), std::round((height - h) * 0.45f), w, h };
        at.rail = { at.box.x, at.box.y, kRailW, h };
        const float inner = w - kRailW - 2.0f * kPad;
        at.body = { at.box.x + kRailW + kPad, at.box.y + kHeaderH, inner, h - kHeaderH - kFooterH };

        // The rail: the pages listed, each a control.
        settings::Context ctx{ port_, host_, const_cast<SettingsCard&>(*this) };
        {
            float y = at.box.y + kRailTop;
            for (int i = 0; i < static_cast<int>(all.size()); ++i)
            {
                const settings::Page& p = all[static_cast<std::size_t>(i)];
                if (p.shown != nullptr && !p.shown(ctx))
                    continue;
                Control c;
                c.act = Act::page;
                c.index = i;
                c.r = { at.box.x + 12.0f, y, kRailW - 24.0f, kRailRowH };
                c.label = p.title;
                c.title = std::string("Settings page: ") + p.title;
                c.on = i == at.page;
                c.id = idOf(at.page, Act::page, i);
                at.controls.push_back(std::move(c));
                y += kRailStep;
            }
        }

        // The page, from the body's top (y 0), its left (x 0); placed in the body below.
        const std::size_t first = at.controls.size();
        {
            settings::Builder b(at, inner, ctx);
            const settings::Page& p = all[static_cast<std::size_t>(at.page)];
            if (p.build != nullptr)
                p.build(b);
        }
        for (std::size_t i = first; i < at.controls.size(); ++i)   // as tall as what it holds
            at.contentH = std::max(at.contentH, at.controls[i].r.bottom());
        for (const Line& l : at.lines)
            at.contentH = std::max(at.contentH, l.r.bottom());
        at.contentH = std::ceil(at.contentH);
        at.maxScroll = std::max(0.0f, at.contentH - at.body.h);
        at.scroll = std::clamp(scroll_.offset, 0.0f, at.maxScroll);
        const float dx = at.body.x, dy = at.body.y - at.scroll;
        for (std::size_t i = first; i < at.controls.size(); ++i)
        {
            Control& c = at.controls[i];
            c.r.x += dx;
            c.r.y += dy;
            c.id = idOf(at.page, c.act, c.index);
        }
        for (Line& l : at.lines)
        {
            l.r.x += dx;
            l.r.y += dy;
        }
        Control done;
        done.act = Act::done;
        done.r = { at.box.right() - kPad - 96.0f, at.box.bottom() - 22.0f - kButtonH, 96.0f, kButtonH };
        done.label = done.title = "Done";
        done.id = idOf(at.page, Act::done, 0);
        at.controls.push_back(std::move(done));
        return at;
    }

    int SettingsCard::focusIndex(const Places& at) const
    {
        for (std::size_t i = 0; i < at.controls.size(); ++i)
            if (at.controls[i].act == focus_.act && at.controls[i].index == focus_.index)
                return static_cast<int>(i);
        return -1;
    }

    std::optional<funkgui::Point> SettingsCard::centreOf(Act act, int index, float width, float height) const
    {
        const Places at = places(width, height);
        for (const Control& c : at.controls)
            if (c.act == act && c.index == index)
            {
                const funkgui::Point p{ c.r.centreX(), c.r.centreY() };
                if (!fixed(c) && !at.body.contains(p))
                    return std::nullopt;
                return p;
            }
        return std::nullopt;
    }

    std::optional<SettingsCard::Control> SettingsCard::focused(float width, float height) const
    {
        const Places at = places(width, height);
        const int i = focusIndex(at);
        if (i < 0)
            return std::nullopt;
        return at.controls[static_cast<std::size_t>(i)];
    }

    void SettingsCard::act(const Control& c, funkgui::HostServices* host)
    {
        if (!c.enabled)
            return;
        if (c.act == Act::done)
        {
            close();
            return;
        }
        const auto all = settings::pages();
        if (c.act == Act::page)
        {
            if (c.index >= 0 && c.index < static_cast<int>(all.size()) && c.index != page_)
            {
                page_ = c.index;
                pageChanged();
            }
            return;
        }
        const settings::Page& p = all[static_cast<std::size_t>(shownPage())];
        if (p.act == nullptr)
            return;
        settings::Context ctx{ port_, host, *this };
        p.act(c, ctx);
        if (open_)
            changed();
    }

    void SettingsCard::pointerDown(funkgui::Point p, float width, float height, funkgui::HostServices* host)
    {
        if (host != nullptr)
            host_ = host;
        const Places at = places(width, height);
        ring_ = false;
        if (!at.box.contains(p))
        {
            close();                                     // a click outside the card
            return;
        }
        if (Scroll s = scrollOf(at); s.press(p, barArea(at)))
        {
            scroll_ = s;                                 // the body's thumb drags (a press on its track jumps first)
            ++revision_;
            return;
        }
        for (const Control& c : at.controls)
            if (c.r.contains(p) && (fixed(c) || at.body.contains(p)))
            {
                focus_ = { c.act, c.index };
                act(c, host);
                break;
            }
        ++revision_;
    }

    void SettingsCard::wheel(const funkgui::WheelEvent& e, float width, float height)
    {
        // As the code scrolls (Scroll.h), a notch three lines: the delta as it came, WheelEvent::reversed never read.
        const Places at = places(width, height);
        Scroll s = scrollOf(at);
        if (!s.wheel(e.dy, e.smooth))
            return;
        scroll_ = s;
        ++revision_;
    }

    Scroll SettingsCard::scrollOf(const Places& at) const
    {
        Scroll s = scroll_;
        s.content = at.contentH;
        s.view = at.body.h;
        s.row = settings::kRowStep;                      // its line pitch
        s.clamp();
        return s;
    }

    funkgui::Rect SettingsCard::barArea(const Places& at) const
    {
        return { at.body.x, at.body.y, at.box.right() - 3.0f - at.body.x, at.body.h };   // the bar in the card's margin
    }

    void SettingsCard::pointerDrag(funkgui::Point p, float width, float height)
    {
        if (!scroll_.dragging())
            return;
        const Places at = places(width, height);
        Scroll s = scrollOf(at);
        s.drag(p, barArea(at));
        scroll_ = s;
        ++revision_;
    }

    void SettingsCard::pointerUp()
    {
        if (!scroll_.dragging())
            return;
        scroll_.release();
        ++revision_;
    }

    void SettingsCard::key(const funkgui::KeyEvent& e, funkgui::HostServices* host, float width, float height)
    {
        if (host != nullptr)
            host_ = host;
        using funkgui::Key;
        if (e.key == Key::escape)
        {
            close();
            return;
        }
        const Places at = places(width, height);
        const int n = static_cast<int>(at.controls.size());
        const int i = focusIndex(at);
        if (e.key == Key::tab || e.key == Key::down || e.key == Key::up)
        {
            if (n == 0)
                return;
            const bool back = e.key == Key::up || (e.key == Key::tab && e.mods.shift);
            const int next = i < 0 ? (back ? n - 1 : 0) : (i + (back ? n - 1 : 1)) % n;
            const Control& c = at.controls[static_cast<std::size_t>(next)];
            focus_ = { c.act, c.index };
            ring_ = true;
            // The body scrolled to show it.
            if (!fixed(c))
            {
                if (c.r.y < at.body.y)
                    scroll_.offset = std::max(0.0f, at.scroll - (at.body.y - c.r.y) - 4.0f);
                else if (c.r.bottom() > at.body.bottom())
                    scroll_.offset = std::min(at.maxScroll, at.scroll + (c.r.bottom() - at.body.bottom()) + 4.0f);
            }
            ++revision_;
            return;
        }
        if ((e.key == Key::enter || e.key == Key::space) && i >= 0)
        {
            ring_ = true;
            act(at.controls[static_cast<std::size_t>(i)], host);
            ++revision_;
        }
    }

    funkgui::Cursor SettingsCard::cursor(funkgui::Point p, float width, float height) const
    {
        const Places at = places(width, height);
        for (const Control& c : at.controls)
            if (c.enabled && c.r.contains(p) && (fixed(c) || at.body.contains(p)))
                return funkgui::Cursor::pointingHand;
        return funkgui::Cursor::normal;
    }

    void SettingsCard::draw(funkgui::Canvas& c, const funkgui::Theme& th, float width, float height, funkgui::Point pointer,
                            bool hasPointer) const
    {
        const chat::Palette p = chat::palette(th);
        const Places at = places(width, height);
        const funkgui::Rect& box = at.box;
        const auto all = settings::pages();
        const settings::Page& page = all[static_cast<std::size_t>(at.page)];
        c.rrect(0.0f, 0.0f, width, height, 0.0f, th.ground.withAlpha(0.68f));   // the panel dimmed
        constexpr funkgui::Col black{ 0, 0, 0, 255 };
        for (int k = 3; k >= 1; --k)                     // a soft shadow under the card
        {
            const float g = static_cast<float>(k) * 3.0f;
            c.rrect(box.x - g, box.y - g + 6.0f, box.w + 2.0f * g, box.h + 2.0f * g, 16.0f + g, black.withAlpha(0.07f));
        }
        c.rrect(box.x, box.y, box.w, box.h, 16.0f, p.card, 1.0f, p.cardEdge);
        // The rail: its ground, the card's title over the pages' names.
        {
            const funkgui::Canvas::ClipScope clip(c, { box.x + 1.0f, box.y + 1.0f, kRailW - 1.0f, box.h - 2.0f });
            c.rrect(box.x + 1.0f, box.y + 1.0f, kRailW + 16.0f, box.h - 2.0f, 15.0f, p.raised);
        }
        c.hairlineV(box.x + kRailW, box.y + 1.0f, box.h - 2.0f, p.cardEdge);
        const float rx = box.x + 18.0f;
        c.rrect(rx, box.y + 22.0f, 30.0f, 30.0f, 9.0f, p.accent.withAlpha(0.16f));
        chat::drawIcon(c, chat::Icon::gear, rx + 15.0f, box.y + 37.0f, 16.0f, p.accent);
        c.text("Settings", rx + 42.0f, c.capCentreTop(box.y + 37.0f, chat::kH2), chat::kH2, p.text);

        // The page's title and its line.
        const float x = at.body.x;
        c.text(fitText(c, page.title, chat::kH2, at.body.w).c_str(), x, c.capCentreTop(box.y + 37.0f, chat::kH2), chat::kH2, p.text);
        c.text(fitText(c, page.blurb, chat::kSmall, box.right() - kPad - x).c_str(), x, c.capCentreTop(box.y + 62.0f, chat::kSmall),
               chat::kSmall, p.soft);

        const int focus = ring_ ? focusIndex(at) : -1;
        const auto hovered = [&](const Control& k) {
            return hasPointer && k.enabled && k.r.contains(pointer) && (fixed(k) || at.body.contains(pointer));
        };
        const auto ring = [&](const Control& k, float radius) {
            c.rrect(k.r.x - 2.0f, k.r.y - 2.0f, k.r.w + 4.0f, k.r.h + 4.0f, radius + 2.0f, p.card.withAlpha(0.0f), 1.5f, p.accent);
        };
        // The rail's pages: the one shown filled.
        for (std::size_t i = 0; i < at.controls.size(); ++i)
        {
            const Control& k = at.controls[i];
            if (k.act != Act::page)
                continue;
            const funkgui::Rect& r = k.r;
            if (k.on)
                c.rrect(r.x, r.y, r.w, r.h, 8.0f, p.accent.withAlpha(0.16f));
            else if (hovered(k))
                c.rrect(r.x, r.y, r.w, r.h, 8.0f, p.hover);
            c.text(fitText(c, k.label, k.on ? chat::kSmallStrong : chat::kSmall, r.w - 20.0f).c_str(), r.x + 10.0f,
                   c.capCentreTop(r.centreY(), chat::kSmall), k.on ? chat::kSmallStrong : chat::kSmall, k.on ? p.text : p.soft);
            if (static_cast<int>(i) == focus)
                ring(k, 8.0f);
        }
        {
            const funkgui::Canvas::ClipScope clip(c, { box.x + kRailW + 1.0f, at.body.y, box.right() - box.x - kRailW - 2.0f, at.body.h });
            for (const Line& l : at.lines)
            {
                const float mid = l.r.centreY();
                switch (l.kind)
                {
                    case Line::Kind::heading:
                    {
                        const std::string t = fitText(c, l.text, chat::kSmallStrong, l.r.w);
                        c.text(t.c_str(), l.r.x, c.capCentreTop(mid, chat::kSmallStrong), chat::kSmallStrong, p.dim);
                        const float after = l.r.x + c.textWidth(t.c_str(), chat::kSmallStrong) + 10.0f;
                        c.hairlineH(after, std::round(mid), std::max(0.0f, l.r.right() - after), p.cardEdge);
                        break;
                    }
                    case Line::Kind::label:
                        c.text(fitText(c, l.text, chat::kSmall, l.r.w).c_str(), l.r.x, c.capCentreTop(mid, chat::kSmall), chat::kSmall, p.soft);
                        break;
                    case Line::Kind::value:
                        c.text(fitText(c, l.text, chat::kSmall, l.r.w).c_str(), l.r.x, c.capCentreTop(mid, chat::kSmall), chat::kSmall, p.text);
                        break;
                    case Line::Kind::note:
                        c.text(fitText(c, l.text, chat::kSmall, l.r.w).c_str(), l.r.x, c.capCentreTop(mid, chat::kSmall), chat::kSmall, p.faint);
                        break;
                    case Line::Kind::error:
                        c.text(fitText(c, l.text, chat::kSmall, l.r.w).c_str(), l.r.x, c.capCentreTop(mid, chat::kSmall), chat::kSmall, p.danger);
                        break;
                }
            }
            for (std::size_t i = 0; i + 1 < at.controls.size(); ++i)   // Done, the last, is outside the body
            {
                const Control& k = at.controls[i];
                if (k.act == Act::page)
                    continue;
                const funkgui::Rect& r = k.r;
                const bool hover = hovered(k);
                if (k.menu)
                {
                    // A select: a dropdown's trigger in a form (Dropdown.h).
                    const dropdown::State state{ k.enabled, hover, menuOpen_ && menuOpen_(r), static_cast<int>(i) == focus };
                    const funkgui::Col labelInk = dropdown::drawTrigger(c, p, r, dropdown::Base::field, state);
                    c.text(fitText(c, k.label, chat::kSmall, dropdown::labelRoom(r, r.x + 14.0f)).c_str(), r.x + 14.0f,
                           c.capCentreTop(r.centreY(), chat::kSmall), chat::kSmall, labelInk);
                }
                else if (k.toggle)
                {
                    // A switch: its track (the accent when on), its knob, its name after it.
                    using settings::kSwitchH;
                    using settings::kSwitchW;
                    const float ty = r.centreY() - 0.5f * kSwitchH;
                    const funkgui::Col track = k.on ? (hover ? funkgui::mix(p.accent, p.text, 0.12f) : p.accent) : (hover ? p.hover : p.composer);
                    c.rrect(r.x, ty, kSwitchW, kSwitchH, 0.5f * kSwitchH, track, k.on ? 0.0f : 1.0f, p.composerEdge);
                    const float kx = k.on ? r.x + kSwitchW - 0.5f * kSwitchH : r.x + 0.5f * kSwitchH;
                    c.disc(kx, r.centreY(), 0.5f * kSwitchH - 2.5f, k.on ? p.onAccent : p.soft);
                    c.text(fitText(c, k.label, chat::kSmall, r.w - kSwitchW - 10.0f).c_str(), r.x + kSwitchW + 10.0f,
                           c.capCentreTop(r.centreY(), chat::kSmall), chat::kSmall, k.on ? p.text : p.soft);
                    if (static_cast<int>(i) == focus)
                        c.rrect(r.x - 2.0f, ty - 2.0f, kSwitchW + 4.0f, kSwitchH + 4.0f, 0.5f * kSwitchH + 2.0f, p.card.withAlpha(0.0f), 1.5f,
                                p.accent);
                }
                else
                {
                    // A button: an outlined pill, its icon before its label (Test's play).
                    c.rrect(r.x, r.y, r.w, r.h, r.h * 0.5f, hover ? p.hover : p.card.withAlpha(0.0f), 1.0f, p.composerEdge);
                    float tx = r.x + 14.0f;
                    if (k.icon != chat::Icon::none)
                    {
                        chat::drawIcon(c, k.icon, r.x + 20.0f, r.centreY(), 11.0f, k.enabled ? p.soft : p.faint);
                        tx = r.x + 32.0f;
                    }
                    c.text(fitText(c, k.label, chat::kSmall, r.right() - 10.0f - tx).c_str(), tx, c.capCentreTop(r.centreY(), chat::kSmall),
                           chat::kSmall, k.enabled ? (hover ? p.text : p.soft) : p.faint);
                    if (static_cast<int>(i) == focus)
                        ring(k, r.h * 0.5f);
                }
            }
        }
        // The body's scroll bar, when the window is too short for the page.
        if (at.maxScroll > 0.0f)
        {
            const funkgui::Rect& b = at.body;
            scrollOf(at).draw(c, barArea(at), th);
            c.hairlineH(box.x + kRailW + 1.0f, b.bottom() + 6.0f, box.right() - box.x - kRailW - 2.0f, p.cardEdge);
        }

        // Done.
        {
            const Control& k = at.controls.back();
            const funkgui::Rect& r = k.r;
            const bool hover = hovered(k);
            c.rrect(r.x, r.y, r.w, r.h, r.h * 0.5f, hover ? funkgui::mix(p.accent, p.text, 0.12f) : p.accent);
            c.text("Done", r.centreX(), c.capCentreTop(r.centreY(), chat::kSmallStrong), chat::kSmallStrong, p.onAccent, funkgui::Align::centre);
            if (focus == static_cast<int>(at.controls.size()) - 1)
                ring(k, r.h * 0.5f);
        }
    }

    void SettingsCard::accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const
    {
        const Places at = places(width, height);
        const auto all = settings::pages();
        funkgui::A11yItem box;
        box.id = kA11yBase;
        box.role = funkgui::a11yrole::group;
        box.bounds = at.box;
        box.title = "Settings";
        box.description = std::string(all[static_cast<std::size_t>(at.page)].title) + ": " + all[static_cast<std::size_t>(at.page)].blurb;
        out.push_back(box);
        uint32_t n = 0;
        for (const Line& l : at.lines)
        {
            if (l.kind == Line::Kind::label && pageId() == settings::kAudioPage.id)
                continue;                                // each control's title says it
            funkgui::A11yItem it;
            it.id = kLineIds + n++;
            it.parent = kA11yBase;
            it.role = funkgui::A11yRole::staticText;
            it.bounds = l.r;
            it.title = l.kind == Line::Kind::error && pageId() == settings::kAudioPage.id
                ? std::string(sectionName(setup_.errorOf)) + " error: " + l.text
                : l.text;
            it.readOnly = true;
            it.visible = meet(at.body, l.r);
            out.push_back(std::move(it));
        }
        for (const Control& c : at.controls)
        {
            funkgui::A11yItem it;
            it.id = c.id;
            it.parent = kA11yBase;
            it.bounds = c.r;
            it.title = c.title;
            it.enabled = c.enabled;
            it.visible = fixed(c) || meet(at.body, c.r);
            if (c.menu)
            {
                it.role = funkgui::A11yRole::comboBox;
                it.value = c.label;
            }
            else if (c.toggle)
            {
                it.role = funkgui::A11yRole::toggleButton;
                it.checkable = true;
                it.checked = c.on;
                if (c.act == Act::mute)
                    it.help = settings::kMuteWhy;
            }
            else if (c.act == Act::page)
            {
                it.role = funkgui::A11yRole::button;
                it.checkable = true;
                it.checked = c.on;                       // the page shown
            }
            else
                it.role = funkgui::A11yRole::button;
            out.push_back(std::move(it));
        }
    }

    bool SettingsCard::a11yAction(uint32_t id, funkgui::A11yAction a, float width, float height, funkgui::HostServices* host)
    {
        if (host != nullptr)
            host_ = host;
        const Places at = places(width, height);
        for (const Control& c : at.controls)
        {
            if (c.id != id)
                continue;
            focus_ = { c.act, c.index };
            if (a == funkgui::A11yAction::press || a == funkgui::A11yAction::toggle || a == funkgui::A11yAction::showMenu)
                act(c, host);
            ++revision_;
            return true;
        }
        return false;
    }
}
