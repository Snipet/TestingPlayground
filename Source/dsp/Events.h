// Source/dsp/Events.h: what the processor hands Engine::process besides the audio (JUCE-free): the block's MIDI as
// events at their sample offsets (an effect ignores them; an instrument plays them there), and the host's transport,
// once a block (Transport).
#pragma once

#include <cstdint>

namespace testingplaygroundsynth
{
    enum class EventKind : uint8_t
    {
        noteOn,
        noteOff,
        allNotesOff,
        controller,                       // a control change: `note` its number (1 the mod wheel, 64 the sustain
                                          // pedal), `velocity` its value 0..1
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
    };
}
