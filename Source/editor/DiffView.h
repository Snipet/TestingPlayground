// Source/editor/DiffView.h: what changed from one side to another (two versions of a project, a version and the
// editor's files), as the A/B card (AbCard.h) and the Compare card (DiffCard.h) show it, in an area they give it:
//
//   the pills   a pill a file either side has ("program.fm +12 -3"; "lib.fm new" in the right side only, "old.fm gone"
//               in the left only; "panel.gui" the same), then DIALS and SAMPLES when the sides' differ (dialsDiff,
//               samplesDiff; the A/B card gives none); a click or Tab chooses one
//   the rows    the chosen pill's lines as a unified diff: the left side's line numbers, the right's, the lines taken
//               away tinted with `danger` and those added with the accent, three lines of context around each change
//               and the lines between said in one ("12 lines the same"), at most kMaxDiffRows. A netlist's drawing (its
//               `*@` lines, fminus/CirDoc.h: layout, not the program) is one row, "the drawing changed", wherever a run
//               of them changed. DIALS: a row a dial that differs, its value each side as the panel says it; SAMPLES: a
//               row a sample added, removed or replaced
//   scrolling   as the code scrolls (Scroll.h, by px): the wheel, the thumb, Page Up and Page Down, Home and End
//   changes     with changeKeys(): ] and [ (and two small buttons at the pills' right) to the next and the previous run
//               of changed rows, on into the next pill that has one
// Pure but for its own scroll and choice: it draws what it was given (set()), reads no port.
#pragma once

#include "editor/Scroll.h"
#include "plugin/EditorPort.h"

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/Input.h>

#include <cstdint>
#include <string>
#include <vector>

namespace playground::ui
{
    class DiffView
    {
    public:
        static constexpr int kContext = 3;               // lines around a change
        static constexpr int kMaxDiffRows = 6000;        // a file's diff, at most (the rest said in one row)
        static constexpr float kTabH = 26.0f;

        // One file's (or the dials', the samples') changes from the left side to the right.
        enum class Change : uint8_t { same, changed, added, removed };   // added: in the right only; removed: in the left only
        struct Line
        {
            // same, removed, added: a line of the file (a dial, a sample); gap: the lines between said in one; drawing:
            // a netlist's drawing lines that changed, in one; changed: a dial or a sample on both sides, otherwise
            enum class Kind : uint8_t { same, removed, added, gap, drawing, changed } kind = Kind::same;
            int a = 0, b = 0;                            // its line numbers in A and in B, from 1 (0: none)
            std::string text;                            // the line (ASCII, tabs as spaces); a gap's words
        };
        enum class Section : uint8_t { file, dials, samples };
        struct FileDiff
        {
            std::string name;
            Change change = Change::same;
            int added = 0, removed = 0;
            std::vector<Line> lines;
            Section section = Section::file;
            int drawing = 0;                             // a netlist's drawing rows (Line::Kind::drawing)
        };
        // Every file of A or B (A's order, then B's own), its changes as the view shows them.
        static std::vector<FileDiff> diffOf(const Sources& a, const Sources& b);
        // The dials that differ (by name: a value said otherwise, or one side's alone), none when none does; the
        // samples added, removed or replaced (by name, their files' hashes). `left` and `right` name the sides.
        static FileDiff dialsDiff(const std::vector<ContentDial>& a, const std::vector<ContentDial>& b,
                                  const std::string& left, const std::string& right);
        static FileDiff samplesDiff(const std::vector<ContentSample>& a, const std::vector<ContentSample>& b);
        // "program.fm +12 -3", "lib.fm new", "old.fm gone", "panel.gui"; "pedal.cir drawing"; "DIALS 2", "SAMPLES 1".
        static std::string tabLabel(const FileDiff& f);

        // What it shows, the scroll at 0. `keep`: the pill of the same name stays chosen (else the first); else the first
        // that changed.
        void set(std::vector<FileDiff> diffs, bool keep);
        void setSides(std::string left, std::string right);   // their names in its words ("A", "B"; "v12", "Editor")
        void setChangeKeys(bool on) noexcept { changeKeys_ = on; }

        struct Places
        {
            std::vector<funkgui::Rect> tabs;             // the pills shown (from the first shown)
            funkgui::Rect diff, previous, next;          // the rows; the change buttons (empty without changeKeys)
            float lineH = 16.0f;
            int lines = 1;                               // the rows that fit
        };
        Places places(const funkgui::Rect& area) const;

        // Events in `area`: whether it took them.
        bool pointerDown(funkgui::Point p, const funkgui::Rect& area);
        bool pointerDrag(funkgui::Point p, const funkgui::Rect& area);
        bool pointerUp();
        bool wheel(const funkgui::WheelEvent& e, const funkgui::Rect& area);
        bool key(const funkgui::KeyEvent& e, const funkgui::Rect& area);   // Tab, Page Up/Down, Home, End; ] [
        bool hand(funkgui::Point p, const funkgui::Rect& area) const;      // a pill or a button under the pointer
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, const funkgui::Rect& area, funkgui::Point pointer,
                  bool hasPointer) const;
        // Its pills (`tabBase` + the pill), the rows (`diffId`) and the change buttons (`buttonBase`, + 1), under `parent`.
        void accessibility(std::vector<funkgui::A11yItem>& out, const funkgui::Rect& area, uint32_t parent, uint32_t tabBase,
                           uint32_t diffId, uint32_t buttonBase = 0) const;
        bool a11yAction(uint32_t id, const funkgui::Rect& area, uint32_t tabBase, uint32_t buttonBase = 0);

        // The next (+1) or the previous (-1) run of changed rows, from the rows shown, on into the pills after (before);
        // false when there is none.
        bool step(int dir, const funkgui::Rect& area);
        void showFile(int i);

        const std::vector<FileDiff>& files() const noexcept { return diffs_; }
        int file() const noexcept { return file_; }      // the pill shown
        int scroll() const noexcept;                     // the first row wholly shown
        Scroll diffScroll(const Places& at) const;       // the rows' scroll as the places have it
        funkgui::Rect diffBar(const Places& at) const;
        funkgui::Point centreOfTab(int i, const funkgui::Rect& area) const;   // far away when not shown
        uint32_t revision() const noexcept { return revision_; }   // bumps when what it draws changes

    private:
        std::vector<int> runStarts(int file) const;      // where its runs of changed rows start
        void reveal(int row, const funkgui::Rect& area); // that row near the top

        std::vector<FileDiff> diffs_;
        int file_ = 0, firstTab_ = 0;
        Scroll diff_;
        std::string left_ = "A", right_ = "B";
        bool changeKeys_ = false;
        uint32_t revision_ = 0;
    };
}
