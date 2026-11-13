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
        constexpr float kBoxW = 680.0f, kBoxH = 540.0f;
        constexpr float kPad = 24.0f;
        constexpr float kCaption = 18.0f;
        constexpr float kGap = 14.0f;
        constexpr float kButtonH = 32.0f;
        constexpr float kFieldH = 34.0f;
        constexpr float kLineH = 18.0f;
        constexpr float kRowH = 22.0f;                   // a commit's row
        constexpr int kOutcomeLines = 4;
        constexpr uint32_t kBoxId = GitCard::kA11yBase, kRemoteId = kBoxId + 1, kBranchId = kBoxId + 2, kOutcomeId = kBoxId + 3,
                           kStateId = kBoxId + 4, kLogId = kBoxId + 5, kButtonId = kBoxId + 0x100;

        float buttonW(const std::string& label) { return std::round(7.4f * static_cast<float>(label.size()) + 30.0f); }

        // `s` in lines of at most `maxW` px (a line break starts a line; words kept whole while they fit).
        std::vector<std::string> wrapped(const funkgui::Canvas& c, const std::string& s, const funkgui::TextStyle& st, float maxW)
        {
            std::vector<std::string> out;
            std::size_t at = 0;
            while (at <= s.size())
            {
                const std::size_t nl = std::min(s.find('\n', at), s.size());
                const std::string para = s.substr(at, nl - at);
                std::string line;
                std::size_t i = 0;
                while (i < para.size())
                {
                    std::size_t end = para.find(' ', i);
                    if (end == std::string::npos)
                        end = para.size();
                    const std::string word = para.substr(i, end - i);
                    const std::string tried = line.empty() ? word : line + " " + word;
                    if (!line.empty() && c.textWidth(tried.c_str(), st) > maxW)
                    {
                        out.push_back(line);
                        line = word;
                    }
                    else
                        line = tried;
                    i = end + 1;
                }
                out.push_back(line);
                at = nl + 1;
            }
            while (!out.empty() && out.back().empty())
                out.pop_back();
            return out;
        }

        std::string plural(int n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }
    }

    std::string GitCard::buttonLabel(Button b, bool confirm)
    {
        switch (b)
        {
            case Button::close: return "Close";
            case Button::connect: return "Connect";
            case Button::add: return "Add";
            case Button::push: return "Push";
            case Button::pull: return "Pull";
            case Button::check: return "Check";
            case Button::folder: return "Show folder";
            case Button::disconnect: return confirm ? "Disconnect? Press again" : "Disconnect";
            case Button::none: break;
        }
        return {};
    }

    std::string GitCard::stateLine(const GitLinkView& l)
    {
        if (!l.linked)
            return {};
        std::string s;
        if (l.toPush > 0)
            s = plural(l.toPush, "version", "versions") + " to push";
        if (l.behind > 0)
            s += (s.empty() ? "" : ", ") + plural(l.behind, "commit", "commits") + " to pull";
        if (s.empty())
            s = l.behind == 0 ? std::string("Up to date") : std::string("Not checked yet");
        if (!l.checked.empty())
            s += ", checked " + l.checked;
        return s;
    }

    void GitCard::open(const std::string& project)
    {
        open_ = true;
        mode_ = Mode::project;
        project_ = project;
        factory_ = isFactoryName(project);
        remote_.set("", kRemoteLength);
        branch_.set("main", kBranchLength);
        branch_.allSelected = false;
        focus_ = Field::remote;
        confirmDisconnect_ = false;
        if (!running_)
        {
            outcome_.clear();
            failed_ = false;
        }
        read();
        ++revision_;
    }

    void GitCard::openAdd()
    {
        open_ = true;
        mode_ = Mode::add;
        project_.clear();
        factory_ = false;
        link_ = {};
        remote_.set("", kRemoteLength);
        branch_.set("", kBranchLength);
        focus_ = Field::remote;
        confirmDisconnect_ = false;
        if (!running_)
        {
            outcome_.clear();
            failed_ = false;
        }
        readSeen_ = port_.projectsRead();
        ++revision_;
    }

    void GitCard::close()
    {
        open_ = false;
        confirmDisconnect_ = false;
        ++revision_;
    }

    void GitCard::read()
    {
        readSeen_ = port_.projectsRead();
        link_ = mode_ == Mode::project && !factory_ ? port_.gitLink(project_) : GitLinkView{};
    }

    bool GitCard::poll()
    {
        bool changed = false;
        if (running_)
        {
            const GitState s = port_.gitState();
            if (s.ended != ended_)
            {
                running_ = false;
                landed_ = true;
                failed_ = !s.ok;
                outcome_ = s.ok ? s.line : s.why;
                step_.clear();
                if (s.ok && kind_ == GitKind::clone && open_)
                {
                    // The project it made: the card is its card now.
                    mode_ = Mode::project;
                    project_ = s.project;
                    factory_ = false;
                }
                const std::string what = kind_ == GitKind::connect ? "CONNECT" : kind_ == GitKind::push ? "PUSH" : kind_ == GitKind::pull ? "PULL"
                                       : kind_ == GitKind::check ? "CHECK" : kind_ == GitKind::clone ? "ADD FROM GIT" : kind_ == GitKind::disconnect ? "DISCONNECT"
                                                                                                                                            : "GIT";
                if (s.cancelled)
                    notice_ = what + " CANCELLED";
                else if (s.ok)
                {
                    std::string line = s.line;
                    if (!line.empty() && line.back() == '.')
                        line.pop_back();
                    notice_ = upper(line.substr(0, std::min(line.find(". "), line.size())));
                }
                else
                    notice_ = what + " FAILED: THE GIT CARD SAYS WHY";
                if (open_)
                    read();
                changed = true;
            }
            else if (s.step != step_)
            {
                step_ = s.step;
                changed = true;
            }
        }
        if (open_ && port_.projectsRead() != readSeen_)
        {
            const GitLinkView before = link_;
            read();
            changed = changed || !(before == link_);
        }
        if (changed)
            ++revision_;
        return changed;
    }

    std::string GitCard::takeNotice() { return std::exchange(notice_, std::string()); }
    bool GitCard::takeLanded() { return std::exchange(landed_, false); }

    void GitCard::setRemote(const std::string& s)
    {
        remote_.set(s, kRemoteLength);
        remote_.allSelected = false;
        remote_.caret = static_cast<int>(remote_.buffer.size());
        ++revision_;
    }

    void GitCard::setBranch(const std::string& s)
    {
        branch_.set(s, kBranchLength);
        branch_.allSelected = false;
        branch_.caret = static_cast<int>(branch_.buffer.size());
        ++revision_;
    }

    void GitCard::focusOn(Field f)
    {
        focus_ = f;
        funkgui::text::LineEdit& e = field(f);
        e.allSelected = false;
        e.caret = static_cast<int>(e.buffer.size());
        ++revision_;
    }

    GitCard::Button GitCard::primary() const
    {
        if (mode_ == Mode::add)
            return Button::add;
        if (factory_)
            return Button::close;
        return link_.linked ? Button::push : Button::connect;
    }

    bool GitCard::enabled(Button b) const
    {
        if (b == Button::close)
            return true;
        if (running_)
            return false;
        if (b == Button::connect || b == Button::add)
            return !remote_.buffer.empty();
        if (b == Button::folder)
            return link_.linked && !link_.folder.empty();
        return true;
    }

    GitCard::Places GitCard::places(float width, float height) const
    {
        const float w = std::min(kBoxW, width - 32.0f), h = std::min(kBoxH, height - 32.0f);
        const funkgui::Rect box{ std::round((width - w) * 0.5f), std::round((height - h) * 0.45f), w, h };
        const float x0 = box.x + kPad, inner = w - 2.0f * kPad;
        Places at;
        at.box = box;
        const float by = box.bottom() - kPad - kButtonH;
        // The buttons: Close at the left; the rest at the right, the main one last.
        std::vector<Button> right;
        if (mode_ == Mode::add)
            right = { Button::add };
        else if (factory_)
            right = {};
        else if (!link_.linked)
            right = { Button::connect };
        else
            right = { Button::disconnect, Button::folder, Button::check, Button::pull, Button::push };
        at.buttons.push_back({ Button::close, { x0, by, buttonW(running_ ? "Cancel" : "Close"), kButtonH } });
        float x = box.right() - kPad;
        std::vector<std::pair<Button, funkgui::Rect>> r;
        for (auto it = right.rbegin(); it != right.rend(); ++it)
        {
            const float bw = buttonW(buttonLabel(*it, *it == Button::disconnect && confirmDisconnect_));
            x -= bw;
            r.insert(r.begin(), { *it, { x, by, bw, kButtonH } });
            x -= 8.0f;
        }
        at.buttons.insert(at.buttons.end(), r.begin(), r.end());
        at.outcome = { x0, by - kGap - static_cast<float>(kOutcomeLines) * kLineH - 12.0f, inner, static_cast<float>(kOutcomeLines) * kLineH + 12.0f };
        float y = box.y + 92.0f;
        if (fields())
        {
            y += kCaption;
            at.remote = { x0, y, inner, kFieldH };
            y = at.remote.bottom() + kGap + kCaption;
            at.branch = { x0, y, std::min(240.0f, inner), kFieldH };
        }
        else if (link_.linked)
        {
            // The facts' rows (remote, state, folder: three rows), then the commits.
            y += 14.0f + 3.0f * kRowH + kGap + kCaption;
            at.log = { x0, y, inner, std::max(0.0f, at.outcome.y - kGap - y) };
        }
        return at;
    }

    void GitCard::start(GitKind kind)
    {
        GitJob job;
        job.kind = kind;
        job.project = project_;
        job.remote = remote_.buffer;
        job.branch = branch_.buffer;
        const uint64_t ended = port_.gitState().ended;
        std::string why;
        if (!port_.startGit(job, why))
        {
            outcome_ = "Not started: " + why + ".";
            failed_ = true;
            ++revision_;
            return;
        }
        running_ = true;
        kind_ = kind;
        ended_ = ended;
        step_ = "Starting git";
        outcome_.clear();
        failed_ = false;
        ++revision_;
    }

    void GitCard::press(Button b)
    {
        if (!enabled(b))
            return;
        if (b != Button::disconnect)
            confirmDisconnect_ = false;
        switch (b)
        {
            case Button::close:
                if (running_)
                    port_.cancelGit();                   // Cancel: git is killed; the outcome comes as any
                else
                    close();
                break;
            case Button::connect: start(GitKind::connect); break;
            case Button::add: start(GitKind::clone); break;
            case Button::push: start(GitKind::push); break;
            case Button::pull: start(GitKind::pull); break;
            case Button::check: start(GitKind::check); break;
            case Button::folder:
            {
                std::string why;
                if (!port_.showPath(link_.folder, why))
                {
                    outcome_ = "Not shown: " + why + ".";
                    failed_ = true;
                }
                break;
            }
            case Button::disconnect:
                if (!confirmDisconnect_)
                    confirmDisconnect_ = true;           // the second press confirms
                else
                {
                    confirmDisconnect_ = false;
                    start(GitKind::disconnect);
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
            close();                                     // as Close: the Panel drops the press
            return;
        }
        if (fields() && at.remote.contains(p))
            focusOn(Field::remote);
        else if (fields() && at.branch.contains(p))
            focusOn(Field::branch);
        for (const auto& [b, r] : at.buttons)
            if (r.contains(p))
            {
                press(b);
                return;
            }
        if (confirmDisconnect_)
        {
            confirmDisconnect_ = false;
            ++revision_;
        }
    }

    void GitCard::key(const funkgui::KeyEvent& e, funkgui::HostServices* host, float /*width*/, float /*height*/)
    {
        using funkgui::Key;
        if (e.key == Key::escape)
        {
            close();
            return;
        }
        if (e.key == Key::enter)
        {
            press(primary());
            return;
        }
        if (!fields() || running_)
            return;
        const int most = focus_ == Field::remote ? kRemoteLength : kBranchLength;
        funkgui::text::LineEdit& f = field(focus_);
        if (e.key == Key::tab)
            focusOn(focus_ == Field::remote ? Field::branch : Field::remote);
        else if (e.key == Key::character && e.mods.cmd && (e.ch == 'v' || e.ch == 'V'))
        {
            if (host != nullptr)
            {
                std::string text = host->clipboardText();
                text.erase(std::remove_if(text.begin(), text.end(), [](char ch) { return ch == '\n' || ch == '\r'; }), text.end());
                f.paste(text, most);
            }
        }
        else if (e.key == Key::character && e.mods.cmd && (e.ch == 'a' || e.ch == 'A'))
            f.allSelected = !f.buffer.empty();
        else
            f.key(e, most);
        ++revision_;
    }

    funkgui::Cursor GitCard::cursor(funkgui::Point p, float width, float height) const
    {
        const Places at = places(width, height);
        for (const auto& [b, r] : at.buttons)
            if (r.contains(p) && enabled(b))
                return funkgui::Cursor::pointingHand;
        if (fields() && (at.remote.contains(p) || at.branch.contains(p)))
            return funkgui::Cursor::normal;
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

        // The title and what the card is for.
        c.rrect(x, box.y + 22.0f, 30.0f, 30.0f, 9.0f, p.accent.withAlpha(0.16f));
        chat::drawIcon(c, chat::Icon::branch, x + 15.0f, box.y + 37.0f, 16.0f, p.accent);
        const std::string title = mode_ == Mode::add ? std::string("Add from Git") : "Git: " + asciiText(ownName(project_));
        c.text(fitText(c, title, chat::kH2, inner * 0.62f).c_str(), x + 42.0f, c.capCentreTop(box.y + 37.0f, chat::kH2), chat::kH2, p.text);
        if (link_.linked)
            c.text(fitText(c, link_.branch, chat::kSmall, inner * 0.3f).c_str(), box.right() - kPad, c.capCentreTop(box.y + 37.0f, chat::kSmall),
                   chat::kSmall, p.faint, funkgui::Align::right);
        const std::string blurb =
            mode_ == Mode::add ? std::string("A repository a project was pushed to becomes a new project here. What it holds will play when "
                                             "you open it: trust it as you would a file you import.")
            : factory_         ? std::string("A factory project cannot be kept in git: save your own copy of it first (PROJECTS, Save).")
            : link_.linked     ? "Each saved version is a commit. Pulls come from " + link_.shortRemote + ": the repository's owner decides "
                                 "what is in it."
                               : std::string("Give it an empty repository: each saved version becomes a commit, pushed. Git signs in its own "
                                             "way (an SSH agent, a credential helper); the plugin keeps no password.");
        const std::vector<std::string> blurbLines = wrapped(c, blurb, chat::kSmall, inner);
        for (std::size_t i = 0; i < blurbLines.size() && i < 2; ++i)
            c.text(blurbLines[i].c_str(), x, c.capCentreTop(box.y + 64.0f + static_cast<float>(i) * 16.0f, chat::kSmall), chat::kSmall, p.soft);

        const auto caption = [&](const funkgui::Rect& r, const char* text) {
            c.text(text, r.x + 2.0f, c.capCentreTop(r.y - 10.0f, chat::kSmallStrong), chat::kSmallStrong, p.faint);
        };
        if (fields())
        {
            const auto line = [&](const funkgui::Rect& r, const char* cap, const funkgui::text::LineEdit& e, bool focused, const char* hint) {
                caption(r, cap);
                // While git works the fields take nothing: drawn quiet, no focus ring.
                const bool live = focused && !running_;
                c.rrect(r.x, r.y, r.w, r.h, 10.0f, running_ ? p.card : p.composer, 1.0f,
                        live ? funkgui::mix(p.composerEdge, p.accent, 0.6f) : p.composerEdge);
                const funkgui::TextStyle& st = chat::kBody;
                const float maxW = r.w - 28.0f;
                const float top = c.capCentreTop(r.centreY(), st);
                if (e.buffer.empty())
                    c.text(hint, r.x + 14.0f, top, st, p.faint);
                else
                {
                    if (e.allSelected && focused)
                        c.rrect(r.x + 12.0f, r.y + 8.0f, std::min(c.textWidth(e.buffer.c_str(), st), maxW) + 4.0f, r.h - 16.0f, 3.0f, th.accentDim);
                    c.text(fitText(c, e.buffer, st, maxW).c_str(), r.x + 14.0f, top, st, running_ ? p.faint : p.text);
                }
                if (focused && !running_)
                {
                    const std::string before = e.buffer.substr(0, static_cast<std::size_t>(std::clamp(e.caret, 0, static_cast<int>(e.buffer.size()))));
                    const float cx = r.x + 14.0f + std::min(c.textWidth(before.c_str(), st), maxW);
                    c.rrect(cx, r.y + 9.0f, 1.5f, r.h - 18.0f, 0.75f, p.accent);
                }
            };
            line(at.remote, "REMOTE", remote_, focus_ == Field::remote, "https://github.com/you/project.git or git@github.com:you/project.git");
            line(at.branch, "BRANCH", branch_, focus_ == Field::branch, mode_ == Mode::add ? "the remote's own" : "main");
        }
        else if (link_.linked)
        {
            // Its facts, then its newest commits.
            float y = box.y + 112.0f;
            const auto fact = [&](const char* name, const std::string& value, funkgui::Col ink) {
                c.text(name, x, c.capCentreTop(y, chat::kSmallStrong), chat::kSmallStrong, p.faint);
                c.text(fitText(c, value, chat::kBody, inner - 90.0f).c_str(), x + 90.0f, c.capCentreTop(y, chat::kBody), chat::kBody, ink);
                y += kRowH;
            };
            fact("REMOTE", link_.shortRemote + "  (" + link_.branch + ")", p.text);
            fact("STATE", stateLine(link_), link_.toPush > 0 || link_.behind > 0 ? p.accent : p.soft);
            fact("FOLDER", link_.folder, p.soft);
            caption(at.log, "COMMITS");
            if (link_.log.empty())
                c.text("None yet.", x + 2.0f, c.capCentreTop(at.log.y + 10.0f, chat::kSmall), chat::kSmall, p.faint);
            const std::size_t rows = std::min(link_.log.size(), static_cast<std::size_t>(std::max(0.0f, std::floor(at.log.h / kRowH))));
            for (std::size_t i = 0; i < rows; ++i)
            {
                const GitCommitEntry& e = link_.log[i];
                const float ry = at.log.y + 10.0f + static_cast<float>(i) * kRowH;
                c.text(e.sha.c_str(), x + 2.0f, c.capCentreTop(ry, chat::kSmall), chat::kSmall, p.faint);
                c.text(e.when.c_str(), box.right() - kPad, c.capCentreTop(ry, chat::kSmall), chat::kSmall, p.faint, funkgui::Align::right);
                c.text(fitText(c, asciiText(e.subject), chat::kSmall, inner - 70.0f - 130.0f).c_str(), x + 70.0f, c.capCentreTop(ry, chat::kSmall),
                       chat::kSmall, p.text);
            }
        }

        // The outcome (or what git does now): git's own words as they are, wrapped.
        {
            const funkgui::Rect& r = at.outcome;
            const std::string said = running_          ? step_ + "..."
                                   : confirmDisconnect_ ? std::string("Press Disconnect again to remove the link to ") + link_.shortRemote
                                                              + ". The working folder stays where it is; nothing is deleted."
                                                        : outcome_;
            if (running_)
            {
                c.rrect(r.x, r.y, r.w, r.h, 10.0f, p.accent.withAlpha(0.07f), 1.0f, p.accent.withAlpha(0.35f));
                c.text("GIT IS WORKING", r.x + 12.0f, c.capCentreTop(r.y - 10.0f, chat::kSmallStrong), chat::kSmallStrong, p.accent);
            }
            if (!said.empty())
            {
                if (failed_ && !running_ && !confirmDisconnect_)
                    c.rrect(r.x, r.y, r.w, r.h, 10.0f, p.danger.withAlpha(0.08f), 1.0f, p.danger.withAlpha(0.35f));
                const std::vector<std::string> lines = wrapped(c, asciiText(said), chat::kSmall, r.w - 24.0f);
                for (std::size_t i = 0; i < lines.size() && i < static_cast<std::size_t>(kOutcomeLines); ++i)
                {
                    const bool last = i + 1 == static_cast<std::size_t>(kOutcomeLines) && lines.size() > i + 1;
                    c.text(fitText(c, last ? lines[i] + " ..." : lines[i], chat::kSmall, r.w - 24.0f).c_str(), r.x + 12.0f,
                           c.capCentreTop(r.y + 6.0f + (static_cast<float>(i) + 0.5f) * kLineH, chat::kSmall), chat::kSmall,
                           running_ ? p.accent : failed_ && !confirmDisconnect_ ? p.danger : p.soft);
                }
            }
        }

        // The buttons.
        const Button main = primary();
        for (const auto& [b, r] : at.buttons)
        {
            const bool on = enabled(b);
            const bool isMain = b == main && b != Button::close;
            const bool hover = on && hasPointer && r.contains(pointer);
            const bool warn = b == Button::disconnect && confirmDisconnect_;
            const funkgui::Col fill = isMain ? (hover ? funkgui::mix(p.accent, p.text, 0.12f) : p.accent) : (hover ? p.hover : p.card.withAlpha(0.0f));
            c.rrect(r.x, r.y, r.w, r.h, r.h * 0.5f, on ? fill : fill.withAlpha(isMain ? 0.35f : 0.0f), isMain ? 0.0f : 1.0f,
                    warn ? p.danger : p.composerEdge);
            const funkgui::TextStyle& st = isMain ? chat::kSmallStrong : chat::kSmall;
            const std::string label = b == Button::close && running_ ? std::string("Cancel") : buttonLabel(b, confirmDisconnect_);
            c.text(label.c_str(), r.centreX(), c.capCentreTop(r.centreY(), st), st,
                   !on ? p.faint : isMain ? p.onAccent : warn ? p.danger : (hover ? p.text : p.soft), funkgui::Align::centre);
        }
    }

    void GitCard::accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const
    {
        const Places at = places(width, height);
        funkgui::A11yItem box;
        box.id = kBoxId;
        box.role = funkgui::a11yrole::group;
        box.bounds = at.box;
        box.title = mode_ == Mode::add ? std::string("Add from Git") : "Git: " + ownName(project_);
        box.description = running_ ? step_ : outcome_;
        box.help = "Git signs in its own way (an SSH agent, a credential helper); the plugin keeps no password";
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
                it.readOnly = running_;
                out.push_back(std::move(it));
            };
            text(kRemoteId, at.remote, "Remote's address", remote_);
            text(kBranchId, at.branch, "Branch", branch_);
        }
        else if (link_.linked)
        {
            funkgui::A11yItem state;
            state.id = kStateId;
            state.parent = kBoxId;
            state.role = funkgui::A11yRole::staticText;
            state.bounds = { at.box.x + kPad, at.box.y + 100.0f, at.box.w - 2.0f * kPad, 3.0f * kRowH };
            state.title = "Linked to " + link_.shortRemote + ", branch " + link_.branch;
            state.value = stateLine(link_);
            state.help = "Working folder: " + link_.folder;
            out.push_back(std::move(state));
            funkgui::A11yItem log;
            log.id = kLogId;
            log.parent = kBoxId;
            log.role = funkgui::A11yRole::staticText;
            log.bounds = at.log;
            log.title = "Commits";
            for (const GitCommitEntry& e : link_.log)
                log.value += (log.value.empty() ? "" : "; ") + e.subject + " (" + e.when + ")";
            out.push_back(std::move(log));
        }
        if (!outcome_.empty() || running_)
        {
            funkgui::A11yItem o;
            o.id = kOutcomeId;
            o.parent = kBoxId;
            o.role = funkgui::A11yRole::staticText;
            o.bounds = at.outcome;
            o.title = running_ ? "Git is working" : failed_ ? "Git failed" : "Done";
            o.value = running_ ? step_ : outcome_;
            out.push_back(std::move(o));
        }
        for (const auto& [b, r] : at.buttons)
        {
            funkgui::A11yItem it;
            it.id = kButtonId + static_cast<uint32_t>(b);
            it.parent = kBoxId;
            it.role = funkgui::A11yRole::button;
            it.bounds = r;
            it.title = b == Button::close && running_ ? std::string("Cancel") : buttonLabel(b, confirmDisconnect_);
            it.enabled = enabled(b);
            if (b == Button::disconnect)
                it.help = "The link goes; the working folder stays where it is";
            out.push_back(std::move(it));
        }
    }

    bool GitCard::a11yAction(uint32_t id, funkgui::A11yAction a, float width, float height)
    {
        if (a != funkgui::A11yAction::press && a != funkgui::A11yAction::focus)
            return false;
        (void)width;
        (void)height;
        if (id == kRemoteId && fields())
            focusOn(Field::remote);
        else if (id == kBranchId && fields())
            focusOn(Field::branch);
        else if (id > kButtonId && id <= kButtonId + static_cast<uint32_t>(Button::disconnect))
            press(static_cast<Button>(id - kButtonId));
        else
            return false;
        return true;
    }
}
