// Source/editor/Explorer.h: the explorer, the card at the editor's left (Panel.cpp): what the program is made of and
// what it can be made from, in one list of sections that fold:
//
//   FILES     the program's files, the editor's tabs (the one shown marked, one with errors a red dot): a click shows
//             it, a double click renames it, the cross on a row closes it (not the program, the first); the header's
//             + makes a new file
//   ASSETS    the program's samples (dsp/Assets.h: WAV, AIFF and FLAC files a program reads with #pragma sample),
//             each with its length, channels, rate and size, a tick on those the program that plays reads: the
//             header's + adds files (a chooser), a file dropped on the panel is added too, the cross on a row removes it
//   FOLDER    the program's files in a folder on disk, synced both ways (plugin/FolderSync.h): off, a row that turns
//             it on; on, Open in VS Code, Show in Finder (the folder's name), Run on save (ticked when on), a row a
//             conflict (a red dot: a click asks which side to keep), and its notes as captions; the header's cross
//             turns it off
//   CLAUDE DESKTOP  this instance as Claude Desktop knows it (EditorPort::desktop; plugin/McpLink.h): its name (a click
//             renames it), Use with Claude Desktop (ticked while it is the target: chosen, or the only one open; a click
//             chooses it, or clears the choice), what Claude Desktop's last call here did and when (lit while one acts
//             on it; a click opens the chat, where it shows), and the other instances open (the target ticked; a click
//             chooses one); none without a socket
//   LLVM IR   what FMinus made of the program (IrView.h), none of its files: a caption saying so, then a row a view
//             (optimised, lowered, and the frozen copy's while it plays; the one shown marked): a click opens the
//             read-only IR tab on it
//   PRESETS   the presets of the project open (dial settings for its program; the one applied marked): a click applies
//             one; the header's + saves the dials as a new one (PROJECTS, its field a preset's name)
//   PROJECTS  the projects saved on this Mac, the newest first, each with its newest version (the one open marked): a
//             click opens its newest version; the header's button opens PROJECTS (to save, or to open an older version).
//             Grouped as PROJECTS groups them (ProjectGroups.h: FAVOURITES, the factory's starred ones among them, a
//             category a group, UNCATEGORISED last): a caption a group, which folds it (the groups folded are
//             PROJECTS', this Mac's: EditorPort::projectsView), unless the one group is UNCATEGORISED (none starred,
//             none given a category: a flat list)
//   FACTORY   the factory projects (plugin/Factory.h), by name (the one open marked), folded at first: a click opens
//             one (read-only: a save makes the user's own copy)
//   LIBRARY   the library's building blocks by category, a tick on those the program has: a click adds one as a tab
//             (and #includes it), or shows it
//   EXAMPLES  a click starts afresh from the example (Panel::loadExample: its files the only tabs, the dials at their
//             defaults, no project open; asking first when changes would be lost) and runs it
//
// The Panel builds the sections (setContent) whenever what they show changes, and does what a row asks (Hooks), as
// the tabs, the LIBRARY and EXAMPLES menus and the presets' card do. The card itself, its title and its fold button
// are the Panel's; folded, the explorer is away (EditorPort::explorerOpen, in the session). The list scrolls as the
// code does (Scroll.h: the wheel, a thumb that drags); every row is a button to a screen reader, every section header
// a toggle.
#pragma once

#include "editor/ChatStyle.h"
#include "editor/Items.h"
#include "editor/Scroll.h"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace playground::ui
{
    class ExplorerItem final : public Item
    {
    public:
        enum class Section : uint8_t { files, presets, projects, library, examples, ir, folder, assets, desktop, factory };
        static constexpr int kSections = 10;

        struct Row
        {
            std::string label;                        // "program.fm"
            std::string detail;                       // faint, at the right ("2026-10-27 14:02"); "" for none
            chat::Icon icon = chat::Icon::file;
            bool heading = false;                     // a library category's caption: no action
            uint8_t fold = 0;                         // a caption that folds its group: 1 open, 2 folded (a click: open())
            bool active = false;                      // the tab shown
            bool marked = false;                      // a library file the program has: a tick
            bool error = false;                       // a file with errors: a red dot
            bool closable = false;                    // a cross under the pointer
            int index = -1;                           // what the Panel acts on: a tab, a library file, an example, an IR view
            std::string key;                          // a preset's or a project's name
            std::string help;                         // to a screen reader ("": the section's)
            std::string spec;                         // the status line's under the pointer ("": the section's; a
                                                      // caption with one says it under the pointer, a caption without
                                                      // is not there for the pointer)
        };

        struct Group
        {
            Section section = Section::files;
            std::string title;                        // "FILES"
            std::vector<Row> rows;
            chat::Icon action = chat::Icon::none;     // the header's button (none: no button)
            std::string actionTitle;                  // its name to a screen reader ("New file")
            std::string empty;                        // shown when there are no rows ("No presets yet")
            bool counted = true;                      // the header counts its rows ("FILES  2")
            int total = -1;                           // the count it says (-1: its rows, captions aside)
        };

        struct Hooks
        {
            std::function<void(Section, const Row&)> open;      // a click
            std::function<void(Section, const Row&)> rename;    // a double click
            std::function<void(Section, const Row&)> close;     // the cross
            std::function<void(Section)> action;                // the header's button
        };

        // a11yBase: the first of its accessibility ids (it uses up to kSections * 512 of them).
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
        float scroll() const noexcept { return scroll_.offset; }
        void scrollTo(float y);
        const Scroll& scroller() const noexcept { return scroll_; }   // its bar is at the item's right

        // Item
        void tick(float dt, bool hovered, bool focused, funkgui::Point pointer) override;
        void draw(funkgui::Canvas&, const funkgui::Theme&, bool focusRing) const override;
        uint64_t drawKey() const override
        {
            return DrawKey{}.mix(revision_).mix(bounds_).mix(pointer_).mix(scroll_.key()).h;
        }
        bool contains(funkgui::Point p) const override { return bounds_.contains(p); }
        funkgui::Cursor cursorAt(funkgui::Point p) const override;
        void pointerExit() override { pointer_ = { -1.0e4f, -1.0e4f }; }
        void pointerDown(const funkgui::PointerEvent&, funkgui::GestureController&, funkgui::HostServices&) override;
        void pointerDrag(const funkgui::PointerEvent&, funkgui::GestureController&) override;
        void pointerUp(const funkgui::PointerEvent&, funkgui::GestureController&) override;
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
        funkgui::Rect onScreen(const funkgui::Rect& r) const;
        int groupOf(Section s) const;
        const Hit* hitAt(funkgui::Point p) const;
        void act(const Hit& h, bool twice);

        uint32_t base_;
        Hooks hooks_;
        funkgui::Rect bounds_;
        std::vector<Group> groups_;
        std::array<bool, kSections> open_{ true, true, true, true, true, true, true, true, true, false };   // FACTORY folded at first
        std::vector<Hit> hits_;                       // every header, header button, row and cross, in the content
        std::vector<float> rowY_;                     // per group: its header's y
        float contentH_ = 0.0f;
        Scroll scroll_;                               // its content: the list's height; its view: the item's
        funkgui::Point pointer_{ -1.0e4f, -1.0e4f };
        bool hidden_ = false;
        uint32_t revision_ = 1;
    };
}
