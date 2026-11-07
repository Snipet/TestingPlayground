// Source/render/Main.cpp: <Name>Render, the headless renderer: a program rendered and measured from the command line,
// with the plugin's own compiler (FMinus's JIT through fminus/Patch.h) and its own offline render (dsp/Measure.h's
// Session), so that what it gives is what the chat's tools give for the same files and settings, bit for bit:
//
//   the report and the metrics   measure_output's (measure::Session::measure): the same words, the same numbers, the
//                                same render hash (the CPU times aside, the rendering thread's own)
//   the WAV and the field log    trace's export (measure::trace, exporting): the same render in calls of 16 samples
//                                (an instrument's is measure_output's, the same bits), the same CSV
//
// Its input is what the tools take (plugin/ChatScenario.h reads it, the file this program compiles with the plugin):
// the program's files (a folder, or the files), and the render's settings as JSON (measure_output's: notes, velocity,
// hold_seconds, tail_seconds, bpm, modwheel, pitchbend, dials, dials_from, window, base_hz, spectrum, scenario,
// try_edits; and trace's fields, voice and export's wav and every), with a few options over it (--dial, --field,
// --every, --pcm24) and an effect's own input as a WAV (--input). There is no plugin here: the dials not named are at
// the program's defaults, a scenario's "host" rate and block are the tools' defaults (48 kHz, 256), and a "capture" (the
// user's recording, which never leaves the plugin) is refused. A library file the program #includes and the files lack
// is added as LIBRARY adds it (fminus/LibraryIncludes.h), saying so. There is no time limit.
//
// Exit codes: 0 rendered; 1 a usage, input or output error (a request the tools would refuse too, a file that could not
// be read or written); 2 the program did not compile (the compiler's messages on stderr); 3 the program stopped (a call
// past its loop budget or a failed bounds check, Patch.h: the report and the files are still written, the program faded
// out from there, as the plugin plays it; an init's stop renders nothing). The report goes to stdout (or --report's
// file), every other line to stderr.
//
// juce_core (juce::var, JSON) and the DSP library: no processor, no editor, no FunkGui (cmake/LintLayers.cmake).
#include "plugin/ChatScenario.h"

#include "dsp/Debug.h"
#include "dsp/Export.h"
#include "dsp/Measure.h"
#include "dsp/Sources.h"
#include "fminus/LibraryIncludes.h"

#include "Product.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace pg = playground;
namespace ms = playground::measure;
namespace fs = std::filesystem;

namespace
{
    constexpr bool kInstrument = pg::product::kIsInstrument;
    constexpr int kOk = 0, kInputError = 1, kNotCompiled = 2, kStopped = 3;
    // A program's file at most (the folder sync's, plugin/FolderSync.h: a larger one is not the program's).
    constexpr std::uintmax_t kMaxFileBytes = std::uintmax_t{ 1 } << 20;
    // The request's properties the renderer reads (above); any other is named on stderr (the tools ignore one).
    constexpr const char* kKnown[] = { "notes", "velocity", "hold_seconds", "tail_seconds", "bpm", "modwheel", "pitchbend",
                                       "dials", "dials_from", "window", "base_hz", "spectrum", "scenario", "try_edits",
                                       "fields", "voice", "export" };

    std::string self() { return std::string(pg::product::kName) + "Render"; }

    void say(const std::string& line) { std::fprintf(stderr, "%s\n", line.c_str()); }

    int fail(int code, const std::string& why)
    {
        say(self() + ": " + why);
        return code;
    }

    std::string usage()
    {
        const std::string effectInput =
            kInstrument ? std::string() : "  -i, --input <file.wav>  the effect's input, played through it (the render at its rate)\n";
        return "usage: " + self() + " [options] <program folder | file...>\n"
               "Renders " + std::string(pg::product::kName) + "'s program offline, as its tools do, with its own compiler.\n"
               "\n"
               "The program: a folder (its program.fm first, then its .fm, .cir, .gui and .sc files by name) or its files\n"
               "(the first the program). The render's settings: measure_output's JSON, and trace's fields, voice and export.\n"
               "\n"
               "  -r, --request <json>    the settings: a JSON file, - for stdin, or the JSON itself ({...})\n"
               "  -d, --dial <name=value> a dial's value, over the request's (repeatable)\n"
               "  -w, --wav <file>        the render as a WAV (trace's export's: float32, or 24-bit PCM with --pcm24)\n"
               "  -c, --csv <file>        the fields (--field, or the request's fields) as a CSV, a row every --every samples\n"
               "  -f, --field <path>      a field to log: a path or a glob (repeatable, 8 at most)\n"
               "  -e, --every <samples>   the CSV's rows: every this many samples, a multiple of 16 (about 1 ms by default)\n"
               + effectInput +
               "  -m, --metrics <file>    the metrics as JSON (- for stdout, in place of the report): values, units, hash\n"
               "  -o, --report <file>     measure_output's report there (- for stdout, the default)\n"
               "      --pcm24             the WAV as 24-bit PCM (clipped at full scale)\n"
               "      --list-fields       name the program's fields (the paths --field takes), and render nothing\n"
               "      --no-measure        no report and no metrics: the WAV and the CSV alone\n"
               "  -q, --quiet             no report on stdout\n"
               "  -h, --help              this; --version: the version\n"
               "\n"
               "Exit codes: 0 rendered; 1 a usage, input or output error; 2 the program did not compile; 3 it stopped (a\n"
               "loop past its budget or an index outside its array: the outputs are still written, faded out from there).\n";
    }

    std::string number(double v, const char* f = "%g")
    {
        char b[64];
        std::snprintf(b, sizeof b, f, v);
        return b;
    }

    std::string sizeText(std::size_t bytes)
    {
        const double b = static_cast<double>(bytes);
        return bytes < 1000 ? std::to_string(bytes) + " bytes"
             : bytes < 1000000 ? number(b / 1e3, "%.0f KB")
                               : number(b / 1e6, "%.1f MB");
    }

    // ---- the program's files ---------------------------------------------------------------------------------------
    bool programFile(const fs::path& p)
    {
        const std::string name = p.filename().string(), ext = p.extension().string();
        std::string lower;
        for (const char c : ext)
            lower += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        return pg::validFileName(name) && (lower == ".fm" || lower == ".cir" || lower == ".gui" || lower == ".sc");
    }

    bool readText(const fs::path& p, std::string& out, std::string& why)
    {
        std::error_code ec;
        const std::uintmax_t size = fs::file_size(p, ec);
        if (ec)
        {
            why = "cannot read " + p.string() + " (" + ec.message() + ")";
            return false;
        }
        if (size > kMaxFileBytes)
        {
            why = p.string() + " is " + sizeText(static_cast<std::size_t>(size)) + ", past the " + sizeText(kMaxFileBytes)
                + " a program's file takes";
            return false;
        }
        std::ifstream in(p, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        if (!in.good() && !in.eof())
        {
            why = "cannot read " + p.string();
            return false;
        }
        out.clear();
        for (const char c : text.str())                  // the editor's line ends (TextEditor::sanitise): LF alone
            if (c != '\r')
                out += c;
        return true;
    }

    // The files named on the command line: one folder, or files (the first the program).
    bool readProgram(const std::vector<std::string>& args, pg::Sources& files, std::string& why)
    {
        std::vector<fs::path> paths;
        if (args.size() == 1 && fs::is_directory(fs::path(args[0])))
        {
            const fs::path dir(args[0]);
            std::error_code ec;
            for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
                if (e.is_regular_file() && programFile(e.path()))
                    paths.push_back(e.path());
            if (ec)
            {
                why = "cannot read the folder " + dir.string() + " (" + ec.message() + ")";
                return false;
            }
            std::sort(paths.begin(), paths.end(), [](const fs::path& a, const fs::path& b) {
                const bool pa = a.filename() == pg::kProgramName, pb = b.filename() == pg::kProgramName;
                return pa != pb ? pa : a.filename().string() < b.filename().string();
            });
            if (paths.empty() || paths.front().filename() != pg::kProgramName)
            {
                why = "the folder " + dir.string() + " has no " + std::string(pg::kProgramName)
                    + " (the program: name its files instead, the program first)";
                return false;
            }
        }
        else
            for (const std::string& a : args)
            {
                const fs::path p(a);
                if (!fs::is_regular_file(p))
                {
                    why = "there is no file " + a + (args.size() > 1 && fs::is_directory(p) ? " (a folder is named alone)" : "");
                    return false;
                }
                if (!pg::validFileName(p.filename().string()))
                {
                    why = "a program's file is named with letters, digits, _, - and . (" + std::to_string(pg::kMaxNameLength)
                        + " at most): " + p.filename().string();
                    return false;
                }
                paths.push_back(p);
            }
        if (paths.size() > pg::kMaxFiles)
        {
            why = "a program holds at most " + std::to_string(pg::kMaxFiles) + " files; these are " + std::to_string(paths.size());
            return false;
        }
        files.clear();
        for (const fs::path& p : paths)
        {
            if (pg::findFile(files, p.filename().string()) != nullptr)
            {
                why = "two files are named " + p.filename().string();
                return false;
            }
            std::string text;
            if (!readText(p, text, why))
                return false;
            files.push_back({ p.filename().string(), std::move(text) });
        }
        return true;
    }

    // ---- the request -----------------------------------------------------------------------------------------------
    bool readRequest(const std::string& arg, juce::var& out, std::string& why)
    {
        std::string text;
        if (arg == "-")
        {
            std::ostringstream all;
            all << std::cin.rdbuf();
            text = all.str();
        }
        else if (const std::size_t first = arg.find_first_not_of(" \t\r\n"); first != std::string::npos && arg[first] == '{')
            text = arg;
        else if (!readText(fs::path(arg), text, why))
            return false;
        juce::var v;
        const juce::String json = juce::String::fromUTF8(text.data(), static_cast<int>(text.size()));
        if (const juce::Result r = juce::JSON::parse(json, v); r.failed())
        {
            why = "the request is not JSON: " + r.getErrorMessage().toStdString();
            return false;
        }
        if (!v.isObject())
        {
            why = "the request is a JSON object: {\"hold_seconds\": 2, \"dials\": {...}, \"scenario\": {...}}";
            return false;
        }
        out = v;
        return true;
    }

    juce::DynamicObject& objectAt(juce::var& in, const char* key)
    {
        juce::var o = in.getProperty(key, juce::var());
        if (!o.isObject())
        {
            o = juce::var(new juce::DynamicObject());
            in.getDynamicObject()->setProperty(key, o);
        }
        return *o.getDynamicObject();
    }

    // ---- output ----------------------------------------------------------------------------------------------------
    // A file written whole through a temporary beside it, then renamed over it (a failed write leaves none).
    bool writeFile(const fs::path& target, const std::function<bool(const ms::Sink&)>& body, std::string& why)
    {
        fs::path temp = target;
        temp += ".part";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                why = "cannot write " + target.string();
                return false;
            }
            const ms::Sink sink = [&out](const void* data, std::size_t bytes) {
                out.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
                return static_cast<bool>(out);
            };
            if (!body(sink) || !out.flush())
            {
                out.close();
                std::error_code ignored;
                fs::remove(temp, ignored);
                why = "writing " + target.string() + " failed (is the disk full?)";
                return false;
            }
        }
        std::error_code ec;
        fs::rename(temp, target, ec);
        if (ec)
        {
            fs::remove(temp, ec);
            why = "cannot move " + target.string() + " into place";
            return false;
        }
        return true;
    }

    bool writeText(const std::string& path, const std::string& text, std::string& why)
    {
        if (path == "-")
        {
            std::fwrite(text.data(), 1, text.size(), stdout);
            std::fflush(stdout);
            return true;
        }
        return writeFile(fs::path(path), [&](const ms::Sink& sink) { return sink(text.data(), text.size()); }, why);
    }

    std::string jsonString(const std::string& s)
    {
        std::string out = "\"";
        for (const char c : s)
        {
            if (c == '"' || c == '\\')
                out += std::string("\\") + c;
            else if (c == '\n')
                out += "\\n";
            else if (c == '\t')
                out += "\\t";
            else if (static_cast<unsigned char>(c) < 0x20)
            {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += b;
            }
            else
                out += c;
        }
        return out + "\"";
    }

    // A value as JSON: 17 significant digits (it reads back to the same double); a value that never came, "inf".
    std::string jsonNumber(double v)
    {
        if (std::isnan(v))
            return "\"nan\"";
        if (std::isinf(v))
            return v > 0.0 ? "\"inf\"" : "\"-inf\"";
        return number(v, "%.17g");
    }

    struct Written
    {
        std::string wav, wavHash, csv;
        std::size_t frames = 0, rows = 0;
        std::vector<std::string> columns;
    };

    std::string metricsJson(const pg::Sources& files, double rate, const ms::Result* res, const Written& w, double seconds)
    {
        std::string out = "{\n  \"renderer\": " + jsonString(self() + " " + pg::product::kVersion) + ",\n  \"program\": [";
        for (std::size_t i = 0; i < files.size(); ++i)
            out += (i == 0 ? "" : ", ") + jsonString(files[i].name);
        out += "],\n  \"rate\": " + jsonNumber(rate);
        if (res != nullptr)
        {
            out += ",\n  \"dials\": " + jsonString(res->dials);
            const bool stopped = res->stopped != pg::Patch::Method::none;
            out += ",\n  \"stopped\": " + (stopped ? jsonString(res->stopWords) : std::string("null"));
            out += ",\n  \"hash\": " + jsonString(res->hash) + ",\n  \"hashed\": " + jsonString(res->hashed);
            std::string values, units;
            for (const ms::MetricInfo& m : ms::metrics())   // in the metrics' order, as the report's line
                if (const auto it = res->values.find(m.name); it != res->values.end())
                {
                    values += std::string(values.empty() ? "" : ",") + "\n    " + jsonString(m.name) + ": " + jsonNumber(it->second);
                    units += std::string(units.empty() ? "" : ", ") + jsonString(m.name) + ": " + jsonString(m.unit);
                }
            out += ",\n  \"metrics\": {" + values + (values.empty() ? "}" : "\n  }");
            out += ",\n  \"units\": {" + units + "}";
            std::string un;
            for (const auto& [name, why] : res->unmeasured)
                un += std::string(un.empty() ? "" : ",") + "\n    " + jsonString(name) + ": " + jsonString(why);
            out += ",\n  \"unmeasured\": {" + un + (un.empty() ? "}" : "\n  }");
        }
        if (!w.wav.empty())
            out += ",\n  \"wav\": {\"file\": " + jsonString(w.wav) + ", \"frames\": " + std::to_string(w.frames) + ", \"hash\": "
                 + jsonString(w.wavHash) + "}";
        if (!w.csv.empty())
        {
            std::string cols;
            for (const std::string& c : w.columns)
                cols += (cols.empty() ? "" : ", ") + jsonString(c);
            out += ",\n  \"csv\": {\"file\": " + jsonString(w.csv) + ", \"rows\": " + std::to_string(w.rows) + ", \"columns\": ["
                 + cols + "]}";
        }
        out += ",\n  \"seconds\": " + number(seconds, "%.3f") + "\n}\n";
        return out;
    }

    // ---- the run ---------------------------------------------------------------------------------------------------
    struct Options
    {
        std::vector<std::string> program;
        std::string request, wav, csv, input, metrics, report = "-";
        std::vector<std::string> dials, fields;
        std::string every;
        bool pcm24 = false, listFields = false, measure = true, quiet = false;
    };

    // The arguments; false (with `code`) when the run ends here: --help, --version, or an error said.
    bool parse(int argc, char** argv, Options& o, int& code)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            std::string v;
            if (a == "-h" || a == "--help")
            {
                std::fputs(usage().c_str(), stdout);
                code = kOk;
                return false;
            }
            if (a == "--version")
            {
                std::printf("%s %s (%s, JUCE %s, FunkGui %s)\n", self().c_str(), pg::product::kVersion, pg::product::kKind,
                            pg::product::kJuceVersion, pg::product::kFunkGuiVersion);
                code = kOk;
                return false;
            }
            const bool flag = a.size() > 1 && a[0] == '-';
            const auto take = [&](std::string& out) {
                if (i + 1 < argc)
                {
                    out = argv[++i];
                    return true;
                }
                code = fail(kInputError, a + " needs a value (" + self() + " --help)");
                return false;
            };
            if (a == "-r" || a == "--request")
            {
                if (!take(o.request))
                    return false;
            }
            else if (a == "-d" || a == "--dial")
            {
                if (!take(v))
                    return false;
                o.dials.push_back(v);
            }
            else if (a == "-w" || a == "--wav")
            {
                if (!take(o.wav))
                    return false;
            }
            else if (a == "-c" || a == "--csv")
            {
                if (!take(o.csv))
                    return false;
            }
            else if (a == "-f" || a == "--field")
            {
                if (!take(v))
                    return false;
                o.fields.push_back(v);
            }
            else if (a == "-e" || a == "--every")
            {
                if (!take(o.every))
                    return false;
            }
            else if (!kInstrument && (a == "-i" || a == "--input"))
            {
                if (!take(o.input))
                    return false;
            }
            else if (a == "-m" || a == "--metrics")
            {
                if (!take(o.metrics))
                    return false;
            }
            else if (a == "-o" || a == "--report")
            {
                if (!take(o.report))
                    return false;
            }
            else if (a == "--pcm24")
                o.pcm24 = true;
            else if (a == "--list-fields")
                o.listFields = true;
            else if (a == "--no-measure")
                o.measure = false;
            else if (a == "-q" || a == "--quiet")
                o.quiet = true;
            else if (flag && a != "-")
            {
                const bool input = a == "-i" || a == "--input";   // an effect's alone
                code = fail(kInputError, "no option " + a + (input ? " (an instrument has no input)" : "") + " (" + self() + " --help)");
                return false;
            }
            else
                o.program.push_back(a);
        }
        if (o.program.empty())
        {
            std::fputs(usage().c_str(), stderr);
            code = kInputError;
            return false;
        }
        return true;
    }

    int run(int argc, char** argv)
    {
        const auto began = std::chrono::steady_clock::now();
        Options o;
        int code = kOk;
        if (!parse(argc, argv, o, code))
            return code;
        std::string why;

        // The program's files (the library's it #includes and they lack are added below, once the request is read).
        pg::Sources files;
        if (!readProgram(o.program, files, why))
            return fail(kInputError, why);

        // The request: the JSON, then the options over it, read by the tools' own readers.
        juce::var input(new juce::DynamicObject());
        if (!o.request.empty() && !readRequest(o.request, input, why))
            return fail(kInputError, why);
        for (const auto& kv : input.getDynamicObject()->getProperties())
            if (std::none_of(std::begin(kKnown), std::end(kKnown), [&](const char* k) { return kv.name.toString() == k; }))
                say("note: the request's " + kv.name.toString().toStdString() + " is not one the renderer reads; left out");
        for (const std::string& d : o.dials)
        {
            const std::size_t eq = d.find('=');
            const std::string name = d.substr(0, eq), text = eq == std::string::npos ? std::string() : d.substr(eq + 1);
            char* end = nullptr;
            const double x = std::strtod(text.c_str(), &end);
            juce::var value;
            if (text == "true" || text == "false")
                value = text == "true";
            else if (!text.empty() && end != nullptr && *end == '\0' && std::isfinite(x))
                value = x;
            if (name.empty() || value.isVoid())
                return fail(kInputError, "--dial is name=value, the value a number (or true, false): " + d);
            objectAt(input, "dials").setProperty(juce::Identifier(juce::String::fromUTF8(name.c_str())), value);
        }
        if (!o.fields.empty())
        {
            juce::var list = input.getProperty("fields", juce::var());
            if (!list.isArray())
                list = juce::var(juce::Array<juce::var>());
            for (const std::string& f : o.fields)
                list.append(juce::String::fromUTF8(f.c_str()));
            input.getDynamicObject()->setProperty("fields", list);
        }
        if (!o.every.empty() || o.pcm24)
        {
            if (const juce::var e = input.getProperty("export", juce::var()); !e.isObject())   // true, or none
                input.getDynamicObject()->setProperty("export", juce::var(new juce::DynamicObject()));
            if (!o.every.empty())
            {
                char* end = nullptr;
                const double n = std::strtod(o.every.c_str(), &end);
                if (end == nullptr || *end != '\0' || !(n >= 1.0))
                    return fail(kInputError, "--every is a number of samples, 16 or more: " + o.every);
                objectAt(input, "export").setProperty("every", n);
            }
            if (o.pcm24)
                objectAt(input, "export").setProperty("wav", "24");
        }

        ms::Request req = pg::scenario::renderOf(input, &why, pg::scenario::forNumbers());   // up to 300 s, as measure_output
        bool defaults = false;
        int trials = 0;
        pg::scenario::ExportAsk ask;
        ms::TraceRequest q;
        if (why.empty())
            pg::scenario::dialsFrom(input, defaults, why);
        if (why.empty())
            pg::scenario::spectrumOf(input, req.spectrum, why);
        if (why.empty())
            pg::scenario::watchOf(input, q, why);
        if (why.empty())
            pg::scenario::exportOf(input, ask, why);
        if (why.empty())
            pg::scenario::tryEdits(input, files, trials, why);
        if (!why.empty())
            return fail(kInputError, why);
        if (req.fromCapture)
            return fail(kInputError, "the scenario names \"capture\", the user's recording, which stays in the plugin: give a "
                                     "phrase (or an input) of your own");
        if (req.hostRate || req.hostBlock)
            say(std::string("note: there is no host here: the scenario's \"host\" ") + (req.hostRate ? "rate is 48000 Hz" : "")
                + (req.hostRate && req.hostBlock ? " and its " : "") + (req.hostBlock ? "block 256 samples" : "")
                + ", the tools' defaults");
        if (ask.on && o.wav.empty() && o.csv.empty())
            say("note: the request's export writes nothing here: --wav and --csv name the files");
        if (trials > 0)
            say("note: with " + std::to_string(trials) + (trials == 1 ? " trial edit" : " trial edits") + " (try_edits)");

        // An effect's own input (a WAV): the render at its rate.
        if (!o.input.empty())
        {
            std::ifstream in(fs::path(o.input), std::ios::binary);
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            ms::Wav wav;
            if (bytes.empty())
                return fail(kInputError, "cannot read " + o.input);
            if (!ms::readWav(bytes, wav, why))
                return fail(kInputError, o.input + ": " + why);
            const juce::var rate = input.getProperty("scenario", juce::var()).getProperty("rate", juce::var());
            if (!rate.isVoid() && !rate.isUndefined() && !req.hostRate && std::abs(req.rate - wav.rate) > 0.5)
                return fail(kInputError, o.input + " is at " + std::to_string(wav.rate) + " Hz and the scenario's rate is "
                                             + number(req.rate) + " Hz: the renderer does not resample (leave the rate out)");
            if (wav.rate < pg::scenario::kMinRate || wav.rate > pg::scenario::kMaxRate)
                return fail(kInputError, o.input + " is at " + std::to_string(wav.rate)
                                             + " Hz: a render's rate is 8000 to 192000 Hz");
            req.rate = wav.rate;
            auto own = std::make_shared<ms::Input>();
            own->l = std::move(wav.l);
            own->r = wav.channels > 1 ? std::move(wav.r) : std::vector<float>{};
            req.input = std::move(own);
        }
        if (!pg::scenario::fits(req, why))
            return fail(kInputError, why);

        for (const pg::SourceFile& f : pg::withLibraryIncludes(files))
            if (pg::findFile(files, f.name) == nullptr)
            {
                files.push_back(f);
                say("note: " + f.name + " is the library's (the program #includes it; in the plugin LIBRARY adds it)");
            }

        // The compile: measure_output's Session, which measures (and says whether it compiled; an init's stop, Patch.h,
        // is a stop).
        ms::Session session(files, req.rate);
        if (!session.compiled())
        {
            std::fputs(session.errors().c_str(), stderr);
            return session.initStopped() != pg::Patch::Method::none ? kStopped : kNotCompiled;
        }
        bool stopped = false;

        // The field map, alone.
        if (o.listFields)
        {
            q.render = req;
            q.list = true;
            const ms::TraceResult r = ms::trace(files, q);
            if (!r.error.empty())
                return fail(kNotCompiled, r.error);
            for (const std::string& line : r.lines)
                std::printf("%s\n", line.c_str());
            return kOk;
        }

        // The report and the metrics: measure_output's.
        std::unique_ptr<ms::Result> res;
        if (o.measure)
        {
            res = std::make_unique<ms::Result>(session.measure(req));
            stopped = res->stopped != pg::Patch::Method::none;
            if (stopped)
                std::fputs(res->stopWords.c_str(), stderr);
        }

        // The WAV and the field log: trace's export.
        Written w;
        if (!o.wav.empty() || !o.csv.empty())
        {
            if (!o.csv.empty() && q.fields.empty())
                return fail(kInputError, "--csv needs fields: --field (or the request's fields); --list-fields names them");
            q.render = req;
            q.render.window = {};                        // the whole render, as trace takes it (a window is the metrics')
            q.exporting = true;
            q.seconds = 1e9;                             // no time limit
            const std::size_t every = pg::scenario::everyOf(ask.every, req.rate);
            q.every = q.fields.empty() ? 0 : every;
            const ms::TraceResult r = ms::trace(files, q);
            if (!r.error.empty())
                return fail(kNotCompiled, r.error);
            if (r.stoppedFirst)
                return fail(kInputError, "the render stopped after " + number(static_cast<double>(r.l.size()) / req.rate, "%.2f")
                                             + " s: the records trace keeps of each call reached 256 MB (fewer fields, or fewer "
                                               "voices, go further); nothing written");
            if (r.stopCall != pg::Patch::Method::none && !stopped)
            {
                stopped = true;
                std::fputs(pg::Patch::stopMessage(files, r.stopCall, static_cast<float>(req.rate),
                                                  r.stopBounds.failed ? &r.stopBounds : nullptr).c_str(), stderr);
            }
            if (!r.lines.empty())
                say(r.lines.front());                    // "Rendered: the note held (C3) ...; in calls of 16 samples."
            if (!o.csv.empty() && r.log.columns.empty())
                return fail(kInputError, "no field matched the fields asked for (--list-fields names them); nothing written");
            const double rate = req.rate;
            if (!o.wav.empty())
            {
                ms::WavWritten ww;
                const auto wavBody = [&](const ms::Sink& sink) { return (ww = ms::writeWav(sink, r.l, r.r, rate, ask.wav)).ok; };
                if (!writeFile(fs::path(o.wav), wavBody, why))
                    return fail(kInputError, why);
                w.wav = o.wav;
                w.frames = r.l.size();
                w.wavHash = r.hash;
                const bool fl = ask.wav == ms::WavFormat::float32;
                say("Wrote " + o.wav + ": " + number(static_cast<double>(r.l.size()) / rate, "%.2f") + " s at " + number(rate) + " Hz, stereo "
                    + (fl ? "float32" : "24-bit PCM") + ", " + sizeText(ww.bytes)
                    + (!fl && ww.clipped > 0 ? ", " + std::to_string(ww.clipped) + " samples past full scale clipped" : "")
                    + "; the render hash (" + (fl ? "its data's SHA-256" : "of its float32 samples") + "): " + r.hash);
            }
            if (!o.csv.empty())
            {
                std::size_t bytes = 0;
                if (!writeFile(fs::path(o.csv), [&](const ms::Sink& sink) { return (bytes = ms::writeCsv(sink, r.log, rate)) > 0; }, why))
                    return fail(kInputError, why);
                w.csv = o.csv;
                w.rows = r.log.rows();
                w.columns = r.log.columns;
                std::string cols;
                for (const std::string& c : r.log.columns)
                    cols += (cols.empty() ? "" : ", ") + c;
                say("Wrote " + o.csv + ": time_s, sample, then " + cols + "; a row every " + std::to_string(every) + " samples, "
                    + std::to_string(r.log.rows()) + " rows, " + sizeText(bytes));
            }
        }

        // The report to stdout unless --quiet (or the metrics go there: --metrics -), or to --report's file.
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        if (res != nullptr && (o.report != "-" || (!o.quiet && o.metrics != "-")))
        {
            if (!writeText(o.report, res->report, why))
                return fail(kInputError, why);
            if (o.report != "-")
                say("Wrote " + o.report + ": the report");
        }
        if (!o.metrics.empty())
        {
            if (!writeText(o.metrics, metricsJson(files, req.rate, res.get(), w, seconds), why))
                return fail(kInputError, why);
            if (o.metrics != "-")
                say("Wrote " + o.metrics + ": the metrics as JSON");
        }
        say(number(seconds, "Done in %.2f s."));
        return stopped ? kStopped : kOk;
    }
}

int main(int argc, char** argv)
{
    return run(argc, argv);
}
