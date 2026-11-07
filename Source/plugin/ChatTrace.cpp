// Source/plugin/ChatTrace.cpp: the chat's trace (ChatAgent.h), read-only: where a problem in the sound starts, or what
// the program's fields do, in an offline render (dsp/Debug.h). Its source is the editor's files (with try_edits), the
// program that plays (its own code, never the frozen copy) or the user's recording (ChatScenario.h's captured()); its
// render measure_output's (scenario::renderOf: the notes, the times, the dials, the wheels, the scenario). find's
// result is at most twenty lines.
//
// Its `export` writes the render as files on this Mac (dsp/Export.h): a WAV (float32, or 24-bit PCM), an effect's input
// beside it, and with fields a CSV of them; the result names each file whole, its path one the user can open (the MCP
// server runs here too), and gives a short CSV inline for a client that cannot open them (a cloud session). The folder
// is rendersFolder() (the user's Music folder, <Company>/<Name>/Renders; a probe's sandbox under CTest), whose files
// are the user's: a name given replaces its own files, nothing else is deleted, and an export that would take the
// folder past kMaxFolderBytes, or the disk past kKeepFree, is refused in words. Never of the user's recording (Capture.h:
// it is never written anywhere): a render that reads it is refused.
#include "plugin/ChatAgent.h"

#include "plugin/ChatScenario.h"

#include "dsp/Debug.h"
#include "dsp/Export.h"

#include "Product.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>

namespace playground
{
    namespace
    {
        constexpr std::size_t kMaxOutput = 12000;

        struct FindWord
        {
            const char* word;
            measure::TraceFind find;
        };
        constexpr FindWord kFinds[] = {
            { "nan", measure::TraceFind::nan },         { "click", measure::TraceFind::click },
            { "runaway", measure::TraceFind::runaway }, { "silence", measure::TraceFind::silence },
            { "dc", measure::TraceFind::dc },           { "any", measure::TraceFind::any },
        };

        bool isNumber(const juce::var& v) { return v.isDouble() || v.isInt() || v.isInt64(); }

        // ---- the export --------------------------------------------------------------------------------------------
        constexpr int64_t kMaxFolderBytes = int64_t{ 2 } << 30;    // the renders folder at most (refused past it)
        constexpr int64_t kKeepFree = int64_t{ 1 } << 30;          // the disk keeps this much free
        constexpr int kInlineRows = 32, kMaxInlineRows = 64;       // the short CSV's rows: default, at most
        constexpr std::size_t kInlineBytes = 6000;                 // and its bytes at most (rows dropped evenly)

        struct ExportAsk
        {
            bool on = false;
            measure::WavFormat wav = measure::WavFormat::float32;
            std::size_t every = 0;                                 // samples (0: about 1 ms)
            int rows = kInlineRows;
            std::string name;                                      // the files' stem ("": a time's)
        };

        // `export`: true, or {wav, every, rows, name} (an object sent as a JSON string read as one); false with why.
        bool exportOf(const juce::var& in, ExportAsk& ask, std::string& why)
        {
            juce::var v = in.getProperty("export", juce::var());
            if (v.isVoid() || v.isUndefined() || (v.isBool() && !static_cast<bool>(v)))
                return true;
            if (v.isString())
                v = juce::JSON::parse(v.toString());
            ask.on = true;
            if (v.isBool())
                return true;
            if (!v.isObject())
            {
                why = "export is true, or {wav, every, rows, name}.";
                return false;
            }
            if (const juce::var w = v.getProperty("wav", juce::var()); !w.isVoid())
            {
                const juce::String word = w.toString().toLowerCase().removeCharacters(" -_");
                if (word == "float32" || word == "float" || word == "32" || word == "f32")
                    ask.wav = measure::WavFormat::float32;
                else if (word == "24" || word == "pcm24" || word == "int24" || word == "24bit" || word == "s24")
                    ask.wav = measure::WavFormat::pcm24;
                else
                {
                    why = "export's wav is float32 (the default) or 24 (24-bit PCM).";
                    return false;
                }
            }
            if (const juce::var e = v.getProperty("every", juce::var()); !e.isVoid())
            {
                if (!isNumber(e) || static_cast<double>(e) < 1.0)
                {
                    why = "export's every is a number of samples, 16 or more (the fields are read after calls of 16).";
                    return false;
                }
                ask.every = static_cast<std::size_t>(std::llround(std::min(static_cast<double>(e), 1e9)));
            }
            if (const juce::var r = v.getProperty("rows", juce::var()); isNumber(r))
                ask.rows = std::clamp(static_cast<int>(std::lround(static_cast<double>(r))), 0, kMaxInlineRows);
            if (const juce::var n = v.getProperty("name", juce::var()); !n.isVoid())
            {
                // Letters, digits, space, - _ . alone; no leading dot or space; 64 characters at most.
                std::string stem;
                for (const char c : n.toString().toStdString())
                    if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == ' ' || c == '-' || c == '_' || c == '.')
                        stem += c;
                while (!stem.empty() && (stem.front() == '.' || stem.front() == ' '))
                    stem.erase(stem.begin());
                while (!stem.empty() && (stem.back() == '.' || stem.back() == ' '))
                    stem.pop_back();
                if (stem.size() > 64)
                    stem.resize(64);
                for (const char* ext : { ".wav", ".csv" })
                    if (stem.size() > 4 && juce::String(stem).endsWithIgnoreCase(ext))
                        stem.resize(stem.size() - 4);
                ask.name = stem;
            }
            return true;
        }

        std::string sizeText(int64_t bytes)
        {
            char b[32];
            if (bytes < 1000)
                std::snprintf(b, sizeof b, "%lld bytes", static_cast<long long>(bytes));
            else if (bytes < 1000000)
                std::snprintf(b, sizeof b, "%.0f KB", static_cast<double>(bytes) / 1e3);
            else if (bytes < 1000000000)
                std::snprintf(b, sizeof b, "%.1f MB", static_cast<double>(bytes) / 1e6);
            else
                std::snprintf(b, sizeof b, "%.2f GB", static_cast<double>(bytes) / 1e9);
            return b;
        }

        std::string grouped(std::size_t n)
        {
            std::string d = std::to_string(n), out;
            for (std::size_t i = 0; i < d.size(); ++i)
            {
                if (i > 0 && (d.size() - i) % 3 == 0)
                    out += ',';
                out += d[i];
            }
            return out;
        }

        std::string secondsText(double s)
        {
            char b[32];
            std::snprintf(b, sizeof b, s < 10.0 ? "%.2f s" : "%.1f s", s);
            return b;
        }

        // A file written whole through a temporary beside it, then moved into place (a failed write leaves no part).
        bool writeFile(const juce::File& target, const std::function<bool(const measure::Sink&)>& body, std::string& why)
        {
            juce::TemporaryFile temp(target);
            {
                std::unique_ptr<juce::FileOutputStream> out = temp.getFile().createOutputStream();
                if (out == nullptr || !out->openedOk())
                {
                    why = "could not write " + target.getFullPathName().toStdString();
                    return false;
                }
                const measure::Sink sink = [&out](const void* data, std::size_t bytes) { return out->write(data, bytes); };
                if (!body(sink) || (out->flush(), out->getStatus().failed()))
                {
                    why = "writing " + target.getFileName().toStdString() + " failed (" + out->getStatus().getErrorMessage().toStdString()
                        + "; is the disk full?)";
                    return false;
                }
            }
            if (!temp.overwriteTargetFileWithTemporary())
            {
                why = "could not move " + target.getFileName().toStdString() + " into place";
                return false;
            }
            return true;
        }
    }

    namespace
    {
        // The renders folder (above): the user's Music folder's <Company>/<Name>/Renders; a probe's sandbox's renders/.
        juce::File rendersFolder()
        {
            // A probe's sandbox (CTest sets <ENV_PREFIX>PREFS_DIR for each, cmake/Probes.cmake): never the user's folder.
            const std::string sandbox = std::string(product::kEnvPrefix) + "PREFS_DIR";
            if (const char* dir = std::getenv(sandbox.c_str()); dir != nullptr && *dir != '\0')
                return juce::File(juce::String::fromUTF8(dir)).getChildFile("renders");
            return juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                .getChildFile(product::kCompany)
                .getChildFile(product::kName)
                .getChildFile("Renders");
        }

        // The files of an export (above) in the renders folder, and the result's lines naming them; the short CSV into
        // `inlineCsv`; "" and why when nothing could be written.
        std::string exportFiles(const measure::TraceResult& r, const ExportAsk& ask, std::size_t every, const measure::TraceRequest& q,
                                std::string& inlineCsv, std::string& why)
        {
            const double rate = q.render.rate > 0.0 ? q.render.rate : 48000.0;
            const juce::File dir = rendersFolder();
            if (!dir.isDirectory() && !dir.createDirectory().wasOk())
            {
                why = "could not make the folder " + dir.getFullPathName().toStdString() + ".";
                return {};
            }
            const std::size_t frames = r.l.size();
            const bool input = !product::kIsInstrument && !r.inL.empty();
            const bool csv = r.log.rows() > 0;
            // What it writes, at most (the CSV's rows at about 24 bytes and 18 a column), against the folder and the disk.
            const int64_t wavSize = static_cast<int64_t>(measure::wavBytes(frames, ask.wav));
            const int64_t needs = wavSize * (input ? 2 : 1)
                                + (csv ? static_cast<int64_t>(r.log.rows() * (24 + 18 * r.log.columns.size())) : 0);
            int64_t held = 0;
            for (const juce::DirectoryEntry& e : juce::RangedDirectoryIterator(dir, false, "*", juce::File::findFiles))
                held += e.getFileSize();
            if (held + needs > kMaxFolderBytes)
            {
                why = "the renders folder (" + dir.getFullPathName().toStdString() + ") holds " + sizeText(held) + " and this needs "
                    + sizeText(needs) + ", past its " + sizeText(kMaxFolderBytes) + ": the user can delete what they no longer need "
                      "there (nothing is deleted for them), or ask for a shorter render.";
                return {};
            }
            if (const int64_t spare = dir.getBytesFreeOnVolume(); spare > 0 && spare - needs < kKeepFree)
            {
                why = "the disk has " + sizeText(spare) + " free and this needs " + sizeText(needs) + ", leaving under "
                    + sizeText(kKeepFree) + ".";
                return {};
            }

            // The stem: the name given (its files replaced), else the time's (a number after it while one is taken).
            std::string stem = ask.name;
            if (stem.empty())
            {
                const std::string base = "render " + juce::Time::getCurrentTime().formatted("%Y-%m-%d %H.%M.%S").toStdString();
                stem = base;
                for (int k = 2; dir.getChildFile(stem + ".wav").exists() || dir.getChildFile(stem + ".csv").exists(); ++k)
                    stem = base + " " + std::to_string(k);
            }
            const juce::File wavFile = dir.getChildFile(stem + ".wav"), inFile = dir.getChildFile(stem + " input.wav"),
                             csvFile = dir.getChildFile(stem + ".csv");
            const bool replaced = !ask.name.empty() && (wavFile.exists() || csvFile.exists() || inFile.exists());

            const bool fl = ask.wav == measure::WavFormat::float32;
            measure::WavWritten w;
            if (!writeFile(wavFile, [&](const measure::Sink& sink) { return (w = measure::writeWav(sink, r.l, r.r, rate, ask.wav)).ok; }, why))
                return {};
            measure::WavWritten wi;
            if (input && !writeFile(inFile, [&](const measure::Sink& sink) { return (wi = measure::writeWav(sink, r.inL, r.inR, rate, ask.wav)).ok; }, why))
                return {};
            std::size_t csvBytes = 0;
            if (csv && !writeFile(csvFile, [&](const measure::Sink& sink) { return (csvBytes = measure::writeCsv(sink, r.log, rate)) > 0; }, why))
                return {};
            // A name given whose last export had an input or a CSV this one has not: those files go, so the name's files are
            // one export's.
            if (!ask.name.empty())
            {
                if (!input)
                    inFile.deleteFile();
                if (!csv)
                    csvFile.deleteFile();
            }

            const std::string format = fl ? "float32" : "24-bit PCM";
            const auto clipped = [&](const measure::WavWritten& x) {
                return fl || x.clipped == 0 ? std::string()
                                            : ", " + grouped(x.clipped) + " samples past full scale clipped (float32 keeps them)";
            };
            std::string out = "Exported on this Mac (the user can open these) to " + dir.getFullPathName().toStdString()
                            + (replaced ? ", replacing that name's files" : "") + ":\n";
            out += "- " + wavFile.getFileName().toStdString() + ": the render, " + secondsText(static_cast<double>(frames) / rate)
                 + " at " + std::to_string(std::llround(rate)) + " Hz, stereo " + format + ", " + sizeText(static_cast<int64_t>(w.bytes))
                 + clipped(w) + (fl ? "; its data's SHA-256 is the render hash" : "")
                 + (r.stopped ? "; the part rendered before trace stopped" : "") + ".\n";
            if (input)
                out += "- " + inFile.getFileName().toStdString() + ": the input as it went in, " + format + ", "
                     + sizeText(static_cast<int64_t>(wi.bytes)) + clipped(wi) + ".\n";
            if (csv)
            {
                const std::string whose = !product::kIsInstrument ? std::string()
                                        : !q.voiceGiven || q.voice == measure::kTraceNewest ? "; the newest voice that sounds, empty where none does"
                                        : q.voice == measure::kTraceAll ? "; the mean of the voices sounding, empty where none does"
                                                                        : "; note " + std::to_string(q.voice) + "'s voice, empty where none sounds";
                std::string cols;
                for (const std::string& c : r.log.columns)
                    cols += (cols.empty() ? "" : ", ") + c;
                out += "- " + csvFile.getFileName().toStdString() + ": time_s, sample, then " + cols + whose + "; a row every "
                     + std::to_string(every) + " samples (" + secondsText(static_cast<double>(every) / rate) + "), "
                     + grouped(r.log.rows()) + " rows, " + sizeText(static_cast<int64_t>(csvBytes)) + ".\n";
                // The short CSV: its rows evenly, the first and the last among them, 6 digits; fewer while it passes
                // kInlineBytes.
                if (ask.rows > 0)
                {
                    std::string text;
                    std::vector<std::size_t> picks;
                    for (std::size_t want = static_cast<std::size_t>(ask.rows); want >= 2 || text.empty(); want = want * 3 / 4)
                    {
                        picks = measure::evenRows(r.log.rows(), want);
                        text.clear();
                        measure::writeCsv([&text](const void* d, std::size_t n) {
                            text.append(static_cast<const char*>(d), n);
                            return true;
                        }, r.log, rate, picks, 6);
                        if (text.size() <= kInlineBytes || want < 2)
                            break;
                    }
                    inlineCsv = "The CSV inline (" + std::to_string(picks.size()) + " of its " + grouped(r.log.rows())
                              + " rows, evenly, to 6 digits):\n" + text;
                }
            }
            else if (!q.fields.empty())
                out += r.log.columns.empty() ? "(No CSV: no field named matched.)\n"
                                             : "(No CSV: the render, " + grouped(frames) + " samples, is shorter than a row's "
                                                   + grouped(every) + ".)\n";
            return out;
        }
    }

    juce::var ChatAgent::traceDefinition()
    {
        // Lean: tools.json sits in every one of Claude Desktop's conversations, so the words find and source take are in
        // the description's second paragraph (the guide's notes), and a word they do not take is refused naming them;
        // the render's settings are measure_output's (scenario::renderOf), undescribed here.
        const auto typed = [](const char* type) { return claude::object({ { "type", type } }); };
        juce::var props = claude::object({ { "find", typed("string") },
                                           { "fields", typed("array") },
                                           { "list", typed("boolean") },
                                           { "voice", juce::var(new juce::DynamicObject()) },
                                           { "source", typed("string") },
                                           { "scenario", scenario::schema() },
                                           { "try_edits", scenario::trialSchema() },
                                           { "dials_from", scenario::dialsFromSchema(false) },
                                           { "points", typed("integer") },
                                           { "around", typed("number") },
                                           { "detail", typed("boolean") },
                                           { "export", typed("object") } });
        auto* o = props.getDynamicObject();
        if constexpr (product::kIsInstrument)
            for (const char* k : { "velocity", "modwheel", "pitchbend" })
                o->setProperty(k, typed("number"));
        if constexpr (product::kIsInstrument)
            o->setProperty("notes", typed("array"));
        for (const char* k : { "hold_seconds", "tail_seconds", "bpm" })
            o->setProperty(k, typed("number"));
        o->setProperty("dials", typed("object"));
        return claude::object(
            { { "name", "trace" },
              { "description",
                juce::String("Find where a problem in the sound starts (the moment, the voice, the first field to go bad and the lines "
                "that write it), or watch the program's fields, in an offline render; export writes it as a WAV, the fields as "
                "a CSV.\n\n"
                "It changes nothing in the plugin. find: nan, click, runaway, silence, dc or any (the default without "
                "fields or export): each "
                "problem's moment, voice and note, the events within 32 samples, up to 3 culprit fields (the first to go "
                "non-finite, or past 1e6 at a thousand times its size in its first 10 ms; at a step, those that jump 10 "
                "times their usual step) with their values around it "
                "and the lines that assign them, and a verdict. A NaN only in a local leaves no field bad. fields: up to 8 "
                "paths or globs (filter.*; list: true names them): min, max, mean, first, last, the first non-finite moment, "
                "the largest jump and a sparkline each; points (at most 64) adds raw values, a sample each around `around` "
                "seconds when given. voice: newest, all or a note (find's default all, the fields' newest). source: editor "
                "(the default), playing (its own code, not the frozen copy) or capture (the user's recording, replayed "
                "through the program that played it; scenario from and to: a part of it). It renders in calls of 16 samples, "
                "then of 1 sample before each problem, and says when the output depends on how the calls are split. detail: "
                "more of each. Its Render hash line: SHA-256 of the render's float32 samples, L R interleaved (the reports' "
                "lines, as measure_output's: an instrument's the same render as this one). export (true, or {wav, every, rows, "
                "name}) writes the render to files on the user's Mac and gives each one's path (one the user can open): a WAV "
                "(wav: float32, the default, whose data hashes to the render hash; or 24, 24-bit PCM, clipped at full scale), "
                "an effect's input beside it, and with fields a CSV of them (time_s, sample, a column a field, as voice "
                "chooses; a row every `every` samples, a multiple of 16, default about 1 ms), up to 300 s of render; then the "
                "CSV's rows evenly, at most 64 (rows, default 32), inline, for a client that cannot open the files. name: the "
                "files' name (a later export of that name replaces them). Never of the user's recording. The render as "
                "measure_output's: ")
                    + (product::kIsInstrument ? "notes, velocity, the wheels, " : "")
                    + "hold_seconds, tail_seconds, bpm, dials, dials_from, scenario and try_edits (an effect plays the "
                      "scenario's input, else the sweep)." },
              { "eager_input_streaming", true },
              { "input_schema", claude::object({ { "type", "object" }, { "properties", props } }) } });
    }

    juce::var ChatAgent::traceTool(const juce::var& use)
    {
        const juce::var input = use.getProperty("input", juce::var());
        juce::var block = claude::object({ { "type", "tool_result" }, { "tool_use_id", use.getProperty("id", "") } });
        // `tail` (an export's short CSV, kInlineBytes at most) goes after the cut, whole.
        const auto done = [&](const std::string& text, bool error, const std::string& tail = {}) {
            const std::string t = (text.size() > kMaxOutput ? text.substr(0, kMaxOutput) + (tail.empty() ? "\n(cut)" : "\n(cut)\n") : text)
                                + tail;
            block.getDynamicObject()->setProperty("content", juce::String::fromUTF8(t.c_str()));
            if (error)
                block.getDynamicObject()->setProperty("is_error", true);
            const std::lock_guard<std::mutex> lock(mutex_);
            addEntry(ChatEntry::Kind::tool, error ? "traced the sound: " + t.substr(0, t.find('\n')) : "traced the sound");
            return block;
        };
        setActivity("Tracing the program");
        // Its render is the whole sound (renderOf would read a window and render on to its end).
        if (input.hasProperty("window"))
            return done("trace takes no window: it watches the whole render (measure_output, view_sound, a check and "
                        "tune_dials measure one).", true);

        measure::TraceRequest q;
        if (const juce::var find = input.getProperty("find", juce::var()); !find.isVoid())
        {
            const std::string word = find.toString().toLowerCase().trim().toStdString();
            bool known = false;
            for (const FindWord& f : kFinds)
                if (word == f.word)
                {
                    q.find = f.find;
                    known = true;
                }
            if (!known)
                return done("find is one of nan, click, runaway, silence, dc or any.", true);
        }
        if (const juce::var fields = input.getProperty("fields", juce::var()); !fields.isVoid())
        {
            const juce::Array<juce::var>* list = fields.getArray();
            if (list == nullptr || list->size() > static_cast<int>(measure::kMaxTracePatterns))
                return done("fields is a list of at most 8 paths or globs (\"filter.*\", \"osc[1].phase\"); list: true names "
                            "the program's fields.", true);
            for (const juce::var& f : *list)
                if (f.isString() && f.toString().isNotEmpty())
                    q.fields.push_back(f.toString().toStdString());
        }
        q.list = static_cast<bool>(input.getProperty("list", false));
        q.detail = static_cast<bool>(input.getProperty("detail", false));
        ExportAsk ask;
        {
            std::string bad;
            if (!exportOf(input, ask, bad))
                return done(bad, true);
            if (ask.on && q.list)
                return done("list names the fields and renders nothing to export: ask for one or the other.", true);
        }
        if (const juce::var voice = input.getProperty("voice", juce::var()); !voice.isVoid())
        {
            const juce::String v = voice.toString();
            q.voiceGiven = true;
            if (v == "newest")
                q.voice = measure::kTraceNewest;
            else if (v == "all")
                q.voice = measure::kTraceAll;
            else if ((isNumber(voice) || v.containsOnly("0123456789")) && v.isNotEmpty() && v.getIntValue() >= 0 && v.getIntValue() <= 127)
                q.voice = v.getIntValue();
            else
                return done("voice is newest, all or a MIDI note (0 to 127).", true);
        }
        if (const juce::var points = input.getProperty("points", juce::var()); isNumber(points))
            q.points = std::clamp(static_cast<int>(std::lround(static_cast<double>(points))), 0, static_cast<int>(measure::kMaxTracePoints));
        if (const juce::var around = input.getProperty("around", juce::var()); isNumber(around))
            q.around = std::max(0.0, static_cast<double>(around));

        // The source.
        const std::string source = input.getProperty("source", "editor").toString().toLowerCase().trim().toStdString();
        Sources files;
        std::string words;
        scenario::Captured recording;
        std::string why;
        if (source == "editor")
        {
            files = host_.chatDraft().files;
            words = "the editor's files";
        }
        else if (source == "playing")
        {
            files = host_.chatRunning();
            words = "the program that plays (its own code; once its dials hold still the plugin plays a frozen copy whose "
                    "last bits differ, which this does not render)";
            if (files.empty())
                return done("Nothing plays yet (run_program plays the editor's files).", true);
        }
        else if (source != "capture")
            return done("source is editor, playing or capture.", true);

        // The render as measure_output's (the notes, the times, the dials, the wheels, the scenario). The recording
        // (source "capture"): the scenario is it (from, to: a part of it), replayed through the program that played it,
        // as it played (ChatScenario.h); read once, here, for the files and the render alike.
        {
            measure::Request base = scenario::forNumbers();   // up to 300 s (Measure.h, "How long a render may be")
            base.fromCapture = source == "capture";
            q.render = scenario::renderOf(input, &why, base);
            if (!why.empty())
                return done(why, true);
            // The user's recording is never written anywhere (Capture.h), nor what it plays through the program.
            if (ask.on && q.render.fromCapture)
                return done("export writes no render of the user's recording: it stays in the plugin's memory (trace it "
                            "without export, or export a render of the editor's files with a scenario of your own).", true);
        }
        if (q.render.fromCapture)
        {
            if (!scenario::captured(host_, recording, why, q.render.captureFrom, q.render.captureTo))
                return done((source == "capture" ? "source" : "the scenario") + std::string(" is the recording, but ") + why + ".", true);
            if (source == "capture")
            {
                files = recording.files;
                q.pins = recording.pins;               // its netlists as they played (an edited one: the store's)
                words = "the user's recording, replayed through the program that played it (" + recording.about + ")";
                if (files.empty())
                {
                    files = host_.chatRunning();
                    words = "the user's recording, replayed through the program that plays now (the one that played it is "
                            "no longer known; " + recording.about + ")";
                }
                if (files.empty())
                    return done("Nothing plays yet, so there is no program to replay the recording through.", true);
            }
        }
        bool defaults = false;
        if (!scenario::dialsFrom(input, defaults, why))
            return done(why, true);
        if (!defaults)
            q.render.current = currentDials();
        if (!scenario::resolve(q.render, host_, why, q.render.fromCapture ? &recording : nullptr))
            return done(why, true);
        int trials = 0;
        if (!scenario::tryEdits(input, files, trials, why))
            return done(why, true);
        q.keepGoing = [this] { return !cancel_.load(); };
        // An export (above): the fields every `every` samples, a multiple of 16 (pass 1's calls), about 1 ms by default.
        std::size_t every = 0;
        if (ask.on)
        {
            q.exporting = true;
            const double rate = q.render.rate > 0.0 ? q.render.rate : 48000.0;
            every = ask.every > 0 ? ask.every : 16 * static_cast<std::size_t>(std::max(1L, std::lround(rate / 16000.0)));
            every = std::max<std::size_t>(16, (every + 8) / 16 * 16);
            q.every = q.fields.empty() ? 0 : every;
        }

        const measure::TraceResult r = measure::trace(files, q);
        if (!r.error.empty())
            return done(scenario::trialLine(trials) + r.error, true);
        std::string out = scenario::trialLine(trials) + scenario::dialsLine(defaults && !q.render.fromCapture)
                        + "Traced offline (nothing that plays changed): " + words
                        + (trials > 0 ? ", with the trial edits" : "") + ".\n";
        std::string tail;                                   // the short CSV, after trace's own lines
        if (ask.on)
        {
            std::string why2;
            const std::string written = exportFiles(r, ask, every, q, tail, why2);
            if (!why2.empty())
                return done(out + "The export failed: " + why2 + "\n", true);
            out += written;
        }
        for (const std::string& line : r.lines)
            out += line + "\n";
        return done(out, false, tail);
    }
}
