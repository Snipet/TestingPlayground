// PROBE name=dsp.shared timeout=180
//
// dsp.shared: an instrument's module Shared on its own (Patch.h and Engine.h, "Shared on its own"): the Engine at 48 kHz
// in blocks of 256 unless a row says otherwise, with programs written for each row. An instrument:
//   TICK    Shared's tick(int n) runs every 16 samples (n = 16) with no note held: after a second, 3,000 ticks of 16 (its
//           sample count 48,000), and a display of its field (#pragma panel number shared->ticks) reads them with no
//           voice sounding
//   ORDER   in blocks of 100: noteOn before its voice's start and noteOff before its voice's stop; a note-on at a tick's
//           sample comes before that tick; the sustain pedal's notes end as it goes up (their noteOff, then their
//           stops), not at their key-up (the ticks Shared had counted at each noteOff); a key pressed again while its
//           note is on gets its noteOff and noteOn, then its voice's stop and start; All Notes Off ends each note (its
//           noteOff, in key order) before the voices' stops; prepare() ends the note that sounds (its noteOff, then its
//           stop). noteOn's velocity is the voice's, a velocity past 1 clamped to 1; each noteOn gets one noteOff
//   DIALS   a param of Shared named as a dial: Shared's init reads the dial's default (not the param's own), its first
//           tick the value set before the first block (a float's, an int's, a bool's); a dial moved glides in Shared as
//           its stream does, tick by tick, from the block it moves in to exactly its value 20 ms later, never back; a
//           param of Shared that no dial is named as keeps its own default, with a note; one named as a circuit's pot
//           (a netlist's module whose set() the program never calls) reads that pot's dial, its default in init
//   SWAP    a new program's Shared hears the held key start again (its noteOn, then the voice's start), and that key's
//           release (its noteOff, then the stop); its noteOn there reads the dials its first glides snap to (the host's
//           value, not the old program's glide under way) and, carried (Controls.h's DialCarry), the carry's value
//   REVERT  a program that stops in its first block, offered revertible while a key is held: the Engine goes back, and
//           the old program's Shared takes up its note again (noteOff, noteOn, as the key starts again on it); one that
//           stops at a later note-on: a key let go since gets its noteOff at the revert, or, the pedal down, when the
//           pedal comes up; a key let go under the pedal before the swap whose pedal comes up while the old program
//           goes gets its noteOff then, before its voice's stop, and nothing at the revert
//   OTHER   a Shared method named tick, noteOn or noteOff declared otherwise is the program's own: never called, a note
//           at its place says so, and the program plays bit for bit as the same program with those methods renamed;
//           one that could allocate is refused with the audio thread's words
//   FROZEN  the frozen copy plays the hooks: while it plays, the ticks and the note hooks go on
//   BLOCKS  a program whose voices read what its Shared's tick moves (an LFO's phase, a one-pole) and whose noteOn
//           counts the notes gives the same bits in blocks of 64, 333, 1 and a mix (a release mid-block, the pedal)
//   TRACE   trace of it: nothing found, its calls of one sample giving the same bits; a NaN its tick makes before any
//           note found in Shared's field
//   STOP    a tick that never ends stops the program as any call does: the stop names Shared's tick()
// An effect:
//   NONE    its Shared's tick, noteOn and noteOff are its own (never called, no note) and a param of Shared named as a
//           dial keeps its default: the effect's Shared has its init and end only
#include "Probe.h"

#include "AutoClipper.h"
#include "Rig.h"

#include "Product.h"

#include "dsp/Debug.h"
#include "dsp/Measure.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace pg = playground;
namespace ms = playground::measure;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr bool kInstrument = pg::product::kIsInstrument;

    // A field of the program's Shared (or of instance 0), read through its field map (Patch::fieldMap); NaN when none.
    double field(const pg::Patch* p, const std::string& path)
    {
        if (p == nullptr)
            return std::nan("");
        for (const pg::Patch::Field& f : p->fieldMap())
            if (f.path == path)
                return p->read(f, 0);
        return std::nan("");
    }

    // Arrays of the program's Shared as far as a count of its says: shared->array[0 .. count), then `then`'s, `kept` at
    // most (the field map lists an array of 16 elements at most).
    std::vector<int> entries(const pg::Patch* p, const std::string& count, int kept, const std::string& array, int size,
                             const std::string& then = {})
    {
        std::vector<int> out;
        const double n = field(p, "shared->" + count);
        for (int k = 0; std::isfinite(n) && k < std::min(static_cast<int>(n), kept); ++k)
        {
            const std::string at = k < size ? array + "[" + std::to_string(k) + "]" : then + "[" + std::to_string(k - size) + "]";
            const double v = field(p, "shared->" + at);
            out.push_back(std::isfinite(v) ? static_cast<int>(v) : -1);
        }
        return out;
    }

    // The log an ORDER program keeps in its Shared (shared->log, then more, as far as count), and the ticks it counted
    // at each noteOff.
    std::vector<int> logOf(const pg::Patch* p) { return entries(p, "count", 24, "log", 16, "more"); }
    std::vector<int> offTicksOf(const pg::Patch* p) { return entries(p, "offs", 8, "offTicks", 8); }

    std::string text(const std::vector<int>& v)
    {
        std::string s;
        for (const int x : v)
            s += (s.empty() ? "" : " ") + std::to_string(x);
        return s;
    }

    probe::Note on(std::size_t at, int note, float velocity = 0.8f) { return { at, pg::EventKind::noteOn, note, velocity }; }
    probe::Note off(std::size_t at, int note) { return { at, pg::EventKind::noteOff, note, 0.0f }; }
    probe::Note pedal(std::size_t at, bool down) { return { at, pg::EventKind::controller, 64, down ? 1.0f : 0.0f }; }
    probe::Note allOff(std::size_t at) { return { at, pg::EventKind::allNotesOff, 0, 0.0f }; }

    // TICK: Shared counts its ticks and their samples; a display reads the count.
    const char* const kTicks = R"(#pragma panel number shared->ticks
module Shared
{
    int ticks;
    int samples;
    void tick(int n)
    {
        ticks += 1;
        samples += n;
    }
};

module Voice
{
    Shared* shared;
    bool held;
    void start(float note, float velocity) { held = true; }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n) { return held; }
};
)";

    // ORDER (and SWAP, REVERT, FROZEN): the hooks and the voices' calls logged in Shared, a code each: 1000 + note a
    // noteOn, 2000 + key a start, 3000 + key a stop, 4000 + note a noteOff (24 kept, in two arrays of at most 16, as the
    // field map lists them, and all counted); the tick a noteOn is
    // followed by, as Shared's tick count then; and the ticks counted at each noteOff (8 kept).
    const char* const kOrder = R"(module Shared
{
    int log[16], more[8];
    int offTicks[8];
    int count, ons, offs, ticks, watched;
    bool watch;
    float velocity;
    void put(int code)
    {
        if (count < 16)
            log[count] = code;
        else if (count < 24)
            more[count - 16] = code;
        count += 1;
    }
    void tick(int n)
    {
        ticks += 1;
        if (watch)
            watched = ticks;
        watch = false;
    }
    void noteOn(float note, float v)
    {
        put(1000 + (int)note);
        ons += 1;
        velocity = v;
        if (watched == 0)
            watch = true;
    }
    void noteOff(float note)
    {
        put(4000 + (int)note);
        if (offs < 8)
            offTicks[offs] = ticks;
        offs += 1;
    }
};

module Voice
{
    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];
    Shared* shared;
    int key;
    bool held;
    float velocity;
    void start(float note, float v)
    {
        key = (int)note;
        held = true;
        velocity = v;
        shared->put(2000 + key);
    }
    void stop()
    {
        held = false;
        shared->put(3000 + key);
    }
    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            left[i] += 0.01f * level[i];
            right[i] += 0.01f * level[i];
        }
        return held;
    }
};
)";

    // DIALS: Shared's params named as the voice's dials (and one named as none).
    const char* const kDials = R"(module Shared
{
    param float drive [[fminus::default(0.25)]];
    param int mode;
    param bool on;
    param float stray [[fminus::default(0.75)]];
    float atInit, atTick, prev, atNote;
    int modeAtTick, onAtTick, ticks, firstMove, landed, back;
    float strayAt;
    void init(float rate)
    {
        atInit = drive[0];
        firstMove = -1;
        landed = -1;
    }
    void tick(int n)
    {
        if (ticks == 0)
        {
            atTick = drive[0];
            modeAtTick = mode[0];
            onAtTick = on[0] ? 1 : 0;
            prev = drive[0];
        }
        if (drive[0] != prev && firstMove < 0)
            firstMove = ticks;
        if (drive[0] > prev)
            back += 1;
        if (drive[0] == 3.0f && landed < 0)
            landed = ticks;
        prev = drive[0];
        strayAt = stray[0];
        ticks += 1;
    }
    void noteOn(float note, float velocity) { atNote = drive[0]; }
};

module Voice
{
    param float drive [[fminus::range(0, 10)]] [[fminus::default(2)]];
    param int mode [[fminus::range(0, 3)]] [[fminus::default(1)]];
    param bool on [[fminus::default(0)]];
    Shared* shared;
    bool held;
    void start(float note, float velocity) { held = true; }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n) { return held; }
};
)";

    // DIALS: a param of Shared named as a circuit's pot, AutoClipper.h's potclip.cir (its pot drive, 0.5 by default), whose
    // set() the voice never calls: the pot is a dial.
    const char* const kPotShared = R"(#include "potclip.cir"

module Shared
{
    param float drive [[fminus::default(0.9)]];
    float atInit, atTick;
    void init(float rate) { atInit = drive[0]; }
    void tick(int n) { atTick = drive[0]; }
};

module Voice
{
    PotClip clip;
    Shared* shared;
    bool held;
    void init(float rate) { clip.init(rate); }
    void start(float note, float velocity) { held = true; }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n) { return held; }
};
)";

    // OTHER: Shared's methods of the hooks' names declared otherwise (`name` tick, noteOn, noteOff), which the voices
    // call themselves; `renamed`: the same program with them named step, begin and end.
    std::string ownMethods(bool renamed)
    {
        const std::string tick = renamed ? "step" : "tick", noteOn = renamed ? "begin" : "noteOn",
                          noteOff = renamed ? "finish" : "noteOff";
        return "module Shared\n{\n    float phase;\n    int notes;\n"
               "    float " + tick + "() { phase += 0.01f; if (phase >= 1) phase -= 1; return phase; }\n"
               "    void " + noteOn + "(int key) { notes += 1; }\n"
               "    void " + noteOff + "(float note, float velocity) { notes -= 1; }\n};\n\n"
               "module Voice\n{\n    Shared* shared;\n    bool held;\n    float env;\n"
               "    void start(float note, float velocity) { held = true; env = 1; shared->" + noteOn + "((int)note); }\n"
               "    void stop() { held = false; shared->" + noteOff + "(0, 0); }\n"
               "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
               "        for (int i = 0; i < n; ++i)\n        {\n            if (!held)\n                env *= 0.999f;\n"
               "            const float y = 0.1f * env * shared->" + tick + "();\n"
               "            left[i] += y;\n            right[i] += y;\n        }\n        return held || env > 0.001f;\n    }\n};\n";
    }

    // BLOCKS and TRACE: the voices read an LFO and a one-pole Shared's tick moves, and a count its noteOn keeps.
    const char* const kMoving = R"(module Shared
{
    param float rate [[fminus::default(3)]];
    float phase, slow, sr;
    int notes;
    void init(float r) { sr = r; }
    void tick(int n)
    {
        phase += rate[0] * (float)n / sr;
        if (phase >= 1)
            phase -= 1;
        slow += 0.002f * (phase - slow);
    }
    void noteOn(float note, float velocity) { notes += 1; }
    void noteOff(float note) { notes -= 1; }
};

module Voice
{
    param float rate [[fminus::range(0.1, 10)]] [[fminus::default(3)]];
    Shared* shared;
    float p, step, env;
    bool held;
    void init(float r) { step = 0; }
    void start(float note, float velocity)
    {
        step = 440 * expf(0.0577622650f * (note - 69)) / 48000;
        held = true;
        if (env <= 0.0001f)
            p = 0;
    }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            env += held ? 0.002f * (1 - env) : -0.0005f * env;
            p += step * (1 + 0.02f * sinf(6.28318531f * shared->phase));
            if (p >= 1)
                p -= 1;
            const float y = 0.2f * env * (shared->slow + 0.5f) * sinf(6.28318531f * p) * (1 + 0.01f * (float)shared->notes);
            left[i] += y;
            right[i] += y;
        }
        return held || env > 0.0001f;
    }
};
)";

    // TRACE: Shared's tick makes a NaN after 1,000 ticks (about 0.33 s), before any note.
    const char* const kNanTick = R"(module Shared
{
    int ticks;
    float bad;
    void tick(int n)
    {
        ticks += 1;
        if (ticks > 1000)
            bad = sqrtf(-1.0f - (float)ticks);
    }
};

module Voice
{
    Shared* shared;
    float env;
    bool held;
    void start(float note, float velocity) { held = true; }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            env += held ? 0.01f * (1 - env) : -0.01f * env;
            left[i] += 0.1f * env;
            right[i] += 0.1f * env;
        }
        return held || env > 0.0001f;
    }
};
)";

    // STOP: a tick that never ends, after a while.
    const char* const kSpinTick = R"(module Shared
{
    int ticks, spins;
    void tick(int n)
    {
        ticks += 1;
        if (ticks > 100)
            while (1)
                spins = spins + 1;
    }
};

module Voice
{
    Shared* shared;
    bool held;
    void start(float note, float velocity) { held = true; }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            left[i] += 0.1f;
            right[i] += 0.1f;
        }
        return held;
    }
};
)";

    // REVERT: a program whose start never ends.
    const char* const kSpinStart = R"(module Voice
{
    int spins;
    void start(float note, float velocity)
    {
        while (1)
            spins = spins + 1;
    }
    bool process(float* restrict left, float* restrict right, int n) { return true; }
};
)";

    // REVERT: a program whose start never ends for a note above 70, so that it stops at a later note-on.
    const char* const kSpinHigh = R"(module Voice
{
    int spins;
    bool held;
    void start(float note, float velocity)
    {
        held = true;
        if (note > 70)
            while (1)
                spins = spins + 1;
    }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n) { return held; }
};
)";

    // The effect's: Shared's methods of the hooks' names and a param named as the dial, none of which the plugin uses.
    [[maybe_unused]] const char* const kEffectShared = R"(module Shared
{
    param float gain [[fminus::default(0.25)]];
    int ticks, notes;
    float seen;
    void tick(int n) { ticks += 1; }
    void noteOn(float note, float velocity) { notes += 1; }
    void noteOff(float note) { notes -= 1; }
    void look() { seen = gain[0]; }
};

module Effect
{
    param float gain [[fminus::range(0, 1)]] [[fminus::default(0.5)]];
    Shared* shared;
    void process(float* restrict left, float* restrict right, int n)
    {
        shared->look();
        for (int i = 0; i < n; ++i)
        {
            left[i] *= gain[i];
            right[i] *= gain[i];
        }
    }
};
)";

    probe::Stereo silence(std::size_t n) { return probe::Stereo(n); }

    // One block of 256 samples of silence through the Engine, the dials `norm` in program `meaning`'s controls (0: the
    // one that plays; Engine::setControls), with `events`.
    void block(pg::Engine& e, const std::array<float, pg::kMaxControls>& norm, uint64_t meaning = 0,
               const std::vector<pg::Event>& events = {})
    {
        std::vector<float> l(256), r(256);
        float* channels[2] = { l.data(), r.data() };
        e.setControls(norm, meaning);
        e.process(channels, 2, 256, events);
    }

    // A Rig with a program loaded; false (the errors printed) when it does not compile.
    bool loaded(probe::Rig& rig, const std::string& source, std::string* notes = nullptr)
    {
        std::string errors;
        const bool ok = rig.load(source, &errors);
        if (!ok)
            std::printf("%s", errors.c_str());
        if (notes != nullptr)
            *notes = errors;
        return ok;
    }

    bool has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

    float dialNorm(const probe::Rig& rig, int c, float plain) { return pg::normOf(rig.controls()[static_cast<std::size_t>(c)], plain); }

    // A row that a text is the one wanted (both printed when it is not).
    bool sameText(funkgui::test::Probe& P, const char* key, const std::string& got, const std::string& want)
    {
        if (got != want)
            std::printf("%s: got \"%s\", want \"%s\"\n", key, got.c_str(), want.c_str());
        return P.eq(key, got == want ? 1 : 0, 1);
    }

    // A number exactly (a count, a value the Engine set): a NaN (no such field) fails.
    bool exactly(funkgui::test::Probe& P, const char* key, double got, double want) { return P.near(key, got, want, 0.0); }
}

PROBE(dsp, shared)
{
    if constexpr (!kInstrument)
    {
        // ---- NONE ------------------------------------------------------------------------------------------------------
        probe::Rig rig(kFs, 512);
        std::string notes;
        P.eq("none.compiles", loaded(rig, kEffectShared, &notes) ? 1 : 0, 1);
        const pg::Patch* p = rig.engine().playingPatch();
        P.eq("none.no_hooks", p != nullptr && !p->sharedTicks() && !p->sharedNotes() && p->sharedDials() == 0 ? 1 : 0, 1);
        P.eq("none.no_note", has(notes, "Shared::") ? 0 : 1, 1);
        rig.norm()[0] = 1.0f;
        probe::Stereo in(48000);
        in.l = probe::sine(440.0, 0.5, kFs, in.size());
        in.r = in.l;
        rig.run(in, 256);
        exactly(P, "none.no_ticks", field(p, "shared->ticks"), 0.0);
        exactly(P, "none.no_notes", field(p, "shared->notes"), 0.0);
        exactly(P, "none.param_default", field(p, "shared->seen"), 0.25);
        return P.finish();
    }

    // ---- TICK ----------------------------------------------------------------------------------------------------------
    {
        probe::Rig rig(kFs, 512);
        P.eq("tick.compiles", loaded(rig, kTicks) ? 1 : 0, 1);
        const pg::Patch* p = rig.engine().playingPatch();
        P.eq("tick.has_hook", p != nullptr && p->sharedTicks() ? 1 : 0, 1);
        rig.run(silence(48000), 256);
        exactly(P, "tick.ticks_no_note", field(p, "shared->ticks"), 3000.0);
        exactly(P, "tick.samples_no_note", field(p, "shared->samples"), 48000.0);
        pg::Meters m;
        const bool read = rig.engine().meters(m);
        P.eq("tick.display_reads_it", read && m.voices == 0 && m.taps[0] == 3000.0f ? 1 : 0, 1);
    }

    // ---- ORDER ---------------------------------------------------------------------------------------------------------
    {
        probe::Rig rig(kFs, 512);
        P.eq("order.compiles", loaded(rig, kOrder) ? 1 : 0, 1);
        const pg::Patch* p = rig.engine().playingPatch();
        P.eq("order.has_hooks", p != nullptr && p->sharedTicks() && p->sharedNotes() ? 1 : 0, 1);
        // In blocks of 100: 4096 (a tick's sample) is mid-block.
        const std::vector<probe::Note> first = { on(4096, 60), off(5000, 60), pedal(6000, true), on(6100, 62),
                                                 off(6500, 62), pedal(7000, false) };
        rig.run(silence(7200), 100, first);
        sameText(P, "order.note_and_pedal", text(logOf(p)), "1060 2060 4060 3060 1062 2062 4062 3062");
        // The noteOn at 4096 came before that sample's tick: the 257th (ticks at 0, 16, ...).
        exactly(P, "order.event_before_tick", field(p, "shared->watched"), 257.0);
        exactly(P, "order.velocity", field(p, "shared->velocity"), 0.8f);
        // The ticks Shared had counted at each noteOff (those at 0, 16, ... before it): 60's at its key-up (5000: 313),
        // 62's at the pedal's going up (7000: 438), not at its key-up under the pedal (6500: 407).
        sameText(P, "order.pedal_note_off_at_pedal_up", text(offTicksOf(p)), "313 438");
    }
    {
        probe::Rig rig(kFs, 512);
        loaded(rig, kOrder);
        const pg::Patch* p = rig.engine().playingPatch();
        rig.run(silence(1000), 100, { on(100, 64), on(200, 64), off(300, 64) });
        sameText(P, "order.retrigger", text(logOf(p)), "1064 2064 4064 1064 3064 2064 4064 3064");
    }
    {
        probe::Rig rig(kFs, 512);
        loaded(rig, kOrder);
        const pg::Patch* p = rig.engine().playingPatch();
        rig.run(silence(1000), 100, { on(100, 65), on(100, 67), allOff(500) });
        std::vector<int> log = logOf(p);
        if (log.size() == 8)
            std::sort(log.begin() + 6, log.end());           // the voices' stops: in their table's order
        sameText(P, "order.all_notes_off", text(log), "1065 2065 1067 2067 4065 4067 3065 3067");
        rig.run(silence(500), 100, { on(0, 69, 1.5f) });   // a velocity past 1
        rig.engine().prepare(kFs, 512);
        const std::vector<int> all = logOf(p);
        sameText(P, "order.prepare", text(std::vector<int>(all.begin() + std::min<std::ptrdiff_t>(8, std::ssize(all)), all.end())),
                 "1069 2069 4069 3069");
        P.eq("order.balanced", field(p, "shared->ons") == field(p, "shared->offs") ? 1 : 0, 1);
        exactly(P, "order.velocity_clamped", field(p, "shared->velocity"), 1.0);
    }

    // ---- DIALS ---------------------------------------------------------------------------------------------------------
    {
        probe::Rig rig(kFs, 512);
        std::string notes;
        P.eq("dials.compiles", loaded(rig, kDials, &notes) ? 1 : 0, 1);
        P.eq("dials.stray_note", has(notes, "Shared::stray reads no dial") ? 1 : 0, 1);
        P.eq("dials.no_note_for_named", has(notes, "Shared::drive") ? 0 : 1, 1);
        const pg::Patch* p = rig.engine().playingPatch();
        P.eq("dials.mask", p != nullptr ? static_cast<int64_t>(p->sharedDials()) : -1, 7);
        exactly(P, "dials.init_default", field(p, "shared->atInit"), 2.0);
        rig.norm()[0] = dialNorm(rig, 0, 7.0f);
        rig.norm()[1] = dialNorm(rig, 1, 3.0f);
        rig.norm()[2] = 1.0f;
        // The dial set before the first block; moved to 3 at block 20 (sample 5120), gliding 20 ms (960 samples).
        rig.run(silence(256 * 40), 256, {}, {}, [&](std::size_t pos, std::array<float, pg::kMaxControls>& n) {
            if (pos == 5120)
                n[0] = dialNorm(rig, 0, 3.0f);
        });
        exactly(P, "dials.first_tick_float", field(p, "shared->atTick"), 7.0);
        exactly(P, "dials.first_tick_int", field(p, "shared->modeAtTick"), 3.0);
        exactly(P, "dials.first_tick_bool", field(p, "shared->onAtTick"), 1.0);
        exactly(P, "dials.glide_starts", field(p, "shared->firstMove"), 320.0);     // the tick at 5120
        exactly(P, "dials.glide_lands", field(p, "shared->landed"), 380.0);        // the tick at 6080, 960 samples on
        exactly(P, "dials.glide_never_back", field(p, "shared->back"), 0.0);
        exactly(P, "dials.stray_default", field(p, "shared->strayAt"), 0.75);
        rig.run(silence(256), 256, { on(10, 60) });
        exactly(P, "dials.at_note", field(p, "shared->atNote"), 3.0);
    }
    {
        // A param of Shared named as a circuit's pot reads that pot's dial.
        probe::Rig rig(kFs, 512);
        std::string notes;
        const bool ok = rig.load(pg::Sources{ { pg::kProgramName, kPotShared }, { "potclip.cir", probe::kPotClipper } }, &notes);
        if (!ok)
            std::printf("%s", notes.c_str());
        P.eq("dials.circuit_compiles", ok ? 1 : 0, 1);
        int pot = -1;
        for (int c = 0; c < pg::kMaxControls; ++c)
            if (rig.controls()[static_cast<std::size_t>(c)].used && std::string(rig.controls()[static_cast<std::size_t>(c)].name) == "drive")
                pot = c;
        const pg::Patch* p = rig.engine().playingPatch();
        P.eq("dials.circuit_pot", pot >= 0 && p != nullptr && p->sharedDials() == (1u << pot) && !has(notes, "Shared::drive") ? 1 : 0, 1);
        exactly(P, "dials.circuit_init_default", field(p, "shared->atInit"), 0.5);
        if (pot >= 0)
            rig.norm()[static_cast<std::size_t>(pot)] = dialNorm(rig, pot, 0.25f);
        rig.run(silence(512), 256);
        exactly(P, "dials.circuit_first_tick", field(p, "shared->atTick"), 0.25);
    }

    // ---- SWAP ----------------------------------------------------------------------------------------------------------
    {
        probe::Rig rig(kFs, 512);
        loaded(rig, kOrder);
        pg::Engine& e = rig.engine();
        rig.run(silence(2048), 256, { on(100, 60) });
        pg::Patch::Result next = pg::Patch::compile(kOrder, e.rate());
        P.eq("swap.compiles", next.patch ? 1 : 0, 1);
        next.patch->setProgram(99);
        e.offer(std::move(next.patch));
        rig.run(silence(4096), 256, { off(2048, 60) });   // (the note-off at the run's own sample 2048)
        const pg::Patch* p = e.playingPatch();
        P.eq("swap.new_program", p != nullptr && p->program() == 99 ? 1 : 0, 1);
        sameText(P, "swap.new_shared_hears_held_key", text(logOf(p)), "1060 2060 4060 3060");
        e.collect();
    }
    {
        // Its noteOn as the held key starts again on it reads what its first glides snap to: the host's value (3), not
        // the old program's glide there from 7, under way.
        probe::Rig rig(kFs, 512);
        loaded(rig, kDials);
        pg::Engine& e = rig.engine();
        std::array<float, pg::kMaxControls> n = rig.norm();
        n[0] = dialNorm(rig, 0, 7.0f);
        block(e, n, 0, { pg::Event{ 10, pg::EventKind::noteOn, 60, 0.8f } });
        block(e, n);
        n[0] = dialNorm(rig, 0, 3.0f);                   // gliding for 20 ms (960 samples) from here
        block(e, n);
        const double gliding = field(e.playingPatch(), "shared->prev");   // where the old program's last tick had it
        pg::Patch::Result next = pg::Patch::compile(kDials, e.rate());
        P.eq("swap.dials_compiles", next.patch ? 1 : 0, 1);
        next.patch->setProgram(99);
        e.offer(std::move(next.patch));
        block(e, n);                                     // taken at the block's start
        const pg::Patch* p = e.playingPatch();
        P.eq("swap.dials_old_gliding", p != nullptr && p->program() == 99 && gliding > 3.0 && gliding < 7.0 ? 1 : 0, 1);
        exactly(P, "swap.dials_snap_at_note", field(p, "shared->atNote"), 3.0);
        exactly(P, "swap.dials_snap_first_tick", field(p, "shared->atTick"), 3.0);
        e.collect();
    }
    {
        // A dial carried (Controls.h's DialCarry, as the processor's carrier makes it): the carry's value while the host's
        // is the one it was made from, in its noteOn there and its first tick.
        probe::Rig rig(kFs, 512);
        loaded(rig, kDials);
        pg::Engine& e = rig.engine();
        const uint64_t from = rig.program();
        std::array<float, pg::kMaxControls> n = rig.norm();
        n[0] = dialNorm(rig, 0, 7.0f);
        block(e, n, 0, { pg::Event{ 10, pg::EventKind::noteOn, 60, 0.8f } });
        block(e, n);
        pg::Patch::Result next = pg::Patch::compile(kDials, e.rate());
        pg::DialCarry carry;
        carry.from = from;
        carry.slots = 1u;                                // drive's
        carry.fromNorm = n;
        carry.plain[0] = 4.5f;
        next.patch->setCarry(carry);
        next.patch->setProgram(99);
        e.offer(std::move(next.patch));
        block(e, n, from);                               // the host's values still in the old program's controls
        const pg::Patch* p = e.playingPatch();
        P.eq("swap.dials_carried_new", p != nullptr && p->program() == 99 ? 1 : 0, 1);
        exactly(P, "swap.dials_carried_at_note", field(p, "shared->atNote"), 4.5);
        exactly(P, "swap.dials_carried_first_tick", field(p, "shared->atTick"), 4.5);
        e.collect();
    }

    // ---- REVERT --------------------------------------------------------------------------------------------------------
    {
        probe::Rig rig(kFs, 512);
        loaded(rig, kOrder);
        pg::Engine& e = rig.engine();
        rig.run(silence(2048), 256, { on(100, 60) });
        const pg::Patch* old = e.playingPatch();
        pg::Patch::Result next = pg::Patch::compile(kSpinStart, e.rate());
        P.eq("revert.compiles", next.patch ? 1 : 0, 1);
        next.patch->setProgram(77);
        e.offer(std::move(next.patch), true);
        rig.run(silence(2048), 256, { off(1024, 60) });
        P.eq("revert.went_back", e.playingPatch() == old && e.stops() == 1 ? 1 : 0, 1);
        sameText(P, "revert.shared_takes_it_up", text(logOf(old)), "1060 2060 4060 1060 2060 3060 4060 3060");
        P.eq("revert.balanced", field(old, "shared->ons") == field(old, "shared->offs") ? 1 : 0, 1);
        e.collect();
    }
    {
        // A program that stops at a later note-on (72's, 300 samples after the swap), in blocks of 100: `before` played
        // on the old program, `after` from the swap on. The old program's log, and whether its noteOns and noteOffs
        // balance.
        const auto revertLater = [&](const char* key, const std::vector<probe::Note>& before, const std::vector<probe::Note>& after,
                                     const std::string& want) {
            probe::Rig rig(kFs, 512);
            loaded(rig, kOrder);
            pg::Engine& e = rig.engine();
            rig.run(silence(2000), 100, before);
            const pg::Patch* old = e.playingPatch();
            pg::Patch::Result next = pg::Patch::compile(kSpinHigh, e.rate());
            next.patch->setProgram(77);
            e.offer(std::move(next.patch), true);
            rig.run(silence(2000), 100, after);
            const bool back = e.playingPatch() == old && e.stops() == 1;
            const bool balanced = field(old, "shared->ons") == field(old, "shared->offs");
            if (!back || !balanced)
                std::printf("%s: went back %d, balanced %d\n", key, back ? 1 : 0, balanced ? 1 : 0);
            sameText(P, key, back && balanced ? text(logOf(old)) : std::string("-"), want);
            e.collect();
        };
        // 60 let go before the revert: its noteOff at the revert; the held 62 ends and starts again; 72 starts. The old
        // program's voices fading since the swap are its again (Engine.h, "A program that stops"): 62's stops with the
        // new one at its key-up (3062 twice), 60's as its fade ends (the last 3060), each after its note's noteOff.
        revertLater("revert.later_released", { on(100, 60), on(200, 62) }, { off(100, 60), on(300, 72), off(600, 62), off(700, 72) },
                    "1060 2060 1062 2062 4060 4062 1062 2062 1072 2072 4062 3062 3062 4072 3072 3060");
        // The same under the pedal, down after the swap: 60's noteOff when the pedal comes up, after the revert.
        revertLater("revert.later_pedal", { on(100, 60), on(200, 62) },
                    { pedal(50, true), off(100, 60), on(300, 72), pedal(500, false), off(600, 62), off(700, 72) },
                    "1060 2060 1062 2062 4062 1062 2062 1072 2072 4060 4062 3062 3062 4072 3072 3060");
        // 60 let go under the pedal before the swap, the pedal up while the old program goes: its noteOff then, before
        // the stop the pedal's going up gives its voice, and nothing of it at the revert.
        revertLater("revert.pedal_up_while_going", { on(100, 60), on(200, 62), pedal(300, true), off(400, 60) },
                    { pedal(100, false), on(300, 72), off(600, 62), off(700, 72) },
                    "1060 2060 1062 2062 4060 3060 4062 1062 2062 1072 2072 4062 3062 3062 4072 3072");
    }

    // ---- OTHER ---------------------------------------------------------------------------------------------------------
    {
        const std::vector<probe::Note> notes = { on(0, 60), on(4000, 64), off(9000, 60), off(12000, 64) };
        probe::Rig own(kFs, 512), renamed(kFs, 512);
        std::string ownNotes, renamedNotes;
        P.eq("other.compiles", loaded(own, ownMethods(false), &ownNotes) && loaded(renamed, ownMethods(true), &renamedNotes) ? 1 : 0, 1);
        const pg::Patch* p = own.engine().playingPatch();
        P.eq("other.no_hooks", p != nullptr && !p->sharedTicks() && !p->sharedNotes() ? 1 : 0, 1);
        P.eq("other.note_tick", has(ownNotes, "program.fm:5:11: note: Shared::tick is not declared as the plugin calls it "
                                              "(`void tick(int n)`), so the plugin never calls it") ? 1 : 0, 1);
        P.eq("other.note_on_off", has(ownNotes, "Shared::noteOn is not declared") && has(ownNotes, "Shared::noteOff is not declared") ? 1 : 0, 1);
        P.eq("other.renamed_no_note", has(renamedNotes, "not declared as the plugin") ? 0 : 1, 1);
        const probe::Stereo a = own.run(silence(24000), 333, notes), b = renamed.run(silence(24000), 333, notes);
        P.eq("other.plays_as_renamed", probe::sameBits(a.l, b.l) && probe::sameBits(a.r, b.r) && probe::peak(a.l) > 0.001f ? 1 : 0, 1);
        const std::string allocating = "module Shared\n{\n    float* buf;\n    void tick(int n) { buf = calloc(16, sizeof(float)); }\n};\n\n"
                                       "module Voice\n{\n    Shared* shared;\n    void start(float note, float velocity) {}\n"
                                       "    bool process(float* restrict left, float* restrict right, int n) { return false; }\n};\n";
        const pg::Patch::Result r = pg::Patch::compile(allocating, static_cast<float>(kFs));
        std::printf("allocating tick: %s", r.errors.c_str());
        P.eq("other.allocating_refused", !r.patch && has(r.errors, "Shared::tick: it runs on the audio thread") ? 1 : 0, 1);
    }

    // ---- FROZEN --------------------------------------------------------------------------------------------------------
    {
        probe::Rig rig(kFs, 512);
        loaded(rig, kOrder);
        pg::Engine& e = rig.engine();
        rig.run(silence(2048), 256);
        std::string why;
        P.eq("frozen.compiles", rig.freeze(&why) ? 1 : 0, 1);
        const pg::Patch* p = e.playingPatch();
        const double before = field(p, "shared->ticks");
        rig.run(silence(256), 256);                            // the copy taken at this block's start
        const bool playing = e.frozen();
        rig.run(silence(4096), 256, { on(1000, 60), off(3000, 60) });
        P.eq("frozen.plays", playing && e.frozen() ? 1 : 0, 1);
        exactly(P, "frozen.ticks_go_on", field(p, "shared->ticks") - before, (256.0 + 4096.0) / 16.0);
        sameText(P, "frozen.note_hooks", text(logOf(p)), "1060 2060 4060 3060");
        e.collect();
    }

    // ---- BLOCKS --------------------------------------------------------------------------------------------------------
    {
        const std::vector<probe::Note> phrase = { on(1000, 60, 0.7f), on(5003, 64, 0.9f), off(9001, 60), pedal(10000, true),
                                                  off(12345, 64), on(13000, 67), pedal(17777, false), off(20000, 67) };
        const auto render = [&](int block, const std::vector<int>& sizes) {
            probe::Rig rig(kFs, 512);
            loaded(rig, kMoving);
            return rig.run(silence(30000), block, phrase, sizes);
        };
        const probe::Stereo a = render(64, {}), b = render(333, {}), c = render(1, {}), d = render(0, { 1, 7, 100, 513, 16, 3 });
        P.eq("blocks.sounds", probe::peak(a.l) > 0.01f ? 1 : 0, 1);
        P.eq("blocks.64_333", probe::sameBits(a.l, b.l) && probe::sameBits(a.r, b.r) ? 1 : 0, 1);
        P.eq("blocks.64_1", probe::sameBits(a.l, c.l) && probe::sameBits(a.r, c.r) ? 1 : 0, 1);
        P.eq("blocks.64_mix", probe::sameBits(a.l, d.l) && probe::sameBits(a.r, d.r) ? 1 : 0, 1);
    }

    // ---- TRACE ---------------------------------------------------------------------------------------------------------
    {
        ms::TraceRequest q;
        q.find = ms::TraceFind::any;
        q.render.phrase = { ms::PhraseNote{ 60, 0.1, 0.5, 0.8f }, ms::PhraseNote{ 64, 0.3, 0.6, 0.8f } };
        q.render.tailSeconds = 0.3f;
        const ms::TraceResult clean = ms::trace(pg::singleFile(kMoving), q);
        for (const std::string& l : clean.lines)
            std::printf("  trace: %s\n", l.c_str());
        P.eq("trace.nothing", clean.error.empty() && clean.problems.empty() ? 1 : 0, 1);
        P.eq("trace.same_bits", !clean.splitDiffers && std::any_of(clean.lines.begin(), clean.lines.end(), [](const std::string& l) {
                                    return has(l, "gave the same bits");
                                }) ? 1 : 0, 1);
        ms::TraceRequest nanQ;
        nanQ.find = ms::TraceFind::nan;
        nanQ.render.phrase = { ms::PhraseNote{ 60, 0.6, 0.2, 0.8f } };
        nanQ.render.tailSeconds = 0.1f;
        const ms::TraceResult nan = ms::trace(pg::singleFile(kNanTick), nanQ);
        for (const std::string& l : nan.lines)
            std::printf("  trace: %s\n", l.c_str());
        const ms::TraceProblem* n0 = nan.problems.empty() ? nullptr : &nan.problems.front();
        P.eq("trace.nan_found", n0 != nullptr && n0->kind == ms::TraceFind::nan ? 1 : 0, 1);
        P.eq("trace.nan_in_shared", n0 != nullptr && !n0->culprits.empty() && n0->culprits.front().path == "shared->bad" ? 1 : 0, 1);
        // Its tick 1,001 (at sample 16,000) made it, before the note at 0.6 s.
        P.le("trace.nan_before_note", n0 != nullptr ? static_cast<double>(n0->at) : 1e9, 16016.0);
    }

    // ---- STOP ----------------------------------------------------------------------------------------------------------
    {
        probe::Rig rig(kFs, 512);
        loaded(rig, kSpinTick);
        pg::Engine& e = rig.engine();
        rig.run(silence(4096), 256);
        pg::Engine::Stopped s;
        const bool read = e.lastStop(s);
        P.eq("stop.stopped", read && e.stops() == 1 && s.call == pg::Patch::Method::sharedTick ? 1 : 0, 1);
        const std::string words = pg::Patch::stopMessage(pg::singleFile(kSpinTick), s.call, static_cast<float>(kFs));
        std::printf("stop: %s", words.c_str());
        P.eq("stop.named", has(words, "program.fm:4:10: error: Shared's tick() ran past its budget") ? 1 : 0, 1);
    }
    return P.finish();
}
