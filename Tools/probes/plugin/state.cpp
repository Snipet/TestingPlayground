// PROBE name=plugin.state timeout=120
//
// plugin.state: the processor as a host sees it, built from the parameter table (Source/dsp/Params.h).
//   - one host parameter per table row, under the row's ID, at the row's default;
//   - the table's normalised mapping (ParamSpec.h: toNormalised / fromNormalised, what the panel draws with) equals
//     JUCE's NormalisableRange at eleven points per parameter;
//   - the state round-trips: values written, saved, overwritten, restored;
//   - a NaN written by a host: the host's text for the parameter is a dash, never "nan";
//   - a state whose values are not numbers ("nan", "inf") loads those parameters at their defaults, the others as
//     saved;
//   - processBlock and processBlockBypassed before any prepareToPlay (JUCE's contract forbids it; a host that breaks
//     it must not crash the plugin): an effect passes its input through sanitised (the same bits, a NaN as 0), an
//     instrument is silent, with a note-on in the block; the Engine is not called;
//   - the bus layout is the kind's (effect: stereo in and out; instrument: stereo out, MIDI in);
//   - processBlock runs at 48 kHz in blocks of 256 and gives finite output (an instrument with a note held).
#include "Probe.h"

#include "plugin/Processor.h"

#include "Product.h"
#include "dsp/Params.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace tps = testingplaygroundsynth;

namespace
{
    // A row key from a parameter ID: lower case, [a-z0-9_] only.
    std::string keyOf(const char* id)
    {
        std::string k;
        for (const char* c = id; *c != '\0'; ++c)
        {
            const unsigned char u = static_cast<unsigned char>(*c);
            k += std::isalnum(u) ? static_cast<char>(std::tolower(u)) : '_';
        }
        return k.empty() ? std::string("param") : "param." + k;
    }
}

PROBE(plugin, state)
{
    const funkgui::HeadlessGuiScope gui;                        // JUCE's message manager: the APVTS needs one
    tps::Processor processor;
    juce::AudioProcessorValueTreeState& apvts = processor.parameters();

    P.eq("params.count", static_cast<int64_t>(processor.getParameters().size()),
         static_cast<int64_t>(tps::kParams.size()));
    P.eq("ports.count", static_cast<int64_t>(processor.ports().size()), static_cast<int64_t>(tps::kParams.size()));
    P.eq("name.is_product", processor.getName() == juce::String(tps::product::kName) ? 1 : 0, 1);

    for (const tps::ParamSpec& s : tps::kParams)
    {
        const std::string key = keyOf(s.id);
        juce::RangedAudioParameter* p = apvts.getParameter(s.id);
        P.eq(key + ".exists", p != nullptr ? 1 : 0, 1);
        if (p == nullptr)
            continue;
        const double span = static_cast<double>(s.max - s.min);
        P.near(key + ".default", p->convertFrom0to1(p->getDefaultValue()), s.def, 1e-5 * span);
        double mapErr = 0.0, plainErr = 0.0;
        for (int k = 0; k <= 10; ++k)
        {
            const float t = static_cast<float>(k) / 10.0f;
            const float plain = p->convertFrom0to1(t);
            plainErr = std::max(plainErr, std::fabs(static_cast<double>(tps::fromNormalised(s, t) - plain)));
            if (s.control != tps::Control::choice)              // a choice's JUCE value snaps to whole indices
                mapErr = std::max(mapErr, std::fabs(static_cast<double>(tps::toNormalised(s, plain) - t)));
        }
        if (s.control != tps::Control::choice)
        {
            P.le(key + ".map_to_normalised_err", mapErr, 1e-5);
            P.le(key + ".map_to_plain_err", plainErr, 1e-5 * span);
        }
        P.eq(key + ".host_text", p->getCurrentValueAsText().isNotEmpty() ? 1 : 0, 1);
    }

    // State: write 0.3 (a choice: its last entry), save, write the defaults, restore, compare.
    for (juce::AudioProcessorParameter* p : processor.getParameters())
        p->setValueNotifyingHost(p->getNumSteps() < 100 ? 1.0f : 0.3f);
    std::vector<float> written;
    for (juce::AudioProcessorParameter* p : processor.getParameters())
        written.push_back(p->getValue());
    juce::MemoryBlock saved;
    processor.getStateInformation(saved);
    for (juce::AudioProcessorParameter* p : processor.getParameters())
        p->setValueNotifyingHost(p->getDefaultValue());
    processor.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    double stateErr = 0.0;
    for (std::size_t i = 0; i < written.size(); ++i)
        stateErr = std::max(stateErr, std::fabs(static_cast<double>(processor.getParameters()[static_cast<int>(i)]
                                                                         ->getValue() - written[i])));
    P.le("state.roundtrip_err", stateErr, 1e-6);
    P.ge("state.bytes", static_cast<double>(saved.getSize()), 16.0);

    // A host's NaN: the host's text is a dash (a choice's text is its entry's: not checked).
    {
        int dashes = 0, floats = 0;
        for (const tps::ParamSpec& s : tps::kParams)
        {
            if (s.control == tps::Control::choice)
                continue;
            juce::RangedAudioParameter* p = apvts.getParameter(s.id);
            p->setValueNotifyingHost(std::numeric_limits<float>::quiet_NaN());
            dashes += p->getCurrentValueAsText() == juce::String::fromUTF8("\xE2\x80\x93") ? 1 : 0;
            ++floats;
            p->setValueNotifyingHost(p->getDefaultValue());
        }
        P.eq("nan.host_text_is_dash", dashes, floats);
    }

    // A state with values that are not numbers: every parameter but the last "nan" or "inf", the last 0.3 (a choice:
    // its last entry), saved over values of 0.6.
    {
        for (juce::AudioProcessorParameter* p : processor.getParameters())
            p->setValueNotifyingHost(p->getNumSteps() < 100 ? 1.0f : 0.3f);
        juce::AudioProcessorParameter* last = processor.getParameters().getLast();
        const float lastWritten = last->getValue();
        juce::ValueTree state = apvts.copyState();
        for (std::size_t i = 0; i + 1 < tps::kParams.size(); ++i)
            state.getChildWithProperty("id", juce::String(tps::kParams[i].id))
                .setProperty("value", i % 2 == 0 ? "nan" : "inf", nullptr);
        juce::MemoryBlock bad;
        juce::AudioProcessor::copyXmlToBinary(*state.createXml(), bad);
        for (juce::AudioProcessorParameter* p : processor.getParameters())
            p->setValueNotifyingHost(p->getNumSteps() < 100 ? 0.0f : 0.6f);
        processor.setStateInformation(bad.getData(), static_cast<int>(bad.getSize()));
        double defaultErr = 0.0;
        bool finite = true;
        for (std::size_t i = 0; i < tps::kParams.size(); ++i)
        {
            const juce::AudioProcessorParameter* p = processor.getParameters()[static_cast<int>(i)];
            finite = finite && std::isfinite(p->getValue());
            if (i + 1 < tps::kParams.size())
                defaultErr = std::max(defaultErr, std::fabs(static_cast<double>(p->getValue() - p->getDefaultValue())));
        }
        P.eq("nan_state.all_finite", finite ? 1 : 0, 1);
        P.le("nan_state.non_numbers_load_defaults_err", defaultErr, 1e-6);
        if (tps::kParams.size() > 1)
            P.near("nan_state.number_kept", last->getValue(), lastWritten, 1e-6);
        for (juce::AudioProcessorParameter* p : processor.getParameters())
            p->setValueNotifyingHost(p->getDefaultValue());
    }

    // processBlock and processBlockBypassed before any prepareToPlay, on a fresh processor: a sine in (an effect) with
    // a NaN at one sample of the right channel; a note-on at the start (an instrument).
    {
        tps::Processor fresh;
        juce::AudioBuffer<float> buffer(2, 256);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 256; ++i)
                buffer.setSample(ch, i, 0.25f * std::sin(0.05f * static_cast<float>(i + 7 * ch)));
        buffer.setSample(1, 100, std::numeric_limits<float>::quiet_NaN());
        const juce::AudioBuffer<float> in(buffer);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 69, 0.8f), 0);
        const auto expected = [&] {
            bool same = true;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 256; ++i)
                {
                    const float got = buffer.getSample(ch, i);
                    const bool zero = tps::product::kIsInstrument || (ch == 1 && i == 100);
                    const float want = zero ? 0.0f : in.getSample(ch, i);
                    same = same && std::memcmp(&got, &want, sizeof got) == 0;
                }
            return same ? 1 : 0;
        };
        const std::string what = tps::product::kIsInstrument ? "silent" : "passes_input_sanitised";
        fresh.processBlock(buffer, midi);
        P.eq("unprepared." + what, expected(), 1);
        buffer.makeCopyOf(in);
        fresh.processBlockBypassed(buffer, midi);
        P.eq("unprepared.bypassed." + what, expected(), 1);
        P.eq("unprepared.engine_not_called", fresh.largestEngineCall(), 0);
    }

    // Buses.
    juce::AudioProcessor::BusesLayout stereo;
    stereo.outputBuses.add(juce::AudioChannelSet::stereo());
    if constexpr (!tps::product::kIsInstrument)
        stereo.inputBuses.add(juce::AudioChannelSet::stereo());
    P.eq("buses.stereo_supported", processor.checkBusesLayoutSupported(stereo) ? 1 : 0, 1);
    P.eq("midi.accepts", processor.acceptsMidi() ? 1 : 0, tps::product::kIsInstrument ? 1 : 0);

    // Process, at the defaults: 1 s of a 440 Hz sine in (an effect), a held A4 (an instrument).
    for (juce::AudioProcessorParameter* p : processor.getParameters())
        p->setValueNotifyingHost(p->getDefaultValue());
    processor.setRateAndBufferSizeDetails(48000.0, 256);
    processor.prepareToPlay(48000.0, 256);
    juce::AudioBuffer<float> buffer(2, 256);
    bool finite = true;
    float peak = 0.0f;
    for (int block = 0; block < 188; ++block)
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 256; ++i)
                buffer.setSample(ch, i, 0.25f * std::sin(2.0f * 3.14159265f * 440.0f
                                                        * static_cast<float>(block * 256 + i) / 48000.0f));
        juce::MidiBuffer midi;
        if (block == 0)
            midi.addEvent(juce::MidiMessage::noteOn(1, 69, 0.8f), 0);
        if constexpr (tps::product::kIsInstrument)
            buffer.clear();
        processor.processBlock(buffer, midi);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 256; ++i)
            {
                const float v = buffer.getSample(ch, i);
                finite = finite && std::isfinite(v);
                peak = std::max(peak, std::fabs(v));
            }
    }
    P.eq("process.finite", finite ? 1 : 0, 1);
    P.ge("process.peak", static_cast<double>(peak), 1e-3);
    processor.releaseResources();
    return P.finish();
}
