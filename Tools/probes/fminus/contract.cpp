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
//   DIALS      nine parameters: the first eight are the dials, in order; the ninth gets a note (it keeps its default)
//   DEFAULTS   a module the main one holds has its parameters at their fminus::default when the program starts
//   INIT       init runs with the sample rate before the first block
#include "Probe.h"

#include "Rig.h"

#include "Product.h"

#include <cstdio>
#include <string>

namespace tp = testingplayground;

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

    bool has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

    tp::Patch::Result compile(const std::string& source)
    {
        tp::Patch::Result r = tp::Patch::compile(source, 48000.0f);
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
        for (int k = 1; k <= 9; ++k)
            params += "    param float p" + std::to_string(k) + " [[fminus::range(0, 10)]] [[fminus::default(" +
                      std::to_string(k) + ")]];\n";
        const auto r = compile(program("", params + process("            left[i] = p1[i];\n")));
        P.eq("dials.compiles", r.patch ? 1 : 0, 1);
        if (r.patch)
        {
            int used = 0;
            for (const tp::Control& c : r.patch->controls())
                used += c.used ? 1 : 0;
            P.eq("dials.eight", used, 8);
            P.eq("dials.in_order", std::string(r.patch->controls()[7].name) == "p8" ? 1 : 0, 1);
            P.near("dials.default", static_cast<double>(r.patch->controls()[2].def), 3.0, 1e-6);
        }
        P.eq("dials.ninth_noted", has(r.notes, "p9 has no dial") ? 1 : 0, 1);
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
    return P.finish();
}
