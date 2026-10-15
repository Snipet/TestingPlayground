// Source/plugin/Processor.h: the JUCE AudioProcessor. It owns the parameters (an APVTS built from the table in
// Source/dsp/Params.h), one FunkGui ParamPort per parameter (what the editor's controls write through), and the
// Engine (Source/dsp/Engine.h), which it calls once per block with the plain parameter values.
//
// Effect or instrument comes from cmake/Product.cmake (product::kIsInstrument in the generated Product.h): an effect
// has a stereo input and output and processes in place; an instrument has an output only and gets the block's MIDI
// notes as Events. Product constants come from Product.h, never from JUCE's JucePlugin_* macros.
//
// What a host may do, and what the Engine sees of it:
//   - a block larger than prepareToPlay announced: the Engine gets it in parts of at most that size, so an Engine
//     may size its scratch memory in prepare() (its output does not depend on the split: the Engine's rule);
//   - a block of 0 samples (JUCE's VST3 wrapper passes them on): an instrument's Engine still gets its events;
//   - more than kMaxEventsPerBlock events in a block: the block is played in parts, cut at the event that does not
//     fit, so no event is lost;
//   - MIDI: note-on (at velocity 0 a note-off), note-off, and the controllers All Sound Off (120) and All Notes Off
//     (123), both an allNotesOff event; everything else is ignored;
//   - bypass (processBlockBypassed; there is no bypass parameter): nothing steps. Each edge, bypass and un-bypass, is
//     a 20 ms smootherstep crossfade (fsdsp::SmoothRamp over fsdsp::kFadeMs) between the Engine's output and the
//     bypassed sound, starting at the first block of the new state; a toggle during a fade turns it round where it
//     is. The Engine keeps running through the bypass (its output unheard), so un-bypassing fades in a state that
//     followed the host, not one frozen when the bypass began: after the fade the output is the one a run never
//     bypassed would give (an instrument's, when no note came or went meanwhile). Its CPU cost does not stop while
//     bypassed.
//       An effect's bypassed sound is its input, sanitised (NaN and inf as 0); the crossfade is equal-gain,
//     input (1 - w) + output w. An Engine with latency must delay that dry path by it.
//       An instrument's bypassed sound is silence: its output fades out, and back in. Its Engine runs on the bypassed
//     blocks' note-offs and all-notes-offs only (their note-ons are dropped): a note held through the bypass keeps
//     sounding, unheard, and fades back in when the bypass ends (the key is still down); a note let go during it
//     releases, so it is not held afterwards.
//     The first block after prepareToPlay starts in its state, bypassed or not, without a fade;
//   - processBlock or processBlockBypassed before any prepareToPlay (JUCE's contract forbids it; a host that breaks it
//     must not crash the plugin): the Engine is not called; an effect passes its input through, sanitised (NaN and
//     inf as 0), and an instrument is silent;
//   - a NaN written to a parameter: the Engine keeps the last good value, the host's text for it is a dash, and a
//     state holding a value that is not a finite number loads that parameter at its default.
//
// Threads: the audio thread reads the parameters' atomics (APVTS raw values) once per block; it never allocates,
// locks, or touches the editor. The editor (message thread) reaches the processor only through ports(). Hosts call
// getTailLengthSeconds() from any thread: the Engine's tailSeconds() must be safe there.
#pragma once

#include "dsp/Engine.h"
#include "dsp/Events.h"
#include "dsp/Params.h"

#include "fsdsp/core/Ramp.h"

#include <funkgui/params/JuceParamPort.h>
#include <funkgui/params/ParamPort.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <memory>
#include <span>
#include <vector>

namespace testingplaygroundsynth
{
    class Processor final : public juce::AudioProcessor
    {
    public:
        Processor();
        ~Processor() override;

        const juce::String getName() const override;
        bool acceptsMidi() const override;
        bool producesMidi() const override;
        bool isMidiEffect() const override;
        double getTailLengthSeconds() const override;

        bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
        void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
        void releaseResources() override;
        void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
        void processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
        using juce::AudioProcessor::processBlock;               // the double-precision overloads keep JUCE's default
        using juce::AudioProcessor::processBlockBypassed;

        bool hasEditor() const override;
        juce::AudioProcessorEditor* createEditor() override;

        int getNumPrograms() override;
        int getCurrentProgram() override;
        void setCurrentProgram(int index) override;
        const juce::String getProgramName(int index) override;
        void changeProgramName(int index, const juce::String& newName) override;

        void getStateInformation(juce::MemoryBlock& destData) override;
        void setStateInformation(const void* data, int sizeInBytes) override;

        // ---- for the editor (message thread) and the probes --------------------------------------------------------
        juce::AudioProcessorValueTreeState& parameters() noexcept { return apvts_; }
        std::span<funkgui::ParamPort* const> ports() const noexcept { return ports_; }   // table order
        const Engine& engine() const noexcept { return engine_; }
        // The most samples one Engine::process call got since prepareToPlay (never more than it announced).
        int largestEngineCall() const noexcept { return largestCall_; }

        static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    private:
        static BusesProperties buses();                          // the kind's buses (product::kIsInstrument)

        // processBlock and processBlockBypassed: the bypass fade's target, the parameters, the Engine.
        void hostBlock(juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi, bool bypassed) noexcept;
        void readParameters() noexcept;                          // the plain values, to the Engine
        // The Engine over the block, with the block's MIDI as events (an instrument; without its note-ons when
        // `bypassed`), in parts of at most maxBlock_ samples and kMaxEventsPerBlock events, under the bypass fade.
        void run(juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi, bool bypassed) noexcept;
        // Around one Engine call while the output is not the Engine's alone: keepDry before it (an effect's input,
        // sanitised, to dry_), bypassMix after it (the fade, sample by sample, or the bypassed sound).
        void keepDry(float* const* channels, int numChannels, int numSamples) noexcept;
        void bypassMix(float* const* channels, int numChannels, int numSamples) noexcept;
        void runUnprepared(juce::AudioBuffer<float>& buffer) noexcept;   // before any prepareToPlay

        juce::AudioProcessorValueTreeState apvts_;
        std::array<std::atomic<float>*, param::count> raw_{};   // the plain values the audio thread reads
        // Declared after apvts_, so they are destroyed before the parameters they refer to.
        std::vector<std::unique_ptr<funkgui::JuceParamPort>> portStore_;
        std::vector<funkgui::ParamPort*> ports_;

        Engine engine_;
        ParamValues values_ = defaultValues();
        std::array<Event, kMaxEventsPerBlock> events_{};
        // prepareToPlay and the audio callbacks never overlap (JUCE's contract), so these need no atomics.
        bool prepared_ = false;
        int maxBlock_ = 1;                                       // what prepareToPlay announced (at least 1)
        int largestCall_ = 0;

        // The bypass: w, the Engine's share of the output, 1 while processing and 0 while bypassed, ramped over 20 ms
        // between them (one tick per sample, so the fade does not depend on how the host splits its blocks).
        fsdsp::SmoothRamp wet_{};
        bool wetSet_ = false;                                    // false until the first block after prepareToPlay
        std::vector<float> weight_;                              // w per sample of one Engine call (maxBlock_)
        std::vector<float> dry_;                                 // an effect's input per channel (stride maxBlock_)

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Processor)
    };

    // The editor factory: exactly one definition per target. Source/editor/gpu/CreateEditorGpu.cpp in the plugin (the
    // FunkGui editor); Source/plugin/CreateEditorGeneric.cpp in the probes (JUCE's generic editor; the probes run the
    // FunkGui Panel headless instead).
    juce::AudioProcessorEditor* createEditor(Processor& processor);
}
