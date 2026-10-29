// PROBE name=fminus.init timeout=180
//
// fminus.init: the default program, the first of the EXAMPLES (Source/fminus/examples/<product>/0-init.fm): Init, the
// classic init patch, a clean start that sounds right as it is. At 48 kHz, in an Engine (Rig.h), the dials at their
// defaults unless a row says (plugin.process checks that a new processor plays it):
//   NAME      the first example is titled Init, and compiles with no message
//   The effect's (INPUT and OUTPUT in dB, MIX in %):
//   CLEAR     every dial at its default: the output is the input, bit for bit, in irregular blocks (1 to 512 samples) of
//             stereo noise at -6 dBFS with a 1 kHz sine, and with no alert
//   LEVELS    OUTPUT at -6 dB halves the level (the output's RMS over the input's: -6.02 dB, within 0.1); INPUT at +6 dB
//             doubles it; MIX at 50 % with INPUT at +6 dB is the average of the dry and the doubled (+3.52 dB)
//   DRY       MIX at 0 % is the dry input, bit for bit, with INPUT at +12 dB
//   GLIDE     OUTPUT turned from 0 to -24 dB between two blocks does not click: around the turn no step between
//             neighbouring samples exceeds the steeper steady run's own by more than 5 % (a hard switch there would)
//   The synth's (a voice of a band-limited sawtooth, a low-pass wide open, an ADSR on the level):
//   PITCH     a held C4 sounds at 261.63 Hz within a cent (its rising zero crossings), and the pitch wheel at its top
//             bends it 2 semitones (293.66 Hz), within a cent
//   ADSR      the level follows the envelope: at the defaults (a 2 ms attack, full sustain) full from 5 ms on and steady,
//             within 0.1 dB, until the key's release; the first sample under 0.001 (the note starts at 0); SUSTAIN at
//             0.5 holds it 6.02 dB down (within 0.1 dB), and a 200 ms ATTACK is still 6 dB down at 50 ms; after the
//             release the level falls to -60 dB in RELEASE (100 ms, within 10 ms), the voice ends, its last sample more
//             than 70 dB under the held peak, and silence follows
//   VELOCITY  a note at velocity 0.5 is 6.02 dB quieter than one at 1 (within 0.05 dB); VOLUME at -6 dB 6 dB louder
//             than at its default, -12
//   CUTOFF    the cutoff turned down darkens it: at 6 (20 Hz times 2^6, 1.28 kHz) a held A3's spectral centroid (its
//             partials, weighted by amplitude) is under half of its centroid wide open (10)
#include "Probe.h"

#include "Rig.h"

#include "Examples.h"
#include "Product.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace pg = playground;

namespace
{
    constexpr double kFs = 48000.0;
    constexpr bool kInstrument = pg::product::kIsInstrument;

    int slotOf(const probe::Rig& rig, const char* name)
    {
        for (int c = 0; c < pg::kMaxControls; ++c)
            if (rig.controls()[static_cast<std::size_t>(c)].used && std::strcmp(rig.controls()[static_cast<std::size_t>(c)].name, name) == 0)
                return c;
        return -1;
    }

    // A dial's plain value, normalised into the rig's dials (a name the program lacks changes nothing: the rows say).
    void setPlain(probe::Rig& rig, const char* name, float plain)
    {
        const int c = slotOf(rig, name);
        if (c >= 0)
            rig.norm()[static_cast<std::size_t>(c)] = pg::normOf(rig.controls()[static_cast<std::size_t>(c)], plain);
    }

    // The helpers below each serve one kind's rows, and a product's build is of one kind: maybe_unused.

    // 20 log10 of the RMS of `a` over that of `b`, over [from, from + count).
    [[maybe_unused]] double gainDb(const std::vector<float>& a, const std::vector<float>& b, std::size_t from, std::size_t count)
    {
        return probe::rmsDb(a, from, count) - probe::rmsDb(b, from, count);
    }

    [[maybe_unused]] double cents(double hz, double want) { return 1200.0 * std::log2(hz / want); }

    struct Played
    {
        std::vector<float> out;        // the left channel
        int voicesAfter = -1;          // voices sounding at the end
        uint32_t alerts = 0;           // the output guard's
    };

    // A note of the Init voice, `seconds` of it, its dials as `dials` sets them (after the defaults), the pitch wheel
    // at `bend` from the start, released at `offAt` samples (held when 0).
    template <class Dials>
    Played play(int note, float velocity, double seconds, std::size_t offAt, Dials&& dials, float bend = 0.0f)
    {
        probe::Rig rig(kFs, 512);
        rig.load(pg::kExamples[0].source);
        dials(rig);
        std::vector<probe::Note> notes;
        if (bend != 0.0f)
            notes.push_back({ 0, pg::EventKind::pitchBend, 0, bend });
        notes.push_back({ 0, pg::EventKind::noteOn, note, velocity });
        if (offAt > 0)
            notes.push_back({ offAt, pg::EventKind::noteOff, note, 0.0f });
        const probe::Stereo out = rig.run(probe::Stereo(static_cast<std::size_t>(seconds * kFs)), 256, notes);
        return { out.l, rig.engine().activeVoices(), rig.engine().takeAlerts() };
    }

    // The spectral centroid of a steady tone at `f0` (partials at its multiples up to the Nyquist frequency), each
    // partial's amplitude from a single DFT bin over [from, from + count): a whole number of its cycles.
    [[maybe_unused]] double centroidHz(const std::vector<float>& x, double f0, std::size_t from, std::size_t count)
    {
        double sum = 0.0, weighted = 0.0;
        for (int k = 1; k * f0 < 0.5 * kFs; ++k)
        {
            const double a = probe::toneAmplitude(x, k * f0, kFs, from, count);
            sum += a;
            weighted += a * k * f0;
        }
        return sum > 0.0 ? weighted / sum : 0.0;
    }

    // The largest |x| over [from, from + count).
    [[maybe_unused]] double peakOver(const std::vector<float>& x, std::size_t from, std::size_t count)
    {
        double m = 0.0;
        for (std::size_t i = from; i < std::min(x.size(), from + count); ++i)
            m = std::max(m, std::fabs(static_cast<double>(x[i])));
        return m;
    }
}

PROBE(fminus, init)
{
    // NAME
    const pg::Example& init = pg::kExamples[0];
    std::printf("the first example: %s\n", init.title);
    P.eq("name.init", static_cast<int64_t>(std::string(init.title) == "Init"), 1);   // no ?: (a constant: -Wunreachable-code)
    {
        probe::Rig rig(kFs, 512);
        std::string messages;
        P.eq("name.compiles", rig.load(init.source, &messages) ? 1 : 0, 1);
        if (!messages.empty())
            std::printf("%s", messages.c_str());
        P.eq("name.no_messages", messages.empty() ? 1 : 0, 1);
    }

    if constexpr (!kInstrument)
    {
        const std::size_t n = static_cast<std::size_t>(1.0 * kFs);
        probe::Stereo in = probe::noise(n, 0.5);
        const std::vector<float> tone = probe::sine(1000.0, 0.25, kFs, n);
        for (std::size_t i = 0; i < n; ++i)
        {
            in.l[i] += tone[i];
            in.r[i] -= tone[i];
        }
        const std::vector<int> sizes{ 1, 7, 64, 333, 2, 512, 129, 48, 256, 3, 500, 17 };
        const auto run = [&](auto&& dials, int block, const std::vector<int>& sz = {}) {
            probe::Rig rig(kFs, 512);
            rig.load(init.source);
            dials(rig);
            return rig.run(in, block, {}, sz);
        };
        {
            probe::Rig rig(kFs, 512);
            rig.load(init.source);
            P.eq("dials.input", slotOf(rig, "input") >= 0 ? 1 : 0, 1);
            P.eq("dials.mix", slotOf(rig, "mix") >= 0 ? 1 : 0, 1);
            P.eq("dials.output", slotOf(rig, "output") >= 0 ? 1 : 0, 1);
        }

        // CLEAR
        {
            probe::Rig rig(kFs, 512);
            rig.load(init.source);
            const probe::Stereo out = rig.run(in, 0, {}, sizes);
            P.eq("clear.same_bits", probe::sameBits(out.l, in.l) && probe::sameBits(out.r, in.r) ? 1 : 0, 1);
            P.eq("clear.no_alerts", static_cast<int64_t>(rig.engine().takeAlerts()), 0);
        }

        // LEVELS (the dials set before the first block: the program starts on them, no glide)
        const std::size_t from = n / 10, count = n - n / 5;
        {
            const probe::Stereo out = run([](probe::Rig& r) { setPlain(r, "output", -6.0f); }, 256);
            const double db = gainDb(out.l, in.l, from, count);
            std::printf("OUTPUT -6 dB: %.4f dB\n", db);
            P.near("levels.output_minus6_halves_db", db, -6.0206, 0.1);
            P.near("levels.output_minus6_right_db", gainDb(out.r, in.r, from, count), -6.0206, 0.1);
        }
        {
            const probe::Stereo out = run([](probe::Rig& r) { setPlain(r, "input", 6.0f); }, 256);
            P.near("levels.input_plus6_doubles_db", gainDb(out.l, in.l, from, count), 6.0206, 0.1);
        }
        {
            const probe::Stereo out = run([](probe::Rig& r) {
                setPlain(r, "input", 6.0f);
                setPlain(r, "mix", 50.0f);
            }, 256);
            const double want = 20.0 * std::log10(0.5 + 0.5 * std::pow(10.0, 6.0 / 20.0));
            P.near("levels.mix50_db", gainDb(out.l, in.l, from, count), want, 0.05);
        }

        // DRY
        {
            const probe::Stereo out = run([](probe::Rig& r) {
                setPlain(r, "input", 12.0f);
                setPlain(r, "mix", 0.0f);
            }, 0, sizes);
            P.eq("dry.mix0_same_bits", probe::sameBits(out.l, in.l) && probe::sameBits(out.r, in.r) ? 1 : 0, 1);
        }

        // GLIDE: OUTPUT from 0 to -24 dB at the block that starts at `at`, on the sine alone (noise would hide a click)
        {
            const std::size_t at = 256 * 40;
            probe::Stereo sine(n);
            sine.l = sine.r = tone;
            const auto steady = [&](float db) {
                probe::Rig rig(kFs, 512);
                rig.load(init.source);
                setPlain(rig, "output", db);
                return rig.run(sine, 256);
            };
            probe::Rig rig(kFs, 512);
            rig.load(init.source);
            const int slot = slotOf(rig, "output");
            const auto turn = [&](std::size_t pos, std::array<float, pg::kMaxControls>& norm) {
                if (pos >= at && slot >= 0)
                    norm[static_cast<std::size_t>(slot)] = pg::normOf(rig.controls()[static_cast<std::size_t>(slot)], -24.0f);
            };
            const probe::Stereo turned = rig.run(sine, 256, {}, {}, turn);
            const probe::Stereo before = steady(0.0f), after = steady(-24.0f);
            const probe::EdgeSteps e = probe::edgeSteps(turned.l, before.l, after.l, at, static_cast<std::size_t>(0.020 * kFs), 48);
            std::printf("OUTPUT turned 0 to -24 dB: the largest step %.3f of the steady runs' (a hard switch %.3f)\n",
                        e.stepOverOwn, e.abruptOverOwn);
            P.le("glide.output_no_click", e.stepOverOwn, 1.05);
            P.ge("glide.a_switch_would_click", e.abruptOverOwn, 2.0);
            P.eq("glide.settles_at_minus24",
                 probe::sameBits(std::vector<float>(turned.l.begin() + static_cast<std::ptrdiff_t>(at + 2048), turned.l.end()),
                                 std::vector<float>(after.l.begin() + static_cast<std::ptrdiff_t>(at + 2048), after.l.end())) ? 1 : 0, 1);
        }
    }
    else
    {
        const auto defaults = [](probe::Rig&) {};
        {
            probe::Rig rig(kFs, 512);
            rig.load(init.source);
            for (const char* name : { "wave", "cutoff", "resonance", "amount", "attack", "decay", "sustain", "release", "volume" })
                P.eq(std::string("dials.") + name, slotOf(rig, name) >= 0 ? 1 : 0, 1);
            const int wave = slotOf(rig, "wave");
            P.eq("dials.wave_saw_default", wave >= 0 && rig.controls()[static_cast<std::size_t>(wave)].def == 0.0f ? 1 : 0, 1);
            const int res = slotOf(rig, "resonance"), amount = slotOf(rig, "amount");
            P.eq("dials.modulation_off", res >= 0 && amount >= 0 && rig.controls()[static_cast<std::size_t>(res)].def == 0.0f
                                             && rig.controls()[static_cast<std::size_t>(amount)].def == 0.0f ? 1 : 0, 1);
        }

        // PITCH
        {
            const Played c4 = play(60, 1.0f, 1.0, 0, defaults);
            const double hz = probe::frequencyHz(c4.out, kFs, static_cast<std::size_t>(0.1 * kFs), static_cast<std::size_t>(0.8 * kFs));
            const double want = 440.0 * std::pow(2.0, (60 - 69) / 12.0);
            std::printf("C4: %.4f Hz (%.3f cents)\n", hz, cents(hz, want));
            P.near("pitch.c4_cents", cents(hz, want), 0.0, 1.0);
            const Played bent = play(60, 1.0f, 1.0, 0, defaults, 1.0f);
            const double up = probe::frequencyHz(bent.out, kFs, static_cast<std::size_t>(0.1 * kFs), static_cast<std::size_t>(0.8 * kFs));
            std::printf("C4 bent up: %.4f Hz (%.3f cents off 2 semitones)\n", up, cents(up, want * std::pow(2.0, 2.0 / 12.0)));
            P.near("pitch.wheel_two_semitones_cents", cents(up, want * std::pow(2.0, 2.0 / 12.0)), 0.0, 1.0);
        }

        // ADSR: C4 (a period of 183.5 samples), held for 0.6 s, then released
        {
            const std::size_t off = static_cast<std::size_t>(0.6 * kFs);
            const std::size_t period = 184;
            const Played held = play(60, 1.0f, 1.5, off, defaults);
            const double full = probe::rmsDb(held.out, static_cast<std::size_t>(0.3 * kFs), static_cast<std::size_t>(0.25 * kFs));
            const double early = probe::rmsDb(held.out, static_cast<std::size_t>(0.005 * kFs), 6 * period);
            const double late = probe::rmsDb(held.out, off - 6 * period, 6 * period);
            std::printf("held: %.2f dB RMS; from 5 ms %.3f dB, before the release %.3f dB; the first sample %.6f\n", full,
                        early - full, late - full, static_cast<double>(held.out[0]));
            P.near("adsr.full_from_5ms_db", early - full, 0.0, 0.1);
            P.near("adsr.steady_to_release_db", late - full, 0.0, 0.1);
            P.le("adsr.first_sample", std::fabs(static_cast<double>(held.out[0])), 0.001);
            P.le("adsr.first_ms_rises", peakOver(held.out, 0, 12), 0.5 * peakOver(held.out, off - 2 * period, 2 * period));

            // The release: the peak of each period after the key's release, against the held peak.
            const double top = peakOver(held.out, off - 4 * period, 4 * period);
            double fell = -1.0;
            for (std::size_t at = off; at + period < held.out.size(); at += period)
                if (peakOver(held.out, at, period) <= top * 0.001)
                {
                    fell = 1000.0 * static_cast<double>(at - off) / kFs;
                    break;
                }
            std::size_t last = 0;
            for (std::size_t i = 0; i < held.out.size(); ++i)
                if (held.out[i] != 0.0f)
                    last = i;
            std::printf("released: -60 dB after %.1f ms; the last sample %zu (%.1f ms after the release), %.2e\n", fell,
                        last, 1000.0 * static_cast<double>(last - off) / kFs, static_cast<double>(held.out[last]));
            P.near("adsr.release_to_minus60_ms", fell, 100.0, 10.0);
            P.eq("adsr.voice_ends", held.voicesAfter, 0);
            P.le("adsr.last_sample_db", probe::dbFromLin(std::fabs(static_cast<double>(held.out[last])) / top), -70.0);
            P.le("adsr.silent_after_db", probe::rmsDb(held.out, held.out.size() - 4800, 4800), -200.0);
            P.eq("adsr.no_alerts", static_cast<int64_t>(held.alerts), 0);

            const Played half = play(60, 1.0f, 1.0, 0, [](probe::Rig& r) { setPlain(r, "sustain", 0.5f); });
            const double sustained = probe::rmsDb(half.out, static_cast<std::size_t>(0.6 * kFs), static_cast<std::size_t>(0.3 * kFs));
            P.near("adsr.sustain_half_db", sustained - full, -6.0206, 0.1);
            const Played slow = play(60, 1.0f, 1.0, 0, [](probe::Rig& r) { setPlain(r, "attack", 200.0f); });
            P.le("adsr.slow_attack_at_50ms_db", probe::rmsDb(slow.out, static_cast<std::size_t>(0.045 * kFs), 2 * period) - full, -6.0);
            P.near("adsr.slow_attack_full_db", probe::rmsDb(slow.out, static_cast<std::size_t>(0.3 * kFs), static_cast<std::size_t>(0.25 * kFs)) - full,
                   0.0, 0.1);
        }

        // VELOCITY and VOLUME
        {
            const std::size_t from = static_cast<std::size_t>(0.2 * kFs), count = static_cast<std::size_t>(0.5 * kFs);
            const Played loud = play(60, 1.0f, 1.0, 0, defaults);
            const Played soft = play(60, 0.5f, 1.0, 0, defaults);
            P.near("velocity.half_db", probe::rmsDb(soft.out, from, count) - probe::rmsDb(loud.out, from, count), -6.0206, 0.05);
            const Played up = play(60, 1.0f, 1.0, 0, [](probe::Rig& r) { setPlain(r, "volume", -6.0f); });
            P.near("volume.plus6_db", probe::rmsDb(up.out, from, count) - probe::rmsDb(loud.out, from, count), 6.0, 0.05);
        }

        // CUTOFF: A3, 220 Hz, over half a second (110 cycles)
        {
            const std::size_t from = static_cast<std::size_t>(0.25 * kFs), count = static_cast<std::size_t>(0.5 * kFs);
            const Played open = play(57, 1.0f, 1.0, 0, defaults);
            const Played dark = play(57, 1.0f, 1.0, 0, [](probe::Rig& r) { setPlain(r, "cutoff", 6.0f); });
            const double a = centroidHz(open.out, 220.0, from, count), b = centroidHz(dark.out, 220.0, from, count);
            std::printf("A3's centroid: %.0f Hz wide open, %.0f Hz at a cutoff of 6\n", a, b);
            P.ge("cutoff.open_centroid_hz", a, 2000.0);
            P.le("cutoff.darker_centroid", b / std::max(a, 1.0), 0.5);
        }
    }
    return P.finish();
}
