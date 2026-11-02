// Source/plugin/standalone/RacingWheel.h: a Logitech G29 racing wheel (its pedals and the Driving Force Shifter on its
// one cable) as a MIDI input of the Standalone, over IOKit's IOHIDManager (C API, no JUCE), scheduled on the main run
// loop: the message thread, in every run loop mode (a menu or a drag open, it still reads). Compiled into the
// Standalone only, like App.cpp; Devices.h owns one (an instrument's only) and lists it among the MIDI inputs.
//
// It watches for a G29 (plugin/WheelMap.h's kindOf: vendor 0x046D, any of its three product ids) and, while it is on:
//   - a G29 in compatibility mode (0xC294, its switch at PS3 without Logitech's software) is switched to its own mode
//     with hid-lg4ff's two reports (wheel::kToNative); it leaves and comes back as 0xC24F (status: "switching to its
//     full mode", or after 8 s without it, what to do);
//   - a G29 in its own mode (0xC24F) is set up as a game does (hid-lg4ff's commands: 900 degrees, wheel::kRange, and a
//     moderate autocentre spring, wheel::kSpring) and read: its elements' values go through a wheel::Map, and the
//     messages that come out (control changes, and the start key's note) are handed to Hooks::send, at most every
//     4 ms (a report's values together);
//   - a G29 at PS4 (0xC260) is not read (status: "set the switch on the wheel to PS3").
// Switched off, or the wheel leaving: the spring stops (wheel::kSpringOff) and what it sent goes back to rest
// (Map::rest: centred, released, neutral, no button, the start key's note off), so the synth holds no throttle open. On quitting, the spring
// stops. Off, the device is open (non-exclusively: a game may read it too) but nothing is read from it or sent to it.
//
// Message thread only. Hooks::changed is called whenever present() or status() may have changed (a device coming,
// going, switching, an error), never from inside a Hooks call.
#pragma once

#include "plugin/WheelMap.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace playground::standalone
{
    class RacingWheel
    {
    public:
        struct Hooks
        {
            std::function<void()> changed;
            std::function<void(const std::vector<wheel::Message>&)> send;   // channel 1's messages, in order
        };

        static constexpr const char* kIdentifier = wheel::kIdentifier;        // its MidiInputDevice::identifier

        RacingWheel(Hooks hooks, bool on);
        ~RacingWheel();
        RacingWheel(const RacingWheel&) = delete;
        RacingWheel& operator=(const RacingWheel&) = delete;

        bool present() const noexcept;           // a G29 is connected, in any mode
        bool on() const noexcept;
        void setOn(bool on);
        std::string status() const;              // a short line for the card, "" when there is none

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
