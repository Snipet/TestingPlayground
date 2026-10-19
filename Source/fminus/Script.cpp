// Source/fminus/Script.cpp: see Script.h.
#include "fminus/Script.h"

#include "fminus/FMinusProgram.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace testingplayground
{
    // One loaded script: its JIT'd code, its three functions and its state.
    struct Script::Kernel
    {
        using Create = float*(float);
        using Process = void(float*, float*, float*, const float*, int);
        using Destroy = void(float*);

        std::unique_ptr<FMinusProgram> program;
        Create* create = nullptr;
        Process* process = nullptr;
        Destroy* destroy = nullptr;
        float* state = nullptr;
        float rate = 0.0f;                         // what `state` was created for

        ~Kernel()
        {
            if (destroy != nullptr)
                destroy(state);
        }
    };

    Script::~Script()
    {
        delete current_;
        delete pending_.exchange(nullptr);
        delete retired_.exchange(nullptr);
    }

    bool Script::load(std::string_view source, std::string_view fileName, std::string& errors)
    {
        FMinusProgram::Options options;
        options.fileName = std::string(fileName);
        auto kernel = std::make_unique<Kernel>();
        kernel->program = FMinusProgram::compile(source, errors, options);
        if (!kernel->program)
            return false;
        // What each function must be, as the error says it.
        const auto need = [&](auto*& function, std::string_view name, std::string_view signature) {
            std::string missing;
            function = kernel->program->function<std::remove_pointer_t<std::remove_reference_t<decltype(function)>>>(
                name, missing);
            if (function == nullptr)
                errors += options.fileName + ": the script needs a function " + std::string(name) + ": "
                          + std::string(signature) + "\n";
        };
        need(kernel->create, "create", "float* create(float rate)");
        need(kernel->process, "process",
             "void process(float* restrict state, float* restrict left, float* restrict right, "
             "const float* restrict amount, int n) [[clang::nonblocking]]");
        need(kernel->destroy, "destroy", "void destroy(float* state)");
        if (kernel->process != nullptr && !kernel->program->nonBlocking("process"))
            errors += options.fileName + ": 'process' runs on the audio thread: mark it [[clang::nonblocking]], after "
                                         "its parameters, so that FMinus checks it cannot allocate\n";
        if (!errors.empty())
        {
            kernel->destroy = nullptr;             // nothing was created
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        kernel->rate = rate_;
        kernel->state = kernel->create(rate_);
        delete pending_.exchange(kernel.release(), std::memory_order_acq_rel);   // one the audio thread never took
        delete retired_.exchange(nullptr, std::memory_order_acquire);
        return true;
    }

    void Script::collect()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        delete retired_.exchange(nullptr, std::memory_order_acquire);
    }

    void Script::prepare(float sampleRate, int maxBlockSize)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rate_ = sampleRate > 0.0f ? sampleRate : 48000.0f;
        maxBlock_ = std::max(1, maxBlockSize);
        const auto size = static_cast<std::size_t>(maxBlock_);
        amounts_.assign(size, 0.0f);
        dryLeft_.assign(size, 0.0f);
        dryRight_.assign(size, 0.0f);
        mono_.assign(size, 0.0f);
        // The audio thread is stopped: a waiting script runs from the first block, and what it handed back goes.
        if (Kernel* next = pending_.exchange(nullptr, std::memory_order_acq_rel); next != nullptr)
        {
            delete current_;
            current_ = next;
        }
        delete retired_.exchange(nullptr, std::memory_order_acquire);
        if (current_ != nullptr && current_->rate != rate_)
        {
            current_->destroy(current_->state);
            current_->state = current_->create(rate_);
            current_->rate = rate_;
        }
        fade_.prepare(rate_);
        fade_.lin.cur = current_ != nullptr ? 1.0f : 0.0f;
        fade_.setTarget(fade_.lin.cur);
        amount_.prepare(rate_, fsdsp::kFadeMs);
    }

    void Script::setAmount(float amount, bool snap) noexcept
    {
        const float a = std::clamp(amount, 0.0f, 1.0f);
        amount_.setTarget(a);
        if (snap)
            amount_.cur = a;
    }

    bool Script::running() const noexcept
    {
        return current_ != nullptr || pending_.load(std::memory_order_acquire) != nullptr;
    }

    void Script::process(float* left, float* right, int n) noexcept
    {
        if (maxBlock_ == 0 || left == nullptr)
            return;                                // not prepared
        // A waiting script: fade the running one out; once it is silent (or there is none), swap at a block's start
        // and fade the new one in. The slot for the old one must be empty first: collect() empties it.
        if (pending_.load(std::memory_order_acquire) != nullptr)
        {
            const bool silent = current_ == nullptr || (fade_.lin.cur == 0.0f && fade_.lin.tgt == 0.0f);
            if (!silent)
                fade_.setTarget(0.0f);
            else if (retired_.load(std::memory_order_acquire) == nullptr)
            {
                if (Kernel* next = pending_.exchange(nullptr, std::memory_order_acq_rel); next != nullptr)
                {
                    if (current_ != nullptr)
                        retired_.store(current_, std::memory_order_release);
                    current_ = next;
                    fade_.setTarget(1.0f);
                }
            }
        }
        for (int done = 0; done < n; done += maxBlock_)
        {
            const int part = std::min(maxBlock_, n - done);
            processPart(left + done, right != nullptr ? right + done : nullptr, part);
        }
    }

    void Script::processPart(float* left, float* right, int n) noexcept
    {
        bool sounding = false;
        for (int i = 0; i < n; ++i)
        {
            const float a = amount_.tick();
            amounts_[static_cast<std::size_t>(i)] = a;
            sounding = sounding || a != 0.0f;
        }
        const bool fading = fade_.moving() || fade_.lin.cur != 1.0f;
        if (current_ == nullptr || !sounding)
        {
            for (int i = 0; fading && i < n; ++i)  // the fade goes on, so a waiting script can swap in
                fade_.tick();
            return;
        }
        float* r = right != nullptr ? right : mono_.data();
        if (right == nullptr)
            std::copy(left, left + n, mono_.data());
        if (!fading)
        {
            current_->process(current_->state, left, r, amounts_.data(), n);   // the script's output, as it is
            return;
        }
        std::copy(left, left + n, dryLeft_.data());
        std::copy(r, r + n, dryRight_.data());
        current_->process(current_->state, left, r, amounts_.data(), n);
        for (int i = 0; i < n; ++i)
        {
            const auto s = static_cast<std::size_t>(i);
            const float w = fade_.tick();
            left[i] = dryLeft_[s] + w * (left[i] - dryLeft_[s]);
            r[i] = dryRight_[s] + w * (r[i] - dryRight_[s]);
        }
    }
} // namespace testingplayground
