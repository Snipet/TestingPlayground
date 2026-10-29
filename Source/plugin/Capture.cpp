// Source/plugin/Capture.cpp: see Capture.h.
#include "plugin/Capture.h"

#include "Product.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <type_traits>

namespace playground::capture
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr std::size_t kBlockWords = sizeof(Block) / 8;
        constexpr std::size_t kNoteWords = sizeof(Note) / 8;
        static_assert(std::is_trivially_copyable_v<Block> && sizeof(Block) % 8 == 0);
        static_assert(std::is_trivially_copyable_v<Note> && sizeof(Note) % 8 == 0);
        constexpr uint64_t kLastSlack = 8192;               // last's ring: what the newest block may write past `seconds`
        constexpr std::size_t kPrograms = 8;                // the programs' files kept (noteProgram)
        constexpr double kGestureGap = 0.05;                // seconds of stillness between two moves of a dial

        // A value as atomic words: what a reader copies is never a data race, and a slot overwritten while it copied is
        // left out by the ring's announced count (Capture.h).
        template <class T>
        void putWords(std::atomic<uint64_t>* dst, const T& v) noexcept
        {
            uint64_t w[sizeof(T) / 8];
            std::memcpy(w, &v, sizeof(T));
            for (std::size_t i = 0; i < sizeof(T) / 8; ++i)
                dst[i].store(w[i], std::memory_order_relaxed);
        }

        template <class T>
        T getWords(const std::atomic<uint64_t>* src) noexcept
        {
            uint64_t w[sizeof(T) / 8];
            for (std::size_t i = 0; i < sizeof(T) / 8; ++i)
                w[i] = src[i].load(std::memory_order_relaxed);
            T v;
            std::memcpy(&v, w, sizeof(T));
            return v;
        }

        // n samples into a ring of `frames` from sample `at` (it wraps).
        void putSamples(std::atomic<float>* ring, uint64_t frames, uint64_t at, const float* x, int n) noexcept
        {
            const uint64_t i0 = at % frames;
            const uint64_t first = std::min<uint64_t>(static_cast<uint64_t>(n), frames - i0);
            for (uint64_t i = 0; i < first; ++i)
                ring[i0 + i].store(x[i], std::memory_order_relaxed);
            for (uint64_t i = first; i < static_cast<uint64_t>(n); ++i)
                ring[i - first].store(x[i], std::memory_order_relaxed);
        }

        void getSamples(const std::atomic<float>* ring, uint64_t frames, uint64_t from, uint64_t count, float* out) noexcept
        {
            for (uint64_t i = 0; i < count; ++i)
                out[i] = ring[(from + i) % frames].load(std::memory_order_relaxed);
        }

        int64_t nanos(Clock::time_point t) noexcept
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
        }

        template <class A>
        void maxStore(A& a, uint64_t v) noexcept                 // the audio thread's own counter: monotonic
        {
            if (v > a.load(std::memory_order_relaxed))
                a.store(v, std::memory_order_relaxed);
        }
    }

    // One recording: its buffers (allocated and touched by arm), the audio thread's positions and header, and the
    // counters that cross.
    struct Recorder::Ring
    {
        // arm's, before it is announced; read-only after (but `epoch`, the audio thread's once it has it).
        Options options;
        float rate = 48000.0f;
        int part = 0;
        uint32_t epoch = 0;
        Clock::time_point armedAt{};
        bool input = false;                                  // an effect's: the input's two streams
        uint64_t frames = 0, blockCap = 0, eventCap = 0;
        std::array<std::unique_ptr<std::atomic<float>[]>, 4> audio;   // in L, in R, out L, out R
        std::unique_ptr<std::atomic<uint64_t>[]> blocks, events;
        std::size_t bytes = 0;

        // The audio thread's; the header (from `started` on) published with the first block.
        uint64_t s = 0, e = 0, b = 0;                        // samples, events, blocks published
        uint64_t alertUntil = 0;                             // stopOnAlert: the sample it ends at (0: none yet)
        bool started = false;
        Start start = Start::arm;
        int channels = 2;
        int voicesAtStart = 0;
        float peakBefore = 0.0f;
        uint64_t clock = 0;
        Clock::time_point startedAt{};

        // Crossing: what is about to be written (announced), what is published (blocksDone), and the flags.
        std::atomic<uint64_t> samplesAnnounced{ 0 }, eventsAnnounced{ 0 }, blocksAnnounced{ 0 }, blocksDone{ 0 };
        std::atomic<int64_t> lastAt{ 0 };                    // its last block's end (steady clock, ns)
        std::atomic<uint8_t> end{ 0 };                       // End, once it ends by itself
        std::atomic<uint8_t> trimmed{ 0 };                   // Trim flags (next's)
        std::atomic<bool> claimed{ false };                  // the audio thread took it
        std::atomic<bool> stopAsked{ false };                // the reader is done with it: hand it back

        void finish(End why) noexcept
        {
            if (end.load(std::memory_order_relaxed) == 0)
                end.store(static_cast<uint8_t>(why), std::memory_order_release);
        }
        void trim(uint8_t flag) noexcept
        {
            trimmed.store(static_cast<uint8_t>(trimmed.load(std::memory_order_relaxed) | flag), std::memory_order_relaxed);
        }
    };

    Recorder::Recorder() : lastUse_(Clock::now()) {}

    Recorder::~Recorder() = default;                         // owned_ frees every ring

    void Recorder::setOwnCode(std::function<void(bool)> hook)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        ownHook_ = std::move(hook);
    }

    // ---- the audio thread --------------------------------------------------------------------------------------------
    void Recorder::prepare(float rate, int part) noexcept
    {
        rate_.store(rate, std::memory_order_relaxed);
        part_.store(part, std::memory_order_relaxed);
        epoch_.fetch_add(1, std::memory_order_release);
    }

    void Recorder::begin(int n, bool bypassed, const Transport& t, const std::array<float, kMaxControls>& dials,
                         const Engine& engine) noexcept
    {
        writing_ = false;
        aborted_ = false;
        const uint32_t epoch = epoch_.load(std::memory_order_acquire);
        if (epoch != seenEpoch_)                             // prepared again: the Engine's clock starts again
        {
            seenEpoch_ = epoch;
            clock_ = 0;
            lastPlaying_ = false;
            lastPeak_ = 0.0f;
        }
        const uint64_t len = static_cast<uint64_t>(std::max(n, 0));
        const uint64_t clock = clock_;
        clock_ += len;
        const bool playing = t.known && t.playing;
        const bool playStarts = playing && !lastPlaying_;
        lastPlaying_ = playing;

        // One the reader is done with goes back (when the slot is free; else it waits, untouched, a block more).
        if (live_ != nullptr && live_->stopAsked.load(std::memory_order_acquire))
        {
            if (retired_.load(std::memory_order_acquire) != nullptr)
                return;
            retired_.store(live_, std::memory_order_release);
            live_ = nullptr;
        }
        // One armed is taken once none is held.
        if (live_ == nullptr && armed_.load(std::memory_order_relaxed) != nullptr)
            if (Ring* r = armed_.exchange(nullptr, std::memory_order_acq_rel); r != nullptr)
            {
                live_ = r;
                r->claimed.store(true, std::memory_order_release);
            }
        Ring* r = live_;
        if (r == nullptr || r->end.load(std::memory_order_relaxed) != 0)
            return;
        if (r->epoch != epoch)                               // the host prepared the plugin again since
        {
            // Before it started, at the same rate and part size, it goes on; else it ends there (the Engine's programs
            // and its clock started again, which a replay would not).
            if (r->started || r->rate != rate_.load(std::memory_order_relaxed) || r->part != part_.load(std::memory_order_relaxed))
            {
                r->finish(End::prepared);
                return;
            }
            r->epoch = epoch;
        }
        if (r->options.mode == Mode::next)
        {
            if (r->b + 1 > r->blockCap)
                r->trim(blocksLimit);
            if (r->s + len > r->frames || r->b + 1 > r->blockCap)
            {
                r->finish(End::full);
                return;
            }
        }
        if (r->started && r->alertUntil != 0 && r->s >= r->alertUntil)
        {
            r->finish(End::alert);
            return;
        }

        writing_ = true;
        startNow_ = r->started || r->options.mode == Mode::last;
        startWhy_ = Start::arm;
        if (!startNow_ && playStarts)
        {
            startNow_ = true;
            startWhy_ = Start::transport;
        }
        inAt_ = outAt_ = r->s;
        blockEvents_ = 0;
        peak_ = 0.0f;
        tally_ = engine.alertTally();
        // Before it starts: the voices sounding (Engine::activeVoices reads the audio thread's own table: no race here).
        voices_ = r->started ? 0 : engine.activeVoices();
        t0_ = Clock::now();
        if (!r->started)
            r->clock = clock;
        pending_ = Block{};
        pending_.sample = r->s;
        pending_.event = r->e;
        pending_.size = static_cast<uint32_t>(len);
        pending_.bpm = t.bpm;
        pending_.beat = t.beat;
        pending_.barStart = t.barStart;
        pending_.beatsPerBar = static_cast<uint8_t>(std::clamp(t.beatsPerBar, 0, 255));
        pending_.beatUnit = static_cast<uint8_t>(std::clamp(t.beatUnit, 0, 255));
        pending_.flags = static_cast<uint8_t>((t.known ? Block::kKnown : 0) | (playing ? Block::kPlaying : 0)
                                              | (bypassed ? Block::kBypassed : 0));
        pending_.dials = dials;
        // The samples this block writes: announced before they are (Capture.h).
        maxStore(r->samplesAnnounced, r->s + len);
        std::atomic_thread_fence(std::memory_order_release);
    }

    void Recorder::input(const float* const* channels, int numChannels, int n) noexcept
    {
        if (!writing_ || n <= 0 || numChannels <= 0 || channels == nullptr || !live_->input)
            return;
        Ring& r = *live_;
        for (int c = 0; c < 2; ++c)
            putSamples(r.audio[static_cast<std::size_t>(c)].get(), r.frames, inAt_, channels[std::min(c, numChannels - 1)], n);
        if (!startNow_)                                      // next: the input's first sound starts it
            for (int c = 0; c < std::min(numChannels, 2) && !startNow_; ++c)
                for (int i = 0; i < n; ++i)
                    if (std::fabs(channels[c][i]) >= kSoundThreshold)
                    {
                        startNow_ = true;
                        startWhy_ = Start::sound;
                        break;
                    }
        inAt_ += static_cast<uint64_t>(n);
    }

    void Recorder::output(const float* const* channels, int numChannels, int n) noexcept
    {
        if (!writing_ || n <= 0 || numChannels <= 0 || channels == nullptr)
            return;
        Ring& r = *live_;
        for (int c = 0; c < 2; ++c)
            putSamples(r.audio[static_cast<std::size_t>(2 + c)].get(), r.frames, outAt_, channels[std::min(c, numChannels - 1)], n);
        if (!r.started)
        {
            r.channels = std::min(numChannels, 2);
            for (int c = 0; c < std::min(numChannels, 2); ++c)
                for (int i = 0; i < n; ++i)
                    if (const float a = std::fabs(channels[c][i]); a > peak_ && std::isfinite(a))
                        peak_ = a;
        }
        outAt_ += static_cast<uint64_t>(n);
    }

    void Recorder::event(int at, const Event& e) noexcept
    {
        if (!writing_ || live_->eventCap == 0)
            return;
        Ring& r = *live_;
        if (!startNow_ && e.kind == EventKind::noteOn)
        {
            startNow_ = true;
            startWhy_ = Start::note;
        }
        const uint64_t k = r.e + blockEvents_;
        if (r.options.mode == Mode::next && k >= r.eventCap)
        {
            aborted_ = true;                                 // the block does not fit: the recording ends before it
            return;
        }
        maxStore(r.eventsAnnounced, k + 1);
        std::atomic_thread_fence(std::memory_order_release);
        Note x;
        x.sample = r.s + static_cast<uint64_t>(std::max(at, 0));
        x.kind = e.kind;
        x.note = e.note;
        x.velocity = e.velocity;
        putWords(&r.events[(k % r.eventCap) * kNoteWords], x);
        ++blockEvents_;
    }

    void Recorder::end(const Engine& engine) noexcept
    {
        if (!writing_)
            return;
        writing_ = false;
        Ring& r = *live_;
        if (aborted_)
        {
            r.trim(eventsLimit);
            r.finish(End::full);
            return;
        }
        if (!startNow_)                                      // next, still waiting: nothing of this block is published
        {
            lastPeak_ = peak_;
            return;
        }
        const Clock::time_point t1 = Clock::now();
        const uint64_t len = pending_.size;
        Meters m;
        if (engine.meters(m))
            pending_.program = m.program;
        // The Engine's steals and dropped voices go here once its Meters count them (pending_.steals, .dropped).
        pending_.events = blockEvents_;
        if (engine.frozen())
            pending_.flags = static_cast<uint8_t>(pending_.flags | Block::kFrozen);
        const uint64_t raised = engine.alertTally() - tally_;
        pending_.nonFinite = static_cast<uint16_t>(std::min<uint64_t>(raised & 0xffffffffu, 0xffffu));
        pending_.clipped = static_cast<uint16_t>(std::min<uint64_t>(raised >> 32, 0xffffu));
        if (len > 0)
            pending_.load = static_cast<float>(std::chrono::duration<double>(t1 - t0_).count() * static_cast<double>(r.rate)
                                               / static_cast<double>(len));
        if (!r.started)                                      // the header, published with this first block
        {
            r.started = true;
            r.start = startWhy_;
            r.voicesAtStart = voices_;
            r.peakBefore = lastPeak_;
            r.startedAt = t0_;
        }
        maxStore(r.blocksAnnounced, r.b + 1);
        std::atomic_thread_fence(std::memory_order_release);
        putWords(&r.blocks[(r.b % r.blockCap) * kBlockWords], pending_);
        r.s += len;
        r.e += blockEvents_;
        r.b += 1;
        r.lastAt.store(nanos(t1), std::memory_order_relaxed);
        r.blocksDone.store(r.b, std::memory_order_release);
        if (r.options.stopOnAlert && r.alertUntil == 0 && (pending_.nonFinite > 0 || pending_.clipped > 0))
            r.alertUntil = r.s + static_cast<uint64_t>(std::ceil(kAlertTailSeconds * static_cast<double>(r.rate)));
        if (r.options.mode == Mode::next && r.s >= r.frames)
            r.finish(End::full);
    }

    // ---- the reader's side -------------------------------------------------------------------------------------------
    Reply Recorder::act(Action action, const Options& options)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        lastUse_ = Clock::now();
        collect();
        const std::size_t before = heldBytes();
        Reply out;
        switch (action)
        {
            case Action::arm:    out = arm(options); break;
            case Action::stop:   out = stop(); break;
            case Action::take:   out = status(true); break;
            case Action::status: out = status(false); break;
            case Action::forget: out = forget(); break;
        }
        out.audio = rate_.load(std::memory_order_relaxed) > 0.0f;
        out.bytes = heldBytes();
        if (action == Action::forget)
            out.freed = before;
        return out;
    }

    void Recorder::housekeep()
    {
        const std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock())
            return;                                          // a call is copying: next round
        collect();
        if (current_ != nullptr && current_->end.load(std::memory_order_acquire) != 0)
            setOwn(false);                                   // it ended by itself: the frozen copy may play again
        if ((current_ != nullptr || take_ != nullptr)
            && std::chrono::duration<double>(Clock::now() - lastUse_).count() > kIdleSeconds)
            forget();
    }

    void Recorder::noteProgram(uint64_t program, const Sources& files)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        programs_.erase(std::remove_if(programs_.begin(), programs_.end(), [&](const auto& p) { return p.first == program; }),
                        programs_.end());
        programs_.emplace_back(program, files);
        if (programs_.size() > kPrograms)
            programs_.erase(programs_.begin());
    }

    std::size_t Recorder::bytes() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return heldBytes();
    }

    int Recorder::rings() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(owned_.size());
    }

    std::size_t Recorder::heldBytes() const
    {
        std::size_t n = take_ != nullptr ? take_->bytes : 0;
        for (const auto& r : owned_)
            n += r->bytes;
        return n;
    }

    void Recorder::setOwn(bool on)
    {
        if (on == own_)
            return;
        own_ = on;
        if (ownHook_)
            ownHook_(on);
    }

    void Recorder::destroy(Ring* r)
    {
        owned_.erase(std::remove_if(owned_.begin(), owned_.end(), [&](const auto& p) { return p.get() == r; }), owned_.end());
    }

    void Recorder::collect()
    {
        if (Ring* r = retired_.exchange(nullptr, std::memory_order_acq_rel); r != nullptr)
            destroy(r);
    }

    void Recorder::release(Ring* r)
    {
        Ring* expected = r;
        if (armed_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel))
        {
            destroy(r);                                      // never taken: nothing else has it
            return;
        }
        r->stopAsked.store(true, std::memory_order_release); // handed back at the audio thread's next block
    }

    State Recorder::stateOf(const Ring& r) const
    {
        if (r.end.load(std::memory_order_acquire) != 0)
            return State::ended;
        if (!r.claimed.load(std::memory_order_acquire) || r.blocksDone.load(std::memory_order_acquire) == 0)
            return State::waiting;
        return State::recording;
    }

    Reply Recorder::arm(const Options& o)
    {
        Reply out;
        const float rate = rate_.load(std::memory_order_relaxed);
        if (!(rate > 0.0f))
        {
            out.state = current_ != nullptr ? stateOf(*current_) : (take_ != nullptr ? State::kept : State::off);
            out.why = "the plugin is not running audio yet (the host has not started it)";
            return out;
        }
        if (current_ != nullptr)                             // the recording before, and its take, go
        {
            release(current_);
            current_ = nullptr;
        }
        take_.reset();

        auto r = std::make_unique<Ring>();
        r->options = o;
        r->options.seconds = std::clamp(std::isfinite(o.seconds) ? o.seconds : kDefaultSeconds, kMinSeconds, kMaxSeconds);
        r->rate = rate;
        r->part = part_.load(std::memory_order_relaxed);
        r->epoch = epoch_.load(std::memory_order_acquire);
        r->armedAt = Clock::now();
        r->input = !product::kIsInstrument;
        const auto asked = static_cast<uint64_t>(std::ceil(r->options.seconds * static_cast<double>(rate)));
        r->frames = asked + (o.mode == Mode::last ? kLastSlack : 0);
        r->blockCap = r->frames / kAverageBlock + 64;
        r->eventCap = product::kIsInstrument ? static_cast<uint64_t>(kEventCapacity) : 0;
        // Allocated and zeroed here (every page touched), so the audio thread never faults one in.
        for (std::size_t c = r->input ? 0 : 2; c < 4; ++c)
            r->audio[c] = std::make_unique<std::atomic<float>[]>(static_cast<std::size_t>(r->frames));
        r->blocks = std::make_unique<std::atomic<uint64_t>[]>(static_cast<std::size_t>(r->blockCap) * kBlockWords);
        if (r->eventCap > 0)
            r->events = std::make_unique<std::atomic<uint64_t>[]>(static_cast<std::size_t>(r->eventCap) * kNoteWords);
        r->bytes = static_cast<std::size_t>(r->frames) * sizeof(float) * (r->input ? 4u : 2u)
                 + static_cast<std::size_t>(r->blockCap) * sizeof(Block) + static_cast<std::size_t>(r->eventCap) * sizeof(Note);
        Ring* ring = r.get();
        owned_.push_back(std::move(r));
        if (Ring* old = armed_.exchange(ring, std::memory_order_acq_rel); old != nullptr)
            destroy(old);                                    // one never taken (release took it already, normally)
        current_ = ring;
        setOwn(true);
        out.state = State::waiting;
        out.armedNow = true;
        return out;
    }

    Reply Recorder::stop()
    {
        Reply out;
        if (current_ == nullptr)
        {
            out.state = take_ != nullptr ? State::kept : State::off;
            out.take = take_;
            if (take_ == nullptr)
                out.why = "nothing is recording";
            return out;
        }
        std::shared_ptr<Take> t = snapshot(*current_);
        if (t->end == End::none)
            t->end = End::stopped;
        release(current_);
        current_ = nullptr;
        setOwn(false);
        if (t->blocks.empty())
        {
            out.state = State::off;
            out.take = t;                                    // empty: what it waited for
            out.why = "nothing was recorded";
            return out;
        }
        take_ = t;
        out.state = State::kept;
        out.take = take_;
        return out;
    }

    Reply Recorder::status(bool take)
    {
        Reply out;
        if (current_ != nullptr && !take)
        {
            out.state = stateOf(*current_);
            out.take = snapshot(*current_);
            return out;
        }
        if (current_ != nullptr)
        {
            out.state = stateOf(*current_);
            out.why = "a recording is under way: stop it first";
            return out;
        }
        out.state = take_ != nullptr ? State::kept : State::off;
        out.take = take_;
        return out;
    }

    Reply Recorder::forget()
    {
        if (current_ != nullptr)
            release(current_);
        current_ = nullptr;
        take_.reset();
        setOwn(false);
        collect();
        Reply out;
        out.state = State::off;
        return out;
    }

    std::shared_ptr<Take> Recorder::snapshot(Ring& r) const
    {
        auto t = std::make_shared<Take>();
        t->options = r.options;
        t->rate = static_cast<double>(r.rate);
        t->part = r.part;
        t->input = r.input;
        t->claimed = r.claimed.load(std::memory_order_acquire);
        t->madeAt = Clock::now();
        t->armedAgo = std::chrono::duration<double>(t->madeAt - r.armedAt).count();
        t->end = static_cast<End>(r.end.load(std::memory_order_acquire));
        const uint64_t done = r.blocksDone.load(std::memory_order_acquire);
        t->trimmed = r.trimmed.load(std::memory_order_relaxed);
        if (done == 0)
            return t;
        // The header: written before the first block was published.
        t->start = r.start;
        t->channels = r.channels;
        t->voicesAtStart = r.voicesAtStart;
        t->peakBefore = r.peakBefore;
        const Clock::time_point now = t->madeAt;
        t->startedAgo = std::chrono::duration<double>(now - r.startedAt).count();
        t->endedAgo = static_cast<double>(nanos(now) - r.lastAt.load(std::memory_order_relaxed)) * 1e-9;

        // The blocks the ring still holds, then those the take keeps: last's `seconds`, what the samples' and the
        // events' rings hold.
        const uint64_t first = done > r.blockCap ? done - r.blockCap : 0;
        std::vector<Block> blocks(static_cast<std::size_t>(done - first));
        for (std::size_t k = 0; k < blocks.size(); ++k)
            blocks[k] = getWords<Block>(&r.blocks[((first + k) % r.blockCap) * kBlockWords]);
        const uint64_t sEnd = blocks.back().sample + blocks.back().size;
        const uint64_t eEnd = blocks.back().event + blocks.back().events;
        const auto keep = static_cast<uint64_t>(std::llround(r.options.seconds * static_cast<double>(r.rate)));
        std::size_t from = 0;
        while (from < blocks.size()
               && ((r.options.mode == Mode::last && sEnd - blocks[from].sample > keep) || sEnd - blocks[from].sample > r.frames
                   || (r.eventCap > 0 && eEnd - blocks[from].event > r.eventCap)))
            ++from;
        if (r.options.mode == Mode::last && from == 0 && first > 0 && sEnd - blocks.front().sample < std::min(keep, sEnd))
            t->trimmed = static_cast<uint8_t>(t->trimmed | blocksLimit);   // the blocks' ring did not reach back so far
        if (r.options.mode == Mode::last && from < blocks.size() && r.eventCap > 0 && eEnd - blocks[from].event >= r.eventCap
            && sEnd - blocks[from].sample < std::min(keep, sEnd))
            t->trimmed = static_cast<uint8_t>(t->trimmed | eventsLimit);
        t->recordedSeconds = static_cast<double>(sEnd) / static_cast<double>(r.rate);
        if (from == blocks.size())
            return t;
        const uint64_t s0 = blocks[from].sample, e0 = blocks[from].event;
        std::array<std::vector<float>, 4> audio;
        for (std::size_t c = r.input ? 0 : 2; c < 4; ++c)
        {
            audio[c].resize(static_cast<std::size_t>(sEnd - s0));
            getSamples(r.audio[c].get(), r.frames, s0, sEnd - s0, audio[c].data());
        }
        std::vector<Note> events(static_cast<std::size_t>(eEnd - e0));
        for (std::size_t k = 0; k < events.size(); ++k)
            events[k] = getWords<Note>(&r.events[((e0 + k) % r.eventCap) * kNoteWords]);
        // What the audio thread may have overwritten while this copied: left out.
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint64_t sa = r.samplesAnnounced.load(std::memory_order_relaxed);
        const uint64_t ea = r.eventsAnnounced.load(std::memory_order_relaxed);
        const uint64_t ba = r.blocksAnnounced.load(std::memory_order_relaxed);
        const uint64_t validS = sa > r.frames ? sa - r.frames : 0;
        const uint64_t validE = r.eventCap > 0 && ea > r.eventCap ? ea - r.eventCap : 0;
        const uint64_t validB = ba > r.blockCap ? ba - r.blockCap : 0;
        std::size_t keepFrom = from;
        while (keepFrom < blocks.size()
               && (first + keepFrom < validB || blocks[keepFrom].sample < validS || blocks[keepFrom].event < validE))
            ++keepFrom;
        if (keepFrom == blocks.size())
            return t;
        const uint64_t base = blocks[keepFrom].sample, eBase = blocks[keepFrom].event;
        for (std::size_t c = 0; c < 4; ++c)
            if (!audio[c].empty())
                audio[c].erase(audio[c].begin(), audio[c].begin() + static_cast<std::ptrdiff_t>(base - s0));
        t->in = { std::move(audio[0]), std::move(audio[1]) };
        t->out = { std::move(audio[2]), std::move(audio[3]) };
        events.erase(events.begin(), events.begin() + static_cast<std::ptrdiff_t>(eBase - e0));
        for (Note& x : events)
            x.sample -= base;
        t->events = std::move(events);
        blocks.erase(blocks.begin(), blocks.begin() + static_cast<std::ptrdiff_t>(keepFrom));
        for (Block& b : blocks)
        {
            b.sample -= base;
            b.event -= eBase;
        }
        t->blocks = std::move(blocks);
        t->clock = r.clock + base;
        t->fromSeconds = static_cast<double>(base) / static_cast<double>(r.rate);
        t->bytes = t->blocks.size() * sizeof(Block) + t->events.size() * sizeof(Note)
                 + (t->out[0].size() + t->out[1].size() + t->in[0].size() + t->in[1].size()) * sizeof(float);
        for (const Block& b : t->blocks)
            if (std::none_of(t->programs.begin(), t->programs.end(), [&](const auto& p) { return p.first == b.program; }))
                for (const auto& p : programs_)
                    if (p.first == b.program)
                        t->programs.push_back(p);
        return t;
    }

    // ---- the numbers -------------------------------------------------------------------------------------------------
    namespace
    {
        Levels levelsOf(const std::array<std::vector<float>, 2>& x)
        {
            Levels l;
            double sq = 0.0;
            uint64_t n = 0;
            double mean[2] = {};
            for (std::size_t c = 0; c < 2; ++c)
            {
                double sum = 0.0;
                uint64_t m = 0;
                for (const float v : x[c])
                {
                    if (!std::isfinite(v))
                    {
                        ++l.nonFinite;
                        continue;
                    }
                    const double d = static_cast<double>(v);
                    l.peak = std::max(l.peak, std::fabs(d));
                    sq += d * d;
                    sum += d;
                    ++m;
                }
                n += m;
                mean[c] = m > 0 ? sum / static_cast<double>(m) : 0.0;
            }
            l.rms = n > 0 ? std::sqrt(sq / static_cast<double>(n)) : 0.0;
            l.dc = std::fabs(mean[0]) >= std::fabs(mean[1]) ? mean[0] : mean[1];
            return l;
        }
    }

    Summary summarize(const Take& t)
    {
        Summary s;
        const double rate = t.rate > 0.0 ? t.rate : 48000.0;
        const auto at = [&](uint64_t sample) { return static_cast<double>(sample) / rate; };
        s.blocks = static_cast<int>(t.blocks.size());
        std::map<uint32_t, int> sizes;
        std::vector<float> loads;
        bool wasPlaying = false;
        for (std::size_t k = 0; k < t.blocks.size(); ++k)
        {
            const Block& b = t.blocks[k];
            const double when = at(b.sample), length = at(b.size);
            if (b.size > 0)
            {
                ++sizes[b.size];
                loads.push_back(b.load);
            }
            else
                ++s.empty;
            if (b.load > s.worstLoad)
            {
                s.worstLoad = b.load;
                s.worstAt = when;
            }
            if (k == 0 || b.program != t.blocks[k - 1].program)
                s.programs.emplace_back(when, b.program);
            if (b.has(Block::kFrozen))
            {
                s.frozenSeconds += length;
                if (s.frozenFrom < 0.0)
                    s.frozenFrom = when;
                s.frozenTo = when + length;
            }
            if (b.nonFinite > 0 || b.clipped > 0)
            {
                ++s.alertBlocks;
                if (s.alerts.size() < 3)
                    s.alerts.push_back({ when, b.nonFinite > 0, b.clipped > 0 });
                s.lastAlert = when;
            }
            s.steals += b.steals;
            s.dropped += b.dropped;
            if (b.has(Block::kKnown))
            {
                if (!s.transport)
                {
                    s.bpmLow = s.bpmHigh = b.bpm;
                    s.beatFrom = b.beat;
                    s.beatsPerBar = b.beatsPerBar;
                    s.beatUnit = b.beatUnit;
                }
                s.transport = true;
                s.bpmLow = std::min(s.bpmLow, b.bpm);
                s.bpmHigh = std::max(s.bpmHigh, b.bpm);
                s.beatTo = b.beat + (b.has(Block::kPlaying) ? length * b.bpm / 60.0 : 0.0);
            }
            const bool playing = b.has(Block::kPlaying);
            if (playing)
                s.playingSeconds += length;
            if (k > 0 && playing && !wasPlaying)
                ++s.transportStarts;
            if (k > 0 && !playing && wasPlaying)
                ++s.transportStops;
            wasPlaying = playing;
            if (b.has(Block::kBypassed))
            {
                s.bypassSeconds += length;
                if (s.bypassFrom < 0.0)
                    s.bypassFrom = when;
                s.bypassTo = when + length;
            }
        }
        for (const auto& [size, count] : sizes)
            if (count > s.commonCount)
            {
                s.commonCount = count;
                s.common = static_cast<int>(size);
            }
        if (!sizes.empty())
        {
            s.smallest = static_cast<int>(sizes.begin()->first);
            s.largest = static_cast<int>(sizes.rbegin()->first);
        }
        if (!loads.empty())
        {
            const auto mid = loads.begin() + static_cast<std::ptrdiff_t>(loads.size() / 2);
            std::nth_element(loads.begin(), mid, loads.end());
            s.medianLoad = *mid;
        }

        // The dials: each slot's range over the take and its moves.
        for (int c = 0; c < kMaxControls && !t.blocks.empty(); ++c)
        {
            const auto i = static_cast<std::size_t>(c);
            DialMove d;
            d.slot = c;
            d.low = d.high = t.blocks.front().dials[i];
            double lastChange = -1.0;
            for (std::size_t k = 1; k < t.blocks.size(); ++k)
            {
                const float v = t.blocks[k].dials[i];
                d.low = std::min(d.low, v);
                d.high = std::max(d.high, v);
                if (v == t.blocks[k - 1].dials[i])
                    continue;
                const double when = at(t.blocks[k].sample);
                if (d.moves == 0 || when - lastChange >= kGestureGap)
                    ++d.moves;
                if (d.moves == 1 && lastChange < 0.0)
                    d.first = when;
                d.last = lastChange = when;
            }
            if (d.moves > 0)
                s.dials.push_back(d);
        }

        // The notes, the pedal and the wheels, from the events in order.
        std::array<bool, 128> held{}, sustained{};
        int nHeld = 0, nSustained = 0;
        const auto setHeld = [&](std::size_t k, bool on) {
            nHeld += (on ? 1 : 0) - (held[k] ? 1 : 0);
            held[k] = on;
        };
        const auto setSustained = [&](std::size_t k, bool on) {
            nSustained += (on ? 1 : 0) - (sustained[k] ? 1 : 0);
            sustained[k] = on;
        };
        bool pedal = false;
        for (const Note& x : t.events)
        {
            const auto k = static_cast<std::size_t>(x.note & 127u);
            switch (x.kind)
            {
                case EventKind::noteOn:
                {
                    setHeld(k, true);
                    setSustained(k, false);
                    ++s.notes;
                    s.lowest = s.lowest < 0 ? x.note : std::min<int>(s.lowest, x.note);
                    s.highest = std::max<int>(s.highest, x.note);
                    const int v = std::clamp(static_cast<int>(std::lround(x.velocity * 127.0f)), 1, 127);
                    s.softest = std::min(s.softest, v);
                    s.hardest = std::max(s.hardest, v);
                    break;
                }
                case EventKind::noteOff:
                    if (held[k] && pedal)
                        setSustained(k, true);
                    setHeld(k, false);
                    break;
                case EventKind::allNotesOff:
                    held.fill(false);
                    sustained.fill(false);
                    nHeld = nSustained = 0;
                    ++s.allNotesOff;
                    break;
                case EventKind::controller:
                    if (x.note == 64)
                    {
                        const bool down = x.velocity >= 0.5f;   // as the Engine reads it
                        if (down && !pedal)
                            ++s.pedalDowns;
                        pedal = down;
                        if (!pedal)
                        {
                            sustained.fill(false);
                            nSustained = 0;
                        }
                    }
                    else if (x.note == 1)
                    {
                        ++s.modEvents;
                        s.modLow = std::min(s.modLow, x.velocity);
                        s.modHigh = std::max(s.modHigh, x.velocity);
                    }
                    break;
                case EventKind::pitchBend:
                    ++s.bendEvents;
                    s.bendLow = std::min(s.bendLow, x.velocity);
                    s.bendHigh = std::max(s.bendHigh, x.velocity);
                    break;
                case EventKind::pressure:
                    ++s.pressureEvents;
                    s.pressureHigh = std::max(s.pressureHigh, x.velocity);
                    break;
            }
            s.mostHeld = std::max(s.mostHeld, nHeld);
            s.mostSounding = std::max(s.mostSounding, nHeld + nSustained);   // a key held is never sustained too
        }
        if (t.input)
            s.in = levelsOf(t.in);
        s.out = levelsOf(t.out);
        return s;
    }
}
