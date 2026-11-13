// Source/editor/GitCard.h: GIT, a card over the panel (PROJECTS' Git... and Add from Git..., the top bar's project
// pill's menu): a project kept in a git repository of the user's (plugin/GitProjects.h; docs/plan/05-git.md).
//
//   not linked    REMOTE (the repository's address: https://..., ssh://... or user@host:path) and BRANCH (main unless
//                 said); Connect: each saved version becomes a commit, pushed
//   linked        the remote (shortened: github.com/me/pad) and its branch, what is known ("3 versions to push",
//                 "2 commits to pull", "Up to date, checked 14:02"), the working folder, the newest commits; Push,
//                 Pull, Check, Show folder, Disconnect (pressed twice: the second press confirms; the folder stays)
//   add           Add from Git...: REMOTE and BRANCH (the remote's own unless said); Add: a new project made of it
//   running       what git does now, and Cancel (which kills it); every other button waits
//   after         the outcome in words, or git's own refusal as it said it (its last lines), with what to do when it
//                 looks like a sandboxed host's or an SSH key's (GitProjects.cpp's words)
//
// It reads the store's links file and cache alone (EditorPort::gitLink, projectsPolled): no repository is read on the
// message thread, and git runs only on the processor's git thread, only when a button here is pressed. A job it started
// goes on when it closes (Escape, Close, a press outside it); the Panel then says the outcome in the status line. Tab
// moves between the fields, Return presses the card's main button, Cmd-V pastes. While it is open it takes every
// pointer event and key. A factory project's card says to save a copy first and offers nothing else.
#pragma once

#include "plugin/EditorPort.h"

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>
#include <funkgui/text/LineEdit.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace playground::ui
{
    class GitCard
    {
    public:
        static constexpr uint32_t kA11yBase = 0x7FFB0000u;   // its accessibility ids, past every item's
        static constexpr int kRemoteLength = 400, kBranchLength = 100;
        static constexpr std::size_t kLogShown = 8;

        enum class Mode : uint8_t { project, add };
        enum class Field : uint8_t { remote, branch };
        // Its buttons, in the row's order from the right.
        enum class Button : uint8_t { none, close, connect, add, push, pull, check, folder, disconnect };

        explicit GitCard(EditorPort& port) : port_(port) {}

        bool isOpen() const noexcept { return open_; }
        void open(const std::string& project);           // a project's git
        void openAdd();                                  // Add from Git...
        void close();                                    // a job it started goes on

        // Each frame while open: the link read again (EditorPort::gitLink: the links file, the store's cache), and the
        // job it started followed (its step, then its outcome). Whether anything it shows changed.
        bool poll();

        struct Places
        {
            funkgui::Rect box, remote, branch, outcome, log;
            std::vector<std::pair<Button, funkgui::Rect>> buttons;   // shown, left to right
        };
        Places places(float width, float height) const;

        void pointerDown(funkgui::Point p, float width, float height);
        void key(const funkgui::KeyEvent& e, funkgui::HostServices* host, float width, float height);
        funkgui::Cursor cursor(funkgui::Point p, float width, float height) const;
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, float width, float height, funkgui::Point pointer,
                  bool hasPointer) const;
        void accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const;
        bool a11yAction(uint32_t id, funkgui::A11yAction a, float width, float height);
        uint32_t revision() const noexcept { return revision_; }

        // The status line's words, once, for an outcome it followed ("PUSHED 3 VERSIONS TO ..."), and whether a job it
        // started ended (the Panel polls the projects again).
        std::string takeNotice();
        bool takeLanded();

        Mode mode() const noexcept { return mode_; }
        const std::string& project() const noexcept { return project_; }
        const GitLinkView& link() const noexcept { return link_; }
        bool running() const noexcept { return running_; }
        bool failed() const noexcept { return failed_; }
        const std::string& outcome() const noexcept { return outcome_; }
        Field focus() const noexcept { return focus_; }
        const std::string& remoteText() const noexcept { return remote_.buffer; }
        const std::string& branchText() const noexcept { return branch_.buffer; }
        bool confirming() const noexcept { return confirmDisconnect_; }
        static std::string buttonLabel(Button b, bool confirm = false);
        // The state's line ("3 versions to push, 2 commits to pull", "Up to date, checked 14:02"): the card's and the
        // mark's title.
        static std::string stateLine(const GitLinkView& l);

        // For the probes: a button pressed, as a click on it would.
        void press(Button b);
        void setRemote(const std::string& s);            // the field's text, as typed
        void setBranch(const std::string& s);

    private:
        void read();                                     // the link, from the store's links file and cache
        bool enabled(Button b) const;
        void start(GitKind kind);
        funkgui::text::LineEdit& field(Field f) { return f == Field::remote ? remote_ : branch_; }
        const funkgui::text::LineEdit& field(Field f) const { return f == Field::remote ? remote_ : branch_; }
        void focusOn(Field f);
        bool fields() const noexcept { return mode_ == Mode::add || (!link_.linked && !factory_); }
        Button primary() const;

        EditorPort& port_;
        bool open_ = false;
        Mode mode_ = Mode::project;
        std::string project_;
        bool factory_ = false;
        GitLinkView link_;
        uint64_t readSeen_ = 0;                          // EditorPort::projectsRead when it read the link
        funkgui::text::LineEdit remote_, branch_;
        Field focus_ = Field::remote;
        bool running_ = false;                           // a job this card started runs
        GitKind kind_ = GitKind::find;
        uint64_t ended_ = 0;                             // GitState::ended when it started
        std::string step_;                               // what git does now
        std::string outcome_;                            // the last outcome (or refusal), in words
        bool failed_ = false;
        bool confirmDisconnect_ = false;
        std::string notice_;
        bool landed_ = false;
        uint32_t revision_ = 0;
    };
}
