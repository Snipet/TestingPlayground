// PROBE name=plugin.state timeout=120
//
// plugin.state: what a session keeps, and what a run does to the dials.
//   ROUND   a processor running the second example, its first dial moved and its draft edited, saved
//           (getStateInformation) and loaded into a fresh processor (setStateInformation): the new one plays the same
//           program (runningSource), shows the same draft, has the same dials with the same names and values, and
//           its output is the first's, bit for bit
//   RENAME  RUN with a program whose first parameter has another name moves that dial to the new parameter's
//           default; one whose first parameter keeps its name keeps the dial where it was
//   BROKEN  a session whose program does not compile (here: a syntax error) leaves the program that played playing,
//           its errors in the status, and the draft the session's
//   EMPTY   a session saved without a program (an older version's) plays the first example
//   NAN     a dial saved as NaN loads as 0.5
#include "Probe.h"

#include "Audio.h"
#include "Rig.h"

#include "plugin/EditorPort.h"
#include "plugin/Processor.h"

#include "Examples.h"
#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <cmath>
#include <limits>
#include <string>

namespace tp = testingplayground;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr bool kInstrument = tp::product::kIsInstrument;

    // Half a second of the processor's output: an effect over noise, an instrument playing middle C.
    probe::Stereo play(tp::Processor& p)
    {
        p.setRateAndBufferSizeDetails(kFs, 256);
        p.prepareToPlay(kFs, 256);
        const std::size_t n = static_cast<std::size_t>(0.5 * kFs);
        probe::Stereo in = kInstrument ? probe::Stereo(n) : probe::noise(n, 0.25);
        for (std::size_t pos = 0; pos < n; pos += 256)
        {
            juce::AudioBuffer<float> buffer(2, 256);
            for (int i = 0; i < 256; ++i)
            {
                buffer.setSample(0, i, in.l[pos + static_cast<std::size_t>(i)]);
                buffer.setSample(1, i, in.r[pos + static_cast<std::size_t>(i)]);
            }
            juce::MidiBuffer midi;
            if (kInstrument && pos == 0)
                midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<uint8_t>(100)), 0);
            p.processBlock(buffer, midi);
            for (int i = 0; i < 256; ++i)
            {
                in.l[pos + static_cast<std::size_t>(i)] = buffer.getSample(0, i);
                in.r[pos + static_cast<std::size_t>(i)] = buffer.getSample(1, i);
            }
        }
        return in;
    }

    void runAndSettle(tp::Processor& p, const std::string& source)
    {
        p.editorPort().run(source);
        p.settle();
    }

    std::string withFirstParam(const std::string& name, double def)
    {
        const std::string param = "    param float " + name + " [[fminus::range(0, 10)]] [[fminus::default(" +
                                  std::to_string(def) + ")]];\n";
        if constexpr (kInstrument)
            return "module Voice\n{\n" + param +
                   "    void start(float n, float v) {}\n"
                   "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n";
        else
            return "module Effect\n{\n" + param + "    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
    }
}

PROBE(plugin, state)
{
    const funkgui::HeadlessGuiScope gui;
    P.ge("examples", static_cast<double>(std::size(tp::kExamples)), 2.0);

    // ROUND
    {
        tp::Processor a;
        runAndSettle(a, tp::kExamples[1].source);
        P.eq("round.runs_second", a.editorPort().runningSource() == tp::kExamples[1].source ? 1 : 0, 1);
        a.getParameters()[0]->setValueNotifyingHost(0.8f);
        a.editorPort().setDraft("// an edit not run yet\n");
        juce::MemoryBlock saved;
        a.getStateInformation(saved);

        tp::Processor b;
        b.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        b.poll();
        P.eq("round.same_program", b.editorPort().runningSource() == tp::kExamples[1].source ? 1 : 0, 1);
        P.eq("round.same_draft", b.editorPort().draft() == "// an edit not run yet\n" ? 1 : 0, 1);
        P.eq("round.same_dial_name", std::string(b.control(0).name) == a.control(0).name ? 1 : 0, 1);
        P.near("round.same_dial_value", static_cast<double>(b.getParameters()[0]->getValue()), 0.8, 1e-6);
        const probe::Stereo oa = play(a), ob = play(b);
        P.eq("round.same_output_bits", probe::sameBits(oa.l, ob.l) && probe::sameBits(oa.r, ob.r) ? 1 : 0, 1);
    }

    // RENAME
    {
        tp::Processor p;
        runAndSettle(p, withFirstParam("alpha", 2.0));
        P.near("rename.default", static_cast<double>(p.getParameters()[0]->getValue()), 0.2, 1e-6);
        p.getParameters()[0]->setValueNotifyingHost(0.7f);
        runAndSettle(p, withFirstParam("alpha", 5.0));          // the same name: the dial stays
        P.near("rename.same_name_keeps", static_cast<double>(p.getParameters()[0]->getValue()), 0.7, 1e-6);
        runAndSettle(p, withFirstParam("beta", 3.0));           // another name: its default
        P.near("rename.new_name_default", static_cast<double>(p.getParameters()[0]->getValue()), 0.3, 1e-6);
        P.eq("rename.host_name", p.getParameters()[0]->getName(100) == "1 beta" ? 1 : 0, 1);
    }

    // BROKEN
    {
        tp::Processor a;
        juce::MemoryBlock saved;
        a.editorPort().setDraft("draft of a broken one");
        a.getStateInformation(saved);
        // The saved state with its program's source replaced by one that does not compile.
        std::unique_ptr<juce::XmlElement> xml = juce::AudioProcessor::getXmlFromBinary(saved.getData(),
                                                                                      static_cast<int>(saved.getSize()));
        P.eq("broken.state_parses", xml != nullptr ? 1 : 0, 1);
        if (xml != nullptr)
        {
            if (juce::XmlElement* program = xml->getChildByName("PROGRAM"))
                program->setAttribute("source", "module {");
            juce::MemoryBlock broken;
            juce::AudioProcessor::copyXmlToBinary(*xml, broken);
            tp::Processor b;
            runAndSettle(b, tp::kExamples[1].source);
            b.setStateInformation(broken.getData(), static_cast<int>(broken.getSize()));
            b.poll();
            const tp::CompileStatus s = b.editorPort().status();
            P.eq("broken.not_ok", s.ok ? 0 : 1, 1);
            P.eq("broken.errors", s.errors.empty() ? 0 : 1, 1);
            P.eq("broken.previous_plays", b.editorPort().runningSource() == tp::kExamples[1].source ? 1 : 0, 1);
            P.eq("broken.draft", b.editorPort().draft() == "draft of a broken one" ? 1 : 0, 1);
        }
    }

    // EMPTY and NAN
    {
        juce::ValueTree state{ "PARAMETERS" };
        juce::ValueTree p1{ "PARAM" };
        p1.setProperty("id", "p1", nullptr);
        p1.setProperty("value", std::numeric_limits<double>::quiet_NaN(), nullptr);
        state.appendChild(p1, nullptr);
        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary(*state.createXml(), block);
        tp::Processor p;
        runAndSettle(p, tp::kExamples[1].source);
        p.setStateInformation(block.getData(), static_cast<int>(block.getSize()));
        p.poll();
        P.eq("empty.first_example", p.editorPort().runningSource() == tp::kExamples[0].source ? 1 : 0, 1);
        P.near("nan.half", static_cast<double>(p.getParameters()[0]->getValue()), 0.5, 1e-6);
    }
    return P.finish();
}
