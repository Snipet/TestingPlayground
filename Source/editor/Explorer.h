// Source/editor/Explorer.h: the explorer, the card at the editor's left (Panel.cpp): what the program is made of and
// what it can be made from, in one list of sections that fold:
//
//   FILES     the program's files, the editor's tabs (the one shown marked, one with errors a red dot): a click shows
//             it, a double click renames it, the cross on a row closes it (not the program, the first); the header's
//             + makes a new file
//   LIBRARY   the library's building blocks by category, a tick on those the program has: a click adds one as a tab
//             (and #includes it), or shows it
//   EXAMPLES  a click puts the example in the program (one edit, Cmd-Z takes it back) and runs it
//   PRESETS   the presets saved on this Mac, the newest first: a click loads one; the header's button opens PRESETS
//
// The Panel builds the sections (setContent) whenever what they show changes, and does what a row asks (Hooks), as
// the tabs, the LIBRARY and EXAMPLES menus and the presets' card do. The card itself, its title and its fold button
// are the Panel's; folded, the explorer is away (EditorPort::explorerOpen, in the session). The list scrolls with
// the wheel; every row is a button to a screen reader, every section header a toggle.
#pragma once

#include "editor/ChatStyle.h"
#include "editor/Items.h"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace testingplaygroundsynth::ui
{
    class ExplorerItem final : public Item
    {
    public:
        enum class Section : uint8_t { files, library, examples, presets };
        static constexpr int kSections = 4;

        struct Row
        {
            std::string label;                        // "program.fm"
            std::string detail;                       // faint, at the right ("2026-10-27 14:02"); "" for none
            chat::Icon icon = chat::Icon::file;
            bool heading = false;                     // a library category's caption: no action
            bool active = false;                      // the tab shown
            bool marked = false;                      // a library file the program has: a tick
            bool error = false;                       // a file with errors: a red dot
            bool closable = false;                    // a cross under the pointer
            int index = -1;                           // what the Panel acts on: a tab, a library file, an example
            std::string key;                          // a preset's name
        };

        struct Group
        {
            Section section = Section::files;
            std::string title;                        // "FILES"
            std::vector<Row> rows;
            chat::Icon action = chat::Icon::none;     // the header's button (none: no button)
            std::string actionTitle;                  // its name to a screen reader ("New file")
            std::string empty;                        // shown when there are no rows ("No presets yet")
        };

        struct Hooks
        {
            std::function<void(Section, const Row&)> open;      // a click
            std::function<void(Section, const Row&)> rename;    // a double click
            std::function<void(Section, const Row&)> close;     // the cross
            std::function<void(Section)> action;                // the header's button
        };

        // a11yBase: the first of its accessibility ids (it uses up to 4096 of them).
        ExplorerItem(uint32_t a11yBase, Hooks hooks);

        void setBounds(funkgui::Rect r);
        funkgui::Rect bounds() const noexcept { return bounds_; }
        // The sections as they are now. The scroll and the folded sections stay.
        void setContent(std::vector<Group> groups);
        bool sectionOpen(Section s) const noexcept { return open_[static_cast<std::size_t>(s)]; }
        void setSectionOpen(Section s, bool open);
        void setHidden(bool hidden) { hidden_ = hidden; }

        // For probes: where a row, a section's header and its button, and a row's cross are, while in view.
        std::optional<funkgui::Point> centreOfRow(Section s, int row) const;
        std::optional<funkgui::Point> centreOfHeader(Section s) const;
        std::optional<funkgui::Point> centreOfAction(Section s) const;
        std::optional<funkgui::Point> centreOfClose(Section s, int row) const;
        int rows(Section s) const;                    // the section's rows (headings included)
        const Row* row(Section s, int i) const;
        float contentHeight() const noexcept { return contentH_; }
        // Bumps when its rows, its folded sections or its scroll change (the Panel's accessibility revision).
        uint32_t revision() const noexcept { return revision_; }
        float scroll() const noexcept { return scroll_; }
        void scrollTo(float y);

        // Item
        void tick(float dt, bool hovered, bool focused, funkgui::Point pointer) override;
        void draw(funkgui::Canvas&, const funkgui::Theme&, bool focusRing) const override;
        bool contains(funkgui::Point p) const override { return bounds_.contains(p); }
        funkgui::Cursor cursorAt(funkgui::Point p) const override;
        void pointerExit() override { pointer_ = { -1.0e4f, -1.0e4f }; }
        void pointerDown(const funkgui::PointerEvent&, funkgui::GestureController&, funkgui::HostServices&) override;
        void doubleClick(const funkgui::PointerEvent&, funkgui::GestureController&) override;
        bool wheel(const funkgui::WheelEvent&, funkgui::GestureController&, double) override;
        void accessibility(std::vector<funkgui::A11yItem>&) const override;
        bool a11yAction(uint32_t id, funkgui::A11yAction, double, funkgui::GestureController&) override;
        void specLine(char* out, std::size_t n) const override;
        funkgui::Point centre() const override { return { bounds_.centreX(), bounds_.centreY() }; }
        bool shown() const override { return !hidden_; }
        bool focusable() const override { return false; }

    private:
        enum class Part : uint8_t { header, action, row, close };
        struct Hit
        {
            Part part = Part::row;
            int group = -1, row = -1;
            funkgui::Rect r;                          // in the content (y from the list's top)
        };

        void layoutList();
        float viewHeight() const noexcept { return bounds_.h; }
        float maxScroll() const noexcept;
        funkgui::Rect onScreen(const funkgui::Rect& r) const;
        int groupOf(Section s) const;
        const Hit* hitAt(funkgui::Point p) const;
        void act(const Hit& h, bool twice);

        uint32_t base_;
        Hooks hooks_;
        funkgui::Rect bounds_;
        std::vector<Group> groups_;
        std::array<bool, kSections> open_{ true, true, true, true };
        std::vector<Hit> hits_;                       // every header, header button, row and cross, in the content
        std::vector<float> rowY_;                     // per group: its header's y
        float contentH_ = 0.0f;
        float scroll_ = 0.0f;
        funkgui::Point pointer_{ -1.0e4f, -1.0e4f };
        bool hidden_ = false;
        uint32_t revision_ = 1;
    };
}
