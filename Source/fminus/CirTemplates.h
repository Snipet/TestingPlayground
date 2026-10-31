// Source/fminus/CirTemplates.h: Play this circuit (the schematic's info strip, for a netlist no program #includes): a
// program.fm of the product's kind that plays a netlist (JUCE-, FunkGui- and FMinus-free, as Lang and CirDoc are, so
// the editor and the probes may use it). It #includes the netlist and plays its module as the netlist says it is
// played (Patch.h, "Circuits"; FMinus's docs/circuits.md):
//   an effect     two of the module, one a channel (a two-channel netlist's one, .channels 2), each sample through
//                 tick: the .input and .output scales do the scaling
//   an instrument the library's oscillator (osc.fm, a saw at the note's pitch) shaped by its ADSR (adsr.fm) into the
//                 module's input, the circuit cleared only when a note starts from silence
// Each law input (.input NAME - kind=...) gets a param of its name (NAME_in when the program uses that word itself:
// rate, i, osc, ...), 0 to 1, mapped across its range each sample (geometrically when the range starts above 0). No
// dial is written for a pot or a switch: the program never calls set(), so the plugin makes them dials of their own.
// Each one compiles with no message for every library netlist, in both products (fminus.circuit TEMPLATE).
#pragma once

#include <string>
#include <string_view>

namespace playground::cir
{
    struct HostProgram
    {
        std::string text;                         // program.fm's text
        std::string module;                       // the netlist's module (.title, else its file's stem, capitalised)
        std::string why;                          // when there is none: why ("" when there is one)
    };

    // The host program for the netlist `file` (its text `netlist`), of an instrument's kind or an effect's.
    HostProgram hostProgram(std::string_view file, std::string_view netlist, bool instrument);
}
