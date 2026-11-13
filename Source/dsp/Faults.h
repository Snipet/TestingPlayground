// Source/dsp/Faults.h: an exception met on a thread of the plugin's own, said in words; and the probes' way to plant one
// (JUCE-free).
//
// A thread the plugin starts catches at its top whatever its piece of work throws (CLAUDE.md, "Layer and real-time
// rules"): that piece fails in its own words, the thread lives on, and nothing reaches std::terminate, which would end
// the host. words() is how such a failure is said ("out of memory"; an exception's own words). Never on the audio
// thread: nothing there throws.
#pragma once

#include <exception>
#include <string>

namespace playground::faults
{
    // An exception's words for a person: "out of memory" for std::bad_alloc, its what() for another std::exception (on
    // one line, cut past 300 bytes), "an unknown error" for anything else (or none).
    std::string words(std::exception_ptr e);

    // The places the compiler's threads can be made to throw, for the probes (reach()).
    enum class Stage
    {
        compile,           // Compiler's build (a RUN, compileNow, a refine, a stop's restore)
        check,             // the check while typing
        freeze,            // the freezer's frozen copy
        ir,                // the IR's compile for the IR alone
        selfTest,          // a circuit's self-test, on its coordinator
        selfTestBatch,     // a batch of a self-test's parallel work, on its workers
        count
    };
    enum class Kind
    {
        outOfMemory,       // std::bad_alloc
        error,             // std::runtime_error("a planted failure\nin two lines")
        unknown            // an int
    };

    // Throws when the probes planted a failure at `stage` (else returns at once: one relaxed load).
    void reach(Stage stage);

#if defined(FP_PROBES)
    // Probes only (the plugin is compiled without FP_PROBES, so nothing of it can call this): the next `times` times
    // work reaches `stage`, on any thread, it throws `kind`. 0 takes it back.
    void plantForTests(Stage stage, Kind kind, int times);
    // The throws still planted at `stage` (each reach() that threw took one).
    int plantedForTests(Stage stage);
#endif
}
