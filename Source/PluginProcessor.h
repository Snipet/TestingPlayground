#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "dsp/Gain.h"
#include "dsp/VoiceScheduler.h"

class TestingPlaygroundSynthProcessor : public juce::AudioProcessor,
                                    private juce::AudioProcessorValueTreeState::Listener
{
public:
    TestingPlaygroundSynthProcessor();
    ~TestingPlaygroundSynthProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi()  const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& dest) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

private:
    void parameterChanged(const juce::String& id, float newValue) override;

    testingplaygroundsynth::VoiceScheduler<> synth;
    testingplaygroundsynth::Gain gain;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TestingPlaygroundSynthProcessor)
};
