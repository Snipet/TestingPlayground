// PROBE name=plugin.chatexample timeout=300
//
// plugin.chatexample: what the chat's system prompt teaches, and Claude's measure_output tool (dsp/Measure.h), at
// 48 kHz:
//   PROMPT   the system prompt has the large-program section, the tools' workflow (measure_output, add_library_file),
//            what the editor's RANDOM button makes and what PROJECTS keeps (versions, presets),
//            THE INTERFACE with the .gui file's language, THE LIBRARY with every library file's header word for word, and
//            the program built from the library and its panel.gui, word for word; the program's other files are the
//            library's, as it has them
//   EXAMPLE  that program (ChatAgent::promptExample, this plugin's kind) compiles with no message (its panel.gui
//            too); its dials are named, with ranges and defaults inside them, every one a control of its interface in
//            a titled section, with two displays at least; it plays two seconds finite and audible with no alert (an effect over noise, an instrument a
//            chord released at 1 s); blocks of 64 and 333 give the same bits; an instrument's voices end
//   MEASURE  measure::run on programs whose answer is known: an instrument's sine voice at 0.25 (peak -12.0 dBFS,
//            its partial at the note's frequency, the voice ending, a chord's level reported) or an effect's gain of
//            0.5 (-6.0 dB on the sine and on the noise, an impulse's response at once); the spectrum's peak of a
//            1 kHz sine within 0.5 Hz; a program that does not compile reports its messages; a dial name that is
//            not one is reported; a voice that never ends (or an effect that makes sound from silence) is flagged;
//            the example's own report is not silent and has no alert; an instrument's Inharmonic line: clean for the
//            sine voice; at C7 a naive saw's aliases above -40 dB, a wavetable voice's (wavetable.fm, its table built
//            once in a module Shared) clean; a saw's own edges are no click, a phase that jumps once is
//            (the prompt teaches the FFT's builtins and the module Shared too); the Host block (host.fm) as asked:
//            its line in the report, an instrument's pitch wheel up 2 semitones, an effect's delay of a beat at 90 BPM
//            (the prompt has THE HOST and host.fm word for word)
//   THREADS  the prompt has THREADS (parallel for, parallel { }, the races refused, a plan an iteration, the cost) and
//            this kind's advice; a program written as it says (an effect: a resonator bank of sixteen bands run at
//            once, a region a part of 256 samples; an instrument: eight wavetables built at once in Shared's init,
//            each with its own FFT plan) compiles with no message, plays finite and audible, gives what its twin on
//            one thread (for in place of parallel for) gives, bit for bit, and the same bits in blocks of 64 and 333
#include "Probe.h"

#include "Audio.h"
#include "Rig.h"

#include "dsp/Measure.h"
#include "fminus/Patch.h"
#include "plugin/ChatAgent.h"

#include "Library.h"
#include "Product.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <string>
#include <vector>

namespace tp = testingplaygroundsynth;

namespace
{
    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    // A file's header: the comment lines it starts with.
    std::string header(const std::string& text)
    {
        std::string out;
        std::size_t at = 0;
        while (at < text.size() && text.compare(at, 2, "//") == 0)
        {
            const std::size_t end = text.find('\n', at);
            out += text.substr(at, end == std::string::npos ? std::string::npos : end + 1 - at);
            if (end == std::string::npos)
                break;
            at = end + 1;
        }
        return out;
    }

    // A program that uses a region as the prompt's THREADS says, for this plugin's kind.
    std::string threadsProgram()
    {
        if constexpr (tp::product::kIsInstrument)
            return R"FM(// Threads: eight band-limited saw tables built at once in Shared's init, each voice reading its note's.
module Table
{
    float data[2048];
    float2 bins[1025];

    // A saw's first harmonics (512, halved t times), through the FFT with a plan of its own, at a peak of 1.
    void build(int t)
    {
        int top = 512;
        for (int j = 0; j < t; ++j)
            top = top / 2;
        void* plan = fft_create(2048);
        for (int k = 0; k < 1025; ++k)
            bins[k] = (float2)(0.0f, 0.0f);
        for (int k = 1; k <= top; ++k)
            bins[k] = (float2)(0.0f, -1.0f / k);
        fft_inverse(plan, bins, data);
        fft_destroy(plan);
        float peak = 0.0f;
        for (int i = 0; i < 2048; ++i)
            peak = fmaxf(peak, fabsf(data[i]));
        for (int i = 0; i < 2048; ++i)
            data[i] = data[i] / peak;
    }
};

module Shared
{
    Table tables[8];

    void init(float rate)
    {
        parallel for (int t = 0; t < 8; ++t)
            tables[t].build(t);
    }
};

module Voice
{
    param float level [[fminus::range(0, 1)]] [[fminus::default(0.8)]];
    Shared* shared;
    float rate;
    float phase;
    float step;
    float env;
    int table;
    bool held;

    void init(float sampleRate) { rate = sampleRate; }

    void start(float note, float velocity)
    {
        const float f = 440.0f * expf(0.0577622650f * (note - 69.0f));
        step = f / rate;
        int top = 512;
        table = 0;
        while (table < 7 && top * f > 0.45f * rate)
        {
            top = top / 2;
            table += 1;
        }
        env = velocity;
        held = true;
    }

    void stop() { held = false; }

    bool process(float* restrict left, float* restrict right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const float x = phase * 2048.0f;
            const int k = (int)x;
            const float a = shared->tables[table].data[k];
            const float b = shared->tables[table].data[k + 1 < 2048 ? k + 1 : 0];
            const float out = level[i] * 0.25f * env * (a + (x - k) * (b - a));
            left[i] += out;
            right[i] += out;
            phase += step;
            if (phase >= 1.0f)
                phase -= 1.0f;
            if (!held)
                env *= 0.9995f;
        }
        return held || env > 0.0001f;
    }
};
)FM";
        else
            return R"FM(// Threads: a resonator bank, eight band-passes a channel, the sixteen run at once, a region a part of 256 samples.
module Band
{
    float g;
    float k;
    float gain;
    float ic1;
    float ic2;
    float out[256];

    void set(float fc, float q, float level, float rate)
    {
        const float w = 3.14159265f * fc / rate;
        g = sinf(w) / cosf(w);
        k = 1.0f / q;
        gain = level;
    }

    // n samples (256 at most) of `in`, band-passed, into out.
    void run(const float* in, int n)
    {
        const float a1 = 1.0f / (1.0f + g * (g + k));
        const float a2 = g * a1;
        const float a3 = g * a2;
        for (int i = 0; i < n; ++i)
        {
            const float v3 = in[i] - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            out[i] = gain * v1;
        }
    }
};

module Effect
{
    param float mix [[fminus::range(0, 1)]] [[fminus::default(0.7)]];
    Band bands[16];                            // 0 to 7 the left channel's, 8 to 15 the right's

    void init(float rate)
    {
        for (int b = 0; b < 16; ++b)
            bands[b].set(150.0f * expf(0.45f * (b % 8)) * (b < 8 ? 1.0f : 1.012f), 12.0f, 0.6f, rate);
    }

    void process(float* restrict left, float* restrict right, int n)
    {
        for (int at = 0; at < n; at += 256)
        {
            const int m = n - at < 256 ? n - at : 256;
            parallel for (int b = 0; b < 16; ++b)
                bands[b].run(b < 8 ? left + at : right + at, m);
            for (int i = 0; i < m; ++i)
            {
                float wl = 0.0f;
                float wr = 0.0f;
                for (int b = 0; b < 8; ++b)
                {
                    wl += bands[b].out[i];
                    wr += bands[b + 8].out[i];
                }
                left[at + i] += mix[at + i] * (wl - left[at + i]);
                right[at + i] += mix[at + i] * (wr - right[at + i]);
            }
        }
    }
};
)FM";
    }

    std::string replaced(std::string s, const std::string& from, const std::string& to)
    {
        for (std::size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size()))
            s.replace(p, from.size(), to);
        return s;
    }

    // Whether every dial has a control in the interface (a .gui file's), each inside a titled section or page.
    bool everyDialTitled(const tp::PanelSpec& spec, const tp::Controls& controls, int& placed)
    {
        placed = 0;
        bool titled = !spec.gui.empty();
        for (const tp::GuiNode& n : spec.gui.nodes)
        {
            if (n.slot < 0)
                continue;
            placed += n.slot2 >= 0 ? 2 : 1;
            bool inTitled = false;
            for (int p = n.parent; p >= 0; p = spec.gui.nodes[static_cast<std::size_t>(p)].parent)
            {
                const tp::GuiNode& up = spec.gui.nodes[static_cast<std::size_t>(p)];
                if ((up.kind == tp::GuiKind::section || up.kind == tp::GuiKind::page) && !up.text.empty())
                    inTitled = true;
            }
            titled = titled && inTitled;
        }
        int used = 0;
        for (const tp::Control& c : controls)
            used += c.used ? 1 : 0;
        return titled && placed == used;
    }

    // The first partial a report lists, in Hz (0 when none).
    [[maybe_unused]] double firstPartial(const std::string& r)
    {
        const std::size_t p = r.find("strongest partials ");
        return p == std::string::npos ? 0.0 : std::strtod(r.c_str() + p + 19, nullptr);
    }
}

PROBE(plugin, chatexample)
{
    constexpr double kFs = 48000.0;
    constexpr bool kInstrument = tp::product::kIsInstrument;

    // ---- PROMPT ----------------------------------------------------------------------------------------------------
    const std::string& prompt = tp::ChatAgent::systemPrompt();
    const tp::Sources example = tp::ChatAgent::promptExample();
    P.eq("prompt.large_section", has(prompt, "BUILDING A LARGE PROGRAM") ? 1 : 0, 1);
    P.eq("prompt.measure_workflow", has(prompt, "measure_output") ? 1 : 0, 1);
    P.eq("prompt.library_tool", has(prompt, "add_library_file") ? 1 : 0, 1);
    P.eq("prompt.random_button", has(prompt, "RANDOM button") ? 1 : 0, 1);
    P.eq("prompt.projects_button", has(prompt, "PROJECTS button") && has(prompt, "version") ? 1 : 0, 1);
    P.eq("prompt.fft", has(prompt, "fft_create(n)") && has(prompt, "fft_forward(plan, in, bins)")
                           && has(prompt, "fft_inverse(plan, bins, out)") ? 1 : 0, 1);
    P.eq("prompt.shared", has(prompt, "module Shared") && has(prompt, "Shared*") ? 1 : 0, 1);
    P.eq("prompt.host", has(prompt, "THE HOST: TEMPO") && has(prompt, "Host* host;") && has(prompt, "lfo.follow(")
                            && has(prompt, "<host_file name=\"host.fm\">\n" + std::string(tp::Patch::hostSource()) + "</host_file>")
                            ? 1 : 0, 1);
    P.eq("prompt.interface_section", has(prompt, "THE INTERFACE: panel.gui") && has(prompt, "section \"FILTER\"")
                                         && has(prompt, "tabs { page") && has(prompt, "knob cutoff size=large taper=log")
                                         && has(prompt, "spectrum out") && has(prompt, "envelope attack decay sustain release")
                                         && has(prompt, "#pragma panel lines") ? 1 : 0, 1);
    int headers = 0;
    for (const tp::LibraryFile& f : tp::kLibrary)
        headers += has(prompt, std::string("<library_file name=\"") + f.name + "\" category=\"" + f.category + "\">\n"
                                   + header(f.source) + "</library_file>") ? 1 : 0;
    P.eq("prompt.library_headers", headers, static_cast<int64_t>(std::size(tp::kLibrary)));
    P.ge("prompt.example_files", static_cast<double>(example.size()), 3.0);
    P.eq("prompt.example_word_for_word",
         !example.empty() && has(prompt, "\">\n" + example[0].text + "</example_program>") ? 1 : 0, 1);
    P.eq("prompt.example_interface", example.size() > 1 && example[1].name == "panel.gui"
                                         && has(prompt, "<example_interface name=\"panel.gui\">\n" + example[1].text + "</example_interface>") ? 1 : 0, 1);
    int fromLibrary = 0;
    for (std::size_t k = 2; k < example.size(); ++k)
    {
        const tp::LibraryFile* f = tp::findLibraryFile(example[k].name);
        fromLibrary += f != nullptr && example[k].text == f->source ? 1 : 0;
    }
    P.eq("prompt.example_uses_library", fromLibrary, static_cast<int64_t>(example.size()) - 2);
    P.eq("prompt.program_first_in_editor", example.empty() ? 0 : (example[0].name == tp::kProgramName ? 1 : 0), 1);
    P.le("prompt.files_fit_editor", static_cast<double>(example.size()), static_cast<double>(tp::kMaxFiles));
    std::printf("system prompt: %zu characters; the example: %zu files\n", prompt.size(), example.size());

    // ---- EXAMPLE ---------------------------------------------------------------------------------------------------
    const std::size_t n = static_cast<std::size_t>(2.0 * kFs);
    const probe::Stereo input = kInstrument ? probe::Stereo(n) : probe::noise(n, 0.25);
    std::vector<probe::Note> chord;
    if constexpr (kInstrument)
    {
        for (const int note : { 48, 55, 60, 64 })
            chord.push_back({ 0, tp::EventKind::noteOn, note, 0.8f });
        for (const int note : { 48, 55, 60, 64 })
            chord.push_back({ static_cast<std::size_t>(kFs), tp::EventKind::noteOff, note, 0.0f });
    }
    {
        probe::Rig rig(kFs, 512);
        std::string messages;
        const bool ok = rig.load(example, &messages);
        if (!messages.empty())
            std::printf("%s", messages.c_str());
        P.eq("example.compiles", ok ? 1 : 0, 1);
        P.eq("example.no_messages", messages.empty() ? 1 : 0, 1);
        if (ok)
        {
            int dials = 0;
            for (int c = 0; c < tp::kMaxControls; ++c)
            {
                const tp::Control& ctl = rig.controls()[static_cast<std::size_t>(c)];
                if (!ctl.used)
                    continue;
                ++dials;
                const std::string ck = "example.dial" + std::to_string(c + 1);
                P.eq(ck + ".range", ctl.max > ctl.min ? 1 : 0, 1);
                P.in(ck + ".default_in_range", static_cast<double>(ctl.def), static_cast<double>(ctl.min),
                     static_cast<double>(ctl.max));
            }
            P.in("example.dials", static_cast<double>(dials), 8.0, static_cast<double>(tp::kMaxControls));
            // Its interface (panel.gui): every dial a control in a titled section (none left over for an untitled one).
            int placed = 0;
            const bool titled = everyDialTitled(rig.panel(), rig.controls(), placed);
            std::printf("example interface: %zu elements, %zu displays, %d dials placed\n", rig.panel().gui.nodes.size(),
                        rig.panel().displays.size(), placed);
            P.eq("example.interface_titled", titled ? 1 : 0, 1);
            P.eq("example.interface_has_every_dial", placed, dials);
            P.ge("example.interface_displays", static_cast<double>(rig.panel().displays.size()), 2.0);
            const probe::Stereo out = rig.run(input, 256, chord);
            P.eq("example.finite", probe::allFinite(out.l) && probe::allFinite(out.r) ? 1 : 0, 1);
            const double level = std::max(probe::rmsDb(out.l, 0, static_cast<std::size_t>(kFs)),
                                          probe::rmsDb(out.r, 0, static_cast<std::size_t>(kFs)));
            std::printf("example: %.1f dBFS RMS over the first second\n", level);
            P.in("example.level_db", level, -40.0, -3.0);
            P.eq("example.no_alerts", static_cast<int64_t>(rig.engine().takeAlerts()), 0);
            if constexpr (kInstrument)
            {
                rig.run(probe::Stereo(static_cast<std::size_t>(6.0 * kFs)), 256);
                P.eq("example.voices_end", rig.engine().activeVoices(), 0);
            }
            probe::Rig a(kFs, 512), b(kFs, 512);
            a.load(example);
            b.load(example);
            const probe::Stereo o64 = a.run(input, 64, chord), o333 = b.run(input, 333, chord);
            P.eq("example.blocks_64_333_same_bits",
                 probe::sameBits(o64.l, o333.l) && probe::sameBits(o64.r, o333.r) ? 1 : 0, 1);
        }
        tp::measure::Request req;
        const std::string report = tp::measure::run(example, req);
        std::printf("the example, measured:\n%s", report.c_str());
        P.eq("example.report_no_alerts", has(report, "Alerts: none.") ? 1 : 0, 1);
        P.eq("example.report_no_notes", has(report, "Note: ") ? 0 : 1, 1);
    }

    // ---- MEASURE -----------------------------------------------------------------------------------------------------
    {
        // The spectrum of a 1 kHz sine.
        std::vector<float> sine(16384);
        for (std::size_t i = 0; i < sine.size(); ++i)
            sine[i] = 0.5f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / kFs));
        double centroid = 0.0;
        const std::vector<tp::measure::Peak> peaks = tp::measure::spectrumPeaks(sine, kFs, 4, &centroid);
        P.eq("measure.sine_one_peak", static_cast<int64_t>(peaks.size()), 1);
        P.near("measure.sine_peak_hz", peaks.empty() ? 0.0 : peaks[0].hz, 1000.0, 0.5);
        P.near("measure.sine_centroid_hz", centroid, 1000.0, 60.0);
    }
    const std::string broken = tp::measure::run(tp::singleFile("module Nothing {};\n"), {});
    P.eq("measure.uncompiled_says_so", broken.rfind("It did not compile", 0) == 0 ? 1 : 0, 1);
    if constexpr (kInstrument)
    {
        const std::string voice =
            "module Voice\n{\n"
            "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.25)]];\n"
            "    float rate; float phase; float step; bool held; float env;\n"
            "    void init(float r) { rate = r; }\n"
            "    void start(float note, float v) { step = 440 * expf(0.0577622650f * (note - 69)) / rate; held = true; env = 1; }\n"
            "    void stop() { held = false; }\n"
            "    bool process(float* restrict left, float* restrict right, int n)\n"
            "    {\n"
            "        for (int i = 0; i < n; ++i)\n"
            "        {\n"
            "            if (!held) env *= 0.999f;\n"
            "            const float out = level[i] * env * sinf(6.28318531f * phase);\n"
            "            left[i] += out; right[i] += out;\n"
            "            phase += step; phase -= floorf(phase);\n"
            "        }\n"
            "        return held || env > 0.0001f;\n"
            "    }\n};\n";
        tp::measure::Request req;
        req.notes = { 57 };                                  // A3: 220 Hz
        req.dials["volume"] = 1.0f;                          // no such dial
        const std::string r = tp::measure::run(tp::singleFile(voice), req);
        std::printf("a sine voice at 0.25:\n%s", r.c_str());
        P.eq("measure.voice_peak", has(r, "Held: peak -12.0 dBFS") ? 1 : 0, 1);
        P.near("measure.voice_partial_hz", firstPartial(r), 220.0, 0.2);
        P.eq("measure.voice_ends", has(r, "The last voice ended") ? 1 : 0, 1);
        P.eq("measure.chord", has(r, "A chord (57 61 64 69)") ? 1 : 0, 1);
        P.eq("measure.unknown_dial", has(r, "No dial is named 'volume'") ? 1 : 0, 1);
        P.eq("measure.sine_nothing_inharmonic", has(r, "Inharmonic: nothing below 20 kHz") ? 1 : 0, 1);
        // The Host block (host.fm) as measure_output is asked: the pitch wheel up bends it 2 semitones.
        {
            std::string bent = "#include \"host.fm\"\n" + voice;
            bent.replace(bent.find("    float rate;"), 0, "    Host* host;\n");
            bent.replace(bent.find("phase += step;"), 14, "phase += step * expf(0.0577622650f * 2 * host->pitchbend);");
            tp::measure::Request wheels;
            wheels.notes = { 57 };
            wheels.bpm = 90.0f;
            wheels.modwheel = 0.5f;
            wheels.pitchbend = 1.0f;
            const std::string w = tp::measure::run(tp::singleFile(bent), wheels);
            if (!has(w, "Host: "))
                std::printf("a sine voice on the pitch wheel:\n%s", w.c_str());
            P.eq("measure.host_line", has(w, "Host: playing from the song's start at 90 BPM, mod wheel 0.50, pitch bend 1.00.") ? 1 : 0, 1);
            P.near("measure.host_bent_hz", firstPartial(w), 220.0 * std::pow(2.0, 2.0 / 12.0), 0.3);
            P.eq("measure.host_none_without", has(r, "Host: ") ? 0 : 1, 1);
        }
        // Aliasing at C7 (2093 Hz): a naive saw's shows; a wavetable's, its table built once in Shared, does not.
        {
            const auto inharmonicDb = [](const std::string& report) {
                const std::string key = "no harmonic of the note is ";
                const std::size_t at = report.find(key);
                return at == std::string::npos ? -200.0 : std::atof(report.c_str() + at + key.size());
            };
            tp::measure::Request high;
            high.notes = { 96 };
            std::string naive = voice;
            naive.replace(naive.find("sinf(6.28318531f * phase)"), 25, "(2 * phase - 1)");
            const std::string a = tp::measure::run(tp::singleFile(naive), high);
            std::printf("a naive saw at C7:\n%s", a.c_str());
            P.ge("measure.naive_saw_aliases_db", inharmonicDb(a), -40.0);
            P.eq("measure.saw_edges_no_click", !has(a, "likely a click") ? 1 : 0, 1);
            const tp::LibraryFile* lib = tp::findLibraryFile("wavetable.fm");
            const std::string program =
                "#include \"wavetable.fm\"\n"
                "module Shared\n{\n    Wavetable table;\n"
                "    void init(float rate) { table.allocate(1); table.shape(0, 2); }\n"
                "    void end() { table.release(); }\n};\n"
                "module Voice\n{\n"
                "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.25)]];\n"
                "    Shared* shared;\n    WaveOsc osc;\n    float rate; float step; bool held; float env;\n"
                "    void init(float r) { rate = r; osc.table = &shared->table; }\n"
                "    void start(float note, float v)\n    {\n"
                "        step = 440 * expf(0.0577622650f * (note - 69)) / rate; held = true; env = 1; osc.reset(0);\n    }\n"
                "    void stop() { held = false; }\n"
                "    bool process(float* restrict left, float* restrict right, int n)\n"
                "    {\n"
                "        for (int i = 0; i < n; ++i)\n"
                "        {\n"
                "            if (!held) env *= 0.999f;\n"
                "            const float out = level[i] * env * osc.tick(step, 0);\n"
                "            left[i] += out; right[i] += out;\n"
                "        }\n"
                "        return held || env > 0.0001f;\n"
                "    }\n};\n";
            P.eq("measure.wavetable_in_library", lib != nullptr ? 1 : 0, 1);
            if (lib != nullptr)
            {
                const tp::Sources files{ { tp::kProgramName, program }, { "wavetable.fm", lib->source } };
                const tp::Patch::Result compiled = tp::Patch::compile(files, static_cast<float>(kFs));
                P.eq("measure.wavetable_compiles_clean", compiled.patch != nullptr && compiled.notes.empty() ? 1 : 0, 1);
                if (!compiled.errors.empty() || !compiled.notes.empty())
                    std::printf("%s%s", compiled.errors.c_str(), compiled.notes.c_str());
                const std::string w = tp::measure::run(files, high);
                std::printf("a wavetable saw at C7, its table in Shared:\n%s", w.c_str());
                P.eq("measure.wavetable_nothing_inharmonic", has(w, "Inharmonic: nothing below 20 kHz") ? 1 : 0, 1);
                P.eq("measure.wavetable_audible", has(w, "Held: peak -") && !has(w, "It is silent") ? 1 : 0, 1);
                P.eq("measure.wavetable_no_click", !has(w, "likely a click") ? 1 : 0, 1);
            }
        }
        // A phase that jumps half a cycle once, at 0.5 s: a click.
        {
            std::string jump = voice;
            jump.replace(jump.find("float env;"), 10, "float env; int count;");
            jump.replace(jump.find("phase += step;"), 14, "phase += step; count += 1; if (count == 24000) phase += 0.5f;");
            const std::string j = tp::measure::run(tp::singleFile(jump), {});
            P.eq("measure.jump_is_a_click", has(j, "likely a click") ? 1 : 0, 1);
            if (!has(j, "likely a click"))
                std::printf("a sine that jumps:\n%s", j.c_str());
        }
        std::string forever = voice;
        forever.replace(forever.find("return held || env > 0.0001f;"), 29, "return true;");
        const std::string f = tp::measure::run(tp::singleFile(forever), {});
        P.eq("measure.never_ends_flagged", has(f, "No voice ended") && has(f, "Note: A voice that never returns false") ? 1 : 0,
             1);
    }
    else
    {
        const std::string gain =
            "module Effect\n{\n"
            "    param float level [[fminus::range(0, 1)]] [[fminus::default(0.5)]];\n"
            "    void process(float* restrict left, float* restrict right, int n)\n"
            "    {\n"
            "        for (int i = 0; i < n; ++i) { left[i] *= level[i]; right[i] *= level[i]; }\n"
            "    }\n};\n";
        tp::measure::Request req;
        req.dials["volume"] = 1.0f;                          // no such dial
        const std::string r = tp::measure::run(tp::singleFile(gain), req);
        std::printf("a gain of 0.5:\n%s", r.c_str());
        P.eq("measure.sine_gain", has(r, "(gain -6.0 dB), DC") ? 1 : 0, 1);
        P.eq("measure.noise_gain", has(r, "-18.0 dBFS RMS for 1.00 s: out RMS -24.0 dBFS (gain -6.0 dB)") ? 1 : 0, 1);
        P.eq("measure.impulse_at_once", has(r, "the response peaks at -12.0 dBFS after 0.0 ms") ? 1 : 0, 1);
        P.eq("measure.silence", has(r, "Silence in, 0.50 s: out RMS silent.") ? 1 : 0, 1);
        P.eq("measure.unknown_dial", has(r, "No dial is named 'volume'") ? 1 : 0, 1);
        const std::string hum =
            "module Effect\n{\n"
            "    float phase;\n"
            "    void process(float* restrict left, float* restrict right, int n)\n"
            "    {\n"
            "        for (int i = 0; i < n; ++i) { left[i] += 0.1f * sinf(6.28318531f * phase); right[i] = left[i];\n"
            "                                      phase += 0.01f; phase -= floorf(phase); }\n"
            "    }\n};\n";
        const std::string h = tp::measure::run(tp::singleFile(hum), {});
        P.eq("measure.self_sound_flagged", has(h, "Note: It makes sound from silence") ? 1 : 0, 1);
        // A delay of one beat (host.fm) at the tempo measure_output is asked for: 90 BPM, 666.7 ms.
        const std::string beat =
            "#include \"host.fm\"\n"
            "module Effect\n{\n"
            "    Host* host;\n    float buf[65536];\n    int w;\n"
            "    void process(float* restrict left, float* restrict right, int n)\n"
            "    {\n"
            "        const int d = (int)(48000 * 60 / host->bpm);\n"
            "        for (int i = 0; i < n; ++i)\n"
            "        {\n"
            "            buf[w] = left[i];\n"
            "            int at = w - d;\n"
            "            if (at < 0) at += 65536;\n"
            "            left[i] = buf[at];\n            right[i] = left[i];\n"
            "            w += 1;\n            if (w >= 65536) w = 0;\n"
            "        }\n"
            "    }\n};\n";
        tp::measure::Request synced;
        synced.bpm = 90.0f;
        const std::string b = tp::measure::run(tp::singleFile(beat), synced);
        if (!has(b, "after 666.7 ms"))
            std::printf("a delay of one beat at 90 BPM:\n%s", b.c_str());
        P.eq("measure.host_line", has(b, "Host: playing from the song's start at 90 BPM.") ? 1 : 0, 1);
        P.eq("measure.host_beat_delay", has(b, "the response peaks at -6.0 dBFS after 666.7 ms") ? 1 : 0, 1);
    }
    // ---- THREADS -----------------------------------------------------------------------------------------------------
    {
        P.eq("threads.prompt", has(prompt, "THREADS: parallel for AND parallel { }") && has(prompt, "parallel for (int i = first; i < end; ++i)")
                                   && has(prompt, "There are no reductions") && has(prompt, "two iterations must never use one plan at once")
                                   && has(prompt, "about 4 microseconds") ? 1 : 0, 1);
        P.eq("threads.kind_advice", has(prompt, kInstrument ? "never put one there" : "process runs once a host block") ? 1 : 0, 1);
        const std::string text = threadsProgram();
        const tp::Sources together{ { tp::kProgramName, text } };
        const tp::Sources alone{ { tp::kProgramName, replaced(text, "parallel for", "for") } };
        P.eq("threads.has_region", has(text, "parallel for") && !has(alone[0].text, "parallel") ? 1 : 0, 1);
        const std::size_t len = static_cast<std::size_t>(1.5 * kFs);
        const probe::Stereo in = kInstrument ? probe::Stereo(len) : probe::noise(len, 0.25);
        std::vector<probe::Note> notes;
        if constexpr (kInstrument)
        {
            for (const int note : { 36, 60, 84, 103 })
                notes.push_back({ 0, tp::EventKind::noteOn, note, 0.8f });
            for (const int note : { 36, 60, 84, 103 })
                notes.push_back({ static_cast<std::size_t>(0.75 * kFs), tp::EventKind::noteOff, note, 0.0f });
        }
        probe::Rig a(kFs, 1024), b(kFs, 1024), c(kFs, 1024);
        std::string messages;
        const bool ok = a.load(together, &messages);
        if (!messages.empty())
            std::printf("%s", messages.c_str());
        P.eq("threads.compiles", ok ? 1 : 0, 1);
        P.eq("threads.no_messages", messages.empty() ? 1 : 0, 1);
        if (ok && b.load(alone) && c.load(together))
        {
            const probe::Stereo o = a.run(in, 512, notes), serial = b.run(in, 512, notes), odd = c.run(in, 333, notes);
            P.eq("threads.finite", probe::allFinite(o.l) && probe::allFinite(o.r) ? 1 : 0, 1);
            const double level = std::max(probe::rmsDb(o.l, 0, len / 2), probe::rmsDb(o.r, 0, len / 2));
            std::printf("threads program: %.1f dBFS RMS\n", level);
            P.in("threads.level_db", level, -40.0, -3.0);
            P.eq("threads.same_as_one_thread", probe::sameBits(o.l, serial.l) && probe::sameBits(o.r, serial.r) ? 1 : 0, 1);
            P.eq("threads.blocks_same_bits", probe::sameBits(o.l, odd.l) && probe::sameBits(o.r, odd.r) ? 1 : 0, 1);
            P.eq("threads.no_alerts", static_cast<int64_t>(a.engine().takeAlerts()), 0);
        }
    }

    return P.finish();
}
