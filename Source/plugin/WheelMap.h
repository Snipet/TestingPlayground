// Source/plugin/WheelMap.h: a Logitech G29 racing wheel (the wheel, its pedals and the Driving Force Shifter, one USB
// cable) as MIDI control changes (and one note, the start key's), in plain C++ (no JUCE, no IOKit): which device a USB
// HID one is, the commands it takes, the table of its elements, and Map, its element values made the messages the
// Standalone sends (dsp/Events.h's wheelcc, which Engine::controller reads into the Host block, host.fm). The
// Standalone's RacingWheel (standalone/RacingWheel.h) reads the device through IOKit and feeds a Map; Tools/wheel-dump
// lists the elements and prints the values and the messages; plugin.wheel tests Map with made-up values.
//
// The device (vendor 0x046D), as Linux's hid-lg4ff tells them apart (drivers/hid/hid-lg4ff.c):
//   0xC24F  a G29 in its own mode ("native"): read
//   0xC294  a Logitech wheel's compatibility mode (Driving Force EX), as a G29 with its switch at PS3 starts without
//           Logitech's software: 10-bit steering, no clutch, no shifter. A G29 is told apart by its bcdDevice
//           (lg4ff_g29_ident_info and _info2: & 0xFFF8 == 0x1350, or & 0xFF00 == 0x8900) and switched to its own mode
//           with hid-lg4ff's two reports (kToNative), after which it leaves and comes back as 0xC24F
//   0xC260  a G29 with its switch at PS4: not read (the card says to set the switch to PS3)
//
// The table (kAxisRows, kGears, kBits) is a guess from what is known of the G29's native report, checked on no device
// yet: Tools/wheel-dump prints what a connected wheel has (README, "A racing wheel"), and a row changed here is the
// whole fix.
//
// The control changes (channel 1), each sent only when its value changes (Map::flush):
//   16 / 48   steering, 14 bits (MSB / LSB): 0 full left, 8192 the centre, 16383 full right
//   17 / 49   throttle, 14 bits: 0 released, 16383 down; 18 / 50 the brake; 19 / 51 the clutch (a pedal's first 1 %
//             of travel reads released, kDeadband: its pot's jitter at rest sends nothing)
//   20        the gear & 0x7F: 127 reverse, 0 neutral, 1 to 7
//   21 to 24  the buttons' mask (kBits), 7 bits each: bits 0-6, 7-13, 14-20, 21-27
// An MSB resets its LSB to 0, as MIDI says: a new MSB goes first, and its LSB after it unless that is 0. A change of an
// axis smaller than kJitter waits until it grows (an encoder's jitter, the steering's at its centre too, which is on
// an MSB's edge), unless it comes to rest. One button is also the start key (kStartButton, the PS button): note 36
// (C2), velocity 100, while it is held.
#pragma once

#include "dsp/Events.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace playground::wheel
{
    inline constexpr int kVendor = 0x046D;                   // Logitech
    inline constexpr int kG29 = 0xC24F;                      // hid-ids.h's USB_DEVICE_ID_LOGITECH_G29_WHEEL
    inline constexpr int kCompat = 0xC294;                   // USB_DEVICE_ID_LOGITECH_WHEEL (Driving Force EX)
    inline constexpr int kG29Ps4 = 0xC260;                   // a G29 at PS4

    // What a USB device is to us.
    enum class Kind : uint8_t
    {
        none,       // not a G29 (another Logitech wheel in compatibility mode among them: left alone)
        g29,        // a G29 in its own mode: read it
        compat,     // a G29 in compatibility mode: switch it (kToNative)
        ps4,        // a G29 at PS4
    };
    // By its vendor and product ids and its version (bcdDevice), as hid-lg4ff's lg4ff_identify_multimode_wheel.
    Kind kindOf(int vendor, int product, int version) noexcept;

    // An output report of the wheel's (report id 0, 7 bytes), as hid-lg4ff sends them (hid_hw_request, SET_REPORT).
    using Report = std::array<uint8_t, 7>;

    // hid-lg4ff's lg4ff_mode_switch_ext09_g29: revert the mode upon a USB reset, then switch to G29 mode with a detach.
    inline constexpr std::array<Report, 2> kToNative{ { { 0xf8, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00 },
                                                        { 0xf8, 0x09, 0x05, 0x01, 0x01, 0x00, 0x00 } } };
    // hid-lg4ff's lg4ff_set_range_g25 (the G25, G27, DFGT and G29's): the rotation, 40 to 900 degrees.
    Report rangeReport(int degrees) noexcept;
    // hid-lg4ff's lg4ff_set_autocenter_default (every wheel but the Formula Force EX), magnitude 1 to 0xFFFF: the
    // spring's strength and limit, then "activate auto-centre". Its 0, deactivated, is kSpringOff.
    std::array<Report, 2> springReports(uint16_t magnitude) noexcept;
    inline constexpr Report kSpringOff{ { 0xf5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } };

    inline constexpr int kRange = 900;                       // degrees lock to lock, as the wheel's own default
    inline constexpr uint16_t kSpring = 0x6000;              // a moderate spring (a game's 37 %): fe 0d 03 03 48

    // ---- the table: the G29's elements in its own mode, by HID usage -----------------------------------------------
    inline constexpr uint32_t kDesktop = 0x01;               // the Generic Desktop page
    inline constexpr uint32_t kButtonPage = 0x09;
    inline constexpr uint32_t kHat = 0x39;                   // the hat switch (Generic Desktop): the d-pad

    enum class Axis : uint8_t { steering, throttle, brake, clutch };   // wheelcc::kSteering + axis

    struct AxisRow
    {
        uint32_t usage;          // on kDesktop
        Axis axis;
        bool restAtMax;          // its logical maximum at rest (Logitech's pedals read their maximum when released)
        const char* name;
    };
    inline constexpr AxisRow kAxisRows[] = {
        { 0x30, Axis::steering, false, "X: steering" },      // 16 bits, 0 to 65535
        { 0x32, Axis::throttle, true, "Z: throttle" },       // 8 bits
        { 0x35, Axis::brake, true, "Rz: brake" },
        { 0x31, Axis::clutch, true, "Y: clutch" },
    };

    // The Driving Force Shifter's positions, buttons (1-based) on kButtonPage; none of them down: neutral.
    struct GearRow
    {
        int button;
        int gear;                // -1 reverse
    };
    inline constexpr GearRow kGears[] = { { 13, 1 }, { 14, 2 }, { 15, 3 }, { 16, 4 }, { 17, 5 }, { 18, 6 }, { 19, -1 } };

    // The buttons' mask, a row a bit (bit i is kBits[i]): a button (1-based, on kButtonPage) or a direction of the
    // d-pad. The shifter's buttons are the gear (CC 20), not bits: the G29's 25 buttons and the d-pad's four would
    // take 29 bits of the 28 the four control changes carry. host.fm names the bits.
    enum class Pad : uint8_t { none, up, right, down, left };
    struct BitRow
    {
        int button;              // 0: a direction of the d-pad
        Pad pad;
        const char* name;
    };
    inline constexpr BitRow kBits[] = {
        { 1, Pad::none, "cross" },           { 2, Pad::none, "square" },        { 3, Pad::none, "circle" },
        { 4, Pad::none, "triangle" },        { 5, Pad::none, "right paddle" },  { 6, Pad::none, "left paddle" },
        { 7, Pad::none, "R2" },              { 8, Pad::none, "L2" },            { 9, Pad::none, "share" },
        { 10, Pad::none, "options" },        { 11, Pad::none, "R3" },           { 12, Pad::none, "L3" },
        { 20, Pad::none, "+" },              { 21, Pad::none, "-" },            { 22, Pad::none, "dial right" },
        { 23, Pad::none, "dial left" },      { 24, Pad::none, "return" },       { 25, Pad::none, "PS" },
        { 0, Pad::up, "d-pad up" },          { 0, Pad::right, "d-pad right" },  { 0, Pad::down, "d-pad down" },
        { 0, Pad::left, "d-pad left" },
    };
    inline constexpr int kButtonsMax = 64;                   // the buttons Map keeps (1 to 64)
    inline constexpr int kDeadband = 100;                    // a pedal's travel within 1 / kDeadband of rest: released
    inline constexpr int kJitter = 3;                        // an axis's least change sent (of 14 bits), but to rest

    // The start key: a button that plays one note while it is held (note-on at its press, note-off at its release), so
    // that a program whose voice runs only while it has a note (an engine that cranks on its first key and then keeps
    // itself going) starts without a keyboard. Its bit stays in the buttons' mask too.
    inline constexpr int kStartButton = 25;                  // the PS button (bit 17)
    inline constexpr uint8_t kStartNote = wheelcc::kStartNote;          // C2 (a render's scenario plays it too)
    inline constexpr uint8_t kStartVelocity = wheelcc::kStartVelocity;
    static_assert(kBits[wheelcc::kStartBit].button == kStartButton && kBits[wheelcc::kStartBit].pad == Pad::none);

    // A MIDI message on channel 1: a control change (its number and 7-bit value), or the start key's note.
    struct Message
    {
        static constexpr uint8_t kCc = 0xB0, kNoteOn = 0x90, kNoteOff = 0x80;
        uint8_t status = kCc;
        uint8_t number = 0;                                  // the controller's, or the note's
        uint8_t value = 0;                                   // its value, or the velocity (0 for a note-off)
        friend bool operator==(const Message&, const Message&) = default;
    };

    // The wheel's state, as the synth will read it (dsp/Events.h's wheelcc decoding: what the Host block will hold).
    struct State
    {
        std::array<int, wheelcc::kAxes> v14{ { wheelcc::kCentre, 0, 0, 0 } };   // steering, throttle, brake, clutch
        int gear = 0;                                        // -1 reverse, 0 neutral, 1..7
        uint32_t buttons = 0;                                // the mask (kBits)
    };

    // A G29's element values in, the messages that bring the synth up to date out. Message thread (or the dump
    // tool's), no locks; flush appends to a vector its caller keeps.
    class Map
    {
    public:
        // An element's new value (its usage page and usage, its logical minimum and maximum): false when the table has
        // no row for it (nothing changes).
        bool input(uint32_t page, uint32_t usage, int64_t value, int64_t min, int64_t max) noexcept;

        // The messages since the last flush (all of the control changes after forget()), in order: each axis's MSB then
        // LSB, the gear, the buttons' four, then the start key's note-on or note-off. Nothing that did not change.
        void flush(std::vector<Message>& out);
        // The messages that put what was sent at rest (the centre, released, neutral, no button, the start key's note
        // off), each only when what was sent differs; then the state is at rest. For the wheel going or switched off:
        // nothing stays held.
        void rest(std::vector<Message>& out);
        // Nothing sent yet: the next flush sends everything (the wheel switched on, a synth that may hold anything).
        void forget() noexcept { sentValid_ = false; }
        // The receiver may have lost what was sent (a synth made afresh, a device restart that dropped what was queued):
        // the next flush sends every control change again, and the start key's note-off when the key is up and its
        // note-on was ever sent (a note-off lost leaves the note held). A key held is not played again: a second
        // note-on would restart the voice the player holds.
        void resend() noexcept;

        const State& state() const noexcept { return now_; }

        // The values as the synth reads them (wheelcc's decoding of what flush sends).
        float steering() const noexcept { return wheelcc::steeringOf(now_.v14[0]); }
        float pedal(Axis a) const noexcept { return wheelcc::pedalOf(now_.v14[static_cast<std::size_t>(a)]); }

        // An axis's 14-bit value of an element's (its logical range): the steering scaled so the range's middle is
        // 8192 (0 to 65535: 32768 the centre, value / 4), a pedal so its rest is 0 and the far end 16383 (its travel
        // past kDeadband spread over the whole range).
        static int axisValue(Axis a, bool restAtMax, int64_t value, int64_t min, int64_t max) noexcept;
        // The d-pad's direction bits (Pad::up's 1, right's 2, down's 4, left's 8) of a hat's value: 8 positions (up,
        // up-right, ..., up-left from the logical minimum) or 4; anything outside the range is the centre.
        static uint32_t padBits(int64_t value, int64_t min, int64_t max) noexcept;

    private:
        void derive() noexcept;                              // the gear and the mask from the buttons and the pad

        State now_;
        uint64_t held_ = 0;                                  // buttons 1 to 64 down, bit n - 1
        uint32_t pad_ = 0;                                   // padBits
        State sent_;
        bool sentValid_ = false;
        bool noteOn_ = false;                                // the start key's note-on sent, its note-off not yet
        bool noteEver_ = false;                              // its note-on sent at some time
        bool offAgain_ = false;                              // resend: its note-off again, if it is still up
    };

    // An element's range as its descriptor meant it: a maximum below the minimum is an unsigned one read as signed
    // (Logical Maximum 65535 written in two bytes is -1), so the range becomes 0 to 2^reportSize - 1 and the value its
    // low reportSize bits. Anything else is left as it is.
    void normalise(int64_t& value, int64_t& min, int64_t& max, uint32_t reportSize) noexcept;

    // "Logitech G29 Racing Wheel": the name the settings card shows, and the identifier it switches it by
    // (MidiInputDevice::identifier, DevicePort::switchMidi).
    inline constexpr const char* kName = "Logitech G29 Racing Wheel";
    inline constexpr const char* kIdentifier = "funk.racing-wheel.g29";
    // The mask's bits as host.fm names them ("0 cross, 1 square, ..."), for the dump tool.
    std::string bitNames();
}
