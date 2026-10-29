// Source/installer/Panel.cpp: see Panel.h.
#include "installer/Panel.h"

#include "editor/ChatStyle.h"
#include "editor/LogoMark.h"
#include "editor/Theme.h"

#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <utility>

namespace playground::installer
{
    namespace chat = ui::chat;
    namespace logo = ui::logo;

    namespace
    {
        constexpr float kW = static_cast<float>(Panel::kWidth), kH = static_cast<float>(Panel::kHeight);
        constexpr float kSide = 208.0f;                  // the sidebar
        constexpr float kPad = 36.0f;
        constexpr float kX0 = kSide + kPad;              // the page's left edge
        constexpr float kInnerW = kW - kX0 - kPad;       // and width
        constexpr float kButtonH = 32.0f;
        constexpr float kFooterY = kH - 28.0f - kButtonH;
        constexpr float kSwitchW = 30.0f, kSwitchH = 17.0f;
        constexpr float kTopStrip = 44.0f;               // a press here, on nothing, drags the window
        constexpr uint32_t kA11yBase = 0x1000u;
        constexpr uint32_t kLineIds = 0x9000u;
        constexpr uint32_t kProgressId = 0x8F00u;
        constexpr double kRefreshSeconds = 3.0;          // Choose and Uninstall look again this often

        constexpr funkgui::TextStyle kHero{ 20.0f, 0.0f, 0.07f, false };
        constexpr funkgui::TextStyle kTitle{ 17.0f, 0.0f, 0.075f, false };

        float textW(const std::string& s, const funkgui::TextStyle& st)
        {
            return funkgui::text::width(funkgui::FontService::get().atlas(), s.c_str(), st);
        }

        // `s` cut to `maxW` px of `st`, with an ellipsis when it is cut.
        std::string fit(std::string s, const funkgui::TextStyle& st, float maxW)
        {
            if (textW(s, st) <= maxW)
                return s;
            while (!s.empty() && textW(s + "...", st) > maxW)
                s.pop_back();
            return s + "...";
        }

        // `s` in lines of at most `maxW` px of `st`, words kept whole while they fit.
        std::vector<std::string> wrapped(const std::string& s, const funkgui::TextStyle& st, float maxW)
        {
            std::vector<std::string> out;
            std::string line;
            std::size_t at = 0;
            while (at < s.size())
            {
                std::size_t end = s.find_first_of(" \n", at);
                if (end == std::string::npos)
                    end = s.size();
                const std::string word = s.substr(at, end - at);
                if (!word.empty())
                {
                    const std::string longer = line.empty() ? word : line + " " + word;
                    if (!line.empty() && textW(longer, st) > maxW)
                    {
                        out.push_back(line);
                        line = word;
                    }
                    else
                        line = longer;
                }
                if (end < s.size() && s[end] == '\n' && !line.empty())
                {
                    out.push_back(line);
                    line.clear();
                }
                at = end + 1;
            }
            if (!line.empty())
                out.push_back(line);
            for (std::string& l : out)
                l = fit(l, st, maxW);                    // a word longer than the line
            return out;
        }

        uint32_t idOf(Panel::Act a, int index)
        {
            return kA11yBase + 0x100u * (static_cast<uint32_t>(a) + 1u) + static_cast<uint32_t>(index);
        }

        std::string switchKey(const std::string& key, Part part)
        {
            return key + "/" + partExtension(part);
        }

        float pillW(const std::string& label, const funkgui::TextStyle& st)
        {
            return std::ceil(textW(label, st) + 32.0f);
        }

        const funkgui::TextStyle& styleOf(Panel::Line::Kind k)
        {
            switch (k)
            {
                case Panel::Line::Kind::hero:     return kHero;
                case Panel::Line::Kind::title:    return kTitle;
                case Panel::Line::Kind::strong:   return chat::kStrong;
                case Panel::Line::Kind::body:     return chat::kBody;
                case Panel::Line::Kind::subtitle:
                case Panel::Line::Kind::label:
                case Panel::Line::Kind::value:
                case Panel::Line::Kind::note:
                case Panel::Line::Kind::faint:
                case Panel::Line::Kind::error:
                case Panel::Line::Kind::warn:     return chat::kSmall;
            }
            return chat::kSmall;
        }

        const char* stateWords(State s)
        {
            switch (s)
            {
                case State::waiting: return "waiting";
                case State::working: return "working";
                case State::done:    return "done";
                case State::skipped: return "skipped";
                case State::failed:  return "failed";
                case State::notRun:  return "not changed";
            }
            return "";
        }
    }

    Panel::Panel(Engine& engine, AppPort& app, std::string installerName)
        : engine_(engine), app_(app), name_(std::move(installerName))
    {
        refresh();
    }

    Panel::~Panel()
    {
        cancel_.store(true);
        if (worker_.joinable())
            worker_.join();
    }

    void Panel::attach(funkgui::HostServices& host)
    {
        host_ = &host;
    }

    // ---- state -------------------------------------------------------------------------------------------------------

    void Panel::refresh()
    {
        std::vector<Selection> all;
        for (const Product& pr : engine_.payload().products)
            for (const Part part : kParts)
                all.push_back({ pr.key, part });
        choices_ = engine_.plan(all, scope_);
        copies_ = engine_.installed();                   // each on, but links and those switched off (by path)
        copyNotes_ = page_ == Page::uninstall ? engine_.uninstallNotes(copies_) : std::vector<std::string>{};
        copyOn_.assign(copies_.size(), true);
        for (std::size_t i = 0; i < copies_.size(); ++i)
            copyOn_[i] = !copies_[i].link && copyOff_.count(copies_[i].path.string()) == 0;
        refreshedAt_ = now_;
        changed();
    }

    void Panel::show(Page page)
    {
        if (busy())
            return;
        page_ = page;
        appeared();
        if (page == Page::choose || page == Page::uninstall || page == Page::welcome)
            refresh();
        changed();
    }

    void Panel::appeared()
    {
        focus_ = {};
        ring_ = false;
        shownAt_ = now_;
    }

    bool Panel::chosen(const std::string& key, Part part) const
    {
        for (const Step& s : choices_.steps)
            if (s.key == key && s.part == part)
            {
                if (s.action == Action::blocked)
                    return false;
                const auto it = on_.find(switchKey(key, part));
                if (it != on_.end())
                    return it->second;
                return s.action != Action::downgrade;    // a newer copy is kept unless asked
            }
        return false;
    }

    void Panel::choose(const std::string& key, Part part, bool on)
    {
        on_[switchKey(key, part)] = on;
        changed();
    }

    void Panel::setScope(Scope scope)
    {
        if (scope_ == scope)
            return;
        scope_ = scope;
        refresh();
    }

    bool Panel::busy() const
    {
        const std::lock_guard<std::mutex> lock(runMutex_);
        return running_;
    }

    bool Panel::working() const
    {
        // tick() takes a finished run's report, and a hidden window's host does not tick: a quit asks this instead.
        const std::lock_guard<std::mutex> lock(runMutex_);
        return running_ && !runFinished_;
    }

    void Panel::waitForRun()
    {
        if (worker_.joinable())
            worker_.join();
    }

    std::string Panel::kindWords(const Product& p) const
    {
        return p.instrument() ? "a synth" : "an effect";
    }

    // What installing a part does, in words, for its row.
    std::string Panel::statusOf(const Step& s) const
    {
        switch (s.action)
        {
            case Action::install:   return "not installed: installs " + s.to;
            case Action::upgrade:   return s.from + " installed: upgrades to " + s.to;
            case Action::downgrade: return s.from + " installed, newer than " + s.to + ": kept unless switched on";
            case Action::rebuild:   return s.from + " installed, another build: replaces it";
            case Action::replace:   return "a copy of no known version: replaces it with " + s.to;
            case Action::same:      return s.to + " installed: already up to date";
            case Action::blocked:   return s.why;
        }
        return {};
    }

    // ---- the runs ----------------------------------------------------------------------------------------------------

    void Panel::startInstall()
    {
        if (busy())
            return;
        std::vector<Selection> sel;
        for (const Step& s : choices_.steps)
            if (chosen(s.key, s.part))
                sel.push_back({ s.key, s.part });
        Plan plan = engine_.plan(sel, scope_);
        if (plan.changing() == 0)
            return;
        runPlan_ = plan;
        runNotes_ = plan.notes;
        uninstalling_ = false;
        {
            const std::lock_guard<std::mutex> lock(runMutex_);
            runStates_.assign(plan.steps.size(), State::waiting);
            runLines_.assign(plan.steps.size(), std::string());
            runFinished_ = false;
            running_ = true;
            ++runSerial_;
        }
        cancel_.store(false);
        page_ = Page::installing;
        appeared();
        changed();
        if (worker_.joinable())
            worker_.join();
        worker_ = std::thread([this, run = std::move(plan)]() {
            Report r = engine_.install(run, [this](int i, State s, const std::string& line) {
                const std::lock_guard<std::mutex> lock(runMutex_);
                if (i >= 0 && static_cast<std::size_t>(i) < runStates_.size())
                {
                    runStates_[static_cast<std::size_t>(i)] = s;
                    runLines_[static_cast<std::size_t>(i)] = line;
                }
                ++runSerial_;
            }, &cancel_);
            const std::lock_guard<std::mutex> lock(runMutex_);
            runReport_ = std::move(r);
            runFinished_ = true;
            ++runSerial_;
        });
    }

    void Panel::startUninstall()
    {
        if (busy())
            return;
        std::vector<Copy> copies;
        for (std::size_t i = 0; i < copies_.size(); ++i)
            if (copyOn_[i] && !copies_[i].link)
                copies.push_back(copies_[i]);
        if (copies.empty())
            return;
        runCopies_ = copies;
        runNotes_ = engine_.uninstallNotes(copies);
        uninstalling_ = true;
        {
            const std::lock_guard<std::mutex> lock(runMutex_);
            runStates_.assign(copies.size(), State::waiting);
            runLines_.assign(copies.size(), std::string());
            runFinished_ = false;
            running_ = true;
            ++runSerial_;
        }
        cancel_.store(false);
        page_ = Page::installing;
        appeared();
        changed();
        if (worker_.joinable())
            worker_.join();
        worker_ = std::thread([this, run = std::move(copies)]() {
            Report r = engine_.uninstall(run, [this](int i, State s, const std::string& line) {
                const std::lock_guard<std::mutex> lock(runMutex_);
                if (i >= 0 && static_cast<std::size_t>(i) < runStates_.size())
                {
                    runStates_[static_cast<std::size_t>(i)] = s;
                    runLines_[static_cast<std::size_t>(i)] = line;
                }
                ++runSerial_;
            }, &cancel_);
            const std::lock_guard<std::mutex> lock(runMutex_);
            runReport_ = std::move(r);
            runFinished_ = true;
            ++runSerial_;
        });
    }

    void Panel::finishRun()
    {
        if (worker_.joinable())
            worker_.join();
        {
            const std::lock_guard<std::mutex> lock(runMutex_);
            report_ = runReport_;
            running_ = false;
            runFinished_ = false;
        }
        page_ = Page::done;
        appeared();
        refresh();
        changed();
    }

    void Panel::tick(float dt)
    {
        now_ += static_cast<double>(dt);
        bool finished = false;
        bool moved = false;
        {
            const std::lock_guard<std::mutex> lock(runMutex_);
            if (running_)
            {
                spin_ += dt;
                moved = runSerial_ != runSeen_;
                runSeen_ = runSerial_;
                finished = runFinished_;
            }
        }
        if (moved)
            changed();
        if (finished)
            finishRun();
        if (!running_ && (page_ == Page::choose || page_ == Page::uninstall) && now_ - refreshedAt_ > kRefreshSeconds)
            refresh();
        if (returnLatched_ && !app_.returnHeld())
            returnLatched_ = false;                      // let go: the next Return is a new press
        // The window's buttons sit over the sidebar, where its top keeps them room.
        if (!buttonsPlaced_ && app_.windowButtons().width > 0.0f)
        {
            app_.placeWindowButtons(kButtonsX, kButtonsMiddle);
            buttonsPlaced_ = true;
        }
    }

    void Panel::idle(double nowSec)
    {
        (void) nowSec;
    }

    bool Panel::wantsFullRate() const
    {
        return busy();                                   // the working step's glyph turns
    }

    // ---- layout ------------------------------------------------------------------------------------------------------

    void Panel::footer(Layout& L, std::vector<Control> left, std::vector<Control> right) const
    {
        float x = kX0;
        for (Control& c : left)
        {
            const float w = std::max(96.0f, pillW(c.label, chat::kSmallStrong));
            c.r = { x, kFooterY, w, kButtonH };
            x += w + 10.0f;
            c.id = idOf(c.act, c.index);
            L.controls.push_back(c);
        }
        float r = kX0 + kInnerW;
        std::vector<Control> placed;
        for (auto it = right.rbegin(); it != right.rend(); ++it)
        {
            Control c = *it;
            const float w = std::max(96.0f, pillW(c.label, chat::kSmallStrong));
            c.r = { r - w, kFooterY, w, kButtonH };
            r -= w + 10.0f;
            c.id = idOf(c.act, c.index);
            placed.insert(placed.begin(), c);
        }
        for (Control& c : placed)
            L.controls.push_back(c);
    }

    int Panel::noteLines(Layout& L, const std::vector<std::string>& notes, float y, int room) const
    {
        const int n = static_cast<int>(notes.size());
        if (room <= 0 || n == 0)
            return 0;
        const float w = kInnerW - 20.0f;
        const auto place = [&](const std::string& text) {
            Line l{ Line::Kind::warn, fit(text, chat::kSmall, w), { kX0 + 20.0f, y, w, 15.0f } };
            if (l.text != text)
                l.whole = text;
            L.lines.push_back(std::move(l));
            y += 15.0f;
        };
        // One line each; when there are more than fit, the last line holds the rest, as much as fits (all of it in its
        // accessibility title), never a bare ellipsis.
        const int whole = n <= room ? n : room - 1;
        for (int i = 0; i < whole; ++i)
            place(notes[static_cast<std::size_t>(i)]);
        if (whole < n)
        {
            std::string rest;
            for (int i = whole; i < n; ++i)
                rest += (rest.empty() ? "" : " ") + notes[static_cast<std::size_t>(i)];
            place(rest);
        }
        return whole < n ? room : n;
    }

    Panel::Layout Panel::layout() const
    {
        Layout L;
        // The sidebar: both marks, the project, the steps.
        const auto& products = engine_.payload().products;
        float mx = 24.0f;
        for (const Product& pr : products)
        {
            if (mx + logo::kTile > kSide - 16.0f)
                break;
            L.marks.push_back({ { mx, 56.0f, logo::kTile, logo::kTile }, pr.instrument(), 1.0f });
            mx += logo::kTile + 8.0f;
        }
        const std::string project = engine_.payload().project.empty() ? name_ : engine_.payload().project;
        L.lines.push_back({ Line::Kind::strong, fit(project, chat::kStrong, kSide - 48.0f), { 24.0f, 96.0f, kSide - 48.0f, 18.0f } });
        L.lines.push_back({ Line::Kind::faint, "Installer", { 24.0f, 114.0f, kSide - 48.0f, 16.0f } });
        switch (page_)
        {
            case Page::welcome:   layoutWelcome(L); break;
            case Page::choose:    layoutChoose(L); break;
            case Page::installing: layoutRun(L); break;
            case Page::done:      layoutDone(L); break;
            case Page::uninstall: layoutUninstall(L); break;
        }
        return L;
    }

    void Panel::layoutWelcome(Layout& L) const
    {
        const Payload& pay = engine_.payload();
        // The hero: each product's mark, large, and the project.
        float x = kX0;
        constexpr float kScale = 2.0f;
        for (const Product& pr : pay.products)
        {
            L.marks.push_back({ { x, 40.0f, logo::kTile * kScale, logo::kTile * kScale }, pr.instrument(), kScale });
            x += logo::kTile * kScale + 12.0f;
        }
        const std::string project = pay.project.empty() ? name_ : pay.project;
        const float tx = x + 10.0f, tw = kX0 + kInnerW - tx;
        L.lines.push_back({ Line::Kind::hero, fit(project, kHero, tw), { tx, 46.0f, tw, 24.0f } });
        std::vector<std::string> kinds;
        for (const Product& pr : pay.products)
            kinds.push_back(kindWords(pr));
        std::string what;
        for (std::size_t i = 0; i < kinds.size(); ++i)
            what += (i == 0 ? "" : i + 1 == kinds.size() ? " and " : ", ") + kinds[i];
        what += kinds.size() == 1 ? " you write while it plays" : " you write while they play";
        L.lines.push_back({ Line::Kind::subtitle, fit(what, chat::kSmall, tw), { tx, 72.0f, tw, 16.0f } });

        if (!pay.error.empty())
        {
            float y = 140.0f;
            for (const std::string& l : wrapped(pay.error, chat::kSmall, kInnerW))
            {
                L.lines.push_back({ Line::Kind::error, l, { kX0, y, kInnerW, 16.0f } });
                y += 17.0f;
            }
            footer(L, {}, { { Act::finish, 0, Control::Kind::primary, {}, "Quit", "Quit the installer", {}, false, true, 0 } });
            return;
        }

        // Each product: its name, what it is, the version this installer holds, its parts.
        float y = 120.0f;
        for (const Product& pr : pay.products)
        {
            const funkgui::Rect card{ kX0, y, kInnerW, 64.0f };
            L.cards.push_back(card);
            L.marks.push_back({ { card.x + 16.0f, card.y + 19.0f, logo::kTile, logo::kTile }, pr.instrument(), 1.0f });
            const float lx = card.x + 56.0f, lw = card.w - 72.0f;
            L.lines.push_back({ Line::Kind::strong, fit(pr.name, chat::kStrong, lw * 0.6f),
                                { lx, card.y + 13.0f, lw * 0.6f, 18.0f } });
            const std::string version = pr.version.empty() ? std::string("version unknown") : "version " + pr.version;
            L.lines.push_back({ Line::Kind::value, version, { lx, card.y + 13.0f, lw, 18.0f }, true });
            std::string parts;
            for (const Part part : kParts)
                if (pr.offer(part).present)
                    parts += (parts.empty() ? "" : ", ") + std::string(part == Part::app ? "app" : partName(part));
            const std::string ext = pr.offer(Part::extension).version;
            const std::string line = std::string(pr.instrument() ? "A synth" : "An effect") + ": " + parts
                                     + (ext.empty() ? "" : " (" + ext + ")");
            L.lines.push_back({ Line::Kind::faint, fit(line, chat::kSmall, lw), { lx, card.y + 35.0f, lw, 16.0f } });
            y += 76.0f;
        }
        y += 14.0f;
        const std::string about = "Each is a plugin whose sound is a program you write, or ask Claude to write, while it "
                                  "plays. Next, choose the parts to install and for whom: just for you, or for every "
                                  "user of this Mac.";
        for (const std::string& l : wrapped(about, chat::kBody, kInnerW))
        {
            L.lines.push_back({ Line::Kind::body, l, { kX0, y, kInnerW, chat::kBodyLine } });
            y += chat::kBodyLine;
        }
        footer(L, {}, { { Act::next, 0, Control::Kind::primary, {}, "Continue", "Continue to the choice of parts", {}, false,
                          !pay.products.empty(), 0 } });
    }

    void Panel::layoutChoose(Layout& L) const
    {
        const Payload& pay = engine_.payload();
        L.lines.push_back({ Line::Kind::title, "Choose what to install", { kX0, 34.0f, kInnerW, 22.0f } });
        L.lines.push_back({ Line::Kind::subtitle, "Each part has its switch; what is installed now is beside it.",
                            { kX0, 60.0f, kInnerW, 16.0f } });
        constexpr float kHead = 30.0f, kStatusX = 220.0f, kNotesY = 84.0f;
        const float products = static_cast<float>(pay.products.size());
        const std::string where = scope_ == Scope::user
                                      ? "Plugins in ~/Library/Audio/Plug-Ins, apps in "
                                            + engine_.shown(engine_.folder(pay.products.empty() ? Product{} : pay.products.front(),
                                                                           Part::app, Scope::user))
                                            + ": no password."
                                      : "Plugins in /Library/Audio/Plug-Ins, apps in /Applications: macOS asks for an "
                                        "administrator's password.";
        const std::vector<std::string> whereLines = wrapped(where, chat::kSmall, kInnerW);

        // The notes first, under the subtitle: what is open that uses what changes, a product installed in the other
        // scope too. The part rows close up (24 px down to 18) to make room for them: below the notes come the cards
        // (each its head, four rows, its padding and a gap), the scope's row and its sentence.
        const float bottom = kFooterY - 10.0f;
        const float below = 2.0f + 38.0f + 15.0f * static_cast<float>(whereLines.size());
        const auto cardsH = [&](float row) { return products * (kHead + 4.0f * row + 8.0f + 12.0f); };
        const int room = static_cast<int>(std::floor((bottom - kNotesY - 14.0f - below - cardsH(18.0f)) / 15.0f));
        const int placed = noteLines(L, choices_.notes, kNotesY, room);
        float y = placed == 0 ? 92.0f : kNotesY + 15.0f * static_cast<float>(placed) + 14.0f;
        const float kRow = std::clamp(std::floor((bottom - y - below - cardsH(0.0f)) / std::max(1.0f, 4.0f * products)),
                                      18.0f, 24.0f);
        for (std::size_t p = 0; p < pay.products.size(); ++p)
        {
            const Product& pr = pay.products[p];
            const float blockH = kHead + 4.0f * kRow + 8.0f;
            L.cards.push_back({ kX0 - 14.0f, y - 6.0f, kInnerW + 28.0f, blockH });
            L.marks.push_back({ { kX0, y, logo::kTile, logo::kTile }, pr.instrument(), 1.0f });
            L.lines.push_back({ Line::Kind::strong, pr.name, { kX0 + 36.0f, y + 4.0f, 260.0f, 18.0f } });
            L.lines.push_back({ Line::Kind::faint, std::string(pr.instrument() ? "synth" : "effect")
                                                       + (pr.version.empty() ? "" : ", " + pr.version),
                                { kX0 + 36.0f, y + 4.0f, kInnerW - 36.0f, 18.0f }, true });
            float ry = y + kHead + 2.0f;
            for (const Part part : kParts)
            {
                const Step* step = nullptr;
                for (const Step& s : choices_.steps)
                    if (s.key == pr.key && s.part == part)
                        step = &s;
                if (step == nullptr)
                    continue;
                const bool on = chosen(pr.key, part);
                const int index = static_cast<int>(p) * 4 + static_cast<int>(part);
                Control c;
                c.act = Act::part;
                c.index = index;
                c.kind = Control::Kind::toggle;
                c.r = { kX0, ry, kStatusX - 12.0f, kRow - 4.0f };
                c.label = partName(part);
                c.title = pr.name + " " + partName(part);
                c.help = statusOf(*step);
                c.on = on;
                c.enabled = step->action != Action::blocked;
                c.id = idOf(c.act, index);
                L.controls.push_back(c);
                const Line::Kind k = step->action == Action::blocked ? Line::Kind::error
                                   : step->action == Action::same     ? Line::Kind::faint
                                                                       : Line::Kind::note;
                Line status{ k, fit(statusOf(*step), chat::kSmall, kInnerW - kStatusX),
                             { kX0 + kStatusX, ry, kInnerW - kStatusX, kRow - 4.0f } };
                if (status.text != statusOf(*step))
                    status.whole = statusOf(*step);
                L.lines.push_back(std::move(status));
                ry += kRow;
            }
            y += blockH + 12.0f;
        }

        // The scope.
        y += 2.0f;
        L.lines.push_back({ Line::Kind::label, "Install for", { kX0, y, 90.0f, 28.0f } });
        const float sx = kX0 + 96.0f;
        const std::string me = "Just for me", all = "All users";
        const float w0 = pillW(me, chat::kSmall), w1 = pillW(all, chat::kSmall);
        L.rules.push_back({ sx - 3.0f, y - 3.0f, w0 + w1 + 6.0f, 34.0f });   // the switch's track
        L.controls.push_back({ Act::scope, 0, Control::Kind::radio, { sx, y, w0, 28.0f }, me, "Install just for me",
                               "Into your own Library folders: no password", scope_ == Scope::user, true, idOf(Act::scope, 0) });
        L.controls.push_back({ Act::scope, 1, Control::Kind::radio, { sx + w0, y, w1, 28.0f }, all, "Install for all users",
                               "Into the system's Library folders: macOS asks for an administrator's password",
                               scope_ == Scope::system, true, idOf(Act::scope, 1) });
        y += 38.0f;
        for (const std::string& l : whereLines)
        {
            L.lines.push_back({ Line::Kind::faint, l, { kX0, y, kInnerW, 15.0f } });
            y += 15.0f;
        }

        int changing = 0;
        for (const Step& s : choices_.steps)
            if (chosen(s.key, s.part) && s.changes())
                ++changing;
        footer(L, { { Act::toUninstall, 0, Control::Kind::secondary, {}, "Uninstall...",
                      "Uninstall: move installed copies to the Trash", {}, false, true, 0 } },
               { { Act::back, 0, Control::Kind::secondary, {}, "Back", "Back to the welcome", {}, false, true, 0 },
                 { Act::install, 0, Control::Kind::primary, {}, scope_ == Scope::system ? "Install..." : "Install",
                   scope_ == Scope::system ? "Install for all users (asks for an administrator's password)" : "Install",
                   changing == 0 ? "Nothing chosen needs installing" : std::to_string(changing) + " parts to install",
                   false, changing > 0, 0 } });
    }

    void Panel::layoutRun(Layout& L) const
    {
        std::vector<State> states;
        std::vector<std::string> lines;
        {
            const std::lock_guard<std::mutex> lock(runMutex_);
            states = runStates_;
            lines = runLines_;
        }
        const bool admin = !uninstalling_ ? runPlan_.admin() : false;
        L.lines.push_back({ Line::Kind::title, uninstalling_ ? "Moving to the Trash" : "Installing", { kX0, 34.0f, kInnerW, 22.0f } });
        const bool waiting = std::any_of(lines.begin(), lines.end(), [](const std::string& l) {
            return l.find("administrator") != std::string::npos;
        });
        const std::string sub = waiting ? "macOS asks for an administrator's password: the installer never sees it."
                              : uninstalling_ ? "Each copy goes to the Trash: nothing is deleted."
                                              : admin ? "Installing for all users, as an administrator."
                                                      : "Each part is copied beside its place, then put in place; old "
                                                        "copies go to the Trash.";
        L.lines.push_back({ Line::Kind::subtitle, fit(sub, chat::kSmall, kInnerW), { kX0, 60.0f, kInnerW, 16.0f } });

        int finished = 0, todo = 0;
        for (const State s : states)
        {
            if (s == State::skipped)
                continue;
            ++todo;
            if (s == State::done || s == State::failed || s == State::notRun)
                ++finished;
        }
        L.progress = { kX0, 92.0f, kInnerW, 8.0f };
        L.fraction = todo == 0 ? 1.0f : static_cast<float>(finished) / static_cast<float>(todo);

        const std::size_t n = states.size();
        const float row = std::clamp((kFooterY - 16.0f - 120.0f) / std::max<float>(1.0f, static_cast<float>(n)), 18.0f, 26.0f);
        float y = 120.0f;
        for (std::size_t i = 0; i < n; ++i)
        {
            std::string what;
            if (uninstalling_ && i < runCopies_.size())
            {
                const Copy& c = runCopies_[i];
                const Product* pr = engine_.payload().find(c.key);
                what = (pr != nullptr ? pr->name : c.key) + ", " + partName(c.part);
            }
            else if (i < runPlan_.steps.size())
                what = runPlan_.steps[i].product + ", " + partName(runPlan_.steps[i].part);
            L.glyphs.push_back({ { kX0 + 8.0f, y + 0.5f * row }, states[i] });
            L.lines.push_back({ Line::Kind::label, fit(what, chat::kSmall, 230.0f), { kX0 + 24.0f, y, 230.0f, row } });
            // Where a part went (or a copy came from), once it is done; else what is happening to it.
            std::string said = lines[i].empty() ? stateWords(states[i]) : lines[i];
            if (states[i] == State::done && uninstalling_ && i < runCopies_.size())
                said = "moved to the Trash from " + engine_.shown(runCopies_[i].path.parent_path());
            else if (states[i] == State::done && !uninstalling_ && i < runPlan_.steps.size())
                said = engine_.shown(runPlan_.steps[i].folder);
            else if (states[i] == State::skipped && !uninstalling_ && i < runPlan_.steps.size() && said.rfind("already", 0) == 0)
                said = engine_.shown(runPlan_.steps[i].folder) + " (already up to date)";
            Line l{ states[i] == State::failed ? Line::Kind::error : Line::Kind::faint,
                    fit(said, chat::kSmall, kInnerW - 262.0f), { kX0 + 262.0f, y, kInnerW - 262.0f, row } };
            if (l.text != said)
                l.whole = said;
            L.lines.push_back(std::move(l));
            y += row;
        }
        // The notes the run started with (what is open), under the steps, as many as there is room for.
        y += 10.0f;
        noteLines(L, runNotes_, y, static_cast<int>(std::floor((kFooterY - 10.0f - y) / 15.0f)));
        const bool stopping = cancel_.load();
        footer(L, {}, { { Act::stop, 0, Control::Kind::secondary, {}, stopping ? "Stopping..." : "Stop",
                          "Stop after the step running", {}, false, !stopping && !admin, 0 } });
    }

    void Panel::layoutDone(Layout& L) const
    {
        const Report& r = report_;
        const Payload& pay = engine_.payload();
        std::string title;
        if (uninstalling_)
            title = r.ok() ? "Moved to the Trash" : r.count(State::done) > 0 ? "Some copies were moved" : "Nothing was moved";
        else
            title = r.ok() ? (r.count(State::done) > 0 ? "Installed" : "Nothing needed installing")
                           : r.count(State::done) > 0 ? "Installed, but not everything" : "Nothing was installed";
        L.lines.push_back({ Line::Kind::title, title, { kX0, 34.0f, kInnerW, 22.0f } });
        float y = 60.0f;
        if (!r.error.empty())
        {
            for (const std::string& l : wrapped(r.error, chat::kSmall, kInnerW))
            {
                L.lines.push_back({ Line::Kind::error, l, { kX0, y, kInnerW, 16.0f } });
                y += 16.0f;
            }
        }
        else
        {
            const bool left = std::any_of(r.lines.begin(), r.lines.end(), [](const std::string& l) {
                return l.rfind("left as it was", 0) == 0;
            });
            const std::string sub = uninstalling_ ? "Each copy is in the Trash: to undo, drag it back to its folder (above)."
                                    : left        ? "Here is what went where; a part that could not go is left as it was."
                                                  : "Here is what went where.";
            L.lines.push_back({ Line::Kind::subtitle, fit(sub, chat::kSmall, kInnerW), { kX0, y, kInnerW, 16.0f } });
            y += 16.0f;
        }
        y += 14.0f;

        // Each product's rows (the steps, or the copies, that are its) and actions (the app opened, the extension added
        // to Claude Desktop, in a row under its parts).
        struct Row
        {
            Part part;
            State state;
            std::string line;
            fs::path target;
        };
        struct Block
        {
            std::size_t product;
            std::vector<Row> rows;
            std::vector<Control> actions;
        };
        std::vector<Block> blocks;
        bool claudeNote = false, plugins = false;
        const fs::path claude = app_.claudeDesktop();
        for (std::size_t p = 0; p < pay.products.size(); ++p)
        {
            const Product& pr = pay.products[p];
            Block b{ p, {}, {} };
            if (uninstalling_)
            {
                for (std::size_t i = 0; i < runCopies_.size() && i < r.states.size(); ++i)
                    if (runCopies_[i].key == pr.key)
                        b.rows.push_back({ runCopies_[i].part, r.states[i], r.lines[i], runCopies_[i].path });
            }
            else
            {
                for (std::size_t i = 0; i < runPlan_.steps.size() && i < r.states.size(); ++i)
                    if (runPlan_.steps[i].key == pr.key)
                        b.rows.push_back({ runPlan_.steps[i].part, r.states[i], r.lines[i], runPlan_.steps[i].target });
            }
            if (b.rows.empty())
                continue;
            for (const Row& row : b.rows)
            {
                plugins = plugins || ((row.part == Part::au || row.part == Part::vst3) && row.state == State::done);
                const bool installed = row.state == State::done
                                       || (row.state == State::skipped && row.line.rfind("already", 0) == 0);
                if (uninstalling_ || !installed)
                    continue;
                if (row.part == Part::app)
                    b.actions.push_back({ Act::openApp, static_cast<int>(p), Control::Kind::small, {}, "Open " + pr.name,
                                          "Open " + pr.name, {}, false, true, idOf(Act::openApp, static_cast<int>(p)) });
                else if (row.part == Part::extension)
                {
                    b.actions.push_back({ Act::addToClaude, static_cast<int>(p), Control::Kind::small, {},
                                          "Add to Claude Desktop", "Add " + pr.name + "'s extension to Claude Desktop",
                                          claude.empty() ? "Claude Desktop is not installed"
                                                         : "Opens it with Claude Desktop, which asks to add it",
                                          false, !claude.empty(), idOf(Act::addToClaude, static_cast<int>(p)) });
                    claudeNote = claudeNote || claude.empty();
                }
            }
            blocks.push_back(std::move(b));
        }
        const bool adding = std::any_of(blocks.begin(), blocks.end(), [](const Block& b) {
            return std::any_of(b.actions.begin(), b.actions.end(), [](const Control& c) { return c.act == Act::addToClaude; });
        });

        // The notes: the DAW's rescan, Claude Desktop, the log. After a failure, the reason a button is off and the log
        // come first.
        std::string rescan, claudeHow, log;
        if (plugins)
            rescan = std::string("Rescan plugins in your DAW, or restart it, ")
                     + (uninstalling_ ? "so that it forgets them" : "to see them")
                     + (r.registrarRestarted ? " (macOS's list of Audio Units is fresh already)." : ".");
        if (claudeNote)
            claudeHow = "Claude Desktop is not installed: get it at claude.ai/download, then double-click the .mcpb.";
        else if (!uninstalling_ && !claude.empty() && adding)
            claudeHow = "Add to Claude Desktop opens the extension with Claude Desktop, which asks before adding it.";
        if (!engine_.places().log.empty())
            log = "The log of what went where: " + engine_.shown(engine_.places().log);
        std::vector<std::vector<std::string>> notes;
        for (const std::string& n : r.ok() ? std::vector<std::string>{ rescan, claudeHow, log }
                                           : std::vector<std::string>{ claudeHow, log, rescan })
            if (!n.empty())
                notes.push_back(wrapped(n, chat::kSmall, kInnerW));
        float notesH = 0.0f;
        for (const auto& n : notes)
            notesH += 15.0f * static_cast<float>(n.size());

        // The cards: roomy, or closed up when the notes would not all fit below them.
        struct Metrics
        {
            float head, row, actions, pillY, gap;
        };
        constexpr Metrics kRoomy{ 30.0f, 18.0f, 32.0f, 6.0f, 12.0f }, kTight{ 26.0f, 16.0f, 30.0f, 4.0f, 10.0f };
        const auto cardsH = [&](const Metrics& m) {
            float h = 0.0f;
            for (const Block& b : blocks)
                h += m.head + m.row * static_cast<float>(b.rows.size()) + (b.actions.empty() ? 0.0f : m.actions) + 12.0f + m.gap;
            return h;
        };
        const float notesTop = kFooterY - 8.0f - notesH - 2.0f;
        const Metrics& m = y + cardsH(kRoomy) <= notesTop ? kRoomy : kTight;

        std::vector<Control> actions;
        for (Block& b : blocks)
        {
            const Product& pr = pay.products[b.product];
            const float blockH = m.head + m.row * static_cast<float>(b.rows.size()) + (b.actions.empty() ? 0.0f : m.actions)
                                 + 12.0f;
            L.cards.push_back({ kX0 - 14.0f, y - 6.0f, kInnerW + 28.0f, blockH });
            L.marks.push_back({ { kX0, y + 0.5f * (m.head - 30.0f), logo::kTile, logo::kTile }, pr.instrument(), 1.0f });
            L.lines.push_back({ Line::Kind::strong, fit(pr.name, chat::kStrong, kInnerW - 36.0f),
                                { kX0 + 36.0f, y + 4.0f + 0.5f * (m.head - 30.0f), kInnerW - 36.0f, 18.0f } });
            float ry = y + m.head;
            for (const Row& row : b.rows)
            {
                L.glyphs.push_back({ { kX0 + 8.0f, ry + 0.5f * m.row }, row.state });
                L.lines.push_back({ Line::Kind::label, partName(row.part), { kX0 + 24.0f, ry, 150.0f, m.row } });
                // Where it is, for a part installed or up to date; else what happened to it.
                std::string said;
                if (row.state == State::done)
                    said = engine_.shown(row.target.parent_path());
                else if (!uninstalling_ && row.state == State::skipped && row.line.rfind("already", 0) == 0)
                    said = engine_.shown(row.target.parent_path()) + " (already up to date)";
                else
                    said = row.line.empty() ? stateWords(row.state) : row.line;
                Line l{ row.state == State::failed ? Line::Kind::error : Line::Kind::faint,
                        fit(said, chat::kSmall, kInnerW - 180.0f), { kX0 + 180.0f, ry, kInnerW - 180.0f, m.row } };
                if (l.text != said)
                    l.whole = said;
                L.lines.push_back(std::move(l));
                ry += m.row;
            }
            float ax = kX0 + 24.0f;
            for (Control& c : b.actions)
            {
                const float w = pillW(c.label, chat::kSmall);
                c.r = { ax, ry + m.pillY, w, 24.0f };
                ax += w + 8.0f;
                actions.push_back(c);
            }
            y += blockH + m.gap;
        }
        for (Control& c : actions)
            L.controls.push_back(c);

        // The notes, each whole, those that fit (a long one left out lets a shorter one after it in).
        y += 2.0f;
        for (const auto& n : notes)
        {
            if (y + 15.0f * static_cast<float>(n.size()) > kFooterY - 8.0f)
                continue;
            for (const std::string& l : n)
            {
                L.lines.push_back({ Line::Kind::note, l, { kX0, y, kInnerW, 15.0f } });
                y += 15.0f;
            }
        }
        footer(L, { { Act::back, 0, Control::Kind::secondary, {}, "Back",
                      uninstalling_ ? "Back to the copies installed" : "Back to the choice of parts", {}, false, true, 0 } },
               { { Act::finish, 0, Control::Kind::primary, {}, "Done", "Done: quit the installer", {}, false, true, 0 } });
    }

    void Panel::layoutUninstall(Layout& L) const
    {
        const Payload& pay = engine_.payload();
        L.lines.push_back({ Line::Kind::title, "Uninstall", { kX0, 34.0f, kInnerW, 22.0f } });
        L.lines.push_back({ Line::Kind::subtitle, "Every copy found on this Mac; the ones switched on go to the Trash.",
                            { kX0, 60.0f, kInnerW, 16.0f } });
        int n = 0, heads = 0;
        for (const Product& pr : pay.products)
        {
            const auto k = std::count_if(copies_.begin(), copies_.end(), [&](const Copy& c) { return c.key == pr.key; });
            n += static_cast<int>(k);
            heads += k > 0 ? 1 : 0;
        }
        // The notes first: an app of theirs or a host that is open.
        constexpr float kNotesY = 84.0f;
        const int room = static_cast<int>(std::floor(
            (kFooterY - 16.0f - kNotesY - 14.0f - static_cast<float>(heads) * 42.0f - 18.0f * static_cast<float>(n)) / 15.0f));
        const int placed = noteLines(L, copyNotes_, kNotesY, room);
        float y = placed == 0 ? 92.0f : kNotesY + 15.0f * static_cast<float>(placed) + 14.0f;
        if (n == 0)
        {
            std::vector<std::string> names;
            for (const Product& pr : pay.products)
                names.push_back(pr.name);
            std::string list;
            for (std::size_t i = 0; i < names.size(); ++i)
                list += (i == 0 ? "" : i + 1 == names.size() ? " or " : ", ") + names[i];
            for (const std::string& l : wrapped("No part of " + list + " is installed on this Mac.", chat::kBody, kInnerW))
            {
                L.lines.push_back({ Line::Kind::body, l, { kX0, y, kInnerW, chat::kBodyLine } });
                y += chat::kBodyLine;
            }
        }
        const float avail = kFooterY - 16.0f - y - static_cast<float>(heads) * 42.0f;
        const float row = std::clamp(avail / std::max(1.0f, static_cast<float>(n)), 18.0f, 24.0f);
        int on = 0;
        for (const Product& pr : pay.products)
        {
            std::vector<std::size_t> mine;
            for (std::size_t i = 0; i < copies_.size(); ++i)
                if (copies_[i].key == pr.key)
                    mine.push_back(i);
            if (mine.empty())
                continue;
            const float blockH = 30.0f + row * static_cast<float>(mine.size()) + 8.0f;
            L.cards.push_back({ kX0 - 14.0f, y - 6.0f, kInnerW + 28.0f, blockH });
            L.marks.push_back({ { kX0, y, logo::kTile, logo::kTile }, pr.instrument(), 1.0f });
            L.lines.push_back({ Line::Kind::strong, pr.name, { kX0 + 36.0f, y + 4.0f, 260.0f, 18.0f } });
            float ry = y + 30.0f;
            for (const std::size_t i : mine)
            {
                const Copy& c = copies_[i];
                Control k;
                k.act = Act::copy;
                k.index = static_cast<int>(i);
                k.kind = Control::Kind::toggle;
                k.r = { kX0, ry, 208.0f, row - 4.0f };
                k.label = partName(c.part);
                k.title = "Move " + pr.name + "'s " + partName(c.part) + " in " + engine_.shown(c.path.parent_path())
                          + " to the Trash";
                k.on = copyOn_[i] && !c.link;
                k.enabled = !c.link;
                k.id = idOf(Act::copy, k.index);
                const std::string where = engine_.shown(c.path.parent_path()) + (c.version.empty() ? "" : " (" + c.version + ")")
                                          + (c.link ? ": a link, left as it is" : c.trashable ? "" : ": asks for a password");
                k.help = where;
                L.controls.push_back(k);
                on += k.on ? 1 : 0;
                L.lines.push_back({ c.link ? Line::Kind::error : Line::Kind::faint, fit(where, chat::kSmall, kInnerW - 220.0f),
                                    { kX0 + 220.0f, ry, kInnerW - 220.0f, row - 4.0f } });
                ry += row;
            }
            y += blockH + 12.0f;
        }
        footer(L, {}, { { Act::back, 0, Control::Kind::secondary, {}, "Back", "Back to the choice of parts", {}, false, true, 0 },
                        { Act::trash, 0, Control::Kind::primary, {}, "Move to Trash", "Move the copies switched on to the Trash",
                          {}, false, on > 0, 0 } });
    }

    std::vector<Panel::Control> Panel::controls() const
    {
        return layout().controls;
    }

    std::vector<Panel::Line> Panel::lines() const
    {
        return layout().lines;
    }

    // ---- actions -----------------------------------------------------------------------------------------------------

    void Panel::act(const Control& c)
    {
        if (!c.enabled)
            return;
        const auto& products = engine_.payload().products;
        switch (c.act)
        {
            case Act::next:        show(Page::choose); break;
            case Act::back:
                show(page_ == Page::uninstall                  ? Page::choose
                     : page_ == Page::done && uninstalling_    ? Page::uninstall
                     : page_ == Page::done                     ? Page::choose
                                                               : Page::welcome);
                break;
            case Act::install:     startInstall(); break;
            case Act::toUninstall: show(Page::uninstall); break;
            case Act::part:
            {
                const std::size_t p = static_cast<std::size_t>(c.index / 4);
                const auto part = static_cast<Part>(c.index % 4);
                if (p < products.size())
                    choose(products[p].key, part, !chosen(products[p].key, part));
                break;
            }
            case Act::scope:       setScope(c.index == 0 ? Scope::user : Scope::system); break;
            case Act::openApp:
            case Act::addToClaude:
            {
                const std::size_t p = static_cast<std::size_t>(c.index);
                if (p >= products.size())
                    break;
                const Part part = c.act == Act::openApp ? Part::app : Part::extension;
                for (const Step& s : runPlan_.steps)
                    if (s.key == products[p].key && s.part == part)
                    {
                        if (c.act == Act::openApp)
                            app_.openApp(s.target);
                        else if (!app_.claudeDesktop().empty())
                            app_.addToClaude(s.target);
                    }
                break;
            }
            case Act::finish:      app_.quit(); break;
            case Act::copy:
                if (c.index >= 0 && static_cast<std::size_t>(c.index) < copyOn_.size())
                {
                    const auto i = static_cast<std::size_t>(c.index);
                    copyOn_[i] = !copyOn_[i];
                    if (copyOn_[i])
                        copyOff_.erase(copies_[i].path.string());
                    else
                        copyOff_.insert(copies_[i].path.string());
                }
                break;
            case Act::trash:       startUninstall(); break;
            case Act::stop:        cancel_.store(true); break;
        }
        changed();
    }

    bool Panel::press(Act a, int index)
    {
        for (const Control& c : controls())
            if (c.act == a && c.index == index)
            {
                if (!c.enabled)
                    return false;
                ring_ = false;                           // as a click
                focus_ = { c.act, c.index };
                act(c);
                return true;
            }
        return false;
    }

    int Panel::focusIndex(const std::vector<Control>& cs) const
    {
        for (std::size_t i = 0; i < cs.size(); ++i)
            if (cs[i].act == focus_.act && cs[i].index == focus_.index)
                return static_cast<int>(i);
        return -1;
    }

    const Panel::Control* Panel::defaultButton(const std::vector<Control>& cs) const
    {
        for (const Control& c : cs)
            if (c.kind == Control::Kind::primary)
                return &c;
        return nullptr;
    }

    // ---- input -------------------------------------------------------------------------------------------------------

    void Panel::pointerMove(const funkgui::PointerEvent& e)
    {
        const funkgui::Point p{ e.x, e.y };
        const auto cs = controls();
        const auto over = [&](funkgui::Point q) {
            for (const Control& c : cs)
                if (c.r.contains(q))
                    return c.id;
            return 0u;
        };
        const bool was = hasPointer_;
        const uint32_t before = was ? over(pointer_) : 0u;
        pointer_ = p;
        hasPointer_ = true;
        if (!was || over(p) != before)
            changed();
    }

    void Panel::pointerExit()
    {
        hasPointer_ = false;
        changed();
    }

    void Panel::pointerDown(const funkgui::PointerEvent& e)
    {
        const funkgui::Point p{ e.x, e.y };
        pointer_ = p;
        hasPointer_ = true;
        if (e.popup)
            return;
        ring_ = false;
        for (const Control& c : controls())
            if (c.r.contains(p))
            {
                // A click as a page appears is the last page's (a double-click on Continue is not Install's).
                if (c.enabled && now_ - shownAt_ >= kHold)
                {
                    focus_ = { c.act, c.index };
                    act(c);
                }
                return;
            }
        if (p.y < kTopStrip || (p.x < kSide && p.y < 140.0f))
            app_.dragWindow(e.clicks);                   // the window's own drag, on a press where nothing is
    }

    bool Panel::key(const funkgui::KeyEvent& e)
    {
        using funkgui::Key;
        const auto cs = controls();
        const int i = focusIndex(cs);
        switch (e.key)
        {
            case Key::enter:
                // One press, one button: a held Return's repeats, and a Return as a page appears, press nothing.
                if (returnLatched_ || now_ - shownAt_ < kHold)
                    return true;
                if (const Control* d = defaultButton(cs); d != nullptr && d->enabled)
                {
                    const Control c = *d;
                    returnLatched_ = true;
                    act(c);
                }
                return true;
            case Key::escape:
                if (page_ == Page::choose || page_ == Page::uninstall || page_ == Page::done)
                    press(Act::back);
                else if (page_ == Page::installing)
                    press(Act::stop);
                return true;
            case Key::tab:
            case Key::down:
            case Key::up:
            {
                std::vector<int> stops;
                for (std::size_t k = 0; k < cs.size(); ++k)
                    if (cs[k].enabled)
                        stops.push_back(static_cast<int>(k));
                if (stops.empty())
                    return true;
                const bool back = e.key == Key::up || (e.key == Key::tab && e.mods.shift);
                const auto at = std::find(stops.begin(), stops.end(), i);
                std::size_t next = 0;
                if (at == stops.end())
                    next = back ? stops.size() - 1 : 0;
                else
                {
                    const auto k = static_cast<std::size_t>(at - stops.begin());
                    next = back ? (k + stops.size() - 1) % stops.size() : (k + 1) % stops.size();
                }
                const Control& c = cs[static_cast<std::size_t>(stops[next])];
                focus_ = { c.act, c.index };
                ring_ = true;
                changed();
                return true;
            }
            case Key::left:
            case Key::right:
                if (i >= 0 && cs[static_cast<std::size_t>(i)].act == Act::scope)
                {
                    const int to = e.key == Key::left ? 0 : 1;
                    setScope(to == 0 ? Scope::user : Scope::system);
                    focus_ = { Act::scope, to };
                    ring_ = true;
                    changed();
                }
                return true;
            case Key::space:
                if (i >= 0)
                {
                    const Control c = cs[static_cast<std::size_t>(i)];
                    ring_ = true;
                    act(c);
                }
                return true;
            case Key::character:
            case Key::pageUp:
            case Key::pageDown:
            case Key::home:
            case Key::end:
            case Key::backspace:
            case Key::del:
                return false;
        }
        return false;
    }

    funkgui::Cursor Panel::cursor() const
    {
        if (!hasPointer_)
            return funkgui::Cursor::normal;
        for (const Control& c : controls())
            if (c.enabled && c.r.contains(pointer_))
                return funkgui::Cursor::pointingHand;
        return funkgui::Cursor::normal;
    }

    // ---- accessibility -----------------------------------------------------------------------------------------------

    void Panel::accessibility(std::vector<funkgui::A11yItem>& out) const
    {
        const Layout L = layout();
        uint32_t n = 0;
        {
            static constexpr const char* kSteps[] = { "Welcome", "Choose", "Install", "Done" };
            const int at = page_ == Page::welcome ? 0 : page_ == Page::choose ? 1 : page_ == Page::done ? 3 : 2;
            funkgui::A11yItem it;
            it.id = kLineIds + n++;
            it.role = funkgui::A11yRole::staticText;
            it.bounds = { 24.0f, 150.0f, kSide - 48.0f, 120.0f };
            it.title = std::string("Step ") + std::to_string(at + 1) + " of 4: "
                       + (at == 2 && (uninstalling_ || page_ == Page::uninstall) ? "Uninstall" : kSteps[at]);
            it.readOnly = true;
            out.push_back(std::move(it));
        }
        for (const Line& l : L.lines)
        {
            funkgui::A11yItem it;
            it.id = kLineIds + n++;
            it.role = funkgui::A11yRole::staticText;
            it.bounds = l.r;
            const std::string& text = l.whole.empty() ? l.text : l.whole;
            it.title = l.kind == Line::Kind::error ? "Error: " + text : text;
            it.readOnly = true;
            out.push_back(std::move(it));
        }
        if (L.fraction >= 0.0f)
        {
            funkgui::A11yItem it;
            it.id = kProgressId;
            it.role = funkgui::A11yRole::progressBar;
            it.bounds = L.progress;
            it.title = "Progress";
            it.v = static_cast<double>(L.fraction);
            it.value = std::to_string(static_cast<int>(std::lround(L.fraction * 100.0f))) + "%";
            it.readOnly = true;
            out.push_back(std::move(it));
        }
        for (const Control& c : L.controls)
        {
            funkgui::A11yItem it;
            it.id = c.id;
            it.bounds = c.r;
            it.title = c.title;
            it.help = c.help;
            it.enabled = c.enabled;
            switch (c.kind)
            {
                case Control::Kind::toggle:
                    it.role = funkgui::A11yRole::toggleButton;
                    it.checkable = true;
                    it.checked = c.on;
                    break;
                case Control::Kind::radio:
                    it.role = funkgui::A11yRole::radioButton;
                    it.checkable = true;
                    it.checked = c.on;
                    break;
                case Control::Kind::primary:
                case Control::Kind::secondary:
                case Control::Kind::small:
                    it.role = funkgui::A11yRole::button;
                    break;
            }
            out.push_back(std::move(it));
        }
    }

    void Panel::a11yAction(uint32_t id, funkgui::A11yAction action, double value)
    {
        (void) value;
        for (const Control& c : controls())
            if (c.id == id)
            {
                focus_ = { c.act, c.index };
                if (action == funkgui::A11yAction::press || action == funkgui::A11yAction::toggle)
                    act(c);
                changed();
                return;
            }
    }

    std::string Panel::a11yDump() const
    {
        std::vector<funkgui::A11yItem> items;
        accessibility(items);
        std::string s;
        for (const funkgui::A11yItem& it : items)
            s += funkgui::a11yDumpLine(it) + "\n";
        return s;
    }

    // ---- drawing -----------------------------------------------------------------------------------------------------

    namespace
    {
        // A product's mark in its tile, in its kind's accent: at scale 1 the drawing hinted for the canvas (as the top
        // bar draws it), larger the 2x drawing scaled.
        void drawMark(funkgui::Canvas& c, const funkgui::Theme& th, const funkgui::Rect& r, bool instrument, float scale)
        {
            const funkgui::Col accent = ui::kindTheme(th, instrument).accent;
            const float tx = c.snapX(r.x), ty = c.snapY(r.y);
            c.rrect(tx, ty, logo::kTile * scale, logo::kTile * scale, logo::kTileRadius * scale, accent.withAlpha(logo::kTileAlpha));
            const logo::Mark& m = logo::mark(instrument, scale == 1.0f ? c.dpi() : 2.0f);
            std::array<float, logo::kMaxPoints> xs{}, tops{}, bottoms{};
            for (std::size_t k = 0; k < static_cast<std::size_t>(m.wave.n); ++k)
            {
                xs[k] = tx + m.wave.xs[k] * scale;
                tops[k] = ty + m.wave.tops[k] * scale;
                bottoms[k] = ty + m.wave.bottoms[k] * scale;
            }
            c.areaStrip(xs.data(), m.wave.n, tops.data(), bottoms.data(), 0.0f, accent);
            const logo::Stem& st = m.caret;
            if (st.round)
            {
                const float w = (st.x1 - st.x0) * scale, x = tx + 0.5f * (st.x0 + st.x1) * scale;
                c.segment(x, ty + st.y0 * scale + 0.5f * w, x, ty + st.y1 * scale - 0.5f * w, w, accent);
            }
            else
                c.rrect(tx + st.x0 * scale, ty + st.y0 * scale, (st.x1 - st.x0) * scale, (st.y1 - st.y0) * scale, 0.0f, accent);
        }

        // The errors' red: neither product's accent (the effect's orange, the instrument's blue; FunkGui's signal, which
        // ChatStyle's `danger` is, is a blue of the instrument's family), as Theme.h asks of an error.
        funkgui::Col errorInk(const funkgui::Theme& th)
        {
            const bool dark = th.ground.r < 128;
            return dark ? funkgui::Col{ 0xFF, 0x5D, 0x6E } : funkgui::Col{ 0xC4, 0x2B, 0x3C };
        }

        void drawGlyph(funkgui::Canvas& c, const chat::Palette& p, funkgui::Col error, funkgui::Point at, State s, float spin)
        {
            constexpr float r = 7.0f;
            const funkgui::Col none = p.card.withAlpha(0.0f);
            switch (s)
            {
                case State::waiting:
                case State::notRun:
                    c.disc(at.x, at.y, r, none, 1.2f, p.faint);
                    break;
                case State::working:
                {
                    c.disc(at.x, at.y, r, none, 1.2f, p.cardEdge);
                    const float a0 = spin * 6.0f;
                    c.arc({ at.x, at.y }, r - 0.6f, a0, a0 + 2.2f, 2.0f, p.accent);
                    break;
                }
                case State::done:
                    c.disc(at.x, at.y, r, p.accent);
                    chat::drawIcon(c, chat::Icon::check, at.x, at.y + 0.5f, 9.0f, p.onAccent);
                    break;
                case State::skipped:
                    c.disc(at.x, at.y, r, none, 1.2f, p.faint);
                    c.segment(at.x - 3.0f, at.y, at.x + 3.0f, at.y, 1.4f, p.faint);
                    break;
                case State::failed:
                    c.disc(at.x, at.y, r, error);
                    chat::drawIcon(c, chat::Icon::cross, at.x, at.y, 8.0f, p.onAccent);
                    break;
            }
        }
    }

    void Panel::draw(funkgui::Canvas& c, const funkgui::Theme& th)
    {
        const chat::Palette p = chat::palette(th);
        const Layout L = layout();
        c.rrect(0.0f, 0.0f, kW, kH, 0.0f, th.ground);

        // The sidebar, its steps.
        c.rrect(0.0f, 0.0f, kSide, kH, 0.0f, p.card);
        c.hairlineV(kSide, 0.0f, kH, p.cardEdge);
        {
            const bool un = page_ == Page::uninstall || (uninstalling_ && (page_ == Page::installing || page_ == Page::done));
            const std::array<const char*, 4> steps{ "Welcome", "Choose", un ? "Uninstall" : "Install", "Done" };
            const int at = page_ == Page::welcome ? 0 : page_ == Page::choose ? 1 : page_ == Page::done ? 3 : 2;
            float y = 162.0f;
            for (int k = 0; k < 4; ++k)
            {
                const float cy = y + 10.0f;
                if (k < at)
                {
                    c.disc(32.0f, cy, 7.0f, p.accent.withAlpha(0.22f));
                    chat::drawIcon(c, chat::Icon::check, 32.0f, cy + 0.5f, 8.0f, p.accent);
                }
                else if (k == at)
                {
                    c.disc(32.0f, cy, 7.0f, p.accent);
                    c.disc(32.0f, cy, 2.5f, p.onAccent);
                }
                else
                    c.disc(32.0f, cy, 7.0f, p.card.withAlpha(0.0f), 1.2f, p.faint);
                if (k < 3)
                    c.segment(32.0f, cy + 9.5f, 32.0f, cy + 22.5f, 1.2f, k < at ? p.accent.withAlpha(0.5f) : p.cardEdge);
                const funkgui::TextStyle& st = k == at ? chat::kSmallStrong : chat::kSmall;
                c.text(steps[static_cast<std::size_t>(k)], 50.0f, c.capCentreTop(cy, st), st,
                       k == at ? p.text : k < at ? p.soft : p.faint);
                y += 32.0f;
            }
        }

        // Cards, rules, marks.
        for (const funkgui::Rect& r : L.cards)
            c.rrect(r.x, r.y, r.w, r.h, 14.0f, p.raised, 1.0f, p.cardEdge);
        for (const funkgui::Rect& r : L.rules)
            c.rrect(r.x, r.y, r.w, r.h, 0.5f * r.h, p.composer, 1.0f, p.composerEdge);
        for (const Layout::Mark& m : L.marks)
            drawMark(c, th, m.r, m.instrument, m.scale);

        // The texts.
        for (const Line& l : L.lines)
        {
            const funkgui::TextStyle& st = styleOf(l.kind);
            funkgui::Col ink = p.text;
            switch (l.kind)
            {
                case Line::Kind::hero:
                case Line::Kind::title:
                case Line::Kind::strong:
                case Line::Kind::label:    ink = p.text; break;
                case Line::Kind::subtitle:
                case Line::Kind::body:
                case Line::Kind::value:
                case Line::Kind::note:     ink = p.soft; break;
                case Line::Kind::faint:    ink = p.dim; break;
                case Line::Kind::error:    ink = errorInk(th); break;
                case Line::Kind::warn:     ink = p.text; break;
            }
            const float top = c.capCentreTop(l.r.centreY(), st);
            if (l.kind == Line::Kind::warn)
                chat::drawIcon(c, chat::Icon::alert, l.r.x - 11.0f, l.r.centreY(), 11.0f, p.accent);
            if (l.right)
                c.text(l.text.c_str(), l.r.right(), top, st, ink, funkgui::Align::right);
            else
                c.text(l.text.c_str(), l.r.x, top, st, ink);
        }

        // The progress bar, the steps' glyphs.
        if (L.fraction >= 0.0f)
        {
            const funkgui::Rect& r = L.progress;
            c.rrect(r.x, r.y, r.w, r.h, 0.5f * r.h, p.composer, 1.0f, p.composerEdge);
            const float w = std::max(r.h, r.w * std::clamp(L.fraction, 0.0f, 1.0f));
            if (L.fraction > 0.0f)
                c.rrect(r.x, r.y, w, r.h, 0.5f * r.h, p.accent);
        }
        for (const Layout::Glyph& g : L.glyphs)
            drawGlyph(c, p, errorInk(th), g.c, g.state, spin_);

        // The controls.
        const int focus = ring_ ? focusIndex(L.controls) : -1;
        for (std::size_t i = 0; i < L.controls.size(); ++i)
        {
            const Control& k = L.controls[i];
            const funkgui::Rect& r = k.r;
            const bool hover = hasPointer_ && k.enabled && r.contains(pointer_);
            const bool focused = static_cast<int>(i) == focus;
            const funkgui::Col clear = p.card.withAlpha(0.0f);
            switch (k.kind)
            {
                case Control::Kind::primary:
                {
                    const funkgui::Col fill = !k.enabled ? p.hover : hover ? funkgui::mix(p.accent, p.text, 0.12f) : p.accent;
                    c.rrect(r.x, r.y, r.w, r.h, 0.5f * r.h, fill);
                    c.text(k.label.c_str(), r.centreX(), c.capCentreTop(r.centreY(), chat::kSmallStrong), chat::kSmallStrong,
                           k.enabled ? p.onAccent : p.faint, funkgui::Align::centre);
                    break;
                }
                case Control::Kind::secondary:
                case Control::Kind::small:
                {
                    const funkgui::TextStyle& st = k.kind == Control::Kind::small ? chat::kSmall : chat::kSmallStrong;
                    c.rrect(r.x, r.y, r.w, r.h, 0.5f * r.h, hover ? p.hover : clear, 1.0f, p.composerEdge);
                    c.text(k.label.c_str(), r.centreX(), c.capCentreTop(r.centreY(), st), st,
                           !k.enabled ? p.faint : hover ? p.text : p.soft, funkgui::Align::centre);
                    break;
                }
                case Control::Kind::toggle:
                {
                    const float ty = r.centreY() - 0.5f * kSwitchH;
                    const funkgui::Col track = !k.enabled ? p.hover
                                             : k.on ? (hover ? funkgui::mix(p.accent, p.text, 0.12f) : p.accent)
                                                    : (hover ? p.hover : p.composer);
                    c.rrect(r.x, ty, kSwitchW, kSwitchH, 0.5f * kSwitchH, track, k.on && k.enabled ? 0.0f : 1.0f, p.composerEdge);
                    const float kx = k.on ? r.x + kSwitchW - 0.5f * kSwitchH : r.x + 0.5f * kSwitchH;
                    c.disc(kx, r.centreY(), 0.5f * kSwitchH - 2.5f, !k.enabled ? p.faint : k.on ? p.onAccent : p.soft);
                    c.text(fit(k.label, chat::kSmall, r.w - kSwitchW - 10.0f).c_str(), r.x + kSwitchW + 10.0f,
                           c.capCentreTop(r.centreY(), chat::kSmall), chat::kSmall, !k.enabled ? p.faint : k.on ? p.text : p.soft);
                    if (focused)
                        c.rrect(r.x - 2.0f, ty - 2.0f, kSwitchW + 4.0f, kSwitchH + 4.0f, 0.5f * kSwitchH + 2.0f, clear, 1.5f,
                                p.accent);
                    break;
                }
                case Control::Kind::radio:
                {
                    if (k.on)
                        c.rrect(r.x, r.y, r.w, r.h, 0.5f * r.h, p.accent);
                    else if (hover)
                        c.rrect(r.x, r.y, r.w, r.h, 0.5f * r.h, p.hover);
                    c.text(k.label.c_str(), r.centreX(), c.capCentreTop(r.centreY(), chat::kSmall), chat::kSmall,
                           k.on ? p.onAccent : hover ? p.text : p.soft, funkgui::Align::centre);
                    break;
                }
            }
            if (focused && k.kind != Control::Kind::toggle)
                c.rrect(r.x - 2.0f, r.y - 2.0f, r.w + 4.0f, r.h + 4.0f, 0.5f * r.h + 2.0f, clear, 1.5f, p.accent);
        }
    }
}
