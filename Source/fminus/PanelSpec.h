// Source/fminus/PanelSpec.h: how a program asks for its dials to be laid out and what else its panel shows
// (JUCE-free), in #pragma panel lines. FMinus ignores every #pragma (as C ignores the pragmas it does not know); the
// plugin reads these from the files it compiled (the program and what it includes), in that order, and the editor
// draws them as an analog synth's panel is printed: titled sections, framed, in rows.
//
//   #pragma panel section "FILTER" cutoff resonance gr     a section: its title (may be ""), then its items in order:
//                                                          dials, and displays by their source's name
//   #pragma panel row                                      the sections after it start a new row
//   #pragma panel label envamount "AMOUNT"                 a dial's label (else its parameter's name)
//   #pragma panel small detune lforate                     small knobs: no value line under them
//   #pragma panel style attack decay fader                 how dials are drawn (the style last): knob (the default),
//                                                          vintage, fader, switch (a bool, or an int of two values),
//                                                          buttons (an int or a bool, a lit button a value)
//   #pragma panel options wave "SAW" "SQUARE" "TRI"        the names of an int's values from its range's low end (a
//                                                          bool's: off, on), on its buttons, its dial and to the host
//   #pragma panel color "FILTER" orange                    a section's colour (by its title) or an item's (by name)
//
// Displays show what the program does, read on the audio thread each block (Engine.h):
//   #pragma panel meter out "OUTPUT"                       LED bars
//   #pragma panel vu out                                   an analog VU meter, a needle
//   #pragma panel led gate "GATE"                          a light
//   #pragma panel number cutoffHz "HZ"                     a readout
//   #pragma panel scope out                                an oscilloscope
// Their source: `in` or `out`, the plugin's audio (an effect's input, the output; left and right, in dBFS), or a
// float, int or bool field of the main module that the program writes (`gr`, `env`), shown between a range given
// after it (default 0 1: #pragma panel meter gr 0 24 "GR"); an instrument's from its newest sounding voice. A
// field's scope is its trace over the last seconds. A section names a display by its name, the source's unless
// `as` gives another, which a second display of one source needs: #pragma panel scope out as wave "SCOPE".
//
// The sections go left to right in the order of their lines, and to another row when one is full. Dials and
// displays no section names come last, in a section of their own without a title. A line, or a part of one, the
// plugin cannot use is a note (its file and line); the rest stands.
#pragma once

#include "dsp/Controls.h"
#include "dsp/Sources.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace testingplaygroundsynth
{
    inline constexpr std::size_t kMaxSectionTitle = 24;   // characters; longer ones are cut
    inline constexpr std::size_t kMaxDialLabel = 14;
    inline constexpr std::size_t kMaxOptionLabel = 10;
    inline constexpr int kMaxDisplays = 8;                // displays (and so field taps) a program may have
    inline constexpr int kMaxOptions = 8;                 // values a switch or buttons may name

    enum class DisplayKind : uint8_t { meter, vu, led, number, scope };
    enum class DialStyle : uint8_t { knob, vintage, fader, toggle, buttons };   // toggle: #pragma panel style x switch

    struct PanelDisplay
    {
        DisplayKind kind = DisplayKind::meter;
        std::string name;                                   // what a section line calls it: the source's, or `as`'s
        std::string source;                                 // "in", "out", or a field of the main module
        std::string label;                                  // "": the source's name
        float lo = 0.0f, hi = 1.0f;                         // a field's range
        int tap = -1;                                       // a field's tap (Patch::taps), in display order; -1: audio
        std::string color;                                  // a colour's name (kPanelColors), "" for the default
        bool audio() const noexcept { return source == "in" || source == "out"; }
        bool operator==(const PanelDisplay&) const = default;
    };

    struct PanelItem
    {
        bool display = false;                               // a display (PanelSpec::displays), else a dial
        int index = 0;                                      // the dial's slot, or the display's index
        bool operator==(const PanelItem&) const = default;
    };

    struct PanelSection
    {
        std::string title;                                  // as written; "" for none
        std::vector<PanelItem> items;                       // its dials and displays, in order
        bool newRow = false;                                // a row starts with it
        std::string color;
        bool operator==(const PanelSection&) const = default;
    };

    struct PanelSpec
    {
        std::vector<PanelSection> sections;                 // empty: the dials in plain rows (no section, no display)
        std::vector<PanelDisplay> displays;
        std::array<std::string, kMaxControls> labels{};     // "": the parameter's name
        std::array<bool, kMaxControls> small{};
        std::array<DialStyle, kMaxControls> styles{};
        std::array<std::vector<std::string>, kMaxControls> options{};   // an int's or a bool's values' names
        std::array<std::string, kMaxControls> colors{};
        bool operator==(const PanelSpec&) const = default;
    };

    // A field of the main module, as FMinus describes it ("float", "int", "bool", "float[16]", "param float", ...).
    struct PanelField
    {
        std::string name, type;
    };

    // The colours a panel line may name.
    inline constexpr const char* kPanelColors[] = { "red",  "orange", "yellow", "green", "teal",
                                                    "blue", "violet", "pink",   "cream", "grey" };

    // The panel `files` ask for (the files compiled, the program first) for `controls`, the program's dials, and
    // `fields`, its main module's (an instrument has no `in`). Notes, a line each, are appended to `notes`:
    // "program.fm:12:1: note: #pragma panel: ...".
    PanelSpec readPanelSpec(const std::vector<const SourceFile*>& files, const Controls& controls,
                            const std::vector<PanelField>& fields, bool instrument, std::string& notes);
}
