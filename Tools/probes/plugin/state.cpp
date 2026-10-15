// PROBE name=plugin.state timeout=120
//
// plugin.state: the processor as a host sees it, built from the parameter table (Source/dsp/Params.h).
//   - one host parameter per table row, under the row's ID, at the row's default;
//   - the table's normalised mapping (ParamSpec.h: toNormalised / fromNormalised, what the panel draws with) equals
//     JUCE's NormalisableRange at eleven points per parameter;
//   - the state round-trips: values written, saved, overwritten, restored;
//   - so does the LFO shape (an <LFO> element beside the parameters): a shape set through EditorPort comes back bit for
//     bit, a state without one (an older session) loads the default shape, and the element never stays in the APVTS
//     tree (the processor keeps the shape itself);
//   - the bus layout is the kind's (effect: stereo in and out; instrument: stereo out, MIDI in);
//   - processBlock runs at 48 kHz in blocks of 256 and gives finite output (an instrument with a note held).
#include "Probe.h"

#include "plugin/Processor.h"

#include "Product.h"
#include "dsp/LfoShape.h"
#include "dsp/Params.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace fpt = testingplayground;

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
    fpt::Processor processor;
    juce::AudioProcessorValueTreeState& apvts = processor.parameters();

    P.eq("params.count", static_cast<int64_t>(processor.getParameters().size()),
         static_cast<int64_t>(fpt::kParams.size()));
    P.eq("ports.count", static_cast<int64_t>(processor.ports().size()), static_cast<int64_t>(fpt::kParams.size()));
    P.eq("name.is_product", processor.getName() == juce::String(fpt::product::kName) ? 1 : 0, 1);

    for (const fpt::ParamSpec& s : fpt::kParams)
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
            plainErr = std::max(plainErr, std::fabs(static_cast<double>(fpt::fromNormalised(s, t) - plain)));
            if (s.control != fpt::Control::choice)              // a choice's JUCE value snaps to whole indices
                mapErr = std::max(mapErr, std::fabs(static_cast<double>(fpt::toNormalised(s, plain) - t)));
        }
        if (s.control != fpt::Control::choice)
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

    // The LFO shape: set, saved, overwritten, restored; then a state without it.
    {
        fpt::EditorPort& port = processor.editorPort();
        fpt::LfoShape custom;
        custom.points[0] = { 0.0f, 0.3f, 1.5f };
        custom.points[1] = { 0.125f, 0.95f, -4.25f };
        custom.points[2] = { 0.5f, 0.05f, 0.0f };
        custom.points[3] = { 0.875f, 0.6f, 7.0f };
        custom.points[4] = { 1.0f, 0.3f, 0.0f };
        custom.count = 5;
        port.setShape(custom);
        P.eq("shape.set.kept", port.shape() == custom ? 1 : 0, 1);
        juce::MemoryBlock withShape;
        processor.getStateInformation(withShape);
        port.setShape(fpt::defaultShape());
        const uint64_t before = port.shapeRevision();
        processor.setStateInformation(withShape.getData(), static_cast<int>(withShape.getSize()));
        P.eq("shape.roundtrip_same_points", port.shape() == custom ? 1 : 0, 1);
        P.eq("shape.restore_bumps_revision", port.shapeRevision() != before ? 1 : 0, 1);
        P.eq("shape.not_left_in_apvts", apvts.state.getChildWithName("LFO").isValid() ? 0 : 1, 1);

        const std::unique_ptr<juce::XmlElement> xml = apvts.copyState().createXml();   // parameters only
        juce::MemoryBlock old;
        juce::AudioProcessor::copyXmlToBinary(*xml, old);
        processor.setStateInformation(old.getData(), static_cast<int>(old.getSize()));
        P.eq("shape.state_without_lfo_loads_default", port.shape() == fpt::defaultShape() ? 1 : 0, 1);
    }

    // Buses.
    juce::AudioProcessor::BusesLayout stereo;
    stereo.outputBuses.add(juce::AudioChannelSet::stereo());
    if constexpr (!fpt::product::kIsInstrument)
        stereo.inputBuses.add(juce::AudioChannelSet::stereo());
    P.eq("buses.stereo_supported", processor.checkBusesLayoutSupported(stereo) ? 1 : 0, 1);
    P.eq("midi.accepts", processor.acceptsMidi() ? 1 : 0, fpt::product::kIsInstrument ? 1 : 0);

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
        if constexpr (fpt::product::kIsInstrument)
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
