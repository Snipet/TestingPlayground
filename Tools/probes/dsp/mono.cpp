// PROBE name=dsp.mono timeout=180
//
// dsp.mono: an instrument's mono programs (Patch.h and Engine.h, "Mono"): the Engine at 48 kHz in blocks of 100 unless a
// row says otherwise, with programs written for each row, whose Shared logs the hooks and the voices' calls. An
// instrument:
//   DECLARE  #pragma mono makes a program mono, with no dial; #pragma mono <bool> names its switch; a real dial, a name no
//            dial has and a second line are notes (mono always); a line in an #if 0 is none; Voice::legato declared
//            otherwise, or in a program that is not mono, is the program's own (a note at its place, never called); one
//            that could allocate is refused with the audio thread's words
//   LEGATO   a key pressed while another is held goes to the voice that sounds (legato, no start, no stop), one voice
//            on one instance; the key it sounds let go goes back to the one still held, at that key's velocity; the
//            last key let go stops it
//   RETRIGGER a note after every key is up starts a voice of its own; a program without legato starts one for every
//            note (the voice before it stopped and faded out over 2 ms); a released voice's tail fades as a new note
//            starts; one voice sounds after the fade (its level alone), and nothing steps
//   PRIORITY the newest key sounds (a lower one too); a key let go that the voice does not sound changes nothing; the
//            voice goes back to the newest key still held
//   PEDAL    the pedal holds the last note when no key does (a key then takes it by legato); a key let go under it
//            gives the voice back to one still held; the pedal's going up stops it, after Shared's noteOffs
//   SWITCH   #pragma mono <dial>: off, the voices' rules (two voices); on, the next note goes to the newest voice by
//            legato and the other fades; off again, a new note a voice of its own; the meters say mono while it is on
//            (Meters::mono)
//   BEND     the pitch wheel across notes: the voice's pitch is its note's plus the bend after a legato and after the
//            return, its one start, and its sine never steps
//   SWAP     the held keys start again on a new mono program: its Shared's noteOns in the order they were pressed, one
//            voice the newest's; that key let go gives the voice the one pressed before it
//   RESTART  a render after Engine::restart() is a fresh one's: a key the render before left held is held no more
//   FROZEN   the frozen copy calls legato
//   BLOCKS   a mono program with a glide of its own (legato, return, retrigger, the pedal, the bend) and one without
//            legato give the same bits in blocks of 64, 333, 1 and a mix; every start had its stop
//   STOP     a legato that never ends stops the program as any call does: the stop names legato()
// An effect:
//   EFFECT   #pragma mono is a note, and the program plays as it would without it
#include "Probe.h"

#include "Rig.h"

#include "Product.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr bool kInstrument = pg::product::kIsInstrument;

    // A field of the program's Shared (or of instance `i`), read through its field map (Patch::fieldMap); NaN when none.
    double field(const pg::Patch* p, const std::string& path, int i = 0)
    {
        if (p == nullptr)
            return std::nan("");
        for (const pg::Patch::Field& f : p->fieldMap())
            if (f.path == path)
                return p->read(f, i);
        return std::nan("");
    }

    // The log a program keeps in its Shared: shared->log, then more, as far as its count (24 at most: the field map
    // lists arrays of 16 elements at most).
    std::vector<int> logOf(const pg::Patch* p)
    {
        std::vector<int> out;
        const double n = field(p, "shared->count");
        for (int k = 0; std::isfinite(n) && k < std::min(static_cast<int>(n), 24); ++k)
        {
            const double v = field(p, "shared->" + (k < 16 ? "log[" + std::to_string(k) + "]" : "more[" + std::to_string(k - 16) + "]"));
            out.push_back(std::isfinite(v) ? static_cast<int>(v) : -1);
        }
        return out;
    }

    std::string text(const std::vector<int>& v)
    {
        std::string s;
        for (const int x : v)
            s += (s.empty() ? "" : " ") + std::to_string(x);
        return s;
    }

    // The log from its entry `from` on.
    std::string logFrom(const pg::Patch* p, std::size_t from)
    {
        const std::vector<int> all = logOf(p);
        return text(std::vector<int>(all.begin() + static_cast<std::ptrdiff_t>(std::min(from, all.size())), all.end()));
    }

    probe::Note on(std::size_t at, int note, float velocity = 0.8f) { return { at, pg::EventKind::noteOn, note, velocity }; }
    probe::Note off(std::size_t at, int note) { return { at, pg::EventKind::noteOff, note, 0.0f }; }
    probe::Note pedal(std::size_t at, bool down) { return { at, pg::EventKind::controller, 64, down ? 1.0f : 0.0f }; }
    probe::Note bend(std::size_t at, float value) { return { at, pg::EventKind::pitchBend, 0, value }; }

    probe::Stereo silence(std::size_t n) { return probe::Stereo(n); }

    bool has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

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

    // What compile() says of a program (its errors, then its notes), and whether it compiled.
    struct Said
    {
        bool ok = false;
        bool mono = false, legatos = false;
        int dial = -1;
        std::string words;
    };
    Said said(const std::string& source)
    {
        Said s;
        const pg::Patch::Result r = pg::Patch::compile(source, static_cast<float>(kFs));
        s.ok = r.patch != nullptr;
        s.mono = s.ok && r.patch->mono();
        s.dial = s.ok ? r.patch->monoDial() : -1;
        s.legatos = s.ok && r.patch->legatos();
        s.words = r.errors + r.notes;
        if (!s.words.empty())
            std::printf("%s", s.words.c_str());
        return s;
    }

    // A row that a text is the one wanted (both printed when it is not).
    bool sameText(funkgui::test::Probe& P, const char* key, const std::string& got, const std::string& want)
    {
        if (got != want)
            std::printf("%s: got \"%s\", want \"%s\"\n", key, got.c_str(), want.c_str());
        return P.eq(key, got == want ? 1 : 0, 1);
    }

    // A number exactly (a count, a value the Engine set): a NaN (no such field) fails.
    bool exactly(funkgui::test::Probe& P, const char* key, double got, double want) { return P.near(key, got, want, 0.0); }

    // The voices of the Engine that sound held (not released, not fading), and the newest of them (null: none).
    std::vector<pg::Engine::VoiceInfo> heldVoices(const pg::Engine& e)
    {
        std::vector<pg::Engine::VoiceInfo> all, out;
        e.voices(all);
        for (const pg::Engine::VoiceInfo& v : all)
            if (!v.released && !v.fading)
                out.push_back(v);
        return out;
    }
    int voiceCount(const pg::Engine& e)
    {
        std::vector<pg::Engine::VoiceInfo> all;
        e.voices(all);
        return static_cast<int>(all.size());
    }

    // The largest step between two samples of the left channel over [from, to).
    double largestStep(const probe::Stereo& s, std::size_t from, std::size_t to)
    {
        double most = 0.0;
        for (std::size_t i = std::max<std::size_t>(from, 1); i < std::min(to, s.l.size()); ++i)
            most = std::max(most, std::fabs(static_cast<double>(s.l[i]) - static_cast<double>(s.l[i - 1])));
        return most;
    }

    // LEGATO, RETRIGGER, PRIORITY, PEDAL, SWAP, FROZEN: a mono program whose Shared logs, a code each: 1000 + note a
    // noteOn, 2000 + key a start, 3000 + key a stop, 4000 + note a noteOff, 5000 + key a legato (24 kept, in two arrays
    // of at most 16, as the field map lists them, and all counted), the velocity of the last start or legato, and the
    // starts, stops and legatos counted. A voice's release lasts 2,000 samples.
    const char* const kLog = R"(#pragma mono
module Shared
{
    int log[16], more[8];
    int count, starts, stops, legatos;
    float velocity;
    void put(int code)
    {
        if (count < 16)
            log[count] = code;
        else if (count < 24)
            more[count - 16] = code;
        count += 1;
    }
    void noteOn(float note, float v) { put(1000 + (int)note); }
    void noteOff(float note) { put(4000 + (int)note); }
};

module Voice
{
    Shared* shared;
    int key;
    bool held;
    float env;
    void start(float note, float v)
    {
        key = (int)note;
        held = true;
        env = 1;
        shared->velocity = v;
        shared->starts += 1;
        shared->put(2000 + key);
    }
    void legato(float note, float v)
    {
        key = (int)note;
        shared->velocity = v;
        shared->legatos += 1;
        shared->put(5000 + key);
    }
    void stop()
    {
        held = false;
        shared->stops += 1;
        shared->put(3000 + key);
    }
    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            if (!held)
                env -= 0.0005f;
            if (env < 0)
                env = 0;
            left[i] += 0.01f * env;
            right[i] += 0.01f * env;
        }
        return held || env > 0;
    }
};
)";

    // The same program another way: `from` replaced by `to` (once).
    std::string with(std::string s, const std::string& from, const std::string& to)
    {
        const std::size_t at = s.find(from);
        if (at != std::string::npos)
            s.replace(at, from.size(), to);
        return s;
    }
    // A program whose Voice has no legato (its method renamed): every note a voice of its own.
    std::string noLegato(const std::string& s) { return with(s, "void legato(", "void glideTo("); }

    // RETRIGGER and SWITCH's steps: a voice of a level of 0.2 under a 240-sample attack that never ends on its own (its
    // stop changes nothing), so that only the Engine's fade takes it away; legato does nothing.
    const char* const kLevel = R"(#pragma mono
module Voice
{
    float env;
    void start(float note, float velocity) {}
    void legato(float note, float velocity) {}
    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            env += 1.0f / 240;
            if (env > 1)
                env = 1;
            left[i] += 0.2f * env;
            right[i] += 0.2f * env;
        }
        return true;
    }
};
)";

    // BEND: a sine at the voice's note plus the pitch wheel's 2 semitones, its phase never reset.
    const char* const kBend = R"(#pragma mono
#include "host.fm"
module Voice
{
    Host* host;
    float rate, phase, note, pitch;
    int starts, legatos;
    bool held;
    void init(float r) { rate = r; }
    void start(float n, float v)
    {
        note = n;
        held = true;
        starts += 1;
    }
    void legato(float n, float v)
    {
        note = n;
        legatos += 1;
    }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        pitch = note + 2 * host->pitchbend;
        const float step = 440 * expf(0.0577622650f * (pitch - 69)) / rate;
        for (int i = 0; i < n; ++i)
        {
            const float out = 0.2f * sinf(6.28318531f * phase);
            left[i] += out;
            right[i] += out;
            phase += step;
            if (phase >= 1)
                phase -= 1;
        }
        return held;
    }
};
)";

    // BLOCKS: a mono lead with a glide of its own (legato sets the target, process moves to it at a fixed rate a
    // sample), an attack and a release a sample at a time, the pitch wheel; Shared counts the starts and stops.
    const char* const kGlide = R"(#pragma mono
#include "host.fm"
module Shared
{
    int starts, stops;
};

module Voice
{
    param float glide [[fminus::range(1, 200)]] [[fminus::default(60)]];   // ms a semitone
    Shared* shared;
    Host* host;
    float rate, phase, pitch, target, env, level;
    bool held;
    void init(float r) { rate = r; }
    void start(float note, float velocity)
    {
        if (env <= 0)
            pitch = note;
        target = note;
        level = velocity;
        held = true;
        shared->starts += 1;
    }
    void legato(float note, float velocity) { target = note; }
    void stop()
    {
        held = false;
        shared->stops += 1;
    }
    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const float most = 1000 / (glide[i] * rate);
            if (pitch < target)
                pitch = fminf(pitch + most, target);
            else
                pitch = fmaxf(pitch - most, target);
            env = held ? fminf(env + 0.004f, 1) : fmaxf(env - 0.001f, 0);
            const float hz = 440 * expf(0.0577622650f * (pitch + 2 * host->pitchbend - 69));
            const float out = 0.2f * level * env * sinf(6.28318531f * phase);
            left[i] += out;
            right[i] += out;
            phase += hz / rate;
            if (phase >= 1)
                phase -= 1;
        }
        return held || env > 0;
    }
};
)";

    // STOP: a legato that never ends.
    const char* const kSpinLegato = R"(#pragma mono
module Voice
{
    int spins;
    bool held;
    void start(float note, float velocity) { held = true; }
    void legato(float note, float velocity)
    {
        while (1)
            spins = spins + 1;
    }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n) { return held; }
};
)";

    // EFFECT: a gain with a #pragma mono line.
    [[maybe_unused]] const char* const kEffectMono = R"(#pragma mono
module Effect
{
    param float gain [[fminus::range(0, 1)]] [[fminus::default(0.5)]];
    void process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            left[i] *= gain[i];
            right[i] *= gain[i];
        }
    }
};
)";
}

PROBE(dsp, mono)
{
    if constexpr (!kInstrument)
    {
        // ---- EFFECT ----------------------------------------------------------------------------------------------------
        const Said s = said(kEffectMono);
        P.eq("effect.compiles", s.ok ? 1 : 0, 1);
        P.eq("effect.note", has(s.words, "program.fm:1:1: note: #pragma mono: the effect plays no notes") ? 1 : 0, 1);
        P.eq("effect.not_mono", s.mono ? 0 : 1, 1);
        const auto render = [](const std::string& source) {
            probe::Rig rig(kFs, 512);
            loaded(rig, source);
            return rig.run(probe::noise(9600), 256);
        };
        const probe::Stereo a = render(kEffectMono), b = render(with(kEffectMono, "#pragma mono\n", ""));
        P.eq("effect.plays_as_without", probe::sameBits(a.l, b.l) && probe::sameBits(a.r, b.r) ? 1 : 0, 1);
        return P.finish();
    }

    // ---- DECLARE -----------------------------------------------------------------------------------------------------------
    {
        const Said plain = said(kLog);
        P.eq("declare.mono", plain.ok && plain.mono && plain.dial == -1 && plain.legatos ? 1 : 0, 1);
        P.eq("declare.no_note", has(plain.words, "note:") ? 0 : 1, 1);
        const std::string switched = with(with(kLog, "#pragma mono\n", "#pragma mono solo   // the panel's switch\n"),
                                          "module Voice\n{\n", "module Voice\n{\n    param bool solo [[fminus::default(1)]];\n");
        const Said dial = said(switched);
        P.eq("declare.dial", dial.ok && dial.mono && dial.dial == 0 && !has(dial.words, "note:") ? 1 : 0, 1);
        const Said real = said(with(with(kLog, "#pragma mono\n", "#pragma mono level\n"), "module Voice\n{\n",
                                    "module Voice\n{\n    param float level;\n"));
        P.eq("declare.real_dial", real.mono && real.dial == -1
                                      && has(real.words, "program.fm:1:1: note: #pragma mono: level is a real dial") ? 1 : 0, 1);
        const Said none = said(with(kLog, "#pragma mono\n", "#pragma mono solo\n"));
        P.eq("declare.no_such_dial", none.mono && none.dial == -1
                                         && has(none.words, "#pragma mono: no dial is named solo, so the program is mono always")
                                         ? 1 : 0, 1);
        const Said twice = said(with(kLog, "module Shared", "#pragma mono\nmodule Shared"));
        P.eq("declare.second_line", twice.mono && has(twice.words, "program.fm:2:1: note: #pragma mono: the program's first such line counts")
                                        ? 1 : 0, 1);
        const Said hidden = said(with(kLog, "#pragma mono\n", "#if 0\n#pragma mono\n#endif\n"));
        P.eq("declare.skipped_group", hidden.ok && !hidden.mono ? 1 : 0, 1);
        // Voice::legato declared otherwise: the program's own, a note at its place.
        const std::string otherwise = with(with(kLog, "void legato(float note, float v)", "void legato(float note)"),
                                           "        shared->velocity = v;\n        shared->legatos", "        shared->legatos");
        const Said other = said(otherwise);
        P.eq("declare.legato_otherwise", other.ok && other.mono && !other.legatos
                                             && has(other.words, ": note: Voice::legato is not declared as the plugin calls it "
                                                                 "(`void legato(float note, float velocity)`)") ? 1 : 0, 1);
        // In a program that is not mono: never called, a note.
        const std::string poly = with(kLog, "#pragma mono\n", "");
        const Said notMono = said(poly);
        P.eq("declare.legato_not_mono", notMono.ok && !notMono.mono && !notMono.legatos
                                            && has(notMono.words, ": note: Voice::legato is called only in a mono program") ? 1 : 0, 1);
        {
            probe::Rig rig(kFs, 512);
            loaded(rig, otherwise);
            rig.run(silence(1000), 100, { on(100, 60), on(300, 64), off(500, 64), off(600, 60) });
            sameText(P, "declare.otherwise_retriggers", text(logOf(rig.engine().playingPatch())),
                     "1060 2060 1064 3060 2064 4064 3064 2060 4060 3060");
        }
        {
            probe::Rig rig(kFs, 512);
            loaded(rig, poly);
            rig.run(silence(1000), 100, { on(100, 60), on(300, 64) });
            sameText(P, "declare.not_mono_two_voices", text(logOf(rig.engine().playingPatch())), "1060 2060 1064 2064");
            P.eq("declare.not_mono_held", static_cast<int>(heldVoices(rig.engine()).size()), 2);
        }
        // One that could allocate: refused, as any method the audio thread calls.
        const Said allocating = said(with(with(kLog, "    int key;\n", "    int key;\n    float* buf;\n"),
                                          "        shared->legatos += 1;\n", "        shared->legatos += 1;\n        buf = calloc(16, sizeof(float));\n"));
        P.eq("declare.legato_allocates", !allocating.ok && has(allocating.words, "Voice::legato: it runs on the audio thread") ? 1 : 0, 1);
    }

    // ---- LEGATO ------------------------------------------------------------------------------------------------------------
    {
        probe::Rig rig(kFs, 512);
        P.eq("legato.compiles", loaded(rig, kLog) ? 1 : 0, 1);
        pg::Engine& e = rig.engine();
        const pg::Patch* p = e.playingPatch();
        rig.run(silence(1500), 100, { on(100, 60, 0.5f), on(1000, 64, 0.9f) });
        sameText(P, "legato.goes_to_the_voice", text(logOf(p)), "1060 2060 1064 5064");
        const std::vector<pg::Engine::VoiceInfo> held = heldVoices(e);
        P.eq("legato.one_voice", voiceCount(e) == 1 && held.size() == 1 && held.front().key == 64 ? 1 : 0, 1);
        exactly(P, "legato.velocity", field(p, "shared->velocity"), 0.9f);
        const int instance = held.empty() ? -1 : held.front().instance;
        rig.run(silence(1000), 100, { off(500, 64) });
        sameText(P, "legato.back_to_held", logFrom(p, 4), "4064 5060");
        exactly(P, "legato.back_velocity", field(p, "shared->velocity"), 0.5f);
        const std::vector<pg::Engine::VoiceInfo> back = heldVoices(e);
        P.eq("legato.same_instance", back.size() == 1 && back.front().instance == instance && back.front().key == 60 ? 1 : 0, 1);
        rig.run(silence(1000), 100, { off(500, 60) });
        sameText(P, "legato.last_key_stops", logFrom(p, 6), "4060 3060");
        exactly(P, "legato.one_start", field(p, "shared->starts"), 1.0);
        exactly(P, "legato.one_stop", field(p, "shared->stops"), 1.0);
        exactly(P, "legato.legatos", field(p, "shared->legatos"), 2.0);
    }

    // ---- RETRIGGER ---------------------------------------------------------------------------------------------------------
    {
        // A note after every key is up: a start of its own, the released voice's tail fading out as it does.
        probe::Rig rig(kFs, 512);
        loaded(rig, kLog);
        pg::Engine& e = rig.engine();
        const pg::Patch* p = e.playingPatch();
        rig.run(silence(1100), 100, { on(100, 60), off(500, 60), on(1000, 62) });
        sameText(P, "retrigger.detached_starts", text(logOf(p)), "1060 2060 4060 3060 1062 2062");
        std::vector<pg::Engine::VoiceInfo> all;
        e.voices(all);
        P.eq("retrigger.tail_fades", all.size() == 2 && std::count_if(all.begin(), all.end(), [](const pg::Engine::VoiceInfo& v) {
                                         return v.fading && v.key == 60;
                                     }) == 1 ? 1 : 0, 1);
        rig.run(silence(200), 100);                      // the fade's 96 samples, to the grid
        P.eq("retrigger.tail_gone", voiceCount(e) == 1 ? 1 : 0, 1);
    }
    {
        // No legato: every note a voice of its own, the one before it stopped and faded.
        probe::Rig rig(kFs, 512);
        loaded(rig, noLegato(kLog));
        pg::Engine& e = rig.engine();
        const pg::Patch* p = e.playingPatch();
        rig.run(silence(1100), 100, { on(100, 60), on(1000, 64) });
        sameText(P, "retrigger.no_legato", text(logOf(p)), "1060 2060 1064 3060 2064");
        std::vector<pg::Engine::VoiceInfo> all;
        e.voices(all);
        P.eq("retrigger.no_legato_fades", all.size() == 2 && heldVoices(e).size() == 1 ? 1 : 0, 1);
        // Its key let go while 60 is held: a voice of its own for 60, the one before stopped; then 60's key-up stops it.
        rig.run(silence(1000), 100, { off(500, 64) });
        sameText(P, "retrigger.no_legato_back", logFrom(p, 5), "4064 3064 2060");
        rig.run(silence(1000), 100, { off(500, 60) });
        sameText(P, "retrigger.no_legato_ends", logFrom(p, 8), "4060 3060");
    }
    {
        // One voice after the fade (its level alone), and nothing steps: a voice that never ends on its own goes only
        // by the Engine's fade.
        const auto level = [](const std::string& source, const std::vector<probe::Note>& notes) {
            probe::Rig rig(kFs, 512);
            loaded(rig, source);
            return rig.run(silence(6000), 100, notes);
        };
        const probe::Stereo legato = level(kLevel, { on(1000, 60), on(3000, 64) });
        const probe::Stereo retrig = level(noLegato(kLevel), { on(1000, 60), on(3000, 64) });
        const probe::Stereo tail = level(noLegato(kLevel), { on(1000, 60), off(2000, 60), on(3000, 64) });
        exactly(P, "retrigger.legato_level", legato.l[5000], 0.2f);
        P.le("retrigger.legato_no_step", largestStep(legato, 1000, 6000), 0.2 / 240 + 1e-6);
        exactly(P, "retrigger.one_voice_level", retrig.l[5000], 0.2f);
        P.le("retrigger.no_step", largestStep(retrig, 1000, 6000), 0.2 / 96 + 1e-6);
        exactly(P, "retrigger.tail_one_voice_level", tail.l[5000], 0.2f);
        P.le("retrigger.tail_no_step", largestStep(tail, 1000, 6000), 0.2 / 96 + 1e-6);
    }

    // ---- PRIORITY ----------------------------------------------------------------------------------------------------------
    {
        probe::Rig rig(kFs, 512);
        loaded(rig, kLog);
        pg::Engine& e = rig.engine();
        const pg::Patch* p = e.playingPatch();
        rig.run(silence(1000), 100, { on(100, 60), on(200, 62), on(300, 64), off(400, 62) });
        sameText(P, "priority.newest", text(logOf(p)), "1060 2060 1062 5062 1064 5064 4062");
        rig.run(silence(1000), 100, { off(100, 64), on(200, 55), off(300, 55), off(400, 60) });
        sameText(P, "priority.back_lower_back", logFrom(p, 7), "4064 5060 1055 5055 4055 5060 4060 3060");
        P.eq("priority.balanced", field(p, "shared->starts") == 1.0 && field(p, "shared->stops") == 1.0 ? 1 : 0, 1);
    }

    // ---- PEDAL -------------------------------------------------------------------------------------------------------------
    {
        probe::Rig rig(kFs, 512);
        loaded(rig, kLog);
        pg::Engine& e = rig.engine();
        const pg::Patch* p = e.playingPatch();
        rig.run(silence(400), 100, { pedal(50, true), on(100, 60), off(200, 60) });
        P.eq("pedal.holds", heldVoices(e).size() == 1 ? 1 : 0, 1);
        rig.run(silence(1000), 100, { on(100, 62), off(200, 62), pedal(500, false) });
        sameText(P, "pedal.legato_then_up", text(logOf(p)), "1060 2060 1062 5062 4060 4062 3062");
        rig.run(silence(1000), 100, { pedal(50, true), on(100, 60), on(200, 62), off(300, 62), off(400, 60), pedal(500, false) });
        sameText(P, "pedal.back_under_pedal", logFrom(p, 7), "1060 2060 1062 5062 5060 4060 4062 3060");
    }

    // ---- SWITCH ------------------------------------------------------------------------------------------------------------
    {
        const std::string switched = with(with(kLog, "#pragma mono\n", "#pragma mono solo\n"), "module Voice\n{\n",
                                          "module Voice\n{\n    param bool solo;\n");
        probe::Rig rig(kFs, 512);
        P.eq("switch.compiles", loaded(rig, switched) ? 1 : 0, 1);
        pg::Engine& e = rig.engine();
        const pg::Patch* p = e.playingPatch();
        rig.run(silence(1000), 100, { on(100, 60), on(200, 64) });
        // The meters say whether it plays mono at a block's end (Meters::mono: listen's voice count).
        const auto monoNow = [&] {
            pg::Meters m;
            return e.meters(m) && m.mono;
        };
        sameText(P, "switch.off_two_voices", text(logOf(p)), "1060 2060 1064 2064");
        P.eq("switch.off_held", static_cast<int>(heldVoices(e).size()), 2);
        P.eq("switch.off_meters_not_mono", monoNow() ? 0 : 1, 1);
        rig.norm()[0] = 1.0f;                            // on: from the next note
        rig.run(silence(1000), 100, { on(100, 67) });
        sameText(P, "switch.on_legato_newest", logFrom(p, 4), "1067 3060 5067");
        const std::vector<pg::Engine::VoiceInfo> held = heldVoices(e);
        P.eq("switch.on_one_voice", held.size() == 1 && held.front().key == 67 ? 1 : 0, 1);
        P.eq("switch.on_meters_mono", monoNow() ? 1 : 0, 1);
        rig.norm()[0] = 0.0f;
        rig.run(silence(1000), 100, { on(100, 69) });
        sameText(P, "switch.off_again", logFrom(p, 7), "1069 2069");
        P.eq("switch.off_again_held", static_cast<int>(heldVoices(e).size()), 2);
        P.eq("switch.off_again_meters", monoNow() ? 0 : 1, 1);
    }
    {
        // The switch's steps: a chord of two voices, then mono: the next note's legato leaves one voice, faded.
        const std::string switched = with(with(kLevel, "#pragma mono\n", "#pragma mono solo\n"), "module Voice\n{\n",
                                          "module Voice\n{\n    param bool solo;\n");
        probe::Rig rig(kFs, 512);
        loaded(rig, switched);
        probe::Stereo out = rig.run(silence(3000), 100, { on(100, 60), on(200, 64) });
        rig.norm()[0] = 1.0f;
        const probe::Stereo then = rig.run(silence(3000), 100, { on(1000, 67) });
        P.near("switch.two_voices_level", out.l[2900], 0.4, 1e-6);
        exactly(P, "switch.one_voice_level", then.l[2900], 0.2f);
        P.le("switch.no_step", largestStep(then, 0, 3000), 0.2 / 96 + 1e-6);
    }

    // ---- BEND --------------------------------------------------------------------------------------------------------------
    {
        probe::Rig rig(kFs, 512);
        P.eq("bend.compiles", loaded(rig, kBend) ? 1 : 0, 1);
        pg::Engine& e = rig.engine();
        const pg::Patch* p = e.playingPatch();
        const auto pitch = [&] {
            const std::vector<pg::Engine::VoiceInfo> held = heldVoices(e);
            return held.size() == 1 ? field(p, "pitch", held.front().instance) : std::nan("");
        };
        const auto count = [&](const char* name) {
            const std::vector<pg::Engine::VoiceInfo> held = heldVoices(e);
            return held.size() == 1 ? field(p, name, held.front().instance) : std::nan("");
        };
        const probe::Stereo a = rig.run(silence(1500), 100, { on(100, 60), bend(500, 0.5f), on(1000, 64) });
        exactly(P, "bend.after_legato", pitch(), 65.0);
        const probe::Stereo b = rig.run(silence(1500), 100, { off(500, 64) });
        exactly(P, "bend.after_return", pitch(), 61.0);
        rig.run(silence(1000), 100, { bend(100, 0.0f) });
        exactly(P, "bend.at_rest", pitch(), 60.0);
        exactly(P, "bend.one_start", count("starts"), 1.0);
        exactly(P, "bend.legatos", count("legatos"), 2.0);
        // The sine's largest step at the highest pitch it played (65), and a little: its phase never reset.
        const double slope = 0.2 * 6.28318531 * 440.0 * std::exp2((65.0 - 69.0) / 12.0) / kFs;
        P.le("bend.no_step", std::max(largestStep(a, 100, 1500), largestStep(b, 0, 1500)), slope * 1.02);
    }

    // ---- SWAP --------------------------------------------------------------------------------------------------------------
    {
        probe::Rig rig(kFs, 512);
        loaded(rig, kLog);
        pg::Engine& e = rig.engine();
        rig.run(silence(512), 256, { on(100, 60), on(200, 64), on(300, 62) });
        pg::Patch::Result next = pg::Patch::compile(kLog, e.rate());
        P.eq("swap.compiles", next.patch ? 1 : 0, 1);
        next.patch->setProgram(99);
        e.offer(std::move(next.patch));
        rig.run(silence(256), 256);                      // taken at the block's start
        const pg::Patch* p = e.playingPatch();
        P.eq("swap.new_program", p != nullptr && p->program() == 99 ? 1 : 0, 1);
        sameText(P, "swap.pressed_order_one_voice", text(logOf(p)), "1060 1064 1062 2062");
        const std::vector<pg::Engine::VoiceInfo> held = heldVoices(e);
        P.eq("swap.one_voice", held.size() == 1 && held.front().key == 62 ? 1 : 0, 1);
        rig.run(silence(512), 256, { off(100, 62) });
        sameText(P, "swap.back_to_pressed_before", logFrom(p, 4), "4062 5064");
        e.collect();
    }

    // ---- RESTART -----------------------------------------------------------------------------------------------------------
    {
        // A render after restart() is a fresh Engine's: a key the render before left held is held no more, so the next
        // render's last key let go stops the voice rather than going back to it.
        probe::Rig rig(kFs, 512);
        loaded(rig, kLog);
        pg::Engine& e = rig.engine();
        rig.run(silence(1000), 100, { on(100, 60) });
        e.restart();
        const pg::Patch* p = e.playingPatch();
        rig.run(silence(1000), 100, { on(100, 64), off(500, 64) });
        sameText(P, "restart.no_key_held", text(logOf(p)), "1064 2064 4064 3064");
    }

    // ---- FROZEN ------------------------------------------------------------------------------------------------------------
    {
        probe::Rig rig(kFs, 512);
        loaded(rig, kLog);
        pg::Engine& e = rig.engine();
        rig.run(silence(2048), 256);
        std::string why;
        P.eq("frozen.compiles", rig.freeze(&why) ? 1 : 0, 1);
        const pg::Patch* p = e.playingPatch();
        rig.run(silence(256), 256);                      // the copy taken at this block's start
        const bool playing = e.frozen();
        rig.run(silence(4096), 256, { on(1000, 60), on(2000, 64), off(3000, 64), off(3500, 60) });
        P.eq("frozen.plays", playing && e.frozen() ? 1 : 0, 1);
        sameText(P, "frozen.legato", text(logOf(p)), "1060 2060 1064 5064 4064 5060 4060 3060");
        e.collect();
    }

    // ---- BLOCKS ------------------------------------------------------------------------------------------------------------
    {
        const std::vector<probe::Note> phrase = { on(1000, 60, 0.7f), on(3001, 64, 0.9f), bend(4000, 0.3f), off(5003, 64),
                                                  pedal(6000, true), on(7000, 67, 0.6f), off(7500, 67), off(8000, 60),
                                                  pedal(9001, false), bend(9500, 0.0f), on(16000, 62), on(16100, 65),
                                                  off(17000, 62), on(17777, 55), off(18000, 65), off(19000, 55) };
        const auto render = [&](const std::string& source, int block, const std::vector<int>& sizes, double* balance = nullptr) {
            probe::Rig rig(kFs, 512);
            loaded(rig, source);
            const probe::Stereo out = rig.run(silence(30000), block, phrase, sizes);
            if (balance != nullptr)
                *balance = field(rig.engine().playingPatch(), "shared->starts") - field(rig.engine().playingPatch(), "shared->stops");
            return out;
        };
        double balance = std::nan("");
        const probe::Stereo a = render(kGlide, 64, {}, &balance), b = render(kGlide, 333, {}), c = render(kGlide, 1, {}),
                            d = render(kGlide, 0, { 1, 7, 100, 513, 16, 3 });
        P.eq("blocks.sounds", probe::peak(a.l) > 0.01f ? 1 : 0, 1);
        P.eq("blocks.64_333", probe::sameBits(a.l, b.l) && probe::sameBits(a.r, b.r) ? 1 : 0, 1);
        P.eq("blocks.64_1", probe::sameBits(a.l, c.l) && probe::sameBits(a.r, c.r) ? 1 : 0, 1);
        P.eq("blocks.64_mix", probe::sameBits(a.l, d.l) && probe::sameBits(a.r, d.r) ? 1 : 0, 1);
        exactly(P, "blocks.every_start_stopped", balance, 0.0);
        const probe::Stereo e = render(noLegato(kGlide), 64, {}, &balance), f = render(noLegato(kGlide), 333, {});
        P.eq("blocks.no_legato_64_333", probe::peak(e.l) > 0.01f && probe::sameBits(e.l, f.l) && probe::sameBits(e.r, f.r) ? 1 : 0, 1);
        exactly(P, "blocks.no_legato_every_start_stopped", balance, 0.0);
    }

    // ---- STOP --------------------------------------------------------------------------------------------------------------
    {
        probe::Rig rig(kFs, 512);
        loaded(rig, kSpinLegato);
        pg::Engine& e = rig.engine();
        rig.run(silence(2048), 256, { on(100, 60), on(500, 64) });
        pg::Engine::Stopped s;
        const bool read = e.lastStop(s);
        P.eq("stop.stopped", read && e.stops() == 1 && s.call == pg::Patch::Method::legato ? 1 : 0, 1);
        const std::string words = pg::Patch::stopMessage(pg::singleFile(kSpinLegato), s.call, static_cast<float>(kFs));
        std::printf("stop: %s", words.c_str());
        P.eq("stop.named", has(words, "program.fm:7:10: error: legato() ran past its budget") ? 1 : 0, 1);
    }
    return P.finish();
}
