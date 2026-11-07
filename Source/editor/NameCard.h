// Source/editor/NameCard.h: this instance's name, a card over the panel, as the key dialog is (from the explorer's CLAUDE
// DESKTOP: a click on its name). What Claude Desktop and the plugin call it ("Bass"; an empty field: its number's name,
// "Synth 2", which keeps reaching it as its number does), kept with the session (EditorPort::setDesktopName). Save (or
// Return) keeps it and closes; Cancel (or Escape) leaves it as it was; a name it cannot take (another's number's name, a
// number alone, one too long: mcp::target::nameRefusal) is said under the field, and the card stays. While it is open it
// takes every pointer event and key.
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
    class NameCard
    {
    public:
        static constexpr uint32_t kA11yBase = 0x7FFA0000u;   // its accessibility ids, past every item's (and the other cards')
        static constexpr int kMaxLength = 32;                // mcp::target::kMaxName

        explicit NameCard(EditorPort& port) : port_(port) {}

        bool isOpen() const noexcept { return open_; }
        void open();                                     // the field its name now (empty for its number's), selected
        void close();

        struct Places
        {
            funkgui::Rect box, field, save, cancel;
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

        std::string takeNotice();                        // what it did, once ("NAMED: BASS"), for the status line
        const std::string& text() const noexcept { return field_.buffer; }
        const std::string& why() const noexcept { return why_; }

    private:
        void save();

        EditorPort& port_;
        bool open_ = false;
        std::string number_;                             // "Synth 2": what an empty field means
        funkgui::text::LineEdit field_;
        std::string why_;
        std::string notice_;
        uint32_t revision_ = 0;
    };
}
