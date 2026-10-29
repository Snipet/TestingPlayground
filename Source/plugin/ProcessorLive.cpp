// Source/plugin/ProcessorLive.cpp: the Processor's part of the live tools (Processor.h): what listen reads of the plugin
// as it plays (chatLive), and the journal get_status `since` reads (Journal.h): poll() compares the dials, the bypass
// and the host's transport from one call to the next, and the calls that change things (the draft, the runs, the
// projects, the chat's tools, prepareToPlay) say so.
#include "plugin/Processor.h"

#include "Product.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace playground
{
    namespace
    {
        std::string bpmText(float bpm)
        {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.1f BPM", static_cast<double>(bpm));
            return buf;
        }

        std::string rateText(double rate)
        {
            char buf[32];
            const double k = rate / 1000.0;
            std::snprintf(buf, sizeof buf, std::fabs(k - std::round(k)) < 0.05 ? "%.0f kHz" : "%.1f kHz", k);
            return buf;
        }
    }

    // ---- ChatHost: the live tools (any thread) --------------------------------------------------------------------------
    void Processor::chatLive(LiveRead& io) const
    {
        io.metered = engine_.meters(io.meters);
        io.out.resize(static_cast<std::size_t>(Engine::kScopeLength));
        engine_.scope(false, io.out.data(), Engine::kScopeLength);
        if constexpr (!product::kIsInstrument)
        {
            io.in.resize(static_cast<std::size_t>(Engine::kScopeLength));
            engine_.scope(true, io.in.data(), Engine::kScopeLength);
        }
        else
            io.in.clear();
        io.blocks.resize(static_cast<std::size_t>(Engine::kBlockRing));
        const int got = engine_.blocks(io.blocksFrom, io.blocks.data(), Engine::kBlockRing, io.blocksTo);
        io.blocks.resize(static_cast<std::size_t>(got));
        io.bypassed = bypassed_.load(std::memory_order_relaxed);
        io.largestBlock = largestHostBlock_.load(std::memory_order_relaxed);
        io.hostBlocks = hostBlocks_.load(std::memory_order_relaxed);
        io.rate = engine_.rate();
        io.load = engine_.load();
        io.alerts = alerts_.load();
        const std::lock_guard<std::mutex> lock(programMutex_);
        io.program = program_;
        io.displays = panel_.displays;
    }

    void Processor::chatListening(bool on)
    {
        if (on)
            addMeterReader();
        else
            removeMeterReader();
    }

    Changes Processor::chatChanges(const std::string& since, bool issue) { return journal_.since(since, issue); }

    std::array<std::vector<std::string>, kMaxControls> Processor::chatDialOptions() const
    {
        const CompileStatus s = compiler_.status();             // as chatDials: the last compile's when it plays
        if (s.ok)
            return s.panel.options;
        const std::lock_guard<std::mutex> lock(programMutex_);
        return panel_.options;
    }

    // ---- the journal --------------------------------------------------------------------------------------------------
    std::string Processor::dialTextOf(int slot, float norm) const
    {
        const Control c = control(slot);
        return dialText(c, plainOf(c, norm), controlOptions(slot));
    }

    void Processor::syncDials()
    {
        for (int c = 0; c < kMaxControls; ++c)
        {
            const float v = raw_[static_cast<std::size_t>(c)]->load();
            if (std::isfinite(v))
                polled_[static_cast<std::size_t>(c)].store(v);
        }
    }

    void Processor::journalPoll()
    {
        // The dials moved since the last poll: the editor's (its ports say so) or the host's.
        for (int c = 0; c < kMaxControls; ++c)
        {
            const auto i = static_cast<std::size_t>(c);
            const bool editor = portStore_[i]->takeMoved();
            const float now = raw_[i]->load();
            const float was = polled_[i].load();
            if (!std::isfinite(now) || now == was)
                continue;
            polled_[i].store(now);
            const Control k = control(c);
            if (k.used)
                journal_.dial(editor ? Journal::Who::you : Journal::Who::host, c, k.name, dialTextOf(c, was), dialTextOf(c, now));
        }
        // The host's bypass and its transport.
        if (const bool b = bypassed_.load(std::memory_order_relaxed); b != polledBypass_)
        {
            journal_.bypass(b);
            polledBypass_ = b;
        }
        Meters m;
        if (!engine_.meters(m))
            return;
        const HostBlock& h = m.host;
        if (m.hostKnown && polledKnown_)
        {
            if (h.playing != polledPlaying_)
            {
                const double qpb = h.beatUnit > 0 ? h.beatsPerBar * 4.0 / h.beatUnit : 4.0;
                const int bar = static_cast<int>(std::floor(static_cast<double>(h.barStart) / std::max(qpb, 1e-9))) + 1;
                journal_.note(Journal::Who::host, h.playing ? "started playing, at " + bpmText(h.bpm) + ", bar " + std::to_string(bar)
                                                            : "stopped playing");
            }
            if (std::fabs(h.bpm - polledBpm_) >= 0.05f)
                journal_.dial(Journal::Who::host, -1, "the tempo", bpmText(polledBpm_), bpmText(h.bpm));
        }
        polledKnown_ = m.hostKnown;
        polledPlaying_ = h.playing;
        polledBpm_ = h.bpm;
    }

    void Processor::journalDraft(const Draft& before, const Draft& after, Journal::Who who)
    {
        const auto find = [](const Draft& d, const std::string& name) -> const SourceFile* {
            for (const SourceFile& f : d.files)
                if (f.name == name)
                    return &f;
            return nullptr;
        };
        std::vector<const SourceFile*> added, gone;
        for (const SourceFile& f : after.files)
        {
            const SourceFile* was = find(before, f.name);
            if (was == nullptr)
                added.push_back(&f);
            else if (was->text != f.text)
                journal_.file(who, f.name, "edited");
        }
        for (const SourceFile& f : before.files)
            if (find(after, f.name) == nullptr)
                gone.push_back(&f);
        if (added.size() == 1 && gone.size() == 1)               // a tab renamed
        {
            journal_.note(who, "renamed " + gone[0]->name + " to " + added[0]->name);
            if (gone[0]->text != added[0]->text)
                journal_.file(who, added[0]->name, "edited");
            return;
        }
        for (const SourceFile* f : added)
            journal_.file(who, f->name, "added");
        for (const SourceFile* f : gone)
            journal_.file(who, f->name, "closed");
    }

    void Processor::journalPrepare(double rate, int block)
    {
        if (prepared_ && rate != preparedRate_)
            journal_.note(Journal::Who::host, "changed the sample rate: " + rateText(preparedRate_) + " -> " + rateText(rate));
        if (prepared_ && block != maxBlock_)
            journal_.note(Journal::Who::host, "changed its largest block: " + std::to_string(maxBlock_) + " -> "
                                                  + std::to_string(block) + " samples");
        preparedRate_ = rate;
        largestHostBlock_.store(0, std::memory_order_relaxed);
    }
}
