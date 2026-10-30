// Source/dsp/SelfTester.h: the circuit self-test, run in the background for one Compiler (JUCE-free). FMinus's circuit
// compiler (fminus/Circuit.h) leaves a netlist's open configuration (.oversample, .adaa, .solver left out) to its
// self-test, which takes seconds: the program plays a provisional configuration meanwhile (fminus/Circuits.h), and
// this runs fminus::circuit::selfTest for each netlist the program that plays wants tested (want()), stores the answer
// (Choice::text()) in circuits::answers() under the netlist's key, and so moves its landed() count: the Compiler then
// compiles the program that plays again with it (Compiler.h, "The self-test").
//
//   want(jobs)   the jobs of the newest program that plays (Patch::Result::circuitJobs): what is tested from now on, in
//                their order. A key is tested when no answer is stored for it and no other tester has it in flight
//                (Answers::claim, released when its test ends: two instances wanting one netlist run one test); a
//                test whose key the newest program does not want is cancelled, and stores nothing
//   runNow()     the jobs wanted, each not stored and not in flight, tested on the calling thread (and the workers),
//                in order: for the probes, as Compiler::freezeNow is
//
// Threads. A coordinator thread of its own (made with the first job) picks the jobs, compiles each candidate (FMinus
// compiles on the calling thread) and runs its share of the stress matrix; for each test it makes workers()
// more threads, which run the rest (SelfTestOptions::parallel over an atomic counter), and joins them when the test
// ends. All of them run at the utility QoS class (macOS), below the host's audio and its UI. Never the audio thread,
// the message thread or the compiler's: nothing here waits for them, nor they for it but on its own mutex, held only
// to read or change what is wanted. A test is cancelled through the atomic SelfTestOptions::cancelled reads (polled
// between jobs and every 4096 reference steps); one cancelled, over the budget, or with no answer stores nothing (a
// key that gave none is not tried again by this tester).
#pragma once

#include "fminus/Circuits.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace playground
{
    class SelfTester
    {
    public:
        SelfTester() = default;
        ~SelfTester();                           // cancels the test that runs and joins the coordinator
        SelfTester(const SelfTester&) = delete;
        SelfTester& operator=(const SelfTester&) = delete;

        // The newest program's jobs (above): what is tested from now on; a test running for a key not among them is
        // cancelled. Any thread but the audio thread; it never waits for a test.
        void want(std::vector<circuits::Job> jobs);
        // Whether the coordinator tests on its own (on by default; off, nothing starts but runNow's, and a test running
        // is cancelled).
        void setEnabled(bool on);
        // The jobs wanted that are not stored and not in flight, tested here, in order (each one's workers made and
        // joined); a job another tester has in flight is left to it. Blocking.
        void runNow();

        // Whether a job wanted waits (no answer stored, none given up on) or a test runs.
        bool busy() const;
        // The tests this tester has started (each a selfTest call), counted: the probes' DEDUP.
        int tests() const noexcept { return tests_.load(std::memory_order_relaxed); }
        // The threads a test adds to the one that runs it: min(4, max(1, cores - 2)).
        static int workers();

    private:
        void coordinator();
        // The next job to test (claimed: Answers::claim), under mutex_; false when none (or, unless `any`, when the
        // coordinator is off).
        bool next(circuits::Job& job, bool any);
        // `job`'s test, its key claimed: stored when it answers; released either way. Under testMutex_, not mutex_.
        void test(const circuits::Job& job);

        mutable std::mutex mutex_;
        std::condition_variable wake_;
        std::vector<circuits::Job> wanted_;      // mutex_
        std::set<std::string> givenUp_;          // keys whose test gave no answer (not cancelled): mutex_
        std::string running_;                    // the key under test ("" for none): mutex_
        bool enabled_ = true;                    // mutex_
        bool changed_ = false;                   // want() or setEnabled() since the coordinator last looked: mutex_
        bool stop_ = false;                      // mutex_
        std::atomic<bool> cancel_{ false };      // the test that runs: SelfTestOptions::cancelled
        std::atomic<bool> stopping_{ false };
        std::atomic<int> tests_{ 0 };
        std::mutex testMutex_;                   // one test at a time (the coordinator's, runNow's)
        std::thread thread_;                     // the coordinator, made with the first job wanted (mutex_)
    };
}
