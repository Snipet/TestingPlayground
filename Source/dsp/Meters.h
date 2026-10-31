// Source/dsp/Meters.h: what the Engine publishes for the displays and for a tool that listens, as of the last block
// (Engine.h, "Meters"): plain data, JUCE-free, copied through a seqlock. Apart from Engine.h, so that what only shows
// them (the editor's displays, editor/DisplayItem.h, and the GUI sandbox's demo values) needs nothing of the audio side.
#pragma once

#include "dsp/Events.h"
#include "fminus/PanelSpec.h"

#include <array>
#include <cstdint>

namespace playground
{
    // What the schematic's probes watch (Engine::setProbes, Engine.h's "Probes"): in the program `program` (its
    // Patch::program; 0: nothing), field `circuit` of its probe table (CompileStatus::probes), its element `instance`
    // (amp[1]: 1), and in each of the four slots a node of the netlist (an index into its nodes, -1: none).
    struct ProbeSet
    {
        uint64_t program = 0;
        int circuit = -1;
        int instance = 0;
        std::array<int16_t, 4> nodes{ -1, -1, -1, -1 };

        bool armed() const noexcept
        {
            return program != 0 && circuit >= 0 && (nodes[0] >= 0 || nodes[1] >= 0 || nodes[2] >= 0 || nodes[3] >= 0);
        }
        friend bool operator==(const ProbeSet&, const ProbeSet&) = default;
    };

    // A probe's slot, as of the last block (Meters::probes): the node's absolute volts.
    struct ProbeMeter
    {
        float peak = 0.0f;                            // the peak of |v|, falling 20 dB a second between louder ones
        float rms = 0.0f;                             // the RMS, its power smoothed over 50 ms
        float mean = 0.0f;                            // the mean, smoothed over 200 ms
        bool valid = false;                           // armed on a readable node that played this block, every stat
                                                      // finite (else the numbers are the last valid ones', or 0)
    };

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
        // The probes (Engine.h, "Probes"), as of the last block, whoever reads:
        ProbeMeter probes[4] = {};                    // each slot's
        uint64_t probeProgram = 0;                    // the program the probes are of (0: none armed)
        uint64_t probeGen = 0;                        // the set they are of (Engine::setProbes' count, from 1)
        ProbeSet probeSet{};                          // ... as it was given (a reader compares its own)
        int16_t probeInstance = -1;                   // the instance followed (an instrument's newest sounding voice's;
                                                      // an effect's 0), -1 while none: no voice sounds, or none armed
    };
}
