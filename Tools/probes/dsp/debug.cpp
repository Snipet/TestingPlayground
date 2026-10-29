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
//   FROMSTART a sentinel past 1e6 that init sets: a note, not a runaway, and the click after it is still found
//   BUFFER  a delay line fed back at 1.1: the runaway's and the NaN's verdicts name the buffer trace does not read
//   TIE     two fields bad in the same sample named together; ATTACK (the synth) a note-on's click is the note-on's
//   LOWSAW  a saw at note 24, whose wrap comes every third window: no click
//   SHARED  a module Shared's field listed, and watched as shared->gain
//   STOP    a cancel in pass 1 (the NaN found so far listed, not traced), one in pass 2, the time limit: each says so
//   NAN0    a NaN from the first sample: the self-check says it had nothing to compare
#include "Probe.h"

#include "Examples.h"
#include "Product.h"

#include "dsp/Debug.h"
#include "dsp/SoundView.h"

#include <cstdio>
#include <string>

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
        P.eq("fromstart.noted", has(r.fromStart, "lastFc") && anyHas(r.lines, "lastFc 1e+09") ? 1 : 0, 1);
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
        ms::TraceRequest tiny = nanReq;
        tiny.seconds = 1e-9;
        const ms::TraceResult t = ms::trace(pg::singleFile(nanProgram), tiny);
        print("STOP at the time limit", t);
        P.eq("stop.time_limit", t.stopped && anyHas(t.lines, "the time limit") && anyHas(t.lines, "rendered before it stopped")
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
