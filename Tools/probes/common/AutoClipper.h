// Tools/probes/common/AutoClipper.h: circuit netlists whose configuration is the self-test's, and a program of this
// product's kind playing one (header-only, JUCE-free), for the probes of the self-test (fminus.circuit's SELFTEST,
// REFRESH and DEDUP; plugin.state and plugin.projects CIRCUITS). Neither names .oversample, .adaa or .solver, so a
// compile plays the provisional configuration and wants a self-test (fminus/Circuits.h).
//   kAutoClipper  Yeh's RC diode clipper (FMinus's examples/circuits/clipper.cir): its self-test takes about half a
//                 second on four threads
//   kPotClipper   the same behind a drive rheostat, a pot the stress matrix walks (three points): about three times as
//                 long, for the rows that need a test still running when the next compile lands
#pragma once

#include "Product.h"

#include <string>

namespace probe
{
    inline constexpr const char* kAutoClipper = "* clipper.cir: Yeh's RC diode clipper, its configuration left to the self-test\n"
                                                ".title Clipper\n"
                                                ".input  u    in   scale=1\n"
                                                ".output vout a\n"
                                                ".target solver=-60dB alias=-60dB level=4.5V freq=3k\n"
                                                "R1     in a  2.2k\n"
                                                "C1     a  0  10n\n"
                                                "D1     a  0  1N914\n"
                                                "D2     0  a  1N914\n"
                                                ".model 1N914 D(IS=2.52n N=1.752)\n"
                                                ".end\n";

    inline constexpr const char* kPotClipper = "* potclip.cir: a diode clipper behind a drive rheostat, its configuration left to the self-test\n"
                                               ".title PotClip\n"
                                               ".input  u    in   scale=1\n"
                                               ".output vout a\n"
                                               ".target solver=-60dB alias=-60dB level=4.5V freq=3k\n"
                                               ".pot    drive 0.5\n"
                                               "R1     in b  2.2k\n"
                                               "P1     b  a  20k {1 - drive}\n"
                                               "C1     a  0  10n\n"
                                               "D1     a  0  1N914\n"
                                               "D2     0  a  1N914\n"
                                               ".model 1N914 D(IS=2.52n N=1.752)\n"
                                               ".end\n";

    // A program of this product's kind playing `module` from the netlist `file` (kAutoClipper's Clipper, kPotClipper's
    // PotClip, set(0.5) on the 16-sample grid when `pot`): an Effect over its input at 4 V a unit, a Voice over a saw
    // at 2 V while its key is held, falling away after it.
    inline std::string clipperProgram(const std::string& file, const std::string& module, bool pot)
    {
        const std::string set = pot ? "            if (grid == 0)\n            {\n                clip.set(0.5f);\n"
                                      "                grid = 16;\n            }\n            grid -= 1;\n"
                                    : "";
        std::string s = "#include \"" + file + "\"\n";
        if constexpr (playground::product::kIsInstrument)
            s += "module Voice\n{\n    " + module + " clip;\n    int grid;\n    float phase;\n    float step;\n    float env;\n"
                 "    bool held;\n\n    void init(float rate)\n    {\n        clip.init(rate);\n    }\n\n"
                 "    void start(float note, float velocity)\n    {\n        step = 440 * expf(0.0577622650f * (note - 69)) / 48000;\n"
                 "        phase = 0;\n        env = velocity;\n        held = true;\n        grid = 0;\n        clip.clear();\n    }\n\n"
                 "    void stop()\n    {\n        held = false;\n    }\n\n"
                 "    bool process(float* restrict left, float* restrict right, int n)\n    {\n        for (int i = 0; i < n; ++i)\n"
                 "        {\n" + set +
                 "            const float x = 2 * phase - 1;\n            phase += step;\n            if (phase >= 1)\n"
                 "                phase -= 1;\n            if (!held)\n                env *= 0.999f;\n"
                 "            const float y = 0.25f * env * clip.tick(2 * x);\n            left[i] += y;\n            right[i] += y;\n"
                 "        }\n        return held || env > 0.0001f;\n    }\n};\n";
        else
            s += "module Effect\n{\n    " + module + " clip;\n    int grid;\n\n"
                 "    void init(float rate)\n    {\n        clip.init(rate);\n    }\n\n"
                 "    void process(float* restrict left, float* restrict right, int n)\n    {\n        for (int i = 0; i < n; ++i)\n"
                 "        {\n" + set +
                 "            left[i] = 0.25f * clip.tick(4 * left[i]);\n            right[i] = left[i];\n        }\n    }\n};\n";
        return s;
    }
}
