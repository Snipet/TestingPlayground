// PROBE name=dsp.metrics timeout=240
//
// dsp.metrics: the metrics' definitions (Measure.h), each measured over the whole held part and the same at every rate,
// on programs whose answers are known; each product's kind.
//   RATES    the same program at 48 and at 192 kHz: an instrument's (a voice of eight harmonics; Init, whose polyBLEP
//            saw is a little darker at 48 kHz, within 10 %) centroid_hz within 5 % and pitch_cents within 2 cents; an
//            effect's (two one-pole low-passes at 1.5 kHz) centroid_out_hz within 5 %; white noise's centroid (an effect's centroid_in_hz, an instrument's noise voice's centroid_hz)
//            within 10 % of a flat spectrum's power centroid over 20 Hz to 20 kHz (10,010 Hz), at both rates
//   PHRASE   an instrument's two-note phrase, a dark note (a sine) then a bright one (sixteen harmonics): its centroid
//            between theirs, each played alone, and more than 10 % from either; its RMS over the notes' held parts, the
//            gap between them left out (the phrase with a rest reads as without it)
//   PITCH    a note with a partial an octave below at -6 dB (a sub-order) reads the note's pitch (pitch_cents within a
//            cent, the partial its own); one without its fundamental (harmonics 2 to 6) reads it from its harmonics'
//            sum within 2 cents; a chord of a fifth reads its lowest note; low notes close together (a chord of C3 and
//            D3, a bass line E2, F#2, G2, A2) and a sine at C2 and at E2 read their lowest note's own partial; the
//            report names each one's pitch (its pitch follows the note: its lowest note and a whole tone up, each
//            alone, a whole tone apart)
//   STRETCH  the expected pitch stretch by stretch (stretches()), at 48 and 192 kHz: a C3 held 1 s, the pitch wheel
//            half way up at 0.3 s, reads the bent pitch (its longest stretch) and 0 cents, the report naming both
//            stretches; down at 0.6 s the unbent pitch; up from the start by a scenario's wheel 0 cents; the wheel
//            jittering by 0.08 cents every 20 ms one stretch, 0 cents; gliding 4 cents every 20 ms no pitch, the wheel
//            named, not the note's length; a phrase whose notes are each off by their own (0, 40, 70, 120 cents) the
//            farthest (+120), its lowest note's own pitch, the spread and the mean (57.5) named, in tune 0, and notes off
//            either way (-30, +30, -30, +30) 30 cents off, not their mean; a window over a phrase (C4, then E4) from E4's
//            note-on E4's pitch at 0 cents (its first 50 ms E4's, not C4's), over both notes 0; sixteenth notes of C3
//            and D3 0; E1 held 0.1 s no pitch (a note of 0.15 s, it says), held 0.2 s its pitch but no inharmonic_db (a
//            note of 0.34 s), held 0.4 s both; a chord of a semitone no pitch ("too near"); a noise hit of 0.05 s,
//            too short for the pitch's frames, "Unpitched" and no Inharmonic line
//   INHARMONIC a 0.3 s attack (linear, exponential) widens the harmonics' lobes but makes no peak: inharmonic_db under
//            -90; a partial of its own at -60 dB under the same attack is found within a dB, at 48 and 192 kHz
//   CLICKS   measure's click metric and trace's click finding agree: a sound with a step every 40 ms (an engine's
//            firing, longer than 1.5 cycles of its note) has none by either, a sine that jumps once has one by both; an
//            effect's: its report marks no step a click for the engine's, one for the jump, as trace finds, and none
//            for an effect that passes its input (the test impulse and its echoes are the response, not clicks), nor
//            trace with the impulse as its input (find click and any), which says why
//   HELD     heldSpans: a note number above 127 (a malformed message, which the Engine ignores) is no held part;
//            stretches(): two notes and a bend cut where the lowest key or the wheel changes, each with the key above;
//            a window's parts no held span covers the key sounding there, else the key released last, else (before
//            any note-on) the one given; the wheel's moves within a cent of each other one stretch, bent by their mean
//   WINDOW   a request's window, at 48 and 192 kHz: an instrument's voice that sounds on after a short note (a sine for
//            its first second, sixteen harmonics after) measured over 1.5 to 2.5 s reads the bright part (its centroid
//            within 5 % of the harmonics' power centroid, its RMS and its pitch), where the held part reads the sine;
//            its render lasts until the window's end; a window inside a held note keeps attack_s, release_s,
//            voice_end_s and step as they are without it, and one past the render's own length (1.5 to 3.5 s, the voice
//            ending at 3 s) keeps every whole-render metric (attack_s, release_s, voice_end_s, decay_s, step, click,
//            alerts) and the step line as they are without it; a window between two calls' starts (blocks of 2048)
//            times the call that renders it; an effect's fader (0 dB, then -6 dB from 0.5 s) reads -6.0 dB
//            on the sine and the noise over 0.6 to 0.95 s, 0 dB over 0.1 to 0.4 s, its impulse as without a window; one
//            that starts after hold_seconds, or holds under 0.01 s before it, leaves the sine's and the noise's metrics
//            unmeasured, saying why, and a check on it says so; checks alike but for their windows measured apart;
//            view_sound's RMS and spectrum over the window, its close-up inside it; a window shorter than its
//            spectrum's frame, just before the sound changes (the voice's harmonics at 1 s, the fader's -6 dB at 0.5 s),
//            reads nothing after it, at 48 and 192 kHz
//   BANDS    a report's band levels at 48 and 192 kHz, each line naming its frames and the band below which a tone
//            spreads: an instrument's sine at note 59 its RMS in its third, octave and semitone, at E1 (41.2 Hz) its RMS
//            in its semitone held 0.95 s and 2 s, a phrase's notes held 0.2 and 0.55 s in frames as long as each
//            note's part (the line saying so); a tone at the centre of each band from sharpFrom up its level within
//            0.3 dB, the bands beside it 13 dB under, over 0.1, 0.95 and 3 s, sharpFrom where the docs say; eight
//            harmonics' thirds their RMS, white noise's thirds 3 dB an octave; an effect's fader every band of the noise
//            -6.02 dB over a window shorter than a frame, a gain of a half every band of its own input's replay, with a
//            window and without, two low-passes the same at both rates up to 2 kHz
//   LOUDNESS lufs and rms_a_db at 48 and 192 kHz: an instrument's sine at 997 Hz of amplitude 0.1 reads -20.0 LUFS
//            (BS.1770's reference), at 100 Hz and 10 kHz the K-weighting's 48 kHz response against it, A-weighted at
//            31.5 Hz to 10 kHz IEC 61672's A within 0.1 dB; a part 40 dB under the rest left out by the relative gate,
//            one under -70 LUFS none ("under -70 LUFS"); an effect's noise_lufs the test noise's own when passed, a gain
//            of a half 6.02 dB under it
//   ORDERS   off_order_db at 48 and 192 kHz: eight harmonics nothing off their orders; a line of its own at 470 Hz its
//            share of the power, named; at 220 Hz a line at 330 Hz among the half-orders, on the orders of base_hz 110;
//            a base under twice the zone (base_hz 10; a phrase of sixteenth notes from C3, each held 0.075 s; the
//            effect's hold_seconds 0.1) not measured, saying why (and the phrase's wobble_db: no note held long enough
//            for a frame); an effect's sine passed nothing, a cubic shaper distorting but on the orders, a line of its
//            own its share
//   WOBBLE   wobble_db at 48 and 192 kHz: a steady sound and a tremolo about 0, harmonics a comb sweeps over at least
//            1 dB, white noise under 1 dB; an effect's steady tone through a pass, a fixed comb, a 20 ms echo, an
//            allpass, a 37 ms feedback comb and a tremolo about 0 (the echo and the allpass under a tenth of the
//            flanger), a flanger at least 0.7 dB; the same at both rates; a held part under 0.2 s, and an effect's
//            silent output, not measured, saying why
//   LEVEL    level_wobble_db at 48 and 192 kHz: a steady sound (an effect's steady tone passed) under 0.05 dB, a 3 Hz
//            tremolo (the voice's level 0.5 to 1.5 times; the effect's gain 0.5 to 1) at least 1 dB, a slow fade (6 dB a
//            second, a straight line in dB) under 0.05 dB, its trend taking it; its line in the report; the tremolo's
//            within 5 % at both rates; a held part under 0.2 s not measured, saying why
//   LINE     the report's last line every metric measured, name=value, in the table's order
//   TERMS    every metric's words name what metricTerms() defines or a span of its own; toleranceText is Goals'
//            toleranceOf
#include "Probe.h"

#include "dsp/Debug.h"
#include "dsp/Goals.h"
#include "dsp/Measure.h"
#include "dsp/SoundView.h"
#include "fminus/LibraryIncludes.h"

#include "Examples.h"
#include "Product.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace pg = playground;
namespace ms = playground::measure;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;
    constexpr double kFlatCentroid = (20.0 + 20000.0) / 2.0;   // a flat spectrum's power centroid over the band

    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    double at(const ms::Metrics& m, const char* k)
    {
        const auto it = m.find(k);
        return it == m.end() ? std::nan("") : it->second;
    }

    [[maybe_unused]] double noteHz(int n) { return 440.0 * std::exp2((n - 69) / 12.0); }

    // An instrument of eight harmonics, each 1/h: the same partials at every rate.
    [[maybe_unused]] const char* kHarmonicVoice = R"(module Voice
{
    float rate, ph, step, env;
    bool held;
    void init(float r) { rate = r; }
    void start(float note, float v) { step = 440 * expf(0.0577622650f * (note - 69)) / rate; held = true; env = 0; ph = 0; }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        const float up = 1 / (0.005f * rate);
        const float fall = expf(-6.90775528f / (0.2f * rate));
        for (int i = 0; i < n; ++i)
        {
            if (held) env = fminf(1, env + up); else env *= fall;
            float x = 0;
            for (int h = 1; h <= 8; ++h)
                x += sinf(6.28318531f * h * ph) / h;
            ph += step;
            if (ph >= 1) ph -= 1;
            left[i] += 0.1f * env * x;
            right[i] += 0.1f * env * x;
        }
        return held || env > 0.00001f;
    }
};
)";

    // White noise (the library's), its level held.
    [[maybe_unused]] const char* kNoiseVoice = R"(#include "noise.fm"
module Voice
{
    Noise noise;
    bool held;
    float env, fall;
    void init(float r) { fall = expf(-6.90775528f / (0.05f * r)); }
    void start(float note, float v) { held = true; env = 1; }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            if (!held) env *= fall;
            const float x = 0.2f * env * noise.white();
            left[i] += x;
            right[i] += x;
        }
        return held || env > 0.00001f;
    }
};
)";

    // A sine below note 60, sixteen harmonics (1/h) from it up: a dark note and a bright one of the same RMS, about.
    [[maybe_unused]] const char* kTwoToneVoice = R"(module Voice
{
    float rate, ph, step, env;
    int bright;
    bool held;
    void init(float r) { rate = r; }
    void start(float note, float v)
    {
        step = 440 * expf(0.0577622650f * (note - 69)) / rate; held = true; env = 0; ph = 0;
        bright = note >= 60 ? 16 : 1;
    }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        const float up = 1 / (0.005f * rate);
        const float fall = expf(-6.90775528f / (0.02f * rate));
        const float g = bright > 1 ? 0.1f : 0.16f;
        for (int i = 0; i < n; ++i)
        {
            if (held) env = fminf(1, env + up); else env *= fall;
            float x = 0;
            for (int h = 1; h <= bright; ++h)
                x += sinf(6.28318531f * h * ph) / h;
            ph += step;
            if (ph >= 1) ph -= 1;
            left[i] += g * env * x;
            right[i] += g * env * x;
        }
        return held || env > 0.00001f;
    }
};
)";

    // A note's partials: `first` to `last` (1 the fundamental), each 1/h, and a sub-order (half the note) at `sub`.
    [[maybe_unused]] std::string partialsVoice(int first, int last, float sub)
    {
        return std::string(R"(module Voice
{
    float rate, ph, step, env;
    bool held;
    void init(float r) { rate = r; }
    void start(float note, float v) { step = 440 * expf(0.0577622650f * (note - 69)) / rate; held = true; env = 0; ph = 0; }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        const float up = 1 / (0.005f * rate);
        const float fall = expf(-6.90775528f / (0.05f * rate));
        for (int i = 0; i < n; ++i)
        {
            if (held) env = fminf(1, env + up); else env *= fall;
            float x = )") + std::to_string(sub) + R"(f * sinf(6.28318531f * ph);
            for (int h = )" + std::to_string(first) + "; h <= " + std::to_string(last) + R"(; ++h)
                x += sinf(12.5663706f * h * ph) / h;
            ph += 0.5f * step;
            if (ph >= 1) ph -= 1;
            left[i] += 0.1f * env * x;
            right[i] += 0.1f * env * x;
        }
        return held || env > 0.00001f;
    }
};
)";
    }

    // Eight harmonics (1/h) on the pitch wheel (2 semitones), tuned `stretch` cents a semitone sharp above note 48 (a
    // phrase's notes each off by their own) and `apart` cents flat on even notes, sharp on odd ones, under an attack of
    // `attack` seconds (`shape` 0: linear; 1: exponential, 60 dB in it), with a partial of its own at 1.37 times the
    // note, `odd` dB under the fundamental (-200: none).
    [[maybe_unused]] const char* kWheelVoice = R"(#include "host.fm"
module Voice
{
    param float stretch [[fminus::range(-50, 50)]] [[fminus::default(0)]];
    param float apart [[fminus::range(-50, 50)]] [[fminus::default(0)]];
    param float attack [[fminus::range(0.001, 4)]] [[fminus::default(0.005)]];
    param int shape [[fminus::range(0, 1)]] [[fminus::default(0)]];
    param float odd [[fminus::range(-200, 0)]] [[fminus::default(-200)]];
    Host* host;
    float rate, ph, po, step, env, t;
    bool held;
    void init(float r) { rate = r; }
    void start(float note, float v)
    {
        step = 440 * expf(0.0577622650f * (note - 69 + 0.01f * stretch[0] * fmaxf(0, note - 48)
                                           - 0.01f * apart[0] * cosf(3.14159265f * note))) / rate;
        held = true; env = 0; t = 0; ph = 0; po = 0;
    }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        const float fall = expf(-6.90775528f / (0.05f * rate));
        const float f = step * expf(0.0577622650f * 2 * host->pitchbend);
        const float og = expf(0.115129255f * odd[0]);
        const int sh = shape[0];
        for (int i = 0; i < n; ++i)
        {
            t += 1 / rate;
            const float a = attack[i];
            if (held) env = sh == 0 ? fminf(1, t / a) : (t >= a ? 1 : expf(6.90775528f * (t / a - 1)));
            else env *= fall;
            float x = og * sinf(6.28318531f * po);
            for (int h = 1; h <= 8; ++h)
                x += sinf(6.28318531f * h * ph) / h;
            ph += f;
            if (ph >= 1) ph -= 1;
            po += 1.37f * f;
            if (po >= 1) po -= 1;
            left[i] += 0.1f * env * x;
            right[i] += 0.1f * env * x;
        }
        return held || env > 0.00001f;
    }
};
)";

    // Whether the report's step line marks one of a signal's steps a click ("0.310 in the sine at 0.50 s, ... (a
    // click)").
    [[maybe_unused]] bool markedIn(const std::string& report, const std::string& signal)
    {
        const std::size_t from = report.find("Largest step");
        if (from == std::string::npos)
            return false;
        const std::string line = report.substr(from, report.find('\n', from) - from);
        for (std::size_t a = 0; a < line.size();)
        {
            std::size_t b = line.find("; ", a);
            if (b == std::string::npos)
                b = line.size();
            const std::string entry = line.substr(a, b - a);
            if (has(entry, " in " + signal + " at ") && has(entry, "(a click)"))
                return true;
            a = b + 2;
        }
        return false;
    }

    // A sine with a firing every 40 ms (a step of 0.25 that decays over 4 ms: an engine's), or with one jump of half a
    // cycle at 0.5 s.
    [[maybe_unused]] std::string clickVoice(bool engine)
    {
        return std::string(R"(module Voice
{
    float rate, ph, step, env, fire;
    int count, period, done;
    bool held;
    void init(float r) { rate = r; period = (int)(0.04f * r); }
    void start(float note, float v) { step = 440 * expf(0.0577622650f * (note - 69)) / rate; held = true; env = 0; ph = 0; }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        const float up = 1 / (0.005f * rate);
        const float fall = expf(-6.90775528f / (0.05f * rate));
        const float decay = expf(-1 / (0.004f * rate));
        for (int i = 0; i < n; ++i)
        {
            if (held) env = fminf(1, env + up); else env *= fall;
            count += 1;
)") + (engine ? R"(            if (count >= period) { count = 0; fire = 0.25f; }
)"
                  : R"(            if (count == (int)(0.5f * rate)) ph += 0.5f;
)")
             + R"(            const float out = env * (0.1f * sinf(6.28318531f * ph) + fire);
            fire *= decay;
            ph += step;
            if (ph >= 1) ph -= 1;
            left[i] += out;
            right[i] += out;
        }
        return held || env > 0.00001f;
    }
};
)";
    }

    // Two one-pole low-passes at 1.5 kHz: a darker noise out, the same at every rate.
    [[maybe_unused]] const char* kDarkEffect = R"(module Effect
{
    float a, l1, l2, r1, r2;
    void init(float r) { a = 1 - expf(-6.28318531f * 1500 / r); }
    void process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            l1 += a * (left[i] - l1);
            l2 += a * (l1 - l2);
            r1 += a * (right[i] - r1);
            r2 += a * (r1 - r2);
            left[i] = l2;
            right[i] = r2;
        }
    }
};
)";

    // The input with a firing every 40 ms added (a step of 0.25 decaying over 4 ms), or with 0.3 added from 0.5 s on.
    [[maybe_unused]] std::string clickEffect(bool engine)
    {
        return std::string(R"(module Effect
{
    float rate, fire;
    int count, period;
    void init(float r) { rate = r; period = (int)(0.04f * r); }
    void process(float* restrict left, float* restrict right, int n)
    {
        const float decay = expf(-1 / (0.004f * rate));
        for (int i = 0; i < n; ++i)
        {
            count += 1;
)") + (engine ? R"(            if (count >= period) { count = 0; fire = 0.25f; }
            const float add = fire;
            fire *= decay;
)"
                  : R"(            const float add = count >= (int)(0.5f * rate) ? 0.3f : 0.0f;
)")
             + R"(            left[i] += add;
            right[i] += add;
        }
    }
};
)";
    }

    // An effect that passes its input as it is.
    [[maybe_unused]] const char* kPassEffect = R"(module Effect
{
    void init(float r) {}
    void process(float* restrict left, float* restrict right, int n) {}
};
)";

    // A voice that sounds for three seconds whatever its note-off: a sine for its first second, then sixteen harmonics
    // (1/h) at about the same level (a sound a short key starts, as an engine's).
    [[maybe_unused]] const char* kOnAfterVoice = R"(module Voice
{
    float rate, ph, step;
    int at;
    void init(float r) { rate = r; }
    void start(float note, float v) { step = 440 * expf(0.0577622650f * (note - 69)) / rate; ph = 0; at = 0; }
    void stop() {}
    bool process(float* restrict left, float* restrict right, int n)
    {
        const int second = (int)rate;
        for (int i = 0; i < n; ++i)
        {
            const int top = at < second ? 1 : 16;
            float x = 0;
            for (int h = 1; h <= top; ++h)
                x += sinf(6.28318531f * h * ph) / h;
            ph += step;
            if (ph >= 1) ph -= 1;
            at += 1;
            const float g = top > 1 ? 0.1f : 0.16f;
            left[i] += g * x;
            right[i] += g * x;
        }
        return at < 3 * second;
    }
};
)";

    // A fader: the input as it is for the first 0.5 s, then at half (-6 dB).
    [[maybe_unused]] const char* kFaderEffect = R"(module Effect
{
    float rate;
    int at;
    void init(float r) { rate = r; at = 0; }
    void process(float* restrict left, float* restrict right, int n)
    {
        const int half = (int)(0.5f * rate);
        for (int i = 0; i < n; ++i)
        {
            const float g = at < half ? 1.0f : 0.5f;
            at += 1;
            left[i] *= g;
            right[i] *= g;
        }
    }
};
)";

    // A gain of a half (-6.02 dB), from the first sample: each sample halved exactly.
    [[maybe_unused]] const char* kHalfEffect = R"(module Effect
{
    void process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            left[i] *= 0.5f;
            right[i] *= 0.5f;
        }
    }
};
)";

    // A number as an FMinus float literal.
    [[maybe_unused]] std::string lit(double v)
    {
        char buf[48];
        std::snprintf(buf, sizeof buf, "%.9g", v);
        std::string s = buf;
        if (s.find('.') == std::string::npos && s.find('e') == std::string::npos)
            s += ".0";
        return s + "f";
    }

    // A voice of one sine at `hz` (whatever the note) of amplitude `amp` in both channels, its level times `gain` from
    // `stepAt` seconds on (none when negative).
    [[maybe_unused]] std::string toneVoice(double hz, double amp, double stepAt = -1.0, double gain = 1.0)
    {
        return R"(module Voice
{
    float rate, ph, env;
    int at;
    bool held;
    void init(float r) { rate = r; }
    void start(float note, float v) { held = true; env = 0; ph = 0; at = 0; }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        const float up = 1 / (0.005f * rate);
        const float fall = expf(-6.90775528f / (0.05f * rate));
        const int step = (int)()" + lit(stepAt) + R"( * rate);
        for (int i = 0; i < n; ++i)
        {
            if (held) env = fminf(1, env + up); else env *= fall;
            const float g = step >= 0 && at >= step ? )" + lit(gain) + R"( : 1.0f;
            at += 1;
            const float x = )" + lit(amp) + R"( * g * env * sinf(6.28318531f * ph);
            ph += )" + lit(hz) + R"( / rate;
            if (ph >= 1) ph -= 1;
            left[i] += x;
            right[i] += x;
        }
        return held || env > 0.00001f;
    }
};
)";
    }

    // kHarmonicVoice's eight harmonics of the note (0.1 / h), and a line of its own at `lineHz` whatever the note (an
    // exhaust's self-oscillation), of amplitude `lineAmp`.
    [[maybe_unused]] std::string ordersVoice(double lineHz, double lineAmp)
    {
        return R"(module Voice
{
    float rate, ph, step, lph, env;
    bool held;
    void init(float r) { rate = r; }
    void start(float note, float v) { step = 440 * expf(0.0577622650f * (note - 69)) / rate; held = true; env = 0; ph = 0; lph = 0; }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        const float up = 1 / (0.005f * rate);
        const float fall = expf(-6.90775528f / (0.2f * rate));
        for (int i = 0; i < n; ++i)
        {
            if (held) env = fminf(1, env + up); else env *= fall;
            float x = 0;
            for (int h = 1; h <= 8; ++h)
                x += 0.1f * sinf(6.28318531f * h * ph) / h;
            x += )" + lit(lineAmp) + R"( * sinf(6.28318531f * lph);
            ph += step;
            if (ph >= 1) ph -= 1;
            lph += )" + lit(lineHz) + R"( / rate;
            if (lph >= 1) lph -= 1;
            left[i] += env * x;
            right[i] += env * x;
        }
        return held || env > 0.00001f;
    }
};
)";
    }

    // kHarmonicVoice's eight harmonics, all of them times 1 + tremolo sin(2 pi 3 Hz t), harmonic h also times
    // 1 + comb sin(2 pi 1 Hz t + h): a level that moves (the shape the same), and a shape that moves (a comb's notches
    // sweeping over them once a second).
    [[maybe_unused]] std::string movingVoice(double tremolo, double comb)
    {
        return R"(module Voice
{
    float rate, ph, step, t, env;
    bool held;
    void init(float r) { rate = r; }
    void start(float note, float v) { step = 440 * expf(0.0577622650f * (note - 69)) / rate; held = true; env = 0; ph = 0; t = 0; }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        const float up = 1 / (0.005f * rate);
        const float fall = expf(-6.90775528f / (0.2f * rate));
        for (int i = 0; i < n; ++i)
        {
            if (held) env = fminf(1, env + up); else env *= fall;
            float x = 0;
            for (int h = 1; h <= 8; ++h)
                x += (1 + )" + lit(comb) + R"( * sinf(6.28318531f * t + h)) * sinf(6.28318531f * h * ph) / h;
            x *= 0.1f * (1 + )" + lit(tremolo) + R"( * sinf(18.8495559f * t));
            ph += step;
            if (ph >= 1) ph -= 1;
            t += 1 / rate;
            left[i] += env * x;
            right[i] += env * x;
        }
        return held || env > 0.00001f;
    }
};
)";
    }

    // The input with a sine of its own added at `hz` of amplitude `amp` (a line that sounds whatever comes in), then
    // shaped: x + cubic x^3.
    [[maybe_unused]] std::string addEffect(double hz, double amp, double cubic)
    {
        return R"(module Effect
{
    float rate, ph;
    void init(float r) { rate = r; }
    void process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const float add = )" + lit(amp) + R"( * sinf(6.28318531f * ph);
            ph += )" + lit(hz) + R"( / rate;
            if (ph >= 1) ph -= 1;
            const float l = left[i] + add;
            const float r = right[i] + add;
            left[i] = l + )" + lit(cubic) + R"( * l * l * l;
            right[i] = r + )" + lit(cubic) + R"( * r * r * r;
        }
    }
};
)";
    }

    // A comb: half the input and half of it delayed by 1 ms + depth ms x sin(2 pi 0.5 Hz t), read between samples in a
    // straight line: a flanger, or with depth 0 a fixed comb.
    [[maybe_unused]] std::string combEffect(double depthMs)
    {
        return R"(module Effect
{
    float rate, t;
    float bl[4096];
    float br[4096];
    int w;
    void init(float r) { rate = r; }
    void process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            bl[w] = left[i];
            br[w] = right[i];
            const float d = (0.001f + )" + lit(depthMs / 1000.0) + R"( * sinf(3.14159265f * t)) * rate;
            const int di = (int)d;
            const float fr = d - di;
            const int a = (w - di + 4096) % 4096;
            const int b = (w - di - 1 + 4096) % 4096;
            const float dl = bl[a] + fr * (bl[b] - bl[a]);
            const float dr = br[a] + fr * (br[b] - br[a]);
            left[i] = 0.5f * (left[i] + dl);
            right[i] = 0.5f * (right[i] + dr);
            w = (w + 1) % 4096;
            t += 1 / rate;
        }
    }
};
)";
    }

    // Effects that hold still but remember: an echo (the input and half of it 20 ms later) and a Schroeder allpass of
    // 5 ms (g 0.7: a flat gain, its phase all that changes). And one that moves its level alone: a tremolo of 3 Hz, its
    // gain 0.5 to 1.
    [[maybe_unused]] const char* kEchoEffect = R"(module Effect
{
    float rate;
    float bl[4096];
    float br[4096];
    int w;
    void init(float r) { rate = r; }
    void process(float* restrict left, float* restrict right, int n)
    {
        const int d = (int)(0.02f * rate);
        for (int i = 0; i < n; ++i)
        {
            bl[w] = left[i];
            br[w] = right[i];
            const int a = (w - d + 4096) % 4096;
            left[i] = left[i] + 0.5f * bl[a];
            right[i] = right[i] + 0.5f * br[a];
            w = (w + 1) % 4096;
        }
    }
};
)";

    [[maybe_unused]] const char* kAllpassEffect = R"(module Effect
{
    float rate;
    float xl[4096];
    float xr[4096];
    float yl[4096];
    float yr[4096];
    int w;
    void init(float r) { rate = r; }
    void process(float* restrict left, float* restrict right, int n)
    {
        const int d = (int)(0.005f * rate);
        for (int i = 0; i < n; ++i)
        {
            const int a = (w - d + 4096) % 4096;
            const float ol = -0.7f * left[i] + xl[a] + 0.7f * yl[a];
            const float orr = -0.7f * right[i] + xr[a] + 0.7f * yr[a];
            xl[w] = left[i];
            xr[w] = right[i];
            yl[w] = ol;
            yr[w] = orr;
            left[i] = ol;
            right[i] = orr;
            w = (w + 1) % 4096;
        }
    }
};
)";

    // A feedback comb of 37 ms (g 0.7: its tail falls 60 dB in about 0.7 s), a memory longer than wobble_db's frames.
    [[maybe_unused]] const char* kFeedbackEffect = R"(module Effect
{
    float rate;
    float yl[8192];
    float yr[8192];
    int w;
    void init(float r) { rate = r; }
    void process(float* restrict left, float* restrict right, int n)
    {
        const int d = (int)(0.037f * rate);
        for (int i = 0; i < n; ++i)
        {
            const int a = (w - d + 8192) % 8192;
            const float ol = left[i] + 0.7f * yl[a];
            const float orr = right[i] + 0.7f * yr[a];
            yl[w] = ol;
            yr[w] = orr;
            left[i] = 0.3f * ol;
            right[i] = 0.3f * orr;
            w = (w + 1) % 8192;
        }
    }
};
)";

    [[maybe_unused]] const char* kMuteEffect = R"(module Effect
{
    void init(float r) {}
    void process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            left[i] = 0;
            right[i] = 0;
        }
    }
};
)";

    [[maybe_unused]] const char* kTremoloEffect = R"(module Effect
{
    float rate, t;
    void init(float r) { rate = r; }
    void process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const float g = 0.75f - 0.25f * sinf(18.8495559f * t);
            left[i] = g * left[i];
            right[i] = g * right[i];
            t += 1 / rate;
        }
    }
};
)";

    // kHarmonicVoice's eight harmonics falling `db` dB a second while held: a slow fade, a straight line in dB.
    [[maybe_unused]] std::string fadingVoice(double db)
    {
        return R"(module Voice
{
    float rate, ph, step, t, env;
    bool held;
    void init(float r) { rate = r; }
    void start(float note, float v) { step = 440 * expf(0.0577622650f * (note - 69)) / rate; held = true; env = 0; ph = 0; t = 0; }
    void stop() { held = false; }
    bool process(float* restrict left, float* restrict right, int n)
    {
        const float up = 1 / (0.005f * rate);
        const float fall = expf(-6.90775528f / (0.2f * rate));
        for (int i = 0; i < n; ++i)
        {
            if (held) env = fminf(1, env + up); else env *= fall;
            float x = 0;
            for (int h = 1; h <= 8; ++h)
                x += sinf(6.28318531f * h * ph) / h;
            x *= 0.1f * expf(-)" + lit(db * 0.115129255) + R"( * t);
            ph += step;
            if (ph >= 1) ph -= 1;
            t += 1 / rate;
            left[i] += env * x;
            right[i] += env * x;
        }
        return held || env > 0.00001f;
    }
};
)";
    }

    // An effect whose gain falls `db` dB a second: a slow fade of whatever comes in.
    [[maybe_unused]] std::string fadingEffect(double db)
    {
        return R"(module Effect
{
    float rate, t;
    void init(float r) { rate = r; }
    void process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const float g = expf(-)" + lit(db * 0.115129255) + R"( * t);
            left[i] = g * left[i];
            right[i] = g * right[i];
            t += 1 / rate;
        }
    }
};
)";
    }

    ms::Result measured(const pg::Sources& files, double rate, ms::Request r)
    {
        r.rate = rate;
        ms::Session s(files, rate);
        if (!s.compiled())
            std::printf("does not compile:\n%s\n", s.errors().c_str());
        return s.measure(r);
    }
}

PROBE(dsp, metrics)
{
    // ---- RATES ---------------------------------------------------------------------------------------------------------
    if constexpr (kInstrument)
    {
        ms::Request r;
        r.notes = { 48 };
        r.holdSeconds = 1.0f;
        r.tailSeconds = 0.5f;
        r.want = { "centroid_hz", "fundamental_hz", "pitch_cents" };
        const pg::Sources init = pg::withLibraryIncludes(pg::singleFile(pg::kExamples[0].source));
        for (const auto& [name, files] : { std::pair<const char*, pg::Sources>{ "init", init },
                                           std::pair<const char*, pg::Sources>{ "harmonics", pg::singleFile(kHarmonicVoice) } })
        {
            const ms::Result a = measured(files, 48000.0, r), b = measured(files, 192000.0, r);
            const double ca = at(a.values, "centroid_hz"), cb = at(b.values, "centroid_hz");
            const double pa = at(a.values, "pitch_cents"), pb = at(b.values, "pitch_cents");
            std::printf("%s at C3: centroid %.1f Hz at 48 kHz, %.1f Hz at 192 kHz; pitch %+.3f and %+.3f cents\n", name, ca, cb, pa, pb);
            const std::string k = std::string("rates.") + name;
            // Init's polyBLEP saw is a little darker at 48 kHz (its top octave, a tenth of a saw's power centroid, falls
            // off near the Nyquist frequency): the harmonics' voice is the same sound at both rates.
            P.near(k + "_centroid", cb / ca, 1.0, std::string(name) == "init" ? 0.1 : 0.05);
            P.near(k + "_pitch_cents", pb - pa, 0.0, 2.0);
            P.near(k + "_in_tune", pa, 0.0, 2.0);
        }
        const pg::Sources noise = pg::withLibraryIncludes(pg::singleFile(kNoiseVoice));
        for (const double rate : { 48000.0, 192000.0 })
        {
            const double c = at(measured(noise, rate, r).values, "centroid_hz");
            std::printf("white noise at %.0f Hz: centroid %.0f Hz (a flat spectrum's %.0f Hz)\n", rate, c, kFlatCentroid);
            P.near("rates.noise_centroid_" + std::to_string(static_cast<int>(rate / 1000.0)) + "k", c / kFlatCentroid, 1.0, 0.1);
        }
    }
    else
    {
        ms::Request r;
        r.holdSeconds = 1.0f;
        r.tailSeconds = 0.2f;
        r.want = { "centroid_in_hz", "centroid_out_hz" };
        const pg::Sources dark = pg::singleFile(kDarkEffect);
        const ms::Result a = measured(dark, 48000.0, r), b = measured(dark, 192000.0, r);
        const double oa = at(a.values, "centroid_out_hz"), ob = at(b.values, "centroid_out_hz");
        std::printf("two low-passes at 1.5 kHz: centroid out %.1f Hz at 48 kHz, %.1f Hz at 192 kHz; in %.0f and %.0f Hz (a flat "
                    "spectrum's %.0f Hz)\n", oa, ob, at(a.values, "centroid_in_hz"), at(b.values, "centroid_in_hz"), kFlatCentroid);
        P.near("rates.dark_centroid_out", ob / oa, 1.0, 0.05);
        P.near("rates.noise_centroid_in_48k", at(a.values, "centroid_in_hz") / kFlatCentroid, 1.0, 0.1);
        P.near("rates.noise_centroid_in_192k", at(b.values, "centroid_in_hz") / kFlatCentroid, 1.0, 0.1);
        P.le("rates.dark_darker", oa, 0.5 * at(a.values, "centroid_in_hz"));
    }

    // ---- PHRASE --------------------------------------------------------------------------------------------------------
    if constexpr (kInstrument)
    {
        const pg::Sources two = pg::singleFile(kTwoToneVoice);
        const auto phrase = [&](std::vector<ms::PhraseNote> notes) {
            ms::Request r;
            r.phrase = std::move(notes);
            r.tailSeconds = 0.3f;
            return measured(two, 48000.0, r);
        };
        const ms::Result both = phrase({ { 48, 0.0, 0.5, 0.8f }, { 72, 0.5, 0.5, 0.8f } });
        const ms::Result dark = phrase({ { 48, 0.0, 0.5, 0.8f } }), bright = phrase({ { 72, 0.0, 0.5, 0.8f } });
        const double cb = at(both.values, "centroid_hz"), cd = at(dark.values, "centroid_hz"), cl = at(bright.values, "centroid_hz");
        std::printf("a dark note then a bright one: centroid %.0f Hz; the dark alone %.0f Hz, the bright alone %.0f Hz\n%s", cb, cd, cl,
                    both.report.c_str());
        P.eq("phrase.between", cd < cb && cb < cl ? 1 : 0, 1);
        P.ge("phrase.not_the_dark", cb / cd, 1.1);
        P.ge("phrase.not_the_bright", cl / cb, 1.1);
        // A rest between the notes is no held part: the RMS reads as without it.
        const ms::Result rest = phrase({ { 48, 0.0, 0.5, 0.8f }, { 72, 1.0, 0.5, 0.8f } });
        std::printf("with a rest of 0.5 s: RMS %.2f dBFS against %.2f dBFS; centroid %.0f Hz\n", at(rest.values, "rms_db"),
                    at(both.values, "rms_db"), at(rest.values, "centroid_hz"));
        P.near("phrase.rest_not_held_rms", at(rest.values, "rms_db"), at(both.values, "rms_db"), 0.2);
        P.near("phrase.rest_not_held_centroid", at(rest.values, "centroid_hz") / cb, 1.0, 0.02);
    }

    // ---- PITCH ---------------------------------------------------------------------------------------------------------
    if constexpr (kInstrument)
    {
        ms::Request r;
        r.notes = { 60 };
        // Partials 1 to 4 and one an octave below at -6 dB (half the fundamental's amplitude).
        const ms::Result sub = measured(pg::singleFile(partialsVoice(1, 4, 0.5f)), 48000.0, r);
        std::printf("a sub-order 6 dB down: %s", sub.report.c_str());
        P.near("pitch.sub_order_cents", at(sub.values, "pitch_cents"), 0.0, 1.0);
        P.near("pitch.sub_order_hz", at(sub.values, "fundamental_hz") / noteHz(60), 1.0, 0.001);
        P.eq("pitch.sub_order_partial", has(sub.report, "its harmonics' sum") ? 0 : 1, 1);
        // The report's judgement (the lowest note and a whole tone up, each alone, a whole tone apart): each of these
        // follows the note.
        const auto follows = [&](const std::string& k, const ms::Result& m) {
            P.eq("pitch." + k + "_follows", has(m.report, " Pitch: ") && !has(m.report, "Unpitched") ? 1 : 0, 1);
        };
        follows("sub_order", sub);
        // No fundamental: harmonics 2 to 6 alone.
        const ms::Result missing = measured(pg::singleFile(partialsVoice(2, 6, 0.0f)), 48000.0, r);
        std::printf("no fundamental: %s", missing.report.c_str());
        P.near("pitch.missing_fundamental_cents", at(missing.values, "pitch_cents"), 0.0, 2.0);
        P.eq("pitch.missing_fundamental_sum", has(missing.report, "its harmonics' sum") ? 1 : 0, 1);
        follows("missing_fundamental", missing);
        // A fifth: the lowest note's.
        ms::Request fifth = r;
        fifth.notes = { 60, 67 };
        const ms::Result f = measured(pg::singleFile(kHarmonicVoice), 48000.0, fifth);
        P.near("pitch.fifth_lowest", at(f.values, "pitch_cents"), 0.0, 2.0);
        follows("fifth", f);
        // Low notes close together: C3 and D3 are 16 Hz apart, a bass line's E2 and F#2 10 Hz; and a low sine, whose
        // range of 600 cents spans a few tens of Hz. Each reads its lowest note's own partial.
        const auto own = [&](const std::string& k, const ms::Result& m, double within) {
            std::printf("%s: pitch %+.2f cents%s\n", k.c_str(), at(m.values, "pitch_cents"),
                        has(m.report, "its harmonics' sum") ? " (its harmonics' sum)" : "");
            P.near("pitch." + k + "_cents", at(m.values, "pitch_cents"), 0.0, within);
            P.eq("pitch." + k + "_partial", has(m.report, "its harmonics' sum") ? 0 : 1, 1);
            follows(k, m);
        };
        ms::Request close = r;
        close.notes = { 48, 50 };
        own("low_chord", measured(pg::singleFile(kHarmonicVoice), 48000.0, close), 2.0);
        ms::Request line;
        line.phrase = { { 40, 0.0, 0.5, 0.8f }, { 42, 0.5, 0.5, 0.8f }, { 43, 1.0, 0.5, 0.8f }, { 45, 1.5, 0.5, 0.8f } };
        line.tailSeconds = 0.3f;
        own("bass_line", measured(pg::singleFile(kHarmonicVoice), 48000.0, line), 2.0);
        for (const int n : { 36, 40 })
        {
            ms::Request low = r;
            low.notes = { n };
            own("sine_" + std::to_string(n), measured(pg::singleFile(partialsVoice(1, 1, 0.0f)), 48000.0, low), 1.0);
        }
    }

    // ---- STRETCH -------------------------------------------------------------------------------------------------------
    if constexpr (kInstrument)
    {
        const pg::Sources wheel = pg::singleFile(kWheelVoice);
        const auto unmeasured = [](const ms::Result& m, const char* k) {
            const auto it = m.unmeasured.find(k);
            return it == m.unmeasured.end() ? std::string() : it->second;
        };
        for (const double rate : { 48000.0, 192000.0 })
        {
            const std::string tag = "_" + std::to_string(static_cast<int>(rate / 1000.0)) + "k";
            // The expected pitch follows the pitch wheel through the note: C3 held 1 s, the wheel up half way (a
            // semitone) at 0.3 s reads the bent pitch over its longest stretch, and each stretch its own; down at 0.6 s,
            // the unbent one; up from the start by a scenario's wheel, not its pitchbend, bent too.
            ms::Request note;
            note.notes = { 48 };
            note.holdSeconds = 1.0f;
            note.tailSeconds = 0.3f;
            const auto bent = [&](double at, float value) {
                ms::Request q = note;
                q.wheels = { { ms::Wheel::bend, at, value } };
                return measured(wheel, rate, q);
            };
            const ms::Result up = bent(0.3, 0.5f), down = bent(0.6, -1.0f), start = bent(0.0, 0.5f);
            std::printf("the wheel up at 0.3 s at %s: %s", tag.c_str(), up.report.c_str());
            P.near("stretch.wheel_up_hz" + tag, at(up.values, "fundamental_hz") / noteHz(49), 1.0, 0.0005);
            P.near("stretch.wheel_up_cents" + tag, at(up.values, "pitch_cents"), 0.0, 1.0);
            P.eq("stretch.wheel_up_says" + tag, has(up.report, "(note 48, bent, its longest stretch)") && has(up.report, "over 2 stretches")
                                                   ? 1 : 0, 1);
            P.near("stretch.wheel_down_hz" + tag, at(down.values, "fundamental_hz") / noteHz(48), 1.0, 0.0005);
            P.near("stretch.wheel_down_cents" + tag, at(down.values, "pitch_cents"), 0.0, 1.0);
            P.near("stretch.wheel_start_cents" + tag, at(start.values, "pitch_cents"), 0.0, 1.0);
            // The wheel's moves within a cent of each other are one stretch (a resting wheel's jitter, every 20 ms between
            // 0 and 0.08 cents: C3 read, 0 cents); a glide in steps of 4 cents every 20 ms leaves no stretch C3's frames
            // can read, and says the wheel moves too often, not that the note is short.
            ms::Request jitter = note, glide = note;
            for (int k = 1; k < 50; ++k)
            {
                jitter.wheels.push_back({ ms::Wheel::bend, 0.02 * k, k % 2 == 0 ? 0.0f : 0.0004f });
                glide.wheels.push_back({ ms::Wheel::bend, 0.02 * k, 0.02f * static_cast<float>(k) });
            }
            const ms::Result jittered = measured(wheel, rate, jitter), glided = measured(wheel, rate, glide);
            std::printf("the wheel gliding every 20 ms at %s: %s", tag.c_str(), glided.report.c_str());
            P.near("stretch.wheel_jitter_cents" + tag, at(jittered.values, "pitch_cents"), 0.0, 1.0);
            P.eq("stretch.wheel_jitter_one" + tag, has(jittered.report, "(note 48)") && !has(jittered.report, "Each stretch") ? 1 : 0, 1);
            P.eq("stretch.wheel_glide_says" + tag, std::isnan(at(glided.values, "pitch_cents"))
                                                      && has(unmeasured(glided, "pitch_cents"), "the pitch wheel moves too often: note 48 "
                                                                                                "stays at one place of it for 0.02 s at most")
                                                      && !has(unmeasured(glided, "pitch_cents"), "a note of") ? 1 : 0, 1);
            // A phrase's expected pitch is each note's: four notes each off by their own (0, 40, 70 and 120 cents) read
            // the farthest (+120: no note off one way hides another off the other), the lowest note's pitch its own, the
            // spread and the mean (57.5) named; in tune, 0.
            ms::Request phrase;
            phrase.phrase = { { 48, 0.0, 0.5, 0.8f }, { 52, 0.5, 0.5, 0.8f }, { 55, 1.0, 0.5, 0.8f }, { 60, 1.5, 0.5, 0.8f } };
            phrase.tailSeconds = 0.3f;
            ms::Request sharp = phrase;
            sharp.dials = { { "stretch", 10.0f } };
            const ms::Result tuned = measured(wheel, rate, phrase), off = measured(wheel, rate, sharp);
            std::printf("a phrase each note off by its own at %s: %s", tag.c_str(), off.report.c_str());
            P.near("stretch.phrase_each_cents" + tag, at(off.values, "pitch_cents"), 120.0, 1.0);
            P.near("stretch.phrase_lowest_hz" + tag, at(off.values, "fundamental_hz") / noteHz(48), 1.0, 0.0005);
            P.eq("stretch.phrase_farthest" + tag, has(off.report, "over 4 stretches: +120.0 cents at the farthest (note 60 at 1.55 s), +0.")
                                                     && has(off.report, " to +120.0, +57.") && has(off.report, " on average.")
                                                     ? 1 : 0, 1);
            P.near("stretch.phrase_tuned_cents" + tag, at(tuned.values, "pitch_cents"), 0.0, 1.0);
            // Notes off either way (-30, +30, -30, +30 cents) read 30 cents off (the farthest), not their mean, 0.
            ms::Request apart = phrase;
            apart.phrase = { { 48, 0.0, 0.5, 0.8f }, { 51, 0.5, 0.5, 0.8f }, { 48, 1.0, 0.5, 0.8f }, { 51, 1.5, 0.5, 0.8f } };
            apart.dials = { { "apart", 30.0f } };
            const ms::Result split = measured(wheel, rate, apart);
            std::printf("a phrase of notes off either way at %s: pitch_cents %+.2f\n", tag.c_str(), at(split.values, "pitch_cents"));
            P.near("stretch.phrase_apart_cents" + tag, std::fabs(at(split.values, "pitch_cents")), 30.0, 1.0);
            // A window over a phrase reads the notes sounding in it: C4 then E4 (1 s each), over 1 to 2 s E4 alone (its
            // first 50 ms, held by no key's span, E4's too, not C4's), over 0 to 2 s each note its own.
            ms::Request legato;
            legato.phrase = { { 60, 0.0, 1.0, 0.8f }, { 64, 1.0, 1.0, 0.8f } };
            legato.tailSeconds = 0.3f;
            ms::Request second = legato, whole = legato;
            second.window = { 1.0, 2.0 };
            whole.window = { 0.0, 2.0 };
            const ms::Result late = measured(wheel, rate, second), both = measured(wheel, rate, whole);
            std::printf("C4 then E4 over 1 to 2 s at %s: %s", tag.c_str(), late.report.c_str());
            P.near("stretch.window_second_hz" + tag, at(late.values, "fundamental_hz") / noteHz(64), 1.0, 0.0005);
            P.near("stretch.window_second_cents" + tag, at(late.values, "pitch_cents"), 0.0, 1.0);
            P.eq("stretch.window_second_says" + tag, has(late.report, "from the expected 329.63 Hz (note 64)") ? 1 : 0, 1);
            P.near("stretch.window_phrase_cents" + tag, at(both.values, "pitch_cents"), 0.0, 1.0);
            // Notes shorter than 0.45 s (their held part under the pitch's 0.4 s frames): sixteenth notes of C3 and D3,
            // each its own; a low note's (E1) pitch measured from a note of 0.2 s, not from one of 0.1 s, which no frame
            // that short parts from 0 Hz (saying how long a note must be), nor its harmonics' gaps under 0.35 s; and a
            // chord of a semitone (C3 and C#3), which no 0.4 s frame parts.
            ms::Request quick;
            for (int k = 0; k < 8; ++k)
                quick.phrase.push_back({ k % 2 == 0 ? 48 : 50, 0.125 * k, 0.125, 0.8f });
            quick.tailSeconds = 0.3f;
            const ms::Result sixteenths = measured(wheel, rate, quick);
            std::printf("sixteenth notes at %s: pitch_cents %+.2f\n", tag.c_str(), at(sixteenths.values, "pitch_cents"));
            P.near("stretch.sixteenths_cents" + tag, at(sixteenths.values, "pitch_cents"), 0.0, 1.0);
            const auto low = [&](float hold) {
                ms::Request q;
                q.notes = { 28 };
                q.holdSeconds = hold;
                q.tailSeconds = 0.3f;
                return measured(wheel, rate, q);
            };
            const ms::Result brief = low(0.1f), held = low(0.2f), longer = low(0.4f);
            std::printf("E1 held 0.1 s at %s: %s", tag.c_str(), brief.report.c_str());
            P.eq("stretch.low_brief_unmeasured" + tag, std::isnan(at(brief.values, "fundamental_hz")) && std::isnan(at(brief.values, "pitch_cents"))
                                                          && has(unmeasured(brief, "fundamental_hz"), "a note of 0.15 s")
                                                          && has(unmeasured(brief, "pitch_cents"), "no stretch could be read")
                                                          && has(brief.report, "Pitch: not measured: ") ? 1 : 0, 1);
            P.near("stretch.low_held_cents" + tag, at(held.values, "pitch_cents"), 0.0, 1.0);
            P.eq("stretch.low_inharmonic_unmeasured" + tag, std::isnan(at(held.values, "inharmonic_db"))
                                                               && has(unmeasured(held, "inharmonic_db"), "a note of 0.34 s")
                                                               && has(held.report, "Inharmonic: not measured, ") ? 1 : 0, 1);
            P.le("stretch.low_inharmonic_longer" + tag, at(longer.values, "inharmonic_db"), -90.0);
            ms::Request semitone = note;
            semitone.notes = { 48, 49 };
            const ms::Result near = measured(wheel, rate, semitone);
            P.eq("stretch.semitone_unmeasured" + tag, std::isnan(at(near.values, "pitch_cents"))
                                                         && has(unmeasured(near, "fundamental_hz"), "is too near it") ? 1 : 0, 1);
            // A noise hit too short for the pitch's frames (note 36 held 0.05 s) is still judged: "Unpitched", not a
            // pitch that a longer note would read, and no Inharmonic line.
            ms::Request hit;
            hit.notes = { 36 };
            hit.holdSeconds = 0.05f;
            hit.tailSeconds = 0.3f;
            const ms::Result drum = measured(pg::withLibraryIncludes(pg::singleFile(kNoiseVoice)), rate, hit);
            std::printf("a noise hit of 0.05 s at %s: %s", tag.c_str(), drum.report.c_str());
            P.eq("stretch.short_unpitched" + tag, has(drum.report, " Unpitched: ") && !has(drum.report, "Pitch: not measured")
                                                     && !has(drum.report, "Inharmonic") ? 1 : 0, 1);
        }
    }

    // ---- INHARMONIC ----------------------------------------------------------------------------------------------------
    if constexpr (kInstrument)
    {
        // A slow attack widens each harmonic's lobe past its 15 Hz within a frame; the skirt has no peak, so inharmonic_db
        // reads none (under the floor), linear or exponential; a partial of its own 60 dB under under the same attack is
        // found where it is.
        const pg::Sources wheel = pg::singleFile(kWheelVoice);
        for (const double rate : { 48000.0, 192000.0 })
        {
            const std::string tag = "_" + std::to_string(static_cast<int>(rate / 1000.0)) + "k";
            const auto attacked = [&](int shape, float odd) {
                ms::Request q;
                q.notes = { 60 };
                q.holdSeconds = 2.0f;
                q.tailSeconds = 0.3f;
                q.want = { "inharmonic_db" };
                q.dials = { { "attack", 0.3f }, { "shape", static_cast<float>(shape) }, { "odd", odd } };
                return at(measured(wheel, rate, q).values, "inharmonic_db");
            };
            const double linear = attacked(0, -200.0f), exponential = attacked(1, -200.0f), own = attacked(1, -60.0f);
            std::printf("a 0.3 s attack at %s: inharmonic_db %.1f (linear), %.1f (exponential), %.1f (a partial at -60 dB)\n", tag.c_str(),
                        linear, exponential, own);
            P.le("inharmonic.slow_attack_linear" + tag, linear, -90.0);
            P.le("inharmonic.slow_attack_exponential" + tag, exponential, -90.0);
            P.near("inharmonic.slow_attack_own" + tag, own, -60.0, 1.0);
        }
    }

    // ---- CLICKS --------------------------------------------------------------------------------------------------------
    for (const bool engine : { true, false })
    {
        const char* what = engine ? "engine" : "jump";
        const pg::Sources files = pg::singleFile(kInstrument ? clickVoice(engine) : clickEffect(engine));
        ms::TraceRequest q;
        q.find = ms::TraceFind::click;
        if constexpr (kInstrument)
        {
            q.render.notes = { 48 };
            q.render.holdSeconds = 1.0f;
            q.render.tailSeconds = 0.3f;
        }
        else
        {
            q.render.holdSeconds = 1.0f;
            q.render.tailSeconds = 0.1f;
            q.render.signal = static_cast<int>(ms::ViewInput::sine);
        }
        const ms::TraceResult t = ms::trace(files, q);
        const bool traced = std::any_of(t.problems.begin(), t.problems.end(),
                                        [](const ms::TraceProblem& p) { return p.kind == ms::TraceFind::click; });
        ms::Request asked = q.render;
        asked.signal = -1;                                   // measure plays its own signals (the sine among them)
        const ms::Result m = measured(files, 48000.0, asked);
        bool marked = false;
        if constexpr (kInstrument)
            marked = at(m.values, "click") > 0.5;
        else
            marked = markedIn(m.report, "the sine");             // the signal trace plays
        std::printf("%s: measure says %s, trace says %s\n%s", what, marked ? "a click" : "none", traced ? "a click" : "none", m.report.c_str());
        for (const std::string& line : t.lines)
            std::printf("  trace: %s\n", line.c_str());
        P.eq(std::string("clicks.") + what + "_agree", marked == traced ? 1 : 0, 1);
        P.eq(std::string("clicks.") + what + "_verdict", marked ? 1 : 0, engine ? 0 : 1);
        if constexpr (kInstrument)
            if (engine)
            {
                // The engine's step is large against the sound (more than 0.35 of its peak, every 40 ms, 1.5 cycles of
                // C3 being 11.5 ms): it is the sound's own because it comes back.
                P.ge("clicks.engine_step_large", at(m.values, "step"), 0.35 * std::pow(10.0, at(m.values, "peak_db") / 20.0));
                P.le("clicks.engine_period_long", 1.5 / noteHz(48), 0.04);
            }
    }

    if constexpr (!kInstrument)
    {
        // An effect that passes its input: the impulse's step is the test signal's own, no click.
        ms::Request r;
        r.holdSeconds = 0.5f;
        r.tailSeconds = 0.2f;
        const ms::Result m = measured(pg::singleFile(kPassEffect), 48000.0, r);
        std::printf("an effect that passes its input:\n%s", m.report.c_str());
        P.eq("clicks.pass_none_marked", has(m.report, "(a click)") ? 0 : 1, 1);
        P.eq("clicks.pass_impulse_listed", has(m.report, " in the impulse at ") ? 1 : 0, 1);
    }

    if constexpr (!kInstrument)
    {
        // trace with the test impulse as its input agrees: no click looked for, and it says why.
        for (const ms::TraceFind find : { ms::TraceFind::click, ms::TraceFind::any })
        {
            ms::TraceRequest q;
            q.find = find;
            q.render.holdSeconds = 0.5f;
            q.render.tailSeconds = 0.2f;
            q.render.signal = static_cast<int>(ms::ViewInput::impulse);
            const ms::TraceResult t = ms::trace(pg::singleFile(kPassEffect), q);
            std::string lines;
            for (const std::string& l : t.lines)
                lines += l + "\n";
            std::printf("trace of the impulse (%s):\n%s", find == ms::TraceFind::click ? "click" : "any", lines.c_str());
            const std::string k = find == ms::TraceFind::click ? "click" : "any";
            P.eq("clicks.trace_impulse_" + k + "_none", t.problems.empty() ? 1 : 0, 1);
            P.eq("clicks.trace_impulse_" + k + "_says", has(lines, "none looked for: the input is the test impulse") ? 1 : 0, 1);
        }
    }

    // ---- HELD ----------------------------------------------------------------------------------------------------------
    {
        // C3 held from 0.1 s to 0.6 s, with a note-on and a note-off of note 200 in it: the same span as C3's alone.
        ms::Timeline tl;
        tl.events.push_back({ 4800, pg::Event{ 0, pg::EventKind::noteOn, 48, 0.8f } });
        const std::vector<ms::Span> alone = ms::heldSpans(tl, 48000, 48000.0);
        tl.events.insert(tl.events.begin() + 1, { 9600, pg::Event{ 0, pg::EventKind::noteOn, 200, 0.8f } });
        tl.events.push_back({ 19200, pg::Event{ 0, pg::EventKind::noteOff, 200, 0.0f } });
        tl.events.push_back({ 28800, pg::Event{ 0, pg::EventKind::noteOff, 48, 0.0f } });
        const std::vector<ms::Span> with = ms::heldSpans(tl, 48000, 48000.0);
        std::printf("held with note 200: %zu span(s), the first [%zu, %zu)\n", with.size(), with.empty() ? 0 : with[0].first,
                    with.empty() ? 0 : with[0].second);
        P.eq("held.no_midi_note_ignored", with.size() == 1 && with[0] == ms::Span{ 4800 + 2400, 28800 } ? 1 : 0, 1);
        P.eq("held.alone_to_the_end", alone.size() == 1 && alone[0] == ms::Span{ 4800 + 2400, 48000 } ? 1 : 0, 1);
        // The expected pitch's stretches: C3 from 0.1 to 0.6 s, E3 from 0.3 to 0.9 s, the wheel to 0.5 at 0.4 s: C3 to
        // 0.4 s (E3 above it from 0.35 s), C3 bent to 0.6 s, E3 bent to its end.
        ms::Timeline two;
        two.events = { { 0, pg::Event{ 0, pg::EventKind::pitchBend, 0, 0.0f } },
                       { 4800, pg::Event{ 0, pg::EventKind::noteOn, 48, 0.8f } },
                       { 14400, pg::Event{ 0, pg::EventKind::noteOn, 52, 0.8f } },
                       { 19200, pg::Event{ 0, pg::EventKind::pitchBend, 0, 0.5f } },
                       { 28800, pg::Event{ 0, pg::EventKind::noteOff, 48, 0.0f } },
                       { 43200, pg::Event{ 0, pg::EventKind::noteOff, 52, 0.0f } } };
        const std::vector<ms::Stretch> parts = ms::stretches(two, ms::heldSpans(two, 48000, 48000.0), 48000, 48000.0, 48);
        std::string got;
        for (const ms::Stretch& p : parts)
            got += "[" + std::to_string(p.span.first) + "," + std::to_string(p.span.second) + ") " + std::to_string(p.key) + "/"
                 + std::to_string(p.next) + (p.bend != 0.0f ? " bent" : "") + "; ";
        std::printf("stretches: %s\n", got.c_str());
        P.eq("held.stretches", got == "[7200,19200) 48/52; [19200,28800) 48/52 bent; [28800,43200) 52/-1 bent; " ? 1 : 0, 1);
        P.near("held.stretch_hz", parts.size() == 3 ? parts[1].hz() : 0.0, noteHz(49), 1e-9);
        const auto listed = [](const std::vector<ms::Stretch>& ps) {
            std::string s;
            for (const ms::Stretch& p : ps)
                s += "[" + std::to_string(p.span.first) + "," + std::to_string(p.span.second) + ") " + std::to_string(p.key) + "/"
                   + std::to_string(p.next) + (p.bend != 0.0f ? " bent" : "") + "; ";
            return s;
        };
        // A window's parts no held span covers: C3 from 0.1 to 0.6 s, then E3 to 0.9 s, over 0 to 1.2 s (`key` 60): before
        // the first note-on `key`, C3's first 50 ms C3, E3's E3 (sounding), after the note-off E3 (released last).
        ms::Timeline seq;
        seq.events = { { 0, pg::Event{ 0, pg::EventKind::pitchBend, 0, 0.0f } },
                       { 4800, pg::Event{ 0, pg::EventKind::noteOn, 48, 0.8f } },
                       { 28800, pg::Event{ 0, pg::EventKind::noteOff, 48, 0.0f } },
                       { 28800, pg::Event{ 0, pg::EventKind::noteOn, 52, 0.8f } },
                       { 43200, pg::Event{ 0, pg::EventKind::noteOff, 52, 0.0f } } };
        const std::string inWindow = listed(ms::stretches(seq, { { 0, 57600 } }, 48000, 48000.0, 60));
        std::printf("stretches in a window: %s\n", inWindow.c_str());
        P.eq("held.window_sounding", inWindow == "[0,4800) 60/-1; [4800,28800) 48/-1; [28800,57600) 52/-1; " ? 1 : 0, 1);
        // The wheel's moves within a cent of each other (kSameBendCents) make one stretch, bent by their mean: C3 from 0.1
        // s, the wheel at 0.003 (0.6 cents) at 0.3 s joins it, at 0.006 at 0.5 s (1.2 cents from the first) starts the
        // next, at 0.1 at 0.7 s the next.
        ms::Timeline wheel;
        wheel.events = { { 0, pg::Event{ 0, pg::EventKind::pitchBend, 0, 0.0f } },
                         { 4800, pg::Event{ 0, pg::EventKind::noteOn, 48, 0.8f } },
                         { 14400, pg::Event{ 0, pg::EventKind::pitchBend, 0, 0.003f } },
                         { 24000, pg::Event{ 0, pg::EventKind::pitchBend, 0, 0.006f } },
                         { 33600, pg::Event{ 0, pg::EventKind::pitchBend, 0, 0.1f } } };
        const std::vector<ms::Stretch> moved = ms::stretches(wheel, ms::heldSpans(wheel, 48000, 48000.0), 48000, 48000.0, 48);
        std::printf("stretches of a wheel's small moves: %s\n", listed(moved).c_str());
        P.eq("held.wheel_within_a_cent", listed(moved) == "[7200,24000) 48/-1 bent; [24000,33600) 48/-1 bent; [33600,48000) 48/-1 bent; "
                                             ? 1 : 0, 1);
        P.near("held.wheel_mean", moved.empty() ? 0.0 : moved[0].bend, static_cast<double>(0.003f) * 9600.0 / 16800.0, 1e-7);
    }

    // ---- WINDOW --------------------------------------------------------------------------------------------------------
    if constexpr (kInstrument)
    {
        // The power centroid of sixteen harmonics at 1/h: the note times the sum of 1/h over the sum of 1/h^2.
        double num = 0.0, den = 0.0;
        for (int h = 1; h <= 16; ++h)
        {
            num += 1.0 / h;
            den += 1.0 / (h * h);
        }
        const double brightHz = noteHz(48) * num / den, brightDb = 20.0 * std::log10(0.1 * std::sqrt(den / 2.0));
        const pg::Sources after = pg::singleFile(kOnAfterVoice);
        ms::Request r;
        r.notes = { 48 };
        r.holdSeconds = 0.2f;
        r.tailSeconds = 0.5f;
        ms::Request w = r;
        w.window = { 1.5, 2.5 };
        std::array<double, 2> centroid{}, level{};
        int k = 0;
        for (const double rate : { 48000.0, 192000.0 })
        {
            const std::string tag = std::to_string(static_cast<int>(rate / 1000.0)) + "k";
            const ms::Result held = measured(after, rate, r), in = measured(after, rate, w);
            std::printf("on after its note at %s: held centroid %.1f Hz, RMS %.2f dBFS; in 1.5 to 2.5 s centroid %.1f Hz (the "
                        "harmonics' %.1f Hz), RMS %.2f dBFS (theirs %.2f), pitch %+.2f cents\n%s",
                        tag.c_str(), at(held.values, "centroid_hz"), at(held.values, "rms_db"), at(in.values, "centroid_hz"), brightHz,
                        at(in.values, "rms_db"), brightDb, at(in.values, "pitch_cents"), in.report.c_str());
            P.near("window.held_reads_the_sine_" + tag, at(held.values, "centroid_hz") / noteHz(48), 1.0, 0.02);
            P.near("window.reads_the_bright_" + tag, at(in.values, "centroid_hz") / brightHz, 1.0, 0.05);
            P.near("window.rms_" + tag, at(in.values, "rms_db"), brightDb, 0.2);
            P.near("window.pitch_" + tag, at(in.values, "pitch_cents"), 0.0, 2.0);
            P.eq("window.report_" + tag, has(in.report, "Window: 1.50 to 2.50 s of the render") && has(in.report, "In the window: peak")
                                            && has(in.report, "Spectrum in the window: ") && !has(held.report, "Window:") ? 1 : 0, 1);
            ms::Session session(after, rate);
            P.ge("window.render_reaches_its_end_" + tag, static_cast<double>(session.lengthOf(w)), 2.5 * rate);
            P.le("window.render_without_" + tag, static_cast<double>(session.lengthOf(r)), 0.75 * rate);
            centroid[static_cast<std::size_t>(k)] = at(in.values, "centroid_hz");
            level[static_cast<std::size_t>(k++)] = at(in.values, "rms_db");
        }
        P.near("window.rates_centroid", centroid[1] / centroid[0], 1.0, 0.05);
        P.near("window.rates_rms", level[1] - level[0], 0.0, 0.1);

        const auto lineOf = [](const std::string& report, const std::string& start) {
            const std::size_t a = report.find(start);
            return a == std::string::npos ? std::string() : report.substr(a, report.find('\n', a) - a);
        };
        for (const double rate : { 48000.0, 192000.0 })
        {
            const std::string tag = std::to_string(static_cast<int>(rate / 1000.0)) + "k";
            // A window past the render's own length (0.7 s): the render goes on to its end, where the voice brightens at
            // 1 s and ends at 3 s, and the whole render's metrics keep the length they have without it.
            ms::Request past = r;
            past.window = { 1.5, 3.5 };
            const ms::Result own = measured(after, rate, r), longer = measured(after, rate, past);
            std::printf("a window past the render at %s:\n%s", tag.c_str(), longer.report.c_str());
            for (const char* m : { "attack_s", "release_s", "voice_end_s", "decay_s", "step", "click", "alerts" })
                P.eq("window.past_keeps_" + std::string(m) + "_" + tag, at(own.values, m) == at(longer.values, m) ? 1 : 0, 1);
            const std::string step = lineOf(own.report, "Largest step");
            P.eq("window.past_step_line_" + tag, !step.empty() && step == lineOf(longer.report, "Largest step") ? 1 : 0, 1);
            P.eq("window.past_says_" + tag, has(longer.report, "the render goes on to its end, the steps, the decay, the release and "
                                                               "the alerts measured over the 0.70 s it has without it")
                                                && has(longer.report, "Release: still within 60 dB of the held level 0.50 s after")
                                                ? 1 : 0, 1);
            // A window between two calls' starts (blocks of 2048: 24576 and 26624 at 48 kHz): the call that renders it
            // is timed, not none.
            ms::Request between = r;
            between.block = 2048;
            between.window = { 0.5125, 0.5225 };
            const ms::Result timed = measured(after, rate, between);
            const std::string cpu = lineOf(timed.report, "CPU: ");
            std::printf("%s (%.4f%%)\n", cpu.c_str(), at(timed.values, "cpu_percent"));
            P.eq("window.cpu_timed_" + tag, has(cpu, "CPU: the window takes ") && !has(cpu, "for 0.00 s") ? 1 : 0, 1);

            // view_sound over a window shorter than its spectrum's frame (8192 points: 0.17 s at 48 kHz), just before the
            // voice brightens at 1 s: the sine alone, none of the harmonics after it.
            ms::ViewRequest before;
            before.render = r;
            before.render.tailSeconds = 1.5f;
            before.render.rate = rate;
            before.render.window = { 0.95, 0.98 };
            const ms::View sv = ms::view(after, before);
            const double pos = std::log(16.0 * noteHz(48) / sv.loHz) / std::log(sv.hiHz / sv.loHz) * static_cast<double>(sv.spectrum.size());
            const float sixteenth = sv.spectrum[static_cast<std::size_t>(std::clamp(pos, 0.0, static_cast<double>(sv.spectrum.size() - 1)))];
            std::printf("view_sound over 0.95 to 0.98 s at %s: the 16th harmonic's column %.1f dB\n", tag.c_str(), sixteenth);
            P.le("window.view_short_before_" + tag, sixteenth, -80.0);
        }

        // A window inside a held note: the notes' own spans as without it.
        ms::Request note;
        note.notes = { 48 };
        note.holdSeconds = 1.0f;
        note.tailSeconds = 0.5f;
        ms::Request inside = note;
        inside.window = { 0.3, 0.6 };
        const pg::Sources harmonic = pg::singleFile(kHarmonicVoice);
        const ms::Result a = measured(harmonic, 48000.0, note), b = measured(harmonic, 48000.0, inside);
        for (const char* m : { "attack_s", "release_s", "voice_end_s", "step", "decay_s" })
            P.eq(std::string("window.keeps_") + m, at(a.values, m) == at(b.values, m) ? 1 : 0, 1);
        P.near("window.steady_rms", at(b.values, "rms_db"), at(a.values, "rms_db"), 0.1);

        // view_sound: its RMS and spectrum over the window, its close-up inside it, the render to its end.
        ms::ViewRequest vr;
        vr.render = w;
        const ms::View v = ms::view(after, vr);
        std::printf("view_sound over 1.5 to 2.5 s: RMS %.2f dBFS, window %.3f to %.3f s, close-up %zu to %zu of %zu\n", v.rmsDb,
                    v.windowFrom, v.windowTo, v.zoomFrom, v.zoomTo, v.length);
        P.near("window.view_rms", v.rmsDb, brightDb, 0.2);
        P.eq("window.view_marks", std::fabs(v.windowFrom - 1.5) < 1e-6 && std::fabs(v.windowTo - 2.5) < 1e-6 ? 1 : 0, 1);
        P.eq("window.view_close_up_inside", v.zoomFrom >= 72000 && v.zoomTo <= 120000 ? 1 : 0, 1);
        P.ge("window.view_render_to_its_end", static_cast<double>(v.length), 2.5 * 48000.0);
        // Its spectrum: the bright part's (its sixteenth harmonic shows, which the sine has none of).
        ms::ViewRequest plain;
        plain.render = r;
        const ms::View p = ms::view(after, plain);
        const auto column = [&](const ms::View& x, double hz) {
            const double pos = std::log(hz / x.loHz) / std::log(x.hiHz / x.loHz) * static_cast<double>(x.spectrum.size());
            return x.spectrum[static_cast<std::size_t>(std::clamp(pos, 0.0, static_cast<double>(x.spectrum.size() - 1)))];
        };
        std::printf("the 16th harmonic's column: %.1f dB in the window, %.1f dB held\n", column(v, 16 * noteHz(48)),
                    column(p, 16 * noteHz(48)));
        P.ge("window.view_spectrum_bright", column(v, 16 * noteHz(48)), -45.0);
        P.le("window.view_spectrum_held_dark", column(p, 16 * noteHz(48)), -80.0);
    }
    else
    {
        const pg::Sources fader = pg::singleFile(kFaderEffect);
        ms::Request r;
        r.holdSeconds = 1.0f;
        r.tailSeconds = 0.2f;
        for (const double rate : { 48000.0, 192000.0 })
        {
            const std::string tag = std::to_string(static_cast<int>(rate / 1000.0)) + "k";
            ms::Request late = r, early = r, past = r;
            late.window = { 0.6, 0.95 };
            early.window = { 0.1, 0.4 };
            past.window = { 1.2, 1.5 };
            const ms::Result all = measured(fader, rate, r), l = measured(fader, rate, late), e = measured(fader, rate, early),
                             gone = measured(fader, rate, past);
            std::printf("a fader at %s: sine gain %.2f dB held, %.2f in 0.6 to 0.95 s, %.2f in 0.1 to 0.4 s\n%s%s", tag.c_str(),
                        at(all.values, "sine_gain_db"), at(l.values, "sine_gain_db"), at(e.values, "sine_gain_db"),
                        l.report.c_str(), gone.report.c_str());
            P.near("window.fader_sine_late_" + tag, at(l.values, "sine_gain_db"), -6.02, 0.05);
            P.near("window.fader_noise_late_" + tag, at(l.values, "noise_gain_db"), -6.02, 0.05);
            P.near("window.fader_peak_late_" + tag, at(l.values, "sine_peak_db"), -12.0 - 6.02, 0.1);
            P.near("window.fader_sine_early_" + tag, at(e.values, "sine_gain_db"), 0.0, 0.05);
            P.near("window.fader_held_between_" + tag, at(all.values, "sine_gain_db"), -2.2, 0.3);
            P.eq("window.impulse_its_own_" + tag, at(l.values, "impulse_peak_db") == at(all.values, "impulse_peak_db") ? 1 : 0, 1);
            P.eq("window.past_unmeasured_" + tag, std::isnan(at(gone.values, "sine_gain_db")) && std::isnan(at(gone.values, "noise_gain_db"))
                                                     && gone.unmeasured.count("sine_gain_db") == 1
                                                     && has(gone.report, "not measured, the window starts after the sine and the noise end")
                                                     && !std::isnan(at(gone.values, "impulse_peak_db")) ? 1 : 0, 1);
            P.eq("window.report_" + tag, has(l.report, "Window: 0.60 to 0.95 s of the sine's and the noise's renders")
                                            && has(l.report, "Sine 220 Hz at -12.0 dBFS for 1.00 s, in the window: ") ? 1 : 0, 1);
            // One that holds under 0.01 s of them (0.995 s to their end at 1 s): not measured either, saying why.
            ms::Request edge = r;
            edge.window = { 0.995, 1.5 };
            const ms::Result little = measured(fader, rate, edge);
            const auto why = little.unmeasured.find("noise_gain_db");
            P.eq("window.short_unmeasured_" + tag, std::isnan(at(little.values, "sine_gain_db")) && std::isnan(at(little.values, "thd_db"))
                                                      && why != little.unmeasured.end()
                                                      && has(why->second, "the window holds less than 0.01 s of the sine and the noise "
                                                                          "(they end at hold_seconds, 1.00 s)")
                                                      ? 1 : 0, 1);
            // view_sound's sine over 30 ms (shorter than its spectrum's frame: 0.17 s at 48 kHz) just before the move at
            // 0.5 s and just after: its spectrum's level 6 dB apart, nothing of the one in the other.
            ms::ViewRequest shortly;
            shortly.render = r;
            shortly.render.rate = rate;
            shortly.input = ms::ViewInput::sine;
            shortly.render.window = { 0.45, 0.48 };
            const ms::View before = ms::view(fader, shortly);
            shortly.render.window = { 0.6, 0.63 };
            const ms::View after = ms::view(fader, shortly);
            std::printf("view_sound's sine over 30 ms at %s: spectrum top %.2f dB before the move, %.2f after\n", tag.c_str(),
                        before.spectrumTop, after.spectrumTop);
            P.near("window.view_short_before_" + tag, before.spectrumTop - after.spectrumTop, 6.02, 0.2);
        }
        // view_sound's sine through the fader: the RMS over the window, at -6 dB.
        ms::ViewRequest vr;
        vr.render = r;
        vr.render.window = { 0.6, 0.95 };
        vr.input = ms::ViewInput::sine;
        const ms::View v = ms::view(fader, vr);
        std::printf("view_sound's sine over 0.6 to 0.95 s: RMS %.2f dBFS, close-up %zu to %zu\n", v.rmsDb, v.zoomFrom, v.zoomTo);
        P.near("window.view_rms", v.rmsDb, -12.0 - 3.01 - 6.02, 0.1);
        P.eq("window.view_close_up_inside", v.zoomFrom >= 28800 && v.zoomTo <= 45600 ? 1 : 0, 1);
    }
    {
        // Checks alike but for their windows: measured apart; one outside says why.
        ms::Check plain, windowed, outside;
        plain.label = windowed.label = outside.label = "level";
        plain.goal.metric = kInstrument ? "rms_db" : "sine_gain_db";
        plain.goal.hasMax = true;
        plain.goal.max = 100.0;
        windowed.goal = outside.goal = plain.goal;
        if constexpr (kInstrument)
        {
            plain.request.notes = { 48 };
            plain.request.holdSeconds = 0.2f;
            plain.request.tailSeconds = 0.5f;
        }
        else
        {
            plain.request.holdSeconds = 1.0f;
            plain.request.tailSeconds = 0.2f;
        }
        windowed.request = outside.request = plain.request;
        windowed.request.window = kInstrument ? ms::Window{ 1.5, 2.5 } : ms::Window{ 0.6, 0.95 };
        outside.request.window = { 1.2, 1.5 };
        const std::vector<ms::CheckResult> res = ms::evaluate(pg::singleFile(kInstrument ? kOnAfterVoice : kFaderEffect),
                                                              kInstrument ? std::vector<ms::Check>{ plain, windowed }
                                                                          : std::vector<ms::Check>{ plain, windowed, outside },
                                                              {});
        for (const ms::CheckResult& c : res)
            std::printf("check: %s\n", c.text.c_str());
        P.eq("window.checks_apart", res.size() >= 2 && res[0].measured && res[1].measured
                                        && std::fabs(res[0].value - res[1].value) > (kInstrument ? 1.0 : 3.0) ? 1 : 0, 1);
        if constexpr (!kInstrument)
            P.eq("window.check_outside_says_why", res.size() == 3 && !res[2].measured
                                                      && has(res[2].text, "not measured: the window starts after the sine") ? 1 : 0, 1);
    }

    // ---- BANDS ---------------------------------------------------------------------------------------------------------
    {
        // A report's bands line (Request::spectrum) as numbers: an octave's and a third's by name, a semitone's in order.
        const auto bandsIn = [](const std::string& report, const char* start) {
            std::vector<std::pair<std::string, double>> out;
            const std::size_t a = report.find(start);
            if (a == std::string::npos)
                return out;
            const std::size_t open = report.find("): ", a), end = report.find(".\n", a);
            if (open == std::string::npos || end == std::string::npos)
                return out;
            const std::string list = report.substr(open + 3, end - open - 3);
            for (std::size_t p = 0; p < list.size();)
            {
                std::size_t q = list.find_first_of(";,", p);
                if (q == std::string::npos)
                    q = list.size();
                const std::string entry = list.substr(p, q - p);
                const std::size_t colon = entry.find(": ");
                if (colon != std::string::npos)
                {
                    const std::string name = entry.substr(entry.find_first_not_of(' '), colon - entry.find_first_not_of(' '));
                    std::size_t v = colon + 2;
                    while (v < entry.size())
                    {
                        const std::size_t w = std::min(entry.find(' ', v), entry.size());
                        const std::string number = entry.substr(v, w - v);
                        out.push_back({ out.empty() || v == colon + 2 ? name : std::string(), number == "none" ? std::nan("") : std::strtod(number.c_str(), nullptr) });
                        v = w + 1;
                    }
                }
                p = q + 1;
            }
            return out;
        };
        const auto named = [](const std::vector<std::pair<std::string, double>>& bands, const std::string& name) {
            for (const auto& [n, v] : bands)
                if (n == name)
                    return v;
            return std::nan("");
        };
        if constexpr (kInstrument)
        {
            // A sine at note 59 (246.9 Hz, inside the 250 Hz third and octave): its band reads its RMS, the same at 48 and
            // 192 kHz; its semitone point too, the notes beside it far under. Each line names its frames (the semitones'
            // as long as the held part, 0.95 s) and the band below which a tone spreads (Measure.h's kSharpBins).
            const pg::Sources sine = pg::singleFile(kTwoToneVoice);
            std::array<double, 2> third{}, semitone{};
            int k = 0;
            for (const double rate : { 48000.0, 192000.0 })
            {
                const std::string tag = std::to_string(static_cast<int>(rate / 1000.0)) + "k";
                ms::Request r;
                r.notes = { 59 };
                r.holdSeconds = 1.0f;
                r.tailSeconds = 0.2f;
                r.spectrum = ms::Bands::third;
                const ms::Result t = measured(sine, rate, r);
                r.spectrum = ms::Bands::octave;
                const ms::Result o = measured(sine, rate, r);
                r.spectrum = ms::Bands::semitone;
                const ms::Result s = measured(sine, rate, r);
                const double rms = at(t.values, "rms_db");
                const auto thirds = bandsIn(t.report, "Third-octave bands while held (Hz: dBFS; 0.4 s Hann frames; below 25 Hz a "
                                                      "tone spreads over the bands beside it): ");
                const auto octaves = bandsIn(o.report, "Octave bands while held (Hz: dBFS; 0.4 s Hann frames): ");
                const auto notes = bandsIn(s.report, "Semitone bands while held, a point a MIDI note, an octave a group after its "
                                                     "first note (dBFS; 0.95 s Hann frames, as long as the part, up to 2 s; below "
                                                     "E1 a tone spreads over the bands beside it): ");
                std::printf("a sine at note 59 at %s: RMS %.2f dBFS; 250 Hz third %.2f, octave %.2f; semitone 58 %.1f, 59 %.2f, 60 "
                            "%.1f (%zu thirds, %zu octaves, %zu semitones)\n%s%s%s",
                            tag.c_str(), rms, named(thirds, "250"), named(octaves, "250"),
                            notes.size() > 44 ? notes[42].second : 0.0, notes.size() > 44 ? notes[43].second : 0.0,
                            notes.size() > 44 ? notes[44].second : 0.0, thirds.size(), octaves.size(), notes.size(), t.report.c_str(),
                            o.report.c_str(), s.report.c_str());
                P.eq("bands.counts_" + tag, thirds.size() == 31 && octaves.size() == 10 && notes.size() == 120 ? 1 : 0, 1);
                P.near("bands.third_reads_rms_" + tag, named(thirds, "250"), rms, 0.1);
                P.near("bands.octave_reads_rms_" + tag, named(octaves, "250"), rms, 0.1);
                // notes[0] is note 16: note 59 is the 44th.
                const double own = notes.size() == 120 ? notes[43].second : std::nan("");
                P.near("bands.semitone_reads_rms_" + tag, own, rms, 0.6);   // printed in whole dB
                P.le("bands.semitone_neighbours_under_" + tag,
                     notes.size() == 120 ? std::max(notes[42].second, notes[44].second) - own : 0.0, -20.0);
                P.le("bands.third_others_under_" + tag, std::max(named(thirds, "200"), named(thirds, "315")) - rms, -30.0);
                third[static_cast<std::size_t>(k)] = named(thirds, "250");
                semitone[static_cast<std::size_t>(k++)] = own;
            }
            P.near("bands.rates_third", third[1] - third[0], 0.0, 0.05);
            P.near("bands.rates_semitone", semitone[1] - semitone[0], 0.0, 0.1);

            // A sine at E1 (note 28, 41.2 Hz: its semitone 2.4 Hz wide, under a 0.4 s frame's main lobe): held 0.95 s, in
            // frames as long as that, it reads its RMS within 0.3 dB (printed in whole dB) and the line names E1 the
            // lowest note that does; held 2 s, in frames of 2 s, every note does (the line says nothing below), the notes
            // beside it 30 dB under.
            for (const double rate : { 48000.0, 192000.0 })
                for (const float hold : { 1.0f, 2.05f })
                {
                    const bool part = hold < 1.5f;
                    const std::string tag = std::string(part ? "095s_" : "2s_") + std::to_string(static_cast<int>(rate / 1000.0)) + "k";
                    ms::Request r;
                    r.notes = { 28 };
                    r.holdSeconds = hold;
                    r.tailSeconds = 0.2f;
                    r.spectrum = ms::Bands::semitone;
                    const ms::Result s = measured(sine, rate, r);
                    const double rms = at(s.values, "rms_db");
                    const auto notes = bandsIn(s.report, "Semitone bands while held, a point a MIDI note");
                    // notes[0] is note 16: note 28 is the 13th.
                    const double own = notes.size() == 120 ? notes[12].second : std::nan("");
                    const double beside = notes.size() == 120 ? std::max(notes[11].second, notes[13].second) - own : 0.0;
                    std::printf("a sine at E1 held %.2f s at %.0f Hz: RMS %.2f dBFS, its semitone %.0f, the notes beside it %+.0f dB\n",
                                static_cast<double>(hold), rate, rms, own, beside);
                    P.near("bands.low_note_reads_rms_" + tag, own, rms, part ? 0.8 : 0.55);   // printed in whole dB
                    P.le("bands.low_note_beside_under_" + tag, beside, part ? -13.0 : -30.0);
                    P.eq("bands.low_note_frames_" + tag,
                         has(s.report, part ? "(dBFS; 0.95 s Hann frames, as long as the part, up to 2 s; below E1 a tone spreads "
                                              "over the bands beside it): E0 20.6: "
                                            : "(dBFS; 2 s Hann frames): E0 20.6: ")
                             ? 1 : 0, 1);
                }

            // A phrase whose notes are held 0.2 and 0.55 s: the octaves' frames as long as each note's part, up to 0.4 s,
            // the line saying so; its 250 Hz octave its RMS.
            {
                ms::Request r;
                r.phrase = { { 59, 0.0, 0.25, 0.8f }, { 59, 0.5, 0.6, 0.8f } };
                r.tailSeconds = 0.2f;
                r.spectrum = ms::Bands::octave;
                const ms::Result p2 = measured(sine, 48000.0, r);
                const auto octaves = bandsIn(p2.report, "Octave bands while held (Hz: dBFS; Hann frames of 0.2 to 0.4 s, each as long "
                                                        "as its part, up to 0.4 s): ");
                std::printf("a phrase held 0.2 and 0.55 s: RMS %.2f dBFS, its 250 Hz octave %.2f (%zu octaves)\n",
                            at(p2.values, "rms_db"), named(octaves, "250"), octaves.size());
                P.eq("bands.phrase_frames_named", static_cast<int>(octaves.size()), 10);
                P.near("bands.phrase_octave_reads_rms", named(octaves, "250"), at(p2.values, "rms_db"), 0.1);
            }

            // The rule those lines state (Measure.h's kSharpBins, sharpFrom), at 48 and 192 kHz over a part of 0.1, 0.95
            // and 3 s: a tone at the centre of each band from sharpFrom up reads its level within 0.3 dB and the bands
            // beside it 13 dB under it (tones three bands apart, a signal for each of the three), and sharpFrom is where
            // the docs say (a semitone's E1 at 0.95 s and every note from 2 s; a third's 25 Hz at 0.4 s).
            for (const double rate : { 48000.0, 192000.0 })
                for (const double seconds : { 0.1, 0.95, 3.0 })
                    for (const ms::Bands kind : { ms::Bands::octave, ms::Bands::third, ms::Bands::semitone })
                    {
                        const char* name = kind == ms::Bands::octave ? "octave" : kind == ms::Bands::third ? "third" : "semitone";
                        const std::string tag = std::string(name) + "_" + std::to_string(static_cast<int>(seconds * 100.0 + 0.5))
                                              + "cs_" + std::to_string(static_cast<int>(rate / 1000.0)) + "k";
                        const auto n = static_cast<std::size_t>(std::llround(seconds * rate));
                        const std::vector<ms::Band> bands = ms::bandsOf(kind);
                        const std::vector<float> silence(n, 0.0f);
                        const std::size_t from = ms::sharpFrom(kind, ms::bandSpectrum(kind, silence, silence, rate, { { 0, n } }));
                        const std::size_t expected = kind == ms::Bands::octave     ? (seconds < 0.2 ? 1u : 0u)
                                                   : kind == ms::Bands::third      ? (seconds < 0.2 ? 7u : 1u)
                                                   : seconds < 0.2                 ? 51u           // G4, 392 Hz
                                                   : seconds < 2.0                 ? 12u           // E1
                                                                                   : 0u;
                        P.eq("bands.sharp_from_" + tag, static_cast<int>(from), static_cast<int>(expected));
                        double worst = 0.0, worstBeside = -300.0;
                        int tones = 0;
                        for (std::size_t offset = 0; offset < 3; ++offset)
                        {
                            std::vector<double> x(n, 0.0);
                            const double amp = 0.05;
                            for (std::size_t b = from + offset; b < bands.size(); b += 3)
                            {
                                const double hz = std::sqrt(bands[b].lo * bands[b].hi);
                                const std::complex<double> turn = std::polar(1.0, 2.0 * std::numbers::pi * hz / rate);
                                std::complex<double> z = std::polar(1.0, 0.7 * static_cast<double>(b));
                                for (std::size_t i = 0; i < n; ++i, z *= turn)
                                    x[i] += amp * z.imag();
                            }
                            const std::vector<float> xf(x.begin(), x.end());
                            const std::vector<double> levels = ms::bandLevels(ms::bandSpectrum(kind, xf, xf, rate, { { 0, n } }), kind);
                            const double level = 10.0 * std::log10(0.5 * amp * amp);
                            for (std::size_t b = from + offset; b < bands.size(); b += 3)
                            {
                                worst = std::max(worst, std::fabs(levels[b] - level));
                                if (b > 0)
                                    worstBeside = std::max(worstBeside, levels[b - 1] - level);
                                if (b + 1 < bands.size())
                                    worstBeside = std::max(worstBeside, levels[b + 1] - level);
                                ++tones;
                            }
                        }
                        std::printf("%s bands over %.2f s at %.0f Hz: sharp from band %zu, %d tones, the worst %.3f dB off, the bands "
                                    "beside them %+.1f dB\n", name, seconds, rate, from, tones, worst, worstBeside);
                        P.le("bands.sharp_reads_level_" + tag, worst, 0.3);
                        P.le("bands.sharp_beside_under_" + tag, worstBeside, -13.0);
                    }

            // Eight harmonics: the thirds' power sums to the RMS's; white noise's bands grow with their width (3 dB an
            // octave: the 8 kHz third 9 dB over the 1 kHz one) at both rates.
            ms::Request r;
            r.notes = { 48 };
            r.holdSeconds = 1.0f;
            r.tailSeconds = 0.2f;
            r.spectrum = ms::Bands::third;
            const ms::Result h = measured(pg::singleFile(kHarmonicVoice), 48000.0, r);
            double sum = 0.0;
            for (const auto& [n, v] : bandsIn(h.report, "Third-octave bands while held"))
                sum += std::pow(10.0, v / 10.0);
            std::printf("eight harmonics: the thirds sum to %.2f dBFS, its RMS %.2f dBFS\n", 10.0 * std::log10(sum), at(h.values, "rms_db"));
            P.near("bands.thirds_sum_to_rms", 10.0 * std::log10(sum), at(h.values, "rms_db"), 0.2);
            const pg::Sources noise = pg::withLibraryIncludes(pg::singleFile(kNoiseVoice));
            for (const double rate : { 48000.0, 192000.0 })
            {
                const auto thirds = bandsIn(measured(noise, rate, r).report, "Third-octave bands while held");
                std::printf("white noise at %.0f Hz: the 1 kHz third %.2f dBFS, the 8 kHz %.2f\n", rate, named(thirds, "1k"),
                            named(thirds, "8k"));
                P.near("bands.noise_widths_" + std::to_string(static_cast<int>(rate / 1000.0)) + "k",
                       named(thirds, "8k") - named(thirds, "1k"), 9.03, 1.0);
            }
        }
        else
        {
            // The fader at half (-6.02 dB) over 0.6 to 0.95 s, a window shorter than a frame: every band of the noise out
            // against in reads it, each line naming its frames of 0.35 s; the two low-passes at 1.5 kHz pass the low bands
            // and read the same at 48 and 192 kHz up to 2 kHz (above, the program's one-poles differ: at 48 kHz they bend
            // near the Nyquist frequency).
            const pg::Sources fader = pg::singleFile(kFaderEffect), dark = pg::singleFile(kDarkEffect);
            std::array<std::vector<std::pair<std::string, double>>, 2> darkBands;
            int k = 0;
            for (const double rate : { 48000.0, 192000.0 })
            {
                const std::string tag = std::to_string(static_cast<int>(rate / 1000.0)) + "k";
                ms::Request r;
                r.holdSeconds = 1.0f;
                r.tailSeconds = 0.2f;
                r.window = { 0.6, 0.95 };
                int at6 = 0, all = 0, framed = 0;
                for (const ms::Bands kind : { ms::Bands::octave, ms::Bands::third, ms::Bands::semitone })
                {
                    r.spectrum = kind;
                    const ms::Result f = measured(fader, rate, r);
                    const char* start = kind == ms::Bands::octave ? "Octave bands of the noise in the window, out against in (Hz: gain dB"
                                      : kind == ms::Bands::third  ? "Third-octave bands of the noise in the window, out against in (Hz: gain dB"
                                                                  : "Semitone bands of the noise in the window, out against in, a point";
                    for (const auto& [n, v] : bandsIn(f.report, start))
                    {
                        at6 += std::fabs(v + 6.0206) < 0.051 ? 1 : 0;   // printed to 0.1 dB
                        ++all;
                    }
                    framed += has(f.report, kind == ms::Bands::octave ? "gain dB; 0.35 s Hann frames, as long as the part, up to 0.4 s): 31.5: "
                                          : kind == ms::Bands::third
                                              ? "gain dB; 0.35 s Hann frames, as long as the part, up to 0.4 s; below 31.5 Hz a tone "
                                                "spreads over the bands beside it): 20: "
                                              : "gain dB; 0.35 s Hann frames, as long as the part, up to 2 s; below A#2 a tone spreads "
                                                "over the bands beside it): E0 20.6: ")
                                  ? 1 : 0;
                    if (rate == 48000.0 && kind == ms::Bands::third)
                        std::printf("%s", f.report.c_str());
                }
                std::printf("the fader at half in the window at %s: %d of %d bands at -6.02 dB\n", tag.c_str(), at6, all);
                P.eq("bands.fader_gain_" + tag, at6, 161);
                P.eq("bands.fader_counted_" + tag, all, 161);
                P.eq("bands.fader_frames_named_" + tag, framed, 3);

                // A gain of a half replaying its own input (Request::input: a second of noise, the channels apart): every
                // band of the line of its own input, out against in, reads -6.02 dB, over all of it (frames of 0.4 s and,
                // the semitones', of 1 s) and over a window of 0.3 to 0.8 s (0.4 s and 0.5 s).
                {
                    auto own = std::make_shared<ms::Input>();
                    uint32_t seed = 777;
                    for (std::size_t i = 0; i < static_cast<std::size_t>(rate); ++i)
                        for (std::vector<float>* ch : { &own->l, &own->r })
                        {
                            seed = seed * 1664525u + 1013904223u;
                            ch->push_back(0.2f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f));
                        }
                    int half = 0, counted = 0;
                    for (const bool windowed : { false, true })
                        for (const ms::Bands kind : { ms::Bands::octave, ms::Bands::semitone })
                        {
                            ms::Request q;
                            q.holdSeconds = 1.0f;
                            q.tailSeconds = 0.1f;
                            q.input = own;
                            q.spectrum = kind;
                            if (windowed)
                                q.window = { 0.3, 0.8 };
                            const ms::Result f = measured(pg::singleFile(kHalfEffect), rate, q);
                            const std::string start
                                = kind == ms::Bands::octave
                                    ? std::string("Octave bands of its own input") + (windowed ? " in the window" : "")
                                          + ", out against in (Hz: gain dB; 0.4 s Hann frames): "
                                    : std::string("Semitone bands of its own input") + (windowed ? " in the window" : "")
                                          + ", out against in, a point a MIDI note, an octave a group after its first note (gain dB; "
                                          + (windowed ? "0.5 s Hann frames, as long as the part, up to 2 s; below E2"
                                                      : "1 s Hann frames, as long as the part, up to 2 s; below E1")
                                          + " a tone spreads over the bands beside it): ";
                            if (windowed && kind == ms::Bands::semitone && rate == 48000.0)
                                std::printf("%s", f.report.c_str());
                            for (const auto& [n, v] : bandsIn(f.report, start.c_str()))
                            {
                                half += std::fabs(v + 6.0206) < 0.051 ? 1 : 0;   // printed to 0.1 dB
                                ++counted;
                            }
                        }
                    std::printf("a half replaying its own input at %s: %d of %d bands at -6.02 dB\n", tag.c_str(), half, counted);
                    P.eq("bands.replay_gain_" + tag, half, 260);
                    P.eq("bands.replay_counted_" + tag, counted, 260);
                }
                ms::Request d;
                d.holdSeconds = 1.0f;
                d.tailSeconds = 0.2f;
                d.spectrum = ms::Bands::octave;
                const ms::Result dr = measured(dark, rate, d);
                std::printf("two low-passes at 1.5 kHz at %s:\n%s", tag.c_str(), dr.report.c_str());
                darkBands[static_cast<std::size_t>(k++)] = bandsIn(dr.report, "Octave bands of the noise, out against in (Hz: gain dB");
                P.near("bands.dark_passes_low_" + tag, named(darkBands[static_cast<std::size_t>(k - 1)], "63"), 0.0, 0.1);
            }
            int same = 0;
            for (const char* band : { "31.5", "63", "125", "250", "500", "1k", "2k" })
                same += std::fabs(named(darkBands[1], band) - named(darkBands[0], band)) < 0.15 ? 1 : 0;
            P.eq("bands.dark_rates", same, 7);
            P.le("bands.dark_cuts_high", named(darkBands[0], "8k"), -20.0);
        }
    }

    // ---- LOUDNESS: lufs and rms_a_db (an instrument's), noise_lufs (an effect's) -----------------------------------------
    for (const double rate : { 48000.0, 192000.0 })
    {
        const std::string tag = rate == 48000.0 ? "48k" : "192k";
        if constexpr (kInstrument)
        {
            ms::Request r;
            r.holdSeconds = 1.0f;
            r.tailSeconds = 0.1f;
            r.want = { "lufs", "rms_a_db", "rms_db" };
            const auto tone = [&](double hz, double amp, const ms::Request& q) {
                return measured(pg::singleFile(toneVoice(hz, amp)), rate, q);
            };
            // BS.1770's reference: a sine at 997 Hz reads its mean square, both channels' summed, plus the K-weighting
            // there less 0.691 dB (one channel's 0 dBFS sine reads -3.01): of amplitude 0.1 in both, -20.0 LUFS.
            const ms::Result ref = tone(997.0, 0.1, r);
            if (rate == 48000.0)
                std::printf("a sine at 997 Hz, amplitude 0.1:\n%s", ref.report.c_str());
            const double lref = at(ref.values, "lufs");
            P.near("loudness.reference_" + tag, lref, -20.0, 0.05);
            P.eq("loudness.line_" + tag, has(ref.report, "Loudness while held: -20.0 LUFS integrated (BS.1770); RMS A-weighted -23.0 dBFS.")
                                             ? 1 : 0, 1);
            // The K-weighting against 997 Hz, as BS.1770's 48 kHz filters have it: -1.82 dB at 100 Hz, +3.35 at 10 kHz.
            for (const auto& [hz, k] : { std::pair{ 100.0, -1.824 }, std::pair{ 10000.0, 3.351 } })
            {
                const double l = at(tone(hz, 0.1, r).values, "lufs");
                std::printf("a sine at %.0f Hz at %s: %.3f LUFS (%+.3f dB against 997 Hz)\n", hz, tag.c_str(), l, l - lref);
                P.near("loudness.k_" + std::to_string(static_cast<int>(hz)) + "_" + tag, l - lref, k, 0.05);
            }
            // A-weighting: rms_db plus IEC 61672's A at the tone, from 31.5 Hz (its slope steep there) up.
            for (const auto& [hz, a] : { std::pair{ 31.5, -39.529 }, std::pair{ 100.0, -19.145 }, std::pair{ 1000.0, 0.0 },
                                         std::pair{ 10000.0, -2.492 } })
            {
                const ms::Result t = tone(hz, 0.1, r);
                const double got = at(t.values, "rms_a_db") - at(t.values, "rms_db");
                std::printf("a sine at %.1f Hz at %s: A-weighted %+.3f dB\n", hz, tag.c_str(), got);
                P.near("loudness.a_" + std::to_string(static_cast<int>(hz)) + "_" + tag, got, a, 0.1);
            }
            // The gates: 0.5 s at amplitude 0.1, then 40 dB under to 1.5 s. Of the eleven blocks the quiet ones are 10 LU
            // under the mean, so the five with any of the loud part make it (loud for 1, 0.875, 0.625, 0.375 and 0.125 of
            // a block: 0.6 of its power), 3.4 dB above the eleven's.
            ms::Request g = r;
            g.holdSeconds = 1.5f;
            const double gated = at(measured(pg::singleFile(toneVoice(997.0, 0.1, 0.5, 0.01)), rate, g).values, "lufs");
            std::printf("loud for 0.5 s, then 40 dB under to 1.5 s, at %s: %.3f LUFS\n", tag.c_str(), gated);
            P.near("loudness.relative_gate_" + tag, gated - lref, 10.0 * std::log10((3.0 + 2.0e-4) / 5.0), 0.02);
            // Under the absolute gate (-70 LUFS): -240, in words "under -70 LUFS".
            const ms::Result quiet = tone(997.0, 1.0e-4, r);
            P.eq("loudness.absolute_gate_" + tag, at(quiet.values, "lufs") == -240.0
                                                      && has(quiet.report, "Loudness while held: under -70 LUFS integrated")
                                                      ? 1 : 0, 1);
        }
        else
        {
            // The test noise's own (-11.9 LUFS at 48 kHz: white, its power above 2 kHz 4 dB up; at 192 kHz -11.7, more
            // of it there), as an effect that passes it reads; a gain of a half 6.02 dB under it at both rates.
            ms::Request r;
            r.holdSeconds = 1.0f;
            r.tailSeconds = 0.1f;
            r.want = { "noise_lufs" };
            const ms::Result pass = measured(pg::singleFile(kPassEffect), rate, r);
            const double half = at(measured(pg::singleFile(kHalfEffect), rate, r).values, "noise_lufs");
            const double in = at(pass.values, "noise_lufs");
            std::printf("the noise's loudness at %s: %.3f LUFS, halved %.3f\n%s", tag.c_str(), in, half, pass.report.c_str());
            P.near("loudness.noise_in_" + tag, in, rate == 48000.0 ? -11.855 : -11.720, 0.01);
            P.near("loudness.half_" + tag, half - in, -6.0206, 0.01);
            P.eq("loudness.noise_line_" + tag,
                 has(pass.report, std::string("; loudness ") + (rate == 48000.0 ? "-11.9 LUFS out, -11.9 LUFS in" : "-11.7 LUFS out, -11.7 LUFS in")
                                      + " (BS.1770, integrated).") ? 1 : 0, 1);
        }
    }

    // ---- ORDERS: off_order_db ------------------------------------------------------------------------------------------
    for (const double rate : { 48000.0, 192000.0 })
    {
        const std::string tag = rate == 48000.0 ? "48k" : "192k";
        if constexpr (kInstrument)
        {
            ms::Request r;
            r.notes = { 48 };
            r.holdSeconds = 1.0f;
            r.tailSeconds = 0.3f;
            r.want = { "off_order_db" };
            // Eight harmonics alone: nothing off their orders but the frames' sidelobes.
            const ms::Result pure = measured(pg::singleFile(kHarmonicVoice), rate, r);
            std::printf("eight harmonics at %s:\n%s", tag.c_str(), pure.report.c_str());
            P.le("orders.harmonics_" + tag, at(pure.values, "off_order_db"), -70.0);
            // A line of its own at 470 Hz, 20 dB under the fundamental: its share of all the power.
            double harmonics = 0.0;
            for (int h = 1; h <= 8; ++h)
                harmonics += 0.5 * (0.1 / h) * (0.1 / h);
            const double share = 10.0 * std::log10(5.0e-5 / (harmonics + 5.0e-5));
            const ms::Result line = measured(pg::singleFile(ordersVoice(470.0, 0.01)), rate, r);
            std::printf("and a line at 470 Hz:\n%s", line.report.c_str());
            P.near("orders.line_" + tag, at(line.values, "off_order_db"), share, 0.1);
            P.eq("orders.line_named_" + tag,
                 has(line.report, "Off the orders of 130.81 Hz (the expected pitch) while held, more than 6.32 Hz from each in 0.95 s "
                                  "Blackman-Harris frames: ")
                         && has(line.report, "the strongest lines off them 470.0 Hz (-20 dB)") ? 1 : 0, 1);
            // A half-order: at 220 Hz (note 57), a line at 330 Hz is off the orders and among the half-orders; base_hz
            // 110 has it on its orders.
            ms::Request a3 = r;
            a3.notes = { 57 };
            const ms::Result half = measured(pg::singleFile(ordersVoice(330.0, 0.01)), rate, a3);
            std::printf("a line at 330 Hz over 220 Hz's harmonics:\n%s", half.report.c_str());
            P.near("orders.half_" + tag, at(half.values, "off_order_db"), share, 0.1);
            P.eq("orders.half_named_" + tag, has(half.report, "-21.9 dB of the power (half-orders -21.9 dB, third-orders -1") ? 1 : 0, 1);
            a3.baseHz = 110.0;
            const ms::Result based = measured(pg::singleFile(ordersVoice(330.0, 0.01)), rate, a3);
            std::printf("and base_hz 110:\n%s", based.report.c_str());
            P.le("orders.base_" + tag, at(based.values, "off_order_db"), -70.0);
            P.eq("orders.base_named_" + tag, has(based.report, "Off the orders of 110.00 Hz (base_hz) while held") ? 1 : 0, 1);
            // A base under twice the zone: every frequency is near an order, so nothing is measured, saying why. A
            // phrase of sixteenth notes at 120 bpm from C3: each held 0.075 s, the zone 80 Hz, the expected pitch under
            // twice it (and no note held long enough for wobble_db's 0.1 s frame).
            const auto unmeasured = [&](const ms::Result& m, const char* metric, const char* why, const char* said) {
                const auto it = m.unmeasured.find(metric);
                return m.values.count(metric) == 0 && it != m.unmeasured.end() && has(it->second, why) && has(m.report, said);
            };
            a3.baseHz = 10.0;
            const ms::Result low = measured(pg::singleFile(ordersVoice(330.0, 0.01)), rate, a3);
            std::printf("base_hz 10:\n%s", low.report.c_str());
            P.eq("orders.base_too_low_" + tag,
                 unmeasured(low, "off_order_db", "every frequency is within 6.32 Hz of a multiple of 10.00 Hz, the base being under "
                                                 "twice that (6 bins of 0.95 s Blackman-Harris frames; longer notes would narrow it",
                            "Off the orders of 10.00 Hz (base_hz) while held: not measured, every frequency is within") ? 1 : 0, 1);
            ms::Request run = r;
            run.want = { "off_order_db", "wobble_db" };
            for (int k = 0; k < 16; ++k)
                run.phrase.push_back({ 48 + 4 * (k % 3), 0.125 * k, 0.125, 0.8f });
            const ms::Result sixteenths = measured(pg::singleFile(kHarmonicVoice), rate, run);
            std::printf("sixteenths from C3:\n%s", sixteenths.report.c_str());
            P.eq("orders.short_notes_" + tag,
                 unmeasured(sixteenths, "off_order_db", "every frequency is within 80 Hz of a multiple of 130.81 Hz",
                            "while held: not measured, every frequency is within 80 Hz") ? 1 : 0, 1);
            P.eq("wobble.short_notes_" + tag,
                 unmeasured(sixteenths, "wobble_db", "the held part makes no 0.1 s frame (each note's held stretch makes its own, "
                                                     "half a frame apart: three take one over 0.15 s, so a note held over 0.2 s)",
                            "Wobble: not measured, the held part makes no 0.1 s frame") ? 1 : 0, 1);
        }
        else
        {
            ms::Request r;
            r.holdSeconds = 1.0f;
            r.tailSeconds = 0.1f;
            r.want = { "off_order_db", "thd_db" };
            // A sine passed: nothing off 220 Hz's orders. A cubic shaper: its harmonics distort (thd_db), all on the
            // orders. A line of its own at 470 Hz: its share of the sine's power.
            const ms::Result pass = measured(pg::singleFile(kPassEffect), rate, r);
            const ms::Result cubic = measured(pg::singleFile(addEffect(470.0, 0.0, 4.0)), rate, r);
            const ms::Result line = measured(pg::singleFile(addEffect(470.0, 0.01, 0.0)), rate, r);
            std::printf("at %s: passed\n%sa cubic\n%sa line at 470 Hz\n%s", tag.c_str(), pass.report.c_str(), cubic.report.c_str(),
                        line.report.c_str());
            const double sine = 0.5 * std::pow(10.0, -12.0 / 10.0);
            P.le("orders.pass_" + tag, at(pass.values, "off_order_db"), -70.0);
            P.ge("orders.cubic_distorts_" + tag, at(cubic.values, "thd_db"), -40.0);
            P.le("orders.cubic_on_orders_" + tag, at(cubic.values, "off_order_db"), -70.0);
            P.near("orders.line_" + tag, at(line.values, "off_order_db"), 10.0 * std::log10(5.0e-5 / (sine + 5.0e-5)), 0.1);
            P.eq("orders.line_named_" + tag, has(line.report, "Off the orders of 220 Hz in the sine's output, more than 6.32 Hz from each")
                                                 && has(line.report, "the strongest lines off them 470.0 Hz (-28 dB)") ? 1 : 0, 1);
            // hold_seconds 0.1: held 0.05 s, the zone 120 Hz, 220 Hz under twice it: not measured, saying why.
            ms::Request brief = r;
            brief.holdSeconds = 0.1f;
            const ms::Result low = measured(pg::singleFile(kPassEffect), rate, brief);
            const auto why = low.unmeasured.find("off_order_db");
            P.eq("orders.base_too_low_" + tag,
                 low.values.count("off_order_db") == 0 && why != low.unmeasured.end()
                         && has(why->second, "every frequency is within 120 Hz of a multiple of 220.00 Hz, the base being under twice "
                                             "that (6 bins of 0.05 s Blackman-Harris frames; a longer hold_seconds would narrow it")
                         && has(low.report, "Off the orders of 220 Hz in the sine's output: not measured, ") ? 1 : 0, 1);
        }
    }

    // ---- WOBBLE: wobble_db ---------------------------------------------------------------------------------------------
    {
        std::array<double, 2> moved{};
        for (const double rate : { 48000.0, 192000.0 })
        {
            const std::string tag = rate == 48000.0 ? "48k" : "192k";
            ms::Request r;
            r.holdSeconds = 1.5f;
            r.tailSeconds = 0.1f;
            r.want = { "wobble_db" };
            if constexpr (kInstrument)
            {
                r.notes = { 48 };
                // A steady sound, and one whose level moves with its shape (a tremolo of 3 Hz): 0. One whose harmonics
                // move as a comb sweeps over them (each 0.5 to 1.5 times, once a second): its shape moves.
                const double steady = at(measured(pg::singleFile(kHarmonicVoice), rate, r).values, "wobble_db");
                const double tremolo = at(measured(pg::singleFile(movingVoice(0.5, 0.0)), rate, r).values, "wobble_db");
                const ms::Result moving = measured(pg::singleFile(movingVoice(0.0, 0.5)), rate, r);
                const double noise = at(measured(pg::withLibraryIncludes(pg::singleFile(kNoiseVoice)), rate, r).values, "wobble_db");
                moved[rate == 48000.0 ? 0 : 1] = at(moving.values, "wobble_db");
                std::printf("wobble at %s: steady %.3f dB, a tremolo %.3f dB, the harmonics moving %.3f dB, white noise "
                            "%.3f dB\n%s", tag.c_str(), steady, tremolo, at(moving.values, "wobble_db"), noise, moving.report.c_str());
                P.le("wobble.steady_" + tag, steady, 0.05);
                P.le("wobble.tremolo_" + tag, tremolo, 0.1);
                P.ge("wobble.moving_" + tag, at(moving.values, "wobble_db"), 1.0);
                P.eq("wobble.moving_line_" + tag, has(moving.report, "Wobble while held: each third-octave band's share of the power moves ")
                                                      && has(moving.report, " (its SD over 28 frames of 0.1 s, power-weighted over the bands "
                                                                            "within 30 dB of the loudest); the most, ")
                                                      ? 1 : 0, 1);
                P.le("wobble.noise_" + tag, noise, 1.0);
            }
            else
            {
                // A steady tone's output, frames on its period: an effect that holds still reads 0 however long it
                // remembers once settled (a pass, a fixed comb, a 20 ms echo, an allpass; a feedback comb whose tail is
                // still falling in the first frames about it), and so does a tremolo (its level moves, its shape does
                // not). A flanger (the comb's delay from 0.2 to 1.8 ms and back every 2 s):
                // its notches move, and the effects that hold still read under a tenth of it.
                const double pass = at(measured(pg::singleFile(kPassEffect), rate, r).values, "wobble_db");
                const double comb = at(measured(pg::singleFile(combEffect(0.0)), rate, r).values, "wobble_db");
                const double echo = at(measured(pg::singleFile(kEchoEffect), rate, r).values, "wobble_db");
                const double allpass = at(measured(pg::singleFile(kAllpassEffect), rate, r).values, "wobble_db");
                const double feedback = at(measured(pg::singleFile(kFeedbackEffect), rate, r).values, "wobble_db");
                const double tremolo = at(measured(pg::singleFile(kTremoloEffect), rate, r).values, "wobble_db");
                const ms::Result flanger = measured(pg::singleFile(combEffect(0.8)), rate, r);
                const double flanged = at(flanger.values, "wobble_db");
                moved[rate == 48000.0 ? 0 : 1] = flanged;
                std::printf("wobble at %s: passed %.4f dB, a fixed comb %.4f dB, an echo %.4f dB, an allpass %.4f dB, a feedback "
                            "comb %.4f dB, a tremolo %.4f dB, a flanger %.3f dB\n%s", tag.c_str(), pass, comb, echo, allpass, feedback,
                            tremolo, flanged, flanger.report.c_str());
                P.le("wobble.pass_" + tag, pass, 0.01);
                P.le("wobble.comb_" + tag, comb, 0.01);
                P.le("wobble.echo_" + tag, echo, 0.05);
                P.le("wobble.allpass_" + tag, allpass, 0.05);
                P.le("wobble.feedback_" + tag, feedback, 0.1);
                P.le("wobble.tremolo_" + tag, tremolo, 0.1);
                P.ge("wobble.flanger_" + tag, flanged, 0.7);
                P.le("wobble.still_under_flanger_" + tag, std::max(echo, allpass), 0.1 * flanged);
                // An effect that silences its input: no frame sounds, and the reason says so.
                const ms::Result mute = measured(pg::singleFile(kMuteEffect), rate, r);
                const auto muted = mute.unmeasured.find("wobble_db");
                P.eq("wobble.silent_" + tag, mute.values.count("wobble_db") == 0 && muted != mute.unmeasured.end()
                                                 && has(muted->second, "the output is silent in 14 of its 14 frames, and fewer than "
                                                                       "three sound") ? 1 : 0, 1);
                P.eq("wobble.flanger_line_" + tag,
                     has(flanger.report, "Steady tone (partials every 10 Hz, flat) at -18.0 dBFS RMS for 1.50 s: each third-octave "
                                         "band's share of the output's power moves ")
                         && has(flanger.report, " (its SD over 14 frames of 0.1 s, on the tone's period, power-weighted over the "
                                                "bands within 30 dB of the loudest); the most, ") ? 1 : 0, 1);
            }
            // A held part under 0.2 s holds fewer than three frames: not measured, saying why (an instrument's note
            // held 0.15 s makes two; an effect's held part, from 0.05 s, one whole period of the tone).
            ms::Request brief = r;
            brief.holdSeconds = 0.2f;
            const ms::Result b = measured(pg::singleFile(kInstrument ? kHarmonicVoice : kPassEffect), rate, brief);
            const auto why = b.unmeasured.find("wobble_db");
            P.eq("wobble.too_short_" + tag,
                 b.values.count("wobble_db") == 0 && why != b.unmeasured.end()
                         && has(why->second, kInstrument ? "the held part makes fewer than three 0.1 s frames (each note's held stretch "
                                                           "makes its own, half a frame apart: three take one over 0.15 s"
                                                         : "the held part holds fewer than three whole 0.1 s periods of the tone (a frame "
                                                           "each, from its start: hold_seconds 0.4 holds three)")
                         && has(b.report, kInstrument ? "Wobble: not measured, " : "wobble not measured, ") ? 1 : 0, 1);
        }
        P.near("wobble.rates", moved[1] / moved[0], 1.0, kInstrument ? 0.05 : 0.15);
    }

    // ---- LEVEL: level_wobble_db ------------------------------------------------------------------------------------
    {
        std::array<double, 2> trembled{};
        for (const double rate : { 48000.0, 192000.0 })
        {
            const std::string tag = rate == 48000.0 ? "48k" : "192k";
            ms::Request r;
            r.holdSeconds = 1.5f;
            r.tailSeconds = 0.1f;
            r.want = { "level_wobble_db" };
            // A steady sound 0; a tremolo (3 Hz) high; a slow fade (6 dB a second, a straight line in dB) about 0:
            // the trend takes it.
            const ms::Result steady = measured(pg::singleFile(kInstrument ? kHarmonicVoice : kPassEffect), rate, r);
            const ms::Result tremolo = measured(pg::singleFile(kInstrument ? movingVoice(0.5, 0.0) : std::string(kTremoloEffect)), rate, r);
            const ms::Result fade = measured(pg::singleFile(kInstrument ? fadingVoice(6.0) : fadingEffect(6.0)), rate, r);
            const double s0 = at(steady.values, "level_wobble_db"), t0 = at(tremolo.values, "level_wobble_db");
            const double f0 = at(fade.values, "level_wobble_db");
            trembled[rate == 48000.0 ? 0 : 1] = t0;
            std::printf("level wobble at %s: steady %.4f dB, a tremolo %.3f dB, a fade of 6 dB a second %.4f dB\n%s", tag.c_str(), s0,
                        t0, f0, tremolo.report.c_str());
            P.le("level.steady_" + tag, s0, 0.05);
            P.ge("level.tremolo_" + tag, t0, 1.0);
            P.le("level.fade_" + tag, f0, 0.05);
            P.eq("level.line_" + tag, has(tremolo.report, kInstrument ? "Its level moves " : "Its output's level moves ")
                                          && has(tremolo.report, " about its trend (the SD of ")
                                          && has(tremolo.report, " frames' levels, each against a line fit within 0.5 s of it; the most, ")
                                          ? 1 : 0, 1);
            // Too short a held part: not measured, with wobble_db's reason.
            ms::Request brief = r;
            brief.holdSeconds = 0.2f;
            const ms::Result b = measured(pg::singleFile(kInstrument ? kHarmonicVoice : kPassEffect), rate, brief);
            const auto why = b.unmeasured.find("level_wobble_db");
            P.eq("level.too_short_" + tag, b.values.count("level_wobble_db") == 0 && why != b.unmeasured.end() ? 1 : 0, 1);
        }
        P.near("level.rates", trembled[1] / trembled[0], 1.0, 0.05);
    }

    // ---- LINE: the report ends with the metrics measured, name=value ---------------------------------------------------
    {
        ms::Request r;
        r.holdSeconds = 0.5f;
        r.tailSeconds = 0.5f;
        if constexpr (kInstrument)
            r.notes = { 48 };
        const ms::Result res = measured(pg::singleFile(kInstrument ? kHarmonicVoice : kDarkEffect), 48000.0, r);
        const std::size_t a = res.report.rfind("Metrics: ");
        const std::string line = a == std::string::npos ? std::string() : res.report.substr(a + 9, res.report.size() - a - 10);
        std::printf("the metrics line: %s\n", line.c_str());
        P.eq("line.last", a != std::string::npos && res.report.find('\n', a) == res.report.size() - 1 ? 1 : 0, 1);
        int agree = 0, said = 0;
        std::string order;
        for (std::size_t p = 0; p < line.size();)
        {
            std::size_t q = line.find(' ', p);
            if (q == std::string::npos)
                q = line.size();
            const std::string entry = line.substr(p, q - p);
            const std::size_t eq = entry.find('=');
            if (eq != std::string::npos)
            {
                const std::string name = entry.substr(0, eq), text = entry.substr(eq + 1);
                const double v = text == "inf" ? std::numeric_limits<double>::infinity() : std::strtod(text.c_str(), nullptr);
                const double want = at(res.values, name.c_str());
                ++said;
                agree += (std::isinf(want) && v == want) || std::fabs(v - want) <= std::max(0.051, 1e-3 * std::fabs(want)) ? 1 : 0;
                order += name + " ";
            }
            p = q + 1;
        }
        std::string table;
        for (const ms::MetricInfo& m : ms::metrics())
            if (res.values.count(m.name) != 0)
                table += std::string(m.name) + " ";
        P.eq("line.every_value", said, static_cast<int64_t>(res.values.size()));
        P.eq("line.values_agree", agree, static_cast<int64_t>(res.values.size()));
        P.eq("line.metrics_order", order == table ? 1 : 0, 1);
    }

    // ---- TERMS ---------------------------------------------------------------------------------------------------------
    {
        const std::string terms = ms::metricTerms();
        P.eq("terms.defined", has(terms, "Held: ") && has(terms, "The spectrum: ") && has(terms, "The band: 20 Hz to 20 kHz")
                                  && has(terms, "0.1 s Hann") ? 1 : 0, 1);
        int agree = 0, spans = 0;
        for (const ms::MetricInfo& mi : ms::metrics())
        {
            // The tolerance in words, against Goals' for a target of 1000 (and of 0, a relative one's least).
            ms::Goal g;
            g.metric = mi.name;
            g.hasTarget = true;
            g.target = 1000.0;
            const double far = ms::toleranceOf(g);
            g.target = 0.0;
            const double least = ms::toleranceOf(g);
            const std::string text = ms::toleranceText(mi);
            const double said = std::strtod(text.c_str() + 2, nullptr);
            const std::size_t floorAt = text.find("at least ");
            const double saidLeast = floorAt == std::string::npos ? said : std::strtod(text.c_str() + floorAt + 9, nullptr);
            const bool ok = mi.relative ? std::fabs(far - said / 100.0 * 1000.0) < 1e-9 && std::fabs(least - saidLeast) < 1e-12
                                        : std::fabs(far - said) < 1e-12 && std::fabs(least - said) < 1e-12;
            agree += ok ? 1 : 0;
            if (!ok)
                std::printf("tolerance of %s: %s, Goals %g and %g\n", mi.name, text.c_str(), far, least);
            // Its span named: the held part (the spectrum's), a note or a signal of its own, the render, a sample.
            const std::string w = mi.what;
            bool named = false;
            for (const char* word : { "held", "render", "note-o", "sample", "silence", "spectrum", "the same of", "expected pitch" })
                named = named || has(w, word);
            spans += named ? 1 : 0;
            if (!named)
                std::printf("no span in %s: %s\n", mi.name, mi.what);
        }
        P.eq("terms.tolerances_are_goals", agree, static_cast<int64_t>(ms::metrics().size()));
        P.eq("terms.every_span", spans, static_cast<int64_t>(ms::metrics().size()));
    }
    return P.finish();
}
