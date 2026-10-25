// Source/fminus/PanelSpec.h: how a program asks for its dials to be laid out (JUCE-free), in #pragma panel lines.
// FMinus ignores every #pragma (as C ignores the pragmas it does not know); the plugin reads these from the files it
// compiled (the program and what it includes), in that order, and the editor draws them as an analog synth's panel
// is printed: titled sections, framed, in rows.
//
//   #pragma panel section "FILTER" cutoff resonance envamount   a section: its title (may be "") and its dials, in
//                                                                that order
//   #pragma panel row                                            the sections after it start a new row
//   #pragma panel label envamount "AMOUNT"                       a dial's label (else its parameter's name)
//   #pragma panel small detune lforate                           small knobs: no value line under them
//
// Names are the main module's parameters; a // comment may end a line. The sections go left to right in the order of
// their lines, and to another row when one is full. Dials no section names come last, in a section of their own
// without a title. A line, or a part of one, the plugin cannot use is a note (its file and line); the rest stands.
#pragma once

#include "dsp/Controls.h"
#include "dsp/Sources.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace testingplayground
{
    inline constexpr std::size_t kMaxSectionTitle = 24;   // characters; longer ones are cut
    inline constexpr std::size_t kMaxDialLabel = 14;

    struct PanelSection
    {
        std::string title;                                  // as written; "" for none
        std::vector<int> slots;                             // its dials, in order
        bool newRow = false;                                // a row starts with it
        bool operator==(const PanelSection&) const = default;
    };

    struct PanelSpec
    {
        std::vector<PanelSection> sections;                 // empty: no section line (the dials in plain rows)
        std::array<std::string, kMaxControls> labels{};     // "": the parameter's name
        std::array<bool, kMaxControls> small{};
        bool operator==(const PanelSpec&) const = default;
    };

    // The panel `files` ask for (the files compiled, the program first) for `controls`, the program's dials. Notes, a
    // line each, are appended to `notes`: "program.fm:12:1: note: #pragma panel: ...".
    PanelSpec readPanelSpec(const std::vector<const SourceFile*>& files, const Controls& controls, std::string& notes);
}
