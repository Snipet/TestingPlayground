// PROBE name=plugin.render timeout=300
//
// plugin.render: the headless renderer (<Name>Render, Source/render/Main.cpp), run as a program, as a user runs it,
// against the plugin's own tools through MCP (ChatAgent::mcpCall) on the same files and settings:
//   SAME     an example (the synth's Saw and filter, the effect's Echo) as a folder, with a scenario (the synth's
//            phrase, a move and the mod wheel; the effect's noise in, a move and the host's tempo), a window, a dial in
//            the JSON and one by --dial, fields and their spacing: exit 0; its report measure_output's word for word
//            (the CPU times aside, the rendering thread's own); its metrics (JSON, 17 digits) the same doubles as a
//            Session's in this process, every metric but the CPU's, and its render hash the same, measure_output's
//            line its first 16 digits; its WAV and its CSV the same bytes as trace's export of the same render (which
//            takes no window: the renderer leaves it to the metrics), the WAV's data hashing to its line; the synth's
//            WAV the render measure_output hashed (the same bits)
//   EXITS    a program that does not compile: 2, its messages on stderr at the file's line; one whose process loops
//            past its budget after a quarter of a second: 3, the report beginning with the stop, the WAV still written
//            whole; one that reaches past its array: 3, saying so; one whose init never ends: 3, nothing written; a hold
//            past 300 s, a glide whose at + over ends past it and an instrument's note whose at + len does, the user's
//            recording, JSON that is none, a folder without program.fm, an option it has not: 1,
//            each saying why; a program that reads a sample (#pragma sample): 1, naming it, nothing written (its
//            fields still listed)
//   MORE     a library file the program #includes and the folder lacks: added, saying so; --list-fields names the
//            fields; measure_output's profile (its line in the report); the effect's own input (--input, a WAV at 44.1
//            kHz): rendered at its rate, the same bits as trace's export of it in this process; the extension
//            (<Name>.mcpb) carries the renderer beside the server
//   NOTES    a dial the program has not, by --dial and by a scenario's move: said on stderr and listed in the metrics
//            (unknown_dials), measured (-q) or not (--no-measure); fields: a pattern that matches none and the fields
//            matched past the CSV's 12 columns (each counted once, however many patterns match it) said on stderr and
//            listed (csv.unmatched, more_matched); the request through a pipe (a FIFO: not on Windows); a folder the
//            editor's FOLDER syncs, with a program of 16 files (the most): its read-only copies (host.fm, a library
//            file the program #includes) left out (the library's added again, saying so), and a copy of another
//            build's host.fm left out, saying so; the effect's own input as WAVE_FORMAT_EXTENSIBLE (24-bit at 96 kHz)
//            rendered at its rate, one past what a render plays of it (311 s at 8 kHz) refused with nothing written, and
//            a scenario's "host" rate said as the input's
// Its files go to the probe's sandbox (<prefix>PREFS_DIR, under CTest), trace's export to its renders folder there.
#include "Probe.h"

#include "dsp/Debug.h"
#include "dsp/Export.h"
#include "dsp/Measure.h"
#include "plugin/ChatAgent.h"
#include "plugin/ChatScenario.h"
#include "plugin/ChatStore.h"
#include "plugin/ClaudeApi.h"
#include "plugin/FolderSync.h"
#include "plugin/Processor.h"

#include "Examples.h"
#include "Product.h"

#include <funkgui/panel/HeadlessGuiScope.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pg = playground;
namespace ms = playground::measure;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;
#if defined(_WIN32)
    constexpr const char* kExe = ".exe";          // the renderer as the build and the extension name it
#else
    constexpr const char* kExe = "";
#endif

    bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

    juce::var input(std::initializer_list<std::pair<const char*, juce::var>> fields) { return pg::claude::object(fields); }

    juce::var copyOf(const juce::var& v) { return juce::JSON::parse(juce::JSON::toString(v)); }

    std::string toolText(const pg::ChatAgent::McpResult& r)
    {
        std::string out;
        if (const auto* a = r.content.getArray())
            for (const juce::var& b : *a)
                if (b.getProperty("type", "").toString() == "text"
                    && !b.getProperty("text", "").toString().contains("since the last MCP call here"))
                    out += (out.empty() ? "" : "\n") + b.getProperty("text", "").toString().toStdString();
        return out;
    }

    // The renderer: CTest's <prefix>RENDER (cmake/Probes.cmake), else beside this probe (the build's top).
    std::string rendererPath()
    {
        if (const char* v = std::getenv((std::string(pg::product::kEnvPrefix) + "RENDER").c_str()); v != nullptr && *v != '\0')
            return v;
        return juce::File::getSpecialLocation(juce::File::currentExecutableFile)
            .getSiblingFile(juce::String(pg::product::kName) + "Render" + kExe)
            .getFullPathName()
            .toStdString();
    }

    struct Ran
    {
        int code = -1;
        std::string out;                          // its stdout and stderr
        double seconds = 0.0;
    };

    Ran render(const std::vector<std::string>& args)
    {
        Ran r;
        juce::StringArray a;
        a.add(juce::String::fromUTF8(rendererPath().c_str()));
        for (const std::string& s : args)
            a.add(juce::String::fromUTF8(s.c_str()));
        const auto t0 = std::chrono::steady_clock::now();
        juce::ChildProcess p;
        if (!p.start(a, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
            return r;
        r.out = p.readAllProcessOutput().toStdString();
        if (p.waitForProcessToFinish(120000))
            r.code = static_cast<int>(p.getExitCode());
        r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return r;
    }

    std::string readText(const juce::File& f) { return f.existsAsFile() ? f.loadFileAsString().toStdString() : std::string(); }

    std::vector<uint8_t> readBytes(const juce::File& f)
    {
        juce::MemoryBlock m;
        if (!f.existsAsFile() || !f.loadFileAsData(m))
            return {};
        const auto* p = static_cast<const uint8_t*>(m.getData());
        return { p, p + m.getSize() };
    }

    // A report without its CPU times (the rendering thread's own time: they differ run to run): its CPU line, and
    // cpu_percent and cpu16_percent on its Metrics line.
    std::string withoutCpu(const std::string& report)
    {
        std::string out;
        std::size_t from = 0;
        while (from < report.size())
        {
            const std::size_t end = std::min(report.find('\n', from), report.size());
            std::string line = report.substr(from, end - from);
            from = end + 1;
            if (line.rfind("CPU:", 0) == 0)
                continue;
            if (line.rfind("Metrics:", 0) == 0)
            {
                std::string kept;
                std::size_t at = 0;
                while (at < line.size())
                {
                    const std::size_t sp = std::min(line.find(' ', at), line.size());
                    const std::string token = line.substr(at, sp - at);
                    if (token.rfind("cpu", 0) != 0)
                        kept += (kept.empty() ? "" : " ") + token;
                    at = sp + 1;
                }
                line = kept;
            }
            out += line + "\n";
        }
        return out;
    }

    // A metric's value in the renderer's JSON, read with strtod (17 digits read back to the double; JUCE's JSON parser
    // may miss its last bit): a number, or "inf", "-inf", "nan".
    bool metricIn(const std::string& json, const std::string& name, double& out)
    {
        const std::size_t block = json.find("\"metrics\": {");
        const std::size_t at = block == std::string::npos ? block : json.find("\n    \"" + name + "\": ", block);
        if (at == std::string::npos)
            return false;
        const std::size_t from = at + name.size() + 9;
        const std::string text = json.substr(from, json.find_first_of(",\n", from) - from);
        if (text == "\"inf\"" || text == "\"-inf\"" || text == "\"nan\"")
        {
            out = text == "\"nan\"" ? std::numeric_limits<double>::quiet_NaN()
                : text == "\"inf\"" ? std::numeric_limits<double>::infinity()
                                    : -std::numeric_limits<double>::infinity();
            return true;
        }
        char* end = nullptr;
        out = std::strtod(text.c_str(), &end);
        return end != nullptr && *end == '\0' && !text.empty();
    }

    // The render hash a report shows ("Render hash: <16 hex> (...)"), "" with none.
    std::string hashIn(const std::string& text)
    {
        const std::size_t at = text.find("Render hash: ");
        return at == std::string::npos ? std::string() : text.substr(at + 13, ms::kHashShown);
    }

    void writeProgram(const juce::File& dir, const std::string& text)
    {
        dir.deleteRecursively();
        dir.createDirectory();
        dir.getChildFile(pg::kProgramName).replaceWithText(juce::String::fromUTF8(text.c_str()), false, false, "\n");
    }

    juce::var jsonFile(const juce::File& f) { return juce::JSON::parse(juce::String::fromUTF8(readText(f).c_str())); }

    std::vector<std::string> strings(const juce::var& list)
    {
        std::vector<std::string> out;
        if (const auto* a = list.getArray())
            for (const juce::var& v : *a)
                out.push_back(v.toString().toStdString());
        return out;
    }

    // A WAV as WAVE_FORMAT_EXTENSIBLE (a fmt chunk of 40 bytes, the PCM sub-format), integer samples of `bits` (16 or
    // 24), mono with `r` null: what ffmpeg and many other tools write past 16 bits, two channels or 48 kHz. Its data's
    // size kept even (a stereo frame, or a 16-bit one).
    [[maybe_unused]] std::vector<uint8_t> extensibleWav(const std::vector<float>& l, const std::vector<float>* r, uint32_t rate, uint32_t bits)
    {
        const uint32_t channels = r != nullptr ? 2u : 1u, width = bits / 8u;
        const uint32_t data = static_cast<uint32_t>(l.size()) * channels * width;
        std::vector<uint8_t> b;
        b.reserve(68 + data);
        const auto u16 = [&](uint32_t v) {
            b.push_back(static_cast<uint8_t>(v & 0xFFu));
            b.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
        };
        const auto u32 = [&](uint32_t v) {
            u16(v & 0xFFFFu);
            u16(v >> 16);
        };
        const auto tag = [&](const char* t) { b.insert(b.end(), t, t + 4); };
        tag("RIFF");
        u32(4 + 8 + 40 + 8 + data);
        tag("WAVE");
        tag("fmt ");
        u32(40);
        u16(0xFFFEu);
        u16(channels);
        u32(rate);
        u32(rate * channels * width);
        u16(channels * width);
        u16(bits);
        u16(22);                                 // cbSize
        u16(bits);                               // the valid bits
        u32(channels == 2 ? 3u : 4u);            // the speakers: front left and right, or centre
        constexpr uint8_t kPcm[16] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
        b.insert(b.end(), kPcm, kPcm + 16);
        tag("data");
        u32(data);
        const double full = bits == 16 ? 32767.0 : 8388607.0;
        for (std::size_t i = 0; i < l.size(); ++i)
            for (uint32_t c = 0; c < channels; ++c)
            {
                const double x = std::clamp(static_cast<double>(c == 0 ? l[i] : (*r)[i]), -1.0, 1.0) * full;
                const auto v = static_cast<uint32_t>(static_cast<int32_t>(std::lround(x)));
                for (uint32_t k = 0; k < width; ++k)
                    b.push_back(static_cast<uint8_t>((v >> (8 * k)) & 0xFFu));
            }
        return b;
    }

    // A program whose `process` does `before` first: the effect halves its input, the voice plays a sine at 0.3.
    std::string program(const std::string& fields, const std::string& before, const std::string& hooks = {})
    {
        if constexpr (kInstrument)
            return "module Voice\n{\n    float phase, inc;\n    bool on;\n" + fields + hooks
                 + "    void start(float note, float velocity)\n    {\n"
                   "        inc = 440.0f * expf(0.0577622650f * (note - 69.0f)) / 48000.0f;\n        on = true;\n    }\n\n"
                   "    void stop() { on = false; }\n\n"
                   "    bool process(float* restrict left, float* restrict right, int n)\n    {\n" + before
                 + "        for (int i = 0; i < n; ++i)\n        {\n            const float s = 0.3f * sinf(6.2831853f * phase);\n"
                   "            phase = phase + inc;\n            if (phase >= 1.0f)\n                phase = phase - 1.0f;\n"
                   "            left[i] = left[i] + s;\n            right[i] = right[i] + s;\n        }\n        return on;\n    }\n};\n";
        else
            return "module Effect\n{\n" + fields + hooks + "    void process(float* restrict left, float* restrict right, int n)\n    {\n"
                 + before
                 + "        for (int i = 0; i < n; ++i)\n        {\n            left[i] = 0.5f * left[i];\n"
                   "            right[i] = 0.5f * right[i];\n        }\n    }\n};\n";
    }
}

PROBE(plugin, render)
{
    const funkgui::HeadlessGuiScope gui;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // The probe's own folder: its sandbox under CTest, else a temporary one.
    juce::File root;
    if (const char* prefs = std::getenv((std::string(pg::product::kEnvPrefix) + "PREFS_DIR").c_str()); prefs != nullptr && *prefs != '\0')
        root = juce::File(juce::String::fromUTF8(prefs));
    else
        root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(juce::String(pg::product::kEnvPrefix) + "render-probe");
    const juce::File work = root.getChildFile("render-cli");
    work.deleteRecursively();
    work.createDirectory();
    root.getChildFile("renders").deleteRecursively();      // trace's export numbers a name taken
    const juce::File keys = work.getChildFile("chat-keys");
    pg::chatstore::setDirectoryForTests(keys);
    std::printf("renderer: %s\n", rendererPath().c_str());
    P.eq("renderer.there", juce::File(juce::String::fromUTF8(rendererPath().c_str())).existsAsFile() ? 1 : 0, 1);
    const std::string w = work.getFullPathName().toStdString() + "/";

    // ---- SAME --------------------------------------------------------------------------------------------------------
    {
        const pg::Example& ex = pg::kExamples[kInstrument ? 2 : 4];
        std::printf("the example: %s\n", ex.title);
        std::string title;
        if constexpr (kInstrument)
            title = "Saw and filter";
        else
            title = "Echo";
        P.eq("same.example", ex.title == title ? 1 : 0, 1);
        const pg::Sources files = pg::singleFile(ex.source);
        const juce::File folder = work.getChildFile("example");
        writeProgram(folder, ex.source);

        // The settings: measure_output's, trace's fields and export, a dial given by --dial on top of the JSON's.
        juce::var request = input({ { "hold_seconds", 0.6 }, { "tail_seconds", 0.3 }, { "dials_from", "defaults" },
                                    { "window", input({ { "from", 0.1 }, { "to", 0.5 } }) },
                                    { "export", input({ { "every", 480 } }) } });
        auto* rq = request.getDynamicObject();
        const char* cliDial = kInstrument ? "level" : "feedback";
        const double cliValue = kInstrument ? -20.0 : 0.6;
        if constexpr (kInstrument)
        {
            rq->setProperty("dials", input({ { "cutoff", 900.0 } }));
            rq->setProperty("fields", juce::Array<juce::var>{ "env", "held", "ic*" });
            juce::Array<juce::var> phrase{ input({ { "note", 57 }, { "at", 0.0 }, { "len", 0.4 } }),
                                           input({ { "note", 64 }, { "at", 0.2 }, { "len", 0.4 }, { "vel", 0.6 } }) };
            rq->setProperty("scenario", input({ { "phrase", phrase },
                                                { "automate", juce::Array<juce::var>{ input({ { "dial", "resonance" }, { "at", 0.1 }, { "to", 0.7 }, { "over", 0.2 } }) } },
                                                { "wheels", juce::Array<juce::var>{ input({ { "wheel", "mod" }, { "at", 0.3 }, { "value", 0.5 } }) } } }));
        }
        else
        {
            rq->setProperty("dials", input({ { "time", 120.0 } }));
            rq->setProperty("fields", juce::Array<juce::var>{ "at", "glide" });
            rq->setProperty("scenario", input({ { "input", "noise" },
                                                { "automate", juce::Array<juce::var>{ input({ { "dial", "mix" }, { "at", 0.2 }, { "to", 0.8 }, { "over", 0.1 } }) } },
                                                { "host", input({ { "bpm", 100 } }) } }));
        }
        const juce::File requestFile = work.getChildFile("request.json");
        requestFile.replaceWithText(juce::JSON::toString(request));

        // The renderer.
        const Ran ran = render({ folder.getFullPathName().toStdString(), "-r", requestFile.getFullPathName().toStdString(), "--dial",
                                 std::string(cliDial) + "=" + juce::String(cliValue).toStdString(), "-w", w + "same.wav", "-c", w + "same.csv",
                                 "-m", w + "same.json", "-o", w + "same.txt" });
        std::printf("the renderer (exit %d, %.2f s):\n%s", ran.code, ran.seconds, ran.out.c_str());
        P.eq("same.exit", ran.code, 0);
        const std::string report = readText(work.getChildFile("same.txt"));
        const std::string metricsText = readText(work.getChildFile("same.json"));
        const juce::var metrics = juce::JSON::parse(juce::String(metricsText));

        // measure_output and trace's export through MCP, on a processor whose editor holds the example.
        pg::Processor sp;
        {
            pg::Draft d;
            d.files = files;
            sp.editorPort().setDraft(d);
        }
        pg::ChatAgent& sa = sp.chatAgent();
        juce::var asked = copyOf(request);
        asked.getDynamicObject()->removeProperty("fields");
        asked.getDynamicObject()->removeProperty("export");
        asked.getProperty("dials", {}).getDynamicObject()->setProperty(cliDial, cliValue);
        const std::string measured = toolText(sa.mcpCall("measure_output", asked));
        std::printf("measure_output:\n%s\n", measured.c_str());
        const std::string lead = "The dials not named are at their defaults.\n";
        P.eq("same.report", !report.empty() && measured.rfind(lead, 0) == 0 && withoutCpu(measured.substr(lead.size())) == withoutCpu(report) ? 1 : 0, 1);
        if (withoutCpu(measured.substr(std::min(lead.size(), measured.size()))) != withoutCpu(report))
            std::printf("the renderer's report:\n%s\n", report.c_str());

        // The numbers: a Session here, on the request as the tools read it.
        std::string why;
        ms::Request req = pg::scenario::renderOf(asked, &why, pg::scenario::forNumbers());
        P.eq("same.request_read", why.empty() ? 1 : 0, 1);
        ms::Session session(files, req.rate);
        const ms::Result here = session.measure(req);
        int same = 0, compared = 0;
        std::string differ;
        for (const auto& [name, value] : here.values)
        {
            if (name.rfind("cpu", 0) == 0)
                continue;
            ++compared;
            double got = 0.0;
            if (metricIn(metricsText, name, got) && (got == value || (std::isnan(got) && std::isnan(value))))
                ++same;
            else
                differ += " " + name;
        }
        const int listed = metrics.getProperty("metrics", {}).getDynamicObject() != nullptr
                               ? metrics.getProperty("metrics", {}).getDynamicObject()->getProperties().size() : 0;
        std::printf("metrics: %d of %d the same doubles (the CPU's aside; the JSON lists %d)%s%s\n", same, compared, listed,
                    differ.empty() ? "" : "; differ:", differ.c_str());
        P.ge("same.metrics_compared", compared, 15);
        P.eq("same.metrics", same, compared);
        P.eq("same.metrics_listed", listed, static_cast<int>(here.values.size()));
        const std::string hash = metrics.getProperty("hash", "").toString().toStdString();
        P.eq("same.hash", hash.size() == 64 && hash == here.hash && hashIn(measured) == hash.substr(0, ms::kHashShown) ? 1 : 0, 1);

        // trace's export of the same render (no window: the renderer leaves it to the metrics).
        juce::var traced = copyOf(request);
        traced.getDynamicObject()->removeProperty("window");
        traced.getDynamicObject()->setProperty("export", input({ { "every", 480 }, { "name", "probe render" } }));
        traced.getProperty("dials", {}).getDynamicObject()->setProperty(cliDial, cliValue);
        const std::string exported = toolText(sa.mcpCall("trace", traced));
        std::printf("trace with export:\n%s\n", exported.c_str());
        const juce::File renders = root.getChildFile("renders");
        const std::vector<uint8_t> cliWav = readBytes(work.getChildFile("same.wav")), toolWav = readBytes(renders.getChildFile("probe render.wav"));
        const std::vector<uint8_t> cliCsv = readBytes(work.getChildFile("same.csv")), toolCsv = readBytes(renders.getChildFile("probe render.csv"));
        std::printf("WAV %zu and %zu bytes, CSV %zu and %zu bytes (the renderer's, trace's)\n", cliWav.size(), toolWav.size(), cliCsv.size(),
                    toolCsv.size());
        P.eq("same.wav", !cliWav.empty() && cliWav == toolWav ? 1 : 0, 1);
        P.eq("same.csv", !cliCsv.empty() && cliCsv == toolCsv ? 1 : 0, 1);
        ms::Wav wav;
        std::string bad;
        const bool read = ms::readWav(cliWav, wav, bad);
        const std::string wavHash = metrics.getProperty("wav", {}).getProperty("hash", "").toString().toStdString();
        P.eq("same.wav_hash", read && ms::renderHash(wav.l, wav.r) == wavHash && hashIn(exported) == wavHash.substr(0, ms::kHashShown) ? 1 : 0, 1);
        if constexpr (kInstrument)   // the same render as measure_output's (the voices on the absolute grid)
            P.eq("same.wav_is_measured", !wavHash.empty() && wavHash == hash ? 1 : 0, 1);
        P.eq("same.csv_rows", static_cast<int>(metrics.getProperty("csv", {}).getProperty("rows", 0)) > 0
                                  && has(ran.out, "a row every 480 samples") ? 1 : 0, 1);
    }

    // ---- EXITS --------------------------------------------------------------------------------------------------------
    {
        const juce::File broken = work.getChildFile("broken");
        writeProgram(broken, program("", "        int x = ;\n"));
        const Ran r = render({ broken.getFullPathName().toStdString() });
        std::printf("does not compile (exit %d): %s", r.code, r.out.c_str());
        P.eq("exit.compile_error", r.code, 2);
        P.eq("exit.compile_error_said", has(r.out, "program.fm:") && has(r.out, "error:") ? 1 : 0, 1);
    }
    {
        const juce::File spins = work.getChildFile("spins");
        writeProgram(spins, program("    int count;\n    int spins;\n\n",
                                    "        count = count + n;\n        if (count > 12000)\n            while (1)\n"
                                    "                spins = spins + 1;\n"));
        const Ran r = render({ spins.getFullPathName().toStdString(), "-r", "{\"hold_seconds\": 0.5, \"tail_seconds\": 0.1}", "-w",
                               w + "spins.wav", "-o", w + "spins.txt" });
        const std::string report = readText(work.getChildFile("spins.txt"));
        std::printf("loops for ever after 12000 samples (exit %d, %.2f s): %s%s", r.code, r.seconds, r.out.c_str(), report.substr(0, 400).c_str());
        P.eq("exit.loop_stop", r.code, 3);
        P.eq("exit.loop_stop_said", has(r.out, "ran past its budget") && report.rfind("Stopped: program.fm:", 0) == 0 ? 1 : 0, 1);
        ms::Wav wav;
        std::string bad;
        P.eq("exit.loop_stop_wav", ms::readWav(readBytes(work.getChildFile("spins.wav")), wav, bad) && wav.l.size() == 28800 ? 1 : 0, 1);
    }
    {
        const juce::File outside = work.getChildFile("outside");
        writeProgram(outside, program("    float table[4];\n    int calls;\n\n",
                                      "        calls = calls + 1;\n        if (calls > 50)\n            table[calls] = 1.0f;\n"));
        const Ran r = render({ outside.getFullPathName().toStdString(), "-q" });
        std::printf("reaches past its array (exit %d): %s", r.code, r.out.c_str());
        P.eq("exit.bounds_stop", r.code, 3);
        P.eq("exit.bounds_stop_said", has(r.out, "went outside an array") ? 1 : 0, 1);
    }
    {
        const juce::File stuck = work.getChildFile("stuck");
        writeProgram(stuck, program("    int spins;\n\n", "", "    void init(float rate)\n    {\n        while (1)\n            spins = spins + 1;\n    }\n\n"));
        const Ran r = render({ stuck.getFullPathName().toStdString(), "-w", w + "stuck.wav" });
        std::printf("init never ends (exit %d, %.1f s): %s", r.code, r.seconds, r.out.c_str());
        P.eq("exit.init_stop", r.code, 3);
        P.eq("exit.init_stop_nothing", has(r.out, "init() ran past its budget") && !work.getChildFile("stuck.wav").exists() ? 1 : 0, 1);
    }
    {
        const juce::File plain = work.getChildFile("plain");
        writeProgram(plain, program("", ""));
        const std::string at = plain.getFullPathName().toStdString();
        const Ran tooLong = render({ at, "-r", "{\"hold_seconds\": 400}" });
        const Ran recording = render({ at, "-r", kInstrument ? "{\"scenario\": {\"phrase\": \"capture\"}}" : "{\"scenario\": {\"input\": \"capture\"}}" });
        const Ran notJson = render({ at, "-r", "{hold_seconds: 1" });
        const Ran noProgram = render({ work.getFullPathName().toStdString() });
        const Ran noOption = render({ at, "--loudly" });
        std::printf("refused: %s%s%s%s%s", tooLong.out.c_str(), recording.out.c_str(), notJson.out.c_str(), noProgram.out.c_str(), noOption.out.c_str());
        P.eq("exit.too_long", tooLong.code == 1 && has(tooLong.out, "hold_seconds is at most 300 s") ? 1 : 0, 1);
        P.eq("exit.recording", recording.code == 1 && has(recording.out, "the user's recording") ? 1 : 0, 1);
        P.eq("exit.not_json", notJson.code == 1 && has(notJson.out, "not JSON") ? 1 : 0, 1);
        P.eq("exit.no_program", noProgram.code == 1 && has(noProgram.out, "has no program.fm") ? 1 : 0, 1);
        P.eq("exit.no_option", noOption.code == 1 && has(noOption.out, "no option --loudly") ? 1 : 0, 1);
        // What lasts ends by the cap, as the tools take it: a glide past 300 s, and an instrument's note, refused.
        const Ran glide = render({ at, "-r", "{\"scenario\": {\"automate\": [{\"dial\": \"level\", \"at\": 295, \"to\": 0.5, \"over\": 20}]}}" });
        std::printf("a glide past the cap (exit %d): %s", glide.code, glide.out.c_str());
        P.eq("exit.glide_end", glide.code == 1 && has(glide.out, "scenario.automate[0]: at + over is 0 to 300 s (this move ends at 315 s)") ? 1 : 0, 1);
        if constexpr (kInstrument)
        {
            const Ran note = render({ at, "-r", "{\"scenario\": {\"phrase\": [{\"note\": 57, \"at\": 299, \"len\": 5}]}}" });
            std::printf("a note past the cap (exit %d): %s", note.code, note.out.c_str());
            P.eq("exit.note_end", note.code == 1 && has(note.out, "scenario.phrase[0]: at + len is 0 to 300 s (this note ends at 304 s)") ? 1 : 0, 1);
        }
    }
    {
        // A program that reads a sample (#pragma sample): the plugin's own (its session, its projects), which no folder
        // holds, so the renderer refuses it rather than render it silent where it reads one; its fields still list.
        const juce::File sampled = work.getChildFile("sampled");
        writeProgram(sampled, "#include \"sample.fm\"\n#pragma sample hit \"kick.wav\"\n" + program("    Sample hit;\n", ""));
        const Ran r = render({ sampled.getFullPathName().toStdString(), "-w", w + "sampled.wav" });
        const Ran listed = render({ sampled.getFullPathName().toStdString(), "--list-fields" });
        std::printf("reads a sample (exit %d): %s", r.code, r.out.c_str());
        P.eq("exit.samples", r.code == 1 && has(r.out, "reads samples (#pragma sample: 'kick.wav')") && has(r.out, "not in a folder")
                                 && !work.getChildFile("sampled.wav").exists() ? 1 : 0, 1);
        P.eq("exit.samples_fields_listed", listed.code == 0 && has(listed.out, "hit.frames") ? 1 : 0, 1);
    }

    // ---- MORE ---------------------------------------------------------------------------------------------------------
    {
        const juce::File lib = work.getChildFile("library");
        writeProgram(lib, "#include \"noise.fm\"\n" + program("", ""));
        const Ran r = render({ lib.getFullPathName().toStdString(), "-q" });
        std::printf("a library file it lacks (exit %d): %s", r.code, r.out.c_str());
        P.eq("more.library_added", r.code == 0 && has(r.out, "noise.fm is the library's") ? 1 : 0, 1);
        const Ran listed = render({ work.getChildFile("spins").getFullPathName().toStdString(), "--list-fields" });
        P.eq("more.list_fields", listed.code == 0 && has(listed.out, "count int") && has(listed.out, "spins int") ? 1 : 0, 1);
        // measure_output's profile (dsp/Profile.h): its line in the report, read as the tool reads it.
        const Ran profiled = render({ lib.getFullPathName().toStdString(), "-r", "{\"profile\": true, \"hold_seconds\": 0.3}" });
        P.eq("more.profile", profiled.code == 0 && has(profiled.out, "\nProfile") ? 1 : 0, 1);
    }
    if constexpr (!kInstrument)
    {
        // The effect's own input: a WAV of a decaying 440 Hz tone at 44.1 kHz, rendered at its rate.
        std::vector<float> l(22050), r(22050);
        for (std::size_t i = 0; i < l.size(); ++i)
        {
            l[i] = 0.5f * std::sin(6.2831853f * 440.0f * static_cast<float>(i) / 44100.0f) * std::exp(-static_cast<float>(i) / 8000.0f);
            r[i] = -l[i];
        }
        std::vector<uint8_t> bytes;
        ms::writeWav([&](const void* d, std::size_t n) {
            bytes.insert(bytes.end(), static_cast<const uint8_t*>(d), static_cast<const uint8_t*>(d) + n);
            return true;
        }, l, r, 44100.0, ms::WavFormat::float32);
        const juce::File in = work.getChildFile("in.wav");
        in.replaceWithData(bytes.data(), bytes.size());
        const juce::File plain = work.getChildFile("plain");
        const Ran ran = render({ plain.getFullPathName().toStdString(), "-i", in.getFullPathName().toStdString(), "-w", w + "own.wav",
                                 "-m", w + "own.json", "-q" });
        std::printf("its own input (exit %d): %s", ran.code, ran.out.c_str());
        ms::TraceRequest q;
        q.render = pg::scenario::forNumbers();
        q.render.rate = 44100.0;
        auto own = std::make_shared<ms::Input>();
        own->l = l;
        own->r = r;
        q.render.input = own;
        q.exporting = true;
        const ms::TraceResult t = ms::trace(pg::singleFile(program("", "")), q);
        ms::Wav wav;
        std::string bad;
        const bool read = ms::readWav(readBytes(work.getChildFile("own.wav")), wav, bad);
        P.eq("more.input_rendered", ran.code == 0 && read && wav.rate == 44100 && wav.l.size() == t.l.size()
                                        && ms::renderHash(wav.l, wav.r) == t.hash ? 1 : 0, 1);
        const juce::var m = juce::JSON::parse(juce::String(readText(work.getChildFile("own.json"))));
        P.eq("more.input_measured", has(m.getProperty("hashed", "").toString().toStdString(), "own input") ? 1 : 0, 1);
    }
    // ---- NOTES --------------------------------------------------------------------------------------------------------
    {
        // A dial the program has not, by --dial and by a scenario's move: on stderr, and in the metrics, measured or not.
        const std::string at = work.getChildFile("plain").getFullPathName().toStdString();
        const Ran measured = render({ at, "-d", "nosuch=3", "-r", "{\"hold_seconds\": 0.3, \"tail_seconds\": 0.1}", "-q", "-m",
                                      w + "unknown.json" });
        std::printf("a dial it has not (exit %d): %s", measured.code, measured.out.c_str());
        P.eq("notes.unknown_dial", measured.code == 0 && has(measured.out, "note: no dial is named 'nosuch' (ignored)")
                                       && strings(jsonFile(work.getChildFile("unknown.json")).getProperty("unknown_dials", {}))
                                              == std::vector<std::string>{ "nosuch" } ? 1 : 0, 1);
        const Ran unmeasured = render({ at, "-d", "nosuch=3", "--no-measure", "-w", w + "unknown.wav", "-m", w + "unknown2.json", "-r",
                                        "{\"hold_seconds\": 0.3, \"tail_seconds\": 0.1, \"scenario\": {\"automate\": [{\"dial\": \"zzz\", "
                                        "\"at\": 0.1, \"to\": 0.5, \"over\": 0.1}]}}" });
        std::printf("--no-measure (exit %d): %s", unmeasured.code, unmeasured.out.c_str());
        P.eq("notes.unknown_dial_unmeasured", unmeasured.code == 0 && has(unmeasured.out, "note: no dial is named 'nosuch' (ignored)")
                                                  && has(unmeasured.out, "note: no dial is named 'zzz' (ignored)")
                                                  && strings(jsonFile(work.getChildFile("unknown2.json")).getProperty("unknown_dials", {}))
                                                         == std::vector<std::string>{ "nosuch", "zzz" } ? 1 : 0, 1);

        // Fields: 14 matched by f*, 2 past the CSV's 12 columns (f1* matches those two again: counted once), one pattern none.
        const juce::File many = work.getChildFile("many");
        writeProgram(many, program("    float f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13;\n\n", ""));
        const Ran fields = render({ many.getFullPathName().toStdString(), "-f", "f*", "-f", "f1*", "-f", "nosuch", "-c", w + "many.csv",
                                    "--no-measure", "-m", w + "many.json", "-r", "{\"hold_seconds\": 0.3, \"tail_seconds\": 0.1}" });
        std::printf("fields (exit %d): %s", fields.code, fields.out.c_str());
        const juce::var csv = jsonFile(work.getChildFile("many.json")).getProperty("csv", {});
        P.eq("notes.field_unmatched", fields.code == 0 && has(fields.out, "note: no field matches nosuch")
                                          && strings(csv.getProperty("unmatched", {})) == std::vector<std::string>{ "nosuch" } ? 1 : 0, 1);
        P.eq("notes.fields_past_columns", has(fields.out, "note: 2 more fields matched, past the 12 columns")
                                              && strings(csv.getProperty("columns", {})).size() == ms::kMaxTraceWatched
                                              && static_cast<int>(csv.getProperty("more_matched", -1)) == 2 ? 1 : 0, 1);
    }
#if !defined(_WIN32)
    {
        // The request through a pipe (as <(...) gives it): a FIFO, written once the renderer opens it.
        const juce::File fifo = work.getChildFile("request.fifo");
        fifo.deleteFile();
        const std::string path = fifo.getFullPathName().toStdString();
        int code = -1;
        bool wrote = false;
        std::string out;
        juce::ChildProcess p;
        if (::mkfifo(path.c_str(), 0600) == 0
            && p.start(juce::StringArray{ juce::String::fromUTF8(rendererPath().c_str()), work.getChildFile("plain").getFullPathName(), "-r",
                                          juce::String::fromUTF8(path.c_str()), "--no-measure", "-m",
                                          juce::String::fromUTF8((w + "fifo.json").c_str()) },
                       juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            int fd = -1;                             // no reader yet: ENXIO, so try again while it runs
            for (int k = 0; k < 2000 && fd < 0 && p.isRunning(); ++k)
                if ((fd = ::open(path.c_str(), O_WRONLY | O_NONBLOCK)) < 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (fd >= 0)
            {
                const std::string json = "{\"hold_seconds\": 0.3, \"dials\": {\"piped\": 1}}";
                wrote = ::write(fd, json.data(), json.size()) == static_cast<ssize_t>(json.size());
                // Held open a moment: on macOS a reader still asleep in open() when the writer closes stays asleep
                // (it wakes only while it sees a writer), so a write and close at once can leave the renderer there.
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                ::close(fd);
            }
            // Waited for with a limit, never read to its end first: a renderer left in open() is ended, not waited on.
            const bool ended = p.waitForProcessToFinish(60000);
            if (!ended)
                p.kill();
            out = p.readAllProcessOutput().toStdString();
            if (ended)
                code = static_cast<int>(p.getExitCode());
        }
        std::printf("the request through a pipe (exit %d): %s", code, out.c_str());
        P.eq("notes.request_pipe", wrote && code == 0
                                       && strings(jsonFile(work.getChildFile("fifo.json")).getProperty("unknown_dials", {}))
                                              == std::vector<std::string>{ "piped" } ? 1 : 0, 1);
        fifo.deleteFile();
    }
#endif
    {
        // A folder the editor's FOLDER syncs, of a program of kMaxFiles files: the sync adds its read-only copies,
        // host.fm and the library's noise.fm, which the renderer leaves out as the plugin does (noise.fm added again).
        pg::Draft draft;
        draft.files.push_back({ pg::kProgramName, "#include \"noise.fm\"\n" + program("", "") });
        for (std::size_t k = 1; k < pg::kMaxFiles; ++k)
            draft.files.push_back({ "part" + std::to_string(k) + ".fm", "// part " + std::to_string(k) + "\n" });
        pg::FolderSync sync({ [&draft] { return draft; },
                              [&draft](const pg::Draft&, const pg::Draft& next) {
                                  draft = next;
                                  return true;
                              },
                              [](const pg::Sources&) {} });
        const std::filesystem::path syncRoot(work.getChildFile("synced").getFullPathName().toStdString());
        std::string why;
        double t = 1000.0;
        const bool on = sync.start(syncRoot, "Program", t, why);
        for (const double end = t + 0.6; t < end; t += 0.05)
            sync.pass(t, true);
        const std::filesystem::path dir = sync.folder();
        std::size_t fm = 0;
        std::error_code ec;
        for (const std::filesystem::directory_entry& e : std::filesystem::directory_iterator(dir, ec))
            fm += e.path().extension() == ".fm" ? 1 : 0;
        std::printf("FOLDER: %s, %zu .fm files%s\n", dir.string().c_str(), fm, why.c_str());
        P.eq("notes.folder_sync_copies", on && fm == pg::kMaxFiles + 2 && std::filesystem::exists(dir / "host.fm")
                                             && std::filesystem::exists(dir / "noise.fm") ? 1 : 0, 1);
        const Ran synced = render({ dir.string(), "-q", "-m", w + "synced.json" });
        const std::vector<std::string> names = strings(jsonFile(work.getChildFile("synced.json")).getProperty("program", {}));
        std::printf("a synced folder (exit %d): %s", synced.code, synced.out.c_str());
        const auto lists = [](const std::vector<std::string>& v, const char* n) { return std::find(v.begin(), v.end(), n) != v.end(); };
        P.eq("notes.folder_sync", synced.code == 0 && names.size() == pg::kMaxFiles + 1 && !names.empty() && names.front() == pg::kProgramName
                                      && names.back() == "noise.fm" && !lists(names, "host.fm") && has(synced.out, "noise.fm is the library's")
                                      ? 1 : 0, 1);
        // A copy as another build wrote it (host.fm, not this build's text): left out, saying so.
        std::filesystem::permissions(dir / "host.fm", std::filesystem::perms::owner_write, std::filesystem::perm_options::add, ec);
        juce::File(juce::String::fromUTF8((dir / "host.fm").string().c_str())).appendText("// an older build's\n", false, false, "\n");
        const Ran stale = render({ dir.string(), "-q", "-m", w + "stale.json" });
        std::printf("a copy from another build (exit %d): %s", stale.code, stale.out.c_str());
        P.eq("notes.folder_sync_stale", stale.code == 0 && has(stale.out, "host.fm is the folder sync's read-only copy, as another build")
                                            && !lists(strings(jsonFile(work.getChildFile("stale.json")).getProperty("program", {})), "host.fm")
                                            ? 1 : 0, 1);
        sync.stop(false);
    }
    if constexpr (!kInstrument)
    {
        // The effect's own input as WAVE_FORMAT_EXTENSIBLE: 24-bit stereo at 96 kHz, rendered at its rate.
        std::vector<float> l(9600), r(9600);
        for (std::size_t i = 0; i < l.size(); ++i)
        {
            l[i] = 0.5f * std::sin(6.2831853f * 440.0f * static_cast<float>(i) / 96000.0f);
            r[i] = -l[i];
        }
        const std::vector<uint8_t> ext = extensibleWav(l, &r, 96000, 24);
        const juce::File in = work.getChildFile("ext24.wav");
        in.replaceWithData(ext.data(), ext.size());
        const std::string at = work.getChildFile("plain").getFullPathName().toStdString();
        const Ran ran = render({ at, "-i", in.getFullPathName().toStdString(), "-w", w + "ext-out.wav", "--no-measure", "-r",
                                 "{\"scenario\": {\"rate\": \"host\"}}" });
        std::printf("an extensible WAV in (exit %d): %s", ran.code, ran.out.c_str());
        ms::Wav wav;
        std::string bad;
        P.eq("notes.input_extensible", ran.code == 0 && ms::readWav(readBytes(work.getChildFile("ext-out.wav")), wav, bad) && wav.rate == 96000
                                           ? 1 : 0, 1);
        P.eq("notes.input_host_rate", has(ran.out, "\"host\" rate is 96000 Hz (the input's)") ? 1 : 0, 1);

        // One past what a render plays of its own input (310 s up to 96 kHz): 311 s at 8 kHz, refused, nothing written.
        const std::vector<uint8_t> along = extensibleWav(std::vector<float>(311 * 8000), nullptr, 8000, 16);
        const juce::File longIn = work.getChildFile("long.wav");
        longIn.replaceWithData(along.data(), along.size());
        const Ran tooLong = render({ at, "-i", longIn.getFullPathName().toStdString(), "-w", w + "long-out.wav", "--no-measure" });
        std::printf("an input past the render (exit %d): %s", tooLong.code, tooLong.out.c_str());
        P.eq("notes.input_too_long", tooLong.code == 1 && has(tooLong.out, "lasts 311.00 s, past the 310 s a render plays of its own input")
                                         && !work.getChildFile("long-out.wav").exists() ? 1 : 0, 1);
        longIn.deleteFile();
    }
    {
        const juce::File mcpb = juce::File(juce::String::fromUTF8(rendererPath().c_str())).getSiblingFile(juce::String(pg::product::kName) + ".mcpb");
        juce::ZipFile zip(mcpb);
        bool carried = false;
        for (int k = 0; k < zip.getNumEntries(); ++k)
            carried = carried || zip.getEntry(k)->filename == "server/" + juce::String(pg::product::kName) + "Render" + kExe;
        P.eq("more.in_extension", carried ? 1 : 0, 1);
    }
    work.getChildFile("chat-keys").deleteRecursively();
    return P.finish();
}
