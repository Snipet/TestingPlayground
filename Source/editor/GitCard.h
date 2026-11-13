// Source/editor/GitCard.h: GIT, a card over the panel (package 05; from PROJECTS' Git... and the project pill's menu):
// a project kept in a git repository of its own (plugin/GitProjects.h), through the user's own git.
//
//   not linked  REMOTE (an https:// or ssh address, or user@host:path) and BRANCH (main unless said); Connect (each kept
//               version a commit, then pushed: an empty repository only), Add from Git... (this card's other face)
//   linked      the remote (shortened: github.com/me/pad) and its branch; what there is to do ("3 versions to push",
//               "2 commits to pull", "Up to date, checked 14:02", "Not checked yet"); the working folder; the last
//               eight commits (subject, when); Push, Pull, Check, Show folder, Disconnect (a second click: Disconnect
//               it, its line saying what stays, the project and the folder, and what goes, the link)
//   diverged    both sides moved and git could not merge them (part 3): Keep mine (their side saved as a version, yours
//               again on top, a merge commit of yours), Take theirs (their side the newest version, yours the one
//               before, a merge commit of theirs), Compare... (COMPARE, the newest version against their side read from
//               the fetched commit into memory: no Open, no A/B for it, nothing written until one of the two is chosen)
//   add         REMOTE and BRANCH (blank: the remote's default): Add (a clone made a new project here, linked); whose
//               repository it pulls from is said, as its program plays when opened
//   Close       or Escape, or a press outside the card: a job under way goes on, its outcome in the status line
// While a job runs (EditorPort::gitState) the card says what it is doing, its buttons wait and Close is Cancel (git
// killed; what a Connect or an Add made is removed again). After: its outcome, or git's own words when it failed (with
// what may help: a sandboxed host, an SSH key, a credential helper). Git runs only when a button here is pressed (and
// Settings' Look for git): nothing on opening, on a timer or on a save. It reads the links file and the store's cache
// alone (EditorPort::gitLink), never a repository, and again only when EditorPort::gitPolled moves. Tab moves between
// the fields; Return presses the card's first button (Connect, Push, Add). While open it takes every pointer event and
// key.
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
#include <string>
#include <vector>

namespace playground::ui
{
    class GitCard
    {
    public:
        static constexpr uint32_t kA11yBase = 0x7FF80000u;   // its accessibility ids, past every item's (and the other cards')
        static constexpr int kRemoteLength = 300, kBranchLength = 100;

        enum class Mode : uint8_t { project, add };
        enum class Field : uint8_t { remote, branch };
        enum class Button : uint8_t { none, primary, pull, check, show, disconnect, close, other };

        explicit GitCard(EditorPort& port) : port_(port) {}

        bool isOpen() const noexcept { return open_; }
        void open(const std::string& project);          // its link read; "" opens Add from Git
        void openAdd();
        void close();
        // Each frame while open: the link and the job read again when EditorPort::gitPolled moved. Whether it changed.
        bool poll();

        struct Places
        {
            funkgui::Rect box, remote, branch, commits, line;
            funkgui::Rect primary, pull, check, show, disconnect, close, other;   // other: Add from Git... / Back
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
        std::string takeNotice();                        // for the status line, once
        // Part 3: their side read (Compare...): the project whose COMPARE the Panel opens, mine against theirs, once.
        std::string takeCompare();

        // What it shows (the probes'): its face, the project, its link, the job, the line, the fields, a question asked.
        Mode mode() const noexcept { return mode_; }
        const std::string& project() const noexcept { return project_; }
        const GitLink& link() const noexcept { return link_; }
        const GitState& state() const noexcept { return state_; }
        bool running() const noexcept { return state_.busy; }
        const std::string& line() const noexcept { return line_; }
        bool failed() const noexcept { return failed_; }
        bool asking() const noexcept { return askDisconnect_; }
        Field focus() const noexcept { return focus_; }
        const std::string& remoteText() const noexcept { return remote_.buffer; }
        const std::string& branchText() const noexcept { return branch_.buffer; }
        void setFields(const std::string& remote, const std::string& branch);   // as typed (the probes')
        std::string label(Button b) const;               // a button's words now ("" when it is not there)
        bool enabled(Button b) const;
        funkgui::Point centreOf(Button b, float width, float height) const;

    private:
        funkgui::text::LineEdit& field(Field f) { return f == Field::remote ? remote_ : branch_; }
        const funkgui::text::LineEdit& field(Field f) const { return f == Field::remote ? remote_ : branch_; }
        bool fields() const noexcept { return mode_ == Mode::add || !link_.linked; }
        void focusOn(Field f);
        void read();                                     // the link and the job, from the port
        void press(Button b);
        void start(const GitJob& job, const std::string& doing);
        std::string stateText() const;                   // "3 versions to push", "Up to date, checked 14:02"
        funkgui::Rect rectOf(const Places& at, Button b) const;

        EditorPort& port_;
        bool open_ = false;
        Mode mode_ = Mode::project;
        std::string project_;
        GitLink link_;
        GitState state_;
        uint64_t polled_ = 0;                            // EditorPort::gitPolled as last read
        uint64_t mine_ = 0;                              // the GitState::ended a job this card started will end at
        bool watching_ = false;                          // a job this card started runs
        funkgui::text::LineEdit remote_, branch_;
        Field focus_ = Field::remote;
        bool askDisconnect_ = false;
        std::string line_;                               // its outcome, or what it says
        bool failed_ = false;
        std::string notice_;
        std::string compare_;                            // takeCompare
        uint32_t revision_ = 0;
    };
}
