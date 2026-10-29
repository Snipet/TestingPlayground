// Tools/probes/common/Rig.h: the Engine as the processor runs it, for the probes (JUCE-free, header-only). A Rig
// prepares an Engine, loads a program into it (compiled for its rate, taken at once: no fade), and runs a signal
// through it in blocks, with the dials at their defaults or as told, and an instrument's notes at absolute samples.
#pragma once

#include "Audio.h"

#include "dsp/Controls.h"
#include "dsp/Engine.h"
#include "dsp/Events.h"
#include "dsp/Sources.h"
#include "fminus/Patch.h"

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace probe
{
    struct Note
    {
        std::size_t at = 0;                                      // the absolute sample
        playground::EventKind kind = playground::EventKind::noteOn;
        int note = 60;
        float velocity = 0.8f;
    };

    class Rig
    {
    public:
        explicit Rig(double fs = 48000.0, int maxBlock = 512) : fs_(fs), maxBlock_(maxBlock)
        {
            engine_.prepare(fs_, maxBlock_);
        }

        // Compiles `source` and plays it from the next block, without a fade (as prepareToPlay takes a waiting one).
        // False, with the messages in `errors`, when it does not compile.
        bool load(const std::string& source, std::string* errors = nullptr)
        {
            return load(playground::singleFile(source), errors);
        }

        // The same for a program of several files (Sources.h).
        bool load(const playground::Sources& files, std::string* errors = nullptr)
        {
            playground::Patch::Result r = playground::Patch::compile(files, engine_.rate());
            if (errors != nullptr)
                *errors = r.errors + r.notes;
            if (!r.patch)
                return false;
            controls_ = r.patch->controls();
            panel_ = r.patch->panel();
            r.patch->setProgram(++loaded_);                     // as the Compiler names them
            recipe_ = r.patch->recipe();
            engine_.offer(std::move(r.patch));
            engine_.prepare(fs_, maxBlock_);
            norm_ = defaults();
            return true;
        }

        // The last program loaded's id (Patch::program): 1 for the first.
        uint64_t program() const noexcept { return loaded_; }

        // The dials at their programs' defaults, normalised.
        std::array<float, playground::kMaxControls> defaults() const
        {
            std::array<float, playground::kMaxControls> n{};
            for (std::size_t c = 0; c < n.size(); ++c)
                n[c] = playground::normOf(controls_[c], controls_[c].def);
            return n;
        }

        // `in` (an effect's input; an instrument's length) through the Engine in blocks of `block` (a block size
        // `sizes` repeats, when given), with `notes` (ascending) and the dials `norm_`, which `perBlock` may change
        // before each block (its argument: the block's first sample).
        Stereo run(const Stereo& in, int block, const std::vector<Note>& notes = {},
                   const std::vector<int>& sizes = {},
                   const std::function<void(std::size_t, std::array<float, playground::kMaxControls>&)>& perBlock = {})
        {
            Stereo out = in;
            std::size_t pos = 0, k = 0, nextNote = 0;
            std::vector<playground::Event> events;
            while (pos < in.size())
            {
                const int want = sizes.empty() ? block : sizes[k++ % sizes.size()];
                const auto len = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(std::min(want, maxBlock_)),
                                                                        in.size() - pos));
                events.clear();
                while (nextNote < notes.size() && notes[nextNote].at < pos + static_cast<std::size_t>(len))
                {
                    const Note& n = notes[nextNote++];
                    playground::Event e;
                    e.offset = static_cast<uint32_t>(n.at >= pos ? n.at - pos : 0);
                    e.kind = n.kind;
                    e.note = static_cast<uint8_t>(n.note);
                    e.velocity = n.velocity;
                    events.push_back(e);
                }
                if (perBlock)
                    perBlock(pos, norm_);
                engine_.setControls(norm_);
                float* channels[2] = { out.l.data() + pos, out.r.data() + pos };
                engine_.process(channels, 2, len, events);
                pos += static_cast<std::size_t>(len);
            }
            return out;
        }

        playground::Engine& engine() noexcept { return engine_; }
        // The frozen copy of the program that plays (Patch::freeze), at the dials as the last block left them, offered
        // to the Engine: false, with why, when it does not compile. `exactMath`: in the program's Math::Vector, as the
        // plugin's copy is not (dsp.freeze measures it).
        bool freeze(std::string* why = nullptr, bool exactMath = false)
        {
            playground::DialState d;
            if (!engine_.dials(d))
                return false;
            std::string w;
            std::unique_ptr<playground::Patch::Frozen> f = playground::Patch::freeze(recipe_, d.plain, w, exactMath);
            if (why != nullptr)
                *why = w;
            if (!f)
                return false;
            engine_.offerFrozen(std::move(f));
            return true;
        }
        const playground::Patch::Recipe& recipe() const noexcept { return recipe_; }

        const playground::Controls& controls() const noexcept { return controls_; }
        const playground::PanelSpec& panel() const noexcept { return panel_; }      // its #pragma panel lines
        std::array<float, playground::kMaxControls>& norm() noexcept { return norm_; }
        double fs() const noexcept { return fs_; }

    private:
        double fs_;
        int maxBlock_;
        playground::Engine engine_;
        playground::Controls controls_{};
        playground::PanelSpec panel_;
        std::array<float, playground::kMaxControls> norm_{};
        uint64_t loaded_ = 0;
        playground::Patch::Recipe recipe_;
    };

    // Stereo noise at `amp` (a fixed seed: the same every run), `n` samples.
    inline Stereo noise(std::size_t n, double amp = 0.25, uint32_t seed = 12345)
    {
        Stereo s(n);
        uint32_t x = seed;
        const auto next = [&x] {
            x = x * 1664525u + 1013904223u;
            return static_cast<double>(x >> 8) / 8388608.0 - 1.0;
        };
        for (std::size_t i = 0; i < n; ++i)
        {
            s.l[i] = static_cast<float>(amp * next());
            s.r[i] = static_cast<float>(amp * next());
        }
        return s;
    }
}
