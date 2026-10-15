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
//   - a session saved by the Visage version (tag legacy-visage: root <PARAMS>, the same plugin identity) restores
//     GAIN, its one parameter;
//   - a NaN written by a host: the host's text for every parameter but a choice is a dash, never "nan";
//   - a state whose value is not a number (size="nan", gain="inf") loads those parameters at their defaults, the
//     others as saved; and so for every parameter but the last, the last kept;
//   - the bus layout is the kind's (effect: stereo in and out; instrument: stereo out, MIDI in);
//   - processBlock and processBlockBypassed before any prepareToPlay (JUCE's contract forbids it; a host that breaks
//     it must not crash the plugin): an effect passes its input through sanitised (the same bits, a NaN as 0), an
//     instrument is silent, with a note-on in the block; the Engine is not called;
//   - processBlock runs at 48 kHz in blocks of 256 and gives finite output (an instrument with a note held);
//   - the tail hosts are told (getTailLengthSeconds) is 1.5 DECAY after the longest line (Engine.cpp: -90 dB), at
//     the defaults (3.033 s) and after DECAY 20 s, SIZE 100 m and 4 lines, within 1 us.
#include "Probe.h"

#include "plugin/Processor.h"

#include "Product.h"
#include "dsp/FdnLayout.h"
#include "dsp/LfoShape.h"
#include "dsp/Params.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace tp = testingplayground;

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
    tp::Processor processor;
    juce::AudioProcessorValueTreeState& apvts = processor.parameters();

    P.eq("params.count", static_cast<int64_t>(processor.getParameters().size()),
         static_cast<int64_t>(tp::kParams.size()));
    P.eq("ports.count", static_cast<int64_t>(processor.ports().size()), static_cast<int64_t>(tp::kParams.size()));
    P.eq("name.is_product", processor.getName() == juce::String(tp::product::kName) ? 1 : 0, 1);

    for (const tp::ParamSpec& s : tp::kParams)
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
            plainErr = std::max(plainErr, std::fabs(static_cast<double>(tp::fromNormalised(s, t) - plain)));
            if (s.control != tp::Control::choice)               // a choice's JUCE value snaps to whole indices
                mapErr = std::max(mapErr, std::fabs(static_cast<double>(tp::toNormalised(s, plain) - t)));
        }
        if (s.control != tp::Control::choice)
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
        tp::EditorPort& port = processor.editorPort();
        tp::LfoShape custom;
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
        port.setShape(tp::defaultShape());
        const uint64_t before = port.shapeRevision();
        processor.setStateInformation(withShape.getData(), static_cast<int>(withShape.getSize()));
        P.eq("shape.roundtrip_same_points", port.shape() == custom ? 1 : 0, 1);
        P.eq("shape.restore_bumps_revision", port.shapeRevision() != before ? 1 : 0, 1);
        P.eq("shape.not_left_in_apvts", apvts.state.getChildWithName("LFO").isValid() ? 0 : 1, 1);

        const std::unique_ptr<juce::XmlElement> xml = apvts.copyState().createXml();   // parameters only
        juce::MemoryBlock old;
        juce::AudioProcessor::copyXmlToBinary(*xml, old);
        processor.setStateInformation(old.getData(), static_cast<int>(old.getSize()));
        P.eq("shape.state_without_lfo_loads_default", port.shape() == tp::defaultShape() ? 1 : 0, 1);
    }

    // A host's NaN: the host's text is a dash (a choice's text is its entry's: not checked).
    {
        int dashes = 0, floats = 0;
        for (const tp::ParamSpec& s : tp::kParams)
        {
            if (s.control == tp::Control::choice)
                continue;
            juce::RangedAudioParameter* p = apvts.getParameter(s.id);
            p->setValueNotifyingHost(std::numeric_limits<float>::quiet_NaN());
            dashes += p->getCurrentValueAsText() == juce::String::fromUTF8("\xE2\x80\x93") ? 1 : 0;
            ++floats;
            p->setValueNotifyingHost(p->getDefaultValue());
        }
        P.eq("nan.host_text_is_dash", dashes, floats);
    }

    // A session of the Visage version: <PARAMS><PARAM id="gain" value="-7.5"/></PARAMS>.
    {
        for (juce::AudioProcessorParameter* p : processor.getParameters())
            p->setValueNotifyingHost(p->getDefaultValue());
        juce::XmlElement legacy("PARAMS");
        juce::XmlElement* gain = legacy.createNewChildElement("PARAM");
        gain->setAttribute("id", "gain");
        gain->setAttribute("value", -7.5);
        juce::MemoryBlock old;
        juce::AudioProcessor::copyXmlToBinary(legacy, old);
        processor.setStateInformation(old.getData(), static_cast<int>(old.getSize()));
        juce::RangedAudioParameter* g = apvts.getParameter("gain");
        P.near("legacy.gain_restored_db", g->convertFrom0to1(g->getValue()), -7.5, 1e-4);
        P.eq("legacy.root_kept_as_apvts", apvts.state.getType() == juce::Identifier("PARAMETERS") ? 1 : 0, 1);
        P.eq("legacy.shape_default", processor.editorPort().shape() == tp::defaultShape() ? 1 : 0, 1);
    }

    // A state with values that are not numbers.
    {
        juce::ValueTree state = apvts.copyState();
        state.getChildWithProperty("id", "size").setProperty("value", "nan", nullptr);
        state.getChildWithProperty("id", "gain").setProperty("value", "inf", nullptr);
        state.getChildWithProperty("id", "decay").setProperty("value", 5000.0, nullptr);
        juce::MemoryBlock bad;
        juce::AudioProcessor::copyXmlToBinary(*state.createXml(), bad);
        processor.getParameters()[static_cast<int>(tp::param::size)]->setValueNotifyingHost(0.9f);
        processor.setStateInformation(bad.getData(), static_cast<int>(bad.getSize()));
        const auto plain = [&](const char* id) {
            juce::RangedAudioParameter* p = apvts.getParameter(id);
            return static_cast<double>(p->convertFrom0to1(p->getValue()));
        };
        P.near("nan_state.size_default", plain("size"), 12.0, 1e-3);
        P.near("nan_state.gain_default", plain("gain"), 0.0, 1e-4);
        P.near("nan_state.other_values_kept", plain("decay"), 5000.0, 0.01);
        bool finite = true;
        for (juce::AudioProcessorParameter* p : processor.getParameters())
            finite = finite && std::isfinite(p->getValue());
        P.eq("nan_state.all_finite", finite ? 1 : 0, 1);
    }

    // And for every parameter: all but the last "nan" or "inf", the last 0.3 (a choice: its last entry), saved over
    // values of 0.6.
    {
        for (juce::AudioProcessorParameter* p : processor.getParameters())
            p->setValueNotifyingHost(p->getNumSteps() < 100 ? 1.0f : 0.3f);
        juce::AudioProcessorParameter* last = processor.getParameters().getLast();
        const float lastWritten = last->getValue();
        juce::ValueTree state = apvts.copyState();
        for (std::size_t i = 0; i + 1 < tp::kParams.size(); ++i)
            state.getChildWithProperty("id", juce::String(tp::kParams[i].id))
                .setProperty("value", i % 2 == 0 ? "nan" : "inf", nullptr);
        juce::MemoryBlock bad;
        juce::AudioProcessor::copyXmlToBinary(*state.createXml(), bad);
        for (juce::AudioProcessorParameter* p : processor.getParameters())
            p->setValueNotifyingHost(p->getNumSteps() < 100 ? 0.0f : 0.6f);
        processor.setStateInformation(bad.getData(), static_cast<int>(bad.getSize()));
        double defaultErr = 0.0;
        bool finite = true;
        for (std::size_t i = 0; i < tp::kParams.size(); ++i)
        {
            const juce::AudioProcessorParameter* p = processor.getParameters()[static_cast<int>(i)];
            finite = finite && std::isfinite(p->getValue());
            if (i + 1 < tp::kParams.size())
                defaultErr = std::max(defaultErr, std::fabs(static_cast<double>(p->getValue() - p->getDefaultValue())));
        }
        P.eq("nan_state.every.all_finite", finite ? 1 : 0, 1);
        P.le("nan_state.non_numbers_load_defaults_err", defaultErr, 1e-6);
        if (tp::kParams.size() > 1)
            P.near("nan_state.number_kept", last->getValue(), lastWritten, 1e-6);
        for (juce::AudioProcessorParameter* p : processor.getParameters())
            p->setValueNotifyingHost(p->getDefaultValue());
    }

    // processBlock and processBlockBypassed before any prepareToPlay, on a fresh processor: a sine in (an effect) with
    // a NaN at one sample of the right channel; a note-on at the start (an instrument).
    {
        tp::Processor fresh;
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
                    const bool zero = tp::product::kIsInstrument || (ch == 1 && i == 100);
                    const float want = zero ? 0.0f : in.getSample(ch, i);
                    same = same && std::memcmp(&got, &want, sizeof got) == 0;
                }
            return same ? 1 : 0;
        };
        const std::string what = tp::product::kIsInstrument ? "silent" : "passes_input_sanitised";
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
    if constexpr (!tp::product::kIsInstrument)
        stereo.inputBuses.add(juce::AudioChannelSet::stereo());
    P.eq("buses.stereo_supported", processor.checkBusesLayoutSupported(stereo) ? 1 : 0, 1);
    P.eq("midi.accepts", processor.acceptsMidi() ? 1 : 0, tp::product::kIsInstrument ? 1 : 0);

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
        if constexpr (tp::product::kIsInstrument)
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

    // The tail: at the defaults, then after DECAY 20 s, SIZE 100 m and 4 lines (a LINES change switches 20 ms later:
    // ten more blocks).
    const auto tailWant = [](double decayMs, float size, int lines) {
        const tp::FdnLayout l = tp::makeLayout(48000.0f, size, lines);
        return 1.5 * decayMs * 0.001 + static_cast<double>(l.delay[static_cast<std::size_t>(lines - 1)]) / 48000.0;
    };
    P.near("tail.defaults_s", processor.getTailLengthSeconds(), tailWant(2000.0, 12.0f, 16), 1e-6);
    {
        const auto setPlain = [&](std::size_t i, float plain) {
            processor.getParameters()[static_cast<int>(i)]->setValueNotifyingHost(
                tp::toNormalised(tp::kParams[i], plain));
        };
        setPlain(tp::param::decay, 20000.0f);
        setPlain(tp::param::size, 100.0f);
        setPlain(tp::param::lines, 0.0f);
        juce::MidiBuffer midi;
        for (int block = 0; block < 10; ++block)
        {
            buffer.clear();
            processor.processBlock(buffer, midi);
        }
        P.near("tail.decay20s_size100m_4lines_s", processor.getTailLengthSeconds(), tailWant(20000.0, 100.0f, 4),
               1e-6);
    }
    processor.releaseResources();
    return P.finish();
}
