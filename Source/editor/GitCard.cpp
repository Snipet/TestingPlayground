// Source/editor/GitCard.cpp: see GitCard.h.
#include "editor/GitCard.h"

#include "editor/ChatStyle.h"
#include "editor/Items.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace playground::ui
{
    namespace
    {
        constexpr float kBoxW = 660.0f, kBoxH = 520.0f;
        constexpr float kPad = 24.0f;
        constexpr float kButtonH = 32.0f;
        constexpr float kFieldH = 34.0f;
        constexpr float kRowH = 22.0f;                   // a fact's row, a commit's
        constexpr int kLineRows = 4;                     // the outcome's lines, at most
        constexpr uint32_t kBoxId = GitCard::kA11yBase, kRemoteId = kBoxId + 1, kBranchId = kBoxId + 2, kPrimaryId = kBoxId + 3,
                           kPullId = kBoxId + 4, kCheckId = kBoxId + 5, kShowId = kBoxId + 6, kDisconnectId = kBoxId + 7,
                           kCloseId = kBoxId + 8, kOtherId = kBoxId + 9, kLineId = kBoxId + 10, kCommitsId = kBoxId + 0x100;

        float pillW(const std::string& label) { return std::round(7.2f * static_cast<float>(label.size()) + 26.0f); }

        // `s` in lines of at most `maxW` px, words kept whole while they fit; a line break starts a line.
        std::vector<std::string> wrapped(const funkgui::Canvas& c, const std::string& s, const funkgui::TextStyle& st, float maxW)
        {
            std::vector<std::string> out;
            std::size_t from = 0;
            while (from <= s.size())
            {
                const std::size_t end = std::min(s.find('\n', from), s.size());
                const std::string para = s.substr(from, end - from);
                std::string line;
                std::size_t at = 0;
                while (at <= para.size())
                {
                    const std::size_t sp = std::min(para.find(' ', at), para.size());
                    const std::string word = para.substr(at, sp - at);
                    const std::string tried = line.empty() ? word : line + " " + word;
                    if (!line.empty() && c.textWidth(tried.c_str(), st) > maxW)
                    {
                        out.push_back(line);
                        line = word;
                    }
                    else
                        line = tried;
                    at = sp + 1;
                }
                out.push_back(line);
                from = end + 1;
            }
            return out;
        }
    }

    void GitCard::open(const std::string& project)
    {
        if (project.empty())
        {
            openAdd();
            return;
        }
        open_ = true;
        mode_ = Mode::project;
        project_ = project;
        remote_.set({}, kRemoteLength);
        branch_.set("main", kBranchLength);
        branch_.allSelected = false;
        focus_ = Field::remote;
        askDisconnect_ = false;
        line_.clear();
        failed_ = false;
        watching_ = false;
        read();
        ++revision_;
    }

    void GitCard::openAdd()
    {
        const bool wasOpen = open_;
        open_ = true;
        mode_ = Mode::add;
        if (!wasOpen)
            project_.clear();
        remote_.set({}, kRemoteLength);
        branch_.set({}, kBranchLength);
        focus_ = Field::remote;
        askDisconnect_ = false;
        line_.clear();
        failed_ = false;
        read();
        ++revision_;
    }

    void GitCard::close()
    {
        open_ = false;
        askDisconnect_ = false;
        ++revision_;
    }

    void GitCard::setFields(const std::string& remote, const std::string& branch)
    {
        remote_.set(remote, kRemoteLength);
        branch_.set(branch, kBranchLength);
        ++revision_;
    }

    void GitCard::read()
    {
        polled_ = port_.gitPolled();
        state_ = port_.gitState();
        link_ = mode_ == Mode::project ? port_.gitLink(project_) : GitLink{};
    }

    bool GitCard::poll()
    {
        if (!open_ && !watching_)
            return false;
        if (port_.gitPolled() == polled_)
            return false;
        const uint64_t endedBefore = state_.ended;
        read();
        if (watching_ && !state_.busy && state_.ended >= mine_ && state_.ended != endedBefore)
        {
            // The job this card started has ended: its outcome, once.
            watching_ = false;
            line_ = state_.words;
            failed_ = !state_.ok && !state_.cancelled;
            // The status line's words: what was done to which project, never git's (a path, an address).
            const std::string what = state_.op == GitOp::connect ? "LINKED"
                                   : state_.op == GitOp::push    ? (state_.behind ? "NOT PUSHED (PULL FIRST)" : "PUSHED")
                                   : state_.op == GitOp::pull    ? (state_.diverged ? "BOTH SIDES CHANGED" : "PULLED")
                                   : state_.op == GitOp::check   ? "CHECKED"
                                   : state_.op == GitOp::add     ? "ADDED"
                                   : state_.op == GitOp::keepMine ? "KEPT YOURS"
                                   : state_.op == GitOp::takeTheirs ? "TOOK THEIRS"
                                   : state_.op == GitOp::theirs  ? "READ THEIR SIDE"
                                                                 : "LOOKED FOR GIT";
            const std::string who = upper(asciiText(ownName(state_.op == GitOp::add ? state_.added : project_)));
            notice_ = state_.cancelled ? "GIT: CANCELLED"
                    : state_.ok        ? "GIT: " + what + (who.empty() ? std::string() : ": " + who)
                    : state_.behind || state_.diverged ? "GIT: " + what + ": " + who
                                                       : "GIT: FAILED (ITS CARD SAYS WHY)";
            if (state_.op == GitOp::theirs && state_.ok)
                compare_ = project_;                     // part 3: COMPARE, mine against theirs (the Panel opens it)
            if (state_.op == GitOp::add && state_.ok && !state_.added.empty())
            {
                // The project it made, shown as linked.
                mode_ = Mode::project;
                project_ = state_.added;
                link_ = port_.gitLink(project_);
            }
        }
        ++revision_;
        return true;
    }

    std::string GitCard::takeCompare()
    {
        std::string c;
        c.swap(compare_);
        return c;
    }

    std::string GitCard::takeNotice()
    {
        std::string n;
        n.swap(notice_);
        return n;
    }

    void GitCard::focusOn(Field f)
    {
        focus_ = f;
        funkgui::text::LineEdit& e = field(f);
        e.allSelected = false;
        e.caret = static_cast<int>(e.buffer.size());
        ++revision_;
    }

    void GitCard::start(const GitJob& job, const std::string& doing)
    {
        std::string why;
        askDisconnect_ = false;
        if (!port_.startGit(job, why))
        {
            line_ = "Not started: " + why + ".";
            failed_ = true;
            ++revision_;
            return;
        }
        watching_ = true;
        read();
        mine_ = state_.ended + 1;
        line_ = doing;
        failed_ = false;
        ++revision_;
    }

    std::string GitCard::stateText() const
    {
        if (!link_.linked)
            return {};
        const int push = link_.toPush + std::max(0, link_.ahead);
        std::string s;
        if (link_.diverged)
            return "Both sides have changed since they last met";
        if (link_.behind > 0)
            s = std::to_string(link_.behind) + (link_.behind == 1 ? " commit to pull" : " commits to pull");
        if (push > 0)
            s += (s.empty() ? "" : ", ") + std::to_string(push) + (push == 1 ? " version to push" : " versions to push");
        if (s.empty())
            s = link_.checked.empty() ? std::string("Not checked yet") : "Up to date, checked " + link_.checked;
        else if (!link_.checked.empty())
            s += " (checked " + link_.checked + ")";
        return s;
    }

    std::string GitCard::label(Button b) const
    {
        const bool linked = mode_ == Mode::project && link_.linked;
        const bool diverged = linked && link_.diverged;  // part 3: the three ways
        switch (b)
        {
            case Button::primary: return mode_ == Mode::add ? "Add" : diverged ? "Keep mine" : linked ? "Push" : "Connect";
            case Button::pull: return diverged ? "Take theirs" : linked ? "Pull" : "";
            case Button::check: return diverged ? "Compare..." : linked ? "Check" : "";
            case Button::show: return linked && !diverged ? "Show folder" : "";   // diverged: the three ways' room
            case Button::disconnect: return linked ? (askDisconnect_ ? "Disconnect it" : "Disconnect...") : "";
            case Button::close: return state_.busy && watching_ ? "Cancel" : "Close";
            case Button::other: return mode_ == Mode::add ? (project_.empty() ? "" : "Back") : linked ? "" : "Add from Git...";
            case Button::none: break;
        }
        return {};
    }

    bool GitCard::enabled(Button b) const
    {
        if (label(b).empty())
            return false;
        if (b == Button::close)
            return true;
        if (state_.busy)                                 // one job at a time: the buttons wait for it
            return false;
        if (b == Button::primary && fields())
            return !remote_.buffer.empty();
        return true;
    }

    GitCard::Places GitCard::places(float width, float height) const
    {
        const float w = std::min(kBoxW, width - 32.0f), h = std::min(kBoxH, height - 32.0f);
        Places at;
        at.box = { std::round((width - w) * 0.5f), std::round((height - h) * 0.45f), w, h };
        const float x0 = at.box.x + kPad, inner = w - 2.0f * kPad;
        const float by = at.box.bottom() - kPad - kButtonH;
        // The buttons: Close and the other (Add from Git... / Back, or Disconnect) at the left, the rest at the right.
        at.close = { x0, by, 84.0f, kButtonH };
        float right = at.box.right() - kPad;
        const auto fromRight = [&](funkgui::Rect& r, Button b) {
            const std::string l = label(b);
            if (l.empty())
                return;
            const float bw = std::max(84.0f, pillW(l));
            r = { right - bw, by, bw, kButtonH };
            right = r.x - 8.0f;
        };
        fromRight(at.primary, Button::primary);
        fromRight(at.pull, Button::pull);
        fromRight(at.check, Button::check);
        fromRight(at.show, Button::show);
        if (const std::string l = label(Button::other); !l.empty())
            at.other = { at.close.right() + 8.0f, by, std::max(84.0f, pillW(l)), kButtonH };
        if (const std::string l = label(Button::disconnect); !l.empty())
            at.disconnect = { at.close.right() + 8.0f, by, std::max(84.0f, pillW("Disconnect it")), kButtonH };
        at.line = { x0, by - 14.0f - static_cast<float>(kLineRows) * 16.0f, inner, static_cast<float>(kLineRows) * 16.0f };
        const float top = at.box.y + 100.0f;
        if (fields())
        {
            const float branchW = 170.0f;
            at.remote = { x0, top + 18.0f, inner - branchW - 12.0f, kFieldH };
            at.branch = { at.remote.right() + 12.0f, top + 18.0f, branchW, kFieldH };
        }
        else
        {
            const float commitsTop = top + 3.0f * kRowH + 30.0f;
            at.commits = { x0, commitsTop, inner, std::max(kRowH, std::min(8.0f * kRowH, at.line.y - 10.0f - commitsTop)) };
        }
        return at;
    }

    funkgui::Rect GitCard::rectOf(const Places& at, Button b) const
    {
        switch (b)
        {
            case Button::primary: return at.primary;
            case Button::pull: return at.pull;
            case Button::check: return at.check;
            case Button::show: return at.show;
            case Button::disconnect: return at.disconnect;
            case Button::close: return at.close;
            case Button::other: return at.other;
            case Button::none: break;
        }
        return {};
    }

    funkgui::Point GitCard::centreOf(Button b, float width, float height) const
    {
        const funkgui::Rect r = rectOf(places(width, height), b);
        return r.w > 0.0f ? funkgui::Point{ r.centreX(), r.centreY() } : funkgui::Point{ -1.0e4f, -1.0e4f };
    }

    void GitCard::press(Button b)
    {
        if (!enabled(b))
            return;
        const bool asked = askDisconnect_;
        askDisconnect_ = false;
        switch (b)
        {
            case Button::close:
                if (state_.busy && watching_)
                {
                    port_.cancelGit();
                    line_ = "Cancelling...";
                    failed_ = false;
                }
                else
                    close();
                break;
            case Button::other:
                if (mode_ == Mode::add)
                    open(project_);
                else
                    openAdd();
                break;
            case Button::primary:
                if (mode_ == Mode::add)
                    start({ GitOp::add, {}, remote_.buffer, branch_.buffer }, "Cloning " + remote_.buffer + "...");
                else if (!link_.linked)
                    start({ GitOp::connect, project_, remote_.buffer, branch_.buffer }, "Connecting...");
                else if (link_.diverged)
                    start({ GitOp::keepMine, project_, {}, {} }, "Keeping yours...");
                else
                    start({ GitOp::push, project_, {}, {} }, "Pushing to " + link_.shortRemote + "...");
                break;
            case Button::pull:
                if (link_.diverged)
                    start({ GitOp::takeTheirs, project_, {}, {} }, "Taking theirs...");
                else
                    start({ GitOp::pull, project_, {}, {} }, "Pulling from " + link_.shortRemote + "...");
                break;
            case Button::check:
                if (link_.diverged)
                    start({ GitOp::theirs, project_, {}, {} }, "Reading " + link_.shortRemote + "'s side...");
                else
                    start({ GitOp::check, project_, {}, {} }, "Checking " + link_.shortRemote + "...");
                break;
            case Button::show:
            {
                std::string why;
                if (!port_.showPath(link_.folder, why))
                {
                    line_ = "Not shown: " + why + ".";
                    failed_ = true;
                }
                break;
            }
            case Button::disconnect:
                if (!asked)
                {
                    askDisconnect_ = true;
                    line_ = "Disconnect from " + link_.shortRemote + "? The project stays, and so does its folder (" + link_.folder
                          + "): only the link goes. Disconnect it to go on.";
                    failed_ = false;
                }
                else
                {
                    std::string why;
                    const bool ok = port_.disconnectGit(project_, why);
                    line_ = why;
                    failed_ = !ok;
                    if (ok)
                        notice_ = "GIT: DISCONNECTED " + upper(asciiText(ownName(project_)));
                    read();
                }
                break;
            case Button::none: break;
        }
        ++revision_;
    }

    void GitCard::pointerDown(funkgui::Point p, float width, float height)
    {
        const Places at = places(width, height);
        if (!at.box.contains(p))
        {
            close();                                     // as Close: the Panel drops the press (a job goes on)
            return;
        }
        for (const Button b : { Button::primary, Button::pull, Button::check, Button::show, Button::disconnect, Button::close,
                                Button::other })
        {
            const funkgui::Rect r = rectOf(at, b);
            if (r.w > 0.0f && r.contains(p))
            {
                press(b);
                return;
            }
        }
        if (fields() && at.remote.contains(p))
            focusOn(Field::remote);
        else if (fields() && at.branch.contains(p))
            focusOn(Field::branch);
        if (askDisconnect_)
        {
            askDisconnect_ = false;                      // a press elsewhere withdraws the question
            line_.clear();
        }
        ++revision_;
    }

    void GitCard::key(const funkgui::KeyEvent& e, funkgui::HostServices* host, float /*width*/, float /*height*/)
    {
        using funkgui::Key;
        if (e.key == Key::escape)
        {
            if (askDisconnect_)
            {
                askDisconnect_ = false;
                line_.clear();
            }
            else
                close();
        }
        else if (e.key == Key::enter)
            press(Button::primary);
        else if (!fields())
            return;
        else if (e.key == Key::tab)
            focusOn(focus_ == Field::remote ? Field::branch : Field::remote);
        else
        {
            funkgui::text::LineEdit& f = field(focus_);
            const int most = focus_ == Field::remote ? kRemoteLength : kBranchLength;
            if (e.key == Key::character && e.mods.cmd && (e.ch == 'v' || e.ch == 'V'))
            {
                if (host != nullptr)
                    f.paste(host->clipboardText(), most);
            }
            else if (e.key == Key::character && e.mods.cmd && (e.ch == 'a' || e.ch == 'A'))
                f.allSelected = !f.buffer.empty();
            else
                f.key(e, most);
        }
        ++revision_;
    }

    funkgui::Cursor GitCard::cursor(funkgui::Point p, float width, float height) const
    {
        const Places at = places(width, height);
        for (const Button b : { Button::primary, Button::pull, Button::check, Button::show, Button::disconnect, Button::close,
                                Button::other })
            if (const funkgui::Rect r = rectOf(at, b); r.w > 0.0f && r.contains(p) && enabled(b))
                return funkgui::Cursor::pointingHand;
        return funkgui::Cursor::normal;
    }

    void GitCard::draw(funkgui::Canvas& c, const funkgui::Theme& th, float width, float height, funkgui::Point pointer,
                       bool hasPointer) const
    {
        const chat::Palette p = chat::palette(th);
        const Places at = places(width, height);
        const funkgui::Rect& box = at.box;
        c.rrect(0.0f, 0.0f, width, height, 0.0f, th.ground.withAlpha(0.68f));
        constexpr funkgui::Col black{ 0, 0, 0, 255 };
        for (int k = 3; k >= 1; --k)
        {
            const float g = static_cast<float>(k) * 3.0f;
            c.rrect(box.x - g, box.y - g + 6.0f, box.w + 2.0f * g, box.h + 2.0f * g, 16.0f + g, black.withAlpha(0.07f));
        }
        c.rrect(box.x, box.y, box.w, box.h, 16.0f, p.card, 1.0f, p.cardEdge);
        const float x = box.x + kPad, inner = box.w - 2.0f * kPad;

        // The title: what it is for, and the project.
        c.rrect(x, box.y + 22.0f, 30.0f, 30.0f, 9.0f, p.accent.withAlpha(0.16f));
        chat::drawIcon(c, chat::Icon::branch, x + 15.0f, box.y + 37.0f, 15.0f, p.accent);
        const bool linked = mode_ == Mode::project && link_.linked;
        const std::string title = mode_ == Mode::add ? std::string("Add from Git") : "Git: " + asciiText(ownName(project_));
        c.text(fitText(c, title, chat::kH2, inner - 60.0f).c_str(), x + 42.0f, c.capCentreTop(box.y + 37.0f, chat::kH2), chat::kH2, p.text);
        const std::string blurb =
            mode_ == Mode::add ? std::string("A repository's project made a project here, its newest commit its first version.")
            : linked           ? std::string("Each saved version is a commit; a pull brings the other side's as new versions.")
                               : std::string("Link it to an empty repository: each version becomes a commit there, and is pushed.");
        c.text(fitText(c, blurb, chat::kSmall, inner).c_str(), x, c.capCentreTop(box.y + 66.0f, chat::kSmall), chat::kSmall, p.soft);

        const auto caption = [&](float cx, float cy, const char* text) {
            c.text(text, cx + 2.0f, c.capCentreTop(cy, chat::kSmallStrong), chat::kSmallStrong, p.faint);
        };
        const float top = box.y + 100.0f;
        if (fields())
        {
            const auto fieldBox = [&](const funkgui::Rect& r, const char* cap, const funkgui::text::LineEdit& e, bool focused,
                                      const char* hint) {
                caption(r.x, r.y - 10.0f, cap);
                c.rrect(r.x, r.y, r.w, r.h, 10.0f, p.composer, 1.0f, focused ? funkgui::mix(p.composerEdge, p.accent, 0.6f) : p.composerEdge);
                const funkgui::TextStyle& st = chat::kBody;
                const float maxW = r.w - 28.0f;
                const float textTop = c.capCentreTop(r.centreY(), st);
                if (e.buffer.empty())
                    c.text(hint, r.x + 14.0f, textTop, st, p.faint);
                else
                {
                    // The end of a long address in view: the caret's side.
                    std::string shown = asciiText(e.buffer);
                    while (shown.size() > 1 && c.textWidth(shown.c_str(), st) > maxW)
                        shown.erase(shown.begin());
                    if (e.allSelected && focused)
                        c.rrect(r.x + 12.0f, r.y + 8.0f, std::min(c.textWidth(shown.c_str(), st), maxW) + 4.0f, r.h - 16.0f, 3.0f, th.accentDim);
                    c.text(shown.c_str(), r.x + 14.0f, textTop, st, p.text);
                }
                if (focused && !state_.busy)
                {
                    const std::size_t caret = static_cast<std::size_t>(std::clamp(e.caret, 0, static_cast<int>(e.buffer.size())));
                    const float before = c.textWidth(asciiText(e.buffer.substr(0, caret)).c_str(), st);
                    const float full = c.textWidth(asciiText(e.buffer).c_str(), st);
                    const float cx = r.x + 14.0f + std::clamp(before - std::max(0.0f, full - maxW), 0.0f, maxW);
                    c.rrect(cx, r.y + 9.0f, 1.5f, r.h - 18.0f, 0.75f, p.accent);
                }
            };
            fieldBox(at.remote, "REMOTE", remote_, focus_ == Field::remote, "https://github.com/you/repo.git or git@host:you/repo.git");
            fieldBox(at.branch, mode_ == Mode::add ? "BRANCH (BLANK: ITS DEFAULT)" : "BRANCH", branch_, focus_ == Field::branch,
                     mode_ == Mode::add ? "its default" : "main");
            const std::string about =
                mode_ == Mode::add
                    ? std::string("You pull from the repository at that address: what it brings is a program that plays when it "
                                  "is opened, as an imported project's does. Git signs in its own way (an SSH agent, a credential "
                                  "helper): nothing asks for a password here.")
                    : std::string("Git runs only when you press a button here, and signs in its own way (an SSH agent, a "
                                  "credential helper): nothing asks for a password here. The working folder goes in ")
                          + port_.gitFolder() + ".";
            const std::vector<std::string> lines = wrapped(c, about, chat::kSmall, inner);
            for (std::size_t i = 0; i < lines.size() && i < 4; ++i)
                c.text(lines[i].c_str(), x, c.capCentreTop(at.remote.bottom() + 24.0f + static_cast<float>(i) * 16.0f, chat::kSmall),
                       chat::kSmall, p.faint);
        }
        else
        {
            // The facts: the remote and its branch, what there is to do, the folder.
            const auto fact = [&](int row, const char* name, const std::string& value, funkgui::Col ink) {
                const float cy = top + static_cast<float>(row) * kRowH + kRowH * 0.5f;
                c.text(name, x, c.capCentreTop(cy, chat::kSmallStrong), chat::kSmallStrong, p.faint);
                c.text(fitText(c, asciiText(value), chat::kBody, inner - 90.0f).c_str(), x + 90.0f, c.capCentreTop(cy, chat::kBody),
                       chat::kBody, ink);
            };
            fact(0, "REMOTE", link_.shortRemote + "   (" + link_.branch + ")", p.text);
            fact(1, "STATE", stateText(), link_.diverged ? p.danger : p.text);
            fact(2, "FOLDER", link_.folder, p.soft);
            const funkgui::Rect& r = at.commits;
            caption(r.x, r.y - 10.0f, "RECENT COMMITS (THE HISTORY IS THE REPOSITORY'S)");
            c.rrect(r.x, r.y, r.w, r.h, 10.0f, p.composer, 1.0f, p.composerEdge);
            const int rows = std::max(1, static_cast<int>(std::floor((r.h - 8.0f) / kRowH)));
            if (link_.recent.empty())
                c.text("None yet.", r.x + 14.0f, c.capCentreTop(r.y + 4.0f + kRowH * 0.5f, chat::kBody), chat::kBody, p.faint);
            for (int i = 0; i < rows && i < static_cast<int>(link_.recent.size()); ++i)
            {
                const GitCommitEntry& k = link_.recent[static_cast<std::size_t>(i)];
                const float cy = r.y + 4.0f + static_cast<float>(i) * kRowH + kRowH * 0.5f;
                const float whenW = c.textWidth(k.when.c_str(), chat::kSmall);
                c.text(k.sha.substr(0, 7).c_str(), r.x + 14.0f, c.capCentreTop(cy, chat::kSmall), chat::kSmall, p.faint);
                c.text(fitText(c, asciiText(k.subject), chat::kBody, r.w - 90.0f - whenW - 24.0f).c_str(), r.x + 76.0f,
                       c.capCentreTop(cy, chat::kBody), chat::kBody, p.text);
                c.text(k.when.c_str(), r.right() - 14.0f, c.capCentreTop(cy, chat::kSmall), chat::kSmall, p.faint, funkgui::Align::right);
            }
        }

        // The line over the buttons: what runs now, its outcome, a question, or git not found.
        std::string said = line_;
        bool bad = failed_;
        if (state_.busy)
        {
            said = state_.doing.empty() ? std::string("Git is running...") : state_.doing;
            if (!watching_)
                said = "Git is busy with something else: " + said;
            bad = false;
        }
        else if (said.empty() && state_.looked && state_.gitPath.empty())
        {
            said = state_.gitWhy;
            bad = true;
        }
        if (!said.empty())
        {
            const std::vector<std::string> lines = wrapped(c, asciiText(said), chat::kSmall, at.line.w - 4.0f);
            const std::size_t n = std::min<std::size_t>(lines.size(), kLineRows);
            const float y0 = at.line.bottom() - static_cast<float>(n) * 16.0f;
            for (std::size_t i = 0; i < n; ++i)
            {
                const std::string text = i + 1 == n && lines.size() > n ? fitText(c, lines[i] + " ...", chat::kSmall, at.line.w) : lines[i];
                c.text(text.c_str(), x + 2.0f, c.capCentreTop(y0 + static_cast<float>(i) * 16.0f + 8.0f, chat::kSmall), chat::kSmall,
                       bad ? p.danger : askDisconnect_ ? p.text : p.soft);
            }
        }
        if (state_.busy)
        {
            // A strip that says it is working (git gives no measure of how far it has come).
            const funkgui::Rect bar{ x, at.line.bottom() + 6.0f, inner, 3.0f };
            c.rrect(bar.x, bar.y, bar.w, bar.h, 1.5f, p.composer);
            c.rrect(bar.x, bar.y, bar.w * 0.3f, bar.h, 1.5f, p.accent);
        }

        const auto button = [&](Button b, bool primary, bool danger) {
            const funkgui::Rect r = rectOf(at, b);
            if (r.w <= 0.0f)
                return;
            const bool on = enabled(b);
            const bool hover = on && hasPointer && r.contains(pointer);
            const funkgui::Col tint = danger ? p.danger : p.accent;
            const funkgui::Col fill = primary ? (hover ? funkgui::mix(tint, p.text, 0.12f) : tint) : (hover ? p.hover : p.card.withAlpha(0.0f));
            c.rrect(r.x, r.y, r.w, r.h, r.h * 0.5f, on ? fill : fill.withAlpha(primary ? 0.35f : 0.0f), primary ? 0.0f : 1.0f, p.composerEdge);
            const funkgui::TextStyle& st = primary ? chat::kSmallStrong : chat::kSmall;
            const std::string l = label(b);
            c.text(l.c_str(), r.centreX(), c.capCentreTop(r.centreY(), st), st, !on ? p.faint : primary ? p.onAccent : (hover ? p.text : p.soft),
                   funkgui::Align::centre);
        };
        button(Button::close, false, false);
        button(Button::other, false, false);
        button(Button::disconnect, askDisconnect_, askDisconnect_);
        button(Button::show, false, false);
        button(Button::check, false, false);
        button(Button::pull, false, false);
        button(Button::primary, true, false);
    }

    void GitCard::accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const
    {
        const Places at = places(width, height);
        funkgui::A11yItem box;
        box.id = kBoxId;
        box.role = funkgui::a11yrole::group;
        box.bounds = at.box;
        box.title = mode_ == Mode::add ? std::string("Add from Git") : "Git: " + ownName(project_);
        box.description = link_.linked && mode_ == Mode::project ? "linked to " + link_.shortRemote + ", " + stateText() : std::string();
        box.help = "Git runs only when a button here is pressed, and signs in its own way: nothing asks for a password here";
        out.push_back(box);
        if (fields())
        {
            const auto text = [&](uint32_t id, const funkgui::Rect& r, const char* title, const funkgui::text::LineEdit& e) {
                funkgui::A11yItem it;
                it.id = id;
                it.parent = kBoxId;
                it.role = funkgui::a11yrole::editableText;
                it.bounds = r;
                it.title = title;
                it.value = e.buffer;
                out.push_back(std::move(it));
            };
            text(kRemoteId, at.remote, "Remote's address", remote_);
            text(kBranchId, at.branch, mode_ == Mode::add ? "Branch (blank: its default)" : "Branch", branch_);
        }
        else
        {
            funkgui::A11yItem list;
            list.id = kCommitsId;
            list.parent = kBoxId;
            list.role = funkgui::a11yrole::list;
            list.bounds = at.commits;
            list.title = "Recent commits";
            list.value = std::to_string(link_.recent.size());
            out.push_back(list);
            for (std::size_t i = 0; i < link_.recent.size(); ++i)
            {
                funkgui::A11yItem it;
                it.id = kCommitsId + 1 + static_cast<uint32_t>(i);
                it.parent = kCommitsId;
                it.role = funkgui::A11yRole::staticText;
                it.bounds = { at.commits.x, at.commits.y + 4.0f + static_cast<float>(i) * kRowH, at.commits.w, kRowH };
                it.title = link_.recent[i].subject;
                it.value = link_.recent[i].when;
                out.push_back(std::move(it));
            }
        }
        if (!line_.empty() || state_.busy)
        {
            funkgui::A11yItem it;
            it.id = kLineId;
            it.parent = kBoxId;
            it.role = funkgui::A11yRole::staticText;
            it.bounds = at.line;
            it.title = state_.busy ? state_.doing : line_;
            out.push_back(std::move(it));
        }
        const auto button = [&](uint32_t id, Button b) {
            const funkgui::Rect r = rectOf(at, b);
            if (r.w <= 0.0f)
                return;
            funkgui::A11yItem it;
            it.id = id;
            it.parent = kBoxId;
            it.role = funkgui::A11yRole::button;
            it.bounds = r;
            it.title = label(b);
            it.enabled = enabled(b);
            out.push_back(std::move(it));
        };
        button(kPrimaryId, Button::primary);
        button(kPullId, Button::pull);
        button(kCheckId, Button::check);
        button(kShowId, Button::show);
        button(kDisconnectId, Button::disconnect);
        button(kCloseId, Button::close);
        button(kOtherId, Button::other);
    }

    bool GitCard::a11yAction(uint32_t id, funkgui::A11yAction a, float /*width*/, float /*height*/)
    {
        if (a != funkgui::A11yAction::press && a != funkgui::A11yAction::focus)
            return false;
        if (id == kRemoteId && fields())
            focusOn(Field::remote);
        else if (id == kBranchId && fields())
            focusOn(Field::branch);
        else if (id == kPrimaryId)
            press(Button::primary);
        else if (id == kPullId)
            press(Button::pull);
        else if (id == kCheckId)
            press(Button::check);
        else if (id == kShowId)
            press(Button::show);
        else if (id == kDisconnectId)
            press(Button::disconnect);
        else if (id == kCloseId)
            press(Button::close);
        else if (id == kOtherId)
            press(Button::other);
        else
            return false;
        return true;
    }
}
