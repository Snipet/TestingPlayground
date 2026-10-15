// Source/dsp/Events.h: note events, as the processor hands them to Engine::process (JUCE-free). An effect ignores
// them; an instrument plays them at their sample offsets within the block.
#pragma once

#include <cstdint>

namespace testingplayground
{
    enum class EventKind : uint8_t { noteOn, noteOff, allNotesOff };

    struct Event
    {
        uint32_t  offset = 0;             // sample offset within the block, ascending
        EventKind kind = EventKind::noteOn;
        uint8_t   note = 0;               // MIDI note number
        float     velocity = 0.0f;        // 0..1 (note on)
    };

    inline constexpr int kMaxEventsPerBlock = 1024;   // per Engine::process call: the processor plays a fuller block
                                                      // in parts, so no event is lost
}
