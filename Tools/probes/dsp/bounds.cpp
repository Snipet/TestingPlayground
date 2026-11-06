// PROBE name=dsp.bounds timeout=900
//
// dsp.bounds: bounds checks (Patch.h, "Bounds checks"; Engine.h and Compiler.h, "A program that stops"). Programs that
// index outside their arrays, played at 48 kHz in blocks of 256:
//   CAUGHT   an out-of-range write in process (`line[at[0]] = 1.0f`, its dial turned to 16 of a float[16]): the call
//            returns before the write (the field after the array still 0), the program stops (Patch::stopped: process;
//            Patch::bounds, and the Engine's Stopped::bounds: program.fm, the line and the column of the `[`, 'line',
//            float[16], 16 elements, index 16), it fades out with no step larger than the signal's own, then is its input
//            (an effect) or silent (an instrument) exactly, and its message is at the access ("program.fm:L:C: error:
//            process() went outside an array: index 16 out of bounds for 'line' of type 'float[16]'", then a note);
//            restart() (reset()) empties FMinus's record, and the program plays again as new with its dial in range. In
//            an #include'd module (ring.fm's `taps[k] = x`): that file's name, line and array. In init: the compile fails
//            with it (Result::stopped init, Result::bounds)
//   RUN      through a Compiler with an audio thread: a RUN whose program writes outside its array at once fails (ok
//            false, stopped, bounds, the message at the access) and the program before plays on; one that does so once a
//            dial turns plays, then a status of its own says so (stopped, bounds) and the program before plays again
//   WORDS    measure_output's report begins with the message and says when; trace's lines and view_sound's words too
//   FROZEN   the frozen copy has no checks: its IR has no record and no check where the program's has both (and a
//            compile without the checks neither), and a read one past an array's end that stops the program goes through
//            in the copy, which reads the field after the array
//   MOVED    the examples whose lines moved from calloc into arrays (the effect's Echo, the synth's Pluck): their lines
//            among the program's checks, and the same bits as before the move at 44.1, 48, 96 and 192 kHz (the Echo at
//            its defaults, and straight with TIME sweeping and much feedback; the Pluck's notes from below 20 Hz up, a
//            chord and its release, at its defaults and darker and shorter). ECHO, the effect's: the Echo as it read its
//            line before (a position just under the line's end rounded up to it, one past the last sample), moved, is
//            caught at 192 kHz, where its line is the whole array, and the Echo as it is plays on there
//   COST     each example with the checks and without: the same bits; the CPU time (the thread's, the best of seven),
//            printed, the sums' ratio within kCostRatio; and the compile's time with and without them, printed
#include "Probe.h"

#include "Rig.h"

#include "dsp/Compiler.h"
#include "dsp/Debug.h"
#include "dsp/Engine.h"
#include "dsp/Measure.h"
#include "dsp/SoundView.h"
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
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;
    constexpr double kFs = 48000.0;
    constexpr int kBlock = 256;
    constexpr double kCostRatio = 1.6;            // the examples' CPU time with the checks over without, at most

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
    // with `extra` declarations and `before` at the top of process, and `hooks` (init, end) as given; the instrument's
    // start begins with `start`.
    std::string effect(const std::string& extra, const std::string& before, const std::string& hooks = {})
    {
        return "module Effect\n{\n" + extra + hooks
             + "    void process(float* restrict left, float* restrict right, int n)\n    {\n" + before
             + "        for (int i = 0; i < n; ++i)\n        {\n            left[i] = 0.5f * left[i];\n"
               "            right[i] = 0.5f * right[i];\n        }\n    }\n};\n";
    }

    std::string voice(const std::string& extra, const std::string& before, const std::string& hooks = {},
                      const std::string& start = {})
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
        return kInstrument ? voice(extra, before, hooks) : effect(extra, before, hooks);
    }

    // A write through a dial: `at` 3 writes inside line, 16 one past its end.
    const char* kAtDial = "    param int at [[fminus::range(0, 20)]] [[fminus::default(3)]];\n    float line[16];\n"
                          "    float after;\n\n";
    const char* kWrite = "        line[at[0]] = 1.0f;\n";
    // One through an #include'd module's method.
    const char* kRing = "// ring.fm: eight taps.\nmodule Ring\n{\n    float taps[8];\n\n    void put(int k, float x)\n    {\n"
                        "        taps[k] = x;\n    }\n};\n";
    const char* kRingField = "    param int at [[fminus::range(0, 20)]] [[fminus::default(3)]];\n    Ring ring;\n\n";
    const char* kRingWrite = "        ring.put(at[0], 1.0f);\n";
    // One whatever the dials say (a field init sets), and one from the 16,000th sample on.
    const char* kAtField = "    float line[16];\n    int k;\n\n";
    const char* kAtFieldInit = "    void init(float rate) { k = 16; }\n\n";
    const char* kWriteK = "        line[k] = 1.0f;\n";
    const char* kCountField = "    float line[16];\n    int count;\n\n";
    const char* kWriteLater = "        count = count + n;\n        line[count / 1000] = 1.0f;\n";
    // One in init, at 48 kHz: line[48].
    const char* kInitWrite = "    void init(float rate) { line[(int)(rate / 1000.0f)] = 1.0f; }\n\n";

    // "L:C" of `offset` bytes into the first `text` in `source`, as FMinus counts them (from 1, the column in bytes).
    std::string placeOf(const std::string& source, const std::string& text, std::size_t offset)
    {
        const std::size_t at = source.find(text);
        if (at == std::string::npos)
            return "?";
        const std::size_t p = at + offset;
        const std::size_t lineStart = source.rfind('\n', p);
        const auto line = static_cast<std::size_t>(std::count(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(p), '\n')) + 1;
        const std::size_t col = p - (lineStart == std::string::npos ? 0 : lineStart + 1) + 1;
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

    // `in` through `engine` in blocks of kBlock: dial 0 at `from` (normalised), then at `to` from the block at `at` on,
    // a note-on of 60 at the first block when `note`; the block a stop came at in `stoppedAt`.
    probe::Stereo play(pg::Engine& engine, const probe::Stereo& in, float from, float to, std::size_t at, bool note,
                       std::size_t* stoppedAt = nullptr)
    {
        probe::Stereo out = in;
        const uint64_t stops = engine.stops();
        for (std::size_t pos = 0; pos < in.size(); pos += kBlock)
        {
            const int n = static_cast<int>(std::min<std::size_t>(kBlock, in.size() - pos));
            std::array<float, pg::kMaxControls> norm{};
            norm[0] = pos >= at ? to : from;
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

    std::unique_ptr<pg::Patch> compiled(const pg::Sources& files, uint64_t id, std::string* errors = nullptr)
    {
        pg::Patch::Result r = pg::Patch::compile(files, static_cast<float>(kFs));
        if (errors != nullptr)
            *errors = r.errors + r.notes;
        if (r.patch)
            r.patch->setProgram(id);
        return std::move(r.patch);
    }

    double maxStepOf(const probe::Stereo& s, std::size_t from, std::size_t count)
    {
        return std::max(probe::maxStep(s.l, from, count), probe::maxStep(s.r, from, count));
    }

    // The field `path` in every instance of the program that plays: the largest of their values (-1: no such field).
    double fieldMost(const pg::Engine& engine, const std::string& path)
    {
        const pg::Patch* p = engine.playingPatch();
        if (p == nullptr)
            return -1.0;
        for (const pg::Patch::Field& f : p->fieldMap())
            if (f.path == path)
            {
                double most = -1e30;
                for (int i = 0; i < p->instances(); ++i)
                    most = std::max(most, p->read(f, i));
                return most;
            }
        return -1.0;
    }

    // The field `path` of the newest voice that is not fading out (an instrument), or of the effect's instance.
    double fieldNow(const pg::Engine& engine, const std::string& path)
    {
        const pg::Patch* p = engine.playingPatch();
        if (p == nullptr)
            return -1.0;
        int instance = 0;
        if constexpr (kInstrument)
        {
            std::vector<pg::Engine::VoiceInfo> vs;
            engine.voices(vs);
            const pg::Engine::VoiceInfo* newest = nullptr;
            for (const pg::Engine::VoiceInfo& v : vs)
                if (!v.fading && (newest == nullptr || v.age > newest->age))
                    newest = &v;
            if (newest == nullptr)
                return -1.0;
            instance = newest->instance;
        }
        for (const pg::Patch::Field& f : p->fieldMap())
            if (f.path == path)
                return p->read(f, instance);
        return -1.0;
    }

    // ---- MOVED ------------------------------------------------------------------------------------------------------
    // The example as it was before its lines moved into arrays (its lines from calloc), the program the move must not
    // change a bit of; and the example's title.
    [[maybe_unused]] const char* const kEchoBefore = R"FM(// Echo: a stereo delay with feedback, ping-pong or straight. Its two lines come from calloc in init and go back with
// free in end, both off the audio thread: process may not allocate, and FMinus refuses one that could. The #pragma
// panel lines lay the dials out in titled sections, as a hardware unit's panel. SYNC sets the time in notes at the
// host's tempo instead (host.fm, a file the plugin provides): 1/16 to 1/2, dotted too.
#include "host.fm"
#pragma panel section "ECHO" time sync feedback pingpong
#pragma panel section "OUTPUT" mix out
#pragma panel label pingpong "PING-PONG"
#pragma panel options sync "OFF" "1/16" "1/8" "1/8 D" "1/4" "1/4 D" "1/2"
#pragma panel style pingpong switch
#pragma panel vu out "LEVEL"
module Effect
{
    param float time [[fminus::range(10, 1000)]] [[fminus::default(350)]];       // ms
    param float feedback [[fminus::range(0, 0.95)]] [[fminus::default(0.4)]];
    param float mix [[fminus::range(0, 1)]] [[fminus::default(0.35)]];
    param bool pingpong [[fminus::default(1)]];
    param int sync [[fminus::range(0, 6)]] [[fminus::default(0)]];   // OFF (TIME), or the time in notes at the tempo
    Host* host;     // the plugin's (host.fm): the tempo
    float glide;    // the delay in samples, gliding to a new tempo or note
    float* lineL;
    float* lineR;
    int length;     // samples a line holds: two seconds and a little (a synced 1/2 note down to 60 BPM)
    int at;         // where the next sample goes
    float rate;

    void init(float sampleRate)
    {
        rate = sampleRate;
        length = 2 * (int)sampleRate + 4;
        lineL = calloc(length, sizeof(float));
        lineR = calloc(length, sizeof(float));
        if (!lineL || !lineR)
            length = 0;
    }

    void process(float* restrict left, float* restrict right, int n)
    {
        if (length == 0)
            return;
        const int s = sync[0];
        const float beats = s == 1 ? 0.25f : s == 2 ? 0.5f : s == 3 ? 0.75f : s == 4 ? 1.0f : s == 5 ? 1.5f : 2.0f;
        const float synced = beats * 60 / host->bpm * rate;                  // samples
        for (int i = 0; i < n; ++i)
        {
            // The delay in samples, between two of them: a moving TIME bends the pitch instead of clicking, and so
            // does a new tempo or note when synced (the delay glides to it over some 20 ms).
            float want = time[i] * 0.001f * rate;
            if (s > 0)
            {
                glide = glide == 0 ? synced : glide + 0.001f * (synced - glide);
                want = glide;
            }
            else
                glide = want;
            const float d = fminf(fmaxf(want, 1), (float)(length - 2));
            float pos = (float)at - d;
            if (pos < 0)
                pos += (float)length;
            int i0 = (int)pos;
            const float frac = pos - (float)i0;
            if (i0 == length)       // a position just under the line's end rounds up to it: the line's first sample
                i0 = 0;
            const int i1 = i0 + 1 == length ? 0 : i0 + 1;
            const float dl = lineL[i0] + frac * (lineL[i1] - lineL[i0]);
            const float dr = lineR[i0] + frac * (lineR[i1] - lineR[i0]);
            const float xl = left[i];
            const float xr = right[i];
            if (pingpong[i])
            {
                lineL[at] = 0.5f * (xl + xr) + feedback[i] * dr;
                lineR[at] = feedback[i] * dl;
            }
            else
            {
                lineL[at] = xl + feedback[i] * dl;
                lineR[at] = xr + feedback[i] * dr;
            }
            left[i] = xl + mix[i] * dl;
            right[i] = xr + mix[i] * dr;
            if (++at == length)
                at = 0;
        }
    }

    void end()
    {
        free(lineL);
        free(lineR);
    }
};
)FM";
    [[maybe_unused]] const char* const kPluckBefore = R"FM(// Pluck: Karplus-Strong. A burst of noise, as long as one period of the note, circulates in a delay line through a
// gentle low-pass, which makes it a decaying string. Each voice's line comes from calloc in init (long enough for
// 20 Hz) and goes back with free in end, both off the audio thread.
module Voice
{
    param float brightness [[fminus::range(0, 1)]] [[fminus::default(0.6)]];
    param float decay [[fminus::range(100, 10000)]] [[fminus::default(3000)]];   // ms, to -60 dB
    param float level [[fminus::range(-36, 0)]] [[fminus::default(-9)]];         // dB
    float* line;
    int capacity;     // samples the line holds
    int length;       // the note's period, in samples
    int at;
    int seed;         // the noise's state, 1 to 65536
    float rate;
    float loss;       // the gain a pass through the line
    float peak;       // the output's decaying peak, which ends the voice
    bool held;

    void init(float sampleRate)
    {
        rate = sampleRate;
        capacity = (int)(sampleRate / 20) + 2;
        line = calloc(capacity, sizeof(float));
        if (!line)
            capacity = 0;
        seed = 1;
    }

    void start(float note, float velocity)
    {
        if (capacity == 0)
            return;
        const float hz = 440 * expf(0.0577622650f * (note - 69));
        length = (int)fminf(fmaxf(rate / hz, 2), (float)capacity);
        for (int i = 0; i < length; ++i)
        {
            seed = seed * 75 % 65537;            // a Lehmer generator: 75 is a primitive root of 65537
            line[i] = velocity * ((float)seed / 32768.0f - 1);
        }
        at = 0;
        peak = velocity;
        held = true;
    }

    void stop()
    {
        held = false;
    }

    bool process(float* restrict left, float* restrict right, int n)
    {
        if (capacity == 0 || length == 0)
            return false;
        const float ms = held ? decay[0] : 150.0f;          // a released key mutes the string
        loss = expf(-6907.7553f * (float)length / (ms * rate));
        const float gain = expf(0.115129255f * level[0]);
        const float s = 0.5f * brightness[0];               // 0.5: the plain average; less is brighter
        for (int i = 0; i < n; ++i)
        {
            const int next = at + 1 == length ? 0 : at + 1;
            const float y = line[at];
            line[at] = loss * (y + (0.5f - s) * (line[next] - y));
            at = next;
            left[i] += gain * y;
            right[i] += gain * y;
            peak = fmaxf(fabsf(y), peak * 0.9999f);
        }
        return peak > 0.0001f;
    }

    void end()
    {
        free(line);
    }
};
)FM";
    const char* const kMovedTitle = kInstrument ? "Pluck" : "Echo";

    const char* exampleSource(const char* title)
    {
        for (const pg::Example& e : pg::kExamples)
            if (std::strcmp(e.title, title) == 0)
                return e.source;
        return nullptr;
    }

    // The Echo as it read its line before (a position just under the line's end rounded up to it, then read one past
    // its last sample), from the example as it is: empty when the example no longer has the lines to put back.
    [[maybe_unused]] std::string echoReadingPast(const std::string& now)
    {
        const std::string fixed = "            int i0 = (int)pos;\n            const float frac = pos - (float)i0;\n"
                                  "            if (i0 == length)";
        const std::size_t at = now.find(fixed);
        const std::size_t end = at == std::string::npos ? at : now.find("const int i1 = i0 + 1 == length ? 0 : i0 + 1;\n", at);
        if (end == std::string::npos)
            return {};
        const std::string before = "            const int i0 = (int)pos;\n            const int i1 = i0 + 1 == length ? 0 : i0 + 1;\n"
                                   "            const float frac = pos - (float)i0;\n";
        const std::size_t stop = end + std::strlen("const int i1 = i0 + 1 == length ? 0 : i0 + 1;\n");
        return now.substr(0, at) + before + now.substr(stop);
    }

    // The dial named `name`'s slot in `c` (-1: none).
    int slotOf(const pg::Controls& c, const char* name)
    {
        for (std::size_t k = 0; k < c.size(); ++k)
            if (c[k].used && std::strcmp(c[k].name, name) == 0)
                return static_cast<int>(k);
        return -1;
    }

    struct Played
    {
        bool loaded = false;
        probe::Stereo out;
        uint64_t stops = 0;
        pg::Engine::Stopped stop;
        std::string errors;
    };

    // The moved example's scenario `scenario` at `fs`, 2.5 s in blocks of 512. The Echo: noise at 0.25 through it, at
    // its defaults (0), or straight with feedback 0.8 and TIME sweeping 100 to 900 ms (1). The Pluck: notes from below
    // 20 Hz up (12, 40), a chord at 0.3 s (64, 67, 71), 40 let go at 1 s and the chord at 1.5 s, at its defaults (0),
    // or with brightness 0.2 and decay 500 ms (1).
    Played playMoved(const std::string& source, double fs, int scenario)
    {
        Played p;
        probe::Rig rig(fs, 512);
        if (!rig.load(pg::withLibraryIncludes(pg::singleFile(source)), &p.errors))
            return p;
        p.loaded = true;
        const auto n = static_cast<std::size_t>(2.5 * fs);
        const pg::Controls& c = rig.controls();
        const auto set = [&c](std::array<float, pg::kMaxControls>& norm, const char* name, float plain) {
            if (const int k = slotOf(c, name); k >= 0)
                norm[static_cast<std::size_t>(k)] = pg::normOf(c[static_cast<std::size_t>(k)], plain);
        };
        const uint64_t stops = rig.engine().stops();
        if constexpr (kInstrument)
        {
            const auto at = [fs](double s) { return static_cast<std::size_t>(s * fs); };
            const std::vector<probe::Note> notes = {
                { 0, pg::EventKind::noteOn, 12, 0.9f },      { 0, pg::EventKind::noteOn, 40, 0.8f },
                { at(0.3), pg::EventKind::noteOn, 64, 0.7f }, { at(0.3), pg::EventKind::noteOn, 67, 0.7f },
                { at(0.3), pg::EventKind::noteOn, 71, 0.7f }, { at(1.0), pg::EventKind::noteOff, 40, 0.0f },
                { at(1.5), pg::EventKind::noteOff, 64, 0.0f }, { at(1.5), pg::EventKind::noteOff, 67, 0.0f },
                { at(1.5), pg::EventKind::noteOff, 71, 0.0f },
            };
            if (scenario == 1)
            {
                set(rig.norm(), "brightness", 0.2f);
                set(rig.norm(), "decay", 500.0f);
            }
            p.out = rig.run(probe::Stereo(n), 512, notes);
        }
        else
        {
            const probe::Stereo in = probe::noise(n, 0.25, 77);
            if (scenario == 1)
            {
                set(rig.norm(), "pingpong", 0.0f);
                set(rig.norm(), "feedback", 0.8f);
            }
            p.out = rig.run(in, 512, {}, {}, [&](std::size_t pos, std::array<float, pg::kMaxControls>& norm) {
                if (scenario == 1)
                    set(norm, "time", static_cast<float>(100.0 + 800.0 * static_cast<double>(pos) / static_cast<double>(n)));
            });
        }
        p.stops = rig.engine().stops() - stops;
        if (p.stops > 0)
            rig.engine().lastStop(p.stop);
        return p;
    }

    // ---- COST -------------------------------------------------------------------------------------------------------
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

    struct Cost
    {
        double on = 0, off = 0, compileOn = 0, compileOff = 0;
        bool sameBits = false, loaded = false;
    };

    // An example with the checks and without: the same bits, the CPU time of each (the best of seven, taken in turn),
    // and the compile's (the best of three).
    Cost costOf(const pg::Sources& files, int voices, double seconds)
    {
        Cost c;
        probe::Rig on(kFs, kBlock), off(kFs, kBlock);
        if (!on.load(files) || !off.load(files, nullptr, true, false))
            return c;
        c.loaded = true;
        const std::vector<probe::Note> notes = chord(voices);
        const probe::Stereo in = costInput(static_cast<std::size_t>(seconds * kFs));
        const probe::Stereo a = on.run(in, kBlock, notes), b = off.run(in, kBlock, notes);
        c.sameBits = probe::sameBits(a.l, b.l) && probe::sameBits(a.r, b.r);
        c.on = c.off = 1e9;
        for (int k = 0; k < 7; ++k)
            for (probe::Rig* r : { &on, &off })
            {
                r->engine().restart();
                const double t0 = threadMs();
                r->run(in, kBlock, notes);
                (r == &on ? c.on : c.off) = std::min(r == &on ? c.on : c.off, threadMs() - t0);
            }
        c.compileOn = c.compileOff = 1e9;
        for (int k = 0; k < 3; ++k)
            for (const bool checks : { true, false })
            {
                const pg::Patch::Result r = pg::Patch::compile(files, static_cast<float>(kFs), false, nullptr, true, checks);
                (checks ? c.compileOn : c.compileOff) = std::min(checks ? c.compileOn : c.compileOff, r.compileMs);
            }
        return c;
    }
}

PROBE(dsp, bounds)
{
    const std::size_t second = static_cast<std::size_t>(kFs);
    const std::string where = std::string(pg::kProgramName) + ":";

    // ---- CAUGHT -------------------------------------------------------------------------------------------------------
    {
        const std::string source = program(kAtDial, kWrite);
        pg::Engine engine;
        engine.prepare(kFs, kBlock);
        std::string errors;
        engine.offer(compiled(pg::singleFile(source), 1, &errors));
        engine.prepare(kFs, kBlock);
        P.eq("caught.compiles", engine.playingPatch() != nullptr ? 1 : 0, 1);
        if (engine.playingPatch() == nullptr)
        {
            std::printf("%s", errors.c_str());
            return P.finish();
        }
        P.eq("caught.checked", engine.playingPatch()->boundsChecked() ? 1 : 0, 1);
        const pg::Control dial = engine.playingPatch()->controls()[0];
        const float in3 = pg::normOf(dial, 3.0f), out16 = pg::normOf(dial, 16.0f);
        const probe::Stereo in = sineIn(second);
        const std::size_t writeAt = 40 * kBlock;
        std::size_t stoppedAt = static_cast<std::size_t>(-1);
        const probe::Stereo out = play(engine, in, in3, out16, writeAt, kInstrument, &stoppedAt);
        pg::Engine::Stopped st;
        const bool read = engine.lastStop(st);
        P.eq("caught.stops", engine.stops() == 1 && read ? 1 : 0, 1);
        P.eq("caught.at_the_write", static_cast<int64_t>(stoppedAt), static_cast<int64_t>(writeAt));
        P.eq("caught.call", static_cast<int>(st.call), static_cast<int>(pg::Patch::Method::process));
        P.eq("caught.published", st.bounds.failed ? 1 : 0, 1);
        P.eq("caught.file", std::string(st.bounds.file) == pg::kProgramName ? 1 : 0, 1);
        const std::string place = placeOf(source, "line[at[0]]", 4);   // the `[`
        P.eq("caught.line_and_column", std::to_string(st.bounds.line) + ":" + std::to_string(st.bounds.column) == place ? 1 : 0, 1);
        P.eq("caught.array", std::string(st.bounds.array) == "line" ? 1 : 0, 1);
        P.eq("caught.type", std::string(st.bounds.type) == "float[16]" ? 1 : 0, 1);
        P.eq("caught.extent", st.bounds.extent, int64_t{ 16 });
        P.eq("caught.index", st.bounds.index, int64_t{ 16 });
        P.eq("caught.width", st.bounds.width, 1);
        const pg::Patch* patch = engine.playingPatch();
        P.eq("caught.patch_stopped", patch->stopped() == pg::Patch::Method::process ? 1 : 0, 1);
        P.eq("caught.patch_bounds", patch->bounds().failed && patch->bounds().index == 16 ? 1 : 0, 1);
        // The write never happened: the field after the array is as init left it, in every instance.
        P.near("caught.after_untouched", fieldMost(engine, "after"), 0.0, 1e-12);
        P.eq("caught.finite", probe::allFinite(out.l) && probe::allFinite(out.r) ? 1 : 0, 1);
        // No step beyond the signal's own: a sine of 220 Hz at 0.5 steps 0.0144 at most (the synth's 0.3 at 262 Hz less).
        P.le("caught.no_step", maxStepOf(out, writeAt - kBlock, 6 * kBlock), 0.03);
        const std::size_t after = writeAt + kBlock + 960 + 64;
        bool exact = true;
        for (std::size_t i = after; i < out.size() && exact; ++i)
            exact = kInstrument ? out.l[i] == 0.0f && out.r[i] == 0.0f : out.l[i] == in.l[i] && out.r[i] == in.r[i];
        P.eq(kInstrument ? "caught.then_silent" : "caught.then_its_input", exact ? 1 : 0, 1);
        // The words, at the access.
        const std::string message = pg::Patch::stopMessage(pg::singleFile(source), st.call, static_cast<float>(kFs), &st.bounds);
        P.eq("caught.message", message.rfind(where + place + ": error: process() went outside an array: index 16 out of "
                                             "bounds for 'line' of type 'float[16]'\n", 0) == 0 ? 1 : 0, 1);
        P.eq("caught.message_note", message.find(where + place + ": note: the plugin checks each index") != std::string::npos ? 1 : 0, 1);
        P.eq("caught.why", pg::Patch::stopWhy(st.call, &st.bounds) == "process() went outside an array (" + where + place
                               + ": index 16 out of bounds for 'line' of type 'float[16]')" ? 1 : 0, 1);
        std::printf("caught: %s", message.c_str());
        // As new again (restart: reset()), its dial back inside: it plays, and stops no more.
        engine.restart();
        const uint64_t before = engine.stops();
        const probe::Stereo again = play(engine, in, in3, in3, 0, kInstrument);
        P.eq("caught.reset_plays", engine.stops() == before && engine.playingPatch()->stopped() == pg::Patch::Method::none
                                       && !engine.playingPatch()->bounds().failed ? 1 : 0, 1);
        if constexpr (kInstrument)
            P.ge("caught.reset_sounds_db", probe::rmsDb(again.l, second / 2, second / 4), -20.0);
        else
        {
            bool half = true;
            for (std::size_t i = 4 * kBlock; i < again.size() && half; ++i)
                half = again.l[i] == 0.5f * in.l[i];
            P.eq("caught.reset_plays_its_half", half ? 1 : 0, 1);
        }
    }

    // ---- CAUGHT in an #include'd module, and in init --------------------------------------------------------------
    {
        const pg::Sources files{ { pg::kProgramName, "#include \"ring.fm\"\n" + program(kRingField, kRingWrite) },
                                 { "ring.fm", kRing } };
        pg::Engine engine;
        engine.prepare(kFs, kBlock);
        engine.offer(compiled(files, 1));
        engine.prepare(kFs, kBlock);
        P.eq("include.compiles", engine.playingPatch() != nullptr ? 1 : 0, 1);
        if (engine.playingPatch() != nullptr)
        {
            const pg::Control dial = engine.playingPatch()->controls()[0];
            play(engine, sineIn(second / 4), pg::normOf(dial, 3.0f), pg::normOf(dial, 16.0f), 8 * kBlock, kInstrument);
            pg::Engine::Stopped st;
            P.eq("include.stops", engine.stops() == 1 && engine.lastStop(st) && st.bounds.failed ? 1 : 0, 1);
            P.eq("include.file", std::string(st.bounds.file) == "ring.fm" ? 1 : 0, 1);
            P.eq("include.line_and_column", std::to_string(st.bounds.line) + ":" + std::to_string(st.bounds.column)
                                                == placeOf(kRing, "taps[k] = x", 4) ? 1 : 0, 1);
            P.eq("include.array", std::string(st.bounds.array) == "taps" ? 1 : 0, 1);
            P.eq("include.index", st.bounds.index, int64_t{ 16 });
            const std::string message = pg::Patch::stopMessage(files, st.call, static_cast<float>(kFs), &st.bounds);
            P.eq("include.message", message.rfind("ring.fm:" + placeOf(kRing, "taps[k] = x", 4) + ": error: process() went "
                                                  "outside an array: index 16 out of bounds for 'taps' of type 'float[8]'\n", 0) == 0
                                        ? 1 : 0, 1);
            std::printf("include: %s", message.c_str());
        }

        const std::string initSource = program(kAtDial, "", kInitWrite);
        const pg::Patch::Result r = pg::Patch::compile(pg::singleFile(initSource), static_cast<float>(kFs));
        P.eq("init.fails", r.patch == nullptr ? 1 : 0, 1);
        P.eq("init.stopped", static_cast<int>(r.stopped), static_cast<int>(pg::Patch::Method::init));
        P.eq("init.bounds", r.bounds.failed && r.bounds.index == 48 ? 1 : 0, 1);
        const std::string place = placeOf(initSource, "line[(int)", 4);
        P.eq("init.message", r.errors.rfind(where + place + ": error: init() went outside an array: index 48 out of bounds "
                                            "for 'line' of type 'float[16]'\n", 0) == 0 ? 1 : 0, 1);
        P.eq("init.message_does_not_play", r.errors.find("and the program does not play") != std::string::npos ? 1 : 0, 1);
        std::printf("init: %s", r.errors.c_str());
    }

    // ---- RUN: through a Compiler, its audio thread running ----------------------------------------------------------
    {
        pg::Engine engine;
        engine.prepare(kFs, kBlock);
        pg::Compiler compiler(engine);
        compiler.setFreezing(false);
        compiler.setSelfTesting(false);
        std::atomic<bool> done{ false };
        std::atomic<float> turn{ 0.0f };
        std::atomic<double> level{ 0.0 };                // the last block's RMS
        std::thread audio([&] {
            const probe::Stereo in = sineIn(kBlock);
            probe::Stereo out(kBlock);
            bool struck = false;
            while (!done.load())
            {
                out = in;
                std::array<float, pg::kMaxControls> norm{};
                norm[0] = turn.load();
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
        const auto settle = [] { std::this_thread::sleep_for(std::chrono::milliseconds(120)); };
        const pg::Sources fa = pg::singleFile(program("", ""));
        const pg::CompileStatus sa = compiler.compileNow(fa);
        P.eq("run.a_plays", sa.ok ? 1 : 0, 1);
        settle();
        // A RUN of a program that writes outside its array at once: it fails, A plays on.
        const std::string bText = program(kAtField, kWriteK, kAtFieldInit);
        compiler.request(pg::singleFile(bText));
        compiler.wait();
        const pg::CompileStatus sb = compiler.status();
        P.eq("run.at_once_fails", !sb.ok && sb.stopped && sb.bounds ? 1 : 0, 1);
        P.eq("run.at_once_a_plays_on", static_cast<int64_t>(sb.program), static_cast<int64_t>(sa.program));
        P.eq("run.at_once_message", sb.errors.rfind(where + placeOf(bText, "line[k]", 4) + ": error: process() went outside an "
                                                    "array: index 16 out of bounds for 'line' of type 'float[16]'\n", 0) == 0
                                        ? 1 : 0, 1);
        std::printf("run: %s", sb.errors.c_str());
        settle();
        pg::Meters m;
        P.eq("run.at_once_engine_plays_a", engine.meters(m) && m.program == sa.program ? 1 : 0, 1);
        P.ge("run.at_once_sounding_db", probe::dbFromLin(level.load()), -30.0);
        // A RUN of one that writes outside once its dial turns (at 20): it plays, then it stops, and A plays again.
        const pg::Sources fc = pg::singleFile(program(kAtDial, kWrite));
        compiler.request(fc);
        compiler.wait();
        const pg::CompileStatus sc = compiler.status();
        P.eq("run.later_plays", sc.ok ? 1 : 0, 1);
        settle();
        turn.store(1.0f);
        pg::CompileStatus later;
        for (int k = 0; k < 300; ++k)
        {
            later = compiler.status();
            if (later.stopped)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        turn.store(0.0f);
        P.eq("run.later_says_stopped", !later.ok && later.stopped && later.bounds ? 1 : 0, 1);
        P.eq("run.later_stopped_program", static_cast<int64_t>(later.stoppedProgram), static_cast<int64_t>(sc.program));
        P.eq("run.later_restored_a", later.restored == fa ? 1 : 0, 1);
        P.eq("run.later_message", later.errors.find("process() went outside an array: index 20 out of bounds for 'line' of type "
                                                    "'float[16]'") != std::string::npos ? 1 : 0, 1);
        settle();
        P.eq("run.later_engine_plays_a_again", engine.meters(m) && m.program == later.program ? 1 : 0, 1);
        P.ge("run.later_sounding_db", probe::dbFromLin(level.load()), -30.0);
        done.store(true);
        audio.join();
    }

    // ---- WORDS: measure_output, trace and view_sound --------------------------------------------------------------
    {
        const std::string source = program(kCountField, kWriteLater);
        const pg::Sources files = pg::singleFile(source);
        pg::measure::Request req;
        req.holdSeconds = 0.5f;
        req.tailSeconds = 0.2f;
        const std::string report = pg::measure::run(files, req);
        const std::string head = "Stopped: " + where + placeOf(source, "line[count", 4) + ": error: process() went outside an "
                                 "array: index 16 out of bounds for 'line' of type 'float[16]'\n";
        P.eq("words.measure_begins_with_it", report.rfind(head, 0) == 0 ? 1 : 0, 1);
        const std::size_t when = report.find("It stopped ");
        const double seconds = when == std::string::npos ? -1.0 : std::atof(report.c_str() + when + 11);
        P.in("words.measure_when_s", seconds, 0.31, 0.34);
        std::printf("measure: %s", report.substr(0, report.find('\n', when == std::string::npos ? 0 : when) + 1).c_str());

        pg::measure::TraceRequest tq;
        tq.render = req;
        tq.find = pg::measure::TraceFind::any;
        const pg::measure::TraceResult tr = pg::measure::trace(files, tq);
        P.eq("words.trace_call", static_cast<int>(tr.stopCall), static_cast<int>(pg::Patch::Method::process));
        P.eq("words.trace_bounds", tr.stopBounds.failed && tr.stopBounds.index == 16 ? 1 : 0, 1);
        bool line = false;
        for (const std::string& l : tr.lines)
            line = line || l.rfind(head.substr(0, head.size() - 1), 0) == 0;
        P.eq("words.trace_line", line ? 1 : 0, 1);

        pg::measure::ViewRequest vq;
        vq.render = req;
        const pg::measure::View v = pg::measure::view(files, vq);
        P.eq("words.view_sound", v.stopped.find(": process() went outside an array (" + where + placeOf(source, "line[count", 4)
                                                + ": index 16 out of bounds for 'line' of type 'float[16]'), and from there")
                                     != std::string::npos ? 1 : 0, 1);
        std::printf("view_sound: %s\n", v.stopped.c_str());
    }

    // ---- FROZEN: the frozen copy has no checks ---------------------------------------------------------------------
    {
        // A read through an index the input (an effect) or the note (an instrument) sets: 0 while it warms up, then 4,
        // one past line's end, where `after` lies (7, from init).
        const std::string extra = kInstrument ? "    float line[4];\n    float after;\n    float seen;\n    int k;\n\n"
                                              : "    float line[4];\n    float after;\n    float seen;\n\n";
        const std::string hooks = "    void init(float rate) { after = 7.0f; }\n\n";
        const std::string source = kInstrument
            ? voice(extra, "        seen = line[k];\n", hooks, "        k = (int)note - 60;\n")
            : effect(extra, "        seen = line[(int)(8.0f * left[0])];\n", hooks);
        const pg::Sources files = pg::singleFile(source);
        const auto past = [&](probe::Rig& rig) {
            // The warm-up (index 0), then a block that reads one past the end.
            if constexpr (kInstrument)
            {
                rig.run(probe::Stereo(4096), kBlock, { { 0, pg::EventKind::noteOn, 60, 0.8f } });
                return rig.run(probe::Stereo(2 * kBlock), kBlock, { { 0, pg::EventKind::noteOn, 64, 0.8f } });
            }
            else
            {
                rig.run(probe::Stereo(4096), kBlock);
                probe::Stereo in(2 * kBlock);
                std::fill(in.l.begin(), in.l.end(), 0.5f);
                std::fill(in.r.begin(), in.r.end(), 0.5f);
                return rig.run(in, kBlock);
            }
        };
        probe::Rig checked(kFs, kBlock), frozen(kFs, kBlock);
        P.eq("frozen.compiles", checked.load(files) && frozen.load(files) ? 1 : 0, 1);
        // The program: it stops at the read.
        const uint64_t stops = checked.engine().stops();
        past(checked);
        pg::Engine::Stopped st;
        P.eq("frozen.program_stops", checked.engine().stops() == stops + 1 && checked.engine().lastStop(st) && st.bounds.failed
                                         && st.bounds.index == 4 ? 1 : 0, 1);
        // Its frozen copy: it reads past the end, the field after the array.
        frozen.run(probe::Stereo(kBlock), kBlock);
        std::string why;
        P.eq("frozen.copy_made", frozen.freeze(&why) ? 1 : 0, 1);
        frozen.run(probe::Stereo(kBlock), kBlock);               // the copy taken at a block's start
        P.eq("frozen.copy_plays", frozen.engine().frozen() ? 1 : 0, 1);
        const uint64_t frozenStops = frozen.engine().stops();
        past(frozen);
        P.eq("frozen.copy_plays_on", frozen.engine().frozen() && frozen.engine().stops() == frozenStops ? 1 : 0, 1);
        P.near("frozen.copy_read_past_the_end", fieldNow(frozen.engine(), "seen"), 7.0, 1e-6);
        // Their IR: the program's has the record and the checks, the copy's neither.
        pg::Patch::Ir ir;
        std::string irWhy, frozenText;
        pg::DialState d;
        frozen.engine().dials(d);
        P.eq("frozen.ir", pg::Patch::ir(frozen.recipe(), ir, irWhy) ? 1 : 0, 1);
        P.eq("frozen.ir_program_has_them", ir.optimised.find("@fminus_rt_bounds") != std::string::npos
                                               && ir.optimised.find("!fminus.bounds.sites") != std::string::npos ? 1 : 0, 1);
        P.eq("frozen.ir_copy", pg::Patch::frozenIr(frozen.recipe(), d.plain, frozenText, irWhy) ? 1 : 0, 1);
        P.eq("frozen.ir_copy_has_none", frozenText.find("fminus_rt_bounds") == std::string::npos
                                            && frozenText.find("fminus.bounds") == std::string::npos && !frozenText.empty() ? 1 : 0, 1);
        const pg::Patch::Result unchecked = pg::Patch::compile(files, static_cast<float>(kFs), true, nullptr, true, false);
        P.eq("frozen.unchecked_compile_has_none", unchecked.patch && !unchecked.patch->boundsChecked()
                                                      && unchecked.ir.optimised.find("fminus_rt_bounds") == std::string::npos ? 1 : 0, 1);
    }

    // ---- MOVED: the example's lines in arrays, the same bits ------------------------------------------------------
    {
        const char* now = exampleSource(kMovedTitle);
        P.eq("moved.example", now != nullptr ? 1 : 0, 1);
        if (now != nullptr)
        {
            const std::string before = kInstrument ? kPluckBefore : kEchoBefore;
            P.eq("moved.before_uses_calloc", before.find("calloc(") != std::string::npos ? 1 : 0, 1);
            P.eq("moved.now_does_not", std::string(now).find("calloc(") == std::string::npos ? 1 : 0, 1);
            // Its lines among the checks: their descriptions in the IR (array, type).
            const pg::Patch::Result r = pg::Patch::compile(pg::withLibraryIncludes(pg::singleFile(now)), static_cast<float>(kFs), true);
            const std::string line = kInstrument ? "!\"line\", !\"float[9602]\"" : "!\"lineL\", !\"float[384004]\"";
            P.eq("moved.checked", r.patch && r.ir.optimised.find(line) != std::string::npos ? 1 : 0, 1);
            int same = 0, renders = 0;
            for (const double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
                for (const int scenario : { 0, 1 })
                {
                    const Played a = playMoved(before, fs, scenario), b = playMoved(now, fs, scenario);
                    ++renders;
                    const bool bits = a.loaded && b.loaded && a.stops == 0 && b.stops == 0 && probe::sameBits(a.out.l, b.out.l)
                                   && probe::sameBits(a.out.r, b.out.r) && probe::rmsDb(b.out.l, 0, b.out.l.size()) > -60.0;
                    same += bits ? 1 : 0;
                    std::printf("moved: %s at %.1f kHz, scenario %d: %s (%.1f dB RMS)%s%s\n", kMovedTitle, fs / 1000.0, scenario,
                                bits ? "the same bits" : "OTHER BITS", probe::rmsDb(b.out.l, 0, b.out.l.size()),
                                a.errors.empty() ? "" : "\n  before: ", a.errors.c_str());
                }
            P.eq("moved.same_bits", same, renders);
        }
        if constexpr (!kInstrument)
        {
            // ECHO: the Echo as it read its line before the fix, moved: caught at 192 kHz, where its line is the whole
            // array; the Echo as it is plays on there.
            const std::string reading = now != nullptr ? echoReadingPast(now) : std::string();
            P.eq("echo.reading_past_made", reading.empty() ? 0 : 1, 1);
            if (!reading.empty())
            {
                const Played bad = playMoved(reading, 192000.0, 0), good = playMoved(now, 192000.0, 0);
                P.eq("echo.reading_past_caught", bad.stops == 1 && bad.stop.bounds.failed ? 1 : 0, 1);
                const std::string array = bad.stop.bounds.array;
                P.eq("echo.reading_past_names_the_line", (array == "lineL" || array == "lineR") && bad.stop.bounds.index >= 384004
                                                             && std::string(bad.stop.bounds.file) == pg::kProgramName ? 1 : 0, 1);
                P.eq("echo.reading_past_where", bad.stop.bounds.line == std::stoi(placeOf(reading, "const float dl", 0)) ||
                                                    bad.stop.bounds.line == std::stoi(placeOf(reading, "const float dr", 0)) ? 1 : 0, 1);
                P.in("echo.reading_past_at_s", static_cast<double>(bad.stop.at) / 192000.0, 0.34, 0.36);
                P.eq("echo.now_plays_on", good.loaded && good.stops == 0 ? 1 : 0, 1);
                std::printf("echo: as it read before, at 192 kHz: %s", pg::Patch::stopMessage(pg::singleFile(reading), bad.stop.call,
                                                                                            192000.0f, &bad.stop.bounds).c_str());
            }
        }
    }

    // ---- COST ---------------------------------------------------------------------------------------------------------
    {
        double on = 0, off = 0, compileOn = 0, compileOff = 0;
        int same = 0, count = 0;
        const int voices = kInstrument ? 8 : 1;
        for (const pg::Example& e : pg::kExamples)
        {
            const Cost c = costOf(pg::withLibraryIncludes(pg::singleFile(e.source)), voices, 2.0);
            if (!c.loaded)
                continue;
            ++count;
            same += c.sameBits ? 1 : 0;
            on += c.on;
            off += c.off;
            compileOn += c.compileOn;
            compileOff += c.compileOff;
            std::printf("cost: %-16s %7.2f ms with the checks, %7.2f without (%+.1f%%), %s; compiled in %6.1f ms and %6.1f "
                        "(%+.1f%%)\n",
                        e.title, c.on, c.off, 100.0 * (c.on / std::max(c.off, 1e-9) - 1.0), c.sameBits ? "the same bits" : "OTHER BITS",
                        c.compileOn, c.compileOff, 100.0 * (c.compileOn / std::max(c.compileOff, 1e-9) - 1.0));
        }
        P.eq("cost.examples_same_bits", same, count);
        P.le("cost.examples_ratio", on / std::max(off, 1e-9), kCostRatio);
        std::printf("cost: the examples (%d, two seconds each at 48 kHz%s): %.1f ms with the checks, %.1f without (%+.2f%%); "
                    "compiled in %.1f ms and %.1f (%+.2f%%)\n",
                    count, kInstrument ? ", eight voices" : "", on, off, 100.0 * (on / std::max(off, 1e-9) - 1.0), compileOn,
                    compileOff, 100.0 * (compileOn / std::max(compileOff, 1e-9) - 1.0));
    }
    return P.finish();
}
