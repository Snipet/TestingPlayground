// PROBE name=dsp.swap timeout=120
//
// dsp.swap: the Engine (Engine.h) at 48 kHz in blocks of 256, with programs written for each row.
// An effect:
//   NONE    with no program the input passes through, bit for bit
//   FIRST   the first program fades in from the input over 20 ms: the input before the offer, the program's output
//           (the input at half) from 20 ms after it, bit for bit; no step between neighbouring samples larger than
//           the input's own steepest (a 440 Hz sine at -6 dBFS)
//   SWAP    a second program (the input at a quarter) takes over the same way, the two crossfaded; afterwards the old
//           one is handed back (collect() destroys it) and no swap is under way
// An instrument:
//   SWAP    a held note's voice fades out over 20 ms when another program arrives, which starts the key again: from
//           20 ms after the offer the output is the new program's alone
//   STEAL   seventeen held notes: sixteen sound, the seventeenth steals the oldest, which fades out over 2 ms (the
//           whole fade, though its process returns false from the stop() its steal gave it)
//   STOPS   a program whose voices keep a key stack in Shared (start pushes the key, stop pops it) and count, per
//           instance, a start before the last start's stop and a stop with no start since the last stop: twenty keys
//           held (four steals, each voice's stop() at its steal; the stolen first key let go inside its 2 ms fade);
//           the quietest cut with all 32 instances taken (a voice that had no stop() yet, its instance the new
//           note's); a voice dropped for going non-finite; then every key up. Every start has exactly one stop before
//           its instance's next start, none twice, and the stack ends empty
//   RELEASE a voice whose process returns false (here: once its key is up) is free again
// Both:
//   GLIDE   a real dial moved from 0 to 1 reaches the program as a stream that glides there in 20 ms, linearly,
//           sample by sample: half way at 10 ms, exactly 1 from 20 ms, never stepping back
//   KNOB    an integer dial reaches the program once a block, unsmoothed: the block after the move reads the new value
//   GUARD   a program that writes a NaN, an infinity or 10 leaves the Engine as 0, 0 and 4, and raises the alerts
#include "Probe.h"

#include "Rig.h"

#include "Product.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace pg = playground;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr bool kInstrument = pg::product::kIsInstrument;

    // A program whose output (an effect's, or a held voice's) is `expr`, with `params` in its main module.
    std::string program(const std::string& params, const std::string& expr)
    {
        if constexpr (kInstrument)
            return "module Voice\n{\n" + params +
                   "    bool held;\n    void start(float n, float v) { held = true; }\n    void stop() { held = false; }\n"
                   "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        for (int i = 0; i < n; ++i)\n        {\n            const float x = 1;\n"
                   "            left[i] = " + expr + ";\n            right[i] = " + expr + ";\n        }\n"
                   "        return held;\n    }\n};\n";
        else
            return "module Effect\n{\n" + params +
                   "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        for (int i = 0; i < n; ++i)\n        {\n            const float x = left[i];\n"
                   "            left[i] = " + expr + ";\n            right[i] = " + expr + ";\n        }\n    }\n};\n";
    }

    std::unique_ptr<pg::Patch> compile(const std::string& source, float rate)
    {
        pg::Patch::Result r = pg::Patch::compile(source, rate);
        if (!r.errors.empty())
            std::printf("%s", r.errors.c_str());
        return std::move(r.patch);
    }

    // A program whose voices keep the keys held in a stack in Shared and count what start and stop see (STOPS).
    const char* const kKeyStack = R"(module Shared
{
    int stack[128];                            // the keys whose voices have had a start and no stop since
    int open[32];                              // per instance: 1 between its start and its stop
    int depth, starts, stops, unstopped, twice, next;
};

module Voice
{
    Shared* shared;
    int me, key;
    bool held;
    float env;

    void init(float rate)
    {
        me = shared->next;
        shared->next += 1;
    }

    void start(float note, float velocity)
    {
        shared->starts += 1;
        if (shared->open[me] != 0)
            shared->unstopped += 1;            // a start before the last start's stop
        shared->open[me] = 1;
        key = (int)note;
        if (shared->depth < 128)
        {
            shared->stack[shared->depth] = key;
            shared->depth += 1;
        }
        held = true;
        env = 1;
    }

    void stop()
    {
        shared->stops += 1;
        if (shared->open[me] == 0)
            shared->twice += 1;                // a stop with no start since the last stop
        shared->open[me] = 0;
        int at = -1;
        for (int k = 0; k < shared->depth; ++k)
            if (shared->stack[k] == key)
                at = k;
        if (at >= 0)
        {
            for (int k = at; k + 1 < shared->depth; ++k)
                shared->stack[k] = shared->stack[k + 1];
            shared->depth -= 1;
        }
        held = false;
    }

    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            if (!held)
                env *= 0.99f;
            float out = 0.01f * env;
            if (key == 99)
                out = env / (env - env);       // key 99 goes non-finite: the plugin drops its voice
            left[i] += out;
            right[i] += out;
        }
        return held || env > 0.001f;
    }
};
)";

    // A field of the program's Shared, read through its field map (Patch::fieldMap: "shared->depth"); -1 when none.
    double sharedField(const pg::Patch* p, const std::string& path)
    {
        if (p == nullptr)
            return -1.0;
        for (const pg::Patch::Field& f : p->fieldMap())
            if (f.path == path)
                return p->read(f, 0);
        return -1.0;
    }

    probe::Stereo sineInput(std::size_t n)
    {
        probe::Stereo s(n);
        s.l = probe::sine(440.0, 0.5, kFs, n);
        s.r = s.l;
        return s;
    }
}

PROBE(dsp, swap)
{
    const std::size_t fade = static_cast<std::size_t>(0.020 * kFs);
    const probe::Note held{ 0, pg::EventKind::noteOn, 60, 1.0f };

    if constexpr (!kInstrument)
    {
        const std::size_t n = static_cast<std::size_t>(kFs);
        const probe::Stereo in = sineInput(n);
        const double steep = probe::maxStep(in.l, 0, n);

        // NONE
        probe::Rig none(kFs, 512);
        const probe::Stereo dry = none.run(in, 256);
        P.eq("none.passes", probe::sameBits(dry.l, in.l) && probe::sameBits(dry.r, in.r) ? 1 : 0, 1);

        // FIRST and SWAP: offers at blocks 40 and 120 (samples 10240 and 30720).
        probe::Rig rig(kFs, 512);
        pg::Engine& e = rig.engine();
        auto half = compile(program("", "0.5f * x"), e.rate());
        auto quarter = compile(program("", "0.25f * x"), e.rate());
        P.eq("programs.compile", half && quarter ? 1 : 0, 1);
        const std::size_t first = 40 * 256, second = 120 * 256;
        const probe::Stereo out = rig.run(in, 256, {}, {}, [&](std::size_t pos, std::array<float, pg::kMaxControls>&) {
            if (pos == first)
                e.offer(std::move(half));
            if (pos == second)
            {
                P.eq("first.done_before_swap", e.swapping() ? 0 : 1, 1);
                e.offer(std::move(quarter));
            }
        });
        bool before = true, afterFirst = true, afterSecond = true;
        for (std::size_t i = 0; i < first; ++i)
            before = before && out.l[i] == in.l[i];
        for (std::size_t i = first + fade + 1; i < second; ++i)
            afterFirst = afterFirst && out.l[i] == 0.5f * in.l[i];
        for (std::size_t i = second + fade + 1; i < n; ++i)
            afterSecond = afterSecond && out.l[i] == 0.25f * in.l[i];
        P.eq("first.input_before", before ? 1 : 0, 1);
        P.eq("first.program_after_20ms", afterFirst ? 1 : 0, 1);
        P.eq("swap.program_after_20ms", afterSecond ? 1 : 0, 1);
        P.le("first.max_step", probe::maxStep(out.l, first - 8, fade + 16), steep * 1.0001);
        P.le("swap.max_step", probe::maxStep(out.l, second - 8, fade + 16), steep * 1.0001);
        e.collect();
        P.eq("swap.over", e.swapping() ? 0 : 1, 1);
    }
    else
    {
        // SWAP: a held note on a program of 1s, then one of 0.5s.
        probe::Rig rig(kFs, 512);
        rig.load(program("", "1.0f"));
        pg::Engine& e = rig.engine();
        auto next = compile(program("", "0.5f"), e.rate());
        const std::size_t offer = 20 * 256;
        const probe::Stereo out = rig.run(probe::Stereo(static_cast<std::size_t>(0.25 * kFs)), 256, { held }, {},
                                          [&](std::size_t pos, std::array<float, pg::kMaxControls>&) {
                                              if (pos == offer)
                                                  e.offer(std::move(next));
                                          });
        P.near("swap.before", static_cast<double>(out.l[offer - 1]), 1.0, 0.0);
        bool after = true;
        for (std::size_t i = offer + fade + 1; i < out.size(); ++i)
            after = after && out.l[i] == 0.5f;
        P.eq("swap.new_alone_after_20ms", after ? 1 : 0, 1);
        P.eq("swap.one_voice", e.activeVoices(), 1);

        // STEAL: seventeen held notes of 1/16 each.
        probe::Rig steal(kFs, 512);
        steal.load(program("", "0.0625f"));
        std::vector<probe::Note> notes;
        for (int k = 0; k < 17; ++k)
            notes.push_back({ static_cast<std::size_t>(k) * 256, pg::EventKind::noteOn, 40 + k, 1.0f });
        const probe::Stereo s = steal.run(probe::Stereo(32 * 256), 256, notes);
        P.near("steal.sixteen_sound_db", static_cast<double>(s.l.back()), 1.0, 1e-6);
        // Half way through the 2 ms (96 samples) the stolen voice is at half, though its process returns false from
        // the stop() its steal gave it: the fade runs whole.
        P.near("steal.fades_whole", static_cast<double>(s.l[16 * 256 + 47]), 1.0 + 0.5 * 0.0625, 1e-4);
        P.eq("steal.voices", steal.engine().activeVoices(), 16);

        // STOPS: twenty keys, the quietest cut and a dropped voice, on a program that keeps a key stack in Shared.
        const auto stops = [&](const std::string& key, const std::vector<probe::Note>& played, std::size_t length,
                               int starts) {
            probe::Rig r(kFs, 512);
            std::string errors;
            P.eq(key + ".compiles", r.load(kKeyStack, &errors) ? 1 : 0, 1);
            if (!errors.empty())
                std::printf("%s", errors.c_str());
            r.run(probe::Stereo(length), 256, played);
            const pg::Patch* p = r.engine().playingPatch();
            P.eq(key + ".starts", static_cast<int64_t>(sharedField(p, "shared->starts")), starts);
            P.eq(key + ".a_stop_each", static_cast<int64_t>(sharedField(p, "shared->stops")), starts);
            P.eq(key + ".no_start_unstopped", static_cast<int64_t>(sharedField(p, "shared->unstopped")), 0);
            P.eq(key + ".no_stop_twice", static_cast<int64_t>(sharedField(p, "shared->twice")), 0);
            P.eq(key + ".stack_empty", static_cast<int64_t>(sharedField(p, "shared->depth")), 0);
            P.eq(key + ".voices_free", r.engine().activeVoices(), 0);
            pg::Meters m;
            r.engine().meters(m);
            return m;
        };
        {
            // Twenty keys a block apart: the 17th to 20th steal the oldest (keys 40 to 43, each its stop() then); key
            // 40, stolen at block 16, is let go 32 samples into its 2 ms fade; the rest come up at block 24.
            std::vector<probe::Note> keys;
            for (int k = 0; k < 20; ++k)
            {
                keys.push_back({ static_cast<std::size_t>(k) * 256, pg::EventKind::noteOn, 40 + k, 1.0f });
                if (k == 16)
                    keys.push_back({ 16 * 256 + 32, pg::EventKind::noteOff, 40, 0.0f });
            }
            for (int k = 1; k < 20; ++k)
                keys.push_back({ 24 * 256 + static_cast<std::size_t>(k), pg::EventKind::noteOff, 40 + k, 0.0f });
            const pg::Meters m = stops("stops.twenty", keys, 40 * 256, 20);
            P.eq("stops.twenty.steals", static_cast<int64_t>(m.steals), 4);
        }
        {
            // The quietest cut: at 0, key 30 and sixteen more (the last steals key 30, whose voice is free from its fade
            // on: slot and instance 0). At 1024, all at one sample: key 60 (instance 0) steals the oldest, key 61 (let
            // go at once) the next, keys 62 to 75 the rest, so sixteen fade, none rendered yet, and sixteen sound. Key
            // 76 steals key 61 (the oldest released), finds no instance free, and cuts the quietest: of equal gains the
            // first voice, key 60's, which has had no stop() yet; key 76 starts on its instance.
            std::vector<probe::Note> keys;
            for (int k = 0; k < 17; ++k)
                keys.push_back({ 0, pg::EventKind::noteOn, 30 + k, 1.0f });
            for (int k = 0; k < 17; ++k)
            {
                keys.push_back({ 1024, pg::EventKind::noteOn, 60 + k, 1.0f });
                if (k == 1)
                    keys.push_back({ 1024, pg::EventKind::noteOff, 61, 0.0f });
            }
            probe::Rig r(kFs, 512);
            r.load(kKeyStack);
            r.run(probe::Stereo(1280), 256, keys);
            std::vector<pg::Engine::VoiceInfo> voices;
            r.engine().voices(voices);
            int cutTo = -1;
            for (const pg::Engine::VoiceInfo& v : voices)
                if (v.key == 76)
                    cutTo = v.instance;
            P.eq("stops.cut.on_the_cut_instance", cutTo, 0);
            for (int k = 0; k < 17; ++k)
                keys.push_back({ 2048 + static_cast<std::size_t>(k), pg::EventKind::noteOff, 30 + k, 0.0f });
            for (int k = 0; k < 17; ++k)
                keys.push_back({ 2560 + static_cast<std::size_t>(k), pg::EventKind::noteOff, 60 + k, 0.0f });
            const pg::Meters m = stops("stops.cut", keys, 16 * 256, 34);
            P.eq("stops.cut.steals", static_cast<int64_t>(m.steals), 19);   // 1 + 16 + 1 stolen, 1 cut
        }
        {
            // Key 99's voice goes non-finite at once: dropped, its stop() then; its key-up later finds no voice.
            const pg::Meters m = stops("stops.dropped",
                                       { { 0, pg::EventKind::noteOn, 99, 1.0f }, { 512, pg::EventKind::noteOff, 99, 0.0f } },
                                       4 * 256, 1);
            P.eq("stops.dropped.dropped", static_cast<int64_t>(m.dropped), 1);
        }

        // RELEASE
        probe::Rig rel(kFs, 512);
        rel.load(program("", "1.0f"));
        rel.run(probe::Stereo(4 * 256), 256,
                { held, { 2 * 256, pg::EventKind::noteOff, 60, 0.0f } });
        P.eq("release.voice_free", rel.engine().activeVoices(), 0);
    }

    // GLIDE: a real dial from 0 to 1 at block 8.
    {
        probe::Rig rig(kFs, 512);
        rig.load(program("    param float g [[fminus::range(0, 1)]] [[fminus::default(0)]];\n", "g[i]"));
        const std::size_t move = 8 * 256;
        const probe::Stereo out = rig.run(sineInput(32 * 256), 256, kInstrument ? std::vector<probe::Note>{ held }
                                                                                 : std::vector<probe::Note>{},
                                          {}, [&](std::size_t pos, std::array<float, pg::kMaxControls>& norm) {
                                              norm[0] = pos >= move ? 1.0f : 0.0f;
                                          });
        P.near("glide.before", static_cast<double>(out.l[move - 1]), 0.0, 0.0);
        P.near("glide.half_at_10ms", static_cast<double>(out.l[move + fade / 2 - 1]), 0.5, 1e-3);
        P.near("glide.one_at_20ms", static_cast<double>(out.l[move + fade - 1]), 1.0, 0.0);
        bool monotonic = true;
        for (std::size_t i = move; i < move + fade + 8; ++i)
            monotonic = monotonic && out.l[i] >= out.l[i - 1];
        P.eq("glide.monotonic", monotonic ? 1 : 0, 1);
        P.le("glide.max_step", probe::maxStep(out.l, move - 1, fade + 2), 1.0 / static_cast<double>(fade) * 1.01);
    }

    // KNOB: an integer dial over 0..4 from 0 to 4 at block 8.
    {
        probe::Rig rig(kFs, 512);
        rig.load(program("    param int k [[fminus::range(0, 4)]] [[fminus::default(0)]];\n", "(float)k[i]"));
        const std::size_t move = 8 * 256;
        const probe::Stereo out = rig.run(sineInput(16 * 256), 256, kInstrument ? std::vector<probe::Note>{ held }
                                                                                 : std::vector<probe::Note>{},
                                          {}, [&](std::size_t pos, std::array<float, pg::kMaxControls>& norm) {
                                              norm[0] = pos >= move ? 1.0f : 0.0f;
                                          });
        P.near("knob.before", static_cast<double>(out.l[move - 1]), 0.0, 0.0);
        P.near("knob.at_the_block", static_cast<double>(out.l[move]), 4.0, 0.0);
    }

    // GUARD: a NaN, an infinity, 10.
    {
        int index = 0;
        for (const char* expr : { "0.0f / 0.0f", "1.0f / 0.0f", "10.0f" })
        {
            const std::string key = "guard" + std::to_string(++index);
            probe::Rig rig(kFs, 512);
            const bool loaded = rig.load(program("", expr));
            P.eq(key + ".compiles", loaded ? 1 : 0, 1);
            const probe::Stereo out = rig.run(sineInput(4 * 256), 256, kInstrument ? std::vector<probe::Note>{ held }
                                                                                     : std::vector<probe::Note>{});
            const uint32_t alerts = rig.engine().takeAlerts();
            const double want = index == 3 ? 4.0 : 0.0;
            P.near(key + ".output", static_cast<double>(out.l[300]), want, 0.0);
            P.eq(key + ".finite", probe::allFinite(out.l) && probe::allFinite(out.r) ? 1 : 0, 1);
            P.eq(key + ".alert", (alerts & (index == 3 ? pg::Engine::clipped : pg::Engine::nonFinite)) != 0 ? 1 : 0, 1);
        }
    }
    return P.finish();
}
