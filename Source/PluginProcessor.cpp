#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace pid
{
    static constexpr const char* gain    = "gain";
    static constexpr const char* attack  = "attack";
    static constexpr const char* release = "release";
}

juce::AudioProcessorValueTreeState::ParameterLayout TestingPlaygroundSynthProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    p.push_back(std::make_unique<AudioParameterFloat>(
        ParameterID{ pid::gain, 1 }, "Gain",
        NormalisableRange<float>(-24.0f, 24.0f, 0.01f), 0.0f,
        AudioParameterFloatAttributes().withLabel("dB")));

    p.push_back(std::make_unique<AudioParameterFloat>(
        ParameterID{ pid::attack, 1 }, "Attack",
        NormalisableRange<float>(1.0f, 200.0f, 0.1f, 0.5f), 5.0f,
        AudioParameterFloatAttributes().withLabel("ms")));

    p.push_back(std::make_unique<AudioParameterFloat>(
        ParameterID{ pid::release, 1 }, "Release",
        NormalisableRange<float>(1.0f, 1000.0f, 0.1f, 0.5f), 50.0f,
        AudioParameterFloatAttributes().withLabel("ms")));

    return { p.begin(), p.end() };
}

TestingPlaygroundSynthProcessor::TestingPlaygroundSynthProcessor()
    : AudioProcessor(BusesProperties()
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMS", createLayout())
{
    for (auto id : { pid::gain, pid::attack, pid::release })
        apvts.addParameterListener(id, this);
}

TestingPlaygroundSynthProcessor::~TestingPlaygroundSynthProcessor()
{
    for (auto id : { pid::gain, pid::attack, pid::release })
        apvts.removeParameterListener(id, this);
}

void TestingPlaygroundSynthProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    synth.prepare(sampleRate);
    gain.prepare(sampleRate, samplesPerBlock);

    parameterChanged(pid::gain,    apvts.getRawParameterValue(pid::gain)->load());
    parameterChanged(pid::attack,  apvts.getRawParameterValue(pid::attack)->load());
    parameterChanged(pid::release, apvts.getRawParameterValue(pid::release)->load());
}

void TestingPlaygroundSynthProcessor::releaseResources() {}

bool TestingPlaygroundSynthProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void TestingPlaygroundSynthProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals _;
    const int numSamples  = buffer.getNumSamples();
    const int numChannels = getTotalNumOutputChannels();

    buffer.clear();

    // Walk the MIDI events in order, rendering the audio between them so that
    // note on/off land on the correct samples.
    auto* const* channels = buffer.getArrayOfWritePointers();
    int cursor = 0;

    for (const auto meta : midi)
    {
        const int pos = juce::jlimit(0, numSamples, meta.samplePosition);

        if (pos > cursor)
        {
            synth.renderBlock(channels, numChannels, cursor, pos - cursor);
            cursor = pos;
        }

        const auto msg = meta.getMessage();
        if (msg.isNoteOn())
            synth.noteOn(msg.getNoteNumber(), msg.getFloatVelocity());
        else if (msg.isNoteOff())
            synth.noteOff(msg.getNoteNumber());
        else if (msg.isAllNotesOff() || msg.isAllSoundOff())
            synth.allNotesOff();
    }

    if (cursor < numSamples)
        synth.renderBlock(channels, numChannels, cursor, numSamples - cursor);

    // Master output gain.
    gain.processBlock(channels, numChannels, numSamples);
}

void TestingPlaygroundSynthProcessor::parameterChanged(const juce::String& id, float v)
{
    if (id == pid::gain)    gain.setGainDb(v);
    if (id == pid::attack)  synth.setAttack(v * 0.001f);   // ms -> seconds
    if (id == pid::release) synth.setRelease(v * 0.001f);  // ms -> seconds
}

juce::AudioProcessorEditor* TestingPlaygroundSynthProcessor::createEditor()
{
    return new TestingPlaygroundSynthEditor(*this);
}

void TestingPlaygroundSynthProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary(*xml, dest);
}

void TestingPlaygroundSynthProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TestingPlaygroundSynthProcessor();
}
