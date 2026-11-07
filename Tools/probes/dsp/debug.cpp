// PROBE name=dsp.debug timeout=240
//
// dsp.debug: trace (dsp/Debug.h) on programs with planted bugs, for each product's kind (a Voice for the synth, an
// Effect for the effect), at 48 kHz:
//   NAN     a state-variable filter with no clamp below the Nyquist frequency (the synth's cutoff 50 times the note's
//           frequency, so note 72's is above it; the effect's cutoff dial moved past it): a problem "not a number"
//           after that moment (the voice of note 72), whose first culprit is one of the filter's states, written at
//           the line that assigns it; find any also finds the runaway before it
//   CLICK   a phase that starts again now and then (every 10007 samples: the bug): a click there, its culprit the phase,
//           with the line that resets it among its writers
//   LOCAL   a division by zero from sample 9000 into the output alone: not a number there, no field gone bad, and the
//           verdict says the NaN lives in a local
//   CLEAN   the product's first example: nothing found, and the self-check's 1-sample calls give the same bits
//   SPLIT   a level smoothed once a call: the self-check says the output depends on how the calls are split
//   WATCH   fields by path and glob: a line each, min to max and a sparkline; a path no field has is said
//   LIST    the field map names the held module's fields; RAW: points around a moment give a sample each
//   VOICE   the synth's NAN traced in the voice of note 48 alone finds nothing
//   SAME    the same request twice gives the same lines
//   SILENCE a voice that ends while its key is held (the synth), a gain stepped to 0 (the effect): found, and why
//   DC      a constant added: found near the start
//   FROMSTART a sentinel past 1e6 that init sets: no runaway and no note (its own size), the click after it still found
//   STEADY  a field large from the start (a pressure about 2e6): no runaway; GROWS the same field from 100, growing
//           0.1 % a sample: a runaway where it passes 1e6 (sample 9215), its culprit the field
//   RENDER  trace's render with notes, a velocity, the times and a dial asked for: measure_output's (the synth's) or
//           view_sound's (the effect's) to the bit, and not the render without the dial
//   BUFFER  a delay line fed back at 1.1: the runaway's and the NaN's verdicts name the buffer trace does not read
//   TIE     two fields bad in the same sample named together; ATTACK (the synth) a note-on's click is the note-on's
//   LOWSAW  a saw at note 24, whose wrap comes every third window: no click
//   SHARED  a module Shared's field listed, and watched as shared->gain
//   STOP    a cancel in pass 1 (the NaN found so far listed, not traced), one in pass 2, the time limit: each says so;
//           stoppedFirst for pass 1's and the time limit's (the render the part before), not for pass 2's (pass 1's
//           render whole, its hash the one a trace never stopped gives)
//   NAN0    a NaN from the first sample: the self-check says it had nothing to compare
//   DOUBLE  a double accumulator (90 + 1e-6 a sample) and its float twin, a 2-D grid of 2 x 3 and one of 4 x 16, and a
//           field's attributes: listed as a double with its label, unit and range, the grid's rows by its two indexes
//           and the large one left out with its shape; watched, the double moves (to 12 digits, as the program sums it)
//           where the float stays at 90, and an element of the grid is read where the program put it; its raw values,
//           evenly and a sample each around a moment, keep its 1e-6 steps
//   GRID    CLICK's phase reset with the phase a double in a 2-D array: the element is the culprit, with the line that
//           writes it by its two indexes, its values a double's; a double beside it that moves 1e-6 a sample is no
//           culprit
//   LONG    CLICK's counter watched over a render of 300 s held (a call that returns numbers: Measure.h, "How long a
//           render may be"): not stopped, its range whole (0 to 10006), in a time printed and bounded
//   EXPORT  trace's export (Export.h): SHA-256's own test vectors (FIPS 180-4's, a million a's in odd pieces); CLICK's
//           counter logged every 160 samples (a row each, the count the samples played: the voice's while it sounds,
//           empty after), no find unless asked, the effect's input kept (the sine as it went in); the render hash the
//           samples' and in the lines, the same twice and another for another note (the effect's noise), an
//           instrument's measure_output's (Result::hash) and an effect's view_sound's; the WAV float32 read back to the
//           bit, its data chunk hashing to the render hash, 24-bit within half a step and past full scale clipped and
//           counted; the CSV's header and rows, a float to 9 digits and a double to 17 reading back to their bits, 6
//           for the inline form; evenRows
#include "Probe.h"

#include "Examples.h"
#include "Product.h"

#include "dsp/Debug.h"
#include "dsp/Export.h"
#include "dsp/SoundView.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace pg = playground;
namespace ms = playground::measure;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;

    const char* const kNanVoice = R"(module Svf
{
    float g;
    float k;
    float ic1;
    float ic2;

    void set(float fc, float rate)
    {
        const float w = 3.14159265f * fc / rate;
        g = sinf(w) / cosf(w);
        k = 1.4f;
    }

    float tick(float x)
    {
        const float a1 = 1 / (1 + g * (g + k));
        const float a2 = g * a1;
        const float a3 = g * a2;
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        return v2;
    }
};

module Voice
{
    float rate;
    float phase;
    float step;
    float fc;
    bool held;
    Svf filter;

    void init(float r)
    {
        rate = r;
    }

    void start(float note, float vel)
    {
        step = 440 * expf(0.0577622650f * (note - 69)) / rate;
        fc = 50 * step * rate;
        held = true;
    }

    void stop()
    {
        held = false;
    }

    bool process(float* restrict left, float* restrict right, int n)
    {
        filter.set(fc, rate);
        for (int i = 0; i < n; ++i)
        {
            const float saw = 2 * phase - 1;
            phase += step;
            if (phase >= 1)
                phase -= 1;
            const float y = 0.2f * filter.tick(saw);
            left[i] += y;
            right[i] += y;
        }
        return held;
    }
};
)";

    const char* const kNanEffect = R"(module Svf
{
    float g;
    float k;
    float ic1;
    float ic2;

    void set(float fc, float rate)
    {
        const float w = 3.14159265f * fc / rate;
        g = sinf(w) / cosf(w);
        k = 1.4f;
    }

    float tick(float x)
    {
        const float a1 = 1 / (1 + g * (g + k));
        const float a2 = g * a1;
        const float a3 = g * a2;
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        return v2;
    }
};

module Effect
{
    param float cutoff [[fminus::range(20, 30000)]] [[fminus::default(1000)]];
    float rate;
    Svf l;
    Svf r;

    void init(float sampleRate)
    {
        rate = sampleRate;
    }

    void process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            l.set(cutoff[i], rate);
            r.set(cutoff[i], rate);
            left[i] = l.tick(left[i]);
            right[i] = r.tick(right[i]);
        }
    }
};
)";

    const char* const kClickVoice = R"(module Voice
{
    float rate;
    float phase;
    float step;
    float level;
    int count;
    bool held;

    void init(float r)
    {
        rate = r;
    }

    void start(float note, float vel)
    {
        step = 440 * expf(0.0577622650f * (note - 69)) / rate;
        level = 0.3f;
        held = true;
    }

    void stop()
    {
        held = false;
    }

    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            count += 1;
            if (count >= 10007)
            {
                count = 0;
                phase = 0.75f;
            }
            if (!held)
                level *= 0.999f;
            const float y = level * sinf(6.28318531f * phase);
            phase += step;
            if (phase >= 1)
                phase -= 1;
            left[i] += y;
            right[i] += y;
        }
        return held || level > 0.0001f;
    }
};
)";

    const char* const kClickEffect = R"(module Effect
{
    float rate;
    float phase;
    int count;

    void init(float r)
    {
        rate = r;
    }

    void process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            count += 1;
            if (count >= 10007)
            {
                count = 0;
                phase = 0.25f;
            }
            const float gain = 0.5f + 0.5f * sinf(6.28318531f * phase);
            phase += 3 / rate;
            if (phase >= 1)
                phase -= 1;
            left[i] *= gain;
            right[i] *= gain;
        }
    }
};
)";

    const char* const kLocalVoice = R"(module Voice
{
    float rate;
    float phase;
    float step;
    int count;
    bool held;

    void init(float r)
    {
        rate = r;
    }

    void start(float note, float vel)
    {
        step = 440 * expf(0.0577622650f * (note - 69)) / rate;
        held = true;
    }

    void stop()
    {
        held = false;
    }

    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            count += 1;
            const float d = count < 9000 ? 1.0f : 0.0f;
            const float y = 0.2f * sinf(6.28318531f * phase) / d;
            phase += step;
            if (phase >= 1)
                phase -= 1;
            left[i] += y;
            right[i] += y;
        }
        return held;
    }
};
)";

    const char* const kLocalEffect = R"(module Effect
{
    int count;

    void process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            count += 1;
            const float d = count < 9000 ? 1.0f : 0.0f;
            left[i] = left[i] / d;
            right[i] = right[i] / d;
        }
    }
};
)";

    const char* const kSplitVoice = R"(module Voice
{
    float rate;
    float phase;
    float step;
    float level;
    bool held;

    void init(float r)
    {
        rate = r;
    }

    void start(float note, float vel)
    {
        step = 440 * expf(0.0577622650f * (note - 69)) / rate;
        held = true;
    }

    void stop()
    {
        held = false;
    }

    bool process(float* restrict left, float* restrict right, int n)
    {
        level += 0.05f * (0.3f - level);
        for (int i = 0; i < n; ++i)
        {
            const float y = level * sinf(6.28318531f * phase);
            phase += step;
            if (phase >= 1)
                phase -= 1;
            left[i] += y;
            right[i] += y;
        }
        return held;
    }
};
)";

    const char* const kSplitEffect = R"(module Effect
{
    float level;

    void process(float* restrict left, float* restrict right, int n)
    {
        level += 0.05f * (1 - level);
        for (int i = 0; i < n; ++i)
        {
            left[i] *= level;
            right[i] *= level;
        }
    }
};
)";

    // A voice that ends (process returns false) at sample 9008, a call's end on the grid, while its key is held.
    const char* const kSilenceVoice = R"(module Voice
{
    float rate;
    float phase;
    float step;
    int count;
    bool held;

    void init(float r)
    {
        rate = r;
    }

    void start(float note, float vel)
    {
        step = 440 * expf(0.0577622650f * (note - 69)) / rate;
        held = true;
    }

    void stop()
    {
        held = false;
    }

    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            count += 1;
            const float y = 0.3f * sinf(6.28318531f * phase);
            phase += step;
            if (phase >= 1)
                phase -= 1;
            left[i] += y;
            right[i] += y;
        }
        return held && count < 9008;
    }
};
)";

    const char* const kSilenceEffect = R"(module Effect
{
    float gain;
    int count;

    void init(float r)
    {
        gain = 1;
    }

    void process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            count += 1;
            if (count == 9000)
                gain = 0;
            left[i] *= gain;
            right[i] *= gain;
        }
    }
};
)";

    const char* const kDcVoice = R"(module Voice
{
    float rate;
    float phase;
    float step;
    bool held;

    void init(float r)
    {
        rate = r;
    }

    void start(float note, float vel)
    {
        step = 440 * expf(0.0577622650f * (note - 69)) / rate;
        held = true;
    }

    void stop()
    {
        held = false;
    }

    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const float y = 0.2f * sinf(6.28318531f * phase) + 0.1f;
            phase += step;
            if (phase >= 1)
                phase -= 1;
            left[i] += y;
            right[i] += y;
        }
        return held;
    }
};
)";

    const char* const kDcEffect = R"(module Effect
{
    void process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            left[i] = 0.5f * left[i] + 0.1f;
            right[i] = 0.5f * right[i] + 0.1f;
        }
    }
};
)";

    // A delay line of 10 samples fed back at 1.1: the state that grows is in the buffer, which trace does not read.
    const char* const kLine = R"(module Line
{
    float* buffer;
    int length;
    int at;

    void allocate(int n)
    {
        buffer = calloc(n, sizeof(float));
        length = buffer ? n : 0;
        at = 0;
    }

    void release()
    {
        if (length > 0)
            free(buffer);
        length = 0;
    }

    float read()
    {
        return length > 0 ? buffer[at] : 0;
    }

    void write(float x)
    {
        if (length == 0)
            return;
        buffer[at] = x;
        at += 1;
        if (at == length)
            at = 0;
    }
};
)";

    const char* const kBufferVoice = R"(module Voice
{
    float rate;
    float phase;
    float step;
    bool held;
    Line line;

    void init(float r)
    {
        rate = r;
        line.allocate(10);
    }

    void end()
    {
        line.release();
    }

    void start(float note, float vel)
    {
        step = 440 * expf(0.0577622650f * (note - 69)) / rate;
        held = true;
    }

    void stop()
    {
        held = false;
    }

    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const float x = 0.1f * sinf(6.28318531f * phase);
            phase += step;
            if (phase >= 1)
                phase -= 1;
            const float y = line.read();
            line.write(x + 1.1f * y);
            left[i] += y;
            right[i] += y;
        }
        return held;
    }
};
)";

    const char* const kBufferEffect = R"(module Effect
{
    Line line;

    void init(float r)
    {
        line.allocate(10);
    }

    void end()
    {
        line.release();
    }

    void process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const float y = line.read();
            line.write(0.5f * left[i] + 1.1f * y);
            left[i] = y;
            right[i] = y;
        }
    }
};
)";

    // Two fields that go bad in the same sample (b is computed from a).
    const char* const kTieVoice = R"(module Voice
{
    float rate;
    float phase;
    float step;
    int count;
    float a;
    float b;
    bool held;

    void init(float r)
    {
        rate = r;
    }

    void start(float note, float vel)
    {
        step = 440 * expf(0.0577622650f * (note - 69)) / rate;
        held = true;
    }

    void stop()
    {
        held = false;
    }

    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            count += 1;
            const float d = count < 9000 ? 1.0f : 0.0f;
            a = (0.2f * sinf(6.28318531f * phase) + 0.01f) / d;
            b = a * 0.5f;
            phase += step;
            if (phase >= 1)
                phase -= 1;
            left[i] += b;
            right[i] += b;
        }
        return held;
    }
};
)";

    const char* const kTieEffect = R"(module Effect
{
    int count;
    float a;
    float b;

    void process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            count += 1;
            const float d = count < 9000 ? 1.0f : 0.0f;
            a = (left[i] + 0.01f) / d;
            b = a * 0.5f;
            left[i] = b;
            right[i] = b;
        }
    }
};
)";

    // A voice that starts at once at its level (its phase where the sine is at its top): the second note's start
    // clicks, and every field its start() sets jumps with it.
    [[maybe_unused]] const char* const kAttackVoice = R"(module Voice
{
    float rate;
    float phase;
    float step;
    float level;
    bool held;

    void init(float r)
    {
        rate = r;
    }

    void start(float note, float vel)
    {
        step = 440 * expf(0.0577622650f * (note - 69)) / rate;
        level = 0.3f;
        phase = 0.25f;
        held = true;
    }

    void stop()
    {
        held = false;
    }

    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            if (!held)
                level *= 0.999f;
            const float y = level * sinf(6.28318531f * phase);
            phase += step;
            if (phase >= 1)
                phase -= 1;
            left[i] += y;
            right[i] += y;
        }
        return held || level > 0.0001f;
    }
};
)";

    // A plain saw at note 24 (32.7 Hz): its wrap comes every 3 windows of the click finder's, and is no click.
    const char* const kLowSawVoice = R"(module Voice
{
    float rate;
    float phase;
    float step;
    float level;
    bool held;

    void init(float r)
    {
        rate = r;
    }

    void start(float note, float vel)
    {
        step = 440 * expf(0.0577622650f * (note - 69)) / rate;
        level = 0.2f;
        held = true;
    }

    void stop()
    {
        held = false;
    }

    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            if (!held)
                level *= 0.999f;
            const float y = level * (2 * phase - 1);
            phase += step;
            if (phase >= 1)
                phase -= 1;
            left[i] += y;
            right[i] += y;
        }
        return held || level > 0.0001f;
    }
};
)";

    const char* const kLowSawEffect = R"(module Effect
{
    float rate;
    float phase;

    void init(float r)
    {
        rate = r;
    }

    void process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const float y = 0.2f * (2 * phase - 1);
            phase += 32.7031957f / rate;
            if (phase >= 1)
                phase -= 1;
            left[i] = y;
            right[i] = y;
        }
    }
};
)";

    // DOUBLE: a double that counts what a float's 24 bits round away, its float twin, a small grid and a large one.
    const char* const kDoubleVoice = R"(module Voice
{
    float rate;
    float phase;
    float step;
    bool held;
    double temp [[ui::unit("C")]] [[ui::label("TEMP")]] [[fminus::range(80, 100)]];
    float ftemp;
    float gain[2][3];
    float bank[4][16];

    void init(float r)
    {
        rate = r;
    }

    void start(float note, float vel)
    {
        step = 440 * expf(0.0577622650f * (note - 69)) / rate;
        held = true;
        temp = 90;
        ftemp = 90;
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 3; ++col)
                gain[row][col] = row + 0.25f * col;
    }

    void stop()
    {
        held = false;
    }

    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            temp += 1e-6;
            ftemp += 1e-6f;
            bank[i & 3][i & 15] = (float)temp;
            const float y = 0.2f * gain[1][2] * sinf(6.28318531f * phase);
            phase += step;
            if (phase >= 1)
                phase -= 1;
            left[i] += y;
            right[i] += y;
        }
        return held;
    }
};
)";

    const char* const kDoubleEffect = R"(module Effect
{
    double temp [[ui::unit("C")]] [[ui::label("TEMP")]] [[fminus::range(80, 100)]];
    float ftemp;
    float gain[2][3];
    float bank[4][16];

    void init(float r)
    {
        temp = 90;
        ftemp = 90;
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 3; ++col)
                gain[row][col] = row + 0.25f * col;
    }

    void process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            temp += 1e-6;
            ftemp += 1e-6f;
            bank[i & 3][i & 15] = (float)temp;
            left[i] *= 0.5f * gain[1][2];
            right[i] *= 0.5f * gain[1][2];
        }
    }
};
)";

    // GRID: CLICK's phase reset (at sample 10005), the phase a double in a 2-D array (written ph[1][2] = ...) that adds
    // a float step a sample, beside a double that moves 1e-6 a sample from 90: rounded to a float it would stand still
    // for 7 or 8 samples and step one ulp (7.6e-6), and it does in sample 10005, which would make it a culprit.
    const char* const kGridVoice = R"(module Voice
{
    float rate;
    float level;
    int count;
    bool held;
    double ph[2][3];
    double temp;

    void init(float r)
    {
        rate = r;
    }

    void start(float note, float vel)
    {
        level = 0.3f;
        held = true;
        temp = 90;
    }

    void stop()
    {
        held = false;
    }

    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            count += 1;
            temp += 1e-6;
            if (count >= 10006)
            {
                count = 0;
                ph[1][2] = 0.75;
            }
            if (!held)
                level *= 0.999f;
            const float y = level * sinf(6.28318531f * (float)ph[1][2]);
            ph[1][2] += 0.003f;
            if (ph[1][2] >= 1)
                ph[1][2] -= 1;
            left[i] += y;
            right[i] += y;
        }
        return held || level > 0.0001f;
    }
};
)";

    const char* const kGridEffect = R"(module Effect
{
    float rate;
    int count;
    double ph[2][3];
    double temp;

    void init(float r)
    {
        rate = r;
        temp = 90;
    }

    void process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            count += 1;
            temp += 1e-6;
            if (count >= 10006)
            {
                count = 0;
                ph[1][2] = 0.25;
            }
            const float gain = 0.5f + 0.5f * sinf(6.28318531f * (float)ph[1][2]);
            ph[1][2] += 0.0000625f;
            if (ph[1][2] >= 1)
                ph[1][2] -= 1;
            left[i] *= gain;
            right[i] *= gain;
        }
    }
};
)";

    const char* const kSharedPart = R"(module Shared
{
    float gain;

    void init(float r)
    {
        gain = 0.25f;
    }
};
)";

    const char* const kSharedVoice = R"(module Voice
{
    Shared* shared;
    float rate;
    float phase;
    float step;
    bool held;

    void init(float r)
    {
        rate = r;
    }

    void start(float note, float vel)
    {
        step = 440 * expf(0.0577622650f * (note - 69)) / rate;
        held = true;
    }

    void stop()
    {
        held = false;
    }

    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const float y = shared->gain * sinf(6.28318531f * phase);
            phase += step;
            if (phase >= 1)
                phase -= 1;
            left[i] += y;
            right[i] += y;
        }
        return held;
    }
};
)";

    const char* const kSharedEffect = R"(module Effect
{
    Shared* shared;

    void process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            left[i] *= shared->gain;
            right[i] *= shared->gain;
        }
    }
};
)";

    // A state large from the start (a cylinder's pressure in pascals, about 2e6) that stays so: no runaway. With
    // `grows`, it starts at 100 and grows by 0.1 % a sample instead, past 1e6 at about 0.19 s: a runaway.
    std::string pressureProgram(bool grows)
    {
        const std::string set = grows ? "pressure = 100;" : "pressure = 2000000;";
        const std::string tick = grows ? "pressure *= 1.001f;" : "pressure = 2000000 + 100000 * sinf(6.28318531f * phase);";
        if constexpr (kInstrument)
            return "module Voice\n{\n    float rate;\n    float phase;\n    float step;\n    float pressure;\n    bool held;\n\n"
                   "    void init(float r)\n    {\n        rate = r;\n    }\n\n"
                   "    void start(float note, float vel)\n    {\n        step = 440 * expf(0.0577622650f * (note - 69)) / rate;\n        "
                 + set + "\n        held = true;\n    }\n\n"
                   "    void stop()\n    {\n        held = false;\n    }\n\n"
                   "    bool process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        for (int i = 0; i < n; ++i)\n        {\n            phase += step;\n            if (phase >= 1)\n"
                   "                phase -= 1;\n            " + tick + "\n"
                   "            const float y = 0.1f * sinf(6.28318531f * phase);\n            left[i] += y;\n            right[i] += y;\n"
                   "        }\n        return held;\n    }\n};\n";
        else
            return "module Effect\n{\n    float rate;\n    float phase;\n    float pressure;\n\n"
                   "    void init(float r)\n    {\n        rate = r;\n        " + set + "\n    }\n\n"
                   "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                   "        for (int i = 0; i < n; ++i)\n        {\n            phase += 220 / rate;\n            if (phase >= 1)\n"
                   "                phase -= 1;\n            " + tick + "\n"
                   "            left[i] *= 0.5f;\n            right[i] *= 0.5f;\n        }\n    }\n};\n";
    }

    // `program` with a field init sets to a sentinel past 1e6 (lastFc = 1e9), harmless: process never reads it.
    std::string withSentinel(std::string program)
    {
        const auto put = [&](const std::string& after, const std::string& line) {
            const std::size_t at = program.find(after);
            if (at != std::string::npos)
                program.insert(at + after.size(), line);
        };
        put("    float rate;\n", "    float lastFc;\n");
        put("        rate = r;\n", "        lastFc = 1e9f;\n");
        return program;
    }

    // The 1-based line of the first line holding `text` ("program.fm:N"), "" when none does.
    std::string lineOf(const std::string& program, const std::string& text)
    {
        int line = 1;
        for (std::size_t at = 0; at < program.size();)
        {
            const std::size_t end = program.find('\n', at);
            const std::string l = program.substr(at, end == std::string::npos ? std::string::npos : end - at);
            if (l.find(text) != std::string::npos)
                return std::string(pg::kProgramName) + ":" + std::to_string(line);
            if (end == std::string::npos)
                break;
            at = end + 1;
            ++line;
        }
        return {};
    }

    bool has(const std::vector<std::string>& v, const std::string& s)
    {
        for (const std::string& x : v)
            if (x == s)
                return true;
        return false;
    }

    bool anyHas(const std::vector<std::string>& lines, const std::string& s)
    {
        for (const std::string& x : lines)
            if (x.find(s) != std::string::npos)
                return true;
        return false;
    }

    void print(const char* what, const ms::TraceResult& r)
    {
        std::printf("---- %s%s\n", what, r.error.empty() ? "" : (": " + r.error).c_str());
        for (const std::string& l : r.lines)
            std::printf("%s\n", l.c_str());
    }

    [[maybe_unused]] ms::PhraseNote note(int n, double at, double len)   // the synth's
    {
        ms::PhraseNote p;
        p.note = n;
        p.at = at;
        p.length = len;
        return p;
    }

    const ms::TraceProblem* firstOf(const ms::TraceResult& r, ms::TraceFind kind)
    {
        for (const ms::TraceProblem& p : r.problems)
            if (p.kind == kind)
                return &p;
        return nullptr;
    }
}

PROBE(dsp, debug)
{
    const double rate = 48000.0;

    // ---- NAN -----------------------------------------------------------------------------------------------------------
    const std::string nanProgram = kInstrument ? kNanVoice : kNanEffect;
    ms::TraceRequest nanReq;
    nanReq.find = ms::TraceFind::nan;
    if constexpr (kInstrument)
    {
        nanReq.render.phrase = { note(48, 0.0, 0.3), note(72, 0.3, 0.5) };
        nanReq.render.tailSeconds = 0.3f;
    }
    else
    {
        nanReq.render.holdSeconds = 1.0f;
        nanReq.render.tailSeconds = 0.2f;
        nanReq.render.automate = { ms::DialMove{ "cutoff", 0.3, 30000.0f, 0.0 } };
        nanReq.input = static_cast<int>(ms::ViewInput::noise);
    }
    const ms::TraceResult nan = ms::trace(pg::singleFile(nanProgram), nanReq);
    print("NAN", nan);
    const ms::TraceProblem* n0 = firstOf(nan, ms::TraceFind::nan);
    P.eq("nan.found", n0 != nullptr ? 1 : 0, 1);
    P.ge("nan.after_the_cause_s", n0 != nullptr ? static_cast<double>(n0->at) / rate : -1.0, 0.3);
    if constexpr (kInstrument)
        P.eq("nan.voice_note", n0 != nullptr ? n0->note : -1, 72);
    // The first culprit is one of the filter's fields (its states, or its coefficient where the cutoff lands at the
    // Nyquist frequency), and its writer the line that assigns it.
    const std::string filter = kInstrument ? "filter." : "l.";
    const bool inFilter = n0 != nullptr && !n0->culprits.empty() && n0->culprits[0].path.rfind(filter, 0) == 0;
    P.eq("nan.culprit_in_the_filter", inFilter ? 1 : 0, 1);
    const std::string field = inFilter ? n0->culprits[0].path.substr(filter.size()) : std::string();
    const std::string written = field == "ic1" ? lineOf(nanProgram, "ic1 = 2 * v1 - ic1;")
                              : field == "ic2" ? lineOf(nanProgram, "ic2 = 2 * v2 - ic2;")
                              : field == "g"   ? lineOf(nanProgram, "g = sinf(w) / cosf(w);") : std::string("?");
    std::printf("nan: the first culprit %s, expected written at %s\n", inFilter ? n0->culprits[0].path.c_str() : "-", written.c_str());
    P.eq("nan.culprit_written_at", inFilter && has(n0->culprits[0].writes, written) ? 1 : 0, 1);
    P.eq("nan.values_around", inFilter && n0->culprits[0].before.size() == 4 && n0->culprits[0].after.size() == 4 ? 1 : 0, 1);
    // The first to go bad, or those that go bad in its sample named together (the effect's filters: l.ic1, l.ic2, r.ic1).
    P.eq("nan.verdict", n0 != nullptr && (n0->verdict.find("first value to go bad") != std::string::npos
                                          || n0->verdict.find("go bad in the same sample") != std::string::npos) ? 1 : 0, 1);
    P.le("nan.lines", static_cast<double>(nan.lines.size()), 20.0);
    {
        ms::TraceRequest any = nanReq;
        any.find = ms::TraceFind::any;
        const ms::TraceResult r = ms::trace(pg::singleFile(nanProgram), any);
        print("NAN any", r);
        const ms::TraceProblem* run = firstOf(r, ms::TraceFind::runaway);
        const ms::TraceProblem* bad = firstOf(r, ms::TraceFind::nan);
        P.eq("nan.any_finds_both", run != nullptr && bad != nullptr && run->at <= bad->at ? 1 : 0, 1);
        P.le("nan.any_lines", static_cast<double>(r.lines.size()), 20.0);
    }

    // ---- CLICK ---------------------------------------------------------------------------------------------------------
    const std::string clickProgram = kInstrument ? kClickVoice : kClickEffect;
    ms::TraceRequest clickReq;
    clickReq.find = ms::TraceFind::click;
    if constexpr (kInstrument)
    {
        clickReq.render.phrase = { note(48, 0.0, 0.5) };
        clickReq.render.tailSeconds = 0.3f;
    }
    else
    {
        clickReq.render.holdSeconds = 0.5f;
        clickReq.render.tailSeconds = 0.1f;
        clickReq.render.signal = static_cast<int>(ms::ViewInput::sine);
    }
    const ms::TraceResult click = ms::trace(pg::singleFile(clickProgram), clickReq);
    print("CLICK", click);
    const ms::TraceProblem* c0 = click.problems.empty() ? nullptr : &click.problems.front();
    P.eq("click.found", c0 != nullptr && c0->kind == ms::TraceFind::click ? 1 : 0, 1);
    // The step comes at the 10007th sample the program plays (the voice from sample 0): sample 10006.
    P.near("click.at_sample", c0 != nullptr ? static_cast<double>(c0->at) : -1.0, 10006.0, 1.0);
    P.eq("click.culprit_phase", c0 != nullptr && !c0->culprits.empty() && c0->culprits[0].path == "phase" ? 1 : 0, 1);
    P.eq("click.reset_line", c0 != nullptr && !c0->culprits.empty()
                                 && has(c0->culprits[0].writes, lineOf(clickProgram, kInstrument ? "phase = 0.75f;" : "phase = 0.25f;")) ? 1 : 0, 1);
    P.ge("click.step_vs_usual", c0 != nullptr && !c0->culprits.empty() ? c0->culprits[0].step / std::max(c0->culprits[0].usual, 1e-12) : 0.0, 10.0);
    P.eq("click.self_check_same", !click.splitDiffers && anyHas(click.lines, "gave the same bits") ? 1 : 0, 1);

    // ---- LOCAL ---------------------------------------------------------------------------------------------------------
    const std::string localProgram = kInstrument ? kLocalVoice : kLocalEffect;
    ms::TraceRequest localReq;
    localReq.find = ms::TraceFind::any;
    if constexpr (kInstrument)
    {
        localReq.render.phrase = { note(60, 0.0, 0.5) };
        localReq.render.tailSeconds = 0.2f;
    }
    else
    {
        localReq.render.holdSeconds = 0.5f;
        localReq.render.tailSeconds = 0.1f;
        localReq.render.signal = static_cast<int>(ms::ViewInput::sine);
    }
    const ms::TraceResult local = ms::trace(pg::singleFile(localProgram), localReq);
    print("LOCAL", local);
    const ms::TraceProblem* l0 = firstOf(local, ms::TraceFind::nan);
    P.eq("local.found", l0 != nullptr ? 1 : 0, 1);
    P.near("local.at_sample", l0 != nullptr ? static_cast<double>(l0->at) : -1.0, 8999.0, 0.0);
    P.eq("local.no_field", l0 != nullptr && l0->culprits.empty() ? 1 : 0, 1);
    P.eq("local.says_local", l0 != nullptr && l0->verdict.find("lives in a local") != std::string::npos ? 1 : 0, 1);
    P.eq("local.one_problem", local.problems.size() == 1 ? 1 : 0, 1);   // the silence after it is the NaN's

    // ---- CLEAN ---------------------------------------------------------------------------------------------------------
    ms::TraceRequest cleanReq;
    cleanReq.find = ms::TraceFind::any;
    const ms::TraceResult clean = ms::trace(pg::singleFile(pg::kExamples[0].source), cleanReq);
    print("CLEAN", clean);
    P.eq("clean.nothing", clean.error.empty() && clean.problems.empty() && anyHas(clean.lines, "Nothing found") ? 1 : 0, 1);
    P.eq("clean.self_check_same", !clean.splitDiffers && anyHas(clean.lines, "gave the same bits") ? 1 : 0, 1);

    // ---- SPLIT ---------------------------------------------------------------------------------------------------------
    ms::TraceRequest splitReq;
    splitReq.find = ms::TraceFind::any;
    if constexpr (kInstrument)
        splitReq.render.phrase = { note(60, 0.0, 0.3) };
    else
    {
        splitReq.render.holdSeconds = 0.3f;
        splitReq.render.signal = static_cast<int>(ms::ViewInput::sine);
    }
    splitReq.render.tailSeconds = 0.1f;
    const ms::TraceResult split = ms::trace(pg::singleFile(kInstrument ? kSplitVoice : kSplitEffect), splitReq);
    print("SPLIT", split);
    P.eq("split.found", split.splitDiffers && anyHas(split.lines, "depends on how its calls are split") ? 1 : 0, 1);

    // ---- WATCH, LIST, RAW ----------------------------------------------------------------------------------------------
    ms::TraceRequest watchReq = clickReq;
    watchReq.find = ms::TraceFind::none;
    watchReq.fields = { "phase", "c*", "nothing.here" };
    const ms::TraceResult watch = ms::trace(pg::singleFile(clickProgram), watchReq);
    print("WATCH", watch);
    P.eq("watch.fields", watch.watched.size() == 2 && watch.watched[0] == "phase" && watch.watched[1] == "count" ? 1 : 0, 1);
    P.eq("watch.lines", anyHas(watch.lines, "phase: min ") && anyHas(watch.lines, "largest jump") && anyHas(watch.lines, "never non-finite") ? 1 : 0, 1);
    P.eq("watch.unknown_named", anyHas(watch.lines, "No field matches nothing.here") ? 1 : 0, 1);
    P.eq("watch.no_find", watch.problems.empty() && !anyHas(watch.lines, "Nothing found") ? 1 : 0, 1);

    ms::TraceRequest listReq;
    listReq.list = true;
    const ms::TraceResult list = ms::trace(pg::singleFile(nanProgram), listReq);
    print("LIST", list);
    P.eq("list.held_module", anyHas(list.lines, (kInstrument ? "filter.ic1 float" : "l.ic1 float")) && anyHas(list.lines, "rate float") ? 1 : 0, 1);
    P.le("list.lines", static_cast<double>(list.lines.size()), 60.0);
    P.eq("list.glob", ms::tracePathMatches(kInstrument ? "filter.*" : "l.*", kInstrument ? "filter.g" : "l.g")
                          && ms::tracePathMatches(kInstrument ? "filter" : "l", kInstrument ? "filter.ic2" : "l.ic2")
                          && !ms::tracePathMatches("phase", "phases") && ms::tracePathMatches("osc[?].p*", "osc[1].phase") ? 1 : 0, 1);

    ms::TraceRequest rawReq = clickReq;
    rawReq.find = ms::TraceFind::none;
    rawReq.fields = { "count" };
    rawReq.points = 8;
    rawReq.around = 0.1;
    const ms::TraceResult raw = ms::trace(pg::singleFile(clickProgram), rawReq);
    print("RAW", raw);
    P.eq("raw.around", anyHas(raw.lines, "count from 0.100 s (sample 4,796), a sample each: 4797 4798 4799 4800 4801 4802 4803 4804") ? 1 : 0, 1);

    // ---- LONG: a field watched over 300 s ---------------------------------------------------------------------------------
    {
        ms::TraceRequest q = clickReq;
        q.find = ms::TraceFind::none;
        q.fields = { "count" };
        q.render.longest = ms::kMaxLongSeconds;
        q.render.phrase.clear();
        q.render.holdSeconds = 300.0f;
        q.render.tailSeconds = 0.1f;
        if constexpr (kInstrument)
            q.render.notes = { 48 };
        const auto t0 = std::chrono::steady_clock::now();
        const ms::TraceResult r = ms::trace(pg::singleFile(clickProgram), q);
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("trace over 300 s, in %.1f s:\n", took);
        print("LONG", r);
        P.eq("long.watched_300s", r.error.empty() && !r.stopped && anyHas(r.lines, "count: min 0, max 10006")
                                      && static_cast<double>(r.l.size()) >= 300.0 * 48000.0 ? 1 : 0, 1);
        P.le("long.trace_seconds", took, 90.0);
    }

    // ---- EXPORT: the hash, the field log, the files' formats (Export.h) -------------------------------------------------
    {
        const auto sha = [](const std::string& text) {
            ms::Sha256 h;
            h.add(text.data(), text.size());
            return h.hex();
        };
        P.eq("export.sha256_abc", sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" ? 1 : 0, 1);
        P.eq("export.sha256_empty", sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" ? 1 : 0, 1);
        P.eq("export.sha256_two_blocks", sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
                                             == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" ? 1 : 0, 1);
        {
            ms::Sha256 h;                                     // a million a's, in pieces of 1 to 97 bytes
            const std::string a(97, 'a');
            std::size_t fed = 0;
            for (std::size_t k = 1; fed < 1000000; k = k % 97 + 1)
            {
                const std::size_t n = std::min(k, 1000000 - fed);
                h.add(a.data(), n);
                fed += n;
            }
            P.eq("export.sha256_million_pieces", h.hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0" ? 1 : 0, 1);
        }

        // CLICK's counter (one a sample, from 0, back to 0 at 10007) logged every 160 samples over 0.6 s.
        ms::TraceRequest q = clickReq;
        q.find = ms::TraceFind::none;
        q.fields = { "count" };
        q.exporting = true;
        q.every = 160;
        q.render.phrase.clear();
        q.render.holdSeconds = 0.5f;
        q.render.tailSeconds = 0.1f;
        if constexpr (kInstrument)
            q.render.notes = { 48 };
        const pg::Sources files = pg::singleFile(clickProgram);
        const ms::TraceResult r = ms::trace(files, q);
        print("EXPORT", r);
        std::size_t valued = 0, right = 0;
        for (std::size_t k = 0; k < r.log.rows(); ++k)
            if (r.log.none[k] == 0)
            {
                ++valued;
                right += r.log.values[k] == static_cast<double>(r.log.at[k] % 10007) ? 1 : 0;
            }
        std::printf("EXPORT: %zu samples, %zu rows (the first at %zu), %zu with a value, %zu of them the count played\n", r.l.size(),
                    r.log.rows(), r.log.rows() > 0 ? r.log.at.front() : 0, valued, right);
        P.eq("export.log_rows", static_cast<int64_t>(r.log.rows()), static_cast<int64_t>(r.l.size() / 160));
        P.eq("export.log_first_row", r.log.rows() > 0 ? static_cast<int64_t>(r.log.at.front()) : -1, 160);
        P.eq("export.log_columns", r.log.columns == std::vector<std::string>{ "count" } && r.log.kinds.size() == 1
                                       && r.log.kinds[0] == ms::FieldLog::Kind::whole ? 1 : 0, 1);
        P.eq("export.log_values", valued > 0 && right == valued ? 1 : 0, 1);
        // The voice sounds while held (0.5 s: 150 rows); the effect's instance throughout.
        P.ge("export.log_valued_rows", static_cast<double>(valued), kInstrument ? 149.0 : static_cast<double>(r.log.rows()));
        P.eq("export.no_find", r.problems.empty() && !anyHas(r.lines, "Nothing found") ? 1 : 0, 1);
        if constexpr (!kInstrument)
        {
            double inPeak = 0.0;
            for (const float x : r.inL)
                inPeak = std::max(inPeak, std::fabs(static_cast<double>(x)));
            P.eq("export.input_kept", r.inL.size() == r.l.size() && r.inR.empty() ? 1 : 0, 1);
            P.near("export.input_sine_peak", inPeak, std::pow(10.0, -12.0 / 20.0), 1e-3);
        }
        // The render hash: the samples', in the lines; the same twice, another with the level halved (a dial moved).
        P.eq("export.hash_of_samples", r.hash.size() == 64 && r.hash == ms::renderHash(r.l, r.r) ? 1 : 0, 1);
        P.eq("export.hash_line", anyHas(r.lines, "Render hash: " + r.hash.substr(0, ms::kHashShown) + " (SHA-256 of the float32 "
                                                 "samples, L R interleaved, of the render).") ? 1 : 0, 1);
        const ms::TraceResult again2 = ms::trace(files, q);
        P.eq("export.hash_stable", again2.hash == r.hash && again2.l == r.l ? 1 : 0, 1);
        {
            ms::TraceRequest other = q;                      // another note; the effect's noise in place of its sine
            if constexpr (kInstrument)
                other.render.notes = { 52 };
            else
                other.render.signal = static_cast<int>(ms::ViewInput::noise);
            const ms::TraceResult o = ms::trace(files, other);
            P.eq("export.hash_differs", !o.hash.empty() && o.hash != r.hash ? 1 : 0, 1);
        }
        {
            // An instrument's render hash is measure_output's (the same render, Result::hash); an effect's view_sound's.
            ms::Session session(files);
            const ms::Result m = session.measure(q.render);
            ms::ViewRequest vr;
            vr.render = q.render;
            const ms::View v = ms::view(files, vr);
            std::printf("EXPORT: trace's hash %s, measure_output's %s (of %s), view_sound's %s\n", r.hash.substr(0, 16).c_str(),
                        m.hash.substr(0, 16).c_str(), m.hashed.c_str(), v.hash.substr(0, 16).c_str());
            if constexpr (kInstrument)
                P.eq("export.hash_is_measure_outputs", m.hash == r.hash && m.report.find("Render hash: " + r.hash.substr(0, 16)) != std::string::npos
                                                           ? 1 : 0, 1);
            else
                P.eq("export.hash_is_measure_outputs", m.hashed == "the sine's, the noise's, the impulse's and silence's renders, in turn"
                                                           && m.report.find("Render hash: " + m.hash.substr(0, 16)) != std::string::npos ? 1 : 0, 1);
            P.eq("export.hash_is_view_sounds", v.hash == r.hash ? 1 : 0, 1);
        }

        // The WAV: float32 back to the bit, its data chunk the render hash's bytes; 24-bit within half a step, clipped.
        std::string bytes;
        const ms::Sink sink = [&bytes](const void* d, std::size_t n) {
            bytes.append(static_cast<const char*>(d), n);
            return true;
        };
        const auto span = [](const std::string& b) { return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(b.data()), b.size()); };
        const ms::WavWritten w = ms::writeWav(sink, r.l, r.r, rate, ms::WavFormat::float32);
        ms::Wav back;
        std::string why;
        const bool read = ms::readWav(span(bytes), back, why);
        P.eq("export.wav_float_back", read && w.ok && back.isFloat && back.bits == 32 && back.rate == 48000 && back.channels == 2
                                          && back.l == r.l && back.r == r.r ? 1 : 0, 1);
        P.eq("export.wav_float_size", w.bytes == bytes.size() && bytes.size() == ms::wavBytes(r.l.size(), ms::WavFormat::float32) ? 1 : 0, 1);
        {
            ms::Sha256 h;
            const std::size_t data = bytes.size() - r.l.size() * 8;
            h.add(bytes.data() + data, bytes.size() - data);
            P.eq("export.wav_data_is_the_hash", data >= 8 && std::memcmp(bytes.data() + data - 8, "data", 4) == 0 && h.hex() == r.hash ? 1 : 0, 1);
        }
        {
            std::vector<float> ramp(4801);
            for (std::size_t i = 0; i < ramp.size(); ++i)
                ramp[i] = static_cast<float>(-1.5 + 3.0 * static_cast<double>(i) / 4800.0);
            std::size_t past = 0;
            for (const float x : ramp)
                past += std::nearbyint(static_cast<double>(x) * 8388608.0) > 8388607.0 || x < -1.0f ? 1 : 0;
            bytes.clear();
            const ms::WavWritten w24 = ms::writeWav(sink, ramp, {}, rate, ms::WavFormat::pcm24);
            ms::Wav b24;
            const bool read24 = ms::readWav(span(bytes), b24, why);
            double worst = 0.0;
            for (std::size_t i = 0; read24 && i < ramp.size(); ++i)
                if (std::fabs(ramp[i]) < 1.0f)
                    worst = std::max(worst, std::fabs(static_cast<double>(b24.l[i]) - ramp[i]) * 8388608.0);
            std::printf("EXPORT: 24-bit, %zu of %zu samples clipped (%zu past full scale on each channel), the worst error %.3f steps\n",
                        w24.clipped, 2 * ramp.size(), past, worst);
            P.eq("export.wav_24_read", read24 && !b24.isFloat && b24.bits == 24 && b24.channels == 2 && b24.l.size() == ramp.size()
                                           && b24.r == b24.l && bytes.size() == ms::wavBytes(ramp.size(), ms::WavFormat::pcm24) ? 1 : 0, 1);
            P.le("export.wav_24_within_half_a_step", worst, 0.5);
            P.eq("export.wav_24_clipped", static_cast<int64_t>(w24.clipped), 2 * static_cast<int64_t>(past));
            P.eq("export.wav_24_full_scale", read24 && b24.l.front() == -1.0f && b24.l.back() == static_cast<float>(8388607.0 / 8388608.0) ? 1 : 0, 1);
        }

        // The CSV: the header and a row a log row; a float to 9 digits and a double to 17 read back to their bits.
        bytes.clear();
        const std::size_t csvBytes = ms::writeCsv(sink, r.log, rate);
        P.eq("export.csv_rows", csvBytes == bytes.size() && bytes.rfind("time_s,sample,count\n0.003333,160,", 0) == 0
                                    && static_cast<std::size_t>(std::count(bytes.begin(), bytes.end(), '\n')) == r.log.rows() + 1 ? 1 : 0, 1);
        {
            ms::FieldLog log;
            log.columns = { "f", "d", "i" };
            log.kinds = { ms::FieldLog::Kind::real, ms::FieldLog::Kind::real64, ms::FieldLog::Kind::whole };
            const float f = 0.1f + 1e-7f;
            const double d = 90.0 + 1.0e-6 / 3.0;
            log.at = { 16, 32 };
            log.values = { f, d, 7.0, 0.0, std::numeric_limits<double>::infinity(), 0.0 };
            log.none = { 0, 0, 0, 1, 0, 0 };
            bytes.clear();
            ms::writeCsv(sink, log, rate);
            const std::size_t row1 = bytes.find('\n') + 1;
            const std::string first = bytes.substr(row1, bytes.find('\n', row1) - row1);
            const std::size_t c2 = first.find(',', first.find(',') + 1), c3 = first.find(',', c2 + 1), c4 = first.find(',', c3 + 1);
            const float fBack = std::strtof(first.c_str() + c2 + 1, nullptr);
            const double dBack = std::strtod(first.c_str() + c3 + 1, nullptr);
            std::printf("EXPORT: the CSV of a float, a double and an int:\n%s", bytes.c_str());
            P.eq("export.csv_float_back", fBack == f ? 1 : 0, 1);
            P.eq("export.csv_double_back", dBack == d ? 1 : 0, 1);
            P.eq("export.csv_whole_and_empty", first.substr(c4 + 1) == "7" && bytes.find("\n0.000667,32,,inf,0\n") != std::string::npos ? 1 : 0, 1);
            bytes.clear();
            ms::writeCsv(sink, log, rate, {}, 6);
            P.eq("export.csv_six_digits", bytes.find(",0.1,90,7\n") != std::string::npos ? 1 : 0, 1);
        }
        const std::vector<std::size_t> even = ms::evenRows(180, 32);
        P.eq("export.even_rows", even.size() == 32 && even.front() == 0 && even.back() == 179 && ms::evenRows(5, 32).size() == 5 ? 1 : 0, 1);
    }

    // ---- VOICE, SAME ---------------------------------------------------------------------------------------------------
    if constexpr (kInstrument)
    {
        ms::TraceRequest only = nanReq;
        only.voice = 48;
        only.voiceGiven = true;
        const ms::TraceResult r = ms::trace(pg::singleFile(nanProgram), only);
        print("VOICE 48", r);
        P.eq("voice.other_note_clean", r.problems.empty() && anyHas(r.lines, "Nothing found") ? 1 : 0, 1);
    }
    const ms::TraceResult again = ms::trace(pg::singleFile(clickProgram), clickReq);
    P.eq("same.lines", again.lines == click.lines ? 1 : 0, 1);

    // The requests the rows below share: an instrument's note 60 held 0.5 s, an effect's sine as long.
    const auto held = [&](ms::TraceFind find, double seconds = 0.5) {
        ms::TraceRequest q;
        q.find = find;
        if constexpr (kInstrument)
            q.render.phrase = { note(60, 0.0, seconds) };
        else
        {
            q.render.holdSeconds = static_cast<float>(seconds);
            q.render.signal = static_cast<int>(ms::ViewInput::sine);
        }
        q.render.tailSeconds = 0.2f;
        return q;
    };

    // ---- SILENCE, DC ---------------------------------------------------------------------------------------------------
    {
        const ms::TraceResult r = ms::trace(pg::singleFile(kInstrument ? kSilenceVoice : kSilenceEffect), held(ms::TraceFind::silence));
        print("SILENCE", r);
        const ms::TraceProblem* s = firstOf(r, ms::TraceFind::silence);
        P.eq("silence.found", s != nullptr ? 1 : 0, 1);
        P.near("silence.at_sample", s != nullptr ? static_cast<double>(s->at) : -1.0, kInstrument ? 9008.0 : 8999.0, 1.0);
        if constexpr (kInstrument)
            P.eq("silence.voice_ends", s != nullptr && s->verdict.find("the voice ends while its key is held") != std::string::npos ? 1 : 0, 1);
        else
            P.eq("silence.culprit_gain", s != nullptr && !s->culprits.empty() && s->culprits[0].path == "gain" ? 1 : 0, 1);
    }
    {
        const ms::TraceResult r = ms::trace(pg::singleFile(kInstrument ? kDcVoice : kDcEffect), held(ms::TraceFind::dc));
        print("DC", r);
        const ms::TraceProblem* d = firstOf(r, ms::TraceFind::dc);
        P.eq("dc.found", d != nullptr ? 1 : 0, 1);
        P.le("dc.near_the_start_s", d != nullptr ? static_cast<double>(d->at) / rate : 1.0, 0.06);
        P.eq("dc.verdict", d != nullptr && d->verdict.find("a mean offset of") != std::string::npos ? 1 : 0, 1);
    }

    // ---- FROMSTART: a sentinel init sets past 1e6 hides nothing --------------------------------------------------------
    {
        ms::TraceRequest q = clickReq;
        q.find = ms::TraceFind::any;
        const ms::TraceResult r = ms::trace(pg::singleFile(withSentinel(clickProgram)), q);
        print("FROMSTART", r);
        // find any keeps the strongest click: one of the resets (samples 10006 and 20013), its culprit the phase.
        const ms::TraceProblem* c = firstOf(r, ms::TraceFind::click);
        P.eq("fromstart.click_still_found", c != nullptr && (c->at + 1) % 10007 == 0 && !c->culprits.empty()
                                                && c->culprits[0].path == "phase" ? 1 : 0, 1);
        P.eq("fromstart.no_runaway", firstOf(r, ms::TraceFind::runaway) == nullptr ? 1 : 0, 1);
        // Large from the start is the field's own size: not a runaway, and nothing to note (a NaN there would be).
        P.eq("fromstart.large_not_noted", !has(r.fromStart, "lastFc") && !anyHas(r.lines, "lastFc 1e+09") ? 1 : 0, 1);
    }

    // ---- STEADY, GROWS: a field runs away past 1e6 only at a thousand times its size in its first 10 ms ---------------
    {
        const ms::TraceResult steady = ms::trace(pg::singleFile(pressureProgram(false)), held(ms::TraceFind::runaway, 0.4));
        print("STEADY", steady);
        P.eq("steady.no_runaway", steady.problems.empty() && anyHas(steady.lines, "Nothing found") ? 1 : 0, 1);
        const ms::TraceResult grows = ms::trace(pg::singleFile(pressureProgram(true)), held(ms::TraceFind::runaway, 0.4));
        print("GROWS", grows);
        const ms::TraceProblem* g = firstOf(grows, ms::TraceFind::runaway);
        // 100 x 1.001^n passes 1e6 at n = 9215 (its first 10 ms reach about 161, a thousand times that is less).
        P.eq("grows.runaway", g != nullptr && !g->culprits.empty() && g->culprits[0].path == "pressure"
                                  && anyHas(grows.lines, "a thousand times its size in its first 10 ms") ? 1 : 0, 1);
        P.near("grows.at_sample", g != nullptr && !g->culprits.empty() ? static_cast<double>(g->culprits[0].at) : -1.0, 9215.0, 10.0);
    }

    // ---- RENDER: trace renders what measure_output (and view_sound) render: the notes, the velocity, the times and the
    // dials asked for, the same bits ----------------------------------------------------------------------------------------
    {
        const pg::Sources files = pg::singleFile(pg::kExamples[0].source);
        ms::TraceRequest q;
        q.find = ms::TraceFind::any;
        q.render.holdSeconds = 0.3f;
        q.render.tailSeconds = 0.2f;
        q.render.velocity = 0.5f;
        if constexpr (kInstrument)
            q.render.notes = { 52, 59 };
        ms::Session session(files);
        const pg::Controls& controls = session.controls();
        std::string dial;
        for (const pg::Control& k : controls)
            if (k.used && k.type == pg::ControlType::real && dial.empty())
            {
                dial = k.name;
                q.render.dials[dial] = k.min + 0.8f * (k.max - k.min);
            }
        const ms::TraceResult r = ms::trace(files, q);
        ms::Session::Render want;
        if constexpr (kInstrument)
            want = session.render(q.render, nullptr, nullptr, 0);
        ms::ViewRequest vr;
        vr.render = q.render;
        const ms::Sound sound = ms::renderSound(files, vr);
        const std::vector<float>& wl = kInstrument ? want.l : sound.l;
        const std::vector<float>& wr = kInstrument ? want.r : sound.r;
        ms::TraceRequest plain = q;
        plain.render.dials.clear();
        const ms::TraceResult d = ms::trace(files, plain);
        std::printf("RENDER: %zu samples, the dial %s at %.3f; measure_output's render %zu samples\n", r.l.size(), dial.c_str(),
                    dial.empty() ? 0.0 : static_cast<double>(q.render.dials[dial]), wl.size());
        P.eq("render.as_measured", !r.l.empty() && r.l == wl && r.r == wr && sound.l == r.l ? 1 : 0, 1);
        P.eq("render.dials_count", !dial.empty() && d.l.size() == r.l.size() && d.l != r.l ? 1 : 0, 1);
    }

    // ---- BUFFER: a feedback that grows in a buffer trace does not read -------------------------------------------------
    {
        const ms::TraceResult r = ms::trace(pg::singleFile(std::string(kLine) + (kInstrument ? kBufferVoice : kBufferEffect)), held(ms::TraceFind::any));
        print("BUFFER", r);
        const ms::TraceProblem* run = firstOf(r, ms::TraceFind::runaway);
        P.eq("buffer.runaway_found", run != nullptr ? 1 : 0, 1);
        P.eq("buffer.runaway_names_the_buffer", run != nullptr && run->verdict.find("line.buffer") != std::string::npos
                                                    && run->verdict.find("rather than a state that grows") == std::string::npos ? 1 : 0, 1);
        const ms::TraceProblem* bad = firstOf(r, ms::TraceFind::nan);
        P.eq("buffer.nan_not_only_a_local", bad == nullptr || (bad->verdict.find("line.buffer") != std::string::npos
                                                               && bad->verdict.find("lives in a local") == std::string::npos) ? 1 : 0, 1);
    }

    // ---- TIE: two fields bad in the same sample are named together; a note-on's step is the note-on's ------------------
    {
        const ms::TraceResult r = ms::trace(pg::singleFile(kInstrument ? kTieVoice : kTieEffect), held(ms::TraceFind::nan));
        print("TIE", r);
        const ms::TraceProblem* t = firstOf(r, ms::TraceFind::nan);
        P.eq("tie.both_named", t != nullptr && t->culprits.size() >= 2 && t->culprits[0].at == t->culprits[1].at
                                   && t->verdict.find("a and b go bad in the same sample") != std::string::npos
                                   && t->verdict.find("first value") == std::string::npos ? 1 : 0, 1);
    }
    if constexpr (kInstrument)
    {
        ms::TraceRequest q;
        q.find = ms::TraceFind::click;
        q.render.phrase = { note(48, 0.0, 0.6), note(60, 0.3, 0.3) };
        q.render.tailSeconds = 0.2f;
        const ms::TraceResult r = ms::trace(pg::singleFile(kAttackVoice), q);
        print("ATTACK", r);
        const ms::TraceProblem* c = firstOf(r, ms::TraceFind::click);
        P.near("attack.at_the_note_on", c != nullptr ? static_cast<double>(c->at) : -1.0, 14400.0, 1.0);
        P.eq("attack.the_note_on_named", c != nullptr && c->verdict.find("comes with the note-on of note 60") != std::string::npos ? 1 : 0, 1);
    }

    // ---- LOWSAW: a low saw's wrap is no click --------------------------------------------------------------------------
    {
        ms::TraceRequest q;
        q.find = ms::TraceFind::click;
        if constexpr (kInstrument)
            q.render.phrase = { note(24, 0.0, 1.0) };
        else
        {
            q.render.holdSeconds = 1.0f;
            q.render.signal = static_cast<int>(ms::ViewInput::silence);
        }
        q.render.tailSeconds = 0.2f;
        const ms::TraceResult r = ms::trace(pg::singleFile(kInstrument ? kLowSawVoice : kLowSawEffect), q);
        print("LOWSAW", r);
        P.eq("lowsaw.no_click", r.problems.empty() && anyHas(r.lines, "Nothing found") ? 1 : 0, 1);
    }

    // ---- SHARED: a Shared field listed and watched as shared->gain -----------------------------------------------------
    {
        const pg::Sources files = pg::singleFile(std::string(kSharedPart) + (kInstrument ? kSharedVoice : kSharedEffect));
        ms::TraceRequest q = held(ms::TraceFind::none, 0.2);
        q.list = true;
        const ms::TraceResult l = ms::trace(files, q);
        print("SHARED list", l);
        P.eq("shared.listed", anyHas(l.lines, "shared->gain float") ? 1 : 0, 1);
        q.list = false;
        q.fields = { "shared" };
        const ms::TraceResult w = ms::trace(files, q);
        print("SHARED watch", w);
        P.eq("shared.watched", w.watched.size() == 1 && w.watched[0] == "shared->gain" && anyHas(w.lines, "shared->gain: min 0.25, max 0.25") ? 1 : 0, 1);
    }

    // ---- DOUBLE: a double field, 2-D arrays and a field's attributes, listed and watched -----------------------------
    {
        const pg::Sources files = pg::singleFile(kInstrument ? kDoubleVoice : kDoubleEffect);
        ms::TraceRequest q = held(ms::TraceFind::none, 0.5);
        q.list = true;
        const ms::TraceResult l = ms::trace(files, q);
        print("DOUBLE list", l);
        P.eq("double.compiles", l.error.empty() ? 1 : 0, 1);
        P.eq("double.listed_with_its_look", has(l.lines, "temp double (\"TEMP\", C, 80 to 100)") ? 1 : 0, 1);
        P.eq("double.float_twin", has(l.lines, "ftemp float") ? 1 : 0, 1);
        P.eq("double.grid_rows", has(l.lines, "gain[0][0..2] float") && has(l.lines, "gain[1][0..2] float") ? 1 : 0, 1);
        P.eq("double.large_grid_left_out", anyHas(l.lines, "left out: bank: float[4][16], 64 elements, more than 16") ? 1 : 0, 1);
        q.list = false;
        q.fields = { "temp", "ftemp", "gain[1][2]" };
        const ms::TraceResult w = ms::trace(files, q);
        print("DOUBLE watch", w);
        // The last value each line gives ("..., last 90.0336; ..."), -1 when there is none.
        const auto last = [&](const std::string& path) {
            for (const std::string& x : w.lines)
                if (x.rfind(path + " (", 0) == 0 || x.rfind(path + ":", 0) == 0)
                    if (const std::size_t at = x.find(", last "); at != std::string::npos)
                        return std::strtod(x.c_str() + at + 7, nullptr);
            return -1.0;
        };
        // The significant digits of the last value temp's line gives ("90.0335999999": 12).
        const auto lastDigits = [&](const std::string& path) {
            int digits = 0;
            for (const std::string& x : w.lines)
                if (x.rfind(path + " (", 0) == 0 || x.rfind(path + ":", 0) == 0)
                    if (const std::size_t at = x.find(", last "); at != std::string::npos)
                    {
                        bool lead = true;
                        for (std::size_t k = at + 7; k < x.size() && x[k] != ';' && x[k] != 'e'; ++k)
                        {
                            if (x[k] < '0' || x[k] > '9' || (lead && x[k] == '0'))
                                continue;
                            lead = false;
                            ++digits;
                        }
                    }
            return static_cast<double>(digits);
        };
        // 1e-6 a sample from 90, summed as the program sums it: 0.7 s at 48 kHz is 33,600 samples; the synth's voice
        // sounds its 0.5 s held, 24,000.
        double expected = 90.0;
        for (int k = 0; k < (kInstrument ? 24000 : 33600); ++k)
            expected += 1e-6;
        const double moved = last("temp"), stuck = last("ftemp");
        std::printf("DOUBLE: temp last %.12g (expected %.15g), ftemp last %.9g\n", moved, expected, stuck);
        P.eq("double.watched", w.watched.size() == 3 ? 1 : 0, 1);
        P.eq("double.its_look_in_the_line", anyHas(w.lines, "temp (\"TEMP\", C, 80 to 100): min 90") ? 1 : 0, 1);
        P.near("double.moves", moved, expected, 1e-9);
        P.ge("double.twelve_digits", lastDigits("temp"), 10.0);
        P.near("double.float_stays", stuck, 90.0, 0.0);
        P.eq("double.grid_element", anyHas(w.lines, "gain[1][2]: min 1.5, max 1.5") ? 1 : 0, 1);

        // Its raw values: 8 evenly over the render, then 8 around 0.25 s, a sample each; each to its 12 digits.
        const auto points = [&](const ms::TraceResult& r, const std::string& head) {
            std::vector<double> v;
            for (const std::string& x : r.lines)
                if (x.rfind("  temp ", 0) == 0 && x.find(head) != std::string::npos)
                {
                    const char* s = x.c_str() + x.rfind(": ") + 2;
                    for (char* end = nullptr;; s = end)
                    {
                        const double d = std::strtod(s, &end);
                        if (end == s)
                            break;
                        v.push_back(d);
                    }
                }
            return v;
        };
        q.fields = { "temp" };
        q.points = 8;
        const ms::TraceResult even = ms::trace(files, q);
        print("DOUBLE points", even);
        // (The synth's voice sounds for the first 0.5 s of the 0.7: its last points are "-", where the numbers end.)
        const std::vector<double> e = points(even, " every ");
        bool rising = e.size() >= (kInstrument ? 5u : 8u);
        for (std::size_t k = 1; k < e.size(); ++k)
            rising = rising && e[k] > e[k - 1];
        P.eq("double.points_rise", rising ? 1 : 0, 1);
        q.around = 0.25;
        const ms::TraceResult around = ms::trace(files, q);
        print("DOUBLE around", around);
        const std::vector<double> a = points(around, ", a sample each: ");
        double worst = a.size() == 8 ? 0.0 : 1.0;
        for (std::size_t k = 1; k < a.size(); ++k)
            worst = std::max(worst, std::fabs(a[k] - a[k - 1] - 1e-6));
        P.le("double.around_steps", worst, 1e-9);
    }

    // ---- GRID: a click whose culprit is a double in a 2-D array, written by its two indexes ------------------------------
    {
        const std::string program = kInstrument ? kGridVoice : kGridEffect;
        const ms::TraceResult r = ms::trace(pg::singleFile(program), clickReq);
        print("GRID", r);
        const ms::TraceProblem* c = firstOf(r, ms::TraceFind::click);
        const ms::TraceCulprit* ph = nullptr;
        bool tempBlamed = false;
        if (c != nullptr)
            for (const ms::TraceCulprit& k : c->culprits)
            {
                ph = ph == nullptr && k.path == "ph[1][2]" ? &k : ph;
                tempBlamed = tempBlamed || k.path == "temp";
            }
        P.eq("grid.found", c != nullptr ? 1 : 0, 1);
        P.near("grid.at_sample", c != nullptr ? static_cast<double>(c->at) : -1.0, 10005.0, 1.0);
        P.eq("grid.culprit_element", ph != nullptr ? 1 : 0, 1);
        P.eq("grid.written_by_its_indexes", ph != nullptr && has(ph->writes, lineOf(program, kInstrument ? "ph[1][2] = 0.75;" : "ph[1][2] = 0.25;"))
                                                && anyHas(r.lines, "  ph[1][2]: steps ") && !anyHas(r.lines, "no line found that writes it") ? 1 : 0, 1);
        // A double's values as it holds them: the reset's value plus one float step (a float's is off in its 8th digit).
        const double reset = kInstrument ? 0.75 + static_cast<double>(0.003f) : 0.25 + static_cast<double>(0.0000625f);
        P.near("grid.values_double", ph != nullptr && !ph->after.empty() ? ph->after[0] : -1.0, reset, 1e-12);
        P.eq("grid.no_double_blamed", c != nullptr && !tempBlamed ? 1 : 0, 1);
    }

    // ---- STOP: a cancel or the time limit says so, and claims nothing it did not check ---------------------------------
    {
        // Pass 1 renders the local program's 0.7 s in 2100 calls of 16 samples, and asks keepGoing every 64th: a cancel
        // at the 10th ask stops it at sample 10,240, after the NaN at 8999 (which it lists, untraced); at the 34th, in
        // the second pass, long before the NaN's window.
        ms::TraceRequest q = localReq;
        int asks = 0, stopAt = 10;
        q.keepGoing = [&] { return ++asks < stopAt; };
        const ms::TraceResult one = ms::trace(pg::singleFile(localProgram), q);
        print("STOP in pass 1", one);
        const ms::TraceProblem* a = firstOf(one, ms::TraceFind::nan);
        P.eq("stop.pass1_stopped", one.stopped && anyHas(one.lines, "cancelled") ? 1 : 0, 1);
        P.eq("stop.pass1_lists_the_nan", a != nullptr && a->at <= 8999 && a->at + 16 > 8999 && a->culprits.empty()
                                             && !anyHas(one.lines, "Nothing found") && anyHas(one.lines, "not traced") ? 1 : 0, 1);
        asks = 0;
        stopAt = 34;
        const ms::TraceResult two = ms::trace(pg::singleFile(localProgram), q);
        print("STOP in pass 2", two);
        const ms::TraceProblem* b = firstOf(two, ms::TraceFind::nan);
        P.eq("stop.pass2_stopped", two.stopped && anyHas(two.lines, "cancelled") ? 1 : 0, 1);
        P.eq("stop.pass2_claims_nothing", b != nullptr && b->culprits.empty() && b->verdict.find("lives in a local") == std::string::npos
                                              && anyHas(two.lines, "not traced") && !anyHas(two.lines, "same bits") ? 1 : 0, 1);
        // Which pass stopped (TraceResult::stoppedFirst: an export's WAV and CSV say "up to where trace stopped" for pass
        // 1's alone): pass 1's render is cut, pass 2's stop leaves it whole, the same bits as a trace that never stopped.
        const ms::TraceResult whole = ms::trace(pg::singleFile(localProgram), localReq);
        std::printf("STOP: pass 1's render %zu samples cut in pass 1, %zu cut in pass 2, %zu never cut\n", one.l.size(), two.l.size(),
                    whole.l.size());
        P.eq("stop.pass1_first", one.stoppedFirst && one.l.size() >= 9000 && one.l.size() < whole.l.size() ? 1 : 0, 1);
        P.eq("stop.pass2_not_first", two.stopped && !two.stoppedFirst && !whole.stopped && two.l.size() == whole.l.size()
                                         && !two.hash.empty() && two.hash == whole.hash ? 1 : 0, 1);
        ms::TraceRequest tiny = nanReq;
        tiny.seconds = 1e-9;
        const ms::TraceResult t = ms::trace(pg::singleFile(nanProgram), tiny);
        print("STOP at the time limit", t);
        P.eq("stop.time_limit", t.stopped && t.stoppedFirst && anyHas(t.lines, "the time limit") && anyHas(t.lines, "rendered before it stopped")
                                    && !anyHas(t.lines, "same bits") ? 1 : 0, 1);
    }

    // ---- NAN0: a NaN from the first sample: the self-check has nothing to compare, and says so -------------------------
    {
        std::string program = kInstrument ? kLocalVoice : kLocalEffect;
        const std::size_t at = program.find("count < 9000");
        program.replace(at, 12, "count < 0");
        const ms::TraceResult r = ms::trace(pg::singleFile(program), held(ms::TraceFind::nan));
        print("NAN0", r);
        P.eq("nan0.found_at_0", r.problems.size() == 1 && r.problems[0].at == 0 ? 1 : 0, 1);
        P.eq("nan0.self_check_claims_nothing", !anyHas(r.lines, "same bits") && anyHas(r.lines, "nothing to compare") ? 1 : 0, 1);
    }
    return P.finish();
}
