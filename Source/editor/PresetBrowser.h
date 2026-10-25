// Source/editor/PresetBrowser.h: PRESETS, a card over the panel, as the key dialog is: the presets saved on this Mac
// (EditorPort::presets), to load one, save the editor's files and dials as one, or delete one.
//
//   the list   a row a preset, its name and when it was saved, the one loaded or saved last marked; a click chooses
//              one (its name goes into the field), a double-click loads it; the wheel scrolls; Up and Down choose
//   the field  the name Save saves under, the chosen preset's or the program's own to start with
//   Save       the editor's files and dials under that name (when a preset has it, a second click: Replace)
//   Load       the chosen preset (when the editor's files have changes no preset holds, a second click: Load anyway)
//   Delete     the chosen preset (a second click: Delete it)
//   Close      or Escape. Return saves when a name was typed, else loads the chosen preset
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

#include <cstdint>
#include <string>
#include <vector>

namespace testingplayground::ui
{
    class PresetBrowser
    {
    public:
        static constexpr int kMaxNameLength = 40;        // presetstore::kMaxName
        static constexpr uint32_t kA11yBase = 0x7FFE0000u;   // its accessibility ids, past every item's

        explicit PresetBrowser(EditorPort& port) : port_(port) {}

        bool isOpen() const noexcept { return open_; }
        void open(const std::string& name);              // the list read afresh, the field `name` (selected)
        void close();

        // Its places, for the panel's size.
        struct Places
        {
            funkgui::Rect box, list, field, save, load, remove, close;
            float rowH = 28.0f;
            int rows = 0;                                // how many rows the list shows
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

        // What it did last ("PRESET SAVED: GLASS BLOOM"), once; "" for nothing new.
        std::string takeNotice();

        // For probes.
        const std::vector<PresetEntry>& entries() const noexcept { return list_; }
        int chosen() const noexcept { return chosen_; }
        int firstShown() const noexcept { return first_; }
        const std::string& name() const noexcept { return name_.buffer; }
        const std::string& why() const noexcept { return why_; }
        funkgui::Point centreOfRow(int i, float width, float height) const;   // a row shown, else far away

    private:
        enum class Ask : uint8_t { none, replace, load, remove };

        int rowAt(funkgui::Point p, const Places& at) const;
        void choose(int i, int rowsShown);
        void save();
        void load();
        void remove();
        std::string saveLabel() const { return ask_ == Ask::replace ? "Replace" : "Save"; }
        std::string loadLabel() const { return ask_ == Ask::load ? "Load anyway" : "Load"; }
        std::string removeLabel() const { return ask_ == Ask::remove ? "Delete it" : "Delete"; }

        EditorPort& port_;
        bool open_ = false;
        std::vector<PresetEntry> list_;
        std::string current_;                            // EditorPort::presetName when it opened
        int chosen_ = -1;
        int first_ = 0;                                  // the first row shown
        funkgui::text::LineEdit name_;
        bool typed_ = false;                             // a name typed since it opened, or since a row was chosen
        Ask ask_ = Ask::none;                            // what the next click on that button does
        std::string why_;                                // the line under the field
        std::string notice_;
        uint32_t revision_ = 0;
    };
}
