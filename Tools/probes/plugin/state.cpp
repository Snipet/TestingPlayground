// PROBE name=plugin.state timeout=120
//
// plugin.state: what a session keeps, and what a run does to the dials.
//   ROUND   a processor running a program of two files (the second example, which includes a second file), its
//           first dial moved and its draft edited (another file added, the second tab shown), saved
//           (getStateInformation) and loaded into a fresh processor (setStateInformation): the new one plays the same
//           files (running), has the same draft (files, names, texts and tab), the same dials with the same names and
//           values, and its output is the first's, bit for bit
//   OLD     a session of the version before files (<PROGRAM source draft>) loads as one file each: the program plays,
//           and the draft is the one it saved
//   TIDY    a session with eleven files, names that are not names, and one name twice loads at most kMaxFiles files,
//           each named validly, no name twice, the program's first
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
#include <cstdio>
#include <limits>
#include <memory>
#include <string>

namespace pg = playground;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr bool kInstrument = pg::product::kIsInstrument;

    // Half a second of the processor's output: an effect over noise, an instrument playing middle C.
    probe::Stereo play(pg::Processor& p)
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

    void runAndSettle(pg::Processor& p, const pg::Sources& files)
    {
        p.editorPort().run(files);
        p.settle();
    }

    void runAndSettle(pg::Processor& p, const std::string& source)
    {
        runAndSettle(p, pg::singleFile(source));
    }

    // The second example, including a file of its own: lib.fm, a function nothing calls.
    pg::Sources twoFiles()
    {
        return { { "program.fm", "#include \"lib.fm\"\n" + std::string(pg::kExamples[1].source) },
                 { "lib.fm", "float twice(float x) { return x * 2.0f; }\n" } };
    }

    // A state's XML with its <PROGRAM> replaced by `program`.
    juce::MemoryBlock withProgram(const juce::MemoryBlock& saved, std::unique_ptr<juce::XmlElement> program)
    {
        std::unique_ptr<juce::XmlElement> xml = juce::AudioProcessor::getXmlFromBinary(saved.getData(),
                                                                                      static_cast<int>(saved.getSize()));
        juce::MemoryBlock out;
        if (xml == nullptr)
            return out;
        if (juce::XmlElement* old = xml->getChildByName("PROGRAM"))
            xml->removeChildElement(old, true);
        xml->addChildElement(program.release());
        juce::AudioProcessor::copyXmlToBinary(*xml, out);
        return out;
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
    P.ge("examples", static_cast<double>(std::size(pg::kExamples)), 2.0);

    // ROUND
    {
        pg::Processor a;
        runAndSettle(a, twoFiles());
        P.eq("round.runs_two_files", a.editorPort().running() == twoFiles() ? 1 : 0, 1);
        a.getParameters()[0]->setValueNotifyingHost(0.8f);
        pg::Draft draft;
        draft.files = twoFiles();
        draft.files[0].text += "// an edit not run yet\n";
        draft.files.push_back({ "notes.fm", "// a third file\n" });
        draft.active = 1;
        a.editorPort().setDraft(draft);
        juce::MemoryBlock saved;
        a.getStateInformation(saved);

        pg::Processor b;
        b.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        b.poll();
        P.eq("round.same_program", b.editorPort().running() == twoFiles() ? 1 : 0, 1);
        P.eq("round.same_draft", b.editorPort().draft() == draft ? 1 : 0, 1);
        P.eq("round.draft_files", static_cast<int64_t>(b.editorPort().draft().files.size()), 3);
        P.eq("round.draft_tab", b.editorPort().draft().active, 1);
        P.eq("round.same_dial_name", std::string(b.control(0).name) == a.control(0).name ? 1 : 0, 1);
        P.near("round.same_dial_value", static_cast<double>(b.getParameters()[0]->getValue()), 0.8, 1e-6);
        const probe::Stereo oa = play(a), ob = play(b);
        P.eq("round.same_output_bits", probe::sameBits(oa.l, ob.l) && probe::sameBits(oa.r, ob.r) ? 1 : 0, 1);
    }

    // RENAME
    {
        pg::Processor p;
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
        pg::Processor a;
        juce::MemoryBlock saved;
        a.editorPort().setDraft({ pg::singleFile("draft of a broken one"), 0 });
        a.getStateInformation(saved);
        // The saved state with its program's text replaced by one that does not compile.
        std::unique_ptr<juce::XmlElement> xml = juce::AudioProcessor::getXmlFromBinary(saved.getData(),
                                                                                      static_cast<int>(saved.getSize()));
        P.eq("broken.state_parses", xml != nullptr ? 1 : 0, 1);
        if (xml != nullptr)
        {
            juce::XmlElement* file = nullptr;
            if (juce::XmlElement* program = xml->getChildByName("PROGRAM"))
                if (juce::XmlElement* run = program->getChildByName("RUN"))
                    file = run->getChildByName("FILE");
            P.eq("broken.state_has_file", file != nullptr ? 1 : 0, 1);
            if (file != nullptr)
                file->setAttribute("text", "module {");
            juce::MemoryBlock broken;
            juce::AudioProcessor::copyXmlToBinary(*xml, broken);
            pg::Processor b;
            runAndSettle(b, pg::kExamples[1].source);
            b.setStateInformation(broken.getData(), static_cast<int>(broken.getSize()));
            b.poll();
            const pg::CompileStatus s = b.editorPort().status();
            P.eq("broken.not_ok", s.ok ? 0 : 1, 1);
            P.eq("broken.errors", s.errors.empty() ? 0 : 1, 1);
            P.eq("broken.previous_plays", b.editorPort().running() == pg::singleFile(pg::kExamples[1].source) ? 1 : 0, 1);
            P.eq("broken.draft", b.editorPort().draft().files == pg::singleFile("draft of a broken one") ? 1 : 0, 1);
        }
    }

    // OLD
    {
        pg::Processor a;
        juce::MemoryBlock saved;
        a.getStateInformation(saved);
        auto program = std::make_unique<juce::XmlElement>("PROGRAM");
        program->setAttribute("source", juce::String::fromUTF8(pg::kExamples[1].source));
        program->setAttribute("draft", "// the old draft\n");
        const juce::MemoryBlock old = withProgram(saved, std::move(program));
        pg::Processor b;
        b.setStateInformation(old.getData(), static_cast<int>(old.getSize()));
        b.poll();
        P.eq("old.plays_its_program", b.editorPort().running() == pg::singleFile(pg::kExamples[1].source) ? 1 : 0, 1);
        P.eq("old.its_draft", b.editorPort().draft().files == pg::singleFile("// the old draft\n") ? 1 : 0, 1);
    }

    // TIDY
    {
        pg::Processor a;
        juce::MemoryBlock saved;
        a.getStateInformation(saved);
        auto program = std::make_unique<juce::XmlElement>("PROGRAM");
        program->setAttribute("active", 99);
        auto* run = program->createNewChildElement("RUN");
        auto* file = run->createNewChildElement("FILE");
        file->setAttribute("name", "program.fm");
        file->setAttribute("text", juce::String::fromUTF8(pg::kExamples[0].source));
        auto* draft = program->createNewChildElement("DRAFT");
        const char* names[] = { "../program", "a.fm", "a.fm", "", ".hidden", "b c", "c.fm", "d.fm", "e.fm", "f.fm",
                                "g.fm" };
        for (const char* n : names)
        {
            auto* f = draft->createNewChildElement("FILE");
            f->setAttribute("name", n);
            f->setAttribute("text", "// text\n");
        }
        const juce::MemoryBlock crafted = withProgram(saved, std::move(program));
        pg::Processor b;
        b.setStateInformation(crafted.getData(), static_cast<int>(crafted.getSize()));
        b.poll();
        const pg::Draft d = b.editorPort().draft();
        P.le("tidy.at_most_max", static_cast<double>(d.files.size()), static_cast<double>(pg::kMaxFiles));
        int valid = 0, unique = 0;
        for (std::size_t i = 0; i < d.files.size(); ++i)
        {
            valid += pg::validFileName(d.files[i].name) ? 1 : 0;
            int same = 0;
            for (const pg::SourceFile& g : d.files)
                same += g.name == d.files[i].name ? 1 : 0;
            unique += same == 1 ? 1 : 0;
            std::printf("  tidy file %zu: %s\n", i, d.files[i].name.c_str());
        }
        P.eq("tidy.names_valid", valid, static_cast<int64_t>(d.files.size()));
        P.eq("tidy.names_unique", unique, static_cast<int64_t>(d.files.size()));
        P.eq("tidy.program_first", d.files.empty() ? 0 : (d.files[0].name == pg::kProgramName ? 1 : 0), 1);
        P.in("tidy.tab_in_range", static_cast<double>(d.active), 0.0, static_cast<double>(d.files.size()) - 1.0);
        P.eq("tidy.plays", b.editorPort().running() == pg::singleFile(pg::kExamples[0].source) ? 1 : 0, 1);
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
        pg::Processor p;
        runAndSettle(p, pg::kExamples[1].source);
        p.setStateInformation(block.getData(), static_cast<int>(block.getSize()));
        p.poll();
        P.eq("empty.first_example", p.editorPort().running() == pg::singleFile(pg::kExamples[0].source) ? 1 : 0, 1);
        P.near("nan.half", static_cast<double>(p.getParameters()[0]->getValue()), 0.5, 1e-6);
    }
    return P.finish();
}
