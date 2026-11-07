// Source/editor/PresetPicker.h: the top bar's list of the project open's presets (EditorPort::presetCycle), opened by a
// click on the preset's name between the top bar's arrows: compact, under the name, over everything. Not PROJECTS: no
// versions, no other project, nothing saved but a preset's category.
//
//   the field   the search, which has the keys as soon as it opens: the presets whose name or category holds each
//               word typed (case aside); Escape clears it, a second Escape closes. After a row's menu's New
//               category..., the category being named (its pill CATEGORY): Return sets it, an empty one refused (None
//               clears one), Escape goes back to the search
//   the rows    in groups by the presets' category (EditorPort::PresetEntry::category: one a category told apart case
//               aside, titled by its least spelling, the groups by name), UNCATEGORISED last; no captions while none
//               has a category. A row a preset: its name, its number at the right (the host's program and MIDI's
//               program change, 1 the first: the cycle's order, by name, whatever the groups), the one applied or
//               matched marked (a ring and its name strong; the modified dot when edited). Up and Down move the
//               highlight (the current one at first, else the first), Return or a click applies it and closes
//               (EditorPort::applyPreset: the dials glide there; changes to the one applied are not kept, as when
//               PROJECTS applies another)
//   the menu    a right-click on a row: Apply, Category (this project's presets' categories, None (Uncategorised), New
//               category...); a factory project's Category greyed (its presets stay as they are)
// A press outside the list closes it, and goes no further (Panel). What it did, it says once (takeNotice) for the
// status line. Its menus are the host's; a menu's answer calls the Panel's hook (setAnswered) as an event does.
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
#include <functional>
#include <string>
#include <vector>

namespace playground::ui
{
    // What applying a preset says in the status line: "PRESET: DARK  2/5", and, when the one applied before had its
    // dials moved, that those changes were not saved.
    std::string presetNotice(const PresetCycle& before, const PresetCycle& after);

    class PresetPicker
    {
    public:
        static constexpr int kMaxSearchLength = 40;
        static constexpr int kMaxCategoryLength = 24;    // projectstore::kMaxTag
        static constexpr uint32_t kA11yBase = 0x7FFA0000u;   // its accessibility ids, past every item's (and the cards')

        explicit PresetPicker(EditorPort& port) : port_(port) {}

        bool isOpen() const noexcept { return open_; }
        // Opened under `anchor` (the top bar's preset name), the cycle read afresh, the search empty, the current
        // preset highlighted.
        void open(const funkgui::Rect& anchor);
        void close();
        void setSize(float width, float height) noexcept { width_ = width; height_ = height; }   // the panel's, before open()
        void setAnchor(const funkgui::Rect& anchor);     // the name moved (a layout)
        // The cycle again (each frame while open): another instance's preset, a category set; the highlight kept on
        // its preset.
        void sync();

        struct Places
        {
            funkgui::Rect box, field, mode, clear, list;
            float rowH = 26.0f;
            int rows = 0;                                // how many rows the list shows
        };
        Places places(float width, float height) const;

        // Events, the panel's size given. A press outside the box closes it (pointerDown returns false then: the
        // press is the Panel's to drop).
        bool pointerDown(const funkgui::PointerEvent& e, funkgui::HostServices* host, float width, float height);
        void wheel(const funkgui::WheelEvent& e, float width, float height);
        void key(const funkgui::KeyEvent& e, funkgui::HostServices* host, float width, float height);
        funkgui::Cursor cursor(funkgui::Point p, float width, float height) const;
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, float width, float height, funkgui::Point pointer,
                  bool hasPointer) const;
        void accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const;
        bool a11yAction(uint32_t id, funkgui::A11yAction a, funkgui::HostServices* host, float width, float height);
        uint32_t revision() const noexcept { return revision_; }   // bumps when what it shows changes
        void setAnswered(std::function<void()> answered) { answered_ = std::move(answered); }
        std::string takeNotice();                        // what it did ("PRESET: DARK 2/5"), once; "" for nothing new

        // For probes. A row: a group's caption (preset -1) or a preset (its index in cycle().presets).
        struct Row
        {
            int group = -1;
            int preset = -1;
        };
        struct Group
        {
            std::string title;                           // "Leads", or "Uncategorised"
            std::string category;                        // "" for the uncategorised
            std::vector<int> members;                    // indexes into cycle().presets, by name
        };
        const PresetCycle& cycle() const noexcept { return cycle_; }
        const std::vector<Group>& groups() const noexcept { return groups_; }
        const std::vector<Row>& rowList() const noexcept { return rows_; }
        bool captions() const noexcept { return captions_; }   // the groups have captions (some preset has a category)
        int highlighted() const noexcept { return lit_; }       // a row (-1: none)
        int firstShown() const noexcept { return first_; }
        int rowOf(const std::string& preset) const;      // its row (-1: not listed)
        const std::string& search() const noexcept { return field_.buffer; }
        bool categoryMode() const noexcept { return categoryMode_; }
        const std::string& why() const noexcept { return why_; }
        funkgui::Point centreOfRow(int row, float width, float height) const;   // a row shown, else far away
        // The categories its menu offers: this project's presets' own, each once (its group's spelling), by name.
        std::vector<std::string> categoryChoices() const;

    private:
        void arrange();                                  // the groups and the rows from the cycle and the search
        void light(int row, int rowsShown);              // a preset's row highlighted and scrolled into view
        void step(int dir, int rowsShown);               // the next or the one before (captions skipped)
        void apply(int row);                             // its preset applied, the list closed
        void setCategory(const std::string& preset, const std::string& typed);
        void showMenu(int row, funkgui::Point at);
        int rowAt(funkgui::Point p, const Places& at) const;
        funkgui::Rect rowRect(const Places& at, int row) const;

        EditorPort& port_;
        funkgui::HostServices* host_ = nullptr;
        std::function<void()> answered_;
        bool open_ = false;
        funkgui::Rect anchor_;
        PresetCycle cycle_;
        std::vector<Group> groups_;
        std::vector<Row> rows_;
        bool captions_ = false;
        int lit_ = -1, first_ = 0;
        funkgui::text::LineEdit field_;                  // the search, or the category being named
        std::string searchKept_;                         // the search while the field names a category
        bool categoryMode_ = false;
        std::string categoryOf_;                         // the preset whose category is being named
        std::string why_;                                // the field's line (a refusal)
        std::string notice_;
        uint32_t revision_ = 1;
        float width_ = 0.0f, height_ = 0.0f;
    };
}
