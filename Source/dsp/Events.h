// Source/dsp/Events.h: what the processor hands Engine::process besides the audio (JUCE-free): the block's MIDI as
// events at their sample offsets (an effect ignores them; an instrument plays them there), and the host's transport,
// once a block (Transport).
#pragma once

#include <cstdint>

namespace playground
{
    enum class EventKind : uint8_t
    {
        noteOn,
        noteOff,
        allNotesOff,
        controller,                       // a control change: `note` its number (1 the mod wheel, 64 the sustain
                                          // pedal, 16 to 24 and 48 to 51 a racing wheel's: wheelcc), `velocity`
                                          // its value 0..1 (the 7-bit value / 127)
        pitchBend,                        // `velocity` -1..1, 0 at rest
        pressure,                         // channel pressure (aftertouch): `velocity` 0..1
    };

    struct Event
    {
        uint32_t  offset = 0;             // sample offset within the block, ascending
        EventKind kind = EventKind::noteOn;
        uint8_t   note = 0;               // MIDI note number (a controller's number)
        float     velocity = 0.0f;        // 0..1 (note on; a controller's value; a bend's -1..1)
    };

    inline constexpr int kMaxEventsPerBlock = 1024;   // per Engine::process call: the processor plays a fuller block
                                                      // in parts, so no event is lost

    // The host's transport at a block's first sample (Engine::setTransport). `known` false: the host said nothing (the
    // Standalone): the Engine keeps its own clock at 120 BPM, never playing.
    struct Transport
    {
        bool   known = false;
        double bpm = 120.0;
        double beat = 0.0;                // the song position, in quarter notes
        double barStart = 0.0;            // the last bar's start, in quarter notes
        int    beatsPerBar = 4, beatUnit = 4;
        bool   playing = false;
    };

    // A racing wheel's control changes (README, "A racing wheel"; any channel): the Engine reads them into the Host
    // block (Engine::controller), the Standalone's wheel sends them (plugin/WheelMap.h), and so may any MIDI source.
    // A 14-bit control's MSB resets its LSB to 0, as MIDI says; its LSB is its number + kLsb.
    namespace wheelcc
    {
        inline constexpr uint8_t kSteering = 16;          // 14 bits: 0 full left, 8192 the centre, 16383 full right
        inline constexpr uint8_t kThrottle = 17;          // 14 bits: 0 released, 16383 down
        inline constexpr uint8_t kBrake = 18;
        inline constexpr uint8_t kClutch = 19;
        inline constexpr uint8_t kGear = 20;              // the gear & 0x7F: 127 reverse, 0 neutral, 1 to 7
        inline constexpr uint8_t kButtons = 21;           // 21 to 24: the buttons' mask, 7 bits each (bits 0-6, 7-13,
        inline constexpr int kButtonCcs = 4;              // 14-20, 21-27)
        inline constexpr int kButtonBits = 7;
        inline constexpr uint8_t kLsb = 32;
        inline constexpr int kAxes = 4;                   // steering, throttle, brake, clutch: kSteering + axis
        inline constexpr int kCentre = 8192;
        inline constexpr int kMost = 16383;

        // The Host block's values of the 14-bit and 7-bit values (Engine::controller; Tools/wheel-dump prints them).
        constexpr float steeringOf(int v) noexcept
        {
            const float s = static_cast<float>(v - kCentre) / static_cast<float>(kMost - kCentre);
            return s < -1.0f ? -1.0f : s > 1.0f ? 1.0f : s;
        }
        constexpr float pedalOf(int v) noexcept { return static_cast<float>(v) / static_cast<float>(kMost); }
        constexpr int gearOf(int v) noexcept
        {
            const int g = v > 63 ? v - 128 : v;
            return g < -1 ? -1 : g > 7 ? 7 : g;
        }

        // The controls that say a wheel's whole state, in the order to send them (Engine::wheelCcs): each axis's MSB
        // and LSB, the gear, the buttons' four.
        inline constexpr int kStateCcs = 2 * kAxes + 1 + kButtonCcs;
        constexpr uint8_t stateCc(int i) noexcept
        {
            if (i < 2 * kAxes)
                return static_cast<uint8_t>(kSteering + i / 2 + (i % 2 == 0 ? 0 : kLsb));
            if (i == 2 * kAxes)
                return kGear;
            return static_cast<uint8_t>(kButtons + i - 2 * kAxes - 1);
        }
    }

    // What a program's Host block holds (Patch::setHost; host.fm, Patch.h): the transport at a call's first sample and
    // the player's controllers.
    struct HostBlock
    {
        float bpm = 120.0f;
        float beat = 0.0f;                // quarter notes, at the call's first sample
        float barStart = 0.0f;
        int   beatsPerBar = 4, beatUnit = 4;
        bool  playing = false;
        float modwheel = 0.0f;            // 0..1
        float pitchbend = 0.0f;           // -1..1
        float aftertouch = 0.0f;          // 0..1
        bool  sustain = false;            // the pedal is down
        // A racing wheel (wheelcc), at rest until its control changes come:
        float steering = 0.0f;            // -1 full left .. 1 full right
        float throttle = 0.0f;            // 0 released .. 1
        float brake = 0.0f;
        float clutch = 0.0f;
        int   gear = 0;                   // -1 reverse, 0 neutral, 1..7
        int   buttons = 0;                // a bit a button (host.fm names a G29's)
    };
}
