// PROBE name=dsp.faults timeout=300
//
// dsp.faults: an exception on the compiler's threads (Compiler.h; dsp/Faults.h) fails that piece of work in words and
// the thread lives on; before, it reached std::terminate and took the host with it. Each is planted with the probes'
// seam (faults::plantForTests: std::bad_alloc, a std::runtime_error, an int) where the work begins, at 48 kHz in blocks
// of 256, this thread playing the audio thread's part (no thread of its own: a RUN waits for no block then):
//   RUN       for each kind: a RUN whose compile throws fails, its error "program.fm:1:1: error: the compile failed
//             inside the plugin: out of memory" (what() on one line; "an unknown error"), its files the RUN's; the
//             program that played plays on (the status's program), the same samples as its twin's, which saw no failed
//             RUN; the next RUN compiles and plays
//   NOW       compileNow whose compile throws returns a failed status in the same words, never the exception; the
//             program before plays on; compileNow again compiles
//   CHECK     a check while typing that throws gives no messages for that request; the next check gives its own, and a
//             RUN after it compiles (the thread lives)
//   IR        the IR's compile for the IR alone that throws: the view's why says it failed inside the plugin; a RUN
//             after it compiles
//   FREEZE    freezeNow whose copy throws: none (false, no copy counted), and once taken back, one; the freezer's
//             thread with a throw planted a thousand times, the dials still for a second: one try, no loop, no copy
//   SELFTEST  a circuit's self-test that throws on its coordinator: its outcome no answer, why "it failed inside the
//             plugin: ...", the key given up on (not tested again: selfTests() stays), the compile's notes say so where
//             the provisional note was; one that throws on a worker of a batch: FMinus's own catch, an internal error,
//             given up on too
#include "Probe.h"

#include "AutoClipper.h"
#include "Rig.h"

#include "dsp/Compiler.h"
#include "dsp/Engine.h"
#include "dsp/Faults.h"
#include "fminus/Circuits.h"
#include "fminus/LibraryIncludes.h"
#include "fminus/Patch.h"

#include "Examples.h"
#include "Product.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;
    constexpr double kFs = 48000.0;
    constexpr int kBlock = 256;
    using pg::faults::Kind;
    using pg::faults::Stage;

    bool has(const std::string& s, const std::string& what)
    {
        return s.find(what) != std::string::npos;
    }

    // An Engine and its Compiler, nothing in the background: no freezer, no self-test; this thread its audio thread.
    struct Twin
    {
        pg::Engine engine;
        std::unique_ptr<pg::Compiler> compiler;
        int at = 0;                                          // the blocks played
        std::array<float, pg::kMaxControls> norm{};

        explicit Twin(bool freezing = false)
        {
            engine.prepare(kFs, kBlock);
            compiler = std::make_unique<pg::Compiler>(engine);
            compiler->setFreezing(freezing, 0.0);
            compiler->setSelfTesting(false);
        }

        void dialsOf(const pg::CompileStatus& s)
        {
            for (std::size_t c = 0; c < norm.size(); ++c)
                norm[c] = s.controls[c].used ? pg::normOf(s.controls[c], s.controls[c].def) : 0.0f;
        }

        // `blocks` blocks of noise in (a note held from the first, an instrument's), their left channel out.
        std::vector<float> play(int blocks)
        {
            std::vector<float> out;
            for (int b = 0; b < blocks; ++b, ++at)
            {
                probe::Stereo io = probe::noise(kBlock, 0.25, 1000u + static_cast<uint32_t>(at));
                std::vector<pg::Event> events;
                if (kInstrument && at == 0)
                {
                    pg::Event e;
                    e.kind = pg::EventKind::noteOn;
                    e.note = 57;
                    e.velocity = 0.8f;
                    events.push_back(e);
                }
                engine.setControls(norm);
                float* ch[2] = { io.l.data(), io.r.data() };
                engine.process(ch, 2, kBlock, events);
                out.insert(out.end(), io.l.begin(), io.l.end());
            }
            return out;
        }
    };

    template <typename F>
    bool until(F&& done)
    {
        const auto end = std::chrono::steady_clock::now() + probe::kWorkWait;
        while (!done())
        {
            if (std::chrono::steady_clock::now() > end)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }

    const char* nameOf(Kind k)
    {
        return k == Kind::outOfMemory ? "memory" : k == Kind::error ? "error" : "unknown";
    }

    std::string wordsOf(Kind k)
    {
        return k == Kind::outOfMemory ? "out of memory" : k == Kind::error ? "a planted failure in two lines" : "an unknown error";
    }
}

PROBE(dsp, faults)
{
    // The program that plays (a second example when there is one: one with state), and another to RUN.
    const pg::Example& e = pg::kExamples[std::size(pg::kExamples) > 1 ? 1 : 0];
    const pg::Sources playing = pg::withLibraryIncludes(pg::singleFile(e.source));
    pg::Sources other = playing;
    other.front().text += "\n// run again\n";
    std::printf("program: %s\n", std::string(e.source).substr(0, std::string(e.source).find('\n')).c_str());

    // ---- RUN -----------------------------------------------------------------------------------------------------------
    for (const Kind kind : { Kind::outOfMemory, Kind::error, Kind::unknown })
    {
        const std::string k = std::string("run.") + nameOf(kind);
        Twin a, b;
        const pg::CompileStatus sa = a.compiler->compileNow(playing);
        const pg::CompileStatus sb = b.compiler->compileNow(playing);
        a.dialsOf(sa);
        b.dialsOf(sb);
        P.eq(k + ".first_plays", sa.ok && sb.ok ? 1 : 0, 1);
        const std::vector<float> a1 = a.play(20), b1 = b.play(20);
        pg::faults::plantForTests(Stage::compile, kind, 1);
        const uint64_t id = a.compiler->request(other);
        a.compiler->wait();
        const pg::CompileStatus failed = a.compiler->status();
        std::printf("%s: %s", k.c_str(), failed.errors.c_str());
        P.eq(k + ".fails", !failed.ok && !failed.stopped && failed.request == id ? 1 : 0, 1);
        P.eq(k + ".words", failed.errors == std::string(pg::kProgramName) + ":1:1: error: the compile failed inside the plugin: "
                                               + wordsOf(kind) + "\n" ? 1 : 0, 1);
        P.eq(k + ".its_files", failed.sources == other ? 1 : 0, 1);
        P.eq(k + ".plays_on", static_cast<int64_t>(failed.program), static_cast<int64_t>(sa.program));
        const std::vector<float> a2 = a.play(40), b2 = b.play(40);
        P.eq(k + ".same_samples", a1 == b1 && a2 == b2 ? 1 : 0, 1);
        const uint64_t next = a.compiler->request(other);
        a.compiler->wait();
        const pg::CompileStatus again = a.compiler->status();
        P.eq(k + ".next_run", again.ok && again.request == next && again.program == next ? 1 : 0, 1);
        a.play(4);
        pg::Meters m;
        bool read = false;
        for (int t = 0; t < 1000 && !(read = a.engine.meters(m)); ++t)
            std::this_thread::yield();
        P.eq(k + ".next_plays", read && m.program == next ? 1 : 0, 1);
    }

    // ---- NOW -----------------------------------------------------------------------------------------------------------
    {
        Twin a;
        const pg::CompileStatus first = a.compiler->compileNow(playing);
        for (const Kind kind : { Kind::outOfMemory, Kind::unknown })
        {
            const std::string k = std::string("now.") + nameOf(kind);
            pg::faults::plantForTests(Stage::compile, kind, 1);
            bool threw = false;
            pg::CompileStatus s;
            try
            {
                s = a.compiler->compileNow(other);
            }
            catch (...)
            {
                threw = true;
            }
            P.eq(k + ".returns", threw ? 0 : 1, 1);
            P.eq(k + ".words", !s.ok && s.errors == std::string(pg::kProgramName) + ":1:1: error: the compile failed inside the plugin: "
                                                        + wordsOf(kind) + "\n" ? 1 : 0, 1);
            P.eq(k + ".plays_on", static_cast<int64_t>(s.program), static_cast<int64_t>(first.program));
        }
        const pg::CompileStatus after = a.compiler->compileNow(other);
        P.eq("now.next_compiles", after.ok && after.program == after.request ? 1 : 0, 1);
    }

    // ---- CHECK ---------------------------------------------------------------------------------------------------------
    {
        Twin a;
        (void)a.compiler->compileNow(playing);
        pg::Sources broken = playing;
        broken.front().text += "\nnot a program\n";
        const uint64_t r0 = a.compiler->checkStatus().revision;
        pg::faults::plantForTests(Stage::check, Kind::error, 1);
        a.compiler->check(broken);
        const bool came = until([&] { return a.compiler->checkStatus().revision > r0; });
        const pg::CheckStatus thrown = a.compiler->checkStatus();
        P.eq("check.answered", came ? 1 : 0, 1);
        P.eq("check.no_messages", came && thrown.errors.empty() && thrown.sources == broken ? 1 : 0, 1);
        a.compiler->check(broken);
        const bool again = until([&] { return a.compiler->checkStatus().revision > thrown.revision; });
        const pg::CheckStatus next = a.compiler->checkStatus();
        P.eq("check.next_messages", again && has(next.errors, "error:") ? 1 : 0, 1);
        const uint64_t id = a.compiler->request(other);
        a.compiler->wait();
        P.eq("check.run_after", a.compiler->status().ok && a.compiler->status().request == id ? 1 : 0, 1);
    }

    // ---- IR ------------------------------------------------------------------------------------------------------------
    {
        Twin a;
        (void)a.compiler->compileNow(playing);
        pg::faults::plantForTests(Stage::ir, Kind::unknown, 1);
        a.compiler->setIrWanted(true);
        const bool came = until([&] {
            const pg::IrStatus ir = a.compiler->ir();
            return !ir.making && (!ir.why.empty() || ir.optimised);
        });
        const pg::IrStatus ir = a.compiler->ir();
        std::printf("ir: %s\n", ir.why.c_str());
        P.eq("ir.why", came && !ir.optimised && ir.why == "it failed inside the plugin: an unknown error" ? 1 : 0, 1);
        a.compiler->setIrWanted(false);
        const uint64_t id = a.compiler->request(other);
        a.compiler->wait();
        P.eq("ir.run_after", a.compiler->status().ok && a.compiler->status().request == id ? 1 : 0, 1);
    }

    // ---- FREEZE --------------------------------------------------------------------------------------------------------
    {
        Twin a;
        a.dialsOf(a.compiler->compileNow(playing));
        a.play(8);
        pg::faults::plantForTests(Stage::freeze, Kind::outOfMemory, 1);
        P.eq("freeze.now_none", a.compiler->freezeNow() ? 0 : 1, 1);
        P.eq("freeze.now_counted_none", a.compiler->freezes(), 0);
        P.eq("freeze.now_after", a.compiler->freezeNow() ? 1 : 0, 1);
    }
    {
        Twin a(true);                                        // the freezer's thread, after 0 s of still dials
        pg::faults::plantForTests(Stage::freeze, Kind::error, 1000);
        a.dialsOf(a.compiler->compileNow(playing));
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < end)
        {
            a.play(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const int left = pg::faults::plantedForTests(Stage::freeze);
        pg::faults::plantForTests(Stage::freeze, Kind::error, 0);
        std::printf("freeze: the freezer took %d of the thousand throws planted\n", 1000 - left);
        P.eq("freeze.thread_one_try", 1000 - left, 1);
        P.eq("freeze.thread_no_copy", a.compiler->freezes(), 0);
    }

    // ---- SELFTEST ------------------------------------------------------------------------------------------------------
    {
        pg::circuits::Answers& store = pg::circuits::answers();
        store.clearForTests();
        const pg::Sources clipper{ { pg::kProgramName, probe::clipperProgram("clipper.cir", "Clipper", false) },
                                   { "clipper.cir", probe::kAutoClipper } };
        Twin a;
        const pg::CompileStatus s = a.compiler->compileNow(clipper);
        P.eq("selftest.compiles", s.ok && s.circuitKeys.size() == 1 ? 1 : 0, 1);
        const std::string key = s.circuitKeys.empty() ? std::string() : s.circuitKeys.front();
        pg::faults::plantForTests(Stage::selfTest, Kind::error, 1);
        (void)a.compiler->selfTestNow();
        const std::optional<pg::circuits::Answers::Outcome> o = store.outcome(key);
        std::printf("selftest: %s\n", o ? o->why.c_str() : "(no outcome)");
        P.eq("selftest.one_test", a.compiler->selfTests(), 1);
        P.eq("selftest.given_up", store.gaveUp(key) && !store.find(key) ? 1 : 0, 1);
        P.eq("selftest.why", o && o->why == "it failed inside the plugin: a planted failure in two lines" ? 1 : 0, 1);
        P.eq("selftest.noted", has(a.compiler->status().notes, "clipper.cir: note: the self-test found no configuration for this "
                                                                "circuit (it failed inside the plugin: a planted failure in two lines)")
                                   ? 1 : 0, 1);
        (void)a.compiler->selfTestNow();
        P.eq("selftest.not_again", a.compiler->selfTests(), 1);

        // On a worker of a batch: FMinus's selfTest catches what its parallel work throws (an internal error).
        store.clearForTests();
        pg::faults::plantForTests(Stage::selfTestBatch, Kind::outOfMemory, 1);
        (void)a.compiler->selfTestNow();
        const std::optional<pg::circuits::Answers::Outcome> w = store.outcome(key);
        std::printf("selftest batch: %s\n", w ? w->why.c_str() : "(no outcome)");
        P.eq("selftest.batch_tested", a.compiler->selfTests(), 2);
        P.eq("selftest.batch_given_up", store.gaveUp(key) && w && has(w->why, "internal error") ? 1 : 0, 1);
        pg::faults::plantForTests(Stage::selfTestBatch, Kind::outOfMemory, 0);
        store.clearForTests();
    }
    return P.finish();
}
