// Source/dsp/Engine.cpp: see Engine.h.
#include "dsp/Engine.h"

#include "Product.h"

#include "fsdsp/core/Sanitize.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace playground
{
    namespace
    {
        constexpr bool kInstrument = product::kIsInstrument;
        constexpr float kLimit = 4.0f;                    // +12 dBFS: the output guard's clip

        int samplesOf(float ms, float fs) noexcept
        {
            return std::max(1, static_cast<int>(std::lround(static_cast<double>(ms) * static_cast<double>(fs) / 1000.0)));
        }
    }

    // ---- Glide ----------------------------------------------------------------------------------------------------------
    void Engine::Glide::set(float target, int length) noexcept
    {
        if (target == tgt)
            return;                                  // on its way there already, or there
        tgt = target;
        left = std::max(1, length);
        inc = (tgt - cur) / static_cast<float>(left);
    }

    float Engine::Glide::tick() noexcept
    {
        if (left > 0)
        {
            cur = --left == 0 ? tgt : cur + inc;
        }
        return cur;
    }

    // ---- life ---------------------------------------------------------------------------------------------------------
    Engine::~Engine()
    {
        delete frozen_;
        for (Patch::Frozen* f : frozenHold_)
            delete f;
        for (auto& slot : frozenRetired_)
            delete slot.exchange(nullptr);
        delete frozenPending_.exchange(nullptr);
        delete current_;
        if (old_ != current_)
            delete old_;
        delete hold_;
        delete pending_.exchange(nullptr);
        delete retired_.exchange(nullptr);
    }

    void Engine::prepare(double sampleRate, int maxBlockSize)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        fs_ = sampleRate > 0.0 ? static_cast<float>(sampleRate) : 48000.0f;
        rate_.store(fs_, std::memory_order_relaxed);
        maxBlock_ = std::max(1, maxBlockSize);
        const auto block = static_cast<std::size_t>(maxBlock_);
        streamBuf_.assign(static_cast<std::size_t>(kMaxControls) * block, 0.0f);
        for (std::vector<float>* v : { &accL_, &accR_, &voiceL_, &voiceR_, &dryL_, &dryR_, &mono_ })
            v->assign(block, 0.0f);
        stealFade_ = samplesOf(fsdsp::kStealFadeMs, fs_);
        swapFade_ = samplesOf(fsdsp::kFadeMs, fs_);
        xfade_.prepare(fs_);

        // The audio thread is stopped: a waiting program plays from the first block, without a fade, and the ones it
        // replaces (and what it handed back) go now.
        if (Patch* next = pending_.exchange(nullptr, std::memory_order_acq_rel); next != nullptr)
        {
            if (old_ != current_)
                delete old_;
            delete current_;
            current_ = next;
            old_ = nullptr;
        }
        else if (old_ != nullptr)
        {
            delete old_;
            old_ = nullptr;
        }
        delete hold_;
        hold_ = nullptr;
        delete retired_.exchange(nullptr, std::memory_order_acquire);
        // The frozen copies go too: the dials count from now.
        if (current_ != nullptr)
            current_->useFrozen(nullptr);
        delete frozen_;
        frozen_ = nullptr;
        for (Patch::Frozen*& f : frozenHold_)
        {
            delete f;
            f = nullptr;
        }
        for (auto& slot : frozenRetired_)
            delete slot.exchange(nullptr);
        delete frozenPending_.exchange(nullptr);
        frozenNow_.store(false, std::memory_order_relaxed);
        samples_ = lastMove_ = 0;
        stillOf_ = nullptr;
        xfading_ = false;
        xfade_.lin.cur = 1.0f;
        xfade_.setTarget(1.0f);
        voices_ = {};
        clock_ = 0;
        if (current_ != nullptr && current_->rate() != fs_)
            current_->reset(fs_);
        fedPatch_ = nullptr;                         // the next block snaps the dials
        load_.store(0.0f, std::memory_order_relaxed);
    }

    void Engine::restart()
    {
        prepare(fs_, maxBlock_);
        const std::lock_guard<std::mutex> lock(mutex_);
        if (current_ != nullptr)
            current_->reset(fs_);
    }

    void Engine::offer(std::unique_ptr<Patch> patch)
    {
        if (!patch)
            return;
        const std::lock_guard<std::mutex> lock(mutex_);
        if (patch->rate() != fs_)                    // compiled for another rate than prepare() last set
            patch->reset(fs_);
        delete pending_.exchange(patch.release(), std::memory_order_acq_rel);   // one the audio thread never took
        delete retired_.exchange(nullptr, std::memory_order_acquire);
    }

    void Engine::offerFrozen(std::unique_ptr<Patch::Frozen> frozen)
    {
        if (!frozen)
            return;
        const std::lock_guard<std::mutex> lock(mutex_);
        delete frozenPending_.exchange(frozen.release(), std::memory_order_acq_rel);   // one never taken
        for (auto& slot : frozenRetired_)
            delete slot.exchange(nullptr, std::memory_order_acquire);
    }

    void Engine::collect()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        delete retired_.exchange(nullptr, std::memory_order_acquire);
        for (auto& slot : frozenRetired_)
            delete slot.exchange(nullptr, std::memory_order_acquire);
    }

    int Engine::activeVoices() const noexcept
    {
        int n = 0;
        for (const Voice& v : voices_)
            n += v.patch != nullptr ? 1 : 0;
        return n;
    }

    // ---- the audio thread: programs ------------------------------------------------------------------------------------
    void Engine::retire(Patch*& p) noexcept
    {
        if (p == nullptr)
            return;
        Patch* expected = nullptr;
        if (retired_.compare_exchange_strong(expected, p, std::memory_order_release, std::memory_order_relaxed))
            p = nullptr;                             // collect() destroys it
    }

    void Engine::takePending() noexcept
    {
        retire(hold_);
        if (hold_ != nullptr || old_ != nullptr || pending_.load(std::memory_order_acquire) == nullptr)
            return;                                  // a swap is still under way, or nothing waits
        Patch* next = pending_.exchange(nullptr, std::memory_order_acq_rel);
        if (next == nullptr)
            return;
        old_ = current_;
        current_ = next;
        if (old_ != nullptr)
            for (int c = 0; c < kMaxControls; ++c)   // the old program holds its dials still while it goes
                old_->setStream(c, nullptr);
        if constexpr (kInstrument)
        {
            // The old program's voices fade out; the keys still held start again on the new one.
            for (Voice& v : voices_)
                if (v.patch != nullptr && v.patch == old_)
                {
                    v.fading = true;
                    v.fall = v.gain / static_cast<float>(swapFade_);
                }
            for (int k = 0; k < 128; ++k)
                if (held_[static_cast<std::size_t>(k)])
                    noteOn(k, heldVelocity_[static_cast<std::size_t>(k)]);
        }
        else
        {
            xfade_.lin.cur = 0.0f;
            xfade_.setTarget(1.0f);
            xfading_ = true;
        }
    }

    // ---- the audio thread: frozen dials ---------------------------------------------------------------------------------
    bool Engine::fits(const Patch::Frozen& f) const noexcept
    {
        if (current_ == nullptr || old_ != nullptr || xfading_ || f.program() != current_->program())
            return false;
        const Controls& cs = current_->controls();
        for (int c = 0; c < kMaxControls; ++c)
        {
            const Glide& g = glide_[static_cast<std::size_t>(c)];
            if (cs[static_cast<std::size_t>(c)].used && (g.moving() || g.cur != f.values()[static_cast<std::size_t>(c)]))
                return false;
        }
        return true;
    }

    bool Engine::holdFrozen(Patch::Frozen* f) noexcept
    {
        for (Patch::Frozen*& slot : frozenHold_)
            if (slot == nullptr)
            {
                slot = f;
                return true;
            }
        return false;
    }

    void Engine::retireFrozen() noexcept
    {
        for (Patch::Frozen*& f : frozenHold_)
            for (auto& slot : frozenRetired_)
            {
                if (f == nullptr)
                    break;
                Patch::Frozen* expected = nullptr;
                if (slot.compare_exchange_strong(expected, f, std::memory_order_release, std::memory_order_relaxed))
                    f = nullptr;
            }
    }

    void Engine::updateFrozen() noexcept
    {
        retireFrozen();
        if (current_ != stillOf_)
        {
            stillOf_ = current_;
            lastMove_ = samples_;
        }
        if (moved_)
            lastMove_ = samples_;
        // The copy stops playing the block a dial moves, another program comes or a swap runs, and goes back once a
        // slot is free (until then it is kept, unused).
        if (frozen_ != nullptr && (moved_ || current_ == nullptr || frozen_->program() != current_->program()
                                   || old_ != nullptr || xfading_ || !current_->frozen()))
        {
            if (current_ != nullptr)
                current_->useFrozen(nullptr);
            if (old_ != nullptr)
                old_->useFrozen(nullptr);
            if (holdFrozen(frozen_))
                frozen_ = nullptr;
        }
        // A copy offered: it plays if it fits, else it goes back.
        if (frozenPending_.load(std::memory_order_acquire) != nullptr
            && std::find(frozenHold_.begin(), frozenHold_.end(), nullptr) != frozenHold_.end())
            if (Patch::Frozen* f = frozenPending_.exchange(nullptr, std::memory_order_acq_rel); f != nullptr)
            {
                if (fits(*f))
                {
                    if (frozen_ != nullptr && !holdFrozen(frozen_))
                        holdFrozen(f);               // no room to let the old one go: this one waits to go instead
                    else
                    {
                        frozen_ = f;
                        current_->useFrozen(f);
                    }
                }
                else
                    holdFrozen(f);
            }
        const bool playing = current_ != nullptr && current_->frozen();
        frozenNow_.store(playing, std::memory_order_relaxed);
        dialState_.program = current_ != nullptr ? current_->program() : 0;
        dialState_.stillSeconds = static_cast<double>(samples_ - lastMove_) / static_cast<double>(fs_);
        dialState_.frozen = playing;
        const Controls* cs = current_ != nullptr ? &current_->controls() : nullptr;
        for (int c = 0; c < kMaxControls; ++c)
            dialState_.plain[static_cast<std::size_t>(c)] =
                cs != nullptr && (*cs)[static_cast<std::size_t>(c)].used ? glide_[static_cast<std::size_t>(c)].cur : 0.0f;
        dials_.publish(dialState_);
    }

    // ---- the audio thread: the dials -------------------------------------------------------------------------------------
    void Engine::setControls(const std::array<float, kMaxControls>& normalised) noexcept
    {
        norm_ = normalised;
        normSet_ = true;
    }

    void Engine::glideControls(int n) noexcept
    {
        if (current_ == nullptr)
            return;
        const Controls& cs = current_->controls();
        const bool snap = fedPatch_ != current_;
        fedPatch_ = current_;
        moved_ = false;
        for (int c = 0; c < kMaxControls; ++c)
        {
            const Control& info = cs[static_cast<std::size_t>(c)];
            Glide& g = glide_[static_cast<std::size_t>(c)];
            streaming_[static_cast<std::size_t>(c)] = false;
            if (!info.used)
                continue;
            const float target = normSet_ ? plainOf(info, norm_[static_cast<std::size_t>(c)]) : info.def;
            if (snap || info.type != ControlType::real)
            {
                moved_ = moved_ || g.cur != target || g.moving();
                g.cur = g.tgt = target;
                g.left = 0;
                continue;
            }
            g.set(target, swapFade_);
            if (!g.moving())
                continue;
            moved_ = true;
            float* buf = streamBuf_.data() + static_cast<std::size_t>(c) * static_cast<std::size_t>(maxBlock_);
            for (int i = 0; i < n; ++i)
                buf[i] = g.tick();
            streaming_[static_cast<std::size_t>(c)] = true;
        }
    }

    void Engine::feed(Patch& p, int offset, int instance) noexcept
    {
        const Controls& cs = p.controls();
        for (int c = 0; c < kMaxControls; ++c)
        {
            const Control& info = cs[static_cast<std::size_t>(c)];
            if (!info.used)
                continue;
            const Glide& g = glide_[static_cast<std::size_t>(c)];
            if (streaming_[static_cast<std::size_t>(c)])
            {
                const float* buf = streamBuf_.data() + static_cast<std::size_t>(c) * static_cast<std::size_t>(maxBlock_);
                p.setStream(c, buf + offset, instance);
                p.setValue(c, g.tgt, instance);
            }
            else
            {
                p.setStream(c, nullptr, instance);
                p.setValue(c, g.cur, instance);
            }
        }
    }

    // ---- the audio thread: a block ---------------------------------------------------------------------------------------
    void Engine::process(float* const* channels, int numChannels, int numSamples, std::span<const Event> events) noexcept
    {
        if (numChannels <= 0 || channels == nullptr)
            return;
        const int n = std::min(numSamples, maxBlock_);
        if (maxBlock_ == 0 || n <= 0)
        {
            if (maxBlock_ == 0)                      // not prepared: an effect's input passes, sanitised
                for (int c = 0; c < numChannels; ++c)
                {
                    if constexpr (kInstrument)
                        std::fill(channels[c], channels[c] + std::max(0, numSamples), 0.0f);
                    else
                        fsdsp::sanitize(channels[c], channels[c], std::max(0, numSamples));
                }
            if constexpr (kInstrument)
                for (const Event& e : events)        // a block of 0 samples still plays its events
                {
                    if (e.kind == EventKind::noteOn)
                        noteOn(e.note, e.velocity);
                    else if (e.kind == EventKind::noteOff)
                        noteOff(e.note);
                    else if (e.kind == EventKind::allNotesOff)
                        allNotesOff();
                    else
                        controller(e);
                }
            return;
        }
        const auto t0 = std::chrono::steady_clock::now();
        takePending();
        glideControls(n);
        updateFrozen();
        measureIn(channels, numChannels, n);
        if constexpr (kInstrument)
            processInstrument(channels, numChannels, n, events);
        else
            processEffect(channels, numChannels, n);
        publish(channels, numChannels, n);
        samples_ += static_cast<uint64_t>(n);
        if (running_)                                // the song's position at the next call's first sample
            beat_ += static_cast<double>(n) * bpm_ / (60.0 * static_cast<double>(fs_));

        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const float share = static_cast<float>(took * static_cast<double>(fs_) / static_cast<double>(n));
        const float decayed = load_.load(std::memory_order_relaxed) * std::pow(0.5f, static_cast<float>(n) / (0.3f * fs_));
        load_.store(std::max(share, decayed), std::memory_order_relaxed);
    }

    namespace
    {
        // A channel's levels after n more samples (what is not finite counts as 0): its peak, which falls 20 dB a
        // second between louder ones (so that a reader at a lower rate sees every peak), and its RMS, smoothed over
        // 50 ms.
        void levels(const float* x, int n, float fs, float& peak, float& rms) noexcept
        {
            float p = 0.0f;
            double s = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const float v = std::isfinite(x[i]) ? x[i] : 0.0f;
                p = std::max(p, std::fabs(v));
                s += static_cast<double>(v) * static_cast<double>(v);
            }
            const float seconds = static_cast<float>(n) / std::max(fs, 1.0f);
            peak = std::max(p, peak * std::exp(-2.30258509f * seconds));
            const float keep = std::exp(-seconds / 0.05f);
            const float block = static_cast<float>(s / static_cast<double>(std::max(n, 1)));
            rms = std::sqrt(keep * rms * rms + (1.0f - keep) * block);
            if (peak < 1.0e-9f)
                peak = 0.0f;
            if (rms < 1.0e-9f)
                rms = 0.0f;
        }
    }

    void Engine::measureIn(float* const* channels, int numChannels, int n) noexcept
    {
        const uint32_t at = scopeAt_.load(std::memory_order_relaxed);
        if constexpr (kInstrument)
        {
            frame_.inPeak[0] = frame_.inPeak[1] = frame_.inRms[0] = frame_.inRms[1] = 0.0f;
            for (int i = 0; i < n; ++i)
                scopeIn_[(at + static_cast<uint32_t>(i)) & (kScopeLength - 1)].store(0.0f, std::memory_order_relaxed);
        }
        else
        {
            const float* l = channels[0];
            const float* r = numChannels > 1 ? channels[1] : channels[0];
            levels(l, n, fs_, frame_.inPeak[0], frame_.inRms[0]);
            levels(r, n, fs_, frame_.inPeak[1], frame_.inRms[1]);
            for (int i = 0; i < n; ++i)
            {
                const float v = 0.5f * (l[i] + r[i]);
                scopeIn_[(at + static_cast<uint32_t>(i)) & (kScopeLength - 1)].store(std::isfinite(v) ? v : 0.0f,
                                                                                    std::memory_order_relaxed);
            }
        }
    }

    void Engine::publish(float* const* channels, int numChannels, int n) noexcept
    {
        const float* l = channels[0];
        const float* r = numChannels > 1 ? channels[1] : channels[0];
        levels(l, n, fs_, frame_.outPeak[0], frame_.outRms[0]);
        levels(r, n, fs_, frame_.outPeak[1], frame_.outRms[1]);
        const uint32_t at = scopeAt_.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
            scopeOut_[(at + static_cast<uint32_t>(i)) & (kScopeLength - 1)].store(0.5f * (l[i] + r[i]), std::memory_order_relaxed);
        scopeAt_.store(at + static_cast<uint32_t>(n), std::memory_order_release);

        // The program's taps: an effect's instance, an instrument's newest voice that sounds.
        for (float& t : frame_.taps)
            t = 0.0f;
        frame_.program = current_ != nullptr ? current_->program() : 0;
        frame_.rate = fs_;
        if (current_ != nullptr && current_->taps() > 0)
        {
            int instance = -1;
            if constexpr (kInstrument)
            {
                uint64_t newest = 0;
                for (const Voice& v : voices_)
                    if (v.patch == current_ && !v.fading && !v.ending && (instance < 0 || v.age >= newest))
                    {
                        newest = v.age;
                        instance = v.instance;
                    }
            }
            else
                instance = 0;
            if (instance >= 0)
                for (int t = 0; t < std::min(current_->taps(), kMaxDisplays); ++t)
                    frame_.taps[t] = current_->tap(t, instance);
        }
        ++frame_.frame;
        meters_.publish(frame_);
    }

    void Engine::scope(bool input, float* out, int n) const noexcept
    {
        const int count = std::clamp(n, 0, kScopeLength);
        const uint32_t end = scopeAt_.load(std::memory_order_acquire);
        const std::array<std::atomic<float>, kScopeLength>& ring = input ? scopeIn_ : scopeOut_;
        for (int i = 0; i < count; ++i)
            out[i] = ring[(end - static_cast<uint32_t>(count) + static_cast<uint32_t>(i)) & (kScopeLength - 1)]
                         .load(std::memory_order_relaxed);
    }

    void Engine::processEffect(float* const* channels, int numChannels, int n) noexcept
    {
        float* left = channels[0];
        float* right = numChannels > 1 ? channels[1] : mono_.data();
        fsdsp::sanitize(left, left, n);
        if (numChannels > 1)
            fsdsp::sanitize(right, right, n);
        else
            std::copy(left, left + n, mono_.data());

        if (current_ == nullptr)
            return;                                  // no program yet: the input, as it came
        syncHost(0);
        if (!xfading_)
        {
            feed(*current_, 0, -1);
            current_->process(left, right, n);
        }
        else
        {
            // The old program (or the dry input, before the first) on copies of the input, the new one in place.
            std::copy(left, left + n, dryL_.data());
            std::copy(right, right + n, dryR_.data());
            if (old_ != nullptr)
                old_->process(dryL_.data(), dryR_.data(), n);
            feed(*current_, 0, -1);
            current_->process(left, right, n);
            guard(dryL_.data(), n);
            guard(dryR_.data(), n);
            guard(left, n);
            guard(right, n);
            for (int i = 0; i < n; ++i)
            {
                const auto s = static_cast<std::size_t>(i);
                const float w = xfade_.tick();
                if (w >= 1.0f)
                    continue;                        // landed: the new program's sample, as it is
                left[i] = dryL_[s] + w * (left[i] - dryL_[s]);
                right[i] = dryR_[s] + w * (right[i] - dryR_[s]);
            }
            if (!xfade_.moving())
            {
                xfading_ = false;
                hold_ = old_;
                old_ = nullptr;
                retire(hold_);
            }
        }
        guard(left, n);
        if (numChannels > 1)
            guard(right, n);
    }

    void Engine::processInstrument(float* const* channels, int numChannels, int n, std::span<const Event> events) noexcept
    {
        std::fill(accL_.begin(), accL_.begin() + n, 0.0f);
        std::fill(accR_.begin(), accR_.begin() + n, 0.0f);
        int done = 0;
        for (const Event& e : events)
        {
            const int at = std::clamp(static_cast<int>(e.offset), done, n);
            if (at > done)
            {
                render(done, at);
                done = at;
            }
            syncHost(at);                            // start and stop read the Host block at the event's sample
            if (e.kind == EventKind::noteOn)
                noteOn(e.note, e.velocity);
            else if (e.kind == EventKind::noteOff)
                noteOff(e.note);
            else if (e.kind == EventKind::allNotesOff)
                allNotesOff();
            else
                controller(e);
        }
        if (done < n)
            render(done, n);
        clock_ += static_cast<uint64_t>(n);

        // The old program goes once its last voice has.
        if (old_ != nullptr)
        {
            bool sounding = false;
            for (const Voice& v : voices_)
                sounding = sounding || v.patch == old_;
            if (!sounding)
            {
                hold_ = old_;
                old_ = nullptr;
                retire(hold_);
            }
        }

        guard(accL_.data(), n);
        guard(accR_.data(), n);
        if (numChannels > 1)
        {
            std::copy(accL_.begin(), accL_.begin() + n, channels[0]);
            std::copy(accR_.begin(), accR_.begin() + n, channels[1]);
            for (int c = 2; c < numChannels; ++c)
                std::fill(channels[c], channels[c] + n, 0.0f);
        }
        else
        {
            for (int i = 0; i < n; ++i)
                channels[0][i] = 0.5f * (accL_[static_cast<std::size_t>(i)] + accR_[static_cast<std::size_t>(i)]);
        }
    }

    // ---- the audio thread: voices ---------------------------------------------------------------------------------------
    int Engine::freeInstance(const Patch& p) const noexcept
    {
        for (int i = 0; i < p.instances(); ++i)
        {
            bool used = false;
            for (const Voice& v : voices_)
                used = used || (v.patch == &p && v.instance == i);
            if (!used)
                return i;
        }
        return -1;
    }

    void Engine::noteOn(int key, float velocity) noexcept
    {
        if (key < 0 || key > 127)
            return;
        held_[static_cast<std::size_t>(key)] = true;
        heldVelocity_[static_cast<std::size_t>(key)] = velocity;
        if (current_ == nullptr || maxBlock_ == 0)
            return;
        // The key's own voice stops first; then, with every voice taken, the oldest released one (else the oldest)
        // fades out to make room.
        int sounding = 0;
        Voice* oldest = nullptr;
        Voice* oldestReleased = nullptr;
        for (Voice& v : voices_)
        {
            if (v.patch != current_ || v.fading)
                continue;
            if (v.key == key && !v.released)
            {
                current_->stop(v.instance);
                v.released = true;
            }
            ++sounding;
            if (oldest == nullptr || v.age < oldest->age)
                oldest = &v;
            if (v.released && (oldestReleased == nullptr || v.age < oldestReleased->age))
                oldestReleased = &v;
        }
        if (sounding >= Patch::kVoices)
            if (Voice* victim = oldestReleased != nullptr ? oldestReleased : oldest; victim != nullptr)
            {
                victim->fading = true;
                victim->fall = victim->gain / static_cast<float>(stealFade_);
            }
        int instance = freeInstance(*current_);
        if (instance < 0)                            // every instance still fading: take the quietest of them now
        {
            Voice* quietest = nullptr;
            for (Voice& v : voices_)
                if (v.patch == current_ && (quietest == nullptr || v.gain < quietest->gain))
                    quietest = &v;
            if (quietest == nullptr)
                return;
            instance = quietest->instance;
            *quietest = Voice{};
        }
        for (Voice& v : voices_)
            if (v.patch == nullptr)
            {
                v = Voice{};
                v.patch = current_;
                v.instance = instance;
                v.key = key;
                v.age = ++noteCount_;
                feed(*current_, 0, instance);
                current_->start(instance, static_cast<float>(key), std::clamp(velocity, 0.0f, 1.0f));
                return;
            }
    }

    void Engine::noteOff(int key) noexcept
    {
        if (key < 0 || key > 127)
            return;
        held_[static_cast<std::size_t>(key)] = false;
        for (Voice& v : voices_)
            if (v.patch != nullptr && v.patch == current_ && v.key == key && !v.released)
            {
                if (host_.sustain)
                {
                    v.pedal = true;                  // the pedal holds it; its stop comes when the pedal goes up
                    continue;
                }
                current_->stop(v.instance);
                v.released = true;
            }
    }

    void Engine::controller(const Event& e) noexcept
    {
        if (e.kind == EventKind::pitchBend)
            host_.pitchbend = std::clamp(e.velocity, -1.0f, 1.0f);
        else if (e.kind == EventKind::pressure)
            host_.aftertouch = std::clamp(e.velocity, 0.0f, 1.0f);
        else if (e.kind == EventKind::controller && e.note == 1)
            host_.modwheel = std::clamp(e.velocity, 0.0f, 1.0f);
        else if (e.kind == EventKind::controller && e.note == 64)
        {
            const bool down = e.velocity >= 0.5f;
            if (host_.sustain && !down)
                for (Voice& v : voices_)             // the pedal up: the keys let go meanwhile stop now
                    if (v.patch != nullptr && v.pedal && !v.released)
                    {
                        v.patch->stop(v.instance);
                        v.released = true;
                        v.pedal = false;
                    }
            host_.sustain = down;
        }
    }

    void Engine::syncHost(int offset) noexcept
    {
        host_.bpm = static_cast<float>(bpm_);
        const double at = running_ ? static_cast<double>(offset) * bpm_ / (60.0 * static_cast<double>(fs_)) : 0.0;
        host_.beat = static_cast<float>(beat_ + at);
        if (current_ != nullptr)
            current_->setHost(host_);
        if (old_ != nullptr)
            old_->setHost(host_);
    }

    void Engine::setTransport(const Transport& t) noexcept
    {
        if (!t.known)
        {
            running_ = true;                         // no host clock: our own, at 120 BPM, never playing
            bpm_ = 120.0;
            host_.playing = false;
            return;
        }
        bpm_ = t.bpm > 1.0 && t.bpm < 1000.0 ? t.bpm : 120.0;
        beat_ = std::isfinite(t.beat) ? t.beat : 0.0;
        running_ = t.playing;
        host_.playing = t.playing;
        host_.barStart = static_cast<float>(std::isfinite(t.barStart) ? t.barStart : 0.0);
        host_.beatsPerBar = t.beatsPerBar > 0 ? t.beatsPerBar : 4;
        host_.beatUnit = t.beatUnit > 0 ? t.beatUnit : 4;
    }

    void Engine::allNotesOff() noexcept
    {
        held_.fill(false);
        for (Voice& v : voices_)
            if (v.patch != nullptr && v.patch == current_ && !v.released)
            {
                current_->stop(v.instance);
                v.released = true;
                v.pedal = false;
            }
    }

    void Engine::render(int from, int to) noexcept
    {
        // In parts that end on the absolute grid (or at `to`), so that a voice ends at a multiple of kGrid.
        constexpr auto grid = static_cast<uint64_t>(kGrid);
        for (int a = from; a < to;)
        {
            const auto phase = static_cast<int>((clock_ + static_cast<uint64_t>(a)) % grid);
            const int b = std::min(to, a + (kGrid - phase));
            renderPart(a, b, (clock_ + static_cast<uint64_t>(b)) % grid == 0);
            a = b;
        }
    }

    void Engine::renderPart(int from, int to, bool onGrid) noexcept
    {
        const int len = to - from;
        if (len <= 0)
            return;
        syncHost(from);
        for (Voice& v : voices_)
        {
            if (v.patch == nullptr)
                continue;
            std::fill(voiceL_.begin(), voiceL_.begin() + len, 0.0f);
            std::fill(voiceR_.begin(), voiceR_.begin() + len, 0.0f);
            if (v.patch == current_)
                feed(*current_, from, v.instance);
            const bool sounding = v.patch->render(v.instance, voiceL_.data(), voiceR_.data(), len);
            bool finite = true;
            for (int i = 0; i < len && finite; ++i)
                finite = std::isfinite(voiceL_[static_cast<std::size_t>(i)]) &&
                         std::isfinite(voiceR_[static_cast<std::size_t>(i)]);
            if (!finite)                             // a voice gone bad is dropped: its program starts it afresh
            {
                alerts_.fetch_or(nonFinite, std::memory_order_relaxed);
                v = Voice{};
                continue;
            }
            float* outL = accL_.data() + from;
            float* outR = accR_.data() + from;
            if (!v.fading)
            {
                for (int i = 0; i < len; ++i)
                {
                    outL[i] += voiceL_[static_cast<std::size_t>(i)];
                    outR[i] += voiceR_[static_cast<std::size_t>(i)];
                }
            }
            else
            {
                for (int i = 0; i < len; ++i)
                {
                    v.gain = std::max(0.0f, v.gain - v.fall);
                    outL[i] += v.gain * voiceL_[static_cast<std::size_t>(i)];
                    outR[i] += v.gain * voiceR_[static_cast<std::size_t>(i)];
                }
            }
            v.ending = v.ending || !sounding || (v.fading && v.gain <= 0.0f);
            if (v.ending && onGrid)
                v = Voice{};
        }
    }

    void Engine::guard(float* x, int n) noexcept
    {
        uint32_t raised = 0;
        for (int i = 0; i < n; ++i)
        {
            const float s = x[i];
            if (!std::isfinite(s))
            {
                x[i] = 0.0f;
                raised |= nonFinite;
            }
            else if (std::fabs(s) > kLimit)
            {
                x[i] = std::copysign(kLimit, s);
                raised |= clipped;
            }
        }
        if (raised != 0)
            alerts_.fetch_or(raised, std::memory_order_relaxed);
    }
}
