// PROBE name=fminus.circuit timeout=300
//
// fminus.circuit: circuit netlists in a program (fminus/Circuits.h, Patch.h's "Circuits"): a .cir file the program
// #includes is the FMinus module FMinus's circuit compiler generates from it. Each library netlist (Library.h's .cir
// files) is played by a program of this product's kind written for it: an Effect holding one module over its input,
// or a Voice holding one a note over a sawtooth, its pots dials at 0.5 set on the 16-sample grid, its law inputs swept
// within their ranges every sample:
//   COMPILE      each library netlist's program compiles with no message (errors and notes empty) and wants no
//                self-test (its configuration is the netlist's)
//   BLOCKS       2 s at the defaults (an effect's noise, an instrument's chord released at 1 s) in blocks of 64 and of
//                333: the same bits (the synth's OTA filter with its per-sample Iabc), finite, sounding
//   FREEZE       Patch::freeze (the plugin's frozen copy) is made, the layout kept; in exact math it plays the
//                program's bits; in Math::Fast it is finite and its difference 80 dB under the signal
//   TRACE        trace's pass 2 (calls of one sample) gives pass 1's bits, and the field map lists the module's
//                fields as paths (pedal.x_C1, pedal.v_D1); no probe field (circuits::kProbeFields) in the map nor in
//                what it leaves out (unmapped); the probe table holds the field, its netlist and its nodes
//   ERRORS       a netlist's error is cited at its line ("broken.cir:L:1: error: ...") in compile's errors and in
//                check's, and stops the compile; an #include of a netlist no tab has is FMinus's 'file not found'; a
//                netlist as the program (the first file) is the plugin's error
//   LINES        FMinus's error on the generated module (a module Screamer declared before the #include) is cited at
//                screamer.cir's .title line, "(in the module generated from screamer.cir, line N)"
//   INCLUDE      a netlist #included twice is generated once; one no #include reaches, broken, neither stops RUN nor
//                makes a job, and check() gives its tab its squiggles
//   LANG         for each library netlist, the language service's init, set, tick and clear (read from the netlist,
//                Lang.h) are the generated module's: its methods as FMinus compiled them (Program::Module), their
//                declarations in its text
//   RECIPE       the recipe keeps the generated texts: freeze(), ir() and frozenIr() generate nothing
//   PROVISIONAL  a netlist that leaves its configuration to the self-test (Yeh's clipper, no directives) compiles and
//                plays at once, with one job (its key, its file, its text) and the provisional note, word for word
//   STORE        the job's self-test (fminus::circuit::selfTest) stored as its answer (circuits::answers()): the next
//                compile wants no job and says nothing, the module generated is generate()'s with that answer, and
//                its key is the program's; the frozen copy of the provisional program keeps its own layout; the
//                store's find, of, claim and release, and its 64 entries
// and the circuit dials (Patch.h's "Circuits", Engine.h's "Circuit dials"):
//   DIALS        a program holding screamer.cir's module (an Effect's Screamer amp[2], a Voice's one), never calling its
//                set(): dials drive, tone and level after its own param, real 0 to 1 shown 0-100 %, linear, at the .pot
//                defaults, one set for both elements, slots 1 to 3, one nonblocking shim; a param named drive gives
//                amp_drive; two scalar fields two sets; with 31 params drive takes the 32nd slot and a note each says
//                tone and level have none; a glide from sample 21312 (a block boundary of 64 and of 333) and another
//                from 42624, 2 s: the same bits and the same calls in blocks of 64 and 333; still pots no call (an
//                instrument's note on a free instance whose values are current none either); the shim's whole text
//                (every element's set()); an instrument's note in a block of 0 samples after a glide landed: the bits
//                of the same note at the next block's first sample (its call the landed value); the frozen copy made
//                at a drive and a level moved plays within 80 dB of the program, and a pot moved drops it
//   OWN          the circuit examples (the effect's tube screamer, the synth's OTA filter) keep exactly their own dials
//                and no message: the screamer's fields say why they have none ("program.fm:L: Screamer's pots have no
//                dials: the program calls its set()"), the OTA filter's has no pot; a set() on a receiver Lang cannot
//                type counts too; only the files FMinus compiles make bindings (a copy of the netlist no #include
//                reaches, a stray tab of the main module: none); a pot's default reads the same in a decimal-comma locale
//   TEMPLATE     Play this circuit's program (fminus/CirTemplates.h) for each library netlist compiles with no message,
//                plays finitely, calls no set() (its pots dials of their own) and is in the house style; a two-channel
//                netlist's too; a law input named as one of the template's words (rate, i) gets a param <law>_in
//   SWITCH       a netlist with a switch (FMinus's switch.cir, open by default): its dial an int 0 to 1; still, no call;
//                flipped to 1, exactly ceil(0.02 rate / 16) + 1 calls at 48 kHz, and its ramp at 1; an instrument's note
//                from silence on that instance (its start's clear()) leaves it at 1 with no call; a fresh instance with
//                the dial at 1 is at 1 after its first 64 samples (its first calls, as many as the ramp takes); a ramp's
//                worth of first calls only where the switch is not at the dial's position: eight notes with it at its
//                default take one call each (an effect's first block one), eight on instances that missed a pot's move
//                one each, eight after it moved while they were free a ramp's worth each, all at 1; a NaN planted in a
//                carried field is cleared by the circuit's guard and the switch stays at 1
// and the plugin's self-test (dsp/SelfTester.h, Compiler.h's "The self-test"), a Compiler offering to an Engine played
// in blocks of 256, on AutoClipper.h's netlists (no assertion is on time; every wait has a deadline):
//   SELFTEST     with the tests off (setSelfTesting(false)) a RUN plays the provisional configuration and no test runs;
//                selfTestNow() runs the test and the refine: the status's revision and program move on (refined, still
//                the RUN's request), it compiled, the Engine's Meters::program names the refined program, and its note
//                names the self-test's choice (circuits::chosenNote); the same files compiled again read the store: no
//                job, no note
//   REFRESH      after a RUN that does not compile, the refine compiles the files that play (the Engine plays them, with
//                the module the store's answer generates) and the status still answers that RUN: not ok, its errors and
//                its files, the refined program as `program`; while a recording is armed (Engine::ownCode) the refine
//                waits, and comes when it ends; one armed (and an instrument's key pressed) while the refine compiles
//                holds it, and it is offered later without compiling again; compileNow waits for a refine running on
//                another thread, and its program plays; a RUN queued before the refine wins (it reads the store: no
//                note, no job) and the refine is dropped; an instrument's refine waits while a key is held (no new
//                program id), and lands after the release, once the voice has ended; with the tests on, a RUN whose
//                files drop the netlist cancels its test, which stores nothing, and a RUN made while the test runs has
//                its status before landed() moves (by order), the refine after it
//   DEDUP        two Compilers wanting one key run one test (the in-flight set), and both refine
//   GIVEUP       a test that gives no answer (a budget of one step) is not tried again: the status says why in place of
//                the provisional note (a new revision, the same program), and a compile of the same files wants no job
//                and says why too
//   MISFIT       an answer stored with the netlist's key that the netlist cannot take (ADAA with BDF2, as a damaged
//                session's would be, restored) neither fails the compile nor stays: provisional, one job, the answer
//                erased
#include "Probe.h"

#include "Audio.h"
#include "AutoClipper.h"
#include "Rig.h"

#include "Examples.h"
#include "Library.h"
#include "Product.h"

#include "dsp/Compiler.h"
#include "dsp/Debug.h"
#include "dsp/Engine.h"
#include "dsp/Sources.h"
#include "fminus/CirTemplates.h"
#include "fminus/Circuits.h"
#include "fminus/Lang.h"
#include "fminus/LibraryIncludes.h"
#include "fminus/Patch.h"

#include "fminus/Circuit.h"
#include "fminus/FMinus.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace pg = playground;
namespace lg = playground::lang;
namespace ms = playground::measure;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr bool kInstrument = pg::product::kIsInstrument;

    // Yeh's RC diode clipper (FMinus's examples/circuits/clipper.cir): no configuration directives, so the self-test
    // chooses its configuration and the provisional one plays meanwhile.
    constexpr const char* kClipper = probe::kAutoClipper;

    std::string libraryText(std::string_view name)
    {
        const pg::LibraryFile* f = pg::findLibraryFile(name);
        return f != nullptr ? f->source : "";
    }

    std::string replaced(std::string s, const std::string& from, const std::string& to)
    {
        const std::size_t at = s.find(from);
        return at == std::string::npos ? s : s.replace(at, from.size(), to);
    }

    // The 1-based line of `text` that starts with `start` (0: none).
    int lineOf(const std::string& text, std::string_view start)
    {
        int n = 0;
        for (const std::string_view l : lg::splitLines(text))
        {
            ++n;
            if (l.rfind(start, 0) == 0)
                return n;
        }
        return 0;
    }

    bool has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

    std::string literal(double v)
    {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.9gf", v);
        std::string s = buf;
        if (s.find_first_of(".e") == std::string::npos)
            s.insert(s.size() - 1, ".0");
        return s;
    }

    // A program of this product's kind playing the module of the netlist `file`, which `r` describes (generate()'s
    // Result): its pots dials (at 0.5) set on the 16-sample grid, counted from init (an instrument's: from the note),
    // its switches at 0, its law inputs swept within their ranges at 3 Hz (geometrically when the range starts above 0).
    std::string programFor(const std::string& file, const fminus::circuit::Result& r)
    {
        std::string dials, set, laws, args;
        for (const std::string& p : r.pots)
        {
            dials += "    param float " + p + " [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n";
            set += (set.empty() ? "" : ", ") + p + "[i]";
        }
        for (std::size_t k = 0; k < r.switches.size(); ++k)
            set += set.empty() ? "0" : ", 0";
        for (std::size_t k = 1; k < r.inputs.size(); ++k)
        {
            const fminus::circuit::Input& in = r.inputs[k];
            const std::string name = "law" + std::to_string(k);
            const double lo = in.low / in.scale, hi = in.high / in.scale;
            if (lo > 0.0)
                laws += "            const float " + name + " = " + literal(lo) + " * expf(" + literal(std::log(hi / lo))
                      + " * (0.5f + 0.5f * sinf(lfo)));\n";
            else
                laws += "            const float " + name + " = " + literal(lo) + " + " + literal(hi - lo)
                      + " * (0.5f + 0.5f * sinf(lfo));\n";
            args += ", " + name;
        }
        const std::string grid = set.empty() ? std::string()
                                             : "            if (grid == 0)\n            {\n                pedal.set(" + set
                                                   + ");\n                grid = 16;\n            }\n            grid -= 1;\n";
        const std::string sweep = "            lfo += 0.000392699082f;\n            if (lfo > 6.28318531f)\n"
                                  "                lfo -= 6.28318531f;\n";
        std::string s = "#include \"" + file + "\"\n";
        if constexpr (kInstrument)
        {
            s += "module Voice\n{\n" + dials + "    " + r.module + " pedal;\n    int grid;\n    float lfo;\n    float phase;\n"
                 "    float step;\n    float env;\n    bool held;\n\n    void init(float rate)\n    {\n        pedal.init(rate);\n    }\n\n"
                 "    void start(float note, float velocity)\n    {\n        step = 440 * expf(0.0577622650f * (note - 69)) / 48000;\n"
                 "        phase = 0;\n        env = velocity;\n        held = true;\n        grid = 0;\n        lfo = 0;\n"
                 "        pedal.clear();\n    }\n\n    void stop()\n    {\n        held = false;\n    }\n\n"
                 "    bool process(float* restrict left, float* restrict right, int n)\n    {\n        for (int i = 0; i < n; ++i)\n"
                 "        {\n" + grid + laws + sweep +
                 "            const float x = 2 * phase - 1;\n            phase += step;\n            if (phase >= 1)\n"
                 "                phase -= 1;\n            if (!held)\n                env *= 0.9995f;\n"
                 "            const float y = env * pedal.tick(x" + args + ");\n            left[i] += y;\n            right[i] += y;\n"
                 "        }\n        return held || env > 0.0001f;\n    }\n};\n";
        }
        else
        {
            s += "module Effect\n{\n" + dials + "    " + r.module + " pedal;\n    int grid;\n    float lfo;\n\n"
                 "    void init(float rate)\n    {\n        pedal.init(rate);\n    }\n\n"
                 "    void process(float* restrict left, float* restrict right, int n)\n    {\n        for (int i = 0; i < n; ++i)\n"
                 "        {\n" + grid + laws + sweep +
                 "            left[i] = pedal.tick(left[i]" + args + ");\n            right[i] = left[i];\n        }\n    }\n};\n";
        }
        return s;
    }

    // A main module of this kind that plays nothing.
    std::string silentMain()
    {
        return kInstrument ? "module Voice\n{\n    void start(float note, float velocity) {}\n"
                             "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n"
                           : "module Effect\n{\n    void process(float* restrict left, float* restrict right, int n) {}\n};\n";
    }

    probe::Stereo input(std::size_t n)
    {
        if constexpr (kInstrument)
            return probe::Stereo(n);
        else
            return probe::noise(n, 0.25);
    }

    std::vector<probe::Note> chord()
    {
        std::vector<probe::Note> out;
        if constexpr (kInstrument)
        {
            for (const int note : { 48, 55, 60 })
                out.push_back({ 0, pg::EventKind::noteOn, note, 0.8f });
            for (const int note : { 48, 55, 60 })
                out.push_back({ static_cast<std::size_t>(kFs), pg::EventKind::noteOff, note, 0.0f });
        }
        return out;
    }

    double differenceDb(const probe::Stereo& a, const probe::Stereo& b)
    {
        double signal = 0.0, diff = 0.0;
        for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        {
            const double x = static_cast<double>(a.l[i]), y = static_cast<double>(b.l[i]);
            signal += x * x;
            diff += (x - y) * (x - y);
        }
        return diff == 0.0 ? -300.0 : 10.0 * std::log10(diff / std::max(signal, 1e-300));
    }

    bool same(const probe::Stereo& a, const probe::Stereo& b) { return probe::sameBits(a.l, b.l) && probe::sameBits(a.r, b.r); }

    bool finite(const probe::Stereo& s) { return probe::allFinite(s.l) && probe::allFinite(s.r); }

    // The methods a module's text declares (Lang's FMinus index of it), each signature without its attributes.
    std::vector<std::string> declared(const std::string& text, const std::string& module, const std::vector<std::string>& names)
    {
        lg::Service s;
        s.setFiles({ { "generated.fm", text } });
        std::vector<std::string> out;
        for (const std::string& n : names)
        {
            std::string sig = "(none)";
            for (const lg::Symbol& x : s.symbols())
                if (x.kind == lg::Kind::method && x.container == module && x.name == n && x.file == 0)
                {
                    sig = x.signature;
                    if (const std::size_t a = sig.find(" [["); a != std::string::npos)
                        sig.resize(a);
                }
            out.push_back(sig);
        }
        return out;
    }

    // An Engine as the processor runs it (prepared for blocks of 256) and a Compiler offering to it, its freezer off and
    // its self-test on or off.
    struct Live
    {
        pg::Engine engine;
        std::unique_ptr<pg::Compiler> compiler;

        explicit Live(bool testing)
        {
            engine.prepare(kFs, 256);
            compiler = std::make_unique<pg::Compiler>(engine);
            compiler->setFreezing(false);
            compiler->setSelfTesting(testing);
        }
        ~Live() { compiler.reset(); }                    // before the Engine it offers to
        Live(const Live&) = delete;
        Live& operator=(const Live&) = delete;

        // `blocks` blocks of 256 (an effect's input noise; an instrument's silence, `events` in the first), as the audio
        // thread plays them: the meters after the last.
        pg::Meters play(int blocks, const std::vector<pg::Event>& events = {})
        {
            std::vector<float> l(256), r(256);
            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < 256; ++i)
                {
                    seed_ = seed_ * 1664525u + 1013904223u;
                    l[static_cast<std::size_t>(i)] = kInstrument ? 0.0f : 0.25f * (static_cast<float>(seed_ >> 8) / 8388608.0f - 1.0f);
                    r[static_cast<std::size_t>(i)] = l[static_cast<std::size_t>(i)];
                }
                float* channels[2] = { l.data(), r.data() };
                engine.process(channels, 2, 256, b == 0 ? std::span<const pg::Event>(events) : std::span<const pg::Event>());
            }
            pg::Meters m;
            for (int k = 0; k < 1000 && !engine.meters(m); ++k)
                std::this_thread::yield();
            return m;
        }

    private:
        uint32_t seed_ = 12345;
    };

    // Waits until `done` holds or `seconds` pass (a deadline, never a measurement): whether it held.
    template <typename Done>
    bool until(Done&& done, double seconds)
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (!done())
        {
            if (std::chrono::steady_clock::now() > end)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }

    // A status of a refined compile: its note of what the self-test chose.
    bool chose(const pg::CompileStatus& s)
    {
        return s.notes.find(": note: the self-test chose this circuit's configuration: ") != std::string::npos;
    }

    // A program of this kind playing AutoClipper.h's clipper (or its pot clipper), and its netlist.
    pg::Sources clipperFiles(bool pot = false, const std::string& edit = {})
    {
        return { { pg::kProgramName, probe::clipperProgram(pot ? "potclip.cir" : "clipper.cir", pot ? "PotClip" : "Clipper", pot) + edit },
                 { pot ? "potclip.cir" : "clipper.cir", pot ? probe::kPotClipper : probe::kAutoClipper } };
    }

    // SelfTestOptions::parallel on a few threads of the probe's own.
    void parallel(int count, const std::function<void(int)>& job)
    {
        std::atomic<int> next{ 0 };
        const auto work = [&] {
            for (int i = next++; i < count; i = next++)
                job(i);
        };
        std::vector<std::thread> pool;
        for (int k = 1; k < 4; ++k)
            pool.emplace_back(work);
        work();
        for (std::thread& t : pool)
            t.join();
    }

    // ---- DIALS, OWN and SWITCH: a circuit's pots and switches as dials (Patch.h, "Circuits"; Engine.h) -----------------
    // A netlist with a switch: FMinus's examples/circuits/switch.cir, its switch open by default.
    constexpr const char* kClipSwitch = "* clipswitch.cir: a diode clipper with a clipping switch, open by default\n"
                                        ".title ClipSwitch\n"
                                        ".input  u    in   scale=1\n"
                                        ".output vout a\n"
                                        ".switch clip 2 0\n"
                                        ".pot    tone 0.5\n"
                                        ".oversample 4 iir blocks=auto\n"
                                        ".adaa on\n"
                                        ".solver omega(1)\n"
                                        ".target solver=-60dB alias=-60dB level=4.5V freq=3k\n"
                                        "R1     in a  2.2k\n"
                                        "C1     a  0  10n\n"
                                        "S1     a  d  clip 1\n"
                                        "D1     d  0  1N914\n"
                                        "D2     0  d  1N914\n"
                                        "R2     a  c  470\n"
                                        "Ptone  c  b  25k {1 - tone}\n"
                                        "C2     b  0  22n\n"
                                        ".end\n";

    // A program of this kind whose main module declares `params` (lines of its own) and holds `fields` of netlist
    // modules (`declaration` each: "Screamer amp[2]"), never calling set(): an Effect running its left input through
    // each field (an array's first element) and its right through each array's second (else the left's copy), a Voice
    // a saw through each into both channels, its circuits cleared at a note's start.
    std::string undriven(const std::string& include, const std::string& params, const std::vector<std::string>& fields)
    {
        std::string decl, init, clear, left = "x", right = "y";
        bool stereo = true;
        for (const std::string& f : fields)
        {
            decl += "    " + f + ";\n";
            const std::string name = f.substr(f.find(' ') + 1);
            const std::string base = name.substr(0, name.find('['));
            const bool array = name.find('[') != std::string::npos;
            const std::string first = array ? base + "[0]" : base;
            init += "        " + first + ".init(rate);\n";
            clear += "        " + first + ".clear();\n";
            left = first + ".tick(" + left + ")";
            if (array)
            {
                init += "        " + base + "[1].init(rate);\n";
                clear += "        " + base + "[1].clear();\n";
                right = base + "[1].tick(" + right + ")";
            }
            else
                stereo = false;
        }
        std::string s = "#include \"" + include + "\"\n";
        if constexpr (kInstrument)
            s += "module Voice\n{\n" + params + decl + "    float phase;\n    float step;\n    float env;\n    bool held;\n\n"
                 "    void init(float rate)\n    {\n" + init + "    }\n\n"
                 "    void start(float note, float velocity)\n    {\n        step = 440 * expf(0.0577622650f * (note - 69)) / 48000;\n"
                 "        phase = 0;\n        env = velocity;\n        held = true;\n" + clear + "    }\n\n"
                 "    void stop()\n    {\n        held = false;\n    }\n\n"
                 "    bool process(float* restrict left, float* restrict right, int n)\n    {\n        for (int i = 0; i < n; ++i)\n"
                 "        {\n            const float x = 0.5f * (2 * phase - 1);\n            phase += step;\n"
                 "            if (phase >= 1)\n                phase -= 1;\n            if (!held)\n                env *= 0.9995f;\n"
                 "            const float out = env * " + left + ";\n            left[i] += out;\n            right[i] += out;\n"
                 "        }\n        return held || env > 0.0001f;\n    }\n};\n";
        else
            s += "module Effect\n{\n" + params + decl + "\n    void init(float rate)\n    {\n" + init + "    }\n\n"
                 "    void process(float* restrict left, float* restrict right, int n)\n    {\n        for (int i = 0; i < n; ++i)\n"
                 "        {\n            const float x = left[i];\n" + (stereo ? "            const float y = right[i];\n" : "")
                 + "            left[i] = " + left + ";\n            right[i] = " + (stereo ? right : "left[i]") + ";\n        }\n    }\n};\n";
        return s;
    }

    // The controls' names in slot order, the used ones.
    std::vector<std::string> dialNames(const pg::Controls& cs)
    {
        std::vector<std::string> out;
        for (const pg::Control& c : cs)
            if (c.used)
                out.push_back(c.name);
        return out;
    }

    std::string joined(const std::vector<std::string>& v)
    {
        std::string s;
        for (const std::string& x : v)
            s += (s.empty() ? "" : " ") + x;
        return s;
    }

    // A field of the program that plays (the field map's path), in instance i: its value, NaN when there is none.
    double fieldOf(const pg::Engine& e, const std::string& path, int i)
    {
        const pg::Patch* p = e.playingPatch();
        if (p == nullptr)
            return std::nan("");
        for (const pg::Patch::Field& f : p->fieldMap())
            if (f.path == path)
                return p->read(f, i);
        return std::nan("");
    }

    // Writes a NaN into a float field of instance i of the program that plays (offline, between blocks): a planted NaN.
    bool plantNaN(pg::Engine& e, const std::string& path, int i)
    {
        const pg::Patch* p = e.playingPatch();
        if (p == nullptr || p->instance(i) == nullptr)
            return false;
        for (const pg::Patch::Field& f : p->fieldMap())
            if (f.path == path && f.type == pg::Patch::Field::Type::real && f.home == pg::Patch::Field::Home::instance)
            {
                const float nan = std::nanf("");
                std::memcpy(static_cast<char*>(const_cast<void*>(p->instance(i))) + f.offset, &nan, sizeof nan);
                return true;
            }
        return false;
    }

    void dialRows(funkgui::test::Probe& P)
    {
        const std::string screamer = libraryText("screamer.cir");
        const std::string volume = "    param float volume [[fminus::range(0, 1)]] [[fminus::default(0.8)]];\n";
        const std::string field = kInstrument ? "Screamer amp" : "Screamer amp[2]";
        const pg::Sources files = { { pg::kProgramName, undriven("screamer.cir", volume, { field }) }, { "screamer.cir", screamer } };
        const pg::Patch::Result c = pg::Patch::compile(files, 48000.0f);
        std::printf("dials: %s%s", c.errors.c_str(), c.notes.c_str());
        P.eq("dials.compiles", c.patch != nullptr && c.errors.empty() && c.notes.empty() ? 1 : 0, 1);
        if (!c.patch)
        {
            std::printf("%s", files[0].text.c_str());
            return;
        }
        const pg::Controls& cs = c.patch->controls();
        std::printf("dials: %s\n", joined(dialNames(cs)).c_str());
        P.eq("dials.after_params", joined(dialNames(cs)) == "volume drive tone level" ? 1 : 0, 1);
        bool percent = true;
        for (int k = 1; k <= 3; ++k)
        {
            const pg::Control& d = cs[static_cast<std::size_t>(k)];
            char text[48];
            pg::formatValue(d, d.def, text, sizeof text);
            percent = percent && d.type == pg::ControlType::real && d.min == 0.0f && d.max == 1.0f && d.scale == 100.0f
                   && std::string(d.unit) == "%" && d.taper == pg::ControlTaper::linear;
            std::printf("  %s: default %s\n", d.name, text);
        }
        P.eq("dials.percent_linear", percent ? 1 : 0, 1);
        P.eq("dials.pot_defaults", cs[1].def == 0.5f && cs[2].def == 0.5f && cs[3].def == 0.7f ? 1 : 0, 1);
        const std::vector<pg::lang::CircuitBinding>& bs = c.patch->circuitBindings();
        P.eq("dials.one_set", bs.size() == 1 && bs[0].field == "amp" && bs[0].module == "Screamer" && bs[0].file == "screamer.cir"
                                  && bs[0].count() == (kInstrument ? 1 : 2) && bs[0].slots == std::vector<int>{ 1, 2, 3 } && !bs[0].own ? 1 : 0, 1);
        const std::string& src = c.patch->recipe().source;
        // the whole shim: every element's set() (both of the effect's amp[2])
        const std::string shim = std::string("void playground_circuit_0(") + (kInstrument ? "Voice" : "Effect")
                               + "* self, float* restrict v) [[clang::nonblocking]] { "
                               + (kInstrument ? "" : "for (int i0 = 0; i0 < 2; ++i0) ") + "self->amp" + (kInstrument ? "" : "[i0]")
                               + ".set(v[0], v[1], v[2]); }";
        P.eq("dials.shim_nonblocking", has(src, shim) && !has(src, "playground_circuit_1(") && c.patch->circuits()
                                           && c.patch->circuitSlots() == 0xEu ? 1 : 0, 1);

        // A name another dial has: <field>_<pot>; two scalar fields: two sets.
        {
            const std::string drive = "    param float drive [[fminus::range(0, 1)]] [[fminus::default(0.25)]];\n";
            const pg::Patch::Result clash = pg::Patch::compile({ { pg::kProgramName, undriven("screamer.cir", drive, { "Screamer amp" }) },
                                                                 { "screamer.cir", screamer } }, 48000.0f);
            const pg::Patch::Result two = pg::Patch::compile({ { pg::kProgramName, undriven("screamer.cir", "", { "Screamer a", "Screamer b" }) },
                                                               { "screamer.cir", screamer } }, 48000.0f);
            const std::string a = clash.patch ? joined(dialNames(clash.patch->controls())) : clash.errors;
            const std::string b = two.patch ? joined(dialNames(two.patch->controls())) : two.errors;
            std::printf("a clash: %s\ntwo fields: %s\n", a.c_str(), b.c_str());
            P.eq("dials.clash_field_pot", a == "drive amp_drive tone level" ? 1 : 0, 1);
            P.eq("dials.two_fields_two_sets", b == "a_drive a_tone a_level b_drive b_tone b_level" ? 1 : 0, 1);
        }
        // 31 params: drive takes the 32nd slot, the note covers tone and level.
        {
            std::string params;
            for (int k = 0; k < 31; ++k)
                params += "    param float p" + std::to_string(k) + " [[fminus::range(0, 1)]] [[fminus::default(0)]];\n";
            const pg::Patch::Result full = pg::Patch::compile({ { pg::kProgramName, undriven("screamer.cir", params, { "Screamer amp" }) },
                                                                { "screamer.cir", screamer } }, 48000.0f);
            std::printf("31 params: %s%s", full.errors.c_str(), full.notes.c_str());
            P.eq("dials.past_32_note", full.patch != nullptr && std::string(full.patch->controls()[31].name) == "drive"
                                           && full.notes == "program.fm: note: amp's pot tone has no dial (the plugin has 32): it stays at "
                                                            "its default\nprogram.fm: note: amp's pot level has no dial (the plugin has 32): "
                                                            "it stays at its default\n" ? 1 : 0, 1);
        }

        // 2 s, a pot gliding from sample 21312 (a block boundary of both: 64 x 333; the Engine starts a program on the
        // dials' values, so no glide starts at its first sample) and another at 42624: the same bits in blocks of 64 and
        // of 333.
        const std::size_t second = static_cast<std::size_t>(kFs);
        {
            const auto glide = [](std::size_t at, std::array<float, pg::kMaxControls>& n) {
                if (at == 21312)
                    n[1] = 1.0f;                         // drive up
                if (at == 42624)
                    n[2] = 0.1f;                         // tone down
            };
            probe::Rig a(kFs, 512), b(kFs, 512);
            a.load(files);
            b.load(files);
            const probe::Stereo in = input(2 * second);
            const probe::Stereo o64 = a.run(in, 64, chord(), {}, glide), o333 = b.run(in, 333, chord(), {}, glide);
            std::printf("glides: %llu and %llu calls in blocks of 64 and 333\n", static_cast<unsigned long long>(a.engine().circuitCalls()),
                        static_cast<unsigned long long>(b.engine().circuitCalls()));
            P.eq("dials.glide_64_333_same_bits", same(o64, o333) && finite(o64) && probe::rmsDb(o64.l, 0, second) > -80.0 ? 1 : 0, 1);
            P.eq("dials.glide_same_calls", a.engine().circuitCalls() == b.engine().circuitCalls() && a.engine().circuitCalls() > 60 ? 1 : 0, 1);
            // Still: no call.
            const uint64_t before = a.engine().circuitCalls();
            a.run(input(second / 2), 64, kInstrument ? std::vector<probe::Note>{ { 0, pg::EventKind::noteOn, 60, 0.8f } } : std::vector<probe::Note>{});
            const uint64_t fresh = a.engine().circuitCalls() - before;   // the synth's new note: on an instance freed after
                                                                          // the windows closed, whose values are current
            const uint64_t after = a.engine().circuitCalls();
            a.run(input(second / 2), 64);
            std::printf("still: %llu calls, then %llu\n", static_cast<unsigned long long>(fresh),
                        static_cast<unsigned long long>(a.engine().circuitCalls() - after));
            P.eq("dials.still_no_call", a.engine().circuitCalls() == after && fresh == 0 ? 1 : 0, 1);
        }
        if constexpr (kInstrument)
        {
            // A note in a block of 0 samples right after a block in which a glide landed (drive from sample 512, 20 ms:
            // it lands at 1472, its window closing at 1488): its first call has the landed value, as the same note at
            // the next block's first sample has. The same bits.
            const auto glide = [](std::size_t at, std::array<float, pg::kMaxControls>& n) {
                if (at == 512)
                    n[1] = 1.0f;
            };
            probe::Rig a(kFs, 512), b(kFs, 512);
            a.load(files);
            b.load(files);
            a.run(input(1536), 512, {}, {}, glide);
            b.run(input(1536), 512, {}, {}, glide);
            pg::Event on;
            on.offset = 0;
            on.kind = pg::EventKind::noteOn;
            on.note = 60;
            on.velocity = 0.8f;
            float none[2] = { 0.0f, 0.0f };
            float* channels[2] = { none, none + 1 };
            a.engine().process(channels, 2, 0, std::span<const pg::Event>(&on, 1));
            const probe::Stereo oa = a.run(input(second / 2), 512);
            const probe::Stereo ob = b.run(input(second / 2), 512, { { 0, pg::EventKind::noteOn, 60, 0.8f } });
            std::printf("a note in a block of 0 samples after a glide landed: %s the note at the next block's start (%.1f dB apart)\n",
                        same(oa, ob) ? "the bits of" : "NOT", differenceDb(oa, ob));
            P.eq("dials.zero_block_note_landed_value", same(oa, ob) && finite(oa) && probe::rmsDb(oa.l, 0, oa.size()) > -80.0 ? 1 : 0, 1);
        }

        // The frozen copy plays with the pots where they are, and a pot that moves drops it.
        {
            const auto set = [](std::size_t at, std::array<float, pg::kMaxControls>& n) {
                if (at == 0)
                {
                    n[1] = 0.9f;
                    n[3] = 0.3f;
                }
            };
            probe::Rig live(kFs, 256), fast(kFs, 256);
            live.load(files);
            fast.load(files);
            const std::vector<probe::Note> held = kInstrument ? std::vector<probe::Note>{ { 0, pg::EventKind::noteOn, 48, 0.8f } }
                                                              : std::vector<probe::Note>{};
            const probe::Stereo warm = input(second / 4);
            live.run(warm, 256, held, {}, set);
            fast.run(warm, 256, held, {}, set);
            live.run(warm, 256);                         // still: the glides landed
            fast.run(warm, 256);
            std::string why;
            const bool made = fast.freeze(&why);
            const probe::Stereo rest = input(second / 2);
            const probe::Stereo ol = live.run(rest, 256), of = fast.run(rest, 256);
            const bool playing = fast.engine().frozen();
            const double d = differenceDb(ol, of);
            std::printf("frozen at drive 90 %%, level 30 %%: %s, the difference %.1f dB\n", made ? "made" : why.c_str(), d);
            P.eq("dials.frozen_plays", made && playing ? 1 : 0, 1);
            P.le("dials.frozen_pots_at_values_db", d, -80.0);
            fast.run(input(256), 256, {}, {}, [](std::size_t, std::array<float, pg::kMaxControls>& n) { n[2] = 0.8f; });
            P.eq("dials.pot_move_drops_frozen", !fast.engine().frozen() ? 1 : 0, 1);
        }

        // ---- OWN: a program that calls the module's set() keeps driving it --------------------------------------------
        for (const pg::Example& e : pg::kExamples)
        {
            const std::string text = e.source;
            const bool screamerEx = has(text, "#include \"screamer.cir\""), ota = has(text, "#include \"otalp.cir\"");
            if (!screamerEx && !ota)
                continue;
            const pg::Sources ex = pg::withLibraryIncludes(pg::singleFile(text));
            const pg::Patch::Result r = pg::Patch::compile(ex, 48000.0f);
            pg::lang::Service ls;
            ls.setFiles(ex);
            const pg::lang::CircuitDials d = pg::lang::circuitDials(ls, kInstrument);
            const std::string own = r.patch ? joined(dialNames(r.patch->controls())) : r.errors;
            std::printf("%s: dials %s; notes: %s\n", e.title, own.c_str(), r.notes.empty() ? "none" : r.notes.c_str());
            const std::string key = std::string("own.") + (screamerEx ? "tube_screamer" : "ota_filter");
            P.eq(key + ".dials_kept", r.patch != nullptr && own == joined(d.params) && r.notes.empty() && r.errors.empty() ? 1 : 0, 1);
            if (!r.patch)
                continue;
            const std::vector<pg::lang::CircuitBinding>& bs2 = r.patch->circuitBindings();
            std::string whys;
            for (const pg::lang::CircuitBinding& b : bs2)
                whys += b.field + (b.own ? " own: " + b.why : std::string(" not own")) + "; ";
            std::printf("  bindings: %s\n", whys.c_str());
            if (screamerEx)
            {
                const std::string note = "program.fm:" + std::to_string(lineOf(text, "                pedalL.set(")) + ": Screamer's pots have "
                                                                                                                     "no dials: the program calls its set()";
                P.eq(key + ".note", bs2.size() == 2 && bs2[0].own && bs2[1].own && bs2[0].why == note && bs2[1].why == note
                                        && bs2[0].slots == std::vector<int>{ -1, -1, -1 } ? 1 : 0, 1);
            }
            else
                P.eq(key + ".no_pots_no_note", bs2.size() == 1 && bs2[0].field == "filter" && bs2[0].module == "OtaLp" && !bs2[0].own
                                                   && bs2[0].args() == 0 && bs2[0].why.empty() && !r.patch->circuits() ? 1 : 0, 1);
        }
        {
            // A set() whose receiver Lang cannot type counts too.
            const std::string program = replaced(undriven("screamer.cir", "", { "Screamer amp" }), "    void init(float rate)\n    {\n",
                                                 "    void init(float rate)\n    {\n        mystery.set(0.5f);\n");
            pg::lang::Service ls;
            ls.setFiles({ { pg::kProgramName, program }, { "screamer.cir", screamer } });
            const pg::lang::CircuitDials d = pg::lang::circuitDials(ls, kInstrument);
            std::printf("unknown receiver: %s\n", d.bindings.empty() ? "no binding" : d.bindings[0].why.c_str());
            P.eq("own.unknown_receiver", d.bindings.size() == 1 && d.bindings[0].own && d.bindings[0].names.empty()
                                             && has(d.bindings[0].why, "Screamer's pots have no dials: the program calls mystery.set()") ? 1 : 0, 1);
        }
        {
            // Only the files FMinus compiles: a copy of the netlist no #include reaches (the same module, before the
            // one included) and a tab of the main module of its own make no binding.
            const std::string program = replaced(undriven("screamer.cir", "", { field }), "#include \"screamer.cir\"",
                                                 "#include \"mine.cir\"");
            const pg::Sources tabs = { { pg::kProgramName, program }, { "screamer.cir", screamer }, { "mine.cir", screamer },
                                       { "stray.fm", undriven("screamer.cir", "", { field }) } };
            pg::lang::Service ls;
            ls.setFiles(tabs);
            const pg::lang::CircuitDials d = pg::lang::circuitDials(ls, kInstrument);
            const pg::Patch::Result r = pg::Patch::compile(tabs, 48000.0f);
            const std::string names = r.patch ? joined(dialNames(r.patch->controls())) : r.errors;
            std::printf("tabs no #include reaches: %zu binding%s (%s); dials %s; notes: %s\n", d.bindings.size(),
                        d.bindings.size() == 1 ? "" : "s", d.bindings.empty() ? "" : d.bindings[0].file.c_str(), names.c_str(),
                        r.notes.empty() ? "none" : r.notes.c_str());
            P.eq("own.only_what_compiles", d.bindings.size() == 1 && d.bindings[0].file == "mine.cir" && r.patch != nullptr
                                               && names == "drive tone level" && r.notes.empty() && r.errors.empty() ? 1 : 0, 1);
        }
        {
            // A pot's default read whatever the process's locale (a host's LC_NUMERIC may want a decimal comma).
            const std::string was = std::setlocale(LC_NUMERIC, nullptr) != nullptr ? std::setlocale(LC_NUMERIC, nullptr) : "C";
            const char* comma = nullptr;
            for (const char* name : { "de_DE.UTF-8", "de_DE", "fr_FR.UTF-8", "fr_FR" })
                if (comma == nullptr && std::setlocale(LC_NUMERIC, name) != nullptr)
                    comma = name;
            const std::optional<double> v = pg::lang::spiceNumber("0.3");
            pg::lang::Service ls;
            ls.setFiles({ { pg::kProgramName, undriven("clipswitch.cir", "", { "ClipSwitch clip" }) },
                          { "clipswitch.cir", replaced(kClipSwitch, ".pot    tone 0.5", ".pot    tone 0.3") } });
            const pg::lang::CircuitDials d = pg::lang::circuitDials(ls, kInstrument);
            std::setlocale(LC_NUMERIC, was.c_str());
            std::printf("in the locale %s: 0.3 reads %g, tone's default %g\n", comma != nullptr ? comma : "C (no comma locale here)",
                        v.value_or(-1.0), d.bindings.empty() || d.bindings[0].pots.empty() ? -1.0 : static_cast<double>(d.bindings[0].pots[0].def));
            P.eq("own.number_any_locale", v && *v == 0.3 && d.bindings.size() == 1 && d.bindings[0].pots.size() == 1
                                              && d.bindings[0].pots[0].def == 0.3f ? 1 : 0, 1);
        }

        // ---- TEMPLATE: Play this circuit's program for each library netlist (fminus/CirTemplates.h) ---------------------
        for (const pg::LibraryFile& f : pg::kLibrary)
        {
            if (!pg::isCircuitFile(f.name))
                continue;
            const pg::cir::HostProgram hp = pg::cir::hostProgram(f.name, f.source, kInstrument);
            const pg::Sources play = pg::withLibraryIncludes({ { pg::kProgramName, hp.text }, { f.name, f.source } });
            probe::Rig rig(kFs, 512);
            std::string messages;
            const bool loaded = rig.load(play, &messages);
            const probe::Stereo out = loaded ? rig.run(input(second), 256, chord()) : probe::Stereo(1);
            std::printf("Play %s: %s; %s, %.1f dBFS%s\n", f.name, hp.module.c_str(), joined(dialNames(rig.controls())).c_str(),
                        probe::rmsDb(out.l, 0, out.size()), messages.empty() ? "" : ("; " + messages).c_str());
            const std::string key = std::string("template.") + std::string(f.name).substr(0, std::strlen(f.name) - 4);
            P.eq(key + ".no_message", loaded && hp.why.empty() && messages.empty() ? 1 : 0, 1);
            P.eq(key + ".plays", loaded && finite(out) && probe::rmsDb(out.l, 0, out.size()) > -80.0 ? 1 : 0, 1);
            const bool own = std::any_of(rig.recipe().bindings.begin(), rig.recipe().bindings.end(),
                                         [](const pg::lang::CircuitBinding& b) { return b.own; });
            P.eq(key + ".pots_dials", loaded && !own && rig.recipe().circuitShims == (has(f.source, ".pot") ? 1 : 0) ? 1 : 0, 1);
            P.eq(key + ".house_style", !pg::lang::format(hp.text).changed && pg::lang::format(hp.text).ok ? 1 : 0, 1);
        }
        {
            // a two-channel netlist: one module over both channels
            const std::string two = replaced(libraryText("diodetone.cir"), ".title DiodeTone\n", ".title DiodeTone\n.channels 2\n");
            const pg::cir::HostProgram hp = pg::cir::hostProgram("stereo.cir", two, kInstrument);
            std::string messages;
            probe::Rig rig(kFs, 512);
            const bool loaded = rig.load(pg::withLibraryIncludes({ { pg::kProgramName, hp.text }, { "stereo.cir", two } }), &messages);
            const probe::Stereo out = loaded ? rig.run(input(second / 2), 256, chord()) : probe::Stereo(1);
            std::printf("Play a two-channel netlist: %s\n", messages.c_str());
            P.eq("template.two_channels", loaded && messages.empty() && has(hp.text, "(float2)(") && finite(out) ? 1 : 0, 1);
        }
        for (const char* law : { "rate", "i" })
        {
            // a law input named as one of the template's own words: its param gets a name of its own (<law>_in)
            std::string netlist = libraryText("otalp.cir");
            for (std::size_t at = netlist.find("iabc"); at != std::string::npos; at = netlist.find("iabc", at))
                netlist.replace(at, 4, law);
            const pg::cir::HostProgram hp = pg::cir::hostProgram("ota.cir", netlist, kInstrument);
            std::string messages;
            probe::Rig rig(kFs, 512);
            const bool loaded = rig.load(pg::withLibraryIncludes({ { pg::kProgramName, hp.text }, { "ota.cir", netlist } }), &messages);
            const probe::Stereo out = loaded ? rig.run(input(second / 2), 256, chord()) : probe::Stereo(1);
            const std::string dials = loaded ? joined(dialNames(rig.controls())) : "";
            std::printf("Play a netlist whose law is named %s: %s%s\n", law, dials.c_str(), messages.empty() ? "" : ("; " + messages).c_str());
            P.eq(std::string("template.law_named_") + law, loaded && messages.empty() && dials == std::string(law) + "_in" && finite(out)
                                                               && probe::rmsDb(out.l, 0, out.size()) > -80.0 ? 1 : 0, 1);
        }

        // ---- SWITCH: a switch dial keeps its position through clear() --------------------------------------------------
        {
            const pg::Sources sw = { { pg::kProgramName, undriven("clipswitch.cir", "", { "ClipSwitch clip" }) }, { "clipswitch.cir", kClipSwitch } };
            probe::Rig rig(kFs, 256);
            std::string why;
            const bool loaded = rig.load(sw, &why);
            std::printf("a switch: %s%s\n", loaded ? joined(dialNames(rig.controls())).c_str() : "", why.c_str());
            P.eq("switch.compiles", loaded && why.empty() && joined(dialNames(rig.controls())) == "tone clip"
                                        && rig.controls()[1].type == pg::ControlType::integer && rig.controls()[1].max == 1.0f ? 1 : 0, 1);
            if (!loaded)
                return;
            const std::vector<probe::Note> note = kInstrument ? std::vector<probe::Note>{ { 0, pg::EventKind::noteOn, 48, 0.8f } }
                                                              : std::vector<probe::Note>{};
            rig.run(input(second / 4), 256, note);       // the program's first calls, the ramp to the default
            const uint64_t still = rig.engine().circuitCalls();
            rig.run(input(second / 4), 256);
            const uint64_t flipped = rig.engine().circuitCalls();
            rig.run(input(second / 2), 256, {}, {}, [](std::size_t at, std::array<float, pg::kMaxControls>& n) {
                if (at == 0)
                    n[1] = 1.0f;
            });
            const uint64_t calls = rig.engine().circuitCalls() - flipped;
            const int want = static_cast<int>(std::ceil(0.02 * kFs / 16.0)) + 1;
            std::printf("a switch flipped: %llu calls (%d wanted), %llu while still before\n", static_cast<unsigned long long>(calls), want,
                        static_cast<unsigned long long>(flipped - still));
            P.eq("switch.still_no_call", static_cast<int64_t>(flipped - still), 0);
            P.eq("switch.change_calls", static_cast<int64_t>(calls), want);
            P.eq("switch.at_1", fieldOf(rig.engine(), "clip.sw_S1", 0) == 1.0 ? 1 : 0, 1);
            if constexpr (kInstrument)
            {
                // the note let go and ended (its voice freed, the window long closed), then another from silence on the
                // same instance: its start's clear() keeps the switch at 1, and no call comes
                rig.run(input(3 * second), 256, { { 0, pg::EventKind::noteOff, 48, 0.0f } });
                const uint64_t quiet = rig.engine().circuitCalls();
                rig.run(input(second / 4), 256, { { 0, pg::EventKind::noteOn, 50, 0.8f } });
                std::printf("a note from silence: switch %g, %llu calls\n", fieldOf(rig.engine(), "clip.sw_S1", 0),
                            static_cast<unsigned long long>(rig.engine().circuitCalls() - quiet));
                P.eq("switch.note_from_silence", rig.engine().activeVoices() == 1 && fieldOf(rig.engine(), "clip.sw_S1", 0) == 1.0
                                                     && rig.engine().circuitCalls() == quiet ? 1 : 0, 1);
            }
            {
                // a fresh instance (a program's first block, a note's first part) starts at the dial's position at once
                probe::Rig fresh(kFs, 256);
                fresh.load(sw);
                fresh.norm()[1] = 1.0f;
                fresh.run(input(64), 64, note);
                std::printf("a fresh instance, the switch's dial at 1: %g after 64 samples, %llu calls\n", fieldOf(fresh.engine(), "clip.sw_S1", 0),
                            static_cast<unsigned long long>(fresh.engine().circuitCalls()));
                P.eq("switch.fresh_at_position", fieldOf(fresh.engine(), "clip.sw_S1", 0) == 1.0
                                                     && static_cast<int>(fresh.engine().circuitCalls()) == want ? 1 : 0, 1);
            }
            {
                // A ramp's worth of first calls only for an instance whose switch is not where the dial is: a program's
                // first notes with the switch at its default take one call each; so do notes on instances that missed
                // a pot's move; after the switch moved while they were free, a ramp's worth each, at its position.
                probe::Rig r(kFs, 256);
                r.load(sw);
                if constexpr (kInstrument)
                {
                    std::vector<probe::Note> eight, off;
                    for (int k = 0; k < 8; ++k)
                    {
                        eight.push_back({ 0, pg::EventKind::noteOn, 48 + 2 * k, 0.8f });
                        off.push_back({ 0, pg::EventKind::noteOff, 48 + 2 * k, 0.0f });
                    }
                    const auto chordCalls = [&]() {
                        const uint64_t was = r.engine().circuitCalls();
                        r.run(input(64), 64, eight);
                        return static_cast<int64_t>(r.engine().circuitCalls() - was);
                    };
                    const int64_t first = chordCalls();
                    r.run(input(second), 256, off);      // let go: they end, every window long closed
                    r.run(input(second / 2), 256, {}, {}, [](std::size_t at, std::array<float, pg::kMaxControls>& n) {
                        if (at == 0)
                            n[0] = 0.9f;             // tone, no voice sounding: every instance misses it
                    });
                    const int64_t afterPot = chordCalls();
                    r.run(input(second), 256, off);
                    r.run(input(second / 2), 256, {}, {}, [](std::size_t at, std::array<float, pg::kMaxControls>& n) {
                        if (at == 0)
                            n[1] = 1.0f;             // the switch to 1, no voice sounding
                    });
                    const int64_t afterSwitch = chordCalls();
                    std::vector<pg::Engine::VoiceInfo> vs;
                    r.engine().voices(vs);
                    bool at1 = vs.size() == 8;
                    for (const pg::Engine::VoiceInfo& v : vs)
                        at1 = at1 && fieldOf(r.engine(), "clip.sw_S1", v.instance) == 1.0;
                    std::printf("eight notes: %lld calls at the default, %lld after a pot moved, %lld after the switch moved (%s at 1)\n",
                                static_cast<long long>(first), static_cast<long long>(afterPot), static_cast<long long>(afterSwitch),
                                at1 ? "all" : "NOT all");
                    P.eq("switch.first_calls_at_default", first, 8);
                    P.eq("switch.first_calls_after_pot", afterPot, 8);
                    P.eq("switch.first_calls_after_switch", afterSwitch == 8 * want && at1 ? 1 : 0, 1);
                }
                else
                {
                    r.run(input(64), 64);
                    P.eq("switch.first_calls_at_default", static_cast<int64_t>(r.engine().circuitCalls()), 1);
                }
            }
            // a NaN planted in a carried field: the circuit's guard clears it, and the switch stays
            const bool planted = plantNaN(rig.engine(), "clip.x_C1", 0);
            const uint64_t before = rig.engine().circuitCalls();
            rig.run(input(256), 64);
            const double x = fieldOf(rig.engine(), "clip.x_C1", 0), s = fieldOf(rig.engine(), "clip.sw_S1", 0);
            std::printf("a planted NaN: x_C1 %g after, switch %g\n", x, s);
            P.eq("switch.nan_guard_keeps", planted && std::isfinite(x) && s == 1.0 && rig.engine().circuitCalls() == before ? 1 : 0, 1);
        }
    }

    // SELFTEST, REFRESH and DEDUP (above): the plugin's self-test, a Compiler's.
    void selfTester(funkgui::test::Probe& P)
    {
        pg::circuits::Answers& store = pg::circuits::answers();
        const pg::Sources files = clipperFiles();
        const std::string key = fminus::circuit::selfTestKey(probe::kAutoClipper, "clipper.cir", {});
        std::string answer;                              // the clipper's, from SELFTEST

        // ---- SELFTEST -------------------------------------------------------------------------------------------------
        {
            store.clearForTests();
            Live live(false);
            live.compiler->request(files);
            live.compiler->wait();
            const pg::CompileStatus a = live.compiler->status();
            live.play(8);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));   // time a test would start in, were they on
            std::printf("selftest: the provisional RUN: %s%s", a.errors.c_str(), a.notes.c_str());
            P.eq("selftest.off_plays_provisional", a.ok && has(a.notes, "clipper.cir: note: the self-test chooses this circuit's configuration; provisional: ") ? 1 : 0, 1);
            P.eq("selftest.off_no_test", live.compiler->selfTests() == 0 && !store.inFlight(key) && !store.find(key)
                                             && live.compiler->selfTesting() ? 1 : 0, 1);
            const uint64_t landed = store.landed();
            const bool refined = live.compiler->selfTestNow();
            const pg::CompileStatus b = live.compiler->status();
            const pg::Meters m = live.play(8);
            answer = store.find(key).value_or("");
            std::printf("selftest: refined %d, the answer %s\n  its status: %s%s", refined ? 1 : 0, answer.c_str(), b.errors.c_str(), b.notes.c_str());
            P.eq("selftest.now_tests_and_refines", refined && live.compiler->selfTests() == 1 && store.landed() == landed + 1 ? 1 : 0, 1);
            P.eq("selftest.revision_program_advance", b.revision == a.revision + 1 && b.refined && b.request == a.request
                                                          && b.program > a.program ? 1 : 0, 1);
            P.eq("selftest.ok", b.ok && b.sources == files ? 1 : 0, 1);
            P.eq("selftest.meters_program", m.program == b.program ? 1 : 0, 1);
            const std::string note = pg::circuits::chosenNote("clipper.cir", answer);
            P.eq("selftest.note_names_choice", !note.empty() && has(b.notes, note) && !has(b.notes, "provisional") ? 1 : 0, 1);
            const pg::CompileStatus c = live.compiler->compileNow(files);
            P.eq("selftest.again_reads_store", c.ok && c.notes.empty() && !live.compiler->selfTesting() && live.compiler->selfTests() == 1
                                                   ? 1 : 0, 1);
        }
        if (answer.empty())
            return;

        // ---- REFRESH ----------------------------------------------------------------------------------------------------
        // After a RUN that does not compile, the refine compiles the files that play, and the status still says why the
        // RUN failed.
        {
            store.clearForTests();
            Live live(false);
            const pg::CompileStatus a = live.compiler->compileNow(files);
            live.play(1);
            const pg::Sources provisional = live.engine.playingPatch() != nullptr ? live.engine.playingPatch()->recipe().circuits
                                                                                  : pg::Sources{};
            pg::Sources broken = files;
            broken[0].text += "module {\n";
            live.compiler->request(broken);
            live.compiler->wait();
            const pg::CompileStatus bad = live.compiler->status();
            const bool refined = live.compiler->selfTestNow();
            const pg::CompileStatus s = live.compiler->status();
            const pg::Meters m = live.play(8);
            const pg::Patch* playing = live.engine.playingPatch();
            const pg::Patch::Result fresh = pg::Patch::compile(files, static_cast<float>(kFs));   // with the answer stored
            std::printf("refresh: after a broken RUN: refined %d, %s%s", refined ? 1 : 0, s.errors.c_str(), s.notes.c_str());
            P.eq("refresh.failed_run_keeps_errors", a.ok && !bad.ok && !bad.errors.empty() && refined && s.refined && !s.ok
                                                        && s.request == bad.request && s.errors == bad.errors && s.sources == broken
                                                        && chose(s) ? 1 : 0, 1);
            P.eq("refresh.failed_run_refines_what_plays", s.program > bad.request && m.program == s.program && playing != nullptr
                                                              && playing->sources() == files && fresh.patch != nullptr
                                                              && playing->recipe().circuits == fresh.patch->recipe().circuits
                                                              && !provisional.empty() && !(provisional == playing->recipe().circuits)
                                                              ? 1 : 0, 1);
        }
        // A refine that may not play once it is compiled (a recording armed, an instrument's key pressed while it
        // compiled) is held, and offered at the first pass that may, without compiling again.
        {
            store.clearForTests();
            Live live(false);
            const pg::CompileStatus a = live.compiler->compileNow(files);
            store.store(key, answer);                    // the answer lands: a refine is due
            std::atomic<bool> stop{ false }, armed{ false };
            std::thread watcher([&] {
                // Once the refine compiles: a recording armed, and an instrument's key pressed (as the audio thread).
                if (until([&] { return stop.load() || live.compiler->busy(); }, 30.0) && !stop.load())
                {
                    live.engine.setOwnCode(true);
                    if constexpr (kInstrument)
                        live.play(1, { pg::Event{ 0, pg::EventKind::noteOn, 60, 0.8f } });
                    armed = true;
                }
            });
            const uint64_t before = pg::circuits::generations();
            const bool offered = live.compiler->selfTestNow();
            stop = true;
            watcher.join();
            const uint64_t built = pg::circuits::generations();
            const pg::Meters m = live.play(8);
            std::printf("refresh: armed while the refine compiled: armed %d, offered %d\n", armed.load() ? 1 : 0, offered ? 1 : 0);
            P.eq("refresh.held_once_compiled", a.ok && armed.load() && !offered && built > before && live.compiler->revision() == a.revision
                                                   && m.program == a.program && live.compiler->selfTesting() ? 1 : 0, 1);
            live.engine.setOwnCode(false);
            pg::Meters q = live.play(1, kInstrument ? std::vector<pg::Event>{ pg::Event{ 0, pg::EventKind::noteOff, 60, 0.0f } }
                                                    : std::vector<pg::Event>{});
            for (int k = 0; k < 400 && (q.voices != 0 || q.fading != 0 || q.held != 0); ++k)
                q = live.play(1);
            const bool later = live.compiler->selfTestNow();
            const pg::CompileStatus s = live.compiler->status();
            P.eq("refresh.held_offered_later", later && pg::circuits::generations() == built && s.refined && s.ok && chose(s)
                                                   && s.revision == a.revision + 1 && live.play(8).program == s.program ? 1 : 0, 1);
        }
        // compileNow waits for a refine the compiler runs on another thread: its program, compiled last, plays.
        {
            store.clearForTests();
            Live live(false);
            const pg::CompileStatus a = live.compiler->compileNow(files);
            store.store(key, answer);
            std::thread refining([&] { live.compiler->selfTestNow(); });
            until([&] { return live.compiler->busy(); }, 30.0);
            const pg::Sources other = pg::singleFile(silentMain());
            const pg::CompileStatus b = live.compiler->compileNow(other);
            refining.join();
            live.compiler->wait();
            const pg::Meters m = live.play(8);
            const pg::CompileStatus s = live.compiler->status();
            const pg::Patch* playing = live.engine.playingPatch();
            P.eq("refresh.compile_now_plays_last", a.ok && b.ok && s.request == b.request && m.program == b.program && playing != nullptr
                                                       && playing->sources() == other && !live.compiler->selfTesting() ? 1 : 0, 1);
        }
        // While a recording is armed the refine waits; it comes when the recording ends.
        {
            store.clearForTests();
            Live live(false);
            const pg::CompileStatus a = live.compiler->compileNow(files);
            live.engine.setOwnCode(true);
            const bool during = live.compiler->selfTestNow();
            const pg::Meters m = live.play(8);
            P.eq("refresh.waits_while_recording", a.ok && !during && store.find(key) && live.compiler->revision() == a.revision
                                                      && m.program == a.request && live.compiler->selfTesting() ? 1 : 0, 1);
            live.engine.setOwnCode(false);
            const bool after = live.compiler->selfTestNow();
            const pg::CompileStatus s = live.compiler->status();
            P.eq("refresh.after_recording", after && s.ok && s.revision == a.revision + 1 && chose(s) && live.play(8).program == s.program
                                                ? 1 : 0, 1);
        }
        // A RUN queued before the refine wins: it reads the store itself, and the refine is dropped.
        {
            store.clearForTests();
            Live live(false);
            const pg::CompileStatus a = live.compiler->compileNow(files);
            store.store(key, answer);                    // another instance's answer lands: a refine is due
            const bool due = live.compiler->selfTesting();
            const uint64_t run = live.compiler->request(files);
            live.compiler->setSelfTesting(true);         // refines may run from here on
            live.compiler->wait();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));   // time a refine would come in, were it due
            const pg::CompileStatus s = live.compiler->status();
            const bool again = live.compiler->selfTestNow();
            std::printf("refresh: a RUN before the refine: %s\n", (s.errors + s.notes).empty() ? "no messages" : (s.errors + s.notes).c_str());
            P.eq("refresh.run_wins", a.ok && due && s.request == run && s.revision == a.revision + 1 && s.ok && s.notes.empty()
                                         && !again && live.compiler->revision() == s.revision && !live.compiler->selfTesting()
                                         && live.compiler->selfTests() == 0 ? 1 : 0, 1);
        }
        // An instrument refines only in silence: no voice sounding, fading or held.
        if constexpr (kInstrument)
        {
            store.clearForTests();
            Live live(false);
            const pg::CompileStatus a = live.compiler->compileNow(files);
            const pg::Meters held = live.play(8, { pg::Event{ 0, pg::EventKind::noteOn, 60, 0.8f } });
            const bool whileHeld = live.compiler->selfTestNow();
            const pg::Meters still = live.play(16);
            P.eq("refresh.synth_waits_while_held", a.ok && held.voices == 1 && held.held == 1 && !whileHeld && store.find(key)
                                                       && still.program == a.request && live.compiler->revision() == a.revision
                                                       ? 1 : 0, 1);
            pg::Meters m = live.play(1, { pg::Event{ 0, pg::EventKind::noteOff, 60, 0.0f } });
            const bool releasing = m.voices == 1 && m.held == 0;
            const bool sounding = live.compiler->selfTestNow();
            for (int k = 0; k < 400 && (m.voices != 0 || m.fading != 0 || m.held != 0); ++k)
                m = live.play(1);
            const bool silent = m.voices == 0 && m.fading == 0 && m.held == 0;
            const bool after = live.compiler->selfTestNow();
            const pg::CompileStatus s = live.compiler->status();
            const pg::Meters now = live.play(8);
            P.eq("refresh.synth_after_release", releasing && !sounding && silent && after && s.ok && chose(s) && now.program == s.program
                                                    ? 1 : 0, 1);
        }
        // With the tests on: a RUN whose files drop the netlist cancels its test, which stores nothing.
        const pg::Sources potFiles = clipperFiles(true);
        const std::string potKey = fminus::circuit::selfTestKey(probe::kPotClipper, "potclip.cir", {});
        {
            store.clearForTests();
            Live live(true);
            const uint64_t landed = store.landed();
            live.compiler->request(potFiles);
            live.compiler->wait();
            const bool started = until([&] { return store.inFlight(potKey) || store.find(potKey); }, 30.0);
            const bool running = store.inFlight(potKey);
            live.compiler->request(pg::singleFile(silentMain()));
            live.compiler->wait();
            const bool ended = until([&] { return !store.inFlight(potKey); }, 60.0);
            std::printf("refresh: dropped while testing: started %d, running %d, ended %d, %d test(s), stored %d\n", started ? 1 : 0,
                        running ? 1 : 0, ended ? 1 : 0, live.compiler->selfTests(), store.find(potKey) ? 1 : 0);
            P.eq("refresh.drop_cancels", started && running && ended && live.compiler->selfTests() == 1 && !live.compiler->selfTesting()
                                             && live.compiler->status().ok ? 1 : 0, 1);
            P.eq("refresh.drop_stores_nothing", !store.find(potKey) && store.landed() == landed ? 1 : 0, 1);
        }
        // With the tests on: a RUN made while the test runs has its status before the answer lands, and the refine
        // follows it.
        {
            store.clearForTests();
            Live live(true);
            live.compiler->request(potFiles);
            live.compiler->wait();
            const pg::CompileStatus a = live.compiler->status();
            const bool started = until([&] { return store.inFlight(potKey) || store.find(potKey); }, 30.0);
            const uint64_t landed = store.landed();
            const bool running = store.inFlight(potKey);
            const pg::Sources edited = clipperFiles(true, "// edited while the self-test runs\n");
            const uint64_t run = live.compiler->request(edited);
            uint64_t landedAtRun = 0;
            const bool answered = until([&] {
                if (live.compiler->status().request < run)
                    return false;
                landedAtRun = store.landed();
                return true;
            }, 60.0);
            const pg::CompileStatus b = live.compiler->status();
            const bool refined = until([&] {
                const pg::CompileStatus s = live.compiler->status();
                return s.program > run && chose(s);
            }, 60.0);
            const pg::CompileStatus s = live.compiler->status();
            std::printf("refresh: a RUN while the test runs: answered first %d (landed %llu then %llu); refined %d: %s", answered ? 1 : 0,
                        static_cast<unsigned long long>(landedAtRun - landed), static_cast<unsigned long long>(store.landed() - landed),
                        refined ? 1 : 0, s.notes.c_str());
            P.eq("refresh.async_provisional", a.ok && has(a.notes, "provisional: ") ? 1 : 0, 1);
            P.eq("refresh.async_run_before_landed", started && running && answered && b.request == run && b.ok && landedAtRun == landed
                                                        ? 1 : 0, 1);
            P.eq("refresh.async_refines_after", refined && s.ok && s.sources == edited && store.landed() == landed + 1 ? 1 : 0, 1);
        }

        // ---- DEDUP ------------------------------------------------------------------------------------------------------
        {
            store.clearForTests();
            Live x(false), y(false);
            const pg::CompileStatus a = x.compiler->compileNow(files);
            const pg::CompileStatus b = y.compiler->compileNow(files);
            x.compiler->setSelfTesting(true);
            y.compiler->setSelfTesting(true);
            const bool both = until([&] { return chose(x.compiler->status()) && chose(y.compiler->status()); }, 60.0);
            std::printf("dedup: tests %d and %d; both refined %d\n", x.compiler->selfTests(), y.compiler->selfTests(), both ? 1 : 0);
            P.eq("dedup.both_wanted", a.ok && b.ok && has(a.notes, "provisional: ") && has(b.notes, "provisional: ") ? 1 : 0, 1);
            P.eq("dedup.one_test", x.compiler->selfTests() + y.compiler->selfTests(), 1);
            P.eq("dedup.both_refine", both ? 1 : 0, 1);
        }

        // ---- GIVEUP -----------------------------------------------------------------------------------------------------
        // A test that gives no answer (here a budget of one step: it stops before its references) is not tried again: the
        // status says why where the provisional note was, the provisional configuration plays on, and a compile of the
        // same files wants no job and says why too.
        {
            store.clearForTests();
            Live live(false);
            live.compiler->setSelfTestBudget(1);
            const pg::CompileStatus a = live.compiler->compileNow(files);
            const bool refined = live.compiler->selfTestNow();
            const pg::CompileStatus b = live.compiler->status();
            const pg::Patch::Result again = pg::Patch::compile(files, static_cast<float>(kFs));
            std::printf("giveup: refined %d; the status: %s%s  a compile again: %s", refined ? 1 : 0, b.errors.c_str(), b.notes.c_str(),
                        again.notes.c_str());
            const std::string says = "clipper.cir: note: the self-test found no configuration for this circuit (the self-test stops "
                                     "before ";
            P.eq("giveup.provisional_first", a.ok && has(a.notes, "provisional: 8x IIR, omega(2)") ? 1 : 0, 1);
            P.eq("giveup.status_says_why", !refined && b.ok && b.revision == a.revision + 1 && b.program == a.program && has(b.notes, says)
                                               && has(b.notes, "), so the provisional one plays on: 8x IIR, omega(2)\n")
                                               && !has(b.notes, "chooses this circuit's configuration") ? 1 : 0, 1);
            P.eq("giveup.not_again", live.compiler->selfTests() == 1 && !live.compiler->selfTesting() && store.gaveUp(key) && !store.find(key)
                                         && !store.claim(key) ? 1 : 0, 1);
            P.eq("giveup.compile_says_why", again.patch != nullptr && again.circuitJobs.empty() && has(again.notes, says)
                                                && !has(again.notes, "chooses this circuit's configuration") ? 1 : 0, 1);
        }

        // ---- MISFIT -----------------------------------------------------------------------------------------------------
        // An answer stored with the netlist's key that the netlist cannot take (a damaged session's or project's: ADAA with
        // BDF2) neither fails the compile nor stays: the compile plays the provisional configuration and wants the test,
        // and the answer is erased, so the self-test may claim the key again.
        {
            store.clearForTests();
            std::optional<fminus::circuit::Choice> bad = fminus::circuit::Choice::parse(answer);
            const bool parsed = bad.has_value();
            if (bad)
            {
                bad->adaa = true;
                bad->method = fminus::circuit::Method::Bdf2;
            }
            const bool restored = parsed && pg::circuits::restore(key, bad->text());
            const pg::Patch::Result r = pg::Patch::compile(files, static_cast<float>(kFs));
            std::printf("misfit: restored %d; %s%s", restored ? 1 : 0, r.errors.c_str(), r.notes.c_str());
            P.eq("misfit.restored", restored ? 1 : 0, 1);
            P.eq("misfit.compiles_provisional", r.patch != nullptr && r.errors.empty() && has(r.notes, "provisional: 8x IIR, omega(2)")
                                                    && r.circuitJobs.size() == 1 && r.circuitJobs[0].key == key ? 1 : 0, 1);
            P.eq("misfit.erased", !store.find(key) && store.claim(key) ? 1 : 0, 1);
            store.release(key);
        }
        store.clearForTests();
    }
}

PROBE(fminus, circuit)
{
    pg::circuits::answers().clearForTests();
    const std::size_t second = static_cast<std::size_t>(kFs);
    std::vector<const pg::LibraryFile*> netlists;
    for (const pg::LibraryFile& f : pg::kLibrary)
        if (pg::isCircuitFile(f.name))
            netlists.push_back(&f);
    P.ge("library.netlists", static_cast<double>(netlists.size()), 3.0);

    for (const pg::LibraryFile* lib : netlists)
    {
        const std::string name = lib->name;
        const std::string key = std::string("netlist.") + name.substr(0, name.size() - 4);
        const fminus::circuit::Result gen = fminus::circuit::generate(lib->source, name);
        const std::string program = programFor(name, gen);
        const pg::Sources files = pg::withLibraryIncludes(pg::singleFile(program));
        std::printf("%s: module %s (%zu pots, %zu inputs); %s\n", name.c_str(), gen.module.c_str(), gen.pots.size(),
                    gen.inputs.size(), gen.configuration.c_str());

        // COMPILE
        const pg::Patch::Result c = pg::Patch::compile(files, 48000.0f);
        if (!c.errors.empty() || !c.notes.empty())
            std::printf("%s%s", c.errors.c_str(), c.notes.c_str());
        P.eq(key + ".compiles", c.patch != nullptr ? 1 : 0, 1);
        P.eq(key + ".no_messages", c.errors.empty() && c.notes.empty() ? 1 : 0, 1);
        P.eq(key + ".no_job", c.circuitJobs.empty() && c.circuitKeys.empty() ? 1 : 0, 1);
        if (!c.patch)
            continue;

        // BLOCKS
        probe::Rig a(kFs, 512), b(kFs, 512);
        a.load(files);
        b.load(files);
        const probe::Stereo in = input(2 * second);
        const probe::Stereo o64 = a.run(in, 64, chord()), o333 = b.run(in, 333, chord());
        const double level = probe::rmsDb(o64.l, 0, second);
        std::printf("  2 s: %.1f dBFS RMS over the first second\n", level);
        P.eq(key + ".blocks_64_333_same_bits", same(o64, o333) ? 1 : 0, 1);
        P.eq(key + ".finite", finite(o64) ? 1 : 0, 1);
        P.ge(key + ".sounds_db", level, -80.0);

        // FREEZE
        {
            probe::Rig live(kFs, 256), fast(kFs, 256), exact(kFs, 256);
            live.load(files);
            fast.load(files);
            exact.load(files);
            const probe::Stereo warm = input(4096);
            live.run(warm, 256, chord());
            fast.run(warm, 256, chord());
            exact.run(warm, 256, chord());
            std::string why;
            const bool madeFast = fast.freeze(&why);
            if (!madeFast)
                std::printf("  the frozen copy: %s\n", why.c_str());
            const bool madeExact = exact.freeze(&why, true);
            if (!madeExact)
                std::printf("  the frozen copy in exact math: %s\n", why.c_str());
            const probe::Stereo rest = input(second);
            const probe::Stereo ol = live.run(rest, 256), of = fast.run(rest, 256), oe = exact.run(rest, 256);
            const double d = differenceDb(ol, of);
            std::printf("  frozen: the difference %.1f dB; exact math %s\n", d, same(ol, oe) ? "the same bits" : "OTHER BITS");
            P.eq(key + ".freeze_made", madeFast && madeExact && fast.engine().frozen() && exact.engine().frozen() ? 1 : 0, 1);
            P.eq(key + ".freeze_exact_same_bits", same(ol, oe) ? 1 : 0, 1);
            P.eq(key + ".freeze_fast_finite", finite(of) ? 1 : 0, 1);
            P.le(key + ".freeze_fast_difference_db", d, -80.0);
        }

        // LANG: what the language service reads from the netlist is what the module FMinus compiled has.
        {
            lg::Service s;
            s.setFiles({ { pg::kProgramName, program }, { name, lib->source } });
            std::vector<std::string> names, fromNetlist;
            for (const lg::Symbol& x : s.symbols())
                if (x.kind == lg::Kind::method && x.container == gen.module && x.file == s.fileIndex(name))
                {
                    names.push_back(x.name);
                    fromNetlist.push_back(x.signature);
                }
            const std::vector<std::string> fromModule = declared(gen.source, gen.module, names);
            std::string errors;
            fminus::Program::Options o;
            o.optimisation = 0;
            o.fileName = name + ".fm";
            const std::unique_ptr<fminus::Program> compiled = fminus::Program::compile(gen.source, errors, o);
            const fminus::Program::Module* m = compiled ? compiled->module(gen.module) : nullptr;
            int found = 0;
            for (const std::string& n : names)
                found += m != nullptr && std::find(m->methods.begin(), m->methods.end(), gen.module + "::" + n) != m->methods.end() ? 1 : 0;
            std::string said;
            for (const std::string& sig : fromNetlist)
                said += (said.empty() ? "" : "; ") + sig;
            std::printf("  the language service: %s\n", said.c_str());
            const bool sets = !gen.pots.empty() || !gen.switches.empty();
            P.eq(key + ".lang_methods", static_cast<int64_t>(names.size()), sets ? 4 : 3);
            P.eq(key + ".lang_compiled", found, static_cast<int64_t>(names.size()));
            P.eq(key + ".lang_signatures", fromNetlist == fromModule ? 1 : 0, 1);
        }
    }

    // ---- TRACE --------------------------------------------------------------------------------------------------------
    {
        const fminus::circuit::Result gen = fminus::circuit::generate(libraryText("screamer.cir"), "screamer.cir");
        const pg::Sources files = pg::withLibraryIncludes(pg::singleFile(programFor("screamer.cir", gen)));
        ms::TraceRequest req;
        req.find = ms::TraceFind::any;
        const ms::TraceResult t = ms::trace(files, req);
        for (const std::string& l : t.lines)
            std::printf("  trace: %s\n", l.c_str());
        bool sameBits = false;
        for (const std::string& l : t.lines)
            sameBits = sameBits || has(l, "gave the same bits");
        P.eq("trace.ran", t.error.empty() ? 1 : 0, 1);
        P.eq("trace.pass2_same_bits", !t.splitDiffers && sameBits ? 1 : 0, 1);
        const pg::Patch::Result c = pg::Patch::compile(files, 48000.0f);
        int paths = 0;
        if (c.patch)
            for (const pg::Patch::Field& f : c.patch->fieldMap())
                paths += f.path == "pedal.x_C1" || f.path == "pedal.v_D1" ? 1 : 0;
        P.eq("trace.field_paths", paths, 2);
        // The probe fields (Patch.h, "Probes": every netlist generated with them) are the probes', never the trace's:
        // no kProbeFields name in the map nor in what it leaves out, while the module has them all.
        int probeNamed = 0, probeLeft = 0;
        if (c.patch)
        {
            for (const pg::Patch::Field& f : c.patch->fieldMap())
                probeNamed += pg::circuits::isProbeField(f.name) ? 1 : 0;
            for (const std::string& l : c.patch->unmapped())
                for (const char* name : pg::circuits::kProbeFields)
                    probeLeft += l.find(std::string(".") + name) != std::string::npos || l.rfind(name, 0) == 0 ? 1 : 0;
        }
        P.eq("trace.no_probe_fields", c.patch != nullptr && probeNamed == 0 && probeLeft == 0 ? 1 : 0, 1);
        P.eq("trace.probe_table", c.patch != nullptr && c.patch->probeCircuits() == 1 && c.patch->probed().size() == 1
                                      && c.patch->probed()[0].field == "pedal" && c.patch->probed()[0].file == "screamer.cir"
                                      && c.patch->probed()[0].paths == std::vector<std::string>{ "pedal" }
                                      && c.patch->probed()[0].nodes.size() == gen.nodes.size() ? 1 : 0, 1);
    }

    // ---- ERRORS -------------------------------------------------------------------------------------------------------
    {
        const std::string diodetone = libraryText("diodetone.cir");
        const std::string broken = replaced(diodetone, ".end\n", "Q1     a  0  b  2N3904\n.end\n");
        const int line = lineOf(broken, "Q1 ");
        const fminus::circuit::Result gen = fminus::circuit::generate(diodetone, "broken.cir");
        const pg::Sources files = { { pg::kProgramName, programFor("broken.cir", gen) }, { "broken.cir", broken } };
        const pg::Patch::Result c = pg::Patch::compile(files, 48000.0f);
        const pg::Patch::Result k = pg::Patch::check(files);
        const std::string at = "broken.cir:" + std::to_string(line) + ":1: error: ";
        std::printf("broken netlist, compile: %scheck: %s", c.errors.c_str(), k.errors.c_str());
        P.eq("errors.compile_at_line", c.patch == nullptr && c.errors.rfind(at, 0) == 0 ? 1 : 0, 1);
        P.eq("errors.check_at_line", k.errors.rfind(at, 0) == 0 ? 1 : 0, 1);
        P.eq("errors.stop_before_fminus", !has(c.errors, "program.fm:") ? 1 : 0, 1);
        const pg::Patch::Result none = pg::Patch::compile(pg::singleFile("#include \"y.cir\"\n" + silentMain()), 48000.0f);
        std::printf("no such netlist: %s", none.errors.c_str());
        P.eq("errors.not_found", none.patch == nullptr && has(none.errors, "'y.cir' file not found") ? 1 : 0, 1);
        const pg::Patch::Result first = pg::Patch::compile({ { "screamer.cir", libraryText("screamer.cir") } }, 48000.0f);
        const pg::Patch::Result firstCheck = pg::Patch::check({ { "screamer.cir", libraryText("screamer.cir") } });
        std::printf("a netlist first: %s", first.errors.c_str());
        P.eq("errors.netlist_first", first.patch == nullptr && first.errors.rfind("screamer.cir: error: the program is an FMinus file", 0) == 0
                                         && firstCheck.errors.rfind("screamer.cir: error: the program is an FMinus file", 0) == 0 ? 1 : 0, 1);
    }

    // ---- LINES --------------------------------------------------------------------------------------------------------
    {
        const std::string screamer = libraryText("screamer.cir");
        const pg::Sources files = { { pg::kProgramName, "module Screamer\n{\n    float x;\n};\n#include \"screamer.cir\"\n" + silentMain() },
                                    { "screamer.cir", screamer } };
        const pg::Patch::Result c = pg::Patch::compile(files, 48000.0f);
        std::printf("a module named twice: %s", c.errors.c_str());
        const std::string at = "screamer.cir:" + std::to_string(lineOf(screamer, ".title")) + ":1: error: ";
        bool cited = false;
        for (const std::string_view l : lg::splitLines(c.errors))
            cited = cited || (l.rfind(at, 0) == 0 && has(std::string(l), "(in the module generated from screamer.cir, line "));
        P.eq("lines.title_line", c.patch == nullptr && cited ? 1 : 0, 1);
        P.eq("lines.no_generated_name", !has(c.errors, "screamer.cir.fm:") ? 1 : 0, 1);
    }

    // ---- INCLUDE ------------------------------------------------------------------------------------------------------
    {
        const fminus::circuit::Result gen = fminus::circuit::generate(libraryText("screamer.cir"), "screamer.cir");
        const std::string program = "#include \"screamer.cir\"\n" + programFor("screamer.cir", gen);
        const uint64_t before = pg::circuits::generations();
        const pg::Patch::Result twice = pg::Patch::compile(pg::withLibraryIncludes(pg::singleFile(program)), 48000.0f);
        const uint64_t generated = pg::circuits::generations() - before;
        std::printf("#included twice: %llu generated\n", static_cast<unsigned long long>(generated));
        P.eq("include.twice_compiles", twice.patch != nullptr && twice.errors.empty() ? 1 : 0, 1);
        P.eq("include.generated_once", static_cast<int64_t>(generated), 1);

        const std::string broken = replaced(libraryText("diodetone.cir"), ".end\n", "Q1     a  0  b  2N3904\n.end\n");
        const pg::Sources files = { { pg::kProgramName, silentMain() }, { "broken.cir", broken } };
        const uint64_t at = pg::circuits::generations();
        const pg::Patch::Result c = pg::Patch::compile(files, 48000.0f);
        P.eq("include.unread_runs", c.patch != nullptr && c.errors.empty() && c.notes.empty() ? 1 : 0, 1);
        P.eq("include.unread_no_job", c.circuitJobs.empty() && pg::circuits::generations() == at ? 1 : 0, 1);
        const pg::Patch::Result k = pg::Patch::check(files);
        std::printf("an unread broken netlist, check: %s", k.errors.c_str());
        P.eq("include.unread_squiggles", has(k.errors, "broken.cir:" + std::to_string(lineOf(broken, "Q1 ")) + ":1: error: ") ? 1 : 0, 1);
        P.eq("include.check_no_job", k.circuitJobs.empty() ? 1 : 0, 1);
    }

    // ---- RECIPE -------------------------------------------------------------------------------------------------------
    {
        const std::string screamer = libraryText("screamer.cir");
        fminus::circuit::Options probed;
        probed.probes = true;                                   // the playground's (Circuits.h, "Probes")
        const fminus::circuit::Result gen = fminus::circuit::generate(screamer, "screamer.cir", probed);
        const pg::Patch::Result c = pg::Patch::compile(pg::withLibraryIncludes(pg::singleFile(programFor("screamer.cir", gen))), 48000.0f);
        P.eq("recipe.compiles", c.patch != nullptr ? 1 : 0, 1);
        if (c.patch)
        {
            const pg::Patch::Recipe r = c.patch->recipe();
            P.eq("recipe.keeps_module", r.circuits.size() == 1 && r.circuits[0].name == "screamer.cir" && r.circuits[0].text == gen.source ? 1 : 0, 1);
            std::array<float, pg::kMaxControls> plain{};
            for (std::size_t k = 0; k < plain.size(); ++k)
                plain[k] = r.controls[k].def;
            const uint64_t before = pg::circuits::generations();
            std::string why, frozenText;
            pg::Patch::Ir ir;
            const bool frozen = pg::Patch::freeze(r, plain, why) != nullptr;
            const bool lowered = pg::Patch::ir(r, ir, why);
            const bool frozenIr = pg::Patch::frozenIr(r, plain, frozenText, why);
            std::printf("recipe: freeze %d, ir %d (%zu bytes), frozenIr %d; %s\n", frozen ? 1 : 0, lowered ? 1 : 0, ir.optimised.size(),
                        frozenIr ? 1 : 0, why.c_str());
            P.eq("recipe.freeze_ir_frozen_ir", frozen && lowered && frozenIr && !ir.optimised.empty() && !frozenText.empty() ? 1 : 0, 1);
            P.eq("recipe.generates_nothing", static_cast<int64_t>(pg::circuits::generations() - before), 0);
        }
    }

    // ---- DIALS, OWN, SWITCH -------------------------------------------------------------------------------------------
    dialRows(P);

    // ---- SELFTEST, REFRESH, DEDUP ---------------------------------------------------------------------------------------
    selfTester(P);

    // ---- PROVISIONAL and STORE ----------------------------------------------------------------------------------------
    {
        const fminus::circuit::Result gen = fminus::circuit::generate(kClipper, "clipper.cir");
        const pg::Sources files = { { pg::kProgramName, programFor("clipper.cir", gen) }, { "clipper.cir", kClipper } };
        const pg::Patch::Result c = pg::Patch::compile(files, 48000.0f);
        std::printf("provisional: %s%s", c.errors.c_str(), c.notes.c_str());
        P.eq("provisional.compiles", c.patch != nullptr && c.errors.empty() ? 1 : 0, 1);
        P.eq("provisional.note", c.notes == "clipper.cir: note: the self-test chooses this circuit's configuration; provisional: 8x "
                                           "IIR, omega(2)\n" ? 1 : 0, 1);
        P.eq("provisional.one_job", c.circuitJobs.size() == 1 && c.circuitJobs[0].key == gen.selfTestKey && !gen.selfTestKey.empty()
                                        && c.circuitJobs[0].file == "clipper.cir" && c.circuitJobs[0].netlist == kClipper ? 1 : 0, 1);
        P.eq("provisional.key", c.circuitKeys == std::vector<std::string>{ gen.selfTestKey } ? 1 : 0, 1);
        probe::Rig rig(kFs, 512);
        rig.load(files);
        const probe::Stereo out = rig.run(input(second / 2), 256, chord());
        P.eq("provisional.plays", finite(out) && probe::rmsDb(out.l, 0, second / 2) > -80.0 ? 1 : 0, 1);
        if (!c.patch || c.circuitJobs.empty())
            return P.finish();
        const pg::Patch::Recipe provisional = c.patch->recipe();

        // The job's self-test, stored as the plugin's self-tester will store it.
        const pg::circuits::Job& job = c.circuitJobs[0];
        fminus::circuit::SelfTestOptions test;
        test.parallel = parallel;
        const fminus::circuit::SelfTestResult answer = fminus::circuit::selfTest(job.netlist, job.file, {}, test);
        std::printf("the self-test: %d candidates; %s%s\n", answer.candidates, answer.errors.c_str(),
                    answer.choice ? answer.choice->text().c_str() : "no answer");
        P.eq("store.answered", answer.choice && answer.key == job.key ? 1 : 0, 1);
        if (!answer.choice)
            return P.finish();
        const uint64_t landed = pg::circuits::answers().landed();
        pg::circuits::answers().store(answer.key, answer.choice->text());
        P.eq("store.landed", pg::circuits::answers().landed() == landed + 1 ? 1 : 0, 1);
        const pg::Patch::Result again = pg::Patch::compile(files, 48000.0f);
        fminus::circuit::Options chosen;
        chosen.chosen = answer.choice;
        chosen.probes = true;                                   // the playground's (Circuits.h, "Probes")
        const fminus::circuit::Result want = fminus::circuit::generate(job.netlist, job.file, chosen);
        std::printf("answered: %s; %s%s\n", want.configuration.c_str(), again.errors.c_str(), again.notes.c_str());
        P.eq("store.compiles_quietly", again.patch != nullptr && again.errors.empty() && again.notes.empty() ? 1 : 0, 1);
        P.eq("store.no_job", again.circuitJobs.empty() && again.circuitKeys == std::vector<std::string>{ job.key } ? 1 : 0, 1);
        P.eq("store.module_is_generates", again.patch != nullptr && again.patch->recipe().circuits.size() == 1
                                              && again.patch->recipe().circuits[0].text == want.source && !want.provisional ? 1 : 0, 1);
        // The provisional program's frozen copy keeps its own module (its layout), whatever the store holds now.
        std::array<float, pg::kMaxControls> plain{};
        std::string why;
        P.eq("store.frozen_keeps_layout", pg::Patch::freeze(provisional, plain, why) != nullptr
                                              && provisional.circuits[0].text != want.source ? 1 : 0, 1);

        // The store itself.
        pg::circuits::Answers& store = pg::circuits::answers();
        P.eq("store.find", store.find(job.key) == answer.choice->text() && !store.find("0000000000000000") ? 1 : 0, 1);
        P.eq("store.of", store.of({ "0000000000000000", job.key, job.key }).size() == 1 ? 1 : 0, 1);
        P.eq("store.claim_stored", !store.claim(job.key) ? 1 : 0, 1);
        const bool claimed = store.claim("00000000000000aa");
        const bool twiceClaimed = store.claim("00000000000000aa");
        const bool flying = store.inFlight("00000000000000aa");
        store.release("00000000000000aa");
        P.eq("store.claim_release", claimed && !twiceClaimed && flying && !store.inFlight("00000000000000aa") ? 1 : 0, 1);
        for (int k = 0; k < 70; ++k)
            store.store("k" + std::to_string(k), "x");
        P.eq("store.capacity", store.size() == pg::circuits::Answers::kCapacity && !store.find(job.key) && !store.find("k5")
                                   && store.find("k69") ? 1 : 0, 1);
        store.clearForTests();
    }
    return P.finish();
}
