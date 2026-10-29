// Source/plugin/ChatTrace.cpp: the chat's trace (ChatAgent.h), read-only: where a problem in the sound starts, or what
// the program's fields do, in an offline render (dsp/Debug.h). Its source is the editor's files (with try_edits), the
// program that plays (its own code, never the frozen copy) or the user's recording (ChatScenario.h's captured()); its
// render the scenario's. find's result is at most twenty lines.
#include "plugin/ChatAgent.h"

#include "plugin/ChatScenario.h"

#include "dsp/Debug.h"

#include "Product.h"

#include <cmath>

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
    }

    juce::var ChatAgent::traceDefinition()
    {
        // Lean: tools.json sits in every one of Claude Desktop's conversations, so the words find and source take are in
        // the description's second paragraph (the guide's notes), and a word they do not take is refused naming them.
        juce::var props = claude::object({ { "find", claude::object({ { "type", "string" } }) },
                                           { "fields", claude::object({ { "type", "array" } }) },
                                           { "list", claude::object({ { "type", "boolean" } }) },
                                           { "voice", juce::var(new juce::DynamicObject()) },
                                           { "source", claude::object({ { "type", "string" } }) },
                                           { "scenario", scenario::schema() },
                                           { "try_edits", scenario::trialSchema() },
                                           { "points", claude::object({ { "type", "integer" } }) },
                                           { "around", claude::object({ { "type", "number" } }) },
                                           { "detail", claude::object({ { "type", "boolean" } }) } });
        return claude::object(
            { { "name", "trace" },
              { "description",
                "Find where a problem in the sound starts (the moment, the voice, the first field to go bad and the lines "
                "that write it), or watch the program's fields, in an offline render.\n\n"
                "It changes nothing. find: nan, click, runaway, silence, dc or any (the default without fields): each "
                "problem's moment, voice and note, the events within 32 samples, up to 3 culprit fields (the first to go "
                "non-finite or past 1e6; at a step, those that jump 10 times their usual step) with their values around it "
                "and the lines that assign them, and a verdict. A NaN only in a local leaves no field bad. fields: up to 8 "
                "paths or globs (filter.*; list: true names them): min, max, mean, first, last, the first non-finite moment, "
                "the largest jump and a sparkline each; points (at most 64) adds raw values, a sample each around `around` "
                "seconds when given. voice: newest, all or a note (find's default all, the fields' newest). source: editor "
                "(the default), playing (its own code, not the frozen copy) or capture (the user's recording, replayed "
                "through the program that played it; scenario from and to: a part of it). It renders in calls of 16 samples, "
                "then of 1 sample before each problem, and says when the output depends on how the calls are split. detail: "
                "more of each. scenario and try_edits as measure_output's (an effect plays the scenario's input, else the "
                "sweep)." },
              { "eager_input_streaming", true },
              { "input_schema", claude::object({ { "type", "object" }, { "properties", props } }) } });
    }

    juce::var ChatAgent::traceTool(const juce::var& use)
    {
        const juce::var input = use.getProperty("input", juce::var());
        juce::var block = claude::object({ { "type", "tool_result" }, { "tool_use_id", use.getProperty("id", "") } });
        const auto done = [&](const std::string& text, bool error) {
            const std::string t = text.size() > kMaxOutput ? text.substr(0, kMaxOutput) + "\n(cut)" : text;
            block.getDynamicObject()->setProperty("content", juce::String::fromUTF8(t.c_str()));
            if (error)
                block.getDynamicObject()->setProperty("is_error", true);
            const std::lock_guard<std::mutex> lock(mutex_);
            addEntry(ChatEntry::Kind::tool, error ? "traced the sound: " + t.substr(0, t.find('\n')) : "traced the sound");
            return block;
        };
        setActivity("Tracing the program");

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
            words = "the program that plays (its own code; once its dials hold still the plugin plays a frozen copy of the "
                    "same bits, which this does not render)";
            if (files.empty())
                return done("Nothing plays yet (run_program plays the editor's files).", true);
        }
        else if (source != "capture")
            return done("source is editor, playing or capture.", true);

        // The recording (source "capture"): the scenario is it (from, to: a part of it), replayed through the program that
        // played it, as it played (ChatScenario.h); read once, here, for the files and the render alike.
        if (source == "capture")
            q.render.fromCapture = true;
        if (!scenario::read(input, q.render, &why))
            return done(why, true);
        if (q.render.fromCapture)
        {
            if (!scenario::captured(host_, recording, why, q.render.captureFrom, q.render.captureTo))
                return done((source == "capture" ? "source" : "the scenario") + std::string(" is the recording, but ") + why + ".", true);
            if (source == "capture")
            {
                files = recording.files;
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
        q.render.current = currentDials();
        if (!scenario::resolve(q.render, host_, why, q.render.fromCapture ? &recording : nullptr))
            return done(why, true);
        int trials = 0;
        if (!scenario::tryEdits(input, files, trials, why))
            return done(why, true);
        q.keepGoing = [this] { return !cancel_.load(); };

        const measure::TraceResult r = measure::trace(files, q);
        if (!r.error.empty())
            return done(scenario::trialLine(trials) + r.error, true);
        std::string out = scenario::trialLine(trials) + "Traced offline (nothing that plays changed): " + words
                        + (trials > 0 ? ", with the trial edits" : "") + ".\n";
        for (const std::string& line : r.lines)
            out += line + "\n";
        return done(out, false);
    }
}
