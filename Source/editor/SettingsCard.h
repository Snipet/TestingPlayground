// Source/editor/SettingsCard.h: SETTINGS, the Standalone's audio and MIDI devices in a card over the panel, in the
// chat's look (ChatStyle.h), in place of JUCE's dialog. It reads and changes the devices through EditorPort::devices
// (plugin/DevicePort.h), which only the Standalone has: in a host there is no card.
//
//   OUTPUT  the output device (a pill: its menu, the host's, lists the type's devices), Test (a tone through it); its
//           channels as switches, a pair each, when it has more than one pair
//   INPUT   an effect's only: the input device (None among them), its pairs, and Mute input with a line on why (a
//           loop through the speakers would feed back); unmuting is the user's click alone
//   AUDIO   the device type (when there are several), the sample rate, the buffer size with its ms, and the latency the
//           device reports (in and out)
//   MIDI    an instrument's only: each MIDI input with a switch, or "No MIDI inputs"
//   Done    or Escape, or a click outside the card: closed
// A device's error (a choice it refused) is under the section whose choice it was, in the danger colour. The numbers
// follow the devices while it is open: it reads them again whenever DevicePort::serial moves (a device plugged in, a
// change made elsewhere). Tab and Shift-Tab (or Down and Up) move the focus over its controls, Return or Space acts
// on the focused one; the wheel scrolls it when the window is too short for it. While it is open it takes every
// pointer event and key, and the rest of the panel is dimmed.
#pragma once

#include "plugin/EditorPort.h"

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace playground::ui
{
    class SettingsCard
    {
    public:
        static constexpr uint32_t kA11yBase = 0x7FFC0000u;   // its accessibility ids, past every item's

        // What a control does.
        enum class Act : uint8_t { type, output, input, rate, buffer, test, outputPair, inputPair, mute, midi, done };

        struct Control
        {
            Act act = Act::done;
            int index = 0;                       // a pair's or a MIDI input's
            funkgui::Rect r;                     // its place (the card's body scrolled)
            std::string label;                   // what it shows
            std::string title;                   // its accessibility title
            bool menu = false;                   // a pill that opens a menu
            bool toggle = false;                 // a switch
            bool on = false;
            bool enabled = true;
            uint32_t id = 0;                     // its accessibility id
        };

        // A text on the card: a section's heading, a row's label, a value, a note, an error line.
        struct Line
        {
            enum class Kind : uint8_t { heading, label, value, note, error };
            Kind kind = Kind::label;
            std::string text;
            funkgui::Rect r;                     // drawn from r.x, centred on r's middle, fitted to r.w
        };

        struct Places
        {
            funkgui::Rect box;                   // the card
            funkgui::Rect body;                  // its sections' part, which scrolls when the window is too short
            float contentH = 0.0f, scroll = 0.0f, maxScroll = 0.0f;
            std::vector<Control> controls;       // in the Tab order; Done last (it does not scroll)
            std::vector<Line> lines;
        };

        explicit SettingsCard(EditorPort& port) : port_(port) {}

        bool isOpen() const noexcept { return open_; }
        bool open();                             // false when there are no devices (a host)
        void close();
        // The devices read again when their serial moved, or forgotten when they went (each frame, open or not: the
        // muted notice reads them too). True when what it holds changed.
        bool sync();
        bool hasDevices() const noexcept { return has_; }
        const DeviceSetup& setup() const noexcept { return setup_; }
        // An effect's input is muted (JUCE's guard against a feedback loop): the Panel's notice says so.
        bool inputMuted() const noexcept { return has_ && setup_.muteApplies && setup_.muted; }

        Places places(float width, float height) const;

        void pointerDown(funkgui::Point p, float width, float height, funkgui::HostServices* host);
        void wheel(const funkgui::WheelEvent& e, float width, float height);
        void key(const funkgui::KeyEvent& e, funkgui::HostServices* host, float width, float height);
        funkgui::Cursor cursor(funkgui::Point p, float width, float height) const;
        void draw(funkgui::Canvas& c, const funkgui::Theme& th, float width, float height, funkgui::Point pointer,
                  bool hasPointer) const;
        void accessibility(std::vector<funkgui::A11yItem>& out, float width, float height) const;
        bool a11yAction(uint32_t id, funkgui::A11yAction a, float width, float height, funkgui::HostServices* host);
        uint32_t revision() const noexcept { return revision_; }   // bumps when what it shows changes

        // For probes: a control's centre (nullopt when it is not on the card, or scrolled out of its body), the
        // focused control (nullopt for none).
        std::optional<funkgui::Point> centreOf(Act act, int index, float width, float height) const;
        std::optional<Control> focused(float width, float height) const;

    private:
        struct Key
        {
            Act act = Act::done;
            int index = -1;                      // -1: none focused
        };
        int focusIndex(const Places& at) const;
        void act(const Control& c, funkgui::HostServices* host);
        void openMenu(const Control& c, funkgui::HostServices* host);
        void changed();                          // after an action: the devices read at once

        EditorPort& port_;
        bool open_ = false;
        bool has_ = false;
        uint64_t seen_ = 0;                      // the serial setup_ was read at
        DeviceSetup setup_;
        Key focus_;
        bool ring_ = false;                      // the focus shown (the keys moved it)
        float scroll_ = 0.0f;
        uint32_t revision_ = 0;
    };
}
