// PROBE name=dsp.host timeout=120
//
// dsp.host: the program's Host block (host.fm, Patch.h), as the Engine fills it, at 48 kHz: a program that #includes
// the plugin's host.fm (no file of that name among its own) and copies what its Host says into fields its displays
// show (Engine::Meters' taps), and, an instrument, bends its sine by the pitch wheel (2 semitones):
//   FILE      host.fm is the plugin's: the program compiles with no file of that name; a module Host that nothing
//             points at is a note
//   PLAYING   the host plays at 90 BPM in 3/4 from beat 4: a call reads the tempo, the signature, playing, and the
//             position of its own first sample (the last call of a second: an instrument's voices run in parts on the
//             16-sample grid, so 4 + 47984 samples at 1.5 beats a second; an effect's call is the host's block, so
//             the last block's start)
//   BLOCKS    in blocks of 48 and 80 samples: the output the same as in blocks of 256, and the position read that of
//             the last call (an instrument's the same as in blocks of 256)
//   STOPPED   a stopped host: the position stands where the host says
//   FREE      a host that says nothing: 120 BPM, not playing, the position running on from 0
//   WHEELS    (an instrument) the mod wheel, the pitch bend and aftertouch from their events; the pitch wheel up bends
//             the sine 2 semitones (440 Hz to 493.9)
//   PEDAL     (an instrument) a key let go while the sustain pedal is down keeps its voice; the pedal up stops it
//   EFFECT    (an effect) the wheels stay at rest
//   EXAMPLES  the examples that read it: an instrument's Sine bent 2 semitones by the pitch wheel; an effect's Tremolo
//             at SYNC 1/4 taking its phase from the song's position (from beat 0.5: at full level, a quarter
//             cycle on at its lowest), its Echo repeating after TIME (350 ms), and at SYNC 1/4 after a beat at 90 BPM
#include "Probe.h"

#include "Rig.h"

#include "fminus/Patch.h"

#include "Examples.h"
#include "Product.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace tp = testingplayground;

namespace
{
    constexpr bool kInstrument = tp::product::kIsInstrument;
    constexpr double kFs = 48000.0;

    // The fields, in display (tap) order.
    enum Tap : int { tBpm, tBeat, tWheel, tBend, tTouch, tSustain, tPlaying, tPerBar, tBar };

    const char* kLines = "#pragma panel number sbpm\n#pragma panel number sbeat\n#pragma panel number swheel\n"
                         "#pragma panel number sbend\n#pragma panel number stouch\n#pragma panel led ssus\n"
                         "#pragma panel led splay\n#pragma panel number sper\n#pragma panel number sbar\n";

    const char* kCopy = "        sbpm = host->bpm;\n        sbeat = host->beat;\n        swheel = host->modwheel;\n"
                        "        sbend = host->pitchbend;\n        stouch = host->aftertouch;\n        ssus = host->sustain;\n"
                        "        splay = host->playing;\n        sper = host->beatsPerBar;\n        sbar = host->barStart;\n";

    const char* kFields = "    Host* host;\n    float sbpm, sbeat, swheel, sbend, stouch, sbar;\n    bool ssus, splay;\n    int sper;\n";

    std::string program()
    {
        if constexpr (kInstrument)
            return std::string("#include \"host.fm\"\n") + kLines + "module Voice\n{\n" + kFields +
                   "    float ph, step;\n    bool held;\n"
                   "    void start(float note, float velocity) { step = 440.0f * expf(0.0577622650f * (note - 69)) / 48000; held = true; }\n"
                   "    void stop() { held = false; }\n"
                   "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        const float f = step * expf(0.0577622650f * 2 * host->pitchbend);\n"
                   "        for (int i = 0; i < n; ++i)\n        {\n"
                   "            const float s = 0.25f * sinf(6.28318531f * ph);\n            ph += f;\n"
                   "            if (ph >= 1) ph -= 1;\n            left[i] += s;\n            right[i] += s;\n        }\n"
                   + kCopy + "        return held;\n    }\n};\n";
        else
            return std::string("#include \"host.fm\"\n") + kLines + "module Effect\n{\n" + kFields +
                   "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        for (int i = 0; i < n; ++i)\n        {\n            left[i] *= 0.5f;\n            right[i] *= 0.5f;\n        }\n"
                   + kCopy + "    }\n};\n";
    }

    float tap(probe::Rig& rig, int t)
    {
        tp::Meters m;
        rig.engine().meters(m);
        return m.taps[t];
    }

    // The sine's frequency over `x`'s second half, by its rising zero crossings.
    [[maybe_unused]] double frequency(const std::vector<float>& x)
    {
        int crossings = 0;
        const std::size_t from = x.size() / 2;
        for (std::size_t i = from + 1; i < x.size(); ++i)
            crossings += x[i - 1] <= 0.0f && x[i] > 0.0f ? 1 : 0;
        return crossings * kFs / static_cast<double>(x.size() - from);
    }

    // The first sample of the last call in `total` samples played in blocks of `sizes` (in turn): an instrument's
    // voices run in parts on the 16-sample grid; an effect's call is the block.
    double lastCall(std::size_t total, const std::vector<int>& sizes)
    {
        if constexpr (kInstrument)
            return static_cast<double>(total - 16);
        std::size_t at = 0, start = 0, k = 0;
        while (at < total)
        {
            start = at;
            at += static_cast<std::size_t>(sizes[k++ % sizes.size()]);
        }
        return static_cast<double>(start);
    }

    [[maybe_unused]] std::string example(const char* title)
    {
        for (const tp::Example& e : tp::kExamples)
            if (std::string(e.title) == title)
                return e.source;
        return {};
    }

    // Sets the dial named `name` to `plain`.
    [[maybe_unused]] void dial(probe::Rig& rig, const char* name, float plain)
    {
        for (std::size_t c = 0; c < rig.norm().size(); ++c)
            if (std::string(rig.controls()[c].name) == name)
                rig.norm()[c] = tp::normOf(rig.controls()[c], plain);
    }

    // Where `x` peaks from sample `from` on.
    [[maybe_unused]] std::size_t peakAt(const std::vector<float>& x, std::size_t from)
    {
        std::size_t at = from;
        for (std::size_t i = from; i < x.size(); ++i)
            if (std::fabs(x[i]) > std::fabs(x[at]))
                at = i;
        return at;
    }

    tp::Transport playing(double bpm, double beat, int perBar)
    {
        tp::Transport t;
        t.known = true;
        t.bpm = bpm;
        t.beat = beat;
        t.barStart = 3.0;
        t.beatsPerBar = perBar;
        t.beatUnit = 4;
        t.playing = true;
        return t;
    }
}

PROBE(dsp, host)
{
    // ---- FILE ----------------------------------------------------------------------------------------------------------
    {
        const tp::Patch::Result lone = tp::Patch::compile(std::string("#include \"host.fm\"\n") +
                                                              (kInstrument ? "module Voice\n{\n    void start(float n, float v) {}\n"
                                                                             "    bool process(float* restrict l, float* restrict r, int n) { return false; }\n};\n"
                                                                           : "module Effect\n{\n    void process(float* restrict l, float* restrict r, int n) {}\n};\n"),
                                                          48000.0f);
        P.eq("file.compiles", lone.patch != nullptr ? 1 : 0, 1);
        P.eq("file.note_nothing_points", lone.notes.find("no field points at it (`Host* host;`") != std::string::npos ? 1 : 0, 1);
        P.eq("file.has_text", tp::Patch::hostSource().find("module Host") != std::string_view::npos ? 1 : 0, 1);
    }

    probe::Rig rig(kFs, 256);
    std::string messages;
    const bool ok = rig.load(program(), &messages);
    if (!messages.empty())
        std::printf("%s", messages.c_str());
    P.eq("load.compiles", ok ? 1 : 0, 1);
    P.eq("load.nine_displays", static_cast<int64_t>(rig.panel().displays.size()), 9);
    if (!ok)
        return P.finish();
    const std::size_t second = static_cast<std::size_t>(kFs);
    probe::Stereo in(second);
    if constexpr (!kInstrument)
        in.l = in.r = probe::sine(1000.0, 0.5, kFs, second);
    std::vector<probe::Note> note;
    if constexpr (kInstrument)
        note.push_back({ 0, tp::EventKind::noteOn, 69, 0.8f });

    // ---- PLAYING -------------------------------------------------------------------------------------------------------
    rig.engine().setTransport(playing(90.0, 4.0, 3));
    const probe::Stereo out256 = rig.run(in, 256, note);
    const double last = 4.0 + lastCall(second, { 256 }) * 90.0 / (60.0 * kFs);
    P.near("playing.bpm", static_cast<double>(tap(rig, tBpm)), 90.0, 1e-4);
    P.near("playing.beat_of_its_call", static_cast<double>(tap(rig, tBeat)), last, 1e-3);
    P.eq("playing.playing", tap(rig, tPlaying) == 1.0f ? 1 : 0, 1);
    P.eq("playing.signature", tap(rig, tPerBar) == 3.0f ? 1 : 0, 1);
    P.near("playing.bar_start", static_cast<double>(tap(rig, tBar)), 3.0, 1e-6);
    P.near("playing.engine_beat", static_cast<double>(rig.engine().host().beat), last, 1e-3);

    // ---- BLOCKS --------------------------------------------------------------------------------------------------------
    {
        probe::Rig other(kFs, 256);
        other.load(program());
        other.engine().setTransport(playing(90.0, 4.0, 3));
        const probe::Stereo outSplit = other.run(in, 256, note, { 48, 80 });
        P.near("blocks.beat_of_its_call", static_cast<double>(tap(other, tBeat)),
               4.0 + lastCall(second, { 48, 80 }) * 90.0 / (60.0 * kFs), 1e-3);
        double worst = 0.0;
        for (std::size_t i = 0; i < second; ++i)
            worst = std::max(worst, static_cast<double>(std::fabs(outSplit.l[i] - out256.l[i])));
        P.le("blocks.output_same", worst, 1e-6);
    }

    // ---- STOPPED -------------------------------------------------------------------------------------------------------
    {
        probe::Rig other(kFs, 256);
        other.load(program());
        tp::Transport t = playing(100.0, 2.0, 4);
        t.playing = false;
        other.engine().setTransport(t);
        other.run(in, 256, note);
        P.near("stopped.beat_stands", static_cast<double>(tap(other, tBeat)), 2.0, 1e-6);
        P.eq("stopped.not_playing", tap(other, tPlaying) == 0.0f ? 1 : 0, 1);
        P.near("stopped.bpm", static_cast<double>(tap(other, tBpm)), 100.0, 1e-4);
    }

    // ---- FREE ----------------------------------------------------------------------------------------------------------
    {
        probe::Rig other(kFs, 256);
        other.load(program());
        other.engine().setTransport(tp::Transport{});
        other.run(in, 256, note);
        P.near("free.bpm", static_cast<double>(tap(other, tBpm)), 120.0, 1e-4);
        P.near("free.beat_runs", static_cast<double>(tap(other, tBeat)), lastCall(second, { 256 }) * 2.0 / kFs, 1e-3);
        P.eq("free.not_playing", tap(other, tPlaying) == 0.0f ? 1 : 0, 1);
    }

    if constexpr (kInstrument)
    {
        // ---- WHEELS ----------------------------------------------------------------------------------------------------
        probe::Rig other(kFs, 256);
        other.load(program());
        std::vector<probe::Note> events = note;
        events.push_back({ 1000, tp::EventKind::controller, 1, 0.5f });
        events.push_back({ 2000, tp::EventKind::pitchBend, 0, 1.0f });
        events.push_back({ 3000, tp::EventKind::pressure, 0, 0.3f });
        const probe::Stereo bent = other.run(in, 256, events);
        P.near("wheels.modwheel", static_cast<double>(tap(other, tWheel)), 0.5, 1e-6);
        P.near("wheels.pitchbend", static_cast<double>(tap(other, tBend)), 1.0, 1e-6);
        P.near("wheels.aftertouch", static_cast<double>(tap(other, tTouch)), 0.3, 1e-6);
        P.near("wheels.unbent_pitch", frequency(out256.l), 440.0, 3.0);
        P.near("wheels.bent_pitch", frequency(bent.l), 440.0 * std::pow(2.0, 2.0 / 12.0), 3.0);

        // ---- PEDAL -----------------------------------------------------------------------------------------------------
        probe::Rig pedal(kFs, 256);
        pedal.load(program());
        const std::size_t half = second / 2;
        probe::Stereo quiet(half);
        pedal.run(quiet, 256, { { 0, tp::EventKind::noteOn, 60, 0.8f },
                                { 4800, tp::EventKind::controller, 64, 1.0f },
                                { 9600, tp::EventKind::noteOff, 60, 0.0f } });
        P.eq("pedal.holds", pedal.engine().activeVoices(), 1);
        P.eq("pedal.down_seen", tap(pedal, tSustain) == 1.0f ? 1 : 0, 1);
        pedal.run(quiet, 256, { { 100, tp::EventKind::controller, 64, 0.0f } });
        P.eq("pedal.up_stops", pedal.engine().activeVoices(), 0);
        // And without the pedal the key's release stops it at once.
        probe::Rig plain(kFs, 256);
        plain.load(program());
        plain.run(quiet, 256, { { 0, tp::EventKind::noteOn, 60, 0.8f }, { 9600, tp::EventKind::noteOff, 60, 0.0f } });
        P.eq("pedal.none_stops", plain.engine().activeVoices(), 0);
    }
    else
    {
        // ---- EFFECT ----------------------------------------------------------------------------------------------------
        P.eq("effect.wheels_at_rest", tap(rig, tWheel) == 0.0f && tap(rig, tBend) == 0.0f && tap(rig, tSustain) == 0.0f ? 1 : 0, 1);
    }

    // ---- EXAMPLES ------------------------------------------------------------------------------------------------------
    if constexpr (kInstrument)
    {
        probe::Rig sine(kFs, 256);
        P.eq("examples.sine_loads", sine.load(example("Sine")) ? 1 : 0, 1);
        const probe::Stereo up = sine.run(in, 256, { { 0, tp::EventKind::noteOn, 69, 0.8f }, { 0, tp::EventKind::pitchBend, 0, 1.0f } });
        P.near("examples.sine_bends", frequency(up.l), 440.0 * std::pow(2.0, 2.0 / 12.0), 3.0);
    }
    else
    {
        probe::Stereo dc(second);
        std::fill(dc.l.begin(), dc.l.end(), 1.0f);
        dc.r = dc.l;
        probe::Rig tremolo(kFs, 256);
        P.eq("examples.tremolo_loads", tremolo.load(example("Tremolo")) ? 1 : 0, 1);
        dial(tremolo, "sync", 3.0f);                                 // 1/4: a cycle a quarter note
        tremolo.engine().setTransport(playing(120.0, 0.5, 4));
        const probe::Stereo t = tremolo.run(dc, 256);
        P.near("examples.tremolo_from_its_place", static_cast<double>(t.l[0]), 1.0, 1e-3);
        P.near("examples.tremolo_quarter_cycle_on", static_cast<double>(t.l[6000]), 0.75, 1e-2);
        P.near("examples.tremolo_lowest_on_the_beat", static_cast<double>(t.l[12000]), 0.5, 1e-3);

        probe::Stereo click(second);
        click.l[0] = click.r[0] = 1.0f;
        probe::Rig echo(kFs, 256);
        P.eq("examples.echo_loads", echo.load(example("Echo")) ? 1 : 0, 1);
        echo.engine().setTransport(playing(90.0, 0.0, 4));
        P.near("examples.echo_after_time", static_cast<double>(peakAt(echo.run(click, 256).l, 100)), 16800.0, 1.0);
        probe::Rig synced(kFs, 256);
        synced.load(example("Echo"));
        dial(synced, "sync", 4.0f);                                  // 1/4
        synced.engine().setTransport(playing(90.0, 0.0, 4));
        P.near("examples.echo_synced_a_beat", static_cast<double>(peakAt(synced.run(click, 256).l, 100)), 32000.0, 1.0);
    }
    return P.finish();
}
