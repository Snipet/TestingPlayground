// Source/plugin/WheelDriver.h: the Standalone's racing wheel as decisions, apart from IOKit: which G29 is read, when a
// compatibility-mode one is switched to its own mode, its range and spring, what the first flush may send, how often
// the Map's messages go, what goes again when the receiver may have lost it, its shift lights, and the card's status
// line. The
// Standalone's RacingWheel (standalone/RacingWheel.cpp) is its transport over IOKit's HID manager; plugin.wheel drives
// it with fake devices that record what they are sent. JUCE-free and IOKit-free, like WheelMap.h.
//
// Its rules:
//   - Read: a G29 in its own mode (Kind::g29) while the entry is on: 900 degrees (kRange), then the spring (kSpring),
//     and no spring when the range fails (the card says so); its values through a Map, the messages handed to
//     Transport::send.
//   - The seed: what a device holds before any report came is IOKit's cache at 0 (its timestamp 0), not the wheel's
//     state (a pedal at 0 is pressed, the steering full left, the hat up): such a value is never taken, so the first
//     flush says rest for it. A value is taken only from a report (its timestamp set).
//   - The switch: a compatibility-mode G29 (Kind::compat) gets hid-lg4ff's two reports at most once each time the
//     entry is switched on (as hid-lg4ff's no_autoswitch would have it: a wheel that comes back in that mode is not
//     switched again until then, so it cannot loop). It leaves and comes back as a G29; while it is away the switch is
//     pending and the entry stays listed ("switching to its full mode"); after kSwitchSeconds without it, or when it
//     comes back in its compatibility mode, the switch failed and the card says what to do.
//   - The spring stops (kSpringOff) on every way out that leaves a device to send it to: switched off, another G29
//     read in its place, quit(); a report that fails says so on the card (its spring may still be on). The wheel
//     unplugged takes its spring with it. A crash or a force-quit sends nothing: the spring stays until it is
//     unplugged.
//   - Reading ends (switched off, the wheel gone): what was sent goes back to rest (Map::rest), so nothing stays held.
//   - The rate: a report's values go together kGather after a quiet spell, and two flushes are kInterval apart at
//     least (125 a second: a wheel and three pedals moving at once make about 1,000 control changes a second).
//   - resend(): the receiver may have lost what it was sent (a device restart drops what was queued, a synth made
//     afresh holds nothing): the whole state goes again (Map::resend).
//   - The shift lights (WheelMap.h's "the shift lights"): while a G29 is read, every kLightsInterval, the mask the
//     program asks for (Transport::lights: its shiftLights field) goes to the wheel (lightsReport) when it differs from
//     what was sent, so at most 40 reports a second and none while it holds; a program that asks none (or none
//     playing) turns off what was lit, then sends nothing. Reading starts with none lit (the wheel's own state: a
//     program that never lights them never sends one). Reading ends (switched off, another G29 read in its place,
//     quit()): off, when any are lit; the wheel unplugged takes them with it; a crash leaves them lit until it is
//     unplugged, as it leaves the spring. A report that fails says so on the card and is not tried again until the
//     mask changes.
//
// The commands go through the transport's setRange, setSpring, setLights and toNative: IOKit's transport sends
// hid-lg4ff's reports (their defaults), Linux's has the kernel's hid-lg4ff do it (sysfs and evdev: WheelEvdev.h).
//
// Message thread only (or a probe's). Transport::changed is called whenever present() or status() may have changed,
// never from inside a Transport::send.
#pragma once

#include "plugin/WheelMap.h"

#include <cstdint>
#include <string>
#include <vector>

namespace playground::wheel
{
    using Device = const void*;                              // the transport's (an IOHIDDeviceRef), never followed here

    // An input element's value as the device last reported it (IOHIDDeviceGetValue's): `timestamp` 0 when no report
    // has carried it yet.
    struct Value
    {
        uint32_t page = 0, usage = 0;
        int64_t value = 0, min = 0, max = 0;                 // its logical range, as the descriptor has it
        uint32_t reportSize = 0;
        uint64_t timestamp = 0;
    };

    enum class Timer : uint8_t { flush, switched, lights };

    class Transport
    {
    public:
        virtual ~Transport() = default;
        virtual int report(Device d, const Report& r) = 0;  // an output report: 0 when it went, else its error (IOReturn)
        // The wheel's commands, each through report() by default (hid-lg4ff's bytes, WheelMap.h); a transport whose
        // kernel drives the wheel (Linux's evdev, plugin/WheelEvdev.h) gives each its own way. 0 when it went, else
        // its error.
        virtual int setRange(Device d, int degrees) { return report(d, rangeReport(degrees)); }
        // The spring at `magnitude` (0: off, kSpringOff), its reports in order, stopping at the first that fails.
        virtual int setSpring(Device d, uint16_t magnitude)
        {
            if (magnitude == 0)
                return report(d, kSpringOff);
            for (const Report& r : springReports(magnitude))
                if (const int e = report(d, r); e != 0)
                    return e;
            return 0;
        }
        virtual int setLights(Device d, int mask) { return report(d, lightsReport(mask)); }
        // The switch to its own mode (kToNative): the first report's error; the second may fail as the wheel detaches.
        virtual int toNative(Device d)
        {
            if (const int e = report(d, kToNative[0]); e != 0)
                return e;
            report(d, kToNative[1]);
            return 0;
        }
        // An error of the transport's in words for the card ("0x...", IOKit's code, by default).
        virtual std::string describe(int error);
        virtual std::vector<Value> values(Device d) = 0;    // its input elements' values now (the seed)
        virtual void listen(Device d) = 0;                  // its values to Driver::value from now on (nullptr: none's)
        virtual void arm(Timer t, double seconds) = 0;      // Driver::fire(t) once, `seconds` from now (armed again: moved)
        virtual void disarm(Timer t) = 0;
        virtual double now() = 0;                           // seconds, steady
        virtual void send(const std::vector<Message>& messages) = 0;   // channel 1's, in order
        virtual void changed() = 0;
        virtual int lights() = 0;                           // the shift lights the program asks for now: their mask
                                                            // (0 to kAllLights), or -1 when it asks none
    };

    class Driver
    {
    public:
        static constexpr double kGather = 0.001;            // a report's values, after a quiet spell
        static constexpr double kInterval = 0.008;          // between two flushes, at least
        static constexpr double kSwitchSeconds = 8.0;       // the switch's re-enumeration, at the most
        static constexpr double kLightsInterval = 0.025;    // the shift lights looked at, and sent when they changed
        static constexpr int kNotPermitted = static_cast<int>(0xE00002E2u);   // kIOReturnNotPermitted

        // The transport is the caller's, alive until quit(); the constructor calls none of it.
        Driver(Transport& transport, bool on) : t_(transport), on_(on) {}
        Driver(const Driver&) = delete;
        Driver& operator=(const Driver&) = delete;

        void opened(int result);                            // IOHIDManagerOpen's (0: open)
        void added(Device d, Kind kind);                    // a device matched (Kind::none: left alone)
        void removed(Device d);
        void value(Device d, const Value& v);               // one of the device's values, from a report
        void fire(Timer t);
        void setOn(bool on);
        void resend();
        // The app quits: the spring off, nothing else sent, and nothing done after it. The owner calls it before the
        // transport goes (the destructor calls nothing).
        void quit();

        bool on() const noexcept { return on_; }
        bool present() const noexcept;                      // the entry is listed: a G29 connected, or a switch's news
        std::string status() const;                         // a short line for the card, "" when there is none

    private:
        struct Known
        {
            Device d;
            Kind kind;
        };
        enum class Switch : uint8_t { none, pending, failed };

        const Known* find(Kind kind) const noexcept;
        bool has(Device d) const noexcept;
        bool take(const Value& v);
        void flush();
        void startReading(Device d);
        void stopReading(bool stillThere);
        void switchToNative(Device d);
        void pollLights();
        void lightsOff();                                   // what is lit turned off, while the wheel is read
        void endSwitch(Switch to);
        void refresh();

        Transport& t_;
        bool on_ = false, quit_ = false;
        int opened_ = 0;
        std::vector<Known> devices_;
        Device reading_ = nullptr;                          // the G29 set up and read, while on
        Map map_;
        std::vector<Message> out_;
        bool flushArmed_ = false;
        double lastFlush_ = -1.0e9;
        bool switchUsed_ = false;                           // this "on"'s one switch is spent
        Switch switch_ = Switch::none;
        std::string problem_;                               // the last report that failed, in words
        int lightsSent_ = 0;                                // the shift lights' mask last sent (or tried) while read
    };
}
