// Source/editor/DiffCard.h: COMPARE, a card over the panel: what changed between two versions of a project, or between
// a version and what is in the editor now, file by file, the dials and the samples. Nothing plays and nothing opens
// until the user asks.
//
//   the sides   two dropdowns, the left side and the right ("Editor": the editor's files, dials and samples as they are
//               now; or a version: its number, when it was saved and what changed in it), the arrow between them
//               swapping them; under each its time and its note in full
//   the line    what differs: "3 files, 2 dials, 1 sample", or "The same"
//   the view    DiffView.h: a pill a file (DIALS and SAMPLES after them when they differ), the chosen one's rows; ] and
//               [ (and the two small buttons) to the next and the previous change, Tab the next pill, Page Up and Page
//               Down, Home and End
//   Open vN     that version opened (EditorPort::openProject); over changes the editor's files have that no version
//               holds, a second click: Open anyway, as PROJECTS asks
//   A/B...      closes this card and starts A/B with these two sides (the Panel's A/B card, AbCard.h)
//   Close       or Escape, or a click outside the card
// A version's content comes through EditorPort::versionPolled (the store's version table; a file it lacks read on the
// store's Reader): until both sides are read it says "Reading version 12..." in the view's place, and it asks again each
// frame that projectsRead() moved (tick()), as PROJECTS does. While it is open it takes every pointer event and key.
// Its menus are the host's (the sides' dropdowns); a menu's answer calls the Panel's hook (setAnswered).
#pragma once

#include "editor/DiffView.h"
#include "plugin/EditorPort.h"

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <tuple>
#include <vector>

namespace playground::ui
{
    class DiffCard
    {
    public:
        static constexpr uint32_t kA11yBase = 0x7FF90000u;   // its accessibility ids, past every item's (and the other cards')
        static constexpr int kEditor = 0;                // a side that is the editor's (else a version's number)
        // Package 05: a side that is a diverged project's remote side (EditorPort::gitTheirs: read into memory by the git
        // job, nothing written): no Open and no A/B while it shows.
        static constexpr int kTheirs = -1;
        bool theirsShown() const noexcept { return sides_[0] == kTheirs || sides_[1] == kTheirs; }

        explicit DiffCard(EditorPort& port) : port_(port) {}

        bool isOpen() const noexcept { return open_; }
        // Opens on `project`'s versions, `left` and `right` its sides (version numbers, kEditor the editor's). False, with
        // why(), when there is no such project.
        bool open(const std::string& project, int left, int right);
        void close();
        void tick();                                     // each frame: the sides and the versions read again when the
                                                         // store's Reader read a file, the editor's when its files changed
        void setSide(int side, int version);             // side 0 the left, 1 the right
        void swap();

        struct Places
        {
            funkgui::Rect box, left, right, swap, view, openLeft, openRight, ab, close;
        };
        Places places(float width, float height) const;

        void pointerDown(funkgui::Point p, funkgui::HostServices* host, float width, float height);
        void pointerDrag(funkgui::Point p, float width, float height);
        void pointerUp();
        void wheel(const funkgui::WheelEvent& e, float width, float height);
        void key(const funkgui::KeyEvent& e, funkgui::HostServices* host, float width, float height);
        funkgui::Cursor cursor(funkgui::Point p, float width, float height) const;
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, float width, float height, funkgui::Point pointer,
                  bool hasPointer) const;
        void accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const;
        bool a11yAction(uint32_t id, funkgui::A11yAction a, funkgui::HostServices* host, float width, float height);
        uint32_t revision() const noexcept { return revision_ + view_.revision(); }   // moves when what it draws changes

        // The Panel's: a menu's answer has acted, and whether a menu opens at a rect (its trigger then drawn open).
        void setAnswered(std::function<void()> answered) { answered_ = std::move(answered); }
        void setMenuOpen(std::function<bool(const funkgui::Rect&)> menuOpen) { menuOpen_ = std::move(menuOpen); }
        // What the Panel does next, once: A/B of these sides (the project, the left, the right; "" for none); what it
        // did ("OPENED V3 OF ENGINE"), for the status.
        std::tuple<std::string, int, int> takeAb();
        std::string takeNotice();

        // For probes.
        const std::string& project() const noexcept { return project_; }
        int side(int k) const noexcept { return sides_[static_cast<std::size_t>(k)]; }
        bool reading() const;                            // a side not read yet
        const std::string& summary() const noexcept { return summary_; }   // "3 files, 2 dials", "The same", ...
        const std::string& why() const noexcept { return why_; }
        const DiffView& view() const noexcept { return view_; }
        const std::vector<VersionEntry>& versionList() const noexcept { return versions_; }
        std::string sideLabel(int k) const;              // "Editor", "v12"
        std::vector<std::string> choices() const;        // the dropdowns' rows: "Editor", "v15  10-27 14:02  note", ...
        void showMenu(int side, funkgui::HostServices* host, float width, float height);   // a side's dropdown
        funkgui::Rect viewArea(float width, float height) const { return places(width, height).view; }

    private:
        void readVersions();
        void readSide(int k);
        void remake();                                   // the diff, once both sides are read
        void openSide(int k);                            // Open vN
        bool askingOpen(int k) const noexcept { return ask_ == k; }

        EditorPort& port_;
        bool open_ = false;
        std::string project_;
        std::vector<VersionEntry> versions_;
        std::array<int, 2> sides_{ 0, 0 };
        std::array<VersionContent, 2> content_{};
        uint64_t readSeen_ = 0, draftSeen_ = 0;
        DiffView view_;
        std::string summary_, why_, notice_;
        std::tuple<std::string, int, int> ab_{ "", 0, 0 };
        int ask_ = -1;                                   // Open asked over the editor's changes: that side's button says
                                                         // Open anyway
        std::function<void()> answered_;
        std::function<bool(const funkgui::Rect&)> menuOpen_;
        uint32_t revision_ = 0;
    };
}
