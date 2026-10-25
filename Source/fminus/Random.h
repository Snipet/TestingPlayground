// Source/fminus/Random.h: RANDOM, a program to start from (JUCE-free): this plugin's kind, put together from the
// library's building blocks with choices and settings drawn from a seed, its dials laid out in a panel, and a name.
//
//   an instrument  one or two oscillators (osc.fm's morph, a wavetable's built once in a module Shared, or two
//                  operators of FM), noise, a filter (svf.fm or ladder.fm) that an envelope opens, an amplitude
//                  envelope (adsr.fm) of a character (a pluck, keys, a pad, a bass, brass, a bell), an LFO (lfo.fm) on
//                  the cutoff, the pitch, the wave, the FM index or the level, a drive (shaper.fm)
//   an effect      two to four of: a drive (tanh, a wavefolder, a crusher or a tube's curve, with a tone), a filter,
//                  still, swept by an LFO or opened by the input's level (follower.fm); a chorus, a flanger, a tremolo
//                  or an auto-pan; an echo (delay.fm) or a reverb (reverb.fm); then a dry/wet mix and an output level
//
// The same seed gives the same program. fminus.random compiles and plays them: no note, the dials in titled
// sections, finite and audible, no sound from silence, voices that end, the same output in any block size.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace testingplaygroundsynth::random
{
    inline constexpr uint32_t kMaxSeed = 99999;

    struct Program
    {
        uint32_t seed = 0;
        std::string title;                      // "Copper Bloom"
        std::string text;                       // the program's file; its first line "// Copper Bloom (random 4821): ..."
        std::vector<std::string> library;       // the library files it #includes, in that order
        std::vector<std::string> parts;         // the choices made ("analog", "ladder", "lfo>cutoff", ...)
    };

    // The program for `seed`, 1 to kMaxSeed (another is taken modulo kMaxSeed).
    Program make(uint32_t seed);

    // Every part make() may choose for this plugin's kind.
    std::vector<std::string> allParts();
}
