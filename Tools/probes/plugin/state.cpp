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
//   SIXTEEN a program of sixteen files (kMaxFiles: the program and fifteen it includes) runs, is saved and loaded into a
//           fresh processor that plays it bit for bit with the same draft; a session of twenty files loads the first
//           sixteen; a seventeenth file is refused by write_file and add_library_file, each naming the cap
//   RENAME  RUN with a program whose first parameter has another name moves that dial to the new parameter's
//           default; one whose first parameter keeps its name keeps the dial where it was
//   CARRY   a dial keeps its value in its units across a run whose declarations change (Controls.h's DialCarry), from
//           the new program's first block, before poll() writes it to the host (a processor playing as a host does;
//           its output says what plays): a log dial's range 10 -> 30 keeps 9.68 L, a dial made log with its range
//           widened (100-3000 linear -> 100-50000 log) keeps 626 kg; the effect's crossfade and the instrument's
//           note after its fade play those values, never the old normalised ones (28.84 L, 309 kg); poll() then
//           writes them, each one gesture, run_program saying "size: 9.68 L -> 9.68 L (range changed)" and "mass:
//           626 kg -> 626 kg (range changed, taper changed)", and a run with the same declarations writes nothing.
//           An int whose values have names keeps its name: KEYS CAR PITCH -> KEYS CAR TRUCK PITCH keeps CAR on CAR
//           (1, never TRUCK) and PITCH on PITCH (2 -> 3, "option moved"), the host's text the name. A range that no
//           longer holds a value clamps it (25 L -> 10 L, "clamped"). Two dials that swap places each start at their
//           default, as before, run_program saying where each moved from. The host's automation: a value the host
//           writes after the compile and before poll() plays as it says (the new declarations read it) and stays. Two
//           runs before one poll(): the second carries from the first program's controls (what the values still mean),
//           never from the one between (9.68 L, never 20 L), and only it is written, one gesture a dial. A state load
//           carries nothing and its values are its program's: a run of the same declarations after it writes nothing.
//           A carry writes once: its status applied again (the self-test giving up: a new revision, the same program)
//           leaves a value the host moved back to the one the carry was made from (an undo) as it is
//   BROKEN  a session whose program does not compile (here: a syntax error) leaves the program that played playing,
//           its errors in the status, and the draft the session's
//   EMPTY   a session saved without a program (an older version's) plays the first example
//   NAN     a dial saved as NaN loads as 0.5
//   CIRCUITS a program with a netlist whose configuration is the self-test's (AutoClipper.h), refined (the tests off,
//           Compiler::selfTestNow), saved: the state keeps the answer (<PROGRAM><CIRCUITS><ANSWER key text/>); loaded
//           into a fresh processor with the store emptied (circuits::Answers::clearForTests), it restores the answer
//           before it compiles, so that compile wants no self-test and says nothing, and it plays the saved
//           processor's bits (half a second, below the freezer's 3 s); an answer that does not read back as this
//           FMinus's (another key, a damaged line) is not restored, and the program plays its provisional configuration
//   PANEL   the panel's own dials (Controls.h's isPanelDial): a program of 40 dials (d1 to d40, each at its number,
//           of 100), the 33rd and the 40th on the left and the right; the host sees 32 parameters, the panel 64 ports,
//           a panel dial's with no host parameter behind it (native: none); run_program lists them, "panel only"; both
//           play their defaults; the 33rd turned through its port (the editor's way) plays at once and writes no host
//           parameter; set_dial sets the 40th; a session keeps all 32 (<PANEL><DIAL slot value/>, the unused too,
//           as the APVTS keeps every <PARAM>; no <PARAM> of them) and a fresh processor loads them; a session from
//           before them (no <PANEL>) plays their defaults; a run that widens the 33rd's range keeps its value in its
//           units (the carry, as a host dial's). An A/B, its sides without their own dials, B a version of 34 dials
//           playing and A the editor's 40 (the 40th at 12): a session saved then plays A in a fresh processor with
//           the 40th at 12, not at its default
//   FILEVIEWS the netlists' views (EditorPort::fileViews: the view, the zoom, the centre, the pinned probes' nodes) kept in
//           the editor's tree (<FILE> children) and read back the same; a file that is no netlist is not kept, nor more
//           than four probes; a state from before them (no <FILE>) loads as it did, with none
#include "Probe.h"

#include "Audio.h"
#include "AutoClipper.h"
#include "Rig.h"

#include "dsp/Compiler.h"
#include "fminus/Circuits.h"
#include "plugin/ChatAgent.h"
#include "plugin/EditorPort.h"
#include "plugin/Processor.h"
#include "plugin/ProjectStore.h"

#include "Examples.h"
#include "Library.h"
#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <thread>

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
            const int len = static_cast<int>(std::min<std::size_t>(256, n - pos));   // the last block: what is left
            juce::AudioBuffer<float> buffer(2, len);
            for (int i = 0; i < len; ++i)
            {
                buffer.setSample(0, i, in.l[pos + static_cast<std::size_t>(i)]);
                buffer.setSample(1, i, in.r[pos + static_cast<std::size_t>(i)]);
            }
            juce::MidiBuffer midi;
            if (kInstrument && pos == 0)
                midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<uint8_t>(100)), 0);
            p.processBlock(buffer, midi);
            for (int i = 0; i < len; ++i)
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

    juce::var object(std::initializer_list<std::pair<const char*, juce::var>> fields)
    {
        auto* o = new juce::DynamicObject();
        for (const auto& [k, v] : fields)
            o->setProperty(k, v);
        return juce::var(o);
    }

    std::string textOf(const pg::ChatAgent::McpResult& r)
    {
        std::string out;
        if (const juce::Array<juce::var>* c = r.content.getArray())
            for (const juce::var& b : *c)
                if (b["type"].toString() == "text")
                    out += b["text"].toString().toStdString();
        return out;
    }

    // The most files a program may have: the second example including fifteen of its own, each a function.
    pg::Sources sixteenFiles()
    {
        pg::Sources files;
        std::string includes;
        for (int k = 1; k < static_cast<int>(pg::kMaxFiles); ++k)
        {
            const std::string n = "f" + std::to_string(k);
            files.push_back({ n + ".fm", "float " + n + "(float x) { return x + " + std::to_string(k) + ".0f; }\n" });
            includes += "#include \"" + n + ".fm\"\n";
        }
        files.insert(files.begin(), { "program.fm", includes + std::string(pg::kExamples[1].source) });
        return files;
    }

    // CARRY's programs: dials as `params` declares them (each a line), the left channel `left` and the right `right`
    // (expressions of them), and a panel.gui of `gui` (its controls' lines).
    pg::Sources carryProgram(const std::string& params, const std::string& left, const std::string& right, const std::string& gui)
    {
        const std::string body = "        for (int i = 0; i < n; ++i)\n        {\n"
                                 "            left[i] " + std::string(kInstrument ? "+=" : "=") + " " + left + ";\n"
                                 "            right[i] " + std::string(kInstrument ? "+=" : "=") + " " + right + ";\n        }\n";
        const std::string program = kInstrument
            ? "module Voice\n{\n" + params + "    bool held;\n    void start(float n, float v) { held = true; }\n"
                  "    void stop() { held = false; }\n    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                  + body + "        return held;\n    }\n};\n"
            : "module Effect\n{\n" + params + "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                  + body + "    }\n};\n";
        return { { "program.fm", program }, { "panel.gui", "row {\n" + gui + "}\n" } };
    }

    // The floats: size in litres (log, 0.05 to `sizeMax`) and mass in kilograms (100 to `massMax`, log or linear); the
    // left channel size / 100, the right mass / 100000.
    pg::Sources carryFloats(int sizeMax, int massMax, bool massLog)
    {
        return carryProgram("    param float size [[fminus::range(0.05, " + std::to_string(sizeMax) + ")]] [[fminus::default(1)]];\n"
                            "    param float mass [[fminus::range(100, " + std::to_string(massMax) + ")]] [[fminus::default(100)]];\n",
                            "size[i] * 0.01f", "mass[i] * 0.00001f",
                            "    knob size taper=log unit=\"L\"\n    knob mass" + std::string(massLog ? " taper=log" : "") + " unit=\"kg\"\n");
    }

    // Two ints whose values have names (`names`, each a quoted text): drive and gear, the left channel drive / 10, the
    // right gear / 10.
    pg::Sources carryOptions(const std::string& names, int count)
    {
        const std::string range = "[[fminus::range(0, " + std::to_string(count - 1) + ")]]";
        return carryProgram("    param int drive " + range + ";\n    param int gear " + range + ";\n", "drive[0] * 0.1f",
                            "gear[0] * 0.1f", "    buttons drive " + names + "\n    buttons gear " + names + "\n");
    }

    // A processor as a host drives it (48 kHz, blocks of 256; an instrument holding middle C from its first block), its
    // freezer off: what it plays is its program's own code.
    struct HostRig
    {
        pg::Processor p;
        bool noteOn = false;

        HostRig()
        {
            p.compiler().setFreezing(false);
            p.setRateAndBufferSizeDetails(kFs, 256);
            p.prepareToPlay(kFs, 256);
        }

        probe::Stereo render(double seconds)
        {
            const std::size_t n = static_cast<std::size_t>(seconds * kFs);
            probe::Stereo out(n);
            for (std::size_t pos = 0; pos < n; pos += 256)
            {
                const int len = static_cast<int>(std::min<std::size_t>(256, n - pos));
                juce::AudioBuffer<float> buffer(2, len);
                buffer.clear();
                juce::MidiBuffer midi;
                if (kInstrument && !noteOn)
                {
                    midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<uint8_t>(100)), 0);
                    noteOn = true;
                }
                p.processBlock(buffer, midi);
                for (int i = 0; i < len; ++i)
                {
                    out.l[pos + static_cast<std::size_t>(i)] = buffer.getSample(0, i);
                    out.r[pos + static_cast<std::size_t>(i)] = buffer.getSample(1, i);
                }
            }
            return out;
        }

        void set(int slot, float plain) { p.getParameters()[slot]->setValueNotifyingHost(pg::normOf(p.control(slot), plain)); }
        float value(int slot) const { return pg::plainOf(p.control(slot), p.getParameters()[slot]->getValue()); }

        // run_program on `files` (the draft set to them): its result, the compile done, poll() not yet run.
        std::string run(const pg::Sources& files)
        {
            p.editorPort().setDraft({ files, 0 });
            return textOf(p.chatAgent().mcpCall("run_program", juce::var(new juce::DynamicObject())));
        }
    };

    // PANEL's program: `count` float dials d1 to d<count>, each 0 to `top` at its number, the left d33 / 100 and the
    // right d<count> / 100 (the panel's first own dial and its last here), a panel.gui of those two knobs.
    pg::Sources panelDials(int count, int top = 100)
    {
        std::string params;
        for (int k = 1; k <= count; ++k)
            params += "    param float d" + std::to_string(k) + " [[fminus::range(0, " + std::to_string(top) + ")]] [[fminus::default("
                    + std::to_string(k) + ")]];\n";
        const std::string last = "d" + std::to_string(count);
        return carryProgram(params, "d33[i] * 0.01f", last + "[i] * 0.01f", "    knob d33\n    knob " + last + "\n");
    }

    // The largest distance of x's samples [from, to) from v.
    double offBy(const std::vector<float>& x, std::size_t from, std::size_t to, double v)
    {
        double worst = 0.0;
        for (std::size_t i = from; i < std::min(to, x.size()); ++i)
            worst = std::max(worst, std::fabs(static_cast<double>(x[i]) - v));
        return worst;
    }

    // The host's view of the parameters: each change (its slot) and each gesture's begin and end, counted.
    struct HostView final : juce::AudioProcessorListener
    {
        std::array<int, pg::kMaxControls> changes{}, begins{}, ends{};
        void audioProcessorParameterChanged(juce::AudioProcessor*, int index, float) override
        {
            if (index >= 0 && index < pg::kMaxControls)
                ++changes[static_cast<std::size_t>(index)];
        }
        void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override {}
        void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int index) override
        {
            if (index >= 0 && index < pg::kMaxControls)
                ++begins[static_cast<std::size_t>(index)];
        }
        void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int index) override
        {
            if (index >= 0 && index < pg::kMaxControls)
                ++ends[static_cast<std::size_t>(index)];
        }
        int total() const
        {
            int n = 0;
            for (int c : changes)
                n += c;
            return n;
        }
        void reset()
        {
            changes.fill(0);
            begins.fill(0);
            ends.fill(0);
        }
    };

    bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

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

    // SIXTEEN
    {
        const pg::Sources files = sixteenFiles();
        P.eq("sixteen.files", static_cast<int64_t>(files.size()), static_cast<int64_t>(pg::kMaxFiles));
        P.eq("sixteen.the_cap", static_cast<int64_t>(pg::kMaxFiles), 16);
        pg::Processor a;
        runAndSettle(a, files);
        P.eq("sixteen.runs", a.editorPort().running() == files && a.editorPort().status().ok ? 1 : 0, 1);
        a.editorPort().setDraft({ files, 15 });
        juce::MemoryBlock saved;
        a.getStateInformation(saved);
        pg::Processor b;
        b.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        b.poll();
        P.eq("sixteen.loads_the_program", b.editorPort().running() == files ? 1 : 0, 1);
        P.eq("sixteen.loads_the_draft", b.editorPort().draft() == pg::Draft{ files, 15 } ? 1 : 0, 1);
        const probe::Stereo oa = play(a), ob = play(b);
        P.eq("sixteen.same_output_bits", probe::sameBits(oa.l, ob.l) && probe::sameBits(oa.r, ob.r) ? 1 : 0, 1);

        // A session of twenty files (a later build's, or crafted): the first sixteen.
        auto program = std::make_unique<juce::XmlElement>("PROGRAM");
        auto* run = program->createNewChildElement("RUN");
        auto* first = run->createNewChildElement("FILE");
        first->setAttribute("name", "program.fm");
        first->setAttribute("text", juce::String::fromUTF8(pg::kExamples[0].source));
        auto* draft = program->createNewChildElement("DRAFT");
        for (int k = 0; k < 20; ++k)
        {
            auto* f = draft->createNewChildElement("FILE");
            f->setAttribute("name", k == 0 ? juce::String("program.fm") : "g" + juce::String(k) + ".fm");
            f->setAttribute("text", "// file " + juce::String(k) + "\n");
        }
        const juce::MemoryBlock twenty = withProgram(saved, std::move(program));
        pg::Processor c;
        c.setStateInformation(twenty.getData(), static_cast<int>(twenty.getSize()));
        c.poll();
        const pg::Draft cut = c.editorPort().draft();
        P.eq("sixteen.twenty_load_sixteen", static_cast<int64_t>(cut.files.size()), static_cast<int64_t>(pg::kMaxFiles));
        P.eq("sixteen.twenty_the_first", cut.files.size() == pg::kMaxFiles && cut.files.back().name == "g15.fm" ? 1 : 0, 1);

        // The seventeenth: refused, the cap named.
        const pg::ChatAgent::McpResult w = a.chatAgent().mcpCall("write_file", object({ { "name", "f16.fm" },
                                                                                       { "content", "// one more\n" } }));
        std::printf("write_file of a seventeenth: %s\n", textOf(w).c_str());
        P.eq("sixteen.write_refused", w.isError && textOf(w).find("at most 16 files") != std::string::npos ? 1 : 0, 1);
        const pg::ChatAgent::McpResult l = a.chatAgent().mcpCall("add_library_file", object({ { "name", pg::kLibrary[0].name } }));
        std::printf("add_library_file of a seventeenth: %s\n", textOf(l).c_str());
        P.eq("sixteen.library_refused", l.isError && textOf(l).find("at most 16 files") != std::string::npos ? 1 : 0, 1);
        P.eq("sixteen.still_sixteen", static_cast<int64_t>(a.editorPort().draft().files.size()), 16);
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

    // CARRY
    {
        // The floats: size 9.68 L (log, to 10) and mass 626 kg (linear, to 3000), then a run that widens size's range
        // to 30 and makes mass log to 50000. The old normalised values would play 28.84 L and 309 kg.
        HostRig h;
        const std::string first = h.run(carryFloats(10, 3000, false));
        h.p.poll();
        P.eq("carry.first_runs", h.p.editorPort().status().ok && pg::Control(h.p.control(0)).taper == pg::ControlTaper::log ? 1 : 0, 1);
        if (!h.p.editorPort().status().ok)
            std::printf("%s\n", first.c_str());
        h.set(0, 9.68f);
        h.set(1, 626.0f);
        h.render(0.1);
        const probe::Stereo before = h.render(0.02);
        P.near("carry.before_size", static_cast<double>(before.l.back()) * 100.0, 9.68, 1e-3);
        P.near("carry.before_mass", static_cast<double>(before.r.back()) * 1e5, 626.0, 0.01);
        HostView view;
        h.p.addListener(&view);
        const std::string ran = h.run(carryFloats(30, 50000, true));
        std::printf("run_program:\n%s\n", ran.c_str());
        const probe::Stereo window = h.render(0.06);                 // the new program plays; poll() has not run
        const std::size_t settled = static_cast<std::size_t>(0.03 * kFs);   // past the effect's crossfade, the voice's fade
        P.eq("carry.window_no_writes", view.total(), 0);
        if constexpr (kInstrument)
            P.le("carry.window_size_never_remapped", offBy(window.l, settled, window.l.size(), 0.0968), 1e-5);
        else
            P.le("carry.window_size_never_remapped", offBy(window.l, 0, window.l.size(), 0.0968), 1e-5);   // the crossfade too
        P.le("carry.window_mass", offBy(window.r, settled, window.r.size(), 0.00626) * 1e5, 0.01);
        h.p.poll();
        P.eq("carry.size_one_gesture", view.changes[0] == 1 && view.begins[0] == 1 && view.ends[0] == 1 ? 1 : 0, 1);
        P.eq("carry.mass_one_gesture", view.changes[1] == 1 && view.begins[1] == 1 && view.ends[1] == 1 ? 1 : 0, 1);
        P.eq("carry.nothing_else_written", view.total(), 2);
        P.near("carry.host_size", static_cast<double>(h.value(0)), 9.68, 1e-3);
        P.near("carry.host_mass", static_cast<double>(h.value(1)), 626.0, 0.01);
        const probe::Stereo after = h.render(0.05);
        P.le("carry.after_size", offBy(after.l, 0, after.l.size(), 0.0968), 1e-5);
        P.le("carry.after_mass", offBy(after.r, 0, after.r.size(), 0.00626) * 1e5, 0.01);
        P.eq("carry.report_size", has(ran, "size: 9.68 L -> 9.68 L (range changed)") ? 1 : 0, 1);
        P.eq("carry.report_mass", has(ran, "mass: 626 kg -> 626 kg (range changed, taper changed)") ? 1 : 0, 1);
        P.eq("carry.report_dial_lines", has(ran, ", now 9.68 L\n") && has(ran, ", now 626 kg\n") ? 1 : 0, 1);

        // The same declarations again: nothing written, no report.
        view.reset();
        const std::string same = h.run(carryFloats(30, 50000, true));
        h.p.poll();
        P.eq("carry.same_writes_nothing", view.total(), 0);
        P.eq("carry.same_no_report", !has(same, "What the new declarations did") ? 1 : 0, 1);

        // A range that no longer holds the value: 25 L to a range of 10, clamped; mass back to linear, 626 kg kept. And
        // the host's automation: size written by the host after the compile and before poll() plays as written (the
        // new declarations read it) and stays.
        h.set(0, 25.0f);
        h.render(0.05);
        const std::string clamped = h.run(carryFloats(10, 3000, false));
        std::printf("run_program:\n%s\n", clamped.c_str());
        h.p.getParameters()[1]->setValueNotifyingHost(0.5f);            // the host's automation, on mass
        const probe::Stereo moved = h.render(0.06);
        P.le("carry.clamped_plays", offBy(moved.l, settled, moved.l.size(), 0.10), 1e-5);
        P.le("carry.automation_plays", offBy(moved.r, settled, moved.r.size(), (100.0 + 0.5 * 2900.0) * 1e-5) * 1e5, 0.05);
        h.p.poll();
        P.near("carry.clamped_host", static_cast<double>(h.value(0)), 10.0, 1e-4);
        P.near("carry.automation_stays", static_cast<double>(h.p.getParameters()[1]->getValue()), 0.5, 1e-7);
        P.eq("carry.report_clamped", has(clamped, "size: 25.0 L -> 10.0 L (range changed, clamped)") ? 1 : 0, 1);
        h.p.removeListener(&view);
    }
    {
        // Ints with names: drive on CAR and gear on PITCH, then TRUCK inserted before PITCH. The old normalised values
        // would play TRUCK (2) for drive.
        HostRig h;
        h.run(carryOptions("\"KEYS\" \"CAR\" \"PITCH\"", 3));
        h.p.poll();
        h.set(0, 1.0f);
        h.set(1, 2.0f);
        h.render(0.1);
        const std::string ran = h.run(carryOptions("\"KEYS\" \"CAR\" \"TRUCK\" \"PITCH\"", 4));
        std::printf("run_program:\n%s\n", ran.c_str());
        const probe::Stereo window = h.render(0.06);
        const std::size_t settled = static_cast<std::size_t>(0.03 * kFs);
        if constexpr (kInstrument)
            P.le("carry.option_car_never_truck", offBy(window.l, settled, window.l.size(), 0.1), 1e-6);
        else
            P.le("carry.option_car_never_truck", offBy(window.l, 0, window.l.size(), 0.1), 1e-6);
        P.le("carry.option_pitch_moved", offBy(window.r, settled, window.r.size(), 0.3), 1e-6);
        h.p.poll();
        P.near("carry.option_car_host", static_cast<double>(h.value(0)), 1.0, 0.0);
        P.near("carry.option_pitch_host", static_cast<double>(h.value(1)), 3.0, 0.0);
        P.eq("carry.option_text", h.p.getParameters()[0]->getCurrentValueAsText() == "CAR"
                                      && h.p.getParameters()[1]->getCurrentValueAsText() == "PITCH" ? 1 : 0, 1);
        P.eq("carry.report_option_moved", has(ran, "gear: PITCH -> PITCH (option moved)") ? 1 : 0, 1);
        P.eq("carry.report_options_changed", has(ran, "drive: CAR -> CAR (options changed)") ? 1 : 0, 1);
    }
    {
        // Two dials that swap places: each at its default, as before.
        HostRig h;
        const auto two = [](const char* a, double da, const char* b, double db) {
            return carryProgram(std::string("    param float ") + a + " [[fminus::range(0, 10)]] [[fminus::default(" + std::to_string(da) + ")]];\n"
                                + "    param float " + b + " [[fminus::range(0, 10)]] [[fminus::default(" + std::to_string(db) + ")]];\n",
                                std::string(a) + "[i] * 0.01f", std::string(b) + "[i] * 0.01f", "");
        };
        h.run(two("alpha", 2.0, "beta", 3.0));
        h.p.poll();
        h.set(0, 7.0f);
        h.set(1, 8.0f);
        h.render(0.05);
        const std::string ran = h.run(two("beta", 3.0, "alpha", 2.0));
        std::printf("run_program:\n%s\n", ran.c_str());
        const probe::Stereo window = h.render(0.06);
        const std::size_t settled = static_cast<std::size_t>(0.03 * kFs);
        P.le("carry.reorder_plays_defaults", std::max(offBy(window.l, settled, window.l.size(), 0.03),
                                                      offBy(window.r, settled, window.r.size(), 0.02)), 1e-6);
        h.p.poll();
        P.near("carry.reorder_beta_default", static_cast<double>(h.value(0)), 3.0, 1e-5);
        P.near("carry.reorder_alpha_default", static_cast<double>(h.value(1)), 2.0, 1e-5);
        P.eq("carry.report_moved", has(ran, "alpha: 7.00 -> 2.00 (moved from dial 1 to 2: its default)") ? 1 : 0, 1);
    }
    {
        // Two runs before one poll(): the second's carry is made from what the host's values still mean (the first
        // program's controls), never from the program between, and only it is written. size 9.68 L (to 10), then to 30,
        // then to 20: carried from the program between, 9.68 L would read as 28.84 L and clamp to 20 L (mass 309 kg).
        HostRig h;
        h.run(carryFloats(10, 3000, false));
        h.p.poll();
        h.set(0, 9.68f);
        h.set(1, 626.0f);
        h.render(0.05);
        HostView view;
        h.p.addListener(&view);
        h.run(carryFloats(30, 50000, true));
        h.render(0.05);
        const std::string ran = h.run(carryFloats(20, 20000, true));
        std::printf("run_program (the second before poll):\n%s\n", ran.c_str());
        const probe::Stereo window = h.render(0.06);
        const std::size_t settled = static_cast<std::size_t>(0.03 * kFs);
        P.eq("carry.twice_window_no_writes", view.total(), 0);
        if constexpr (kInstrument)
            P.le("carry.twice_window_size", offBy(window.l, settled, window.l.size(), 0.0968), 1e-5);
        else
            P.le("carry.twice_window_size", offBy(window.l, 0, window.l.size(), 0.0968), 1e-5);   // the crossfade too
        P.le("carry.twice_window_mass", offBy(window.r, settled, window.r.size(), 0.00626) * 1e5, 0.01);
        h.p.poll();
        P.eq("carry.twice_one_gesture_each", view.changes[0] == 1 && view.begins[0] == 1 && view.ends[0] == 1 && view.changes[1] == 1
                                                 && view.begins[1] == 1 && view.ends[1] == 1 && view.total() == 2 ? 1 : 0, 1);
        P.near("carry.twice_host_size", static_cast<double>(h.value(0)), 9.68, 1e-3);
        P.near("carry.twice_host_mass", static_cast<double>(h.value(1)), 626.0, 0.01);
        P.eq("carry.twice_report", has(ran, "size: 9.68 L -> 9.68 L (range changed)\n") && has(ran, "mass: 626 kg -> 626 kg (range changed)\n")
                                       ? 1 : 0, 1);
        h.p.removeListener(&view);
    }
    {
        // A state load carries nothing, and its values are its program's from then on: a run of the same declarations
        // after it writes nothing and keeps 9.68 L and 626 kg (carried from the program the processor started with,
        // Init, whose dials have other names, both would start at their defaults, 1 L and 100 kg).
        juce::MemoryBlock saved;
        {
            HostRig a;
            a.run(carryFloats(10, 3000, false));
            a.p.poll();
            a.set(0, 9.68f);
            a.set(1, 626.0f);
            a.p.getStateInformation(saved);
        }
        HostRig h;
        h.p.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        h.p.poll();
        P.eq("carry.load_values", std::fabs(h.value(0) - 9.68f) < 1e-3f && std::fabs(h.value(1) - 626.0f) < 0.01f ? 1 : 0, 1);
        HostView view;
        h.p.addListener(&view);
        const std::string ran = h.run(carryFloats(10, 3000, false));
        const probe::Stereo window = h.render(0.06);
        const std::size_t settled = static_cast<std::size_t>(0.03 * kFs);
        P.le("carry.load_run_plays_size", offBy(window.l, settled, window.l.size(), 0.0968), 1e-5);
        P.le("carry.load_run_plays_mass", offBy(window.r, settled, window.r.size(), 0.00626) * 1e5, 0.01);
        h.p.poll();
        P.eq("carry.load_run_writes_nothing", view.total(), 0);
        P.near("carry.load_run_size", static_cast<double>(h.value(0)), 9.68, 1e-3);
        P.near("carry.load_run_mass", static_cast<double>(h.value(1)), 626.0, 0.01);
        P.eq("carry.load_run_unchanged", has(ran, "Its dials: unchanged") && !has(ran, "What the new declarations did") ? 1 : 0, 1);
        h.p.removeListener(&view);
    }
    {
        // A carry writes once: its status applied again (a new revision of the same program: the self-test's notes,
        // here a test that gives up) leaves a value moved back since where it is (an undo in the host, or its
        // automation, back on the value the carry was made from), never writing the carry over it again.
        pg::circuits::Answers& store = pg::circuits::answers();
        store.clearForTests();
        const auto clipped = [](pg::Sources files) {
            files.front().text = "#include \"clipper.cir\"\n" + files.front().text;
            files.push_back({ "clipper.cir", probe::kAutoClipper });
            return files;
        };
        HostRig h;
        h.p.compiler().setSelfTesting(false);
        h.p.compiler().setSelfTestBudget(1);                     // its test stops before its references: no answer
        h.run(clipped(carryFloats(10, 3000, false)));
        h.p.poll();
        h.set(0, 9.68f);
        h.render(0.05);
        const float fromNorm = h.p.getParameters()[0]->getValue();
        h.run(clipped(carryFloats(30, 50000, true)));
        h.p.poll();                                              // the carry written: 9.68 L in the range to 30
        const bool written = h.p.getParameters()[0]->getValue() != fromNorm && std::fabs(h.value(0) - 9.68f) < 1e-3f;
        h.p.getParameters()[0]->setValueNotifyingHost(fromNorm); // the host's undo
        const pg::CompileStatus before = h.p.editorPort().status();
        HostView view;
        h.p.addListener(&view);
        const bool refined = h.p.compiler().selfTestNow();
        const pg::CompileStatus after = h.p.editorPort().status();
        h.p.poll();
        std::printf("carry once: refined %d, revisions %llu -> %llu, notes: %s", refined ? 1 : 0,
                    static_cast<unsigned long long>(before.revision), static_cast<unsigned long long>(after.revision), after.notes.c_str());
        P.eq("carry.once_written", written ? 1 : 0, 1);
        P.eq("carry.once_applied_again", before.ok && has(before.notes, "provisional: ") && !refined && after.ok
                                             && after.revision == before.revision + 1 && after.program == before.program
                                             && after.carry.ticket == before.carry.ticket ? 1 : 0, 1);
        P.eq("carry.once_undo_stands", view.total() == 0 && h.p.getParameters()[0]->getValue() == fromNorm ? 1 : 0, 1);
        h.p.removeListener(&view);
        store.clearForTests();
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

    // CIRCUITS
    {
        pg::circuits::Answers& store = pg::circuits::answers();
        store.clearForTests();
        const pg::Sources files = { { pg::kProgramName, probe::clipperProgram("clipper.cir", "Clipper", false) },
                                    { "clipper.cir", probe::kAutoClipper } };
        const std::vector<std::string> keys = pg::circuits::keysOf(files);
        const std::string key = keys.empty() ? std::string() : keys.front();
        pg::Processor a;
        a.compiler().setSelfTesting(false);
        runAndSettle(a, files);
        const pg::CompileStatus provisional = a.editorPort().status();
        const bool refined = a.compiler().selfTestNow();
        a.settle();
        const std::string answer = store.find(key).value_or("");
        std::printf("circuits: provisional: %s; refined %d: %s", provisional.notes.c_str(), refined ? 1 : 0,
                    a.editorPort().status().notes.c_str());
        P.eq("circuits.refined", provisional.ok && !provisional.notes.empty() && refined && a.editorPort().status().ok && !answer.empty()
                                     ? 1 : 0, 1);
        juce::MemoryBlock saved;
        a.getStateInformation(saved);
        const std::unique_ptr<juce::XmlElement> xml = juce::AudioProcessor::getXmlFromBinary(saved.getData(),
                                                                                            static_cast<int>(saved.getSize()));
        const juce::XmlElement* program = xml != nullptr ? xml->getChildByName("PROGRAM") : nullptr;
        const juce::XmlElement* kept = program != nullptr ? program->getChildByName("CIRCUITS") : nullptr;
        const juce::XmlElement* one = kept != nullptr ? kept->getChildByName("ANSWER") : nullptr;
        P.eq("circuits.saved", kept != nullptr && kept->getNumChildElements() == 1 && one != nullptr
                                   && one->getStringAttribute("key").toStdString() == key
                                   && one->getStringAttribute("text").toStdString() == answer ? 1 : 0, 1);

        store.clearForTests();
        pg::Processor b;
        b.compiler().setSelfTesting(false);
        b.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        b.poll();
        const pg::CompileStatus s = b.editorPort().status();
        std::printf("circuits: loaded: %s%s\n", s.errors.c_str(), s.notes.empty() ? "no messages" : s.notes.c_str());
        P.eq("circuits.restored", store.find(key) == answer ? 1 : 0, 1);
        P.eq("circuits.no_job_no_note", s.ok && s.notes.empty() && !b.compiler().selfTesting() && b.editorPort().running() == files
                                            ? 1 : 0, 1);
        const probe::Stereo oa = play(a), ob = play(b);
        P.eq("circuits.same_bits", probe::sameBits(oa.l, ob.l) && probe::sameBits(oa.r, ob.r) && probe::rmsDb(oa.l, 0, oa.size()) > -80.0
                                       ? 1 : 0, 1);

        // An answer that is not this key's (or not an answer) is not restored: the provisional configuration plays.
        std::unique_ptr<juce::XmlElement> other = juce::AudioProcessor::getXmlFromBinary(saved.getData(), static_cast<int>(saved.getSize()));
        if (juce::XmlElement* p = other != nullptr ? other->getChildByName("PROGRAM") : nullptr)
            if (juce::XmlElement* c = p->getChildByName("CIRCUITS"))
                if (juce::XmlElement* x = c->getChildByName("ANSWER"))
                {
                    x->setAttribute("key", "0123456789abcdef");
                    auto* damaged = c->createNewChildElement("ANSWER");
                    damaged->setAttribute("key", juce::String(key));
                    damaged->setAttribute("text", "fminus-circuit-choice 99 nonsense");
                }
        juce::MemoryBlock crafted;
        if (other != nullptr)
            juce::AudioProcessor::copyXmlToBinary(*other, crafted);
        store.clearForTests();
        pg::Processor c;
        c.compiler().setSelfTesting(false);
        c.setStateInformation(crafted.getData(), static_cast<int>(crafted.getSize()));
        c.poll();
        P.eq("circuits.foreign_not_restored", store.size() == 0 && c.editorPort().status().ok
                                                  && c.editorPort().status().notes == provisional.notes && c.compiler().selfTesting()
                                                  ? 1 : 0, 1);
        store.clearForTests();
    }

    // PANEL
    {
        constexpr int first = pg::kHostControls, last = 39;      // d33 and d40
        HostRig h;
        const std::string ran = h.run(panelDials(40));
        h.p.poll();
        std::printf("PANEL: run_program:\n%s\n", ran.c_str());
        P.eq("panel.runs", h.p.editorPort().status().ok && h.p.control(first).used && h.p.control(last).used ? 1 : 0, 1);
        P.eq("panel.host_sees_32", static_cast<int64_t>(h.p.getParameters().size()), pg::kHostControls);
        P.eq("panel.ports", h.p.ports().size() == static_cast<std::size_t>(pg::kMaxControls)
                                && h.p.ports()[first]->native() == nullptr && h.p.ports()[first - 1]->native() != nullptr ? 1 : 0, 1);
        {
            // Each dial a line ("33 d33: float [0, 100] default 33.0, now 33.0, panel only"): the host's without the mark.
            int marked = 0;
            for (std::size_t at = ran.find("panel only"); at != std::string::npos; at = ran.find("panel only", at + 1))
                ++marked;
            const std::size_t d32 = ran.find("32 d32: "), d33 = ran.find("33 d33: ");
            P.eq("panel.listed_panel_only", marked == 8 && d32 != std::string::npos && d33 != std::string::npos
                                                && ran.find("panel only", d32) > d33 ? 1 : 0, 1);
        }
        h.render(0.05);
        probe::Stereo out = h.render(0.02);
        P.near("panel.default_first", static_cast<double>(out.l.back()), 0.33, 1e-5);
        P.near("panel.default_last", static_cast<double>(out.r.back()), 0.40, 1e-5);
        // The editor's way: a gesture on the dial's port. No host parameter is written.
        HostView view;
        h.p.addListener(&view);
        funkgui::ParamPort& port = *h.p.ports()[first];
        port.beginGesture();
        port.setValue01(pg::normOf(h.p.control(first), 70.0f));
        port.endGesture();
        h.render(0.05);
        out = h.render(0.02);
        P.near("panel.port_plays", static_cast<double>(out.l.back()), 0.70, 1e-5);
        P.eq("panel.port_no_host_write", view.total(), 0);
        // Claude's set_dial.
        const std::string set = textOf(h.p.chatAgent().mcpCall("set_dial", object({ { "name", "d40" }, { "value", 12.0 } })));
        h.p.poll();
        h.render(0.05);
        out = h.render(0.02);
        std::printf("PANEL: set_dial: %s\n", set.c_str());
        P.near("panel.set_dial_plays", static_cast<double>(out.r.back()), 0.12, 1e-5);
        P.eq("panel.set_dial_no_host_write", view.total(), 0);
        h.p.removeListener(&view);
        // The session: kept beside the host's parameters, and loaded.
        juce::MemoryBlock saved;
        h.p.getStateInformation(saved);
        std::unique_ptr<juce::XmlElement> xml = juce::AudioProcessor::getXmlFromBinary(saved.getData(), static_cast<int>(saved.getSize()));
        const juce::XmlElement* panel = xml != nullptr ? xml->getChildByName("PANEL") : nullptr;
        int kept = 0;
        uint64_t slots = 0;
        bool firstKept = false;
        for (const juce::XmlElement* d = panel != nullptr ? panel->getFirstChildElement() : nullptr; d != nullptr; d = d->getNextElement())
        {
            ++kept;
            if (const int slot = d->getIntAttribute("slot", -1); pg::isPanelDial(slot))
                slots |= pg::slotBit(slot);
            firstKept = firstKept || (d->getIntAttribute("slot") == first && std::fabs(d->getDoubleAttribute("value")
                                                                                       - pg::normOf(h.p.control(first), 70.0f)) < 1e-6);
        }
        bool paramOf = false;
        for (const juce::XmlElement* c = xml != nullptr ? xml->getFirstChildElement() : nullptr; c != nullptr; c = c->getNextElement())
            paramOf = paramOf || (c->hasTagName("PARAM") && c->getStringAttribute("id") == "p" + juce::String(first + 1));
        // Every one of the 32, as the APVTS keeps every <PARAM>: the 8 the program uses and the 24 it does not.
        P.eq("panel.state_kept", panel != nullptr && kept == pg::kPanelControls && slots == ~(pg::slotBit(pg::kHostControls) - 1u)
                                     && firstKept && !paramOf ? 1 : 0, 1);
        {
            HostRig b;
            b.p.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
            b.p.poll();
            b.render(0.05);
            const probe::Stereo loaded = b.render(0.02);
            P.near("panel.state_loads_first", static_cast<double>(loaded.l.back()), 0.70, 1e-5);
            P.near("panel.state_loads_last", static_cast<double>(loaded.r.back()), 0.12, 1e-5);
        }
        {
            if (xml != nullptr)
                if (juce::XmlElement* p = xml->getChildByName("PANEL"))
                    xml->removeChildElement(p, true);
            juce::MemoryBlock older;
            if (xml != nullptr)
                juce::AudioProcessor::copyXmlToBinary(*xml, older);
            HostRig c;
            c.p.setStateInformation(older.getData(), static_cast<int>(older.getSize()));
            c.p.poll();
            c.render(0.05);
            const probe::Stereo loaded = c.render(0.02);
            P.near("panel.older_state_defaults_first", static_cast<double>(loaded.l.back()), 0.33, 1e-5);
            P.near("panel.older_state_defaults_last", static_cast<double>(loaded.r.back()), 0.40, 1e-5);
        }
        // A run that widens every range to 200: d33 keeps 70 (the carry), at its new turn.
        const std::string wider = h.run(panelDials(40, 200));
        h.p.poll();
        h.render(0.08);
        out = h.render(0.02);
        P.near("panel.carry_keeps_units", static_cast<double>(out.l.back()), 0.70, 1e-5);
        P.near("panel.carry_new_turn", static_cast<double>(h.p.ports()[first]->value01()), 0.35, 1e-6);
    }
    {
        // An A/B whose sides have no dials of their own, its B a version of 34 dials (d35 to d40 unused while it
        // plays), A the editor's 40 with d40 at 12: a session saved while B plays keeps A's files and every panel dial,
        // so a fresh processor plays A with d40 at 12, as a host dial in that case keeps its value, not at its default.
        const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                   .getChildFile(juce::String(pg::product::kEnvPrefix) + "state-probe-" + juce::String(juce::Time::currentTimeMillis()));
        dir.deleteRecursively();
        pg::projectstore::setDirectoryForTests(dir);
        {
            HostRig a;
            pg::EditorPort& port = a.p.editorPort();
            (void)a.run(panelDials(34));
            a.p.poll();
            pg::SavedVersion version;
            std::string why;
            P.eq("panel.ab_version", port.saveVersion("Panel AB", version, why) && version.number == 1 ? 1 : 0, 1);
            (void)a.run(panelDials(40));
            a.p.poll();
            (void)textOf(a.p.chatAgent().mcpCall("set_dial", object({ { "name", "d40" }, { "value", 12.0 } })));
            a.p.poll();
            a.render(0.05);
            const bool begun = port.startAb("Panel AB", 0, 1, why);
            port.playAb(1);
            a.p.settle();
            for (int k = 0; k < 3000 && (port.ab().playing != 1 || port.ab().measuring); ++k)
            {
                a.p.poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            a.render(0.05);
            P.eq("panel.ab_b_plays", begun && port.ab().on && port.ab().playing == 1 && !port.ab().ownDials
                                         && a.p.control(33).used && !a.p.control(39).used ? 1 : 0, 1);
            juce::MemoryBlock during;
            a.p.getStateInformation(during);
            HostRig b;
            b.p.setStateInformation(during.getData(), static_cast<int>(during.getSize()));
            b.p.poll();
            b.render(0.05);
            const probe::Stereo loaded = b.render(0.02);
            P.eq("panel.ab_session_a", b.p.control(39).used ? 1 : 0, 1);
            P.near("panel.ab_session_keeps_unused", static_cast<double>(loaded.r.back()), 0.12, 1e-5);
            port.endAb();
            a.p.settle();
        }
        pg::projectstore::setDirectoryForTests({});
        dir.deleteRecursively();
    }

    // FILEVIEWS
    {
        pg::Processor a;
        P.eq("fileviews.none_at_first", static_cast<int64_t>(a.editorPort().fileViews().size()), 0);
        pg::FileView pedal;
        pedal.file = "pedal.cir";
        pedal.schematic = true;
        pedal.zoom = 1.5f;
        pedal.x = 12.25f;
        pedal.y = -3.5f;
        pedal.probes = { "n1", "vout" };
        pg::FileView tone;
        tone.file = "tone.cir";
        tone.zoom = 0.75f;
        pg::FileView notNetlist;
        notNetlist.file = "program.fm";                  // a view only a netlist has: not kept
        pg::FileView manyProbes = pedal;
        manyProbes.file = "many.cir";
        manyProbes.probes = { "a", "b", "c", "d", "e", "f" };
        a.editorPort().setFileViews({ pedal, tone, notNetlist, manyProbes });
        juce::MemoryBlock saved;
        a.getStateInformation(saved);
        pg::Processor b;
        b.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        const std::vector<pg::FileView> views = b.editorPort().fileViews();
        P.eq("fileviews.count", static_cast<int64_t>(views.size()), 3);
        P.eq("fileviews.round_trip", views.size() == 3 && views[0] == pedal && views[1] == tone ? 1 : 0, 1);
        P.eq("fileviews.probes_capped", views.size() == 3 && views[2].probes.size() == pg::kMaxFileProbes ? 1 : 0, 1);
        // a state from before them: the same state loads, with none
        std::unique_ptr<juce::XmlElement> xml = juce::AudioProcessor::getXmlFromBinary(saved.getData(), static_cast<int>(saved.getSize()));
        bool had = false;
        if (juce::XmlElement* editor = xml != nullptr ? xml->getChildByName("EDITOR") : nullptr)
            for (juce::XmlElement* f = editor->getChildByName("FILE"); f != nullptr; f = editor->getChildByName("FILE"))
            {
                editor->removeChildElement(f, true);
                had = true;
            }
        P.eq("fileviews.in_the_editor_tree", had ? 1 : 0, 1);
        juce::MemoryBlock older;
        if (xml != nullptr)
            juce::AudioProcessor::copyXmlToBinary(*xml, older);
        pg::Processor c;
        c.setStateInformation(older.getData(), static_cast<int>(older.getSize()));
        c.poll();
        P.eq("fileviews.older_state_none", c.editorPort().fileViews().empty() ? 1 : 0, 1);
        P.eq("fileviews.older_state_loads", c.editorPort().draft() == b.editorPort().draft() ? 1 : 0, 1);
    }
    return P.finish();
}
