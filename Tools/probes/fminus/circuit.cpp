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
//                fields as paths (pedal.x_C1, pedal.v_D1)
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
// and the plugin's self-test (dsp/SelfTester.h, Compiler.h's "The self-test"), a Compiler offering to an Engine played
// in blocks of 256, on AutoClipper.h's netlists (no assertion is on time; every wait has a deadline):
//   SELFTEST     with the tests off (setSelfTesting(false)) a RUN plays the provisional configuration and no test runs;
//                selfTestNow() runs the test and the refine: the status's revision and request move on, it compiled,
//                the Engine's Meters::program names the refined program, and its note names the self-test's choice
//                (circuits::chosenNote); the same files compiled again read the store: no job, no note
//   REFRESH      after a RUN that does not compile, the refine compiles the files that play; while a recording is armed
//                (Engine::ownCode) the refine waits, and comes when it ends; a RUN queued before the refine wins (it
//                reads the store: no note, no job) and the refine is dropped; an instrument's refine waits while a key
//                is held (no new program id), and lands after the release, once the voice has ended; with the tests on,
//                a RUN whose files drop the netlist cancels its test, which stores nothing, and a RUN made while the
//                test runs has its status before landed() moves (by order), the refine after it
//   DEDUP        two Compilers wanting one key run one test (the in-flight set), and both refine
#include "Probe.h"

#include "Audio.h"
#include "AutoClipper.h"
#include "Rig.h"

#include "Library.h"
#include "Product.h"

#include "dsp/Compiler.h"
#include "dsp/Debug.h"
#include "dsp/Engine.h"
#include "dsp/Sources.h"
#include "fminus/Circuits.h"
#include "fminus/Lang.h"
#include "fminus/LibraryIncludes.h"
#include "fminus/Patch.h"

#include "fminus/Circuit.h"
#include "fminus/FMinus.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
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
            P.eq("selftest.revision_request_advance", b.revision == a.revision + 1 && b.request > a.request ? 1 : 0, 1);
            P.eq("selftest.ok", b.ok && b.sources == files ? 1 : 0, 1);
            P.eq("selftest.meters_program", m.program == b.request ? 1 : 0, 1);
            const std::string note = pg::circuits::chosenNote("clipper.cir", answer);
            P.eq("selftest.note_names_choice", !note.empty() && has(b.notes, note) && !has(b.notes, "provisional") ? 1 : 0, 1);
            const pg::CompileStatus c = live.compiler->compileNow(files);
            P.eq("selftest.again_reads_store", c.ok && c.notes.empty() && !live.compiler->selfTesting() && live.compiler->selfTests() == 1
                                                   ? 1 : 0, 1);
        }
        if (answer.empty())
            return;

        // ---- REFRESH ----------------------------------------------------------------------------------------------------
        // After a RUN that does not compile, the refine compiles the files that play.
        {
            store.clearForTests();
            Live live(false);
            const pg::CompileStatus a = live.compiler->compileNow(files);
            pg::Sources broken = files;
            broken[0].text += "module {\n";
            live.compiler->request(broken);
            live.compiler->wait();
            const pg::CompileStatus bad = live.compiler->status();
            const bool refined = live.compiler->selfTestNow();
            const pg::CompileStatus s = live.compiler->status();
            std::printf("refresh: after a broken RUN: refined %d, %s%s", refined ? 1 : 0, s.errors.c_str(), s.notes.c_str());
            P.eq("refresh.failed_run_refines_what_plays", a.ok && !bad.ok && refined && s.ok && s.sources == files && chose(s) ? 1 : 0, 1);
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
            P.eq("refresh.after_recording", after && s.ok && s.revision == a.revision + 1 && chose(s) && live.play(8).program == s.request
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
            P.eq("refresh.synth_after_release", releasing && !sounding && silent && after && s.ok && chose(s) && now.program == s.request
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
                return s.request > run && chose(s);
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
        const fminus::circuit::Result gen = fminus::circuit::generate(screamer, "screamer.cir");
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
