// Source/dsp/Compiler.h: compiles programs and hands them to the Engine (JUCE-free).
//
//   request(files)      the editor's RUN: compiled on the compiler's own thread, so the editor never waits for LLVM.
//                       A request made while another compiles replaces any still waiting: the last one wins.
//   compileNow(files)   the same on the calling thread, for the processor's construction and a host's state load,
//                       which want the program playing when they return (and the probes, which want determinism)
//
// The files are the program's (Sources.h): the first plays, and the others are what its #includes read.
//
// A program that compiles is offered to the Engine at once (Engine::offer), compiled for its current rate; one that
// does not leaves the running one playing. Either way the outcome is the status (status(); revision() bumps when one
// arrives): FMinus's messages, the plugin's, the program's dials, how long it took. The thread also destroys, every
// tenth of a second, what the audio thread handed back (Engine::collect), so the processor need not.
//
// Frozen dials (Patch.h). A thread of its own, the freezer, watches the dials (Engine::dials) ten times a second: once
// they have held still for freezeAfter seconds (3 by default) on the program that plays, it compiles that program's
// frozen copy at their values (Patch::freeze, from the recipe the program's compile kept) and offers it to the Engine,
// which plays it if the dials still stand there. Values it has tried are not tried again until a dial moves. It runs
// beside the compiles: a RUN never waits for it.
//
// A program that plays and has a .gui file with shaders gets them built too (fminus/Shaders.h: shaderc, with the
// tools setShaderTools gave; a shader seen before comes from the cache): their binaries go into the status's panel
// (PanelShader::binary), and a shader that does not compile is a note on its own file's line.
#pragma once

#include "dsp/Controls.h"
#include "dsp/Engine.h"
#include "dsp/Sources.h"
#include "fminus/Patch.h"
#include "fminus/PanelSpec.h"
#include "fminus/Shaders.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace testingplaygroundsynth
{
    class Engine;

    struct CompileStatus
    {
        uint64_t    revision = 0;              // 0 before the first compile; bumps with each one that finishes
        uint64_t    request = 0;               // the request it answers (request()'s, compileNow()'s)
        bool        ok = false;                // the program compiled and plays
        std::string errors;                    // one message a line: "program.fm:3:12: error: ...", and clang's
                                               // "In file included from program.fm:2:" before one in another file
        std::string notes;                     // the plugin's notes on a program that plays
        double      ms = 0.0;
        Controls    controls{};                // the program's dials, when ok
        PanelSpec   panel;                     // how its #pragma panel lines lay them out, when ok
        Sources     sources;                   // the files compiled
    };

    class Compiler
    {
    public:
        explicit Compiler(Engine& engine);
        ~Compiler();                           // stops the thread and waits for it
        Compiler(const Compiler&) = delete;
        Compiler& operator=(const Compiler&) = delete;

        // What building a .gui file's shaders needs (the plugin's shaderc and FunkGui's texts); none until it is set.
        void setShaderTools(ShaderTools tools);

        uint64_t request(Sources files);
        CompileStatus compileNow(Sources files);

        CompileStatus status() const;
        uint64_t revision() const noexcept { return revision_.load(std::memory_order_acquire); }
        bool busy() const;                     // a request waits or compiles
        void wait();                           // until none does

        // Frozen dials (above): on or off, and after how long still. freezeNow() compiles the copy for the dials as they
        // stand now on the calling thread, still or not, and offers it (false when there is nothing to freeze or it
        // does not compile); freezes() counts the copies offered.
        void setFreezing(bool on, double afterSeconds = kFreezeAfterSeconds);
        bool freezeNow();
        int freezes() const noexcept { return freezes_.load(std::memory_order_relaxed); }
        static constexpr double kFreezeAfterSeconds = 3.0;

    private:
        void run();
        void freezer();                        // the freezer's thread
        bool freeze(const DialState& d);       // compiles and offers the copy (the recipe under mutex_)
        void finish(CompileStatus s);

        Engine& engine_;
        mutable std::mutex mutex_;
        ShaderTools tools_;                    // mutex_
        ShaderCache shaders_;
        std::condition_variable wake_, idle_;
        Sources queued_;
        bool hasQueued_ = false;
        bool compiling_ = false;
        bool stop_ = false;
        uint64_t queuedId_ = 0, nextId_ = 0;
        CompileStatus status_;
        std::atomic<uint64_t> revision_{ 0 };
        // The freezer's: the recipe of the program last offered, whether to freeze and after how long, the values
        // tried for which program.
        Patch::Recipe recipe_;                 // mutex_
        bool freezing_ = true;                 // mutex_
        double freezeAfter_ = kFreezeAfterSeconds;
        uint64_t triedProgram_ = 0;
        std::array<float, kMaxControls> tried_{};
        std::atomic<int> freezes_{ 0 };
        std::mutex freezeMutex_;               // one freeze at a time (the freezer's, freezeNow's)
        std::condition_variable freezerWake_;
        std::thread thread_;                   // last: started once everything above is
        std::thread freezer_;
    };
}
