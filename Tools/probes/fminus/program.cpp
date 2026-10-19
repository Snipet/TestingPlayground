// PROBE name=fminus.program timeout=120
//
// fminus.program: FMinus, the JIT compiler, in this plugin's build (Source/fminus/FMinusProgram.h): a program compiles,
// its functions run through C function pointers, and they compute what the same C++ computes, bit for bit (FMinus,
// like this plugin's flags, fuses no multiply and add).
//   GAIN    a block kernel, gain(in, out, n, g), one call for 512 samples: exactly x * g.
//   SMOOTH  a one-pole smoother whose state is the caller's float: one call for 512 samples, two calls of 256, and the
//           same recursion in C++ give the same bits.
//   LANES   poly_lanes(), inside the program, is FMinusProgram::polyLanes() (4 with NEON).
//   ERRORS  a program that does not compile gives no program and FMinus's message, at its line and column; a function
//           the program lacks gives a null pointer and a message.
//   HEAP    a delay whose line the program allocates (calloc: 64-byte aligned, silent), runs in a kernel marked
//           [[clang::nonblocking]] over two blocks, and frees; a nonblocking kernel that calls malloc does not compile.
//   TIME    compiling the program and looking up its three functions at -O2 takes under a second (tens of ms here).
#include "Probe.h"

#include "fminus/FMinusProgram.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace tp = testingplaygroundsynth;

namespace
{
    constexpr const char* kSource = R"(
void gain(const float* restrict in, float* restrict out, int n, float g)
{
    for (int i = 0; i < n; ++i)
        out[i] = in[i] * g;
}

// A one-pole smoother. Its state is the caller's: read at the start of a call, written back at its end.
void smooth(const float* restrict in, float* restrict out, int n, float k, float* restrict state)
{
    float y = *state;
    for (int i = 0; i < n; ++i)
        out[i] = y += k * (in[i] - y);
    *state = y;
}

int lanes()
{
    return poly_lanes();
}

// A delay of `length` samples, its line on the heap: created and freed off the audio thread, run on it.
float* line_create(int length)
{
    return calloc(length, sizeof(float));
}

void line_destroy(float* line)
{
    free(line);
}

void delay(float* restrict line, int length, int* restrict position, const float* restrict in, float* restrict out,
           int n) [[clang::nonblocking]]
{
    int w = *position;
    for (int i = 0; i < n; ++i)
    {
        out[i] = line[w];
        line[w] = in[i];
        if (++w == length)
            w = 0;
    }
    *position = w;
}
)";

    int64_t mismatches(const std::vector<float>& a, const std::vector<float>& b)
    {
        int64_t n = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
            n += a[i] != b[i] ? 1 : 0;
        return n;
    }
} // namespace

PROBE(fminus, program)
{
    std::string errors;
    const auto start = std::chrono::steady_clock::now();
    auto program = tp::FMinusProgram::compile(kSource, errors);
    if (!program)
    {
        std::printf("fminus.program: the program did not compile:\n%s", errors.c_str());
        P.eq("compile.ok", 0, 1);
        return P.finish();
    }
    auto* gain = program->function<void(const float*, float*, int, float)>("gain", errors);
    auto* smooth = program->function<void(const float*, float*, int, float, float*)>("smooth", errors);
    auto* lanes = program->function<int()>("lanes", errors);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    P.eq("compile.ok", gain && smooth && lanes ? 1 : 0, 1);
    if (!gain || !smooth || !lanes)
        return P.finish();
    P.le("time.compile_ms", ms, 1000.0);

    constexpr int n = 512;
    std::vector<float> in(n);
    uint32_t noise = 0x12345678u;
    for (float& x : in)
    {
        noise = noise * 1664525u + 1013904223u;
        x = static_cast<float>(noise >> 8) / 8388608.0f - 1.0f;
    }

    // GAIN
    std::vector<float> out(n), want(n);
    gain(in.data(), out.data(), n, 0.5f);
    for (int i = 0; i < n; ++i)
        want[static_cast<std::size_t>(i)] = in[static_cast<std::size_t>(i)] * 0.5f;
    P.eq("gain.mismatches", mismatches(out, want), 0);

    // SMOOTH
    constexpr float k = 0.01f;
    float state = 0.0f;
    std::vector<float> whole(n), halves(n), cpp(n);
    smooth(in.data(), whole.data(), n, k, &state);
    float split = 0.0f;
    smooth(in.data(), halves.data(), n / 2, k, &split);
    smooth(in.data() + n / 2, halves.data() + n / 2, n / 2, k, &split);
    float y = 0.0f;
    for (int i = 0; i < n; ++i)
        cpp[static_cast<std::size_t>(i)] = y += k * (in[static_cast<std::size_t>(i)] - y);
    P.eq("smooth.blocks_mismatches", mismatches(whole, halves), 0);
    P.eq("smooth.state_equal", state == split ? 1 : 0, 1);
    P.eq("smooth.cpp_mismatches", mismatches(whole, cpp), 0);

    // LANES
    P.eq("lanes.poly", lanes(), static_cast<int64_t>(program->polyLanes()));

    // ERRORS
    std::string message;
    tp::FMinusProgram::Options options;
    options.fileName = "bad.fm";
    const auto bad = tp::FMinusProgram::compile("float f(float x)\n{\n    return x + y;\n}\n", message, options);
    P.eq("errors.no_program", bad ? 0 : 1, 1);
    P.eq("errors.located", message.find("bad.fm:3:16: error: use of undeclared identifier 'y'") != std::string::npos
                               ? 1 : 0, 1);
    std::string missing;
    P.eq("errors.missing_function", program->address("nothing", missing) == 0 ? 1 : 0, 1);
    P.eq("errors.missing_message", missing.find("nothing") != std::string::npos ? 1 : 0, 1);

    // HEAP
    auto* create = program->function<float*(int)>("line_create", errors);
    auto* destroy = program->function<void(float*)>("line_destroy", errors);
    auto* delay = program->function<void(float*, int, int*, const float*, float*, int)>("delay", errors);
    P.eq("heap.functions", create && destroy && delay ? 1 : 0, 1);
    if (create && destroy && delay)
    {
        constexpr int length = 3;
        float* line = create(length);
        P.eq("heap.aligned", line != nullptr && reinterpret_cast<std::uintptr_t>(line) % 64 == 0 ? 1 : 0, 1);
        P.eq("heap.silent", line != nullptr && line[0] == 0.0f && line[length - 1] == 0.0f ? 1 : 0, 1);
        int position = 0;
        const std::vector<float> ramp{1, 2, 3, 4, 5, 6, 7, 8};
        std::vector<float> delayed(8), wantDelayed{0, 0, 0, 1, 2, 3, 4, 5};
        delay(line, length, &position, ramp.data(), delayed.data(), 5);   // two blocks, of 5 and 3
        delay(line, length, &position, ramp.data() + 5, delayed.data() + 5, 3);
        P.eq("heap.delay_mismatches", mismatches(delayed, wantDelayed), 0);
        destroy(line);
    }
    std::string realtime;
    const auto allocating = tp::FMinusProgram::compile("void process(float* out, int n) [[clang::nonblocking]]\n"
                                                       "{\n"
                                                       "    float* scratch = malloc(n);\n"
                                                       "    free(scratch);\n"
                                                       "}\n",
                                                       realtime, options);
    P.eq("heap.realtime_refused", allocating ? 0 : 1, 1);
    P.eq("heap.realtime_message",
         realtime.find("bad.fm:3:22: error: function with 'nonblocking' attribute must not call non-'nonblocking' "
                       "function 'malloc'") != std::string::npos ? 1 : 0, 1);

    std::printf("fminus.program: compiled and looked up in %.1f ms; a missing function says: %s\n", ms,
                missing.c_str());
    return P.finish();
}
