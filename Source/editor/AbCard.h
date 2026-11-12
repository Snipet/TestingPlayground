// Source/editor/AbCard.h: A/B, a card over the panel (from PROJECTS' A/B...): two versions of a project, or the editor's
// files and a version, played in turn while the host plays (EditorPort::ab: the processor's A/B listening), with what
// changed between them, file by file:
//
//   VERSIONS  "Current" (the editor's files as they were when it began) and the project's versions, the newest first;
//             each row's A and B chips choose that side (a click on a row elsewhere makes it B, Up and Down move B,
//             Shift with them A)
//   CHANGES   a tab a file A or B has (its lines added and taken away; "new" in B only, "gone" in A only, "same"), and
//             the chosen file's lines as a unified diff: A's line numbers, B's, the lines taken away and added tinted,
//             three lines of context around each change and the lines between said in one; the wheel, its thumb, Page Up
//             and Page Down, Home and End scroll it, Tab moves to the next file
//   A  B      which side plays (A and B, or Space, switch; the one on its way is outlined until its compile lands)
//   Dials     the same for both, or each its own (a version's saved ones, Current's as they were)
//   Match     the louder side turned down to the quieter's loudness (measured offline: AbMatch.h), and by how much
//   Keep A, Keep B   that side made current: its files the editor's (one undo step), a version's the project open
//             (Return keeps the side that plays)
//   Close     or Escape: back to what played before
// An editor opened while an A/B is on (the window closed and opened again) shows its card at once (attach).
// While it is open it takes every pointer event and key. The A/B is the processor's: the card shows it (its revision
// followed each frame: tick()), and closes itself when it ends on its own (another program ran), saying so once
// (takeNotice). The versions and the diff scroll as the code does (Scroll.h), by px.
#pragma once

#include "editor/Scroll.h"
#include "plugin/EditorPort.h"

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>

#include <cstdint>
#include <string>
#include <vector>

namespace playground::ui
{
    class AbCard
    {
    public:
        static constexpr uint32_t kA11yBase = 0x7FFB0000u;   // its accessibility ids, past every item's (and the other cards')
        static constexpr int kContext = 3;                   // lines around a change
        static constexpr int kMaxDiffRows = 6000;            // a file's diff, at most (the rest said in one row)

        explicit AbCard(EditorPort& port) : port_(port) {}

        bool isOpen() const noexcept { return open_; }
        // Starts the A/B on `project`'s versions: A Current, B `version` (0: the newest; none when there is no project).
        // False, with why() saying so, when it cannot (the card stays closed).
        bool open(const std::string& project, int version);
        // The A/B that is on already (an editor opened while it plays): shown as it is. False when none is on.
        bool attach();
        void close();                                    // Close, Escape: EditorPort::endAb, back to what played before
        void keep(int side);                             // Keep A, Keep B: EditorPort::keepAb
        void play(int side);                             // EditorPort::playAb
        // Each frame: the A/B's state followed (its revision), the diff made again when the sides change; closes the card
        // when the A/B ended on its own.
        void tick();

        // Its places, for the panel's size.
        struct Places
        {
            funkgui::Rect box, list, diff, playA, playB, same, own, match, close, keepA, keepB;
            std::vector<funkgui::Rect> tabs;             // the files' tabs shown (from the first shown)
            float rowH = 28.0f, lineH = 16.0f;
            int rows = 1, lines = 1;                     // the version rows and the diff's lines that fit
        };
        Places places(float width, float height) const;

        void pointerDown(funkgui::Point p, float width, float height);
        void pointerDrag(funkgui::Point p, float width, float height);   // a thumb's drag, while pressed
        void pointerUp();
        void wheel(const funkgui::WheelEvent& e, float width, float height);
        void key(const funkgui::KeyEvent& e, float width, float height);
        funkgui::Cursor cursor(funkgui::Point p, float width, float height) const;
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, float width, float height, funkgui::Point pointer,
                  bool hasPointer) const;
        void accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const;
        bool a11yAction(uint32_t id, funkgui::A11yAction a, float width, float height);
        uint32_t revision() const noexcept { return revision_; }   // bumps when what it draws changes

        std::string takeNotice();                        // what it did ("KEPT B: V3"), once; "" for nothing new
        const std::string& why() const noexcept { return why_; }

        // One file's changes from A to B.
        enum class Change : uint8_t { same, changed, added, removed };   // added: in B only; removed: in A only
        struct Line
        {
            enum class Kind : uint8_t { same, removed, added, gap } kind = Kind::same;
            int a = 0, b = 0;                            // its line numbers in A and in B, from 1 (0: none)
            std::string text;                            // the line (ASCII, tabs as spaces); a gap's words
        };
        struct FileDiff
        {
            std::string name;
            Change change = Change::same;
            int added = 0, removed = 0;
            std::vector<Line> lines;
        };
        // Every file of A or B (A's order, then B's own), its changes as the card shows them.
        static std::vector<FileDiff> diffOf(const Sources& a, const Sources& b);
        static std::string tabLabel(const FileDiff& f);  // "program.fm +12 -3", "lib.fm new", "old.fm gone", "panel.gui"

        // For probes.
        const AbState& state() const noexcept { return state_; }
        const std::vector<VersionEntry>& versionList() const noexcept { return versions_; }   // the project's, newest first
        const std::vector<FileDiff>& files() const noexcept { return diffs_; }
        int file() const noexcept { return file_; }      // the tab shown
        int scroll() const noexcept;                     // the diff's first line wholly shown
        Scroll listScroll(const Places& at) const;       // the versions' scroll and the diff's: content and view as the
        Scroll diffScroll(const Places& at) const;       // places have them
        funkgui::Rect listBar(const Places& at) const;   // where their bars are (Scroll's areas)
        funkgui::Rect diffBar(const Places& at) const;
        // A row's centre (row 0 Current, then the versions), or its A or B chip's (`chip` 0, 1; -1: the row's middle);
        // far away when it is not shown.
        funkgui::Point centreOfRow(int row, int chip, float width, float height) const;
        funkgui::Point centreOfTab(int i, float width, float height) const;

    private:
        int rowCount() const noexcept { return 1 + static_cast<int>(versions_.size()); }
        int versionOfRow(int row) const;                 // 0 for Current
        int rowOfVersion(int version) const;
        void setSide(int side, int row);                 // that side to the row's version
        void follow();                                   // state_ from the port, the diff when the sides changed
        void showFile(int i);
        funkgui::Rect rowRect(const Places& at, int row) const;   // a version's row, scrolled
        int rowAt(funkgui::Point p, const Places& at) const;
        float chipX(const funkgui::Rect& row, int chip) const;

        EditorPort& port_;
        bool open_ = false;
        AbState state_;
        std::vector<VersionEntry> versions_;
        std::vector<FileDiff> diffs_;
        Sources diffA_, diffB_;                          // the sides the diff is of
        int file_ = 0, firstTab_ = 0;
        Scroll diff_, list_;                             // their offsets kept (diffScroll, listScroll)
        std::string why_;                                // the line at the foot: what went wrong
        std::string notice_;
        uint32_t revision_ = 0;
    };
}
