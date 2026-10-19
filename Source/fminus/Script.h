// Source/fminus/Script.h: an FMinus script on the audio thread. A script is FMinus source with three functions:
//
//   float* create(float rate);      off the audio thread, when the script loads (and again when the sample rate
//                                   changes): the script's state, from calloc, or null when it keeps none
//   void process(float* restrict state, float* restrict left, float* restrict right, const float* restrict amount,
//                int n) [[clang::nonblocking]];
//                                   on the audio thread: n samples of both channels, in place. `amount` is the SCRIPT
//                                   parameter, per sample, 0 to 1 (smoothed). At 0 a script should leave the sound as it
//                                   is: the Engine stops calling it there
//   void destroy(float* state);     off the audio thread, when the script is replaced or the plugin goes
//
// process must be marked [[clang::nonblocking]]: FMinus then refuses to compile it if it could reach malloc, calloc or
// free, however far down, and load() refuses a script whose process is not marked. The state is the script's own: it
// reads it at the start of a call and writes it back at the end, so the output does not depend on how the host splits
// its blocks.
//
// Threads. load() and collect() run off the audio thread (ScriptWatcher.h), prepare() while it is stopped, setAmount()
// and process() on it. load() compiles the script and creates its state, then hands it over; the audio thread fades the
// running script out over 20 ms, swaps at the start of a block, fades the new one in, and hands the old one back, which
// collect() frees (destroy, then the JIT'd code). The handovers are two atomic pointers, so neither side waits.
#pragma once

#include "fsdsp/core/Ramp.h"
#include "fsdsp/core/Smoother.h"

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace testingplaygroundsynth
{
    class Script
    {
    public:
        Script() = default;
        ~Script();
        Script(const Script&) = delete;
        Script& operator=(const Script&) = delete;

        // Off the audio thread: compiles `source` and creates its state at the last prepared rate. True when the
        // script is waiting for the audio thread; false, with FMinus's errors (or what the script lacks), otherwise,
        // and the running script runs on.
        bool load(std::string_view source, std::string_view fileName, std::string& errors);

        // Off the audio thread: frees the script the audio thread has finished with, if any.
        void collect();

        // While the audio thread is stopped: the rate and the largest block. Takes a waiting script at once, without a
        // fade, and creates the running one's state again when the rate changed. Allocates.
        void prepare(float sampleRate, int maxBlockSize);

        // The audio thread: SCRIPT, 0 to 1, reached over 20 ms (at once with `snap`).
        void setAmount(float amount, bool snap) noexcept;

        // The audio thread: the script over n samples of the channels, in place (`right` null: a mono bus). At an amount
        // of 0, and with no script, the samples stay as they are.
        void process(float* left, float* right, int n) noexcept;

        // Whether a script runs (or will on the next block): for the probes, while the audio thread is stopped.
        bool running() const noexcept;

    private:
        struct Kernel;
        void processPart(float* left, float* right, int n) noexcept;

        std::mutex mutex_;                         // load, collect and prepare; never the audio thread
        std::atomic<Kernel*> pending_{ nullptr };  // loaded, waiting for the audio thread
        std::atomic<Kernel*> retired_{ nullptr };  // swapped out by the audio thread, waiting for collect()
        Kernel* current_ = nullptr;                // the audio thread's (and prepare()'s, while it is stopped)
        float rate_ = 48000.0f;
        int maxBlock_ = 0;
        fsdsp::SmoothRamp fade_{};                 // the running script's weight: 0 while one swaps for another
        fsdsp::LinearRamp amount_{};               // SCRIPT
        std::vector<float> amounts_;               // per sample of a part (maxBlock_)
        std::vector<float> dryLeft_, dryRight_;    // the input of a part, while the fade moves
        std::vector<float> mono_;                  // a mono bus's second channel, for the script to write
    };
} // namespace testingplaygroundsynth
