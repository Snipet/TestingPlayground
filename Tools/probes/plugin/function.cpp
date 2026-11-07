// PROBE name=plugin.function timeout=120
//
// plugin.function: run_function (plugin/ChatFunction.cpp, fminus/PatchCall.cpp) through the agent (ChatAgent::mcpCall)
// over a real processor, which a host thread plays as an audio thread would (48 kHz, blocks of 256; an instrument's note
// held):
//   CALL     a function of each scalar type, bit for bit as written (a float's tanhf, an int, a double's 17 digits, a
//            bool); arrays: one written in place and given back, a buffer of {extent} filled, a const one read, one
//            written past what it was given (said so), one of a single element written at the length given beside it
//            (its buffer that long: the write landed in it, said so); a method on a fresh instance of the editor's
//            files (init run at the host's rate, the dials as they are: an int dial set to 5 reads 5), a field's method
//            by its path (and an array's element), the fields it changed (counter.n: 0 -> 1), another module's on an
//            instance of its own; the processor's program untouched (its state counts on)
//   STOP     a runaway method stopped by the loop budget within seconds, in words, an error; an index past an array
//            stopped by the bounds check, the access named
//   REFUSE   a signature mismatch (the count, a bool for a float, 1.5 for an int, a number for an array, an array for a
//            float), a name the program lacks, a method named as a function (its module named), the plugin's own
//            playground_process, a path through a field that is no module, an index past an array of modules, a
//            pointer to a module, args that are no list, on that is neither word, a length past what an array holds
//            beside an array: nothing called; editor files that do not compile: their errors
//   PLAYING  on: playing, a copy of the state that plays made by the audio thread at a block's end: the effect's sample
//            count (or the held voice's, its note named) larger on a later call; a method that zeroes it run on a copy,
//            and the state that plays counts on (never written); Shared reached through the copy's own Shared (its
//            pointer repointed), and Shared's own method by shared-> (and Shared:: on a fresh one); a program whose init
//            allocated a block (a pointer not null) refused, saying why; an instrument with no voice sounding refused; a
//            host that has stopped calling the plugin: refused after the wait, nothing copied, while a function runs
//            (no copy asked) and a call with a wrong count says so (checked before a copy is asked); a newer run
//            offered while the swap to the last one is under way (blocks of 16 samples, slowly): the copy is of the
//            program before, refused, and once the newest plays, its own code on its own state
#include "Probe.h"

#include "plugin/ChatAgent.h"
#include "plugin/Processor.h"
#include "plugin/ProjectStore.h"

#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

namespace pg = playground;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr int kBlock = 256;
    constexpr bool kInstrument = pg::product::kIsInstrument;
    constexpr int kNote = 60;

    juce::var object(std::initializer_list<std::pair<const char*, juce::var>> fields)
    {
        auto* o = new juce::DynamicObject();
        for (const auto& [k, v] : fields)
            o->setProperty(k, v);
        return juce::var(o);
    }

    juce::var list(std::initializer_list<juce::var> items)
    {
        juce::Array<juce::var> a;
        for (const juce::var& v : items)
            a.add(v);
        return juce::var(a);
    }

    // The tool's own text: its last block (a line saying what changed may come first).
    std::string resultOf(const pg::ChatAgent::McpResult& r)
    {
        const juce::Array<juce::var>* c = r.content.getArray();
        return c != nullptr && !c->isEmpty() ? c->getLast()["text"].toString().toStdString() : std::string();
    }

    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    // The number after "Returned " (NaN when there is none).
    double returned(const std::string& s)
    {
        const std::size_t at = s.find("Returned ");
        return at == std::string::npos ? std::nan("") : std::strtod(s.c_str() + at + 9, nullptr);
    }

    // The main module of each kind, around the same fields and methods.
    std::string mainModule(const std::string& body)
    {
        if constexpr (kInstrument)
            return "module Voice\n{\n" + body
                 + "    bool held;\n"
                   "    void start(float note, float velocity) { held = true; }\n"
                   "    void stop() { held = false; }\n"
                   "    bool process(float* restrict left, float* restrict right, int n) { count = count + n; return held; }\n"
                   "};\n";
        else
            return "module Effect\n{\n" + body
                 + "    void process(float* restrict left, float* restrict right, int n) { count = count + n; }\n};\n";
    }

    std::string program()
    {
        return "module Counter\n{\n    int n;\n    int bump() { n = n + 1; return n; }\n};\n\n"
               "float shape(float x, float drive) { return tanhf(x * drive); }\n"
               "int twice(int n) { return 2 * n; }\n"
               "double third(double x) { return x / 3.0; }\n"
               "bool positive(float x) { return x > 0; }\n"
               "void scale(float* x, int n, float k) { for (int i = 0; i < n; ++i) x[i] = x[i] * k; }\n"
               "void ramp(float* out, int n) { for (int i = 0; i < n; ++i) out[i] = i; }\n"
               "void spill(float* x) { for (int i = 0; i < 6; ++i) x[i] = 1; }\n"
               "float sum(const float* x, int n) { float s = 0; for (int i = 0; i < n; ++i) s = s + x[i]; return s; }\n"
               "void last(float* x, int n) { x[n - 1] = 1; }\n\n"
             + mainModule("    param int steps [[fminus::range(0, 8)]] [[fminus::default(3)]];\n"
                          "    Counter counter;\n    Counter many[3];\n    float rate;\n    int count;\n    int spins;\n"
                          "    float taps[4];\n"
                          "    void init(float r) { rate = r; }\n"
                          "    float getRate() { return rate; }\n"
                          "    int stepsNow() { return steps[0]; }\n"
                          "    int seen() { return count; }\n"
                          "    void forget() { count = 0; }\n"
                          "    void spin() { while (1) spins = spins + 1; }\n"
                          "    float tap(int i) { return taps[i]; }\n");
    }

    // Shared's one, reached through the main module's pointer.
    std::string sharedProgram()
    {
        return "module Shared\n{\n    int built;\n    void init(float r) { built = 7; }\n    int twiceBuilt() { return 2 * built; }\n};\n\n"
             + mainModule("    Shared* shared;\n    int count;\n    int fromShared() { return shared->built; }\n");
    }

    // A block its init allocated: a pointer that is not null in the state that plays.
    std::string heapProgram()
    {
        return mainModule("    float* buffer;\n    int count;\n"
                          "    void init(float r) { buffer = calloc(16, sizeof(float)); }\n"
                          "    void end() { free(buffer); }\n"
                          "    int seen() { return count; }\n");
    }

    // One of a few versions of a program, all laid out alike: only what version() returns differs.
    std::string versionProgram(int v)
    {
        return mainModule("    int count;\n    int version() { return " + std::to_string(v) + "; }\n");
    }

    bool load(pg::Processor& p, const std::string& text)
    {
        pg::Draft d;
        d.files = pg::singleFile(text);
        p.editorPort().setDraft(d);
        p.editorPort().run(d.files);
        p.settle();
        return p.editorPort().status().ok;
    }

    // A host that plays the processor on a thread of its own, as fast as a block a millisecond (or blocks of `size`
    // samples, one each `pauseMs`): an instrument's note held from its first block (while `note`; else every note let
    // go there).
    struct Host
    {
        pg::Processor& p;
        bool note;
        int size, pauseMs;
        std::atomic<bool> stop{ false };
        std::atomic<int> blocks{ 0 };
        std::thread thread;

        Host(pg::Processor& proc, bool n, int blockSize = kBlock, int pause = 1) : p(proc), note(n), size(blockSize), pauseMs(pause)
        {
            thread = std::thread([this] { run(); });
        }
        ~Host() { end(); }
        Host(const Host&) = delete;
        Host& operator=(const Host&) = delete;

        void end()
        {
            stop.store(true);
            if (thread.joinable())
                thread.join();
        }

        void run()
        {
            juce::AudioBuffer<float> buffer(2, size);
            for (int b = 0; !stop.load(); ++b)
            {
                buffer.clear();
                juce::MidiBuffer midi;
                if (kInstrument && b == 0)             // the note held, or every note let go
                    midi.addEvent(note ? juce::MidiMessage::noteOn(1, kNote, static_cast<juce::uint8>(100))
                                       : juce::MidiMessage::allNotesOff(1), 0);
                p.processBlock(buffer, midi);
                blocks.store(b + 1);
                std::this_thread::sleep_for(std::chrono::milliseconds(pauseMs));
            }
        }

        void waitBlocks(int n) const
        {
            const int until = blocks.load() + n;
            while (blocks.load() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
}

PROBE(plugin, function)
{
    const funkgui::HeadlessGuiScope gui;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const juce::File projects = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                    .getChildFile(juce::String(pg::product::kEnvPrefix) + "function-projects-" + juce::String(getpid()));
    projects.deleteRecursively();
    pg::projectstore::setDirectoryForTests(projects);

    auto owned = std::make_unique<pg::Processor>();
    pg::Processor& p = *owned;
    p.setRateAndBufferSizeDetails(kFs, kBlock);
    p.prepareToPlay(kFs, kBlock);
    pg::ChatAgent& agent = p.chatAgent();
    const std::string kWord = kInstrument ? "Voice" : "Effect";
    const juce::String kMain(kWord);
    const auto call = [&](juce::var args, bool* error = nullptr) {
        const pg::ChatAgent::McpResult r = agent.mcpCall("run_function", args);
        if (error != nullptr)
            *error = r.isError;
        const std::string t = resultOf(r);
        std::printf("run_function %s:\n  %s\n", juce::JSON::toString(args, true).toRawUTF8(), t.c_str());
        return t;
    };
    P.eq("call.program_compiles", load(p, program()) ? 1 : 0, 1);

    // ---- CALL ---------------------------------------------------------------------------------------------------------
    {
        bool error = true;
        std::string t = call(object({ { "name", "shape" }, { "args", list({ 0.5, 2 }) } }), &error);
        P.near("call.float", returned(t), static_cast<double>(std::tanh(1.0f)), 1e-6);
        P.eq("call.float_words", !error && has(t, "Ran float shape(float x, float drive) on a function: on its arguments") ? 1 : 0, 1);
        t = call(object({ { "name", "twice" }, { "args", list({ 7 }) } }));
        P.eq("call.int", has(t, "Returned 14.") ? 1 : 0, 1);
        t = call(object({ { "name", "third" }, { "args", list({ 1 }) } }));
        P.eq("call.double", has(t, "Returned 0.33333333333333331.") ? 1 : 0, 1);
        t = call(object({ { "name", "positive" }, { "args", list({ -0.25 }) } }));
        P.eq("call.bool", has(t, "Returned false.") ? 1 : 0, 1);

        t = call(object({ { "name", "scale" }, { "args", list({ list({ 1, -2, 0.5 }), 3, 2 }) } }));
        P.eq("call.array_written", has(t, "x (float *), after the call: [2, -4, 1]") && has(t, "Returned nothing (void).") ? 1 : 0, 1);
        t = call(object({ { "name", "ramp" }, { "args", list({ object({ { "extent", 8 } }), 8 }) } }));
        P.eq("call.extent", has(t, "out (float *), after the call: [0, 1, 2, 3, 4, 5, 6, 7]") ? 1 : 0, 1);
        t = call(object({ { "name", "sum" }, { "args", list({ object({ { "extent", 4 }, { "values", list({ 1, 2 }) } }), 4 }) } }));
        P.eq("call.const_array", has(t, "Returned 3.") && has(t, "x (const float *): [1, 2, 0, 0]") ? 1 : 0, 1);
        t = call(object({ { "name", "spill" }, { "args", list({ list({ 0, 0 }) }) } }));
        P.eq("call.past", has(t, "[1, 1]") && has(t, "It wrote past the 2 elements given") ? 1 : 0, 1);
        // C's (buffer, n) with a buffer of one: the buffer is n long, so x[n - 1] lands in it (and is seen there), far past
        // the 1,024 zeros after the one element.
        t = call(object({ { "name", "last" }, { "args", list({ list({ 0 }), 5000 }) } }), &error);
        P.eq("call.length_covered", !error && has(t, "x (float *), after the call: [0]") && has(t, "It wrote past the 1 elements given")
                                        ? 1 : 0, 1);

        t = call(object({ { "name", kMain + "::getRate" } }));
        P.eq("call.method_fresh", returned(t) == kFs && has(t, "on a fresh " + kWord + " of the editor's files") && has(t, "init(48000) run")
                                      ? 1 : 0, 1);
        t = call(object({ { "name", kMain + "::stepsNow" } }));
        P.eq("call.dial_default", has(t, "Returned 3.") ? 1 : 0, 1);
        agent.mcpCall("set_dial", object({ { "name", "steps" }, { "value", 5 } }));
        t = call(object({ { "name", kMain + "::stepsNow" } }));
        P.eq("call.dial_as_set", has(t, "Returned 5.") ? 1 : 0, 1);
        p.poll();

        t = call(object({ { "name", "counter.bump" } }));
        P.eq("call.field_path", has(t, "Returned 1.") && has(t, "its field counter") && has(t, "counter.n: 0 -> 1") ? 1 : 0, 1);
        t = call(object({ { "name", "many[2].bump" } }));
        P.eq("call.element_path", has(t, "Returned 1.") && has(t, "many[2].n: 0 -> 1") ? 1 : 0, 1);
        t = call(object({ { "name", "Counter::bump" } }));
        P.eq("call.own_instance", has(t, "Returned 1.") && has(t, "a Counter of its own") ? 1 : 0, 1);
    }

    // ---- STOP ---------------------------------------------------------------------------------------------------------
    {
        bool error = false;
        const auto t0 = std::chrono::steady_clock::now();
        const std::string t = call(object({ { "name", kMain + "::spin" } }), &error);
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("spin stopped after %.2f s\n", took);
        P.eq("stop.runaway", error && has(t, "It stopped:") && has(t, "loop budget") ? 1 : 0, 1);
        P.le("stop.runaway_seconds", took, 20.0);
        std::string b = call(object({ { "name", kMain + "::tap" }, { "args", list({ 9 }) } }), &error);
        P.eq("stop.bounds", error && has(b, "It stopped:") && has(b, "index 9 out of bounds for 'taps'") ? 1 : 0, 1);
        b = call(object({ { "name", kMain + "::tap" }, { "args", list({ 2 }) } }), &error);
        P.eq("stop.bounds_within", !error && has(b, "Returned 0.") ? 1 : 0, 1);
    }

    // ---- REFUSE -------------------------------------------------------------------------------------------------------
    {
        const auto refused = [&](const char* row, juce::var args, const std::string& words) {
            bool error = false;
            const std::string t = call(args, &error);
            P.eq(row, error && has(t, "Nothing was called") && has(t, words) ? 1 : 0, 1);
        };
        refused("refuse.count", object({ { "name", "shape" }, { "args", list({ 0.5 }) } }), "'shape' takes 2 arguments, not 1");
        refused("refuse.bool_for_float", object({ { "name", "shape" }, { "args", list({ true, 2 }) } }),
                "argument 1 of 'shape' is true, not a 'float' (its parameter 'x', 'float')");
        refused("refuse.not_whole", object({ { "name", "twice" }, { "args", list({ 1.5 }) } }), "is 1.5, not an int");
        refused("refuse.number_for_array", object({ { "name", "scale" }, { "args", list({ 0.5, 1, 2 }) } }), "is 0.5, not an array");
        refused("refuse.array_for_float", object({ { "name", "shape" }, { "args", list({ list({ 1 }), 2 }) } }), "is an array, not a 'float'");
        refused("refuse.no_function", object({ { "name", "shapes" } }), "shapes");
        refused("refuse.method_as_function", object({ { "name", "bump" } }), "(a method of that name: Counter::bump)");
        refused("refuse.plugin_own", object({ { "name", "playground_process" } }), "the plugin's own calls");
        refused("refuse.not_module", object({ { "name", "rate.tick" } }), "'rate' is a float, not a module");
        refused("refuse.index_past", object({ { "name", "many[3].bump" } }), "index 3 is past 'many'");
        refused("refuse.length_past", object({ { "name", "ramp" }, { "args", list({ list({ 0 }), 2000000 }) } }),
                "an int argument of 2000000 beside it is past the 1048576 elements");
        bool error = false;
        std::string t = call(object({ { "name", "shape" }, { "args", 0.5 } }), &error);
        P.eq("refuse.args_not_list", error && has(t, "args is a list") ? 1 : 0, 1);
        t = call(object({ { "name", "shape" }, { "on", "later" } }), &error);
        P.eq("refuse.on_word", error && has(t, "on is fresh or playing") ? 1 : 0, 1);
        pg::Draft broken = p.editorPort().draft();
        const pg::Draft good = broken;
        broken.files.front().text += "\nfloat oops( {\n";
        p.editorPort().setDraft(broken);
        t = call(object({ { "name", "twice" }, { "args", list({ 1 }) } }), &error);
        P.eq("refuse.not_compiled", error && has(t, "did not compile here") && has(t, "error") ? 1 : 0, 1);
        p.editorPort().setDraft(good);
    }

    // ---- PLAYING ------------------------------------------------------------------------------------------------------
    {
        Host host(p, true);
        host.waitBlocks(20);
        bool error = true;
        std::string t = call(object({ { "name", kMain + "::seen" }, { "on", "playing" } }), &error);
        const double first = returned(t);
        P.ge("playing.counted", first, static_cast<double>(kBlock));
        P.eq("playing.words", !error && has(t, "a copy of") && has(t, "made by the audio thread at a block's end")
                                  && (!kInstrument || has(t, "note " + std::to_string(kNote))) ? 1 : 0, 1);
        call(object({ { "name", kMain + "::forget" }, { "on", "playing" } }));
        host.waitBlocks(10);
        t = call(object({ { "name", kMain + "::seen" }, { "on", "playing" } }));
        P.ge("playing.untouched", returned(t) - first, 10.0 * kBlock);
        t = call(object({ { "name", kMain + "::seen" } }));
        P.eq("playing.fresh_zero", has(t, "Returned 0.") ? 1 : 0, 1);
        host.end();

        Host still(p, false);
        still.end();
        t = call(object({ { "name", kMain + "::seen" }, { "on", "playing" } }), &error);
        P.eq("playing.not_processing", error && has(t, "no audio went through the plugin") ? 1 : 0, 1);
        // What needs no copy asks for none: a function runs on its arguments in the program that plays, and a call that
        // is wrong anyway says what is wrong.
        t = call(object({ { "name", "twice" }, { "args", list({ 7 }) }, { "on", "playing" } }), &error);
        P.eq("playing.function_no_copy", !error && has(t, "Returned 14.") && has(t, "in the program that plays") ? 1 : 0, 1);
        t = call(object({ { "name", "shape" }, { "args", list({ 0.5 }) }, { "on", "playing" } }), &error);
        P.eq("playing.checked_first", error && has(t, "'shape' takes 2 arguments, not 1") ? 1 : 0, 1);
    }
    {
        P.eq("playing.shared_compiles", load(p, sharedProgram()) ? 1 : 0, 1);
        Host host(p, true);
        host.waitBlocks(5);
        bool error = true;
        const std::string t = call(object({ { "name", kMain + "::fromShared" }, { "on", "playing" } }), &error);
        P.eq("playing.shared", !error && has(t, "Returned 7.") ? 1 : 0, 1);
        const std::string own = call(object({ { "name", "shared->twiceBuilt" }, { "on", "playing" } }), &error);
        P.eq("playing.shared_method", !error && has(own, "Returned 14.") && has(own, "on the Shared of a copy") ? 1 : 0, 1);
        const std::string fresh = call(object({ { "name", "Shared::twiceBuilt" } }), &error);
        P.eq("call.shared_method", !error && has(fresh, "Returned 14.") ? 1 : 0, 1);
    }
    {
        P.eq("playing.heap_compiles", load(p, heapProgram()) ? 1 : 0, 1);
        Host host(p, true);
        host.waitBlocks(5);
        bool error = false;
        std::string t = call(object({ { "name", kMain + "::seen" }, { "on", "playing" } }), &error);
        P.eq("playing.heap_refused", error && has(t, "its field buffer (float *) points at memory of the program that plays") ? 1 : 0, 1);
        t = call(object({ { "name", kMain + "::seen" } }), &error);
        P.eq("playing.heap_fresh", !error && has(t, "Returned 0.") ? 1 : 0, 1);
    }
    {
        // A run the audio thread has not taken yet: the copy is of the program before it, laid out alike, and refused.
        P.eq("playing.versions_compile", load(p, versionProgram(1)) ? 1 : 0, 1);
        {
            Host warm(p, true);
            warm.waitBlocks(10);                    // version 1 plays (an instrument's note held, its voice sounding on)
        }
        P.eq("playing.version2_compiles", load(p, versionProgram(2)) ? 1 : 0, 1);
        // Blocks of 16 samples, one each 40 ms: the first takes version 2, and the swap from version 1 (20 ms, 960
        // samples: 60 blocks) lasts 2.4 s, while a program offered waits.
        Host slow(p, true, 16, 40);
        slow.waitBlocks(2);
        P.eq("playing.version3_compiles", load(p, versionProgram(3)) ? 1 : 0, 1);
        bool error = false;
        std::string t = call(object({ { "name", kMain + "::version" }, { "on", "playing" } }), &error);
        P.eq("playing.other_program", error && has(t, "changed as it was copied") ? 1 : 0, 1);
        slow.end();
        Host fast(p, true);
        fast.waitBlocks(40);                         // the swap over, version 3 taken
        t = call(object({ { "name", kMain + "::version" }, { "on", "playing" } }), &error);
        P.eq("playing.newest_program", !error && has(t, "Returned 3.") ? 1 : 0, 1);
    }
    if constexpr (kInstrument)
    {
        P.eq("playing.silent_compiles", load(p, program()) ? 1 : 0, 1);
        Host host(p, false);
        host.waitBlocks(5);
        bool error = false;
        const std::string t = call(object({ { "name", "Voice::seen" }, { "on", "playing" } }), &error);
        P.eq("playing.no_voice", error && has(t, "No voice sounds now") ? 1 : 0, 1);
    }

    owned.reset();
    projects.deleteRecursively();
    return P.finish();
}
