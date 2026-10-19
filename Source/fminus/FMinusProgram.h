// Source/fminus/FMinusProgram.h: FMinus (~/audio/libraries/FMinus), the JIT compiler for DSP, in this plugin. A program
// of FMinus source is compiled for this machine, and its functions are called through C function pointers. JUCE-free
// and LLVM-free: LLVM and FMinus's internals stay in FMinusProgram.cpp (the library <name>_fminus, which <name>_dsp
// links), so any file of Source/dsp may use it.
//
//   std::string errors;
//   auto program = FMinusProgram::compile(source, errors);              // nullptr when it does not compile
//   auto* gain = program->function<void(const float*, float*, int, float)>("gain", errors);
//   gain(in, out, n, 0.5f);                                              // one call a block: the loop is FMinus's
//
// A function's C type follows its FMinus declaration: float is float, int is int, a pointer a pointer, a float4 clang's
// ext_vector_type(4) (FMinus's docs/simd.md); FMinus checks nothing at this boundary, so a wrong Signature is undefined
// behaviour. Every FMinus function has C's calling convention and its own name.
//
// On the audio thread, only call: compile() and function() take milliseconds (they run LLVM's optimiser and code
// generator) and allocate. Compile in prepare() or off the audio thread, hand the pointer over (an std::atomic does),
// and keep the FMinusProgram alive while the audio thread may still be running its code: the pointers die with it.
// FMinus's README ("Notes for running inside a plugin") has the rest: a DAW's hardened runtime must allow the JIT's
// executable memory (the Standalone always does), and JIT'd code must not allocate or lock either.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace testingplayground
{
    class FMinusProgram
    {
    public:
        struct Options
        {
            int optimisation = 2;                  // -O0 to -O3: LLVM's pipeline, as fminus -O<n>
            std::string fileName = "program.fm";   // what the error messages call the source
        };

        // Compiles `source`. Returns nullptr when it does not compile, with `errors` holding FMinus's messages, one a
        // line, as clang words them (`program.fm:3:12: error: use of undeclared identifier 'x'`).
        static std::unique_ptr<FMinusProgram> compile(std::string_view source, std::string& errors,
                                                     const Options& options);
        static std::unique_ptr<FMinusProgram> compile(std::string_view source, std::string& errors)
        {
            return compile(source, errors, Options{});
        }

        ~FMinusProgram();
        FMinusProgram(const FMinusProgram&) = delete;
        FMinusProgram& operator=(const FMinusProgram&) = delete;

        // The function `name`, as a pointer of type Signature*, which must be its FMinus declaration's C type. The JIT
        // compiles it now, on this thread. nullptr, with `errors` set, when the program has no such function.
        template <typename Signature>
        Signature* function(std::string_view name, std::string& errors)
        {
            return reinterpret_cast<Signature*>(address(name, errors));
        }

        // The function's address, or 0 (with `errors` set).
        std::uintptr_t address(std::string_view name, std::string& errors);

        // How many lanes a poly float has here: this machine's vector width in floats (4 with NEON).
        unsigned polyLanes() const;

    private:
        struct Impl;
        explicit FMinusProgram(std::unique_ptr<Impl> impl);

        std::unique_ptr<Impl> impl_;
    };
} // namespace testingplayground
