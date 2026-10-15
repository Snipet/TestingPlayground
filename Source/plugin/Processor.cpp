// Source/plugin/Processor.cpp: see Processor.h.
#include "plugin/Processor.h"

#include "Product.h"

#include "fsdsp/core/Sanitize.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace testingplaygroundsynth
{
    namespace
    {
        // The host's text for a plain value (generic editors, automation lanes): the value and its unit.
        juce::String hostText(const ParamSpec& s, float v)
        {
            if (!std::isfinite(v))                               // a host's NaN: the sound keeps the last good value
                return juce::String::fromUTF8("\xE2\x80\x93");
            char buf[48];
            switch (s.unit)
            {
                case Unit::db:
                    std::snprintf(buf, sizeof buf, "%.1f dB", static_cast<double>(v));
                    break;
                case Unit::hz:
                    if (v < 10.0f)                               // low rates (an LFO) need their decimals
                        std::snprintf(buf, sizeof buf, "%.2f Hz", static_cast<double>(v));
                    else if (v < 100.0f)
                        std::snprintf(buf, sizeof buf, "%.1f Hz", static_cast<double>(v));
                    else if (v < 1000.0f)
                        std::snprintf(buf, sizeof buf, "%.0f Hz", static_cast<double>(v));
                    else
                        std::snprintf(buf, sizeof buf, "%.2f kHz", static_cast<double>(v) / 1000.0);
                    break;
                case Unit::ms:
                    if (v < 10.0f)
                        std::snprintf(buf, sizeof buf, "%.2f ms", static_cast<double>(v));
                    else if (v < 1000.0f)
                        std::snprintf(buf, sizeof buf, "%.1f ms", static_cast<double>(v));
                    else
                        std::snprintf(buf, sizeof buf, "%.2f s", static_cast<double>(v) / 1000.0);
                    break;
                case Unit::percent:
                    std::snprintf(buf, sizeof buf, "%.0f %%", static_cast<double>(v));
                    break;
                case Unit::none:
                    std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(v));
                    break;
            }
            return juce::String::fromUTF8(buf);
        }

        // The inverse, for a value typed into a host's field: a number, scaled by its unit word ("2.5 kHz", "1.2 s").
        float parseText(const ParamSpec& s, const juce::String& text)
        {
            const juce::String t = text.trim().toLowerCase();
            float v = t.getFloatValue();
            if (s.unit == Unit::hz && t.containsChar('k'))
                v *= 1000.0f;
            if (s.unit == Unit::ms && t.endsWith("s") && !t.endsWith("ms"))
                v *= 1000.0f;
            return std::clamp(v, s.min, s.max);
        }

        // The Event of a MIDI message an instrument plays, else false: note-on (at velocity 0, a note-off), note-off,
        // and the controllers All Sound Off (120) and All Notes Off (123), both allNotesOff. The caller sets the
        // offset. (An effect never calls it: maybe_unused.)
        [[maybe_unused]] bool toEvent(const juce::MidiMessageMetadata& m, Event& e) noexcept
        {
            if (m.numBytes < 1)
                return false;
            const int status = m.data[0] & 0xf0;
            const uint8_t d1 = m.numBytes > 1 ? m.data[1] : 0;
            const uint8_t d2 = m.numBytes > 2 ? m.data[2] : 0;
            e.note = d1;
            if (status == 0x90 && d2 > 0)
            {
                e.kind = EventKind::noteOn;
                e.velocity = static_cast<float>(d2) / 127.0f;
            }
            else if (status == 0x80 || status == 0x90)
                e.kind = EventKind::noteOff;
            else if (status == 0xb0 && (d1 == 120 || d1 == 123))
                e.kind = EventKind::allNotesOff;
            else
                return false;
            return true;
        }

        constexpr int kMaxChannels = 8;                          // handed to the Engine (the layouts allow two)
    }

    juce::AudioProcessor::BusesProperties Processor::buses()
    {
        const auto stereo = juce::AudioChannelSet::stereo();
        if constexpr (product::kIsInstrument)
            return BusesProperties().withOutput("Output", stereo, true);
        else
            return BusesProperties().withInput("Input", stereo, true).withOutput("Output", stereo, true);
    }

    juce::AudioProcessorValueTreeState::ParameterLayout Processor::createLayout()
    {
        std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
        for (const ParamSpec& s : kParams)
        {
            const juce::ParameterID id{ s.id, 1 };
            if (s.control == Control::choice)
            {
                juce::StringArray names;
                for (int i = 0; i < s.numChoices; ++i)
                    names.add(s.choices[i]);
                params.push_back(std::make_unique<juce::AudioParameterChoice>(id, s.name, names,
                                                                              static_cast<int>(s.def)));
                continue;
            }
            const ParamSpec* spec = &s;                              // kParams has static storage
            const auto attributes = juce::AudioParameterFloatAttributes()
                .withStringFromValueFunction([spec](float v, int) { return hostText(*spec, v); })
                .withValueFromStringFunction([spec](const juce::String& t) { return parseText(*spec, t); });
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id, s.name, juce::NormalisableRange<float>(s.min, s.max, 0.0f, skewOf(s)), s.def, attributes));
        }
        return { params.begin(), params.end() };
    }

    Processor::Processor()
        : juce::AudioProcessor(buses()),
          apvts_(*this, nullptr, "PARAMETERS", createLayout())
    {
        for (std::size_t i = 0; i < param::count; ++i)
        {
            raw_[i] = apvts_.getRawParameterValue(kParams[i].id);
            juce::RangedAudioParameter* p = apvts_.getParameter(kParams[i].id);
            jassert(raw_[i] != nullptr && p != nullptr);            // two rows with one id?
            portStore_.push_back(std::make_unique<funkgui::JuceParamPort>(*p));
            ports_.push_back(portStore_.back().get());
        }
    }

    Processor::~Processor() = default;

    const juce::String Processor::getName() const { return product::kName; }
    bool Processor::acceptsMidi() const { return product::kIsInstrument; }
    bool Processor::producesMidi() const { return false; }
    bool Processor::isMidiEffect() const { return false; }
    double Processor::getTailLengthSeconds() const { return engine_.tailSeconds(); }

    bool Processor::isBusesLayoutSupported(const BusesLayout& layouts) const
    {
        const juce::AudioChannelSet& in = layouts.getMainInputChannelSet();
        const juce::AudioChannelSet& out = layouts.getMainOutputChannelSet();
        const bool monoOrStereo = out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
        if constexpr (product::kIsInstrument)
            return monoOrStereo && in.isDisabled();
        else
            return monoOrStereo && in == out;
    }

    void Processor::prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock)
    {
        maxBlock_ = std::max(1, maximumExpectedSamplesPerBlock);
        largestCall_ = 0;
        engine_.prepare(sampleRate, maxBlock_);
        wet_.prepare(static_cast<float>(sampleRate));           // fsdsp::kFadeMs, 20 ms
        wetSet_ = false;
        weight_.assign(static_cast<std::size_t>(maxBlock_), 0.0f);
        if constexpr (!product::kIsInstrument)
            dry_.assign(static_cast<std::size_t>(kMaxChannels) * static_cast<std::size_t>(maxBlock_), 0.0f);
        prepared_ = true;
    }

    void Processor::releaseResources() {}

    void Processor::readParameters() noexcept
    {
        // A host can write a NaN (JUCE's ranges pass it through): such a value keeps the last good one, so the Engine
        // only ever sees finite values within the table's range (a NaN in a smoother would stay there for good).
        for (std::size_t i = 0; i < param::count; ++i)
            if (const float v = raw_[i]->load(std::memory_order_relaxed); std::isfinite(v))
                values_[i] = std::clamp(v, kParams[i].min, kParams[i].max);
        engine_.setParams(values_);
    }

    void Processor::run(juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi, bool bypassed) noexcept
    {
        const int n = buffer.getNumSamples();
        const int numOut = std::min(getTotalNumOutputChannels(), buffer.getNumChannels());
        for (int ch = getTotalNumInputChannels(); ch < numOut; ++ch)
            buffer.clear(ch, 0, n);
        const int numCh = std::min(numOut, kMaxChannels);         // any further channel stays as it is

        // The Engine from `done` to `end` (at most maxBlock_ samples) with the events collected so far, their offsets
        // counted from `done`. Cutting the block there changes no sample: the Engine plays every event on its own
        // sample, and its output does not depend on how the host splits its blocks. While processing (w steady at 1)
        // the output is the Engine's, untouched; otherwise the bypass fade, or the bypassed sound, follows.
        std::size_t numEvents = 0;
        int done = 0;
        const auto playTo = [&](int end) {
            const int len = end - done;
            std::array<float*, kMaxChannels> channels{};
            for (int c = 0; c < numCh; ++c)
                channels[static_cast<std::size_t>(c)] = buffer.getWritePointer(c) + done;
            const bool engineOnly = !wet_.moving() && wet_.lin.cur >= 1.0f;
            if (!engineOnly)
                keepDry(channels.data(), numCh, len);
            largestCall_ = std::max(largestCall_, len);
            engine_.process(channels.data(), numCh, len, std::span<const Event>(events_.data(), numEvents));
            if (!engineOnly)
                bypassMix(channels.data(), numCh, len);
            numEvents = 0;
            done = end;
        };

        if constexpr (product::kIsInstrument)
        {
            for (const juce::MidiMessageMetadata m : midi)
            {
                Event e;
                if (!toEvent(m, e) || (bypassed && e.kind == EventKind::noteOn))
                    continue;
                const int at = std::clamp(m.samplePosition, done, std::max(done, n - 1));
                while (at - done >= maxBlock_)                    // whole parts before the event's
                    playTo(done + maxBlock_);
                if (numEvents == events_.size())                // full: play up to this event, then carry on
                    playTo(at);
                e.offset = static_cast<uint32_t>(at - done);
                events_[numEvents++] = e;
            }
        }
        else
        {
            juce::ignoreUnused(midi, bypassed);
        }
        while (n - done > maxBlock_)
            playTo(done + maxBlock_);
        playTo(n);                                               // also a block of 0 samples: its events apply
    }

    void Processor::keepDry(float* const* channels, int numChannels, int numSamples) noexcept
    {
        if constexpr (!product::kIsInstrument)
            for (int c = 0; c < numChannels; ++c)
                fsdsp::sanitize(channels[c], dry_.data() + static_cast<std::size_t>(c * maxBlock_), numSamples);
        else
            juce::ignoreUnused(channels, numChannels, numSamples);
    }

    void Processor::bypassMix(float* const* channels, int numChannels, int numSamples) noexcept
    {
        if (!wet_.moving())                                      // bypassed (processing never gets here)
        {
            for (int c = 0; c < numChannels; ++c)
            {
                if constexpr (product::kIsInstrument)
                    std::fill(channels[c], channels[c] + numSamples, 0.0f);
                else
                    std::copy_n(dry_.data() + static_cast<std::size_t>(c * maxBlock_), numSamples, channels[c]);
            }
            return;
        }
        // The fade: one tick per sample, whatever the channels. Where it has landed (w exactly 0 or 1, also for the
        // rest of the call) the output is exactly the bypassed sound or the Engine's.
        for (int i = 0; i < numSamples; ++i)
            weight_[static_cast<std::size_t>(i)] = wet_.tick();
        for (int c = 0; c < numChannels; ++c)
        {
            float* y = channels[c];
            if constexpr (product::kIsInstrument)
            {
                for (int i = 0; i < numSamples; ++i)
                {
                    const float w = weight_[static_cast<std::size_t>(i)];
                    y[i] = w > 0.0f ? y[i] * w : 0.0f;
                }
            }
            else
            {
                const float* x = dry_.data() + static_cast<std::size_t>(c * maxBlock_);
                for (int i = 0; i < numSamples; ++i)
                {
                    const float w = weight_[static_cast<std::size_t>(i)];
                    if (w <= 0.0f)
                        y[i] = x[i];
                    else if (w < 1.0f)
                        y[i] = x[i] * (1.0f - w) + y[i] * w;     // equal-gain: the weights sum to 1
                }
            }
        }
    }

    void Processor::runUnprepared(juce::AudioBuffer<float>& buffer) noexcept
    {
        const int n = buffer.getNumSamples();
        const int numIn = product::kIsInstrument ? 0 : std::min(getTotalNumInputChannels(), buffer.getNumChannels());
        for (int ch = 0; ch < numIn; ++ch)
            fsdsp::sanitize(buffer.getReadPointer(ch), buffer.getWritePointer(ch), n);
        for (int ch = numIn; ch < buffer.getNumChannels(); ++ch)
            buffer.clear(ch, 0, n);
    }

    void Processor::hostBlock(juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi, bool bypassed) noexcept
    {
        const juce::ScopedNoDenormals noDenormals;
        if (!prepared_)
        {
            runUnprepared(buffer);
            return;
        }
        const float target = bypassed ? 0.0f : 1.0f;
        wet_.setTarget(target);
        if (!wetSet_)                                            // the first block after prepareToPlay: no fade
        {
            wet_.lin.cur = target;
            wetSet_ = true;
        }
        readParameters();
        run(buffer, midi, bypassed);
    }

    void Processor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
    {
        hostBlock(buffer, midi, false);
    }

    // Not JUCE's default (the input passed through at once): the Engine keeps running and its output fades out (see
    // Processor.h); for an instrument, on the note-offs only, so a note let go during the bypass releases and none
    // starts.
    void Processor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
    {
        hostBlock(buffer, midi, true);
    }

    bool Processor::hasEditor() const { return true; }

    juce::AudioProcessorEditor* Processor::createEditor()
    {
        return testingplaygroundsynth::createEditor(*this);
    }

    int Processor::getNumPrograms() { return 1; }
    int Processor::getCurrentProgram() { return 0; }
    void Processor::setCurrentProgram(int) {}
    const juce::String Processor::getProgramName(int) { return {}; }
    void Processor::changeProgramName(int, const juce::String&) {}

    void Processor::getStateInformation(juce::MemoryBlock& destData)
    {
        if (const std::unique_ptr<juce::XmlElement> xml = apvts_.copyState().createXml())
            copyXmlToBinary(*xml, destData);
    }

    void Processor::setStateInformation(const void* data, int sizeInBytes)
    {
        const std::unique_ptr<juce::XmlElement> xml = getXmlFromBinary(data, sizeInBytes);
        if (xml == nullptr || !xml->hasTagName(apvts_.state.getType()))
            return;
        juce::ValueTree state = juce::ValueTree::fromXml(*xml);
        const juce::Identifier kParamId{ "id" }, kParamValue{ "value" };   // the APVTS's <PARAM id value> children
        // A value that is not a finite number (a damaged or crafted state, or one saved after a host wrote a NaN)
        // loads as the parameter's default: a parameter never holds a NaN from a state.
        for (const ParamSpec& s : kParams)
        {
            juce::ValueTree p = state.getChildWithProperty(kParamId, juce::var(juce::String(s.id)));
            if (p.isValid() && p.hasProperty(kParamValue)
                && !std::isfinite(static_cast<double>(p.getProperty(kParamValue))))
                p.setProperty(kParamValue, static_cast<double>(s.def), nullptr);
        }
        apvts_.replaceState(state);
    }
}

// JUCE's entry point for every format.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new testingplaygroundsynth::Processor();
}
