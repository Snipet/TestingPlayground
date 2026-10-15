#pragma once

#include <atomic>
#include "Voice.h"

namespace testingplaygroundsynth
{
    // Owns a fixed pool of voices and routes MIDI note events to them.
    //
    // Polyphonic with simple voice allocation:
    //   - noteOn  : reuse the voice already playing this note, else a free
    //               voice, else steal the oldest-started voice.
    //   - noteOff : release every voice playing that note.
    //
    // The caller (the processor) iterates the MidiBuffer and drives noteOn/
    // noteOff and renderBlock at the right sample offsets, keeping this class
    // free of any JUCE dependency. Audio-thread only once prepared: no
    // allocation, no locks.
    template <int NumVoices = 16>
    class VoiceScheduler
    {
    public:
        void prepare(double sampleRate) noexcept
        {
            for (auto& v : voices_)
                v.prepare(sampleRate);
            for (auto& a : ages_)
                a = 0;
            ageCounter_ = 0;
        }

        // Envelope setters — called from the message thread (parameterChanged).
        // Stored atomically; the audio thread applies them when a voice starts.
        void setAttack(float seconds) noexcept
        {
            attackSeconds_.store(seconds, std::memory_order_relaxed);
        }

        void setRelease(float seconds) noexcept
        {
            releaseSeconds_.store(seconds, std::memory_order_relaxed);
        }

        void noteOn(int midiNote, float velocity) noexcept
        {
            Voice* target = findVoicePlaying(midiNote);
            if (target == nullptr) target = findFreeVoice();
            if (target == nullptr) target = stealOldestVoice();

            target->setAttackTime(attackSeconds_.load(std::memory_order_relaxed));
            target->setReleaseTime(releaseSeconds_.load(std::memory_order_relaxed));
            target->noteOn(midiNote, velocity);
            ages_[indexOf(target)] = ageCounter_++;
        }

        void noteOff(int midiNote) noexcept
        {
            for (auto& v : voices_)
                if (v.isActive() && v.note() == midiNote)
                    v.noteOff();
        }

        void allNotesOff() noexcept
        {
            for (auto& v : voices_)
                v.noteOff();
        }

        // Renders a segment of the block, summing every active voice and
        // adding the mono mix into each output channel.
        void renderBlock(float* const* channels, int numChannels,
                         int startSample, int numSamples) noexcept
        {
            for (int i = 0; i < numSamples; ++i)
            {
                float mix = 0.0f;
                for (auto& v : voices_)
                    mix += v.renderNextSample();

                mix *= kHeadroom;   // leave headroom for chords

                const int idx = startSample + i;
                for (int ch = 0; ch < numChannels; ++ch)
                    channels[ch][idx] += mix;
            }
        }

    private:
        static constexpr float kHeadroom = 0.25f;

        int indexOf(const Voice* v) const noexcept
        {
            return static_cast<int>(v - voices_);
        }

        Voice* findVoicePlaying(int note) noexcept
        {
            for (auto& v : voices_)
                if (v.isActive() && v.note() == note)
                    return &v;
            return nullptr;
        }

        Voice* findFreeVoice() noexcept
        {
            for (auto& v : voices_)
                if (! v.isActive())
                    return &v;
            return nullptr;
        }

        Voice* stealOldestVoice() noexcept
        {
            Voice*   oldest    = &voices_[0];
            unsigned oldestAge = ages_[0];
            for (int i = 1; i < NumVoices; ++i)
            {
                if (ages_[i] < oldestAge)
                {
                    oldestAge = ages_[i];
                    oldest    = &voices_[i];
                }
            }
            return oldest;
        }

        Voice    voices_[NumVoices];
        unsigned ages_[NumVoices]   = {};
        unsigned ageCounter_        = 0;

        std::atomic<float> attackSeconds_{ 0.005f };
        std::atomic<float> releaseSeconds_{ 0.050f };
    };
}
