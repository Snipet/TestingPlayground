// PROBE name=dsp.budget timeout=900
//
// dsp.budget: runaway-loop protection (Patch.h, "The loop budget"; Engine.h and Compiler.h, "A program that stops").
// Programs whose loops never end, played at 48 kHz in blocks of 256:
//   PROCESS  a loop in process that never ends once a dial is on (a phase wrap whose step is infinite): the call
//            returns, the program stops (Patch::stopped, the Engine's stop: process, not gone back from), its output
//            fades from its last samples to its input (an effect) or to silence (an instrument's voice) with no step
//            larger than the signal's own, then is the input exactly (silence exactly); it is called no more (blocks
//            after it take no budget's time) and an instrument starts no note on it
//   PARALLEL an effect's process whose parallel for's body never ends: every thread's part stops, and the call returns
//   INIT     an init that never ends, and Shared's: the compile fails with its message (Patch::Result::stopped), in
//            seconds; an end that never ends: the program is destroyed, in seconds
//   FIRST    a program that stops at its first block, offered revertible while another plays: the Engine goes back to
//            that one, which plays on (an effect: the same bits as it alone; an instrument: its held key sounds again),
//            the stop published as gone back from (settle() then says so); offered otherwise, it stops and fades out
//   RUN      through a Compiler with an audio thread: a RUN of a program that stops at once fails (ok false, stopped,
//            its message) and the program before plays on (the status's program, the Engine's); a RUN of one that stops
//            only when a dial turns: ok, and once it turns, a status of its own says it stopped, and the program
//            before plays again (restored: its files, its dials, a new id, the Engine's, sounding)
//   WORDS    the message at the method's line ("program.fm:L:C: error: process() ran past its budget: a loop that never
//            ends?"), Shared's init()'s; measure_output's report begins with it and says when; trace's lines too
//   COST     each example with and without the budget (an instrument's eight voices, two seconds): the same bits, its
//            own code and its frozen copy; the CPU time of each (the thread's, the best of seven, printed) and the
//            sums' ratio within kCostRatio; the most of its budget a call took (printed). PROBE_BUDGET_PROGRAM, a folder
//            holding a program's files (program.fm first, then the others by name), adds it to what is printed, not
//            asserted.
#include "Probe.h"

#include "Rig.h"

#include "dsp/Compiler.h"
#include "dsp/Debug.h"
#include "dsp/Engine.h"
#include "dsp/Measure.h"
#include "fminus/LibraryIncludes.h"
#include "fminus/Patch.h"

#include "Examples.h"
#include "Product.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;
    constexpr double kFs = 48000.0;
    constexpr int kBlock = 256;
    constexpr double kCostRatio = 1.15;           // the examples' time with the budget over without, at most
    constexpr double kSecondsAtMost = 60.0;       // a stuck init or end, stopped (2^32 iterations, a few seconds)

    using Clock = std::chrono::steady_clock;
    double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

    // The calling thread's CPU time, ms: what the cost rows time, so that time the thread waits for a core (the
    // machine busy with other work) is not counted.
    double threadMs()
    {
        timespec ts{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1.0e6;
    }

    // ---- the programs ----------------------------------------------------------------------------------------------
    // The main module's head and its plain body (an effect: half its input; an instrument: a sine at 0.3 while held),
    // with `extra` declarations and `before` at the top of process, and `hooks` (start, stop, init, end) as given.
    std::string effect(const std::string& extra, const std::string& before, const std::string& hooks = {})
    {
        return "module Effect\n{\n" + extra + hooks
             + "    void process(float* restrict left, float* restrict right, int n)\n    {\n" + before
             + "        for (int i = 0; i < n; ++i)\n        {\n            left[i] = 0.5f * left[i];\n"
               "            right[i] = 0.5f * right[i];\n        }\n    }\n};\n";
    }

    std::string voice(const std::string& extra, const std::string& before, const std::string& start = {},
                      const std::string& hooks = {})
    {
        return "module Voice\n{\n    float phase, inc;\n    bool on;\n" + extra + hooks
             + "    void start(float note, float velocity)\n    {\n" + start
             + "        inc = 440.0f * expf(0.0577622650f * (note - 69.0f)) / 48000.0f;\n        on = true;\n    }\n\n"
               "    void stop() { on = false; }\n\n"
               "    bool process(float* restrict left, float* restrict right, int n)\n    {\n" + before
             + "        for (int i = 0; i < n; ++i)\n        {\n            const float s = 0.3f * sinf(6.2831853f * phase);\n"
               "            phase = phase + inc;\n            if (phase >= 1.0f)\n                phase = phase - 1.0f;\n"
               "            left[i] = left[i] + s;\n            right[i] = right[i] + s;\n        }\n        return on;\n    }\n};\n";
    }

    std::string program(const std::string& extra, const std::string& before, const std::string& hooks = {})
    {
        return kInstrument ? voice(extra, before, {}, hooks) : effect(extra, before, hooks);
    }

    // A loop that never ends once the dial `blow` is on: a phase wrap whose step is infinite (FMinus's own example).
    const char* kBlowDial = "    param bool blow [[fminus::default(0)]];\n    float wrap;\n\n";
    const char* kBlow = "        const float step = blow[0] ? 1.0e30f * 1.0e30f : 0.001f;\n"
                        "        for (int i = 0; i < n; ++i)\n        {\n            wrap = wrap + step;\n"
                        "            while (wrap >= 1.0f)\n                wrap = wrap - 1.0f;\n        }\n";
    // One that never ends at all, and one from the 12,000th sample on (a quarter of a second at 48 kHz).
    const char* kSpinField = "    int spins;\n    int count;\n\n";
    const char* kSpin = "        while (1)\n            spins = spins + 1;\n";
    const char* kSpinLater = "        count = count + n;\n        if (count > 12000)\n            while (1)\n"
                             "                spins = spins + 1;\n";

    // The method's position in `source` ("process(" in its declaration), as the message cites it: "L:C".
    std::string positionOf(const std::string& source, const std::string& declaration, const std::string& name)
    {
        const std::size_t at = source.find(declaration);
        if (at == std::string::npos)
            return "?";
        const std::size_t word = source.find(name, at);
        const std::size_t lineStart = source.rfind('\n', word);
        const std::size_t line = static_cast<std::size_t>(std::count(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(word), '\n')) + 1;
        const std::size_t col = word - (lineStart == std::string::npos ? 0 : lineStart + 1) + 1;
        return std::to_string(line) + ":" + std::to_string(col);
    }

    probe::Stereo sineIn(std::size_t n)
    {
        probe::Stereo s(n);
        if constexpr (!kInstrument)
        {
            s.l = probe::sine(220.0, 0.5, kFs, n);
            s.r = s.l;
        }
        return s;
    }

    // `in` through `engine` in blocks of kBlock from sample `from`: the dials `norm` from the block at `blowAt` on (0:
    // all), a note-on of 60 at the first block when `note`.
    probe::Stereo play(pg::Engine& engine, const probe::Stereo& in, std::size_t blowAt = static_cast<std::size_t>(-1),
                       bool note = false, std::size_t* stoppedAt = nullptr)
    {
        probe::Stereo out = in;
        const uint64_t stops = engine.stops();
        for (std::size_t pos = 0; pos < in.size(); pos += kBlock)
        {
            const int n = static_cast<int>(std::min<std::size_t>(kBlock, in.size() - pos));
            std::array<float, pg::kMaxControls> norm{};
            norm[0] = pos >= blowAt ? 1.0f : 0.0f;
            engine.setControls(norm);
            std::vector<pg::Event> events;
            if (note && pos == 0)
            {
                pg::Event e;
                e.kind = pg::EventKind::noteOn;
                e.note = 60;
                e.velocity = 0.8f;
                events.push_back(e);
            }
            float* ch[2] = { out.l.data() + pos, out.r.data() + pos };
            engine.process(ch, 2, n, events);
            if (stoppedAt != nullptr && *stoppedAt == static_cast<std::size_t>(-1) && engine.stops() != stops)
                *stoppedAt = pos;
        }
        return out;
    }

    std::unique_ptr<pg::Patch> compiled(const std::string& source, uint64_t id, std::string* errors = nullptr)
    {
        pg::Patch::Result r = pg::Patch::compile(pg::singleFile(source), static_cast<float>(kFs));
        if (errors != nullptr)
            *errors = r.errors;
        if (r.patch)
            r.patch->setProgram(id);
        return std::move(r.patch);
    }

    double maxStepOf(const probe::Stereo& s, std::size_t from, std::size_t count)
    {
        return std::max(probe::maxStep(s.l, from, count), probe::maxStep(s.r, from, count));
    }

    // ---- the cost ------------------------------------------------------------------------------------------------
    std::vector<probe::Note> chord(int voices)
    {
        std::vector<probe::Note> notes;
        if constexpr (kInstrument)
            for (int k = 0; k < voices; ++k)
                notes.push_back({ 0, pg::EventKind::noteOn, 40 + 3 * k, 0.8f });
        return notes;
    }

    probe::Stereo costInput(std::size_t n)
    {
        if constexpr (kInstrument)
            return probe::Stereo(n);
        else
            return probe::noise(n, 0.25, 31);
    }

    // The CPU time each rig takes to play `in`, the best of `tries`, taken in turn.
    std::vector<double> timeEach(const std::vector<probe::Rig*>& rigs, const probe::Stereo& in, const std::vector<probe::Note>& notes,
                                 int tries)
    {
        std::vector<double> best(rigs.size(), 1e9);
        for (int k = 0; k < tries; ++k)
            for (std::size_t r = 0; r < rigs.size(); ++r)
            {
                const double t0 = threadMs();
                rigs[r]->run(in, kBlock, notes);
                best[r] = std::min(best[r], threadMs() - t0);
            }
        return best;
    }

    // The most of its budget the program's last call of each block took, over `in` (an effect's one call a block).
    double mostTaken(probe::Rig& rig, const probe::Stereo& in, const std::vector<probe::Note>& notes)
    {
        double most = 0.0;
        std::size_t pos = 0;
        probe::Stereo out = in;
        std::vector<pg::Event> events;
        for (const probe::Note& n : notes)
        {
            pg::Event e;
            e.kind = n.kind;
            e.note = static_cast<uint8_t>(n.note);
            e.velocity = n.velocity;
            events.push_back(e);
        }
        while (pos < in.size())
        {
            const int n = static_cast<int>(std::min<std::size_t>(kBlock, in.size() - pos));
            rig.engine().setControls(rig.defaults());
            float* ch[2] = { out.l.data() + pos, out.r.data() + pos };
            rig.engine().process(ch, 2, n, pos == 0 ? std::span<const pg::Event>(events) : std::span<const pg::Event>());
            if (const pg::Patch* p = rig.engine().playingPatch(); p != nullptr && p->lastBudget() > 0)
                most = std::max(most, static_cast<double>(p->lastBudget() - p->budgetLeft()) / static_cast<double>(p->lastBudget()));
            pos += static_cast<std::size_t>(n);
        }
        return most;
    }

    struct Cost
    {
        double on = 0, off = 0, frozenOn = 0, frozenOff = 0, taken = 0;
        bool sameBits = false, sameFrozenBits = false, loaded = false;
    };

    Cost costOf(const pg::Sources& files, int voices, double seconds)
    {
        Cost c;
        probe::Rig on(kFs, kBlock), off(kFs, kBlock), fon(kFs, kBlock), foff(kFs, kBlock);
        if (!on.load(files) || !off.load(files, nullptr, false) || !fon.load(files) || !foff.load(files, nullptr, false))
            return c;
        c.loaded = true;
        const std::vector<probe::Note> notes = chord(voices);
        for (probe::Rig* r : { &on, &off, &fon, &foff })
            r->run(costInput(4096), kBlock, notes);
        fon.freeze();
        foff.freeze();
        const probe::Stereo in = costInput(static_cast<std::size_t>(seconds * kFs));
        // The same bits: each pair from the program as new, its notes struck again.
        for (probe::Rig* r : { &on, &off, &fon, &foff })
            r->engine().restart();
        const probe::Stereo a = on.run(in, kBlock, notes), b = off.run(in, kBlock, notes);
        c.sameBits = probe::sameBits(a.l, b.l) && probe::sameBits(a.r, b.r);
        fon.freeze();
        foff.freeze();
        fon.run(costInput(kBlock), kBlock);               // the copies taken at a block's start
        foff.run(costInput(kBlock), kBlock);
        const probe::Stereo fa = fon.run(in, kBlock, notes), fb = foff.run(in, kBlock, notes);
        c.sameFrozenBits = fon.engine().frozen() && foff.engine().frozen() && probe::sameBits(fa.l, fb.l) && probe::sameBits(fa.r, fb.r);
        const std::vector<double> t = timeEach({ &on, &off, &fon, &foff }, in, notes, 7);
        c.on = t[0];
        c.off = t[1];
        c.frozenOn = t[2];
        c.frozenOff = t[3];
        on.engine().restart();
        c.taken = mostTaken(on, in, notes);
        return c;
    }

    // A program's files from a folder: program.fm first, then the others by name.
    pg::Sources folderProgram(const std::filesystem::path& dir)
    {
        pg::Sources files;
        std::vector<std::filesystem::path> others;
        for (const auto& e : std::filesystem::directory_iterator(dir))
            if (e.is_regular_file())
                others.push_back(e.path());
        std::sort(others.begin(), others.end());
        const auto add = [&files](const std::filesystem::path& p) {
            std::ifstream f(p, std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            files.push_back({ p.filename().string(), ss.str() });
        };
        for (const auto& p : others)
            if (p.filename() == pg::kProgramName)
                add(p);
        for (const auto& p : others)
        {
            const std::string ext = p.extension().string();
            if (p.filename() != pg::kProgramName && (ext == ".fm" || ext == ".cir" || ext == ".gui" || ext == ".sc"))
                add(p);
        }
        return files;
    }
}

PROBE(dsp, budget)
{
    const std::size_t second = static_cast<std::size_t>(kFs);

    // ---- PROCESS ------------------------------------------------------------------------------------------------------
    {
        pg::Engine engine;
        engine.prepare(kFs, kBlock);
        std::string errors;
        engine.offer(compiled(program(kBlowDial, kBlow), 1, &errors));
        engine.prepare(kFs, kBlock);
        P.eq("process.compiles", engine.playingPatch() != nullptr ? 1 : 0, 1);
        if (!errors.empty())
            std::printf("%s", errors.c_str());
        const probe::Stereo in = sineIn(second);
        const std::size_t blowAt = 40 * kBlock;
        std::size_t stoppedAt = static_cast<std::size_t>(-1);
        const auto t0 = Clock::now();
        const probe::Stereo out = play(engine, in, blowAt, kInstrument, &stoppedAt);
        const double ms = msSince(t0);
        pg::Engine::Stopped st;
        const bool read = engine.lastStop(st);
        P.eq("process.stops", engine.stops() == 1 && read ? 1 : 0, 1);
        P.eq("process.call", static_cast<int>(st.call), static_cast<int>(pg::Patch::Method::process));
        P.eq("process.not_reverted", st.reverted ? 1 : 0, 0);
        P.eq("process.patch_stopped", engine.playingPatch() != nullptr
                                          && engine.playingPatch()->stopped() == pg::Patch::Method::process ? 1 : 0, 1);
        P.eq("process.at_the_blow", static_cast<int64_t>(stoppedAt), static_cast<int64_t>(blowAt));
        P.ge("process.at_sample", static_cast<double>(st.at), static_cast<double>(blowAt));
        P.le("process.at_sample_by", static_cast<double>(st.at), static_cast<double>(blowAt + kBlock));
        P.eq("process.finite", probe::allFinite(out.l) && probe::allFinite(out.r) ? 1 : 0, 1);
        // No step beyond the signal's own: a sine of 220 Hz at 0.5 steps 0.0144 at most (the synth's 0.3 at 262 Hz less).
        P.le("process.no_step", maxStepOf(out, blowAt - kBlock, 6 * kBlock), 0.03);
        // Then the input exactly (an effect), silence exactly (an instrument), from 20 ms after.
        const std::size_t after = blowAt + kBlock + 960 + 64;
        bool exact = true;
        for (std::size_t i = after; i < out.size() && exact; ++i)
            exact = kInstrument ? out.l[i] == 0.0f && out.r[i] == 0.0f : out.l[i] == in.l[i] && out.r[i] == in.r[i];
        P.eq(kInstrument ? "process.then_silent" : "process.then_its_input", exact ? 1 : 0, 1);
        // Called no more: the second's blocks after the stop take no budget's time (one stopped call took up to it).
        const auto t1 = Clock::now();
        play(engine, sineIn(second), 0, kInstrument);    // the dial on, a note again: nothing starts on it
        const double afterMs = msSince(t1);
        P.le("process.called_no_more_ms", afterMs, 200.0);
        std::printf("process: the stopped block and the second around it took %.0f ms; a second after it %.1f ms\n", ms, afterMs);
        if constexpr (kInstrument)
        {
            const probe::Stereo again = play(engine, sineIn(kBlock * 8), static_cast<std::size_t>(-1), true);
            P.le("process.no_note_on_it", probe::peak(again.l), 0.0);
        }
    }

    // ---- PARALLEL (an effect's: regions in a voice's calls cost more than they split, ChatPrompt.cpp) ----------------
    if constexpr (!kInstrument)
    {
        pg::Engine engine;
        engine.prepare(kFs, kBlock);
        std::string errors;
        engine.offer(compiled("module Effect\n{\n    float acc[4];\n\n"
                              "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                              "        parallel for (int k = 0; k < 4; ++k)\n        {\n            while (1)\n"
                              "                acc[k] = acc[k] + 1.0f;\n        }\n    }\n};\n",
                              1, &errors));
        engine.prepare(kFs, kBlock);
        P.eq("parallel.compiles", engine.playingPatch() != nullptr ? 1 : 0, 1);
        if (!errors.empty())
            std::printf("%s", errors.c_str());
        const auto t0 = Clock::now();
        const probe::Stereo out = play(engine, sineIn(4 * kBlock));
        std::printf("parallel: a region whose parts never end stopped in %.0f ms\n", msSince(t0));
        pg::Engine::Stopped st;
        P.eq("parallel.stops", engine.lastStop(st) && st.call == pg::Patch::Method::process ? 1 : 0, 1);
        P.eq("parallel.finite", probe::allFinite(out.l) ? 1 : 0, 1);
    }

    // ---- INIT and END -----------------------------------------------------------------------------------------------
    {
        const std::string init = program(kSpinField, "", "    void init(float rate)\n    {\n" + std::string(kSpin) + "    }\n\n");
        auto t0 = Clock::now();
        pg::Patch::Result r = pg::Patch::compile(pg::singleFile(init), static_cast<float>(kFs));
        const double initMs = msSince(t0);
        P.eq("init.fails", r.patch == nullptr ? 1 : 0, 1);
        P.eq("init.stopped", static_cast<int>(r.stopped), static_cast<int>(pg::Patch::Method::init));
        const std::string at = std::string(pg::kProgramName) + ":" + positionOf(init, "void init(", "init");
        P.eq("init.message", r.errors.rfind(at + ": error: init() ran past its budget: a loop that never ends?\n", 0) == 0 ? 1 : 0, 1);
        P.le("init.seconds", initMs / 1000.0, kSecondsAtMost);
        std::printf("init: %s(stopped in %.0f ms)\n", r.errors.c_str(), initMs);

        const std::string shared = "module Shared\n{\n    int spins;\n\n    void init(float rate)\n    {\n" + std::string(kSpin)
                                 + "    }\n};\n\n" + program("    Shared* shared;\n\n", "");
        t0 = Clock::now();
        r = pg::Patch::compile(pg::singleFile(shared), static_cast<float>(kFs));
        const double sharedMs = msSince(t0);
        P.eq("shared_init.fails", r.patch == nullptr ? 1 : 0, 1);
        P.eq("shared_init.stopped", static_cast<int>(r.stopped), static_cast<int>(pg::Patch::Method::sharedInit));
        const std::string sat = std::string(pg::kProgramName) + ":" + positionOf(shared, "void init(", "init");
        P.eq("shared_init.message",
             r.errors.rfind(sat + ": error: Shared's init() ran past its budget: a loop that never ends?\n", 0) == 0 ? 1 : 0, 1);
        P.le("shared_init.seconds", sharedMs / 1000.0, kSecondsAtMost);
        std::printf("shared init: stopped in %.0f ms\n", sharedMs);

        // An end that never ends: the first instance's is stopped, and no other end is called.
        const std::string end = program(kSpinField, "", "    void end()\n    {\n" + std::string(kSpin) + "    }\n\n");
        r = pg::Patch::compile(pg::singleFile(end), static_cast<float>(kFs));
        P.eq("end.compiles", r.patch != nullptr ? 1 : 0, 1);
        t0 = Clock::now();
        r.patch.reset();
        const double endMs = msSince(t0);
        P.le("end.seconds", endMs / 1000.0, kSecondsAtMost);
        std::printf("end: destroyed in %.0f ms\n", endMs);
    }

    // ---- FIRST --------------------------------------------------------------------------------------------------------
    {
        // A, which plays; B, whose process (an instrument: its start) never ends.
        const std::string a = program("", "");
        const std::string b = kInstrument ? voice(kSpinField, "", kSpin) : program(kSpinField, kSpin);
        pg::Engine engine, alone;
        for (pg::Engine* e : { &engine, &alone })
        {
            e->prepare(kFs, kBlock);
            e->offer(compiled(a, 1));
            e->prepare(kFs, kBlock);
        }
        const probe::Stereo in = sineIn(second);
        const std::size_t half = second / 2 / kBlock * kBlock;
        probe::Stereo first(half), rest(in.size() - half);
        std::copy_n(in.l.begin(), half, first.l.begin());
        std::copy_n(in.r.begin(), half, first.r.begin());
        std::copy(in.l.begin() + static_cast<std::ptrdiff_t>(half), in.l.end(), rest.l.begin());
        std::copy(in.r.begin() + static_cast<std::ptrdiff_t>(half), in.r.end(), rest.r.begin());
        const probe::Stereo o1 = play(engine, first, static_cast<std::size_t>(-1), kInstrument);
        engine.offer(compiled(b, 2), true);
        const probe::Stereo o2 = play(engine, rest);
        const probe::Stereo r1 = play(alone, first, static_cast<std::size_t>(-1), kInstrument);
        const probe::Stereo r2 = play(alone, rest);
        pg::Engine::Stopped st;
        P.eq("first.stops", engine.lastStop(st) && st.program == 2 ? 1 : 0, 1);
        P.eq("first.call", static_cast<int>(st.call), static_cast<int>(kInstrument ? pg::Patch::Method::start : pg::Patch::Method::process));
        P.eq("first.reverted", st.reverted ? 1 : 0, 1);
        P.eq("first.at_its_first_block", static_cast<int64_t>(st.at), static_cast<int64_t>(half));
        P.eq("first.previous_plays", engine.playingPatch() != nullptr ? static_cast<int>(engine.playingPatch()->program()) : 0, 1);
        P.eq("first.settle_says_reverted", engine.settle(2) ? 1 : 0, 0);
        if constexpr (kInstrument)
        {
            // Its held key sounds again on it: a sine at 0.3 from the block after (its voice there restarted).
            P.ge("first.held_key_sounds_db", probe::rmsDb(o2.l, 4 * kBlock, rest.size() - 4 * kBlock), -15.0);
            P.le("first.no_step", maxStepOf(o2, 0, 8 * kBlock), 0.03);
        }
        else
            P.eq("first.same_bits_as_alone", probe::sameBits(o2.l, r2.l) && probe::sameBits(o2.r, r2.r) ? 1 : 0, 1);
        (void) o1;
        (void) r1;

        // Not revertible (compileNow's): it stops, and fades out (its input, or silence).
        engine.offer(compiled(b, 3));
        const probe::Stereo o3 = play(engine, rest);
        P.eq("first.unrevertible_stops", engine.lastStop(st) && st.program == 3 && !st.reverted ? 1 : 0, 1);
        P.le("first.unrevertible_no_step", maxStepOf(o3, 0, 8 * kBlock), 0.03);
        bool exact = true;
        for (std::size_t i = 6 * kBlock; i < o3.size() && exact; ++i)
            exact = kInstrument ? o3.l[i] == 0.0f : o3.l[i] == rest.l[i];
        P.eq("first.unrevertible_then_input_or_silence", exact ? 1 : 0, 1);
    }

    // ---- RUN: through a Compiler, its audio thread running ----------------------------------------------------------
    {
        pg::Engine engine;
        engine.prepare(kFs, kBlock);
        pg::Compiler compiler(engine);
        compiler.setFreezing(false);
        compiler.setSelfTesting(false);
        std::atomic<bool> done{ false };
        std::atomic<float> blow{ 0.0f };
        std::atomic<double> level{ 0.0 };                // the last block's RMS
        std::thread audio([&] {
            const probe::Stereo in = sineIn(kBlock);
            probe::Stereo out(kBlock);
            bool struck = false;
            while (!done.load())
            {
                out = in;
                std::array<float, pg::kMaxControls> norm{};
                norm[0] = blow.load();
                engine.setControls(norm);
                std::vector<pg::Event> events;
                if (kInstrument && !struck && engine.playing())
                {
                    pg::Event e;
                    e.kind = pg::EventKind::noteOn;
                    e.note = 60;
                    e.velocity = 0.8f;
                    events.push_back(e);
                    struck = true;
                }
                float* ch[2] = { out.l.data(), out.r.data() };
                engine.process(ch, 2, kBlock, events);
                double s = 0.0;
                for (const float x : out.l)
                    s += static_cast<double>(x) * static_cast<double>(x);
                level.store(std::sqrt(s / kBlock));
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
        const auto settle = [&] { std::this_thread::sleep_for(std::chrono::milliseconds(120)); };
        const pg::Sources fa = pg::singleFile(program("", ""));
        const pg::CompileStatus sa = compiler.compileNow(fa);
        P.eq("run.a_plays", sa.ok ? 1 : 0, 1);
        settle();
        // A RUN of a program that stops at once: it fails, A plays on.
        const std::string bText = kInstrument ? voice(kSpinField, kSpin) : program(kSpinField, kSpin);
        compiler.request(pg::singleFile(bText));
        compiler.wait();
        const pg::CompileStatus sb = compiler.status();
        P.eq("run.at_once_fails", !sb.ok && sb.stopped ? 1 : 0, 1);
        P.eq("run.at_once_a_plays_on", static_cast<int64_t>(sb.program), static_cast<int64_t>(sa.program));
        const std::string where = std::string(pg::kProgramName) + ":"
                                + positionOf(bText, kInstrument ? "bool process(" : "void process(", "process");
        P.eq("run.at_once_message",
             sb.errors.rfind(where + ": error: process() ran past its budget: a loop that never ends?\n", 0) == 0 ? 1 : 0, 1);
        std::printf("run: %s", sb.errors.c_str());
        settle();
        pg::Meters m;
        P.eq("run.at_once_engine_plays_a", engine.meters(m) && m.program == sa.program ? 1 : 0, 1);
        P.ge("run.at_once_sounding_db", probe::dbFromLin(level.load()), -30.0);
        // A RUN of one that stops when its dial turns: it plays, then it stops, and A plays again.
        const pg::Sources fc = pg::singleFile(program(kBlowDial, kBlow));
        compiler.request(fc);
        compiler.wait();
        const pg::CompileStatus sc = compiler.status();
        P.eq("run.later_plays", sc.ok ? 1 : 0, 1);
        settle();
        blow.store(1.0f);
        pg::CompileStatus later;
        for (int k = 0; k < 300; ++k)
        {
            later = compiler.status();
            if (later.stopped)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        blow.store(0.0f);
        P.eq("run.later_says_stopped", !later.ok && later.stopped ? 1 : 0, 1);
        P.eq("run.later_stopped_program", static_cast<int64_t>(later.stoppedProgram), static_cast<int64_t>(sc.program));
        P.eq("run.later_restored_a", later.restored == fa ? 1 : 0, 1);
        P.eq("run.later_new_program", later.program != sc.program && later.program != sa.program ? 1 : 0, 1);
        P.eq("run.later_message", later.errors.find("process() ran past its budget: a loop that never ends?") != std::string::npos ? 1 : 0, 1);
        settle();
        P.eq("run.later_engine_plays_it", engine.meters(m) && m.program == later.program ? 1 : 0, 1);
        if constexpr (kInstrument)
        {
            // The held key starts again on the program played again.
            P.ge("run.later_sounding_db", probe::dbFromLin(level.load()), -30.0);
        }
        else
            P.ge("run.later_sounding_db", probe::dbFromLin(level.load()), -30.0);
        done.store(true);
        audio.join();
    }

    // ---- WORDS: measure_output and trace ---------------------------------------------------------------------------
    {
        const pg::Sources files = pg::singleFile(program(kSpinField, kSpinLater));
        pg::measure::Request req;
        req.holdSeconds = 0.5f;
        req.tailSeconds = 0.2f;
        const std::string report = pg::measure::run(files, req);
        P.eq("words.measure_begins_with_it", report.rfind("Stopped: " + std::string(pg::kProgramName) + ":", 0) == 0 ? 1 : 0, 1);
        const std::size_t when = report.find("It stopped ");
        const double seconds = when == std::string::npos ? -1.0 : std::atof(report.c_str() + when + 11);
        P.in("words.measure_when_s", seconds, 0.24, 0.27);
        std::printf("measure: %s", report.substr(0, report.find('\n', when == std::string::npos ? 0 : when) + 1).c_str());

        pg::measure::TraceRequest tq;
        tq.render = req;
        tq.find = pg::measure::TraceFind::any;
        const pg::measure::TraceResult tr = pg::measure::trace(files, tq);
        P.eq("words.trace_call", static_cast<int>(tr.stopCall), static_cast<int>(pg::Patch::Method::process));
        bool line = false;
        for (const std::string& l : tr.lines)
            line = line || l.rfind("Stopped: ", 0) == 0;
        P.eq("words.trace_line", line ? 1 : 0, 1);
    }

    // ---- COST ---------------------------------------------------------------------------------------------------------
    {
        double on = 0, off = 0, fon = 0, foff = 0, most = 0;
        std::string mostName;
        int same = 0, sameFrozen = 0, count = 0;
        const int voices = kInstrument ? 8 : 1;
        for (const pg::Example& e : pg::kExamples)
        {
            const Cost c = costOf(pg::withLibraryIncludes(pg::singleFile(e.source)), voices, 2.0);
            if (!c.loaded)
                continue;
            ++count;
            same += c.sameBits ? 1 : 0;
            sameFrozen += c.sameFrozenBits ? 1 : 0;
            on += c.on;
            off += c.off;
            fon += c.frozenOn;
            foff += c.frozenOff;
            if (c.taken > most)
            {
                most = c.taken;
                mostName = e.title;
            }
            std::printf("cost: %-16s %7.2f ms with the budget, %7.2f without (%+.1f%%); frozen %7.2f, %7.2f (%+.1f%%); "
                        "%s; the most a call took: %.4f%% of its budget\n",
                        e.title, c.on, c.off, 100.0 * (c.on / std::max(c.off, 1e-9) - 1.0), c.frozenOn, c.frozenOff,
                        100.0 * (c.frozenOn / std::max(c.frozenOff, 1e-9) - 1.0),
                        c.sameBits && c.sameFrozenBits ? "the same bits" : "other bits", 100.0 * c.taken);
        }
        P.eq("cost.examples_same_bits", same, count);
        P.eq("cost.examples_frozen_same_bits", sameFrozen, count);
        P.le("cost.examples_ratio", on / std::max(off, 1e-9), kCostRatio);
        P.le("cost.examples_frozen_ratio", fon / std::max(foff, 1e-9), kCostRatio);
        P.le("cost.most_of_a_budget", most, 0.01);
        std::printf("cost: the examples (%d, two seconds each at 48 kHz%s): %.1f ms with the budget, %.1f without (%+.2f%%); "
                    "frozen %.1f, %.1f (%+.2f%%); the most a call took of its budget: %.4f%% (%s)\n",
                    count, kInstrument ? ", eight voices" : "", on, off, 100.0 * (on / std::max(off, 1e-9) - 1.0), fon, foff,
                    100.0 * (fon / std::max(foff, 1e-9) - 1.0), 100.0 * most, mostName.c_str());
        if (const char* dir = std::getenv("PROBE_BUDGET_PROGRAM"); dir != nullptr && *dir != '\0' && std::filesystem::is_directory(dir))
        {
            const pg::Sources files = folderProgram(dir);
            for (const int v : { 1, voices })
            {
                const Cost c = costOf(files, v, 4.0);
                if (!c.loaded)
                {
                    std::printf("cost: the folder's program did not compile\n");
                    break;
                }
                std::printf("cost: the folder's program (%zu files, %d voice%s, four seconds): %.1f ms with the budget, %.1f "
                            "without (%+.2f%%); frozen %.1f, %.1f (%+.2f%%); %s; the most a call took: %.4f%% of its budget\n",
                            files.size(), v, v == 1 ? "" : "s", c.on, c.off, 100.0 * (c.on / std::max(c.off, 1e-9) - 1.0),
                            c.frozenOn, c.frozenOff, 100.0 * (c.frozenOn / std::max(c.frozenOff, 1e-9) - 1.0),
                            c.sameBits && c.sameFrozenBits ? "the same bits" : "other bits", 100.0 * c.taken);
                if (!kInstrument)
                    break;
            }
        }
    }
    return P.finish();
}
