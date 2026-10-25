// PROBE name=plugin.chatexample timeout=300
//
// plugin.chatexample: what the chat's system prompt teaches, and Claude's measure_output tool (dsp/Measure.h), at
// 48 kHz:
//   PROMPT   the system prompt has the large-program section, the tools' workflow (measure_output, add_library_file),
//            what the editor's RANDOM button makes,
//            THE PANEL with its #pragma lines, THE LIBRARY with every library file's header word for word, and the
//            program built from the library, word for word; the program's other files are the library's, as it has them
//   EXAMPLE  that program (ChatAgent::promptExample, this plugin's kind) compiles with no message (its panel lines
//            too); its dials are named, with ranges and defaults inside them, and every one is in a titled section of
//            its panel; it plays two seconds finite and audible with no alert (an effect over noise, an instrument a
//            chord released at 1 s); blocks of 64 and 333 give the same bits; an instrument's voices end
//   MEASURE  measure::run on programs whose answer is known: an instrument's sine voice at 0.25 (peak -12.0 dBFS,
//            its partial at the note's frequency, the voice ending, a chord's level reported) or an effect's gain of
//            0.5 (-6.0 dB on the sine and on the noise, an impulse's response at once); the spectrum's peak of a
//            1 kHz sine within 0.5 Hz; a program that does not compile reports its messages; a dial name that is
//            not one is reported; a voice that never ends (or an effect that makes sound from silence) is flagged;
//            the example's own report is not silent and has no alert; an instrument's Inharmonic line: clean for the
//            sine voice; at C7 a naive saw's aliases above -40 dB, a wavetable voice's (wavetable.fm, its table built
//            once in a module Shared) clean; a saw's own edges are no click, a phase that jumps once is
//            (the prompt teaches the FFT's builtins and the module Shared too)
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

namespace tp = testingplayground;

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
    P.eq("prompt.fft", has(prompt, "fft_create(n)") && has(prompt, "fft_forward(plan, in, bins)")
                           && has(prompt, "fft_inverse(plan, bins, out)") ? 1 : 0, 1);
    P.eq("prompt.shared", has(prompt, "module Shared") && has(prompt, "Shared*") ? 1 : 0, 1);
    P.eq("prompt.panel_section", has(prompt, "THE PANEL") && has(prompt, "#pragma panel section") && has(prompt, "#pragma panel row")
                                     && has(prompt, "#pragma panel label") && has(prompt, "#pragma panel small") ? 1 : 0, 1);
    int headers = 0;
    for (const tp::LibraryFile& f : tp::kLibrary)
        headers += has(prompt, std::string("<library_file name=\"") + f.name + "\" category=\"" + f.category + "\">\n"
                                   + header(f.source) + "</library_file>") ? 1 : 0;
    P.eq("prompt.library_headers", headers, static_cast<int64_t>(std::size(tp::kLibrary)));
    P.ge("prompt.example_files", static_cast<double>(example.size()), 3.0);
    P.eq("prompt.example_word_for_word",
         !example.empty() && has(prompt, "\">\n" + example[0].text + "</example_program>") ? 1 : 0, 1);
    int fromLibrary = 0;
    for (std::size_t k = 1; k < example.size(); ++k)
    {
        const tp::LibraryFile* f = tp::findLibraryFile(example[k].name);
        fromLibrary += f != nullptr && example[k].text == f->source ? 1 : 0;
    }
    P.eq("prompt.example_uses_library", fromLibrary, static_cast<int64_t>(example.size()) - 1);
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
            // Its panel: every dial in a titled section (none left over for an untitled one).
            int placed = 0;
            bool titled = !rig.panel().sections.empty();
            for (const tp::PanelSection& s : rig.panel().sections)
            {
                for (const tp::PanelItem& it : s.items)
                    placed += it.display ? 0 : 1;
                titled = titled && !s.title.empty();
            }
            std::printf("example panel: %zu sections, %d dials placed\n", rig.panel().sections.size(), placed);
            P.eq("example.panel_titled", titled ? 1 : 0, 1);
            P.eq("example.panel_has_every_dial", placed, dials);
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
    }
    return P.finish();
}
