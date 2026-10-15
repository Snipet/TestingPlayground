#pragma once

// Real-time effect annotation (01 §2.4; 03 §2.5 rtsan; seeded from FCompressor: FC 01 §2.2 rule 6, FC ADR-46,
// FC K2 #18). FSDSP_NONBLOCKING expands to
// [[clang::nonblocking]] where the compiler supports it, else to nothing. The rtsan preset compiles fsdsp with
// -Wfunction-effects (fsdsp only), which proves at compile time that a nonblocking function calls only functions that
// are nonblocking themselves: annotated, or inline with a visible body the compiler can infer.
//
// Rules (01 §2.4 holds the whole list; FC's FZ0 errata, review R-F0 #1):
//   - Every declaration the audio thread reaches carries it: EngineHost::process and reset, the Decimator's reset
//     and process, every IOscEngine, IFxEngine and IFnEngine virtual (the destructors included) and its override in
//     OscEngine<T>, FxEngine<T> and FnEngine<T>, their construct functions and the construct pointer types of the
//     registry entries, the registry lookups, and the core helpers (Simd, FastMath, Units, ScopedFtz, Sanitize,
//     Smoother4, LinearRamp, ControlTicker, Seqlock, Phase, Pcg32, BusSum, Ramp). Virtual calls, calls through
//     function pointers and calls to out-of-line definitions cannot be inferred, so those declarations MUST carry it.
//   - Placement: after noexcept and before `override`, `= 0`, `= default` or the body:
//         virtual void process(const OscIo&) noexcept FSDSP_NONBLOCKING = 0;
//         virtual ~IOscEngine() FSDSP_NONBLOCKING = default;
//         IOscEngine* (*construct)(void* arena) noexcept FSDSP_NONBLOCKING;
//   - An out-of-line definition repeats the macro so its effects match the declaration (-Wfunction-effects warns on a
//     mismatch).
//   - Kernel functions in a Mode's traits and kit helpers are header-inline, so the compiler infers them; annotating
//     them is allowed and recommended. A nonblocking body may not allocate, lock, throw, use thread_local or a
//     function-local static, or call an unannotated out-of-line function (libm, assert's failure path when NDEBUG is
//     off).
//
// Frozen at FZ0 (the macro name and the rules above).

#if defined(__has_cpp_attribute)
  #if __has_cpp_attribute(clang::nonblocking)
    #define FSDSP_NONBLOCKING [[clang::nonblocking]]
  #endif
#endif
#if !defined(FSDSP_NONBLOCKING)
  #define FSDSP_NONBLOCKING
#endif
