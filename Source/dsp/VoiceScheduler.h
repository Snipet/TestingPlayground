// Source/dsp/VoiceScheduler.h: which voice plays a note. The legacy synth's policy (its VoiceScheduler), unchanged:
//
//   note-on   the voice already sounding this note (held or releasing) plays it again; else the first silent voice
//             (the lowest index); else the voice whose note-on came first is stolen (a note played again counts from
//             its new note-on)
//   note-off  releases every voice playing that note (the Engine asks plays())
//
// It only decides and keeps the bookkeeping (each voice's note and note-on order); the Engine carries the decision out:
// a retrigger, a start, or a steal (a 2 ms fade, then the start). While a stolen voice fades out it belongs to its new
// note already: a second note-on of that note finds it, a note-off of the old note does not. Audio thread only; no
// allocation.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace testingplaygroundsynth
{
    template <std::size_t N>
    class VoiceScheduler
    {
    public:
        enum class Action : uint8_t
        {
            retrigger,      // the voice sounds this note already: play it again
            start,          // a silent voice: start the note now
            steal,          // every voice sounds: this one, the oldest note, fades out and then starts the note
        };

        struct Assignment
        {
            std::size_t voice = 0;
            Action action = Action::start;
        };

        void reset() noexcept
        {
            note_.fill(-1);
            order_.fill(0);
            counter_ = 0;
        }

        // `sounding[v]`: voice v is busy (held, releasing, or fading out after a steal).
        Assignment noteOn(int note, const std::array<bool, N>& sounding) noexcept
        {
            Assignment a;
            if (!find(note, sounding, a.voice))
            {
                a.action = Action::steal;
                a.voice = 0;
                for (std::size_t v = 0; v < N; ++v)
                {
                    if (!sounding[v])
                    {
                        a.action = Action::start;
                        a.voice = v;
                        break;
                    }
                    if (order_[v] < order_[a.voice])
                        a.voice = v;
                }
            }
            else
            {
                a.action = Action::retrigger;
            }
            note_[a.voice] = note;
            order_[a.voice] = ++counter_;
            return a;
        }

        // The note voice v belongs to (after a steal: the new one); -1 when it has had none since reset().
        int note(std::size_t v) const noexcept { return note_[v]; }
        bool plays(std::size_t v, int note) const noexcept { return note_[v] == note; }

    private:
        bool find(int note, const std::array<bool, N>& sounding, std::size_t& voice) const noexcept
        {
            for (std::size_t v = 0; v < N; ++v)
                if (sounding[v] && note_[v] == note)
                {
                    voice = v;
                    return true;
                }
            return false;
        }

        std::array<int, N> note_ = filled(-1);
        std::array<uint64_t, N> order_{};       // note-on order: 1, 2, ...; 0 = never
        uint64_t counter_ = 0;

        static constexpr std::array<int, N> filled(int x) noexcept
        {
            std::array<int, N> a{};
            a.fill(x);
            return a;
        }
    };
}
