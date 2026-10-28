// Source/editor/ProjectBrowser.h: PROJECTS, a card over the panel, as the key dialog is: the projects saved on this Mac
// (EditorPort::projects), each a program and its dials kept as versions, with presets (dial settings for it).
//
//   PROJECTS   a row a project, its name, its newest version and when it was saved; the one open marked. A click
//              chooses one (its name goes into the field, its versions and presets show at the right), a double-click
//              opens its newest version
//   VERSIONS   the chosen project's, the newest first, each with when and what changed; the one open marked. A click
//              chooses one, a double-click opens it
//   PRESETS    the chosen project's dial settings; the one applied marked. A click chooses one (its name goes into the
//              field, which then names a preset), a click under the rows starts a new one, a double-click applies it
//   the field  the name Save saves under: a project's, or, while the presets are chosen from, a preset's (its pill
//              says which)
//   Save       a project: the editor's files and dials as a new version of the project named (the one open, or a new
//              one; another's, a second click: Add version). A preset: the dials as a preset of the project open (one
//              of that name, a second click: Replace)
//   Open       the chosen project's newest version, or the chosen version; Apply, the chosen preset (its project
//              opened first when it is not the one open). When the editor's files have changes no version holds, a
//              second click: Open anyway
//   Delete     the chosen project, version or preset (a second click: Delete it)
//   Close      or Escape. Return saves when a name was typed, else opens (applies); Up and Down choose in the list
//              chosen from, Tab moves to the next list
// While it is open it takes every pointer event and key. What it did, it says once (takeNotice) for the status line.
#pragma once

#include "plugin/EditorPort.h"

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>
#include <funkgui/text/LineEdit.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace testingplayground::ui
{
    class ProjectBrowser
    {
    public:
        static constexpr int kMaxNameLength = 40;        // projectstore::kMaxName
        static constexpr uint32_t kA11yBase = 0x7FFE0000u;   // its accessibility ids, past every item's

        enum class List : uint8_t { projects, versions, presets };

        explicit ProjectBrowser(EditorPort& port) : port_(port) {}

        bool isOpen() const noexcept { return open_; }
        // The lists read afresh, the project open chosen; the field `name` (a project's), or, with `preset`, the
        // presets chosen from and the field the preset applied or a new preset's name.
        void open(const std::string& name, bool preset);
        void close();

        // Its places, for the panel's size.
        struct Places
        {
            funkgui::Rect box, projects, versions, presets, mode, field, save, open, remove, close;
            float rowH = 28.0f;
            std::array<int, 3> rows{};                   // how many rows each list shows
        };
        Places places(float width, float height) const;

        // Events, the panel's size given. The pointer goes nowhere else while it is open.
        void pointerDown(funkgui::Point p, float width, float height);
        void doubleClick(funkgui::Point p, float width, float height);
        void wheel(const funkgui::WheelEvent& e, float width, float height);
        void key(const funkgui::KeyEvent& e, funkgui::HostServices* host, float width, float height);
        funkgui::Cursor cursor(funkgui::Point p, float width, float height) const;
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, float width, float height, funkgui::Point pointer,
                  bool hasPointer) const;
        void accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const;
        bool a11yAction(uint32_t id, funkgui::A11yAction a, float width, float height);
        uint32_t revision() const noexcept { return revision_; }   // bumps when its items change

        // What it did last ("SAVED: GLASS BLOOM V3"), once; "" for nothing new.
        std::string takeNotice();

        // For probes.
        const std::vector<ProjectEntry>& projectList() const noexcept { return projects_; }
        const std::vector<VersionEntry>& versionList() const noexcept { return versions_; }
        const std::vector<PresetEntry>& presetList() const noexcept { return presets_; }
        int chosen(List l) const noexcept { return chosen_[static_cast<std::size_t>(l)]; }
        List focus() const noexcept { return focus_; }
        bool presetMode() const noexcept { return focus_ == List::presets; }
        const std::string& name() const noexcept { return name_.buffer; }
        const std::string& why() const noexcept { return why_; }
        std::string saveLabel() const;
        std::string openLabel() const;
        std::string removeLabel() const;
        funkgui::Point centreOfRow(List l, int i, float width, float height) const;   // a row shown, else far away
        funkgui::Point centreOfEmpty(List l, float width, float height) const;        // under its rows, else far away

    private:
        enum class Ask : uint8_t { none, addTo, replace, open, remove };

        funkgui::Rect listRect(const Places& at, List l) const;
        int count(List l) const;
        int rowAt(funkgui::Point p, const Places& at, List l) const;
        void readChosen();                               // the chosen project's versions and presets
        void choose(List l, int i, int rowsShown);
        void startPreset();                              // the presets chosen from, the field a new preset's name
        void save();
        void openChosen();
        void remove();
        bool chosenIsOpen() const;                       // the chosen project is the one open
        std::string chosenProject() const;

        EditorPort& port_;
        bool open_ = false;
        std::vector<ProjectEntry> projects_;
        std::vector<VersionEntry> versions_;
        std::vector<PresetEntry> presets_;
        ProjectState state_;                             // EditorPort::project when it opened, or after it acted
        std::array<int, 3> chosen_{ -1, -1, -1 };
        std::array<int, 3> first_{ 0, 0, 0 };            // each list's first row shown
        List focus_ = List::projects;
        funkgui::text::LineEdit name_;
        bool typed_ = false;                             // a name typed since it opened, or since a row was chosen
        Ask ask_ = Ask::none;                            // what the next click on that button does
        std::string why_;                                // the line under the field
        std::string notice_;
        uint32_t revision_ = 0;
    };
}
