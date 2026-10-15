// Source/plugin/Processor.h: the JUCE AudioProcessor. It owns the parameters (an APVTS built from the table in
// Source/dsp/Params.h), one FunkGui ParamPort per parameter (what the editor's controls write through), the LFO shape
// (EditorPort.h: what the editor's graph edits) and the Engine (Source/dsp/Engine.h), which it calls once per block
// with the plain parameter values.
//
// Effect or instrument comes from cmake/Product.cmake (product::kIsInstrument in the generated Product.h): an effect
// has a stereo input and output and processes in place; an instrument has an output only and gets the block's MIDI
// notes as Events. Product constants come from Product.h, never from JUCE's JucePlugin_* macros.
//
// Threads: the audio thread reads the parameters' atomics (APVTS raw values) once per block, keeping the last good
// value of one a host set to NaN, and takes a new LFO table when one was published (a Seqlock: no lock, no
// allocation; a read that meets a publish in progress is retried next block); it never allocates, locks, or touches the
// editor. The editor (message thread) reaches the processor only through ports() and editorPort(). The LFO shape is
// kept under a mutex shared by the message thread and the host's state calls (never the audio thread), and saved in
// the state beside the parameters (an <LFO> element of <POINT x y curve> children). setStateInformation also loads the
// Visage version's sessions (root <PARAMS>: GAIN), and loads a non-finite value as the parameter's default. Hosts call
// getTailLengthSeconds() from any thread: the Engine's tailSeconds() is an atomic.
#pragma once

#include "plugin/EditorPort.h"

#include "dsp/Engine.h"
#include "dsp/Events.h"
#include "dsp/LfoShape.h"
#include "dsp/Params.h"

#include "fsdsp/core/Seqlock.h"

#include <funkgui/params/JuceParamPort.h>
#include <funkgui/params/ParamPort.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace testingplayground
{
    class Processor final : public juce::AudioProcessor, private EditorPort
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
        using juce::AudioProcessor::processBlock;               // the double-precision overload keeps JUCE's default

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
        EditorPort& editorPort() noexcept { return *this; }
        const Engine& engine() const noexcept { return engine_; }

        static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    private:
        static BusesProperties buses();                          // the kind's buses (product::kIsInstrument)

        // EditorPort
        LfoShape shape() const override;
        uint64_t shapeRevision() const override;
        void setShape(const LfoShape& shape) override;
        void endShapeEdit() override;
        float sampleRate() const override;

        void storeShape(const LfoShape& shape);                 // sanitise, keep, publish the table (any thread but audio)

        juce::AudioProcessorValueTreeState apvts_;
        std::array<std::atomic<float>*, param::count> raw_{};   // the plain values the audio thread reads
        // Declared after apvts_, so they are destroyed before the parameters they refer to.
        std::vector<std::unique_ptr<funkgui::JuceParamPort>> portStore_;
        std::vector<funkgui::ParamPort*> ports_;

        // The LFO shape: kept here (shapeMutex_), its table published to the audio thread (table_, published_).
        mutable std::mutex shapeMutex_;
        LfoShape shape_ = defaultShape();
        std::atomic<uint64_t> shapeRevision_{ 1 };
        fsdsp::Seqlock<LfoTable> table_;
        std::atomic<uint32_t> published_{ 0 };                   // bumps after every publish
        uint32_t taken_ = 0;                                     // audio thread: the last publish handed to the Engine
        bool takeTable_ = true;                                  // audio thread: hand the table over on the next block
        LfoTable incoming_{};                                    // audio thread: the read's destination
        std::atomic<float> sampleRate_{ 48000.0f };

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
