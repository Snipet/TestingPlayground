#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace pid
{
    static constexpr const char* gain = "gain";
}

juce::AudioProcessorValueTreeState::ParameterLayout TestingPlaygroundProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    p.push_back(std::make_unique<AudioParameterFloat>(
        ParameterID{ pid::gain, 1 }, "Gain",
        NormalisableRange<float>(-24.0f, 24.0f, 0.01f), 0.0f,
        AudioParameterFloatAttributes().withLabel("dB")));

    return { p.begin(), p.end() };
}

TestingPlaygroundProcessor::TestingPlaygroundProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input",  juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMS", createLayout())
{
    for (auto id : { pid::gain })
        apvts.addParameterListener(id, this);
}

TestingPlaygroundProcessor::~TestingPlaygroundProcessor()
{
    for (auto id : { pid::gain })
        apvts.removeParameterListener(id, this);
}

void TestingPlaygroundProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    gain.prepare(sampleRate, samplesPerBlock);

    parameterChanged(pid::gain, apvts.getRawParameterValue(pid::gain)->load());
}

void TestingPlaygroundProcessor::releaseResources() {}

bool TestingPlaygroundProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& in  = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    return in == out && (in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono());
}

void TestingPlaygroundProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals _;
    const int n    = buffer.getNumSamples();
    const int nIn  = getTotalNumInputChannels();
    const int nOut = getTotalNumOutputChannels();

    for (int ch = nIn; ch < nOut; ++ch)
        buffer.clear(ch, 0, n);

    gain.processBlock(buffer.getArrayOfWritePointers(), nIn, n);
}

void TestingPlaygroundProcessor::parameterChanged(const juce::String& id, float v)
{
    if (id == pid::gain) gain.setGainDb(v);
}

juce::AudioProcessorEditor* TestingPlaygroundProcessor::createEditor()
{
    return new TestingPlaygroundEditor(*this);
}

void TestingPlaygroundProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary(*xml, dest);
}

void TestingPlaygroundProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TestingPlaygroundProcessor();
}
