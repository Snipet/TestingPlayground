// Source/plugin/Processor.h: the JUCE AudioProcessor. It owns the parameters (an APVTS built from the table in
// Source/dsp/Params.h), one FunkGui ParamPort per parameter (what the editor's controls write through), and the
// Engine (Source/dsp/Engine.h), which it calls once per block with the plain parameter values.
//
// Effect or instrument comes from cmake/Product.cmake (product::kIsInstrument in the generated Product.h): an effect
// has a stereo input and output and processes in place; an instrument has an output only and gets the block's MIDI
// notes as Events. Product constants come from Product.h, never from JUCE's JucePlugin_* macros.
//
// An instrument never loses a note-off: a block of 0 samples still hands its events to the Engine; a block with more
// than kMaxEventsPerBlock events is played in parts, cut at the event that does not fit; and while a host bypasses it
// (processBlockBypassed: there is no bypass parameter) the output is silent but the Engine keeps running on the
// block's note-offs and all-notes-offs (its note-ons are dropped), so a note let go meanwhile is not held afterwards.
// An effect's bypass is JUCE's default: the input passes through.
//
// Threads: the audio thread reads the parameters' atomics (APVTS raw values) once per block, keeping the last good
// value of one a host set to NaN; it never allocates, locks, or touches the editor. The editor (message thread)
// reaches the processor only through ports(). Hosts call getTailLengthSeconds() from any thread: the Engine's
// tailSeconds() must be safe there.
#pragma once

#include "dsp/Engine.h"
#include "dsp/Events.h"
#include "dsp/Params.h"

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

        static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    private:
        static BusesProperties buses();                          // the kind's buses (product::kIsInstrument)

        void readParameters() noexcept;                          // the plain values, to the Engine
        // The Engine over the block, with the block's MIDI as events (an instrument; without its note-ons when
        // `noteOns` is false), in parts when they do not fit in events_.
        void run(juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi, bool noteOns) noexcept;

        juce::AudioProcessorValueTreeState apvts_;
        std::array<std::atomic<float>*, param::count> raw_{};   // the plain values the audio thread reads
        // Declared after apvts_, so they are destroyed before the parameters they refer to.
        std::vector<std::unique_ptr<funkgui::JuceParamPort>> portStore_;
        std::vector<funkgui::ParamPort*> ports_;

        Engine engine_;
        ParamValues values_ = defaultValues();
        std::array<Event, kMaxEventsPerBlock> events_{};

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Processor)
    };

    // The editor factory: exactly one definition per target. Source/editor/gpu/CreateEditorGpu.cpp in the plugin (the
    // FunkGui editor); Source/plugin/CreateEditorGeneric.cpp in the probes (JUCE's generic editor; the probes run the
    // FunkGui Panel headless instead).
    juce::AudioProcessorEditor* createEditor(Processor& processor);
}
