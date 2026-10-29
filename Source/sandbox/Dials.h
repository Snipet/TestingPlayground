// Source/sandbox/Dials.h: the GUI sandbox's dials, mocked, never compiled: a program's parameter declarations as
// FMinus's language service reads them (fminus/Lang.h: the main module's params, in order, with their
// [[fminus::range]] and [[fminus::default]]), or a list Claude gives, or both (Claude's replacing a declaration of the
// same name); and the main module's fields, which the displays read. JUCE's var for the lists (juce_core).
#pragma once

#include "dsp/Controls.h"
#include "dsp/Sources.h"
#include "fminus/PanelSpec.h"

#include <juce_core/juce_core.h>

#include <string>
#include <vector>

namespace playground::sandbox
{
    struct Dial
    {
        std::string name;
        ControlType type = ControlType::real;
        float min = 0.0f, max = 1.0f, def = 0.0f;
        std::string unit;                         // after a float's value ("Hz"), 7 characters at most
        std::vector<std::string> options;         // an int's or a bool's values' names, from its low end
        std::string style;                        // the control a blank .gui gives it: knob, slider, switch, buttons, menu
        bool given = false;                       // Claude's (else the program's declaration)
    };

    // What a program's files declare (the .fm among `files`): its main module (Effect or Voice: "" when no file has it)
    // and the file it is in, its params as dials (in order; past kMaxControls they are counted, as the plugin leaves
    // them at their defaults), its fields.
    struct Declared
    {
        std::string module, file;
        std::vector<Dial> dials;
        int beyond = 0;                           // params past kMaxControls
        std::vector<PanelField> fields;
    };
    Declared readProgram(const Sources& files, bool instrument);

    // Claude's list: [{ name, range: [lo, hi], default, type: float | int | bool, unit, options: [...], style }], each
    // mistake a line in `notes`.
    std::vector<Dial> parseDials(const juce::var& list, std::string& notes);
    juce::var toJson(const std::vector<Dial>& dials);   // what parseDials reads back (the server keeps it)

    // `base`, each dial replaced by `given`'s of the same name (case aside), then the rest of `given`.
    std::vector<Dial> merge(std::vector<Dial> base, const std::vector<Dial>& given);

    // The dials as the plugin's slots hold them (Patch's controlOf: a bool 0..1, an int 0..10 and a float 0..1 without a
    // range, the default inside it), the first kMaxControls.
    Controls controlsOf(const std::vector<Dial>& dials);

    // A small .gui that holds `dials` (each in its style, else a knob; a bool a switch; an int with names buttons) and
    // the meters of the audio (an effect's in and out, an instrument's out).
    std::string blankGui(const std::vector<Dial>& dials, bool instrument);

    // The fields a .gui's displays and live= marks name (a display of in or out aside), for a sandbox with no program:
    // they are mocked as floats. In the order they first come.
    std::vector<std::string> fieldsNamed(const std::string& gui);

    // A dial's plain value from what Claude gives: a number, true or false, "on" or "off", or one of its options' names
    // (case aside). False when it is none of these.
    bool valueOf(const Dial& d, const juce::var& v, float& plain);
}
