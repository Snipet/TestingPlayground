// Source/dsp/Compiler.h: compiles programs and hands them to the Engine (JUCE-free).
//
//   request(source)     the editor's RUN: compiled on the compiler's own thread, so the editor never waits for LLVM.
//                       A request made while another compiles replaces any still waiting: the last one wins.
//   compileNow(source)  the same on the calling thread, for the processor's construction and a host's state load,
//                       which want the program playing when they return (and the probes, which want determinism)
//
// A program that compiles is offered to the Engine at once (Engine::offer), compiled for its current rate; one that
// does not leaves the running one playing. Either way the outcome is the status (status(); revision() bumps when one
// arrives): FMinus's messages, the plugin's, the program's dials, how long it took. The thread also destroys, every
// tenth of a second, what the audio thread handed back (Engine::collect), so the processor need not.
#pragma once

#include "dsp/Controls.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace testingplayground
{
    class Engine;

    struct CompileStatus
    {
        uint64_t    revision = 0;              // 0 before the first compile; bumps with each one that finishes
        uint64_t    request = 0;               // the request it answers (request()'s, compileNow()'s)
        bool        ok = false;                // the program compiled and plays
        std::string errors;                    // one message a line: "program.fm:3:12: error: ..."
        std::string notes;                     // the plugin's notes on a program that plays
        double      ms = 0.0;
        Controls    controls{};                // the program's dials, when ok
        std::string source;                    // the source compiled
    };

    class Compiler
    {
    public:
        explicit Compiler(Engine& engine);
        ~Compiler();                           // stops the thread and waits for it
        Compiler(const Compiler&) = delete;
        Compiler& operator=(const Compiler&) = delete;

        uint64_t request(std::string source);
        CompileStatus compileNow(std::string source);

        CompileStatus status() const;
        uint64_t revision() const noexcept { return revision_.load(std::memory_order_acquire); }
        bool busy() const;                     // a request waits or compiles
        void wait();                           // until none does

        static constexpr const char* kFileName = "program.fm";

    private:
        void run();
        void finish(CompileStatus s);

        Engine& engine_;
        mutable std::mutex mutex_;
        std::condition_variable wake_, idle_;
        std::string queued_;
        bool hasQueued_ = false;
        bool compiling_ = false;
        bool stop_ = false;
        uint64_t queuedId_ = 0, nextId_ = 0;
        CompileStatus status_;
        std::atomic<uint64_t> revision_{ 0 };
        std::thread thread_;                   // last: started once everything above is
    };
}
