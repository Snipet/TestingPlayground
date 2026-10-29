// Source/dsp/Meters.h: what the Engine publishes for the displays and for a tool that listens, as of the last block
// (Engine.h, "Meters"): plain data, JUCE-free, copied through a seqlock. Apart from Engine.h, so that what only shows
// them (the editor's displays, editor/DisplayItem.h, and the GUI sandbox's demo values) needs nothing of the audio side.
#pragma once

#include "dsp/Events.h"
#include "fminus/PanelSpec.h"

#include <cstdint>

namespace playground
{
    // What the editor's displays show, as of the last block.
    struct Meters
    {
        uint32_t frame = 0;                           // bumps with each block
        float inPeak[2] = {}, inRms[2] = {};          // the input (an effect's), left and right, linear: the peak
                                                      // falls 20 dB a second, the RMS is smoothed over 50 ms (0
                                                      // while no one reads them: Engine::readers)
        float outPeak[2] = {}, outRms[2] = {};        // the output
        float taps[kMaxDisplays] = {};                // the fields the program's displays show (PanelDisplay::tap)
        uint64_t program = 0;                         // the program they are of (Patch::program), 0 while none plays
        float rate = 48000.0f;                        // the sample rate (a spectrum's frequencies)
        // The player and the host, as of the last block, for a tool that listens (whoever reads; the counts run on
        // from the Engine's making, so a reader takes differences):
        uint64_t samples = 0;                         // processed so far
        HostBlock host{};                             // the Host block: the tempo, the position at the block's first
                                                      // sample, the signature, playing; an instrument's wheels and pedal
        bool hostKnown = false;                       // the host says where it is (Transport::known), else our own clock
        uint8_t voices = 0;                           // an instrument's voices sounding (fading ones aside), of them:
        uint8_t released = 0, pedalHeld = 0;          // released, and let go while the pedal holds them
        uint8_t fading = 0;                           // voices fading out (stolen, or of a program swapped out)
        uint8_t held = 0;                             // keys down
        int8_t lastNote = -1;                         // the last note-on's key (-1: none yet)
        float lastVelocity = 0.0f;
        uint32_t heldKeys[4] = {};                    // the keys down, a bit each (key k: word k / 32, bit k % 32)
        uint32_t noteOns = 0;                         // the player's note-ons (not the held keys a new program starts)
        uint32_t steals = 0;                          // voices stolen for a note (with every voice taken)
        uint32_t dropped = 0;                         // voices dropped for going non-finite
        int32_t droppedKey = -1;                      // the last one's key
        uint64_t droppedAt = 0;                       // and its sample (as `samples` counts them)
    };
}
