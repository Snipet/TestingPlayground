// PROBE name=fminus.contract timeout=120
//
// fminus.contract: what the plugin asks of a program (Patch.h), and what it says when a program does not fit. Each
// row compiles a small program for this plugin's kind (an effect's module Effect, an instrument's module Voice):
//   MODULE     a program without the main module is refused: "the plugin plays a module ...", naming the ones it has
//   METHOD     one without a required method (process; an instrument's start too) is refused: "... needs a method"
//   SIGNATURE  one whose method has other parameters than the plugin calls it with is refused (never called: a call
//              of another type would crash the host), and the message says how the plugin calls it, naming no line
//              of the program's (the plugin's own lines are not the author's)
//   ALLOCATES  one whose audio-thread method could reach calloc is refused: "it runs on the audio thread", with
//              FMinus's note naming the program's line that calls it
//   SYNTAX     a syntax error is FMinus's message, with the line and column in the program
//   DIALS      seventeen parameters: the first sixteen are the dials, in order; the seventeenth gets a note (it keeps
//              its default)
//   DEFAULTS   a module the main one holds has its parameters at their fminus::default when the program starts
//   INIT       init runs with the sample rate before the first block
//   INCLUDE    a program of several files (Sources.h): one that includes another compiles and plays what it includes;
//              the main module may be in an included file; two files that include a third read it once; an error in
//              an included file names that file and its line, after clang's include stack; an #include of a name no
//              file has is "file not found"; and a method of the wrong type is still the plugin's message, with the
//              shims after an #include
#include "Probe.h"

#include "Rig.h"

#include "Product.h"

#include <cstdio>
#include <string>

namespace tp = testingplaygroundsynth;

namespace
{
    constexpr bool kInstrument = tp::product::kIsInstrument;

    // A program of this plugin's kind whose main module has `body` (and an instrument's start).
    std::string program(const std::string& before, const std::string& body)
    {
        if constexpr (kInstrument)
            return before + "module Voice\n{\n    float note;\n    void start(float n, float v) { note = n; }\n" + body +
                   "};\n";
        else
            return before + "module Effect\n{\n" + body + "};\n";
    }

    // The process method with `inner` as its loop's body (left[i] and right[i] in scope).
    std::string process(const std::string& inner)
    {
        if constexpr (kInstrument)
            return "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        for (int i = 0; i < n; ++i)\n        {\n" + inner + "        }\n        return true;\n    }\n";
        else
            return "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        for (int i = 0; i < n; ++i)\n        {\n" + inner + "        }\n    }\n";
    }

    bool has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

    tp::Patch::Result compile(const std::string& source)
    {
        tp::Patch::Result r = tp::Patch::compile(source, 48000.0f);
        std::printf("----\n%s%s", r.errors.c_str(), r.notes.c_str());
        return r;
    }

    tp::Patch::Result compile(const tp::Sources& files)
    {
        tp::Patch::Result r = tp::Patch::compile(files, 48000.0f);
        std::printf("----\n%s%s", r.errors.c_str(), r.notes.c_str());
        return r;
    }

    // The first sample of `program`'s output for an input of 1s (an effect) or one note (an instrument).
    float firstSample(const std::string& source)
    {
        probe::Rig rig(48000.0, 256);
        if (!rig.load(source))
            return -1.0f;
        probe::Stereo in(256);                           // an instrument's: silence; an effect's: 1s
        if constexpr (!kInstrument)
        {
            std::fill(in.l.begin(), in.l.end(), 1.0f);
            std::fill(in.r.begin(), in.r.end(), 1.0f);
        }
        const probe::Stereo out = rig.run(in, 256, { { 0, tp::EventKind::noteOn, 60, 1.0f } });
        return out.l[16];
    }
}

PROBE(fminus, contract)
{
    const char* mod = kInstrument ? "Voice" : "Effect";

    // MODULE
    {
        const auto r = compile("module Other\n{\n    float x;\n};\n");
        P.eq("module.refused", r.patch ? 0 : 1, 1);
        P.eq("module.says_which", has(r.errors, (std::string("plays a module ") + mod).c_str()) ? 1 : 0, 1);
        P.eq("module.names_what_it_has", has(r.errors, "Other") ? 1 : 0, 1);
    }

    // METHOD
    {
        const auto r = compile(kInstrument ? "module Voice\n{\n    bool process(float* l, float* r, int n) { return false; }\n};\n"
                                           : "module Effect\n{\n    float x;\n};\n");
        P.eq("method.refused", r.patch ? 0 : 1, 1);
        P.eq("method.says_needs", has(r.errors, "needs a method") ? 1 : 0, 1);
        P.eq("method.names_it", has(r.errors, kInstrument ? "void start(float note, float velocity)"
                                                          : "void process(float* restrict left") ? 1 : 0, 1);
    }

    // SIGNATURE
    {
        const std::string wrong = kInstrument ? "    bool process(float* restrict left, int n) { return false; }\n"
                                              : "    void process(float* restrict left, int n) { left[0] = 0; }\n";
        const auto r = compile(program("", wrong));
        P.eq("signature.refused", r.patch ? 0 : 1, 1);
        P.eq("signature.says_how", has(r.errors, (std::string(mod) + "::process: the plugin calls it as `").c_str()) ? 1 : 0,
             1);
        P.eq("signature.names_no_plugin_line", has(r.errors, "program.fm:1") || has(r.errors, "program.fm:2") ? 1 : 0, 0);
    }

    // ALLOCATES
    {
        const auto r = compile(program("", process("            float* t = calloc(4, 4);\n            left[i] = t[0];\n")));
        P.eq("allocates.refused", r.patch ? 0 : 1, 1);
        P.eq("allocates.says_audio_thread", has(r.errors, "it runs on the audio thread") ? 1 : 0, 1);
        P.eq("allocates.notes_calloc", has(r.errors, "calloc") ? 1 : 0, 1);
    }

    // SYNTAX
    {
        const auto r = compile(program("", process("            left[i] = right[i] +;\n")));
        P.eq("syntax.refused", r.patch ? 0 : 1, 1);
        const int line = kInstrument ? 9 : 7;            // the line of `left[i] = right[i] +;` in program()
        P.eq("syntax.has_line", has(r.errors, ("program.fm:" + std::to_string(line) + ":").c_str()) ? 1 : 0, 1);
    }

    // DIALS
    {
        std::string params;
        for (int k = 1; k <= tp::kMaxControls + 1; ++k)
            params += "    param float p" + std::to_string(k) + " [[fminus::range(0, 100)]] [[fminus::default(" +
                      std::to_string(k) + ")]];\n";
        const auto r = compile(program("", params + process("            left[i] = p1[i];\n")));
        P.eq("dials.compiles", r.patch ? 1 : 0, 1);
        if (r.patch)
        {
            int used = 0;
            for (const tp::Control& c : r.patch->controls())
                used += c.used ? 1 : 0;
            P.eq("dials.all_slots", used, tp::kMaxControls);
            const std::string last = r.patch->controls()[tp::kMaxControls - 1].name;
            P.eq("dials.in_order", last == "p" + std::to_string(tp::kMaxControls) ? 1 : 0, 1);
            P.near("dials.default", static_cast<double>(r.patch->controls()[2].def), 3.0, 1e-6);
        }
        P.eq("dials.next_noted", has(r.notes, "p" + std::to_string(tp::kMaxControls + 1) + " has no dial") ? 1 : 0, 1);
    }

    // DEFAULTS: a held module's parameter at its default (0.25), read in the main module's process.
    {
        const std::string before = "module Gain\n{\n    param float g [[fminus::default(0.25)]];\n"
                                   "    float apply(float x) { return g[0] * x; }\n};\n";
        const std::string inner = kInstrument ? "            left[i] = gain.apply(1);\n" : "            left[i] = gain.apply(left[i]);\n";
        const float y = firstSample(program(before, "    Gain gain;\n" + process(inner)));
        P.near("defaults.held_module_param", static_cast<double>(y), 0.25, 1e-7);
    }

    // INIT: the rate, before the first block.
    {
        const std::string body = "    float rate;\n    void init(float r) { rate = r; }\n" +
                                 process("            left[i] = rate / 100000;\n");
        const float y = firstSample(program("", body));
        P.near("init.rate", static_cast<double>(y), 0.48, 1e-6);
    }
    // INCLUDE
    {
        // The output's first sample, scaled by a function from another file.
        const tp::Sources two{ { "program.fm", "#include \"gain.fm\"\n" + program("", process("            left[i] = half(1.0f);\n")) },
                               { "gain.fm", "float half(float x) { return 0.5f * x; }\n" } };
        const auto r = compile(two);
        P.eq("include.compiles", r.patch ? 1 : 0, 1);
        P.eq("include.patch_keeps_files", r.patch && r.patch->sources() == two ? 1 : 0, 1);
        if (r.patch)
        {
            probe::Rig rig(48000.0, 256);
            P.eq("include.rig_loads", rig.load(two) ? 1 : 0, 1);
            const probe::Stereo out = rig.run(probe::Stereo(256), 256, { { 0, tp::EventKind::noteOn, 60, 1.0f } });
            P.near("include.plays_it", static_cast<double>(out.l[16]), 0.5, 1e-6);
        }

        // The main module in an included file: the program's own file only includes it.
        const auto inside = compile(tp::Sources{ { "program.fm", "#include \"main.fm\"\n" },
                                                 { "main.fm", program("", process("            left[i] = 0.25f;\n")) } });
        P.eq("include.main_module_included", inside.patch ? 1 : 0, 1);

        // Two files include a third: read once (a second reading would define its function twice).
        const auto diamond = compile(tp::Sources{
            { "program.fm", "#include \"a.fm\"\n#include \"b.fm\"\n" + program("", process("            left[i] = a() + b();\n")) },
            { "a.fm", "#include \"common.fm\"\nfloat a() { return one(); }\n" },
            { "b.fm", "#include \"common.fm\"\n#include \"program.fm\"\nfloat b() { return one(); }\n" },
            { "common.fm", "float one() { return 1.0f; }\n" } });
        P.eq("include.diamond_once", diamond.patch ? 1 : 0, 1);

        // An error in an included file: its name and line, after the include stack.
        const auto wrong = compile(tp::Sources{ { "program.fm", "#include \"lib.fm\"\n" + program("", process("            left[i] = 0.0f;\n")) },
                                                { "lib.fm", "float f(float x)\n{\n    return y;\n}\n" } });
        P.eq("include.error_refused", wrong.patch ? 0 : 1, 1);
        P.eq("include.error_names_file", has(wrong.errors, "lib.fm:3:12: error: use of undeclared identifier 'y'") ? 1 : 0, 1);
        P.eq("include.error_stack", has(wrong.errors, "In file included from program.fm:1:") ? 1 : 0, 1);

        // A name no file has.
        const auto missing = compile(tp::Sources{ { "program.fm", "#include \"nope.fm\"\n" + program("", process("")) } });
        P.eq("include.missing_refused", missing.patch ? 0 : 1, 1);
        P.eq("include.missing_says", has(missing.errors, "program.fm:1:10: error: 'nope.fm' file not found") ? 1 : 0, 1);

        // The plugin's message for a method of the wrong type, with an #include before the main module.
        const std::string bad = kInstrument ? "    bool process(float* restrict left, int n) { return false; }\n"
                                            : "    void process(float* restrict left, int n) { left[0] = 0; }\n";
        const auto shim = compile(tp::Sources{ { "program.fm", "#include \"gain.fm\"\n" + program("", bad) },
                                               { "gain.fm", "float half(float x) { return 0.5f * x; }\n" } });
        P.eq("include.signature_refused", shim.patch ? 0 : 1, 1);
        P.eq("include.signature_says_how",
             has(shim.errors, (std::string(mod) + "::process: the plugin calls it as `").c_str()) ? 1 : 0, 1);
    }
    return P.finish();
}
