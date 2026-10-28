// Source/plugin/ChatAgent.cpp: see ChatAgent.h.
#include "plugin/ChatAgent.h"

#include "plugin/ChatImages.h"

#include "plugin/ChatStore.h"

#include "dsp/Goals.h"
#include "dsp/Measure.h"
#include "dsp/SoundView.h"
#include "dsp/Tune.h"

#include "plugin/SoundPicture.h"

#include "mcp/McpGuide.h"

#include "Library.h"
#include "Product.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>
#include <utility>

namespace playground
{
    namespace
    {
        constexpr int kDefaultMaxTokens = 64000;   // a model /v1/models has not described
        constexpr int kMaxTokensCap = 128000;      // the longest answer asked for: a big file is written in one call
        constexpr int kMaxAttempts = 4;            // a request, retried on an overload, a rate limit or a dropped line
        constexpr int kMaxCutOffs = 2;             // replies cut off at max_tokens in a row before a turn ends
        constexpr std::size_t kMaxToolOutput = 12000;   // a tool result's characters (a compile's messages)
        constexpr int kMaxChecks = 12;             // set_checks's checks
        constexpr int kMaxCheckNudges = 2;         // times a turn goes on because its checks fail (ChatAgent.h)
        constexpr int kMaxPlannerAsks = 4;         // ask_planner's calls a turn
        // The tools that change the program, the dials or the checks: a turn that used none is never sent on by them.
        constexpr const char* kChangingTools[] = { "write_file", "edit_file", "rename_file", "delete_file", "add_library_file",
                                                   "run_program", "set_dial", "tune_dials", "set_checks" };
        constexpr bool kInstrument = product::kIsInstrument;
        constexpr const char* kDefaultModel = "claude-opus-5-5";
        constexpr const char* kDefaultEffort = "medium";
        constexpr const char* kFallbackBeta = "server-side-fallback-2026-07-01";

        // Server-side compaction (the compaction docs): once a request's input reaches the trigger, the API summarizes
        // the conversation before it and answers from the summary; the summary comes back as a compaction block, which
        // goes back with the rest of the answer, and the API ignores what came before it. The history then starts at
        // the block (appendAnswer, repairHistory).
        constexpr const char* kCompactBeta = "compact-2026-01-12";
        constexpr int kCompactTrigger = 150000;   // input tokens (the API's default; 50,000 at least)
        constexpr const char* kCompactInstructions =
            "Summarize the conversation so far between a user and Claude, who builds and changes FMinus DSP programs "
            "inside an audio plugin through tools (read_file, write_file, edit_file, run_program, set_dial, "
            "measure_output, view_sound, set_checks, tune_dials, ask_planner, get_status). The summary replaces that part of the "
            "conversation, so keep everything "
            "needed to carry on: what the user asked for, and their preferences about the sound and the code; what "
            "was built: each file and what it holds, the signal flow and the modulation routing, what each dial does "
            "and the values chosen; decisions made and ideas rejected, and why; problems met and how they were "
            "solved, and measurements that matter; the checks set (set_checks) and how they stand (the plugin keeps "
            "measuring them); what is unfinished or was promised next; and the user's latest "
            "request, word for word. Do not copy the programs' code: the plugin sends the current files after this "
            "summary, and read_file shows any of them. Plain prose and short lists.";

        const std::vector<std::string> kAllEfforts{ "low", "medium", "high", "xhigh", "max" };

        // Before /v1/models answers (or without a key): the current models, which all take adaptive thinking and
        // every effort level; Claude Opus 5.5 takes fast mode too.
        std::vector<ChatModel> builtInModels()
        {
            return { { "claude-opus-5-5", "Claude Opus 5.5", kAllEfforts, 128000, true, true },
                     { "claude-fable-5-1", "Claude Fable 5.1", kAllEfforts, 128000, true },
                     { "claude-sonnet-5-5", "Claude Sonnet 5.5", kAllEfforts, 128000, true },
                     { "claude-haiku-5-5", "Claude Haiku 5.5", kAllEfforts, 128000, true } };
        }

        // The models that take "fallbacks": "default" (a classifier's refusal retried on another model).
        bool takesDefaultFallbacks(const std::string& id)
        {
            return id == "claude-fable-5-1" || id == "claude-opus-5-5" || id == "claude-opus-5"
                || id == "claude-sonnet-5-5";
        }

        // Fast mode (the fast-mode docs; /v1/models does not list it): Claude Opus 5.5, and in research preview
        // Claude Opus 5 and Opus 4.8, whose fast requests need the beta header (without it they get a 400).
        constexpr const char* kFastBeta = "fast-mode-2026-02-01";

        bool takesFast(const std::string& id)
        {
            return id == "claude-opus-5-5" || id == "claude-opus-5" || id == "claude-opus-4-8";
        }

        bool fastNeedsBeta(const std::string& id) { return id == "claude-opus-5" || id == "claude-opus-4-8"; }

        // Whether a refused request's error is about fast mode itself (not open to this key, not on this model).
        bool aboutSpeed(const std::string& error)
        {
            const juce::String e = juce::String(error).toLowerCase();
            return e.contains("speed") || e.contains("fast mode") || e.contains("fast-mode");
        }

        std::string str(const juce::var& v, const char* key)
        {
            return v.getProperty(key, "").toString().toStdString();
        }

        // FNV-1a, 64 bits: how the agent remembers which text of a file Claude has seen.
        uint64_t hashText(std::string_view t)
        {
            uint64_t h = 1469598103934665603ull;
            for (const char c : t)
            {
                h ^= static_cast<uint8_t>(c);
                h *= 1099511628211ull;
            }
            return h;
        }

        // The string value of `key` in a JSON object that may still be arriving: "" until its closing quote has.
        std::string partialString(const std::string& json, const char* key)
        {
            const std::string k = std::string("\"") + key + "\"";
            std::size_t p = json.find(k);
            if (p == std::string::npos)
                return {};
            p = json.find(':', p + k.size());
            if (p == std::string::npos)
                return {};
            p = json.find('"', p + 1);
            if (p == std::string::npos)
                return {};
            std::string out;
            for (std::size_t i = p + 1; i < json.size(); ++i)
            {
                if (json[i] == '\\')
                {
                    if (++i >= json.size())
                        return {};
                    out += json[i];
                }
                else if (json[i] == '"')
                    return out;
                else
                    out += json[i];
            }
            return {};
        }

        // `t` cut to kMaxToolOutput characters at a line's end, saying how much was left out.
        std::string capped(const std::string& t)
        {
            if (t.size() <= kMaxToolOutput)
                return t;
            std::size_t cut = t.rfind('\n', kMaxToolOutput);
            if (cut == std::string::npos || cut < kMaxToolOutput / 2)
                cut = kMaxToolOutput;
            const int lines = static_cast<int>(std::count(t.begin() + static_cast<std::ptrdiff_t>(cut), t.end(), '\n'));
            return t.substr(0, cut) + "\n(" + std::to_string(lines) + " more lines left out: fix these first)\n";
        }

        juce::var text(const std::string& t)
        {
            return claude::object({ { "type", "text" }, { "text", juce::String::fromUTF8(t.c_str()) } });
        }

        int lineCount(const std::string& t)
        {
            if (t.empty())
                return 0;
            return static_cast<int>(std::count(t.begin(), t.end(), '\n')) + (t.back() == '\n' ? 0 : 1);
        }

        std::string number(double v)
        {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%g", v);
            return buf;
        }

        std::string thousands(int64_t n)
        {
            char buf[32];
            if (n >= 10000)
                std::snprintf(buf, sizeof buf, "%.0fK", static_cast<double>(n) / 1000.0);
            else if (n >= 1000)
                std::snprintf(buf, sizeof buf, "%.1fK", static_cast<double>(n) / 1000.0);
            else
                std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(n));
            return buf;
        }

        // What one reply holds, told to Claude with the user's message at xhigh and max effort, where thinking is long:
        // it may otherwise draft a whole file in its thinking, write it again, and run out of room (the API's advice
        // for long deliverables at those levels). Once sent it stays in the history as it was.
        std::string limitNote(int maxTokens)
        {
            return "(From the plugin: everything in one reply of yours, your thinking included, counts toward one limit "
                   "of about " + thousands(maxTokens) + " tokens, and a reply that reaches it is cut off. So don't write "
                   "a file out in full in your thinking and then again in the reply: in your thinking settle the design "
                   "and the hard decisions, then write the code once, straight into its files, a few files to a reply.)";
        }

        // After a reply cut off at max_tokens with no tool call: what the turn goes on with.
        std::string cutOffNote(int maxTokens)
        {
            return "(From the plugin: your reply reached its limit of about " + thousands(maxTokens)
                 + " tokens, your thinking included, and was cut off. Go on from where it stopped: write what you have "
                   "worked out into its files now, then do what remains a step at a time.)";
        }

        const char* kindName(ChatEntry::Kind k)
        {
            switch (k)
            {
                case ChatEntry::Kind::user: return "user";
                case ChatEntry::Kind::assistant: return "assistant";
                case ChatEntry::Kind::thinking: return "thinking";
                case ChatEntry::Kind::tool: return "tool";
                case ChatEntry::Kind::error: return "error";
                case ChatEntry::Kind::note: return "note";
                case ChatEntry::Kind::question: return "question";
                case ChatEntry::Kind::checks: return "checks";
                case ChatEntry::Kind::plan: return "plan";
            }
            return "note";
        }

        ChatEntry::Kind kindOf(const std::string& s)
        {
            for (const auto k : { ChatEntry::Kind::user, ChatEntry::Kind::assistant, ChatEntry::Kind::thinking,
                                  ChatEntry::Kind::tool, ChatEntry::Kind::error, ChatEntry::Kind::note,
                                  ChatEntry::Kind::question, ChatEntry::Kind::checks, ChatEntry::Kind::plan })
                if (s == kindName(k))
                    return k;
            return ChatEntry::Kind::note;
        }

        std::string controlType(const Control& c)
        {
            switch (c.type)
            {
                case ControlType::real: return "float";
                case ControlType::integer: return "int";
                case ControlType::toggle: return "bool";
            }
            return "float";
        }

        // The dials of `controls` with their values, one a line.
        std::string dialLines(const Controls& controls, const ChatHost& host)
        {
            std::string out;
            for (int c = 0; c < kMaxControls; ++c)
            {
                const Control& k = controls[static_cast<std::size_t>(c)];
                if (!k.used)
                    continue;
                out += std::to_string(c + 1) + " " + k.name + ": " + controlType(k) + " [" + number(k.min) + ", "
                     + number(k.max) + "] default " + number(k.def) + ", now " + number(host.chatDialValue(c)) + "\n";
            }
            return out.empty() ? "none\n" : out;
        }

        // The panel a program's #pragma panel lines made, in a line: its sections in order ("/" where a row starts),
        // their items (a display as its source and kind), then the labels, small dials, styles, options and colours;
        // none without a panel line.
        std::string panelLine(const PanelSpec& p, const Controls& controls)
        {
            const auto dialName = [&](int c) { return std::string(controls[static_cast<std::size_t>(c)].name); };
            // A .gui file's interface: what is in it, in a line (the file itself is with the others).
            if (!p.gui.empty())
            {
                int shownControls = 0, pages = 0, sections = 0;
                std::string titles;
                for (const GuiNode& n : p.gui.nodes)
                {
                    shownControls += n.slot >= 0 ? (n.slot2 >= 0 ? 2 : 1) : 0;
                    sections += n.kind == GuiKind::section ? 1 : 0;
                    if (n.kind == GuiKind::page)
                    {
                        ++pages;
                        titles += (titles.empty() ? "" : ", ") + n.text;
                    }
                }
                return "Its interface is " + p.gui.file + "'s: " + std::to_string(shownControls) + " controls, "
                       + std::to_string(p.displays.size()) + " displays, " + std::to_string(sections) + " sections"
                       + (pages > 0 ? ", pages " + titles : std::string()) + "\n";
            }
            const auto kindOf = [](DisplayKind k) {
                switch (k)
                {
                    case DisplayKind::meter:  return "meter";
                    case DisplayKind::vu:     return "vu";
                    case DisplayKind::led:    return "led";
                    case DisplayKind::number: return "number";
                    case DisplayKind::scope:  return "scope";
                    case DisplayKind::spectrum: return "spectrum";
                }
                return "";
            };
            const auto itemName = [&](const PanelItem& it) {
                if (!it.display)
                    return dialName(it.index);
                const PanelDisplay& d = p.displays[static_cast<std::size_t>(it.index)];
                return d.name + " (" + kindOf(d.kind) + (d.name == d.source ? std::string() : " of " + d.source) + ")";
            };
            std::string out;
            for (std::size_t k = 0; k < p.sections.size(); ++k)
            {
                const PanelSection& s = p.sections[k];
                out += k == 0 ? "" : (s.newRow ? " / " : " | ");
                out += (s.title.empty() ? std::string("(untitled)") : s.title) + ": ";
                for (std::size_t j = 0; j < s.items.size(); ++j)
                    out += (j == 0 ? "" : " ") + itemName(s.items[j]);
            }
            const char* const styleNames[] = { "knob", "vintage", "fader", "switch", "buttons" };
            std::string labels, small, styles, options, colors;
            for (int c = 0; c < kMaxControls; ++c)
            {
                const std::size_t i = static_cast<std::size_t>(c);
                if (!controls[i].used)
                    continue;
                if (!p.labels[i].empty())
                    labels += (labels.empty() ? "" : ", ") + dialName(c) + " \"" + p.labels[i] + "\"";
                if (p.small[i])
                    small += (small.empty() ? "" : " ") + dialName(c);
                if (p.styles[i] != DialStyle::knob)
                    styles += (styles.empty() ? "" : ", ") + dialName(c) + " " + styleNames[static_cast<int>(p.styles[i])];
                if (!p.options[i].empty())
                {
                    options += (options.empty() ? "" : ", ") + dialName(c);
                    for (const std::string& o : p.options[i])
                        options += " \"" + o + "\"";
                }
                if (!p.colors[i].empty())
                    colors += (colors.empty() ? "" : ", ") + dialName(c) + " " + p.colors[i];
            }
            for (const PanelSection& sec : p.sections)
                if (!sec.color.empty())
                    colors += (colors.empty() ? "" : ", ") + ("\"" + sec.title + "\" ") + sec.color;
            for (const PanelDisplay& d : p.displays)
                if (!d.color.empty())
                    colors += (colors.empty() ? "" : ", ") + d.name + " " + d.color;
            if (out.empty() && labels.empty() && small.empty() && styles.empty() && options.empty() && colors.empty())
                return {};
            return "Its panel: " + (out.empty() ? std::string("no sections") : out) + (labels.empty() ? "" : "; labels " + labels)
                   + (small.empty() ? "" : "; small " + small) + (styles.empty() ? "" : "; styles " + styles)
                   + (options.empty() ? "" : "; options " + options) + (colors.empty() ? "" : "; colours " + colors) + "\n";
        }

        std::string compileLine(const CompileStatus& s)
        {
            if (s.revision == 0)
                return "nothing compiled yet\n";
            if (s.ok)
                return "compiled in " + number(std::round(s.ms)) + " ms and plays\n" + s.notes;
            return "did not compile (the program before it plays on):\n" + s.errors + s.notes;
        }

        juce::var tool(const char* name, const char* description, juce::var properties,
                       std::initializer_list<const char*> required)
        {
            juce::Array<juce::var> req;
            for (const char* r : required)
                req.add(juce::String(r));
            juce::var schema = claude::object({ { "type", "object" }, { "properties", properties } });
            if (req.size() > 0)
                schema.getDynamicObject()->setProperty("required", req);
            return claude::object({ { "name", name },
                                    { "description", description },
                                    { "eager_input_streaming", true },
                                    { "input_schema", schema } });
        }

        juce::var prop(const char* type, const char* description)
        {
            return claude::object({ { "type", type }, { "description", description } });
        }

        bool isNumber(const juce::var& v) { return v.isDouble() || v.isInt() || v.isInt64(); }

        // A render's settings from a tool's input (measure_output's, view_sound's, a check's, tune_dials's): the notes,
        // the velocity, the times, the host and the dials asked for.
        measure::Request renderOf(const juce::var& input)
        {
            measure::Request req;
            const auto numberArg = [&](const char* k, float& out) {
                const juce::var v = input.getProperty(k, juce::var());
                if (isNumber(v))
                    out = static_cast<float>(static_cast<double>(v));
            };
            const juce::var notes = input.getProperty("notes", juce::var());
            if (const auto* arr = notes.getArray())
                for (const auto& n : *arr)
                    if (isNumber(n))
                        req.notes.push_back(static_cast<int>(std::lround(static_cast<double>(n))));
            numberArg("velocity", req.velocity);
            numberArg("hold_seconds", req.holdSeconds);
            numberArg("tail_seconds", req.tailSeconds);
            numberArg("bpm", req.bpm);
            numberArg("modwheel", req.modwheel);
            numberArg("pitchbend", req.pitchbend);
            const juce::var dials = input.getProperty("dials", juce::var());
            if (const auto* o = dials.getDynamicObject())
                for (const auto& kv : o->getProperties())
                    if (isNumber(kv.value) || kv.value.isBool())
                        req.dials[kv.name.toString().toStdString()] = static_cast<float>(static_cast<double>(kv.value));
            return req;
        }

        // The render's settings as a tool's input schema has them (measure_output's own, which the others share).
        void addRenderProps(juce::var& props)
        {
            auto* o = props.getDynamicObject();
            if constexpr (kInstrument)
            {
                o->setProperty("notes", claude::object({ { "type", "array" },
                                                         { "items", claude::object({ { "type", "integer" } }) },
                                                         { "description", "MIDI notes played together (69 is A, 440 Hz). "
                                                                          "Default: one note, 48 (C3)." } }));
                o->setProperty("velocity", prop("number", "The velocity, 0 to 1 (default 0.8)"));
                o->setProperty("modwheel", prop("number", "The mod wheel, 0 to 1, from the start (default 0)"));
                o->setProperty("pitchbend", prop("number", "The pitch wheel, -1 to 1, from the start (default 0)"));
            }
            o->setProperty("hold_seconds", prop("number", kInstrument ? "How long the notes are held; default 1, at most 10"
                                                                      : "How long the input plays; default 1, at most 10"));
            o->setProperty("tail_seconds", prop("number", kInstrument ? "How long to go on after the release; default 1.5, "
                                                                        "at most 10"
                                                                      : "How long to go on after it (silence); default 1.5, "
                                                                        "at most 10"));
            o->setProperty("dials", claude::object({ { "type", "object" },
                                                     { "additionalProperties", claude::object({ { "type", "number" } }) },
                                                     { "description", "Dial values to render with, by parameter name, in "
                                                                      "the parameter's own units. Dials not named keep the "
                                                                      "values they have now." } }));
            o->setProperty("bpm", prop("number", "The host's tempo for a program that reads it (host.fm); default 120"));
        }

        // The metrics' names, for a schema's enum (Measure.h).
        juce::var metricEnum(const char* description)
        {
            juce::Array<juce::var> names;
            for (const measure::MetricInfo& m : measure::metrics())
                names.add(juce::String(m.name));
            return claude::object({ { "type", "string" }, { "enum", names }, { "description", description } });
        }

        // A goal (Goals.h) from a tool's input: metric and min, max or target (and a tolerance, a weight); false with why.
        bool goalOf(const juce::var& in, measure::Goal& g, std::string& why)
        {
            g = {};
            g.metric = in.getProperty("metric", juce::var()).toString().toStdString();
            const auto take = [&](const char* k, bool& has, double& v) {
                const juce::var x = in.getProperty(k, juce::var());
                if (isNumber(x))
                {
                    has = true;
                    v = static_cast<double>(x);
                }
            };
            take("min", g.hasMin, g.min);
            take("max", g.hasMax, g.max);
            take("target", g.hasTarget, g.target);
            bool hasTol = false, hasWeight = false;
            take("tolerance", hasTol, g.tolerance);
            take("weight", hasWeight, g.weight);
            if (!hasWeight)
                g.weight = 1.0;
            why = measure::invalid(g);
            return why.empty();
        }

        // The history as another model may read it (the planner's view, ChatAgent.h): no thinking (bound to the model,
        // the prompt and the conversation that made it), a compaction block as its summary in words, no server tools'
        // blocks (their results go back only to their own requests), text without citations (which point at them). A
        // message left empty says so, and a history that starts at a summary gets a user message before it.
        juce::Array<juce::var> forAnotherModel(const juce::Array<juce::var>& messages)
        {
            juce::Array<juce::var> out;
            for (const juce::var& m : messages)
            {
                const std::string role = str(m, "role");
                const juce::var content = m.getProperty("content", juce::var());
                const auto* blocks = content.getArray();
                if (blocks == nullptr)
                {
                    out.add(claude::object({ { "role", juce::String(role) }, { "content", content } }));
                    continue;
                }
                juce::Array<juce::var> kept;
                for (const juce::var& b : *blocks)
                {
                    const std::string type = str(b, "type");
                    const bool serverResult = type != "tool_result" && type.size() > 12
                                           && type.compare(type.size() - 12, 12, "_tool_result") == 0;
                    if (type == "thinking" || type == "redacted_thinking" || type == "server_tool_use" || serverResult
                        || type == "fallback")
                        continue;
                    if (type == "compaction")
                        kept.add(text("(A summary of the conversation before this point:)\n" + str(b, "content")));
                    else if (type == "text" && b.hasProperty("citations"))
                        kept.add(text(str(b, "text")));
                    else
                        kept.add(b);
                }
                if (kept.isEmpty())
                    kept.add(text(role == "assistant" ? "(no reply)" : "(nothing)"));
                out.add(claude::object({ { "role", juce::String(role) }, { "content", kept } }));
            }
            if (!out.isEmpty() && str(out[0], "role") == "assistant")
                out.insert(0, claude::object({ { "role", "user" },
                                               { "content", juce::Array<juce::var>{ text("(The conversation so far.)") } } }));
            return out;
        }

        // What the planner is asked before a request's first reply (planTurn).
        std::string plannerBrief(const std::string& builder)
        {
            return "(From the plugin: you are the planner for this request. Another model, " + builder
                 + ", does the work in the plugin with the tools your instructions describe (you call none of them "
                   "now) and follows the plan you write here. Write that plan: what to build (the modules and the "
                   "signal flow, each dial with its range, default and unit, the files and what each holds, the library "
                   "files to use, the panel's sections), the hard decisions (the DSP's methods: a filter's topology, how "
                   "the oscillators avoid aliasing, the envelopes' curves, what must be smoothed), what to measure and "
                   "the checks to set (set_checks), and the order to do it in. Plan only: do not write the files out "
                   "(a short formula or snippet where it must be exact). A small request (a value changed, a question) "
                   "needs a line or two, or none: say so. The user reads your plan.)";
        }

        // A goal's properties in a schema.
        juce::var goalProps(bool weight)
        {
            juce::var p = claude::object({ { "metric", metricEnum("What to measure (CHECKING YOUR WORK lists them)") },
                                           { "min", prop("number", "At least this, in the metric's unit") },
                                           { "max", prop("number", "At most this, in the metric's unit") },
                                           { "target", prop("number", "Near this (instead of min and max)") },
                                           { "tolerance", prop("number", "How near the target counts, in the metric's "
                                                                         "unit (default: the metric's own)") } });
            if (weight)
                p.getDynamicObject()->setProperty("weight", prop("number", "How much this goal counts against the "
                                                                           "others (default 1)"));
            return p;
        }

        // The web tools (ChatAgent.h): the API's server tools, with dynamic filtering unless `direct`.
        constexpr int kWebUses = 5;                // searches, and fetches, a request
        constexpr int kWebFetchTokens = 40000;     // a fetched page's text at most (a PDF is not cut)

        juce::var withWebTools(const juce::var& tools, bool direct)
        {
            juce::Array<juce::var> all;
            if (const auto* arr = tools.getArray())
                all = *arr;
            juce::var search = claude::object({ { "type", "web_search_20260209" }, { "name", "web_search" }, { "max_uses", kWebUses } });
            juce::var fetch = claude::object({ { "type", "web_fetch_20260209" },
                                               { "name", "web_fetch" },
                                               { "max_uses", kWebUses },
                                               { "max_content_tokens", kWebFetchTokens },
                                               { "citations", claude::object({ { "enabled", true } }) } });
            if (direct)
                for (juce::var* t : { &search, &fetch })
                    t->getDynamicObject()->setProperty("allowed_callers", juce::Array<juce::var>{ "direct" });
            all.add(search);
            all.add(fetch);
            return all;
        }

        // A 400 about the web tools: one that wants them called directly, or an organisation without them.
        bool wantsDirect(const std::string& error) { return error.find("allowed_callers") != std::string::npos; }
        bool webDisabled(const std::string& error)
        {
            const std::string e = juce::String(error).toLowerCase().toStdString();
            return (e.find("web search") != std::string::npos || e.find("web fetch") != std::string::npos
                    || e.find("web_search") != std::string::npos || e.find("web_fetch") != std::string::npos)
                && e.find("not enabled") != std::string::npos;
        }

        // ask_user's limits.
        constexpr int kAskMaxQuestions = 4, kAskMinOptions = 2, kAskMaxOptions = 6;
        constexpr std::size_t kAskMaxQuestionText = 400, kAskMaxLabel = 60, kAskMaxDescription = 200, kAskMaxHeader = 16;

        // ask_user's input, checked: false, with why, for one the chat cannot show.
        bool parseQuestions(const juce::var& input, std::vector<ChatQuestion>& out, std::string& why)
        {
            out.clear();
            const auto* qs = input.getProperty("questions", juce::var()).getArray();
            if (qs == nullptr || qs->isEmpty() || qs->size() > kAskMaxQuestions)
            {
                why = "questions must be an array of 1 to " + std::to_string(kAskMaxQuestions) + " questions.";
                return false;
            }
            for (const juce::var& q : *qs)
            {
                ChatQuestion cq;
                cq.question = juce::String(str(q, "question")).trim().toStdString();
                cq.header = juce::String(str(q, "header")).trim().toStdString();
                cq.multi = static_cast<bool>(q.getProperty("multi_select", false));
                if (cq.question.empty() || cq.question.size() > kAskMaxQuestionText)
                {
                    why = "Each question needs its text (at most " + std::to_string(kAskMaxQuestionText) + " characters).";
                    return false;
                }
                if (cq.header.size() > kAskMaxHeader)
                    cq.header = cq.header.substr(0, kAskMaxHeader);
                const auto* os = q.getProperty("options", juce::var()).getArray();
                if (os == nullptr || os->size() < kAskMinOptions || os->size() > kAskMaxOptions)
                {
                    why = "\"" + cq.question + "\" needs " + std::to_string(kAskMinOptions) + " to " + std::to_string(kAskMaxOptions)
                        + " options (the user can always type an answer of their own, so leave out \"Other\").";
                    return false;
                }
                for (const juce::var& o : *os)
                {
                    ChatOption co;
                    co.label = juce::String(str(o, "label")).trim().toStdString();
                    co.description = juce::String(str(o, "description")).trim().toStdString();
                    if (co.label.empty() || co.label.size() > kAskMaxLabel)
                    {
                        why = "Each option needs a label of a few words (at most " + std::to_string(kAskMaxLabel) + " characters).";
                        return false;
                    }
                    for (const ChatOption& other : cq.options)
                        if (other.label == co.label)
                        {
                            why = "Two options of \"" + cq.question + "\" have the label \"" + co.label + "\".";
                            return false;
                        }
                    if (co.description.size() > kAskMaxDescription)
                        co.description = co.description.substr(0, kAskMaxDescription);
                    cq.options.push_back(std::move(co));
                }
                out.push_back(std::move(cq));
            }
            return true;
        }

        // The questions as plain text: the entry's text (the transcript, a screen reader).
        std::string questionsText(const std::vector<ChatQuestion>& qs)
        {
            std::string t;
            for (const ChatQuestion& q : qs)
            {
                t += (t.empty() ? "" : "\n") + (q.header.empty() ? std::string() : q.header + ": ") + q.question
                   + (q.multi ? " (choose any)" : "");
                for (const ChatOption& o : q.options)
                    t += "\n- " + o.label + (o.description.empty() ? std::string() : " -- " + o.description);
            }
            return t;
        }

        juce::var questionsVar(const std::vector<ChatQuestion>& qs)
        {
            juce::Array<juce::var> out;
            for (const ChatQuestion& q : qs)
            {
                juce::Array<juce::var> options;
                for (const ChatOption& o : q.options)
                    options.add(claude::object({ { "l", juce::String::fromUTF8(o.label.c_str()) },
                                                 { "d", juce::String::fromUTF8(o.description.c_str()) } }));
                out.add(claude::object({ { "h", juce::String::fromUTF8(q.header.c_str()) },
                                         { "q", juce::String::fromUTF8(q.question.c_str()) },
                                         { "m", q.multi },
                                         { "o", options } }));
            }
            return out;
        }

        std::vector<ChatQuestion> questionsOf(const juce::var& v)
        {
            std::vector<ChatQuestion> out;
            if (const auto* arr = v.getArray())
                for (const juce::var& q : *arr)
                {
                    ChatQuestion cq{ str(q, "h"), str(q, "q"), {}, static_cast<bool>(q.getProperty("m", false)) };
                    if (const auto* os = q.getProperty("o", juce::var()).getArray())
                        for (const juce::var& o : *os)
                            cq.options.push_back({ str(o, "l"), str(o, "d") });
                    out.push_back(std::move(cq));
                }
            return out;
        }

        // The answer as the tool's result.
        std::string answerText(const std::vector<ChatQuestion>& qs, const std::vector<std::string>& answers)
        {
            std::string t = "The user answered:";
            for (std::size_t i = 0; i < qs.size(); ++i)
            {
                const std::string a = i < answers.size() ? answers[i] : std::string();
                t += "\n- " + qs[i].question + " " + (a.empty() ? std::string("(no answer)") : a);
            }
            return t;
        }
    }

    // ---- construction -----------------------------------------------------------------------------------------------
    ChatAgent::ChatAgent(ChatHost& host, std::unique_ptr<claude::Transport> transport)
        : host_(host), transport_(std::move(transport))
    {
        models_ = builtInModels();
        const chatstore::Choice choice = chatstore::loadChoice();
        model_ = choice.model.empty() ? std::string(kDefaultModel) : choice.model;
        effort_ = choice.effort.empty() ? std::string(kDefaultEffort) : choice.effort;
        fast_ = choice.fast;
        web_ = choice.web;
        planner_ = choice.planner;
        chatstore::forgetOldSignIn();              // what an earlier version kept of a sign-in (ChatStore.h)
    }

    ChatAgent::~ChatAgent()
    {
        cancel_.store(true);
        {
            const std::lock_guard<std::mutex> lock(jobMutex_);
            jobStop_ = true;                       // a job still queued is dropped
        }
        jobCv_.notify_all();
        joinWorker();
        if (jobThread_.joinable())
            jobThread_.join();
    }

    void ChatAgent::setTransport(std::unique_ptr<claude::Transport> transport)
    {
        waitIdle();
        const std::lock_guard<std::mutex> lock(mutex_);
        transport_ = std::move(transport);
        modelsFetched_ = false;
    }

    void ChatAgent::waitIdle()
    {
        joinWorker();
        waitJobs();
    }

    void ChatAgent::queueJob(std::function<void()> job)
    {
        {
            const std::lock_guard<std::mutex> lock(jobMutex_);
            if (jobStop_)
                return;
            jobs_.push_back(std::move(job));
            if (!jobThread_.joinable())
                jobThread_ = std::thread([this] { jobLoop(); });
        }
        jobCv_.notify_all();
    }

    void ChatAgent::jobLoop()
    {
        std::unique_lock<std::mutex> lock(jobMutex_);
        for (;;)
        {
            jobCv_.wait(lock, [this] { return jobStop_ || !jobs_.empty(); });
            if (jobStop_)
                return;
            std::function<void()> job = std::move(jobs_.front());
            jobs_.pop_front();
            jobRunning_ = true;
            lock.unlock();
            job();
            job = nullptr;                         // what it held goes now
            lock.lock();
            jobRunning_ = false;
            jobCv_.notify_all();
        }
    }

    void ChatAgent::waitJobs()
    {
        std::unique_lock<std::mutex> lock(jobMutex_);
        jobCv_.wait(lock, [this] { return jobStop_ || (jobs_.empty() && !jobRunning_); });
    }

    std::vector<std::string> ChatAgent::requestsSent() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

    void ChatAgent::joinWorker()
    {
        if (worker_.joinable())
            worker_.join();
    }

    // ---- ChatPort -----------------------------------------------------------------------------------------------------
    uint64_t ChatAgent::chatRevision() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return revision_;
    }

    ChatState ChatAgent::chatState() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        ChatState s;
        s.hasKey = !key_.empty() || chatstore::hasKey();
        s.keyProblem = keyProblem_;
        s.busy = busy_;
        s.asking = !asking_.empty();
        s.checks = !checks_.empty();
        s.planner = planner_;
        for (const Attached& a : attached_)
            s.attached.push_back(a.picture);
        s.attaching = attaching_;
        s.activity = activity_;
        s.entries = entries_;
        s.models = models_;
        s.model = model_;
        s.effort = effort_;
        s.fast = fast_;
        s.web = web_;
        s.usage = usage_;
        return s;
    }

    bool ChatAgent::saveKey(const std::string& key, std::string& why)
    {
        std::string trimmed = juce::String(key).trim().toStdString();
        if (!chatstore::saveKey(trimmed, &why))
            return false;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            key_ = trimmed;
            keyProblem_.clear();
            modelsFetched_ = false;
            addEntry(ChatEntry::Kind::note, "API key saved, encrypted, for both playground plugins on this Mac.");
        }
        return true;
    }

    void ChatAgent::forgetKey()
    {
        chatstore::forgetKey();
        const std::lock_guard<std::mutex> lock(mutex_);
        key_.clear();
        keyProblem_.clear();
        addEntry(ChatEntry::Kind::note, "API key removed from this Mac.");
    }

    void ChatAgent::setModel(const std::string& id)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto it = std::find_if(models_.begin(), models_.end(), [&](const ChatModel& m) { return m.id == id; });
        if (it == models_.end())
            return;
        model_ = id;
        if (std::find(it->efforts.begin(), it->efforts.end(), effort_) == it->efforts.end() && !it->efforts.empty())
        {
            const auto medium = std::find(it->efforts.begin(), it->efforts.end(), std::string(kDefaultEffort));
            effort_ = medium != it->efforts.end() ? *medium : it->efforts.front();
        }
        chatstore::saveChoice({ model_, effort_, fast_, web_, planner_ });
        touch();
    }

    void ChatAgent::setPlanner(const std::string& id)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!id.empty() && std::none_of(models_.begin(), models_.end(), [&](const ChatModel& m) { return m.id == id; }))
            return;
        planner_ = id;
        chatstore::saveChoice({ model_, effort_, fast_, web_, planner_ });
        touch();
    }

    void ChatAgent::setEffort(const std::string& effort)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto it = std::find_if(models_.begin(), models_.end(), [&](const ChatModel& m) { return m.id == model_; });
        if (it != models_.end() && std::find(it->efforts.begin(), it->efforts.end(), effort) == it->efforts.end())
            return;                                // a level this model does not take
        effort_ = effort;
        chatstore::saveChoice({ model_, effort_, fast_, web_, planner_ });
        touch();
    }

    void ChatAgent::setFast(bool on)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        fast_ = on;
        chatstore::saveChoice({ model_, effort_, fast_, web_, planner_ });
        touch();
    }

    void ChatAgent::setWeb(bool on)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        web_ = on;
        chatstore::saveChoice({ model_, effort_, fast_, web_, planner_ });
        touch();
    }

    void ChatAgent::attach(const std::vector<std::string>& paths)
    {
        std::vector<std::string> take;
        uint64_t epoch = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            for (const std::string& p : paths)
            {
                const std::string name = juce::File(juce::String::fromUTF8(p.c_str())).getFileName().toStdString();
                const int waiting = static_cast<int>(attached_.size()) + attaching_ + static_cast<int>(take.size());
                if (!isPictureName(p))
                    addEntry(ChatEntry::Kind::note, name + " is not a picture (PNG, JPEG, GIF, WebP, HEIC or TIFF).");
                else if (waiting >= chatimages::kMaxPerMessage)
                    addEntry(ChatEntry::Kind::note, "A message takes " + std::to_string(chatimages::kMaxPerMessage)
                                                        + " pictures at most, so " + name + " was not added.");
                else if (picturesInHistory() + waiting >= chatimages::kMaxInChat)
                {
                    addEntry(ChatEntry::Kind::note, "This conversation holds " + std::to_string(chatimages::kMaxInChat)
                                                        + " pictures already (each request carries them all): start a "
                                                          "new chat to add more.");
                    break;
                }
                else
                    take.push_back(p);
            }
            attaching_ += static_cast<int>(take.size());
            epoch = attachEpoch_;
            touch();
        }
        for (const std::string& p : take)
            queueJob([this, p, epoch] {
                chatimages::Prepared r = chatimages::fromFile(p);
                const std::lock_guard<std::mutex> lock(mutex_);
                --attaching_;
                if (epoch != attachEpoch_)
                    return;                        // a new chat since
                if (!r.why.empty())
                    addEntry(ChatEntry::Kind::note, r.why);
                else
                    attached_.push_back({ std::move(r.picture), std::move(r.mediaType), std::move(r.data) });
                touch();
            });
    }

    void ChatAgent::detach(uint32_t id)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto before = attached_.size();
        attached_.erase(std::remove_if(attached_.begin(), attached_.end(),
                                       [&](const Attached& a) { return a.picture->id == id; }),
                        attached_.end());
        if (attached_.size() != before)
            touch();
    }

    int ChatAgent::picturesInHistory() const
    {
        int n = 0;
        for (const juce::var& m : messages_)
            if (str(m, "role") == "user")
                if (const auto* arr = m.getProperty("content", juce::var()).getArray())
                    for (const juce::var& b : *arr)
                        n += str(b, "type") == "image" ? 1 : 0;
        return n;
    }

    void ChatAgent::send(const std::string& message)
    {
        const std::string trimmed = juce::String(message).trim().toStdString();
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (busy_ || (trimmed.empty() && attached_.empty()))
                return;
        }
        joinWorker();                              // a finished turn's thread
        const std::string context = contextBlock();
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            // An open question is answered in the user's own words: its result says so, and they follow.
            for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
                if (it->kind == ChatEntry::Kind::question && it->open)
                {
                    it->typed = true;
                    break;
                }
            juce::Array<juce::var> content =
                missingResults("The user did not choose from the options: their own words follow in this message.");
            // The pictures first, then the words (the vision docs' advice).
            ChatEntry entry;
            entry.kind = ChatEntry::Kind::user;
            entry.text = trimmed;
            for (const Attached& a : attached_)
            {
                content.add(claude::object({ { "type", "image" },
                                             { "source", claude::object({ { "type", "base64" },
                                                                          { "media_type", juce::String(a.mediaType) },
                                                                          { "data", juce::Base64::toBase64(a.data.data(), a.data.size()) } }) } }));
                entry.pictures.push_back(a.picture);
            }
            attached_.clear();
            entries_.push_back(std::move(entry));
            touch();
            if (!trimmed.empty())
                content.add(text(trimmed));
            if (!context.empty())
                content.add(text(context));
            if (effort_ == "xhigh" || effort_ == "max")
                content.add(text(limitNote(maxTokensFor(model_))));
            planNext_ = !planner_.empty();
            deliver(std::move(content));
        }
        startWorker();
    }

    void ChatAgent::answer(const std::vector<std::string>& answers)
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (busy_ || asking_.empty())
                return;
        }
        joinWorker();
        const std::string context = contextBlock();
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (busy_ || asking_.empty())
                return;
            std::vector<ChatQuestion> qs;
            std::vector<std::string> given;
            for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
                if (it->kind == ChatEntry::Kind::question && it->open)
                {
                    qs = it->questions;
                    given = answers;
                    given.resize(qs.size());
                    for (std::string& a : given)
                        a = juce::String(a).trim().toStdString();
                    it->answers = given;
                    break;
                }
            juce::Array<juce::var> content = missingResults(answerText(qs, given));
            if (!context.empty())
                content.add(text(context));
            if (effort_ == "xhigh" || effort_ == "max")
                content.add(text(limitNote(maxTokensFor(model_))));
            deliver(std::move(content));
        }
        startWorker();
    }

    // The tool calls of the last reply without their results (the history's last assistant message, and the user
    // message after it, if any): an open question's, and any a turn left behind. Each gets one; the open question's
    // says `instead`. The question closes.
    juce::Array<juce::var> ChatAgent::missingResults(const std::string& instead)
    {
        juce::Array<juce::var> out;
        int at = messages_.size() - 1;
        while (at >= 0 && str(messages_[at], "role") != "assistant")
            --at;
        if (at >= 0 && at >= messages_.size() - 2)
        {
            std::set<std::string> answered;
            if (at + 1 < messages_.size())
                if (const auto* arr = messages_[at + 1].getProperty("content", juce::var()).getArray())
                    for (const auto& b : *arr)
                        if (str(b, "type") == "tool_result")
                            answered.insert(str(b, "tool_use_id"));
            if (const auto* arr = messages_[at].getProperty("content", juce::var()).getArray())
                for (const auto& b : *arr)
                {
                    const std::string id = str(b, "id");
                    if (str(b, "type") != "tool_use" || answered.count(id) > 0)
                        continue;
                    const bool question = id == asking_;
                    juce::var r = claude::object({ { "type", "tool_result" },
                                                   { "tool_use_id", juce::String(id) },
                                                   { "content", juce::String::fromUTF8((question ? instead
                                                                                         : std::string("(The turn ended before this ran.)")).c_str()) } });
                    if (!question)
                        r.getDynamicObject()->setProperty("is_error", true);
                    out.add(r);
                }
        }
        for (ChatEntry& e : entries_)
            e.open = false;
        asking_.clear();
        return out;
    }

    // A user message's blocks into the history, and the turn under way. A turn that ended without an answer (stopped,
    // or an error), or one that waits for a question's answer, left a user message last: this one joins it, so the
    // roles keep alternating (nothing was answered to it, so no thinking block depends on it), its tool results
    // first, as the API asks.
    void ChatAgent::deliver(juce::Array<juce::var> content)
    {
        const int n = messages_.size();
        if (n > 0 && str(messages_[n - 1], "role") == "user")
        {
            const juce::var last = messages_[n - 1];
            const juce::var before = last.getProperty("content", juce::var());
            juce::Array<juce::var> all, results, rest;
            if (const auto* arr = before.getArray())
                all = *arr;
            all.addArray(content);
            for (const auto& b : all)
                (str(b, "type") == "tool_result" ? results : rest).add(b);
            results.addArray(rest);
            if (auto* o = last.getDynamicObject())
                o->setProperty("content", results);
        }
        else
        {
            messages_.add(claude::object({ { "role", "user" }, { "content", content } }));
        }
        repairHistory();
        resendState_ = false;                      // the state (whole, after a summary) is in this message
        busy_ = true;
        mcpNoted_ = false;                         // Claude Desktop's next call says so again
        activity_ = "Thinking";
        cancel_.store(false);
        touch();
    }

    void ChatAgent::stop()
    {
        cancel_.store(true);
        const std::lock_guard<std::mutex> lock(mutex_);
        if (busy_)
        {
            activity_ = "Stopping";
            touch();
        }
    }

    void ChatAgent::newChat()
    {
        stop();
        joinWorker();
        const std::lock_guard<std::mutex> lock(mutex_);
        messages_.clear();
        entries_.clear();
        seen_.clear();
        lastSignature_ = 0;                        // the next message sends the files again
        usage_.clear();
        activity_.clear();
        busy_ = false;
        asking_.clear();
        attached_.clear();
        ++attachEpoch_;
        checks_.clear();
        checksFailing_ = false;
        touch();
    }

    // ---- the session --------------------------------------------------------------------------------------------------
    std::string ChatAgent::saveState() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        juce::Array<juce::var> entries;
        for (const ChatEntry& e : entries_)
        {
            juce::var entry = claude::object({ { "k", kindName(e.kind) }, { "t", juce::String::fromUTF8(e.text.c_str()) } });
            if (e.kind == ChatEntry::Kind::question)
            {
                juce::Array<juce::var> answers;
                for (const std::string& a : e.answers)
                    answers.add(juce::String::fromUTF8(a.c_str()));
                auto* o = entry.getDynamicObject();
                o->setProperty("q", questionsVar(e.questions));
                o->setProperty("a", answers);
                o->setProperty("o", e.open);
                o->setProperty("y", e.typed);
            }
            if (e.kind == ChatEntry::Kind::plan)
            {
                entry.getDynamicObject()->setProperty("f", juce::String::fromUTF8(e.from.c_str()));
                entry.getDynamicObject()->setProperty("n", e.answer);
            }
            if (e.kind == ChatEntry::Kind::checks)
            {
                juce::Array<juce::var> rows;
                for (const ChatCheck& c : e.checks)
                    rows.add(claude::object({ { "l", juce::String::fromUTF8(c.label.c_str()) },
                                              { "d", juce::String::fromUTF8(c.detail.c_str()) },
                                              { "m", c.measured },
                                              { "p", c.pass } }));
                entry.getDynamicObject()->setProperty("c", rows);
            }
            if (!e.pictures.empty())
            {
                juce::Array<juce::var> pictures;
                for (const ChatPicturePtr& p : e.pictures)
                    pictures.add(claude::object({ { "n", juce::String::fromUTF8(p->name.c_str()) },
                                                  { "w", p->width },
                                                  { "h", p->height },
                                                  { "m", juce::String(p->thumbType) },
                                                  { "d", juce::Base64::toBase64(p->thumbData.data(), p->thumbData.size()) } }));
                entry.getDynamicObject()->setProperty("p", pictures);
            }
            entries.add(entry);
        }
        // The panel's pictures (view_panel's images, hundreds of KB each) stay out of the session: a line in each one's
        // place, and a mark that the history was changed, so a session that loads it drops its thinking (bound to the
        // pictures).
        bool cut = false;
        juce::Array<juce::var> messages;
        for (const juce::var& m : messages_)
        {
            const juce::Array<juce::var>* content = m.getProperty("content", juce::var()).getArray();
            bool pictures = false;
            if (content != nullptr)
                for (const juce::var& b : *content)
                    if (str(b, "type") == "tool_result")
                        if (const auto* inner = b.getProperty("content", juce::var()).getArray())
                            for (const juce::var& x : *inner)
                                pictures = pictures || str(x, "type") == "image";
            if (!pictures)
            {
                messages.add(m);
                continue;
            }
            cut = true;
            juce::Array<juce::var> blocks;
            for (const juce::var& b : *content)
            {
                const auto* inner = b.getProperty("content", juce::var()).getArray();
                if (str(b, "type") != "tool_result" || inner == nullptr)
                {
                    blocks.add(b);
                    continue;
                }
                juce::Array<juce::var> kept;
                for (const juce::var& x : *inner)
                    kept.add(str(x, "type") == "image"
                                 ? claude::object({ { "type", "text" }, { "text", "(A picture of the panel or the sound: not kept in the session.)" } })
                                 : x);
                juce::var nb = claude::object({});
                for (const auto& kv : b.getDynamicObject()->getProperties())
                    nb.getDynamicObject()->setProperty(kv.name, kv.value);
                nb.getDynamicObject()->setProperty("content", kept);
                blocks.add(nb);
            }
            juce::var nm = claude::object({});
            for (const auto& kv : m.getDynamicObject()->getProperties())
                nm.getDynamicObject()->setProperty(kv.name, kv.value);
            nm.getDynamicObject()->setProperty("content", blocks);
            messages.add(nm);
        }
        juce::var state = claude::object({ { "version", 1 },
                                           { "prompt", juce::String(promptPrint()) },
                                           { "messages", messages },
                                           { "entries", entries } });
        if (cut)
            state.getDynamicObject()->setProperty("pictures_cut", true);
        if (!asking_.empty())
            state.getDynamicObject()->setProperty("asking", juce::String(asking_));
        state.getDynamicObject()->setProperty("web", historyWeb_);
        if (!checks_.empty())
        {
            // Each check as set_checks takes it, so loadState reads it back the same way.
            juce::Array<juce::var> checks;
            for (const measure::Check& c : checks_)
            {
                juce::var v = claude::object({ { "label", juce::String::fromUTF8(c.label.c_str()) },
                                               { "metric", juce::String(c.goal.metric) } });
                auto* o = v.getDynamicObject();
                if (c.goal.hasMin)
                    o->setProperty("min", c.goal.min);
                if (c.goal.hasMax)
                    o->setProperty("max", c.goal.max);
                if (c.goal.hasTarget)
                    o->setProperty("target", c.goal.target);
                if (c.goal.tolerance > 0.0)
                    o->setProperty("tolerance", c.goal.tolerance);
                const measure::Request& r = c.request;
                juce::Array<juce::var> notes;
                for (const int n : r.notes)
                    notes.add(n);
                if (!notes.isEmpty())
                    o->setProperty("notes", notes);
                o->setProperty("velocity", r.velocity);
                o->setProperty("hold_seconds", r.holdSeconds);
                o->setProperty("tail_seconds", r.tailSeconds);
                o->setProperty("bpm", r.bpm);
                o->setProperty("modwheel", r.modwheel);
                o->setProperty("pitchbend", r.pitchbend);
                if (!r.dials.empty())
                {
                    juce::var dials = claude::object({});
                    for (const auto& [k, x] : r.dials)
                        dials.getDynamicObject()->setProperty(juce::Identifier(juce::String::fromUTF8(k.c_str())), x);
                    o->setProperty("dials", dials);
                }
                checks.add(v);
            }
            state.getDynamicObject()->setProperty("checks", checks);
        }
        return claude::toJson(state);
    }

    // What a replayed thinking block is bound to besides the messages before it (the preserved-thinking docs): the
    // system prompt and the tools.
    std::string ChatAgent::promptPrint()
    {
        char buf[24];
        const uint64_t h = hashText(systemPrompt() + '\x1f' + claude::toJson(toolDefinitions()));
        std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
        return buf;
    }

    void ChatAgent::loadState(const std::string& json)
    {
        stop();
        joinWorker();
        const juce::var v = claude::parseJson(json);
        const std::lock_guard<std::mutex> lock(mutex_);
        messages_.clear();
        entries_.clear();
        seen_.clear();
        lastSignature_ = 0;                        // the next message sends the files again
        busy_ = false;
        asking_.clear();
        activity_.clear();
        historyWeb_ = static_cast<bool>(v.getProperty("web", false));
        checks_.clear();
        checksFailing_ = false;
        if (const auto* cs = v.getProperty("checks", juce::var()).getArray())
            for (const auto& c : *cs)
            {
                measure::Check k;
                std::string why;
                if (!goalOf(c, k.goal, why) || static_cast<int>(checks_.size()) >= kMaxChecks)
                    continue;
                k.label = str(c, "label");
                k.request = renderOf(c);
                checks_.push_back(std::move(k));
            }
        const juce::var messages = v.getProperty("messages", juce::var()), entries = v.getProperty("entries", juce::var());
        if (const auto* arr = messages.getArray())
            messages_ = *arr;
        if (const auto* arr = entries.getArray())
            for (const auto& e : *arr)
            {
                ChatEntry entry;
                entry.kind = kindOf(str(e, "k"));
                entry.text = str(e, "t");
                entry.from = str(e, "f");
                entry.answer = static_cast<bool>(e.getProperty("n", false));
                if (const auto* rows = e.getProperty("c", juce::var()).getArray())
                    for (const auto& r : *rows)
                        entry.checks.push_back({ str(r, "l"), str(r, "d"), static_cast<bool>(r.getProperty("m", false)),
                                                 static_cast<bool>(r.getProperty("p", false)) });
                if (entry.kind == ChatEntry::Kind::question)
                {
                    entry.questions = questionsOf(e.getProperty("q", juce::var()));
                    if (const auto* as = e.getProperty("a", juce::var()).getArray())
                        for (const auto& a : *as)
                            entry.answers.push_back(a.toString().toStdString());
                    entry.answers.resize(entry.questions.size());
                    entry.open = static_cast<bool>(e.getProperty("o", false));
                    entry.typed = static_cast<bool>(e.getProperty("y", false));
                }
                if (const auto* ps = e.getProperty("p", juce::var()).getArray())
                    for (const auto& pv : *ps)
                    {
                        auto p = std::make_shared<ChatPicture>();
                        p->id = chatimages::nextId();
                        p->name = str(pv, "n");
                        p->width = static_cast<int>(pv.getProperty("w", 0));
                        p->height = static_cast<int>(pv.getProperty("h", 0));
                        p->thumbType = str(pv, "m");
                        juce::MemoryOutputStream bytes;
                        if (juce::Base64::convertFromBase64(bytes, pv.getProperty("d", "").toString()))
                        {
                            const auto* b = static_cast<const uint8_t*>(bytes.getData());
                            p->thumbData.assign(b, b + bytes.getDataSize());
                        }
                        chatimages::decodeThumb(*p);   // none: the conversation shows its frame without the picture
                        entry.pictures.push_back(std::move(p));
                    }
                entries_.push_back(std::move(entry));
            }
        // Saved with another system prompt or other tools (an earlier version of the plugin): the API would refuse
        // its thinking blocks (bound to a different conversation), so the history goes on without them.
        if (str(v, "prompt") != promptPrint() || static_cast<bool>(v.getProperty("pictures_cut", false)))
            dropThinking();
        repairHistory();                           // a session saved by a version that cut it at the wrong place
        // A question that waited when the session was saved waits again, if its call is still the last reply's.
        const std::string asking = str(v, "asking");
        bool waiting = false;
        if (!asking.empty() && !messages_.isEmpty())
        {
            const int last = messages_.size() - 1;
            const int reply = str(messages_[last], "role") == "assistant" ? last : last - 1;
            if (reply >= 0)
                if (const auto* arr = messages_[reply].getProperty("content", juce::var()).getArray())
                    for (const auto& b : *arr)
                        waiting = waiting || (str(b, "type") == "tool_use" && str(b, "id") == asking);
        }
        bool reopened = false;
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
        {
            const bool open = waiting && !reopened && it->kind == ChatEntry::Kind::question && it->open;
            reopened = reopened || open;
            it->open = open;
        }
        if (reopened)
            asking_ = asking;
        touch();
    }

    // ---- bookkeeping (mutex held) -------------------------------------------------------------------------------------
    void ChatAgent::touch() { ++revision_; }

    void ChatAgent::appendAnswer(const juce::Array<juce::var>& content)
    {
        messages_.add(claude::object({ { "role", "assistant" }, { "content", content } }));
        const bool compacted = std::any_of(content.begin(), content.end(),
                                           [](const juce::var& b) { return str(b, "type") == "compaction"; });
        if (!compacted)
            return;
        // The API ignores everything before a compaction block: the history starts at it (repairHistory). Claude has
        // seen no file since, as far as the summary goes (it leaves their text out): the state goes again, whole, with
        // what comes next, the user's next message or this turn's next tool results.
        repairHistory();
        seen_.clear();
        lastSignature_ = 0;
        resendState_ = true;
    }

    // A history the API takes. After a compaction block, nothing before it: the API ignores it anyway, and a message
    // kept from before it can be the results of tool calls the summary took away, which the API refuses ("unexpected
    // tool_use_id found in tool_result blocks"). The history then starts with the assistant message that carries the
    // block, as the API's examples continue after one. And no tool result without its call in the message before it:
    // such a result goes (a message left with nothing says so in a line, so the roles still alternate).
    void ChatAgent::repairHistory()
    {
        for (int i = messages_.size(); --i >= 0;)
        {
            if (str(messages_[i], "role") != "assistant")
                continue;
            const juce::var content = messages_[i].getProperty("content", juce::var());
            const auto* blocks = content.getArray();
            int at = -1;
            for (int j = 0; blocks != nullptr && j < blocks->size(); ++j)
                if (str((*blocks)[j], "type") == "compaction")
                    at = j;
            if (at < 0)
                continue;
            if (at > 0)
            {
                juce::Array<juce::var> fromBlock;
                for (int j = at; j < blocks->size(); ++j)
                    fromBlock.add((*blocks)[j]);
                if (auto* o = messages_[i].getDynamicObject())
                    o->setProperty("content", fromBlock);
            }
            messages_.removeRange(0, i);
            break;
        }
        for (int i = 0; i < messages_.size(); ++i)
        {
            if (str(messages_[i], "role") != "user")
                continue;
            std::set<std::string> calls;           // the tool calls of the message before
            const juce::var before = i > 0 ? messages_[i - 1].getProperty("content", juce::var()) : juce::var();
            if (const auto* arr = before.getArray())   // the var holds the array: never getArray() on a temporary
                for (const auto& b : *arr)
                    if (str(b, "type") == "tool_use")
                        calls.insert(str(b, "id"));
            const juce::var content = messages_[i].getProperty("content", juce::var());
            const auto* blocks = content.getArray();
            if (blocks == nullptr)
                continue;
            juce::Array<juce::var> kept;
            for (const auto& b : *blocks)
                if (str(b, "type") != "tool_result" || calls.count(str(b, "tool_use_id")) > 0)
                    kept.add(b);
            if (kept.size() == blocks->size())
                continue;
            if (kept.isEmpty())
                kept.add(text("(The results of tool calls that are no longer in this conversation.)"));
            if (auto* o = messages_[i].getDynamicObject())
                o->setProperty("content", kept);
        }
    }

    // The history without its thinking: an assistant message keeps its text, tool calls and compaction (one left with
    // nothing says so in a line, so the roles still alternate).
    void ChatAgent::dropThinking()
    {
        for (const auto& message : messages_)
        {
            if (str(message, "role") != "assistant")
                continue;
            const juce::var content = message.getProperty("content", juce::var());
            const auto* blocks = content.getArray();
            if (blocks == nullptr)
                continue;
            juce::Array<juce::var> kept;
            for (const auto& b : *blocks)
                if (const std::string type = str(b, "type"); type != "thinking" && type != "redacted_thinking")
                    kept.add(b);
            if (kept.size() == blocks->size())
                continue;
            if (kept.isEmpty())
                kept.add(text("(no answer)"));
            if (auto* o = message.getDynamicObject())
                o->setProperty("content", kept);
        }
    }

    // The max_tokens a request to this model asks for: the model's own limit, at most kMaxTokensCap.
    int ChatAgent::maxTokensFor(const std::string& id) const
    {
        const auto model = std::find_if(models_.begin(), models_.end(), [&](const ChatModel& m) { return m.id == id; });
        return std::min(kMaxTokensCap, model != models_.end() ? model->maxTokens : kDefaultMaxTokens);
    }

    void ChatAgent::markSeen(const std::string& name, const std::string& text) { seen_[name] = hashText(text); }

    void ChatAgent::addEntry(ChatEntry::Kind kind, std::string t)
    {
        ChatEntry e;
        e.kind = kind;
        e.text = std::move(t);
        entries_.push_back(std::move(e));
        touch();
    }

    void ChatAgent::appendToLast(ChatEntry::Kind kind, std::string_view t)
    {
        if (entries_.empty() || entries_.back().kind != kind)
        {
            ChatEntry e;
            e.kind = kind;
            entries_.push_back(std::move(e));
        }
        entries_.back().text += t;
        touch();
    }

    void ChatAgent::setActivity(std::string activity)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (activity_ != activity && !(cancel_.load() && busy_))
        {
            activity_ = std::move(activity);
            touch();
        }
    }

    void ChatAgent::startWorker()
    {
        worker_ = std::thread([this] { runTurn(); });
    }

    void ChatAgent::ensureKey()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        loadKey();
    }

    void ChatAgent::loadKey()
    {
        if (!key_.empty())
            return;
        std::string why;
        if (const auto k = chatstore::loadKey(&why))
        {
            key_ = *k;
            keyProblem_.clear();
        }
        else if (chatstore::hasKey())
        {
            keyProblem_ = why;
        }
    }

    claude::Headers ChatAgent::headersFor(const std::string& key)
    {
        return { { "x-api-key", key }, { "anthropic-version", claude::kApiVersion } };
    }

    // A 401 (from /v1/models): the key is refused. It stays, and the chat says so until the user replaces or removes
    // it, unless another key has replaced it since.
    void ChatAgent::refused(const std::string& key, int status, const std::string& why)
    {
        if (status != 401 || key != key_)
            return;
        keyProblem_ = why;
        touch();
    }

    // ---- the plugin's state, for Claude -------------------------------------------------------------------------------
    // Sent with a user message when anything in it changed since Claude last got one. A file whose text Claude has
    // already seen (in an earlier block, through read_file, or as it wrote it) goes as one line instead of its text, so
    // a long program is not sent again with every message.
    std::string ChatAgent::contextBlock()
    {
        const Draft d = host_.chatDraft();
        const Sources running = host_.chatRunning();
        const CompileStatus status = host_.chatLastStatus();
        const std::string shown =
            d.files.empty() ? std::string(kProgramName)
                            : d.files[static_cast<std::size_t>(std::clamp(d.active, 0, static_cast<int>(d.files.size()) - 1))].name;
        std::string tail = "What plays: "
                         + std::string(running == d.files ? "these files" : "an earlier version of the files (they "
                                                                            "were edited since the last run)")
                         + "\nLast compile: " + compileLine(status) + "Dials of the program that plays:\n"
                         + dialLines(host_.chatDials(), host_) + (status.ok ? panelLine(status.panel, status.controls) : "");
        const uint32_t a = host_.chatAlerts();
        tail += "Output alerts: ";
        tail += a == 0 ? "none" : ((a & 1u) != 0 ? ((a & 2u) != 0 ? "NaN or inf replaced, clipped" : "NaN or inf replaced")
                                                 : "clipped (beyond +-4)");
        uint64_t signature = hashText(tail + '\x1f' + shown);
        for (const SourceFile& f : d.files)
            signature = (signature ^ hashText(f.name + '\x1f' + f.text)) * 1099511628211ull;

        const std::lock_guard<std::mutex> lock(mutex_);
        if (signature == lastSignature_)
            return {};
        lastSignature_ = signature;
        std::string c = "<plugin_state>\nThe plugin's files, a tab each in the editor (the program is the first; the "
                        "editor shows " + shown + "):\n";
        bool unchanged = false;
        for (const SourceFile& f : d.files)
        {
            const auto it = seen_.find(f.name);
            if (it != seen_.end() && it->second == hashText(f.text))
            {
                c += "<file name=\"" + f.name + "\" lines=\"" + std::to_string(lineCount(f.text)) + "\" unchanged=\"true\"/>\n";
                unchanged = true;
                continue;
            }
            c += "<file name=\"" + f.name + "\">\n" + f.text + (f.text.empty() || f.text.back() == '\n' ? "" : "\n")
               + "</file>\n";
            markSeen(f.name, f.text);
        }
        if (unchanged)
            c += "(A file marked unchanged is as you last saw it in this conversation; read_file shows it again.)\n";
        c += tail + "\n</plugin_state>";
        return c;
    }

    // ---- the request -------------------------------------------------------------------------------------------------
    juce::var ChatAgent::toolDefinitions()
    {
        juce::Array<juce::var> tools;
        tools.add(tool("read_file",
                       "Read one of the plugin's files as it is now in the editor. Call this before editing a file you "
                       "have not seen in its current form (the user may have typed in it since the last plugin_state).",
                       claude::object({ { "name", prop("string", "The file's name, such as program.fm") } }),
                       { "name" }));
        tools.add(tool("write_file",
                       "Create a file or replace a file's whole text. Use it for a new program or a large rewrite; use "
                       "edit_file for a change to part of a file. The editor shows the file and the user can undo the "
                       "change there. A write does not play anything: call run_program after the files are ready.",
                       claude::object({ { "name", prop("string", "The file's name: letters, digits, _, - and ., not "
                                                                  "starting with '.', such as program.fm or "
                                                                  "filters.fm") },
                                        { "content", prop("string", "The file's whole text") } }),
                       { "name", "content" }));
        tools.add(tool("edit_file",
                       "Replace one exact passage of a file with new text. old_text must occur exactly once in the "
                       "file, with the same whitespace; include enough surrounding lines to make it unique. Prefer "
                       "this to write_file for changes to part of a file.",
                       claude::object({ { "name", prop("string", "The file's name") },
                                        { "old_text", prop("string", "The passage as it is now, exactly") },
                                        { "new_text", prop("string", "What replaces it") } }),
                       { "name", "old_text", "new_text" }));
        tools.add(tool("rename_file",
                       "Rename a file. #include lines that name it are not changed: edit them too.",
                       claude::object({ { "name", prop("string", "The file's name now") },
                                        { "new_name", prop("string", "Its new name") } }),
                       { "name", "new_name" }));
        tools.add(tool("delete_file",
                       "Delete a file the program no longer needs (never the program, the first file). Ask the user "
                       "before deleting a file you did not write.",
                       claude::object({ { "name", prop("string", "The file's name") } }), { "name" }));
        {
            juce::Array<juce::var> names;
            for (const LibraryFile& f : kLibrary)
                names.add(juce::String(f.name));
            juce::var name = prop("string", "The library file's name, as THE LIBRARY lists it");
            name.getDynamicObject()->setProperty("enum", names);
            tools.add(tool("add_library_file",
                           "Add one of the plugin's ready-made building blocks (THE LIBRARY in your instructions) to the "
                           "editor as a tab of its own, and get its whole text back. #include it in the program to use "
                           "it. A file of that name in the editor already is left as it is.",
                           claude::object({ { "name", name } }), { "name" }));
        }
        tools.add(tool("run_program",
                       "Compile the files as they are now and, when they compile, play them (as the editor's RUN "
                       "does). Returns the compiler's messages, or the dials of the new program. Call it after every "
                       "set of changes, and fix any error it reports before you finish.",
                       claude::object({}), {}));
        tools.add(tool("set_dial",
                       "Set one of the running program's dials (its parameters) to a value in its range, in the "
                       "parameter's own units. Use it to leave a new sound at good settings, like a preset. The user "
                       "can move the dial afterwards.",
                       claude::object({ { "name", prop("string", "The parameter's name, as declared") },
                                        { "value", prop("number", "The value, inside the parameter's range") } }),
                       { "name", "value" }));
        {
            juce::var notes = claude::object({ { "type", "array" },
                                               { "items", claude::object({ { "type", "integer" } }) },
                                               { "description", "An instrument's MIDI notes, played together (69 is A, "
                                                                "440 Hz). Default: one note, 48 (C3). An effect "
                                                                "ignores it." } });
            juce::var dials = claude::object({ { "type", "object" },
                                               { "additionalProperties", claude::object({ { "type", "number" } }) },
                                               { "description", "Dial values to render with, by parameter name, in "
                                                                "the parameter's own units. Dials not named keep the "
                                                                "values they have now." } });
            tools.add(tool("measure_output",
                           "Render the program in the editor offline and measure what comes out. An instrument: the "
                           "notes held and released, reporting the held level (peak and RMS in dBFS, DC, the stereo "
                           "correlation), the strongest partials and the spectral centroid, with one note the strongest "
                           "component that is no harmonic of it (aliasing, or the sound's own), the largest step between "
                           "two samples (clicks), when the release falls 60 dB and whether the voice ends, and a "
                           "chord's summed level. An effect: a sine, noise, an impulse and silence through it, "
                           "reporting the gain, the partials it adds, the brightness in and out, the impulse "
                           "response's delay and decay, and any sound it makes from silence. Both: the output "
                           "guard's alerts and the CPU cost. You cannot hear the plugin: measure every new sound once "
                           "it compiles, and measure when debugging what the user hears. It compiles the files as "
                           "they are now (they need not be running) and changes nothing that plays.",
                           claude::object({ { "notes", notes },
                                            { "velocity", prop("number", "An instrument's velocity, 0 to 1 (default 0.8)") },
                                            { "hold_seconds", prop("number", "How long the notes are held (an effect: how "
                                                                             "long the sine and the noise play); default "
                                                                             "1, at most 10") },
                                            { "tail_seconds", prop("number", "How long to go on after the release (an "
                                                                             "effect: after the impulse); default 1.5, at "
                                                                             "most 10") },
                                            { "dials", dials },
                                            { "bpm", prop("number", "The host's tempo for a program that reads it "
                                                                    "(host.fm: the host plays from the song's start); "
                                                                    "default 120") },
                                            { "modwheel", prop("number", "An instrument's mod wheel, 0 to 1, from the "
                                                                         "start (default 0)") },
                                            { "pitchbend", prop("number", "An instrument's pitch wheel, -1 to 1, from "
                                                                          "the start (default 0)") } }),
                           {}));
        }
        tools.add(tool("view_panel",
                       "See the plugin's interface: a picture (PNG) of the program's panel as the user sees it in the "
                       "PANEL view (its panel.gui, or its dials), drawn now from the program that plays. Use it after "
                       "you write or change panel.gui and run it, to check the layout as a designer would: crowding, "
                       "overlaps, text cut off, ragged rows, sizes and colours that do not read, a page left empty; "
                       "then fix what you see. view \"window\" shows the whole editor instead (the code and the "
                       "chat). page names a tabs element's page to show (the text says which pages there are). "
                       "Shaders show as a striped placeholder (the GPU draws them) and the displays show the sound "
                       "playing now. A picture costs about 2,000 input tokens: look once or twice for a new interface, "
                       "not after every small change.",
                       claude::object({ { "view", claude::object({ { "type", "string" },
                                                                   { "enum", juce::Array<juce::var>{ "panel", "window" } },
                                                                   { "description", "panel (the default): the program's "
                                                                                    "panel; window: the whole editor" } }) },
                                        { "page", prop("string", "A page's title to show (a tabs element's), else the "
                                                                 "pages the user has") } }),
                       {}));
        {
            juce::var props = claude::object({});
            addRenderProps(props);
            if constexpr (!kInstrument)
            {
                props.getDynamicObject()->setProperty(
                    "input", claude::object({ { "type", "string" },
                                              { "enum", juce::Array<juce::var>{ "sweep", "noise", "impulse", "sine", "silence" } },
                                              { "description", "What goes in, for hold_seconds, then silence for tail_seconds: "
                                                               "sweep (the default: a sine from 20 Hz to 20 kHz at -12 "
                                                               "dBFS, rising evenly in octaves), noise (-18 dBFS RMS), "
                                                               "impulse (0.5, then silence: a reverb's or a delay's "
                                                               "response), sine (at frequency, -12 dBFS) or silence" } }));
                props.getDynamicObject()->setProperty("frequency", prop("number", "The sine's frequency in Hz (default 220)"));
            }
            props.getDynamicObject()->setProperty("from_seconds", prop("number", "The close-up's start, in seconds from "
                                                                                 "the start (default: a few cycles of "
                                                                                 "the held sound)"));
            props.getDynamicObject()->setProperty("to_seconds", prop("number", "The close-up's end (at most 2 s after "
                                                                               "its start)"));
            tools.add(tool("view_sound",
                           kInstrument
                               ? "See the sound: a picture (PNG) of the program rendered offline as measure_output "
                                 "renders it (the notes held, then released), drawn: the waveform and the level over "
                                 "time, a spectrogram (frequency against time, 20 Hz to 20 kHz on a log scale), the "
                                 "average spectrum while held (a note's harmonics marked) and a close-up of a few "
                                 "cycles (or of from_seconds to to_seconds). You cannot hear the plugin: look when the "
                                 "sound's shape matters, as an envelope's attack and release, a filter's sweep, "
                                 "aliasing (lines that fold back down in the spectrogram), a click (a spike in the "
                                 "waveform, a thin vertical line in the spectrogram), a wave's shape, noise, a "
                                 "modulation's motion. A picture costs about 1,200 input tokens: measure_output gives "
                                 "numbers, this shows the shape. It compiles the files as they are now and changes "
                                 "nothing that plays; the user sees the picture too."
                               : "See the sound: a picture (PNG) of the program rendered offline with an input (a sweep "
                                 "by default), drawn: the waveform and the level over time, a spectrogram (frequency "
                                 "against time, 20 Hz to 20 kHz on a log scale), the average spectrum while the input "
                                 "plays and a close-up of a few cycles (or of from_seconds to to_seconds). You cannot "
                                 "hear the plugin: look when the sound's shape matters, as a filter's response (a "
                                 "sweep: where it is cut), what a drive adds (a sweep's or a sine's harmonics, aliasing "
                                 "that folds back down), a reverb's or a delay's tail (an impulse), a modulation's "
                                 "motion, a click. A picture costs about 1,200 input tokens: measure_output gives "
                                 "numbers, this shows the shape. It compiles the files as they are now and changes "
                                 "nothing that plays; the user sees the picture too.",
                           props, {}));
        }
        {
            juce::var item = goalProps(false);
            item.getDynamicObject()->setProperty("label", prop("string", "What it checks, in a few words the user reads "
                                                                         "(\"Releases within two seconds\")"));
            addRenderProps(item);
            juce::var checks = claude::object({ { "type", "array" },
                                                { "maxItems", kMaxChecks },
                                                { "items", claude::object({ { "type", "object" },
                                                                            { "properties", item },
                                                                            { "required", juce::Array<juce::var>{ "label", "metric" } } }) },
                                                { "description", "The checks, each a metric with min, max or target, "
                                                                 "and the render it is measured in (measure_output's "
                                                                 "settings; the defaults when left out). Empty: no "
                                                                 "checks" } });
            tools.add(tool("set_checks",
                           "Set the checks this work must pass: what the user asked for, as numbers the plugin "
                           "measures (the metrics in CHECKING YOUR WORK), each with min, max or a target. The list "
                           "replaces the checks set before; an empty one clears them. The plugin measures them now, "
                           "again after every run_program (the results follow its own), and when you end a turn in "
                           "which you changed something: while any fails, that turn goes on with the results (twice "
                           "at most), so fix the program, "
                           "change a check that asks for the wrong thing, or say why it cannot pass. The user sees them "
                           "as a checklist. Set them early in a large piece of work, from the request (\"a pluck that "
                           "dies away in a second\": decay_s at most 1; \"no aliasing\": inharmonic_db at most -70 "
                           "at note 96), not afterwards to describe what was built.",
                           claude::object({ { "checks", checks } }), { "checks" }));
        }
        {
            juce::var props = claude::object({});
            addRenderProps(props);
            props.getDynamicObject()->setProperty(
                "tune", claude::object({ { "type", "array" },
                                         { "items", claude::object({ { "type", "string" } }) },
                                         { "minItems", 1 },
                                         { "maxItems", measure::kMaxTuned },
                                         { "description", "The float dials to tune, by parameter name (ints and bools "
                                                          "are set with set_dial)" } }));
            props.getDynamicObject()->setProperty(
                "goals", claude::object({ { "type", "array" },
                                          { "minItems", 1 },
                                          { "maxItems", measure::kMaxGoals },
                                          { "items", claude::object({ { "type", "object" },
                                                                      { "properties", goalProps(true) },
                                                                      { "required", juce::Array<juce::var>{ "metric" } } }) },
                                          { "description", "What the measurements should come to" } }));
            props.getDynamicObject()->setProperty("budget", prop("integer", "Renders at most (default 150, at most 400; "
                                                                            "each takes a few to a few hundred ms)"));
            props.getDynamicObject()->setProperty("apply", prop("boolean", "Set the dials to the best values found "
                                                                           "(default true)"));
            props.getDynamicObject()->setProperty("seconds", prop("number", "How long it may search, in seconds (default "
                                                                             "120, at most 170): it stops there with the best "
                                                                             "so far"));
            tools.add(tool("tune_dials",
                           "Find the values of up to eight float dials that bring measurements to goals, by rendering "
                           "the program offline many times (as measure_output does, up to budget renders) and "
                           "searching, instead of guessing: from the dials as they are, through points spread over "
                           "their ranges, then a simplex search. Goals are metrics (CHECKING YOUR WORK) with min, max "
                           "or a target. Use it to fit a level, a brightness, an envelope's times, a filter, an "
                           "amount of drive, once the program does the right thing and only its settings are off; it "
                           "cannot make a program do what it cannot. The best values go on the dials, as set_dial "
                           "sets them, unless apply is false; the result gives the dials and the measurements before "
                           "and after. Dials not tuned keep their values (or those in dials). It changes no file.",
                           props, { "tune", "goals" }));
        }
        {
            juce::var option = claude::object(
                { { "type", "object" },
                  { "properties", claude::object({ { "label", prop("string", "The choice in a few words (at most 60 "
                                                                             "characters), such as Warm and analog") },
                                                   { "description", prop("string", "A line of what it means or does "
                                                                                   "(optional)") } }) },
                  { "required", juce::Array<juce::var>{ "label" } } });
            juce::var options = claude::object({ { "type", "array" },
                                                 { "minItems", kAskMinOptions },
                                                 { "maxItems", kAskMaxOptions },
                                                 { "items", option },
                                                 { "description", "2 to 6 choices. The user can always type an answer "
                                                                  "of their own instead, so leave out \"Other\"" } });
            juce::var question = claude::object(
                { { "type", "object" },
                  { "properties", claude::object({ { "question", prop("string", "The question: one sentence, ending "
                                                                                "in a question mark") },
                                                   { "header", prop("string", "A tag of a word or two above it (at "
                                                                              "most 16 characters), such as Sound or "
                                                                              "Panel") },
                                                   { "options", options },
                                                   { "multi_select", prop("boolean", "true when several options may "
                                                                                     "be chosen together (default "
                                                                                     "false: one)") } }) },
                  { "required", juce::Array<juce::var>{ "question", "options" } } });
            tools.add(tool("ask_user",
                           "Ask the user one to four questions they answer with a click: the chat shows each question "
                           "with its options as buttons, and your turn waits until they answer. The result is what "
                           "they chose (the options' labels), or a note that they wrote a message instead, which "
                           "follows it. Use it when a request leaves choices only the user can make, above all a new "
                           "user's open request (\"make me something cool\") and before large work that depends on the "
                           "answer: the sound's character or style, what it is for, how many controls, the panel's "
                           "look. Do not ask what you can decide, measure or see yourself, nor for permission to do "
                           "ordinary work. Put the option you recommend first, its label ending in \"(Recommended)\". "
                           "Call it once in a reply, last.",
                           claude::object({ { "questions", claude::object({ { "type", "array" },
                                                                            { "minItems", 1 },
                                                                            { "maxItems", kAskMaxQuestions },
                                                                            { "items", question } }) } }),
                           { "questions" }));
        }
        tools.add(tool("ask_planner",
                       "Ask the planning model the user chose (the model menu's Plan with; each of the user's requests "
                       "then comes with its plan, in <plan>) a design question: an architecture, a DSP method, a "
                       "trade-off, or what to do when a measurement or a check shows the plan does not work. It sees "
                       "the conversation as it stood before your current reply, not your thinking: say in context what "
                       "you found and tried. Four times a turn at most; without a planner it is refused (then decide "
                       "yourself).",
                       claude::object({ { "question", prop("string", "The question, on its own") },
                                        { "context", prop("string", "What you found or tried that bears on it "
                                                                    "(measurements, errors, what you changed)") } }),
                       { "question" }));
        tools.add(tool("get_status",
                       "Read the plugin's state now: the files, whether the program that plays matches them, the last "
                       "compile, the dials and their values, the CPU load and the output guard's alerts (NaN or inf "
                       "replaced, samples clipped). Use it when debugging what the user hears.",
                       claude::object({}), {}));
        return juce::var(tools);
    }

    juce::var ChatAgent::buildRequest(std::string& beta) const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        juce::Array<juce::var> system;
        system.add(claude::object({ { "type", "text" },
                                    { "text", juce::String::fromUTF8(systemPrompt().c_str()) },
                                    { "cache_control", claude::object({ { "type", "ephemeral" }, { "ttl", "1h" } }) } }));
        const auto model = std::find_if(models_.begin(), models_.end(),
                                        [&](const ChatModel& m) { return m.id == turnModel_; });
        const int maxTokens = maxTokensFor(turnModel_);
        juce::var body = claude::object({ { "model", juce::String(turnModel_) },
                                          { "max_tokens", maxTokens },
                                          { "stream", true },
                                          { "system", system },
                                          { "tools", turnWeb_ ? withWebTools(toolDefinitions(), webDirect_.count(turnModel_) > 0)
                                                              : toolDefinitions() },
                                          { "messages", messages_ },
                                          { "thinking", claude::object({ { "type", "adaptive" },
                                                                         { "display", "summarized" } }) } });
        auto* o = body.getDynamicObject();
        if (model != models_.end()
            && std::find(model->efforts.begin(), model->efforts.end(), turnEffort_) != model->efforts.end())
            o->setProperty("output_config", claude::object({ { "effort", juce::String(turnEffort_) } }));
        o->setProperty("cache_control", claude::object({ { "type", "ephemeral" } }));   // the conversation, automatic
        beta.clear();
        if (turnFast_)
        {
            // Fast mode goes without "fallbacks": the docs say nothing of the two together, and a fallback model need
            // not take fast mode.
            o->setProperty("speed", "fast");
            if (fastNeedsBeta(turnModel_))
                beta = kFastBeta;
        }
        else if (takesDefaultFallbacks(turnModel_))
        {
            o->setProperty("fallbacks", "default");
            beta = kFallbackBeta;
        }
        if (model != models_.end() && model->compaction)
        {
            juce::Array<juce::var> edits;
            edits.add(claude::object({ { "type", "compact_20260112" },
                                       { "trigger", claude::object({ { "type", "input_tokens" },
                                                                     { "value", kCompactTrigger } }) },
                                       { "instructions", kCompactInstructions } }));
            o->setProperty("context_management", claude::object({ { "edits", edits } }));
            beta += std::string(beta.empty() ? "" : ",") + kCompactBeta;
        }
        return body;
    }

    // ---- the turn -----------------------------------------------------------------------------------------------------
    bool ChatAgent::streamOnce(const std::string& body, const std::string& beta, claude::MessageBuilder& builder,
                               int& status, std::string& error, int& retryAfter)
    {
        std::string key;
        claude::Transport* transport = nullptr;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            loadKey();                             // a key saved again during the turn
            key = key_;
            transport = transport_.get();
            if (key.empty())
            {
                // The key removed while the turn ran: nothing to send it with.
                status = 401;
                retryAfter = -1;
                error = "The API key was removed. Add it again.";
                return false;
            }
            requests_.push_back(body);
            if (requests_.size() > 64)
                requests_.erase(requests_.begin());
        }
        claude::Headers headers = headersFor(key);
        headers.emplace_back("content-type", "application/json");
        if (!beta.empty())
            headers.emplace_back("anthropic-beta", beta);
        claude::SseParser sse;
        const claude::HttpResult r = transport->post(claude::kHost, "/v1/messages", body, headers,
                                                     [&](std::string_view chunk) {
                                                         sse.feed(chunk, [&](const std::string& ev, const std::string& data) {
                                                             builder.event(ev, data);
                                                         });
                                                         return !cancel_.load();
                                                     },
                                                     cancel_);
        status = r.status;
        retryAfter = r.retryAfterSeconds;
        if (r.status != 200)
        {
            error = r.status == 0 ? r.error : claude::apiError(r.status, r.body);
            return false;
        }
        error.clear();
        return true;
    }

    void ChatAgent::fetchModels()
    {
        std::string key;
        claude::Transport* transport = nullptr;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            key = key_;
            if (modelsFetched_ || key.empty())
                return;
            modelsFetched_ = true;
            transport = transport_.get();
        }
        const claude::HttpResult r = transport->get(claude::kHost, "/v1/models?limit=100", headersFor(key), cancel_);
        if (r.status == 401)
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            refused(key, r.status, claude::apiError(r.status, r.body));
            return;
        }
        if (r.status != 200)
            return;                                // the built-in list stays
        const juce::var v = claude::parseJson(r.body);
        std::vector<ChatModel> found;
        const juce::var models = v.getProperty("data", juce::var());
        if (const auto* data = models.getArray())
            for (const auto& m : *data)
            {
                const juce::var caps = m.getProperty("capabilities", juce::var());
                const bool adaptive = static_cast<bool>(caps.getProperty("thinking", juce::var())
                                                            .getProperty("types", juce::var())
                                                            .getProperty("adaptive", juce::var())
                                                            .getProperty("supported", false));
                if (!adaptive)
                    continue;                      // the requests use adaptive thinking
                ChatModel cm{ str(m, "id"), str(m, "display_name"), {} };
                cm.maxTokens = static_cast<int>(m.getProperty("max_tokens", kDefaultMaxTokens));
                if (cm.maxTokens <= 0)
                    cm.maxTokens = kDefaultMaxTokens;
                cm.compaction = static_cast<bool>(caps.getProperty("context_management", juce::var())
                                                      .getProperty("compact_20260112", juce::var())
                                                      .getProperty("supported", false));
                cm.fast = takesFast(cm.id);        // /v1/models has no capability for it
                const juce::var server = caps.getProperty("server_tools", juce::var());
                cm.web = server.isObject() ? static_cast<bool>(server.getProperty("web_search", juce::var())
                                                                   .getProperty("supported", false))
                                           : true;
                const juce::var effort = caps.getProperty("effort", juce::var());
                for (const std::string& e : kAllEfforts)
                    if (static_cast<bool>(effort.getProperty(juce::String(e), juce::var()).getProperty("supported", false)))
                        cm.efforts.push_back(e);
                if (cm.name.empty())
                    cm.name = cm.id;
                found.push_back(std::move(cm));
            }
        if (found.empty())
            return;
        const std::lock_guard<std::mutex> lock(mutex_);
        models_ = std::move(found);
        if (std::none_of(models_.begin(), models_.end(), [&](const ChatModel& m) { return m.id == model_; }))
        {
            const auto def = std::find_if(models_.begin(), models_.end(),
                                          [](const ChatModel& m) { return m.id == kDefaultModel; });
            model_ = def != models_.end() ? def->id : models_.front().id;
        }
        touch();
    }

    void ChatAgent::runTurn()
    {
        ensureKey();
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (key_.empty())
            {
                addEntry(ChatEntry::Kind::error, keyProblem_.empty()
                                                     ? "Add your Anthropic API key (KEY, above) to chat with Claude."
                                                     : "The saved API key cannot be used: " + keyProblem_);
                busy_ = false;
                activity_.clear();
                return;
            }
        }
        fetchModels();
        int maxTokens = 0;                         // what one reply of this turn's model holds
        {
            // The model and the thinking level for the whole turn: a change while it runs applies to the next one.
            const std::lock_guard<std::mutex> lock(mutex_);
            turnModel_ = model_;
            turnEffort_ = effort_;
            const auto model = std::find_if(models_.begin(), models_.end(),
                                            [&](const ChatModel& m) { return m.id == model_; });
            turnFast_ = fast_ && model != models_.end() && model->fast;
            turnWeb_ = web_ && model != models_.end() && model->web;
            if (turnWeb_ != historyWeb_)
            {
                dropThinking();                    // its thinking was made with the other tools
                historyWeb_ = turnWeb_;
            }
            maxTokens = maxTokensFor(turnModel_);
            plannerAsks_ = 0;
            turnPlanUsage_.clear();
        }
        bool plan = false;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            plan = std::exchange(planNext_, false) && !planner_.empty();
        }
        if (plan && !cancel_.load())
            planTurn();

        int cutOffs = 0;                           // replies cut off at max_tokens in a row, this turn
        int checkNudges = 0;                       // times this turn went on because a check failed
        bool worked = false;                       // a tool changed the program, the dials or the checks this turn
        // The web pages the turn's answers cite (url, title), in order: listed under its last answer.
        std::vector<std::pair<std::string, std::string>> cited;
        std::size_t turnStart = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            turnStart = entries_.size();
        }
        for (int round = 0; round < kMaxRounds && !cancel_.load(); ++round)
        {
            std::string beta;
            std::string body = claude::toJson(buildRequest(beta));
            int fastBusy = 0;                      // the fast requests the API turned away as busy, this round

            std::unique_ptr<claude::MessageBuilder> builder;
            std::shared_ptr<std::vector<std::pair<std::string, std::string>>> lastSources;
            bool ok = false;
            std::string error;
            for (int attempt = 0; attempt < kMaxAttempts && !cancel_.load(); ++attempt)
            {
                setActivity("Thinking");
                std::size_t shown = 0;                     // the entries before this attempt's
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    shown = entries_.size();
                }
                auto tool = std::make_shared<std::string>();   // the tool call streaming in
                claude::MessageBuilder::Callbacks cb;
                cb.text = [this](std::string_view t) {
                    {
                        const std::lock_guard<std::mutex> lock(mutex_);
                        appendToLast(ChatEntry::Kind::assistant, t);
                    }
                    setActivity("Writing");
                };
                cb.thinking = [this](std::string_view t) {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    appendToLast(ChatEntry::Kind::thinking, t);
                };
                cb.toolStart = [this, tool](const std::string& name) {
                    *tool = name;
                    setActivity(name == "write_file" || name == "edit_file" ? std::string("Writing code")
                                : name == "run_program"                     ? std::string("About to run the program")
                                : name == "measure_output"                  ? std::string("About to measure the sound")
                                : name == "view_panel"                      ? std::string("Looking at the panel")
                                : name == "view_sound"                      ? std::string("About to look at the sound")
                                : name == "set_checks"                      ? std::string("Writing checks")
                                : name == "tune_dials"                      ? std::string("About to tune the dials")
                                : name == "ask_planner"                     ? std::string("Writing to the planner")
                                : name == "ask_user"                        ? std::string("Writing a question for you")
                                                                            : "Using " + name);
                };
                cb.toolInput = [this, tool](const std::string& json) {
                    // A big file takes a while to arrive: its name and size so far.
                    if (*tool != "write_file" && *tool != "edit_file")
                        return;
                    const std::string file = partialString(json, "name");
                    const std::string kb = std::to_string((json.size() + 1023) / 1024) + " KB";
                    setActivity((*tool == "write_file" ? "Writing " : "Editing ") + (file.empty() ? std::string("code") : file)
                                + " (" + kb + ")");
                };
                cb.fallback = [this](const std::string& from, const std::string& to) {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    addEntry(ChatEntry::Kind::note, from + " declined this request; " + to + " carried on.");
                };
                cb.compaction = [this](std::size_t) {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    addEntry(ChatEntry::Kind::note, "The conversation got long, so its earlier part was summarized to "
                                                    "make room; Claude carries on from the summary.");
                };
                auto sources = std::make_shared<std::vector<std::pair<std::string, std::string>>>();
                cb.serverTool = [this](const std::string& name) {
                    setActivity(name == "web_search"  ? std::string("Searching the web")
                                : name == "web_fetch" ? std::string("Reading a page")
                                                      : std::string("Sifting what it found"));
                };
                cb.serverInput = [this](const std::string& name, const juce::var& input) {
                    const std::string line = name == "web_search" ? "Searched the web: " + str(input, "query")
                                           : name == "web_fetch"  ? "Read " + str(input, "url")
                                                                  : std::string();
                    if (line.empty())
                        return;
                    const std::lock_guard<std::mutex> lock(mutex_);
                    addEntry(ChatEntry::Kind::tool, line);
                };
                cb.serverResult = [this](const juce::var& block) {
                    const juce::var c = block.getProperty("content", juce::var());
                    if (!c.isObject() || str(c, "type").find("_error") == std::string::npos)
                        return;
                    const std::string what = str(block, "type").rfind("web_fetch", 0) == 0 ? "page" : "search";
                    const std::lock_guard<std::mutex> lock(mutex_);
                    addEntry(ChatEntry::Kind::tool, "The " + what + " failed with an error: " + str(c, "error_code"));
                };
                cb.citation = [sources](const juce::var& c) {
                    const std::string url = str(c, "url");
                    if (url.empty())
                        return;                    // a fetched document's passage: its page is listed as read
                    for (const auto& s : *sources)
                        if (s.first == url)
                            return;
                    sources->emplace_back(url, str(c, "title"));
                };
                builder = std::make_unique<claude::MessageBuilder>(std::move(cb));
                lastSources = sources;
                int status = 0, retryAfter = -1;
                ok = streamOnce(body, beta, *builder, status, error, retryAfter);
                if (cancel_.load())
                    break;
                // A stream that broke off (the connection dropped, or the API was overloaded mid-answer) is asked
                // again, as a failed request is: nothing of it has gone into the history yet.
                const std::string& et = builder->errorType();
                const bool brokeOff = ok && !builder->finished()
                                   && (builder->error().empty() || et == "overloaded_error" || et == "api_error");
                if (ok && !brokeOff)
                    break;
                // Fast mode turned away: busy again after one wait (its rate limit, or its capacity), or refused
                // (not open to this key, not on this model). The turn goes on at standard speed (the fast-mode docs'
                // fallback; the cache starts again, as speeds share no cached prefix), and a refusal turns it off.
                bool fastNow = false;
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    fastNow = turnFast_;
                }
                const bool fastRefused = fastNow && !ok && status >= 400 && status < 500 && status != 429
                                      && aboutSpeed(error);
                const bool fastBusyAgain = fastNow && !ok && (status == 429 || status == 529) && ++fastBusy >= 2;
                // The web tools turned away: called directly on a model that cannot run dynamic filtering, else
                // (web search off for the organisation) the turn goes on without them, and the switch is off.
                bool webNow = false;
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    webNow = turnWeb_;
                }
                if (webNow && !ok && status == 400 && (wantsDirect(error) || webDisabled(error)))
                {
                    {
                        const std::lock_guard<std::mutex> lock(mutex_);
                        if (!(wantsDirect(error) && webDirect_.insert(turnModel_).second))
                        {
                            turnWeb_ = false;
                            web_ = false;
                            chatstore::saveChoice({ model_, effort_, fast_, web_, planner_ });
                            dropThinking();
                            historyWeb_ = false;
                            addEntry(ChatEntry::Kind::note, "Web search is not open to this request (" + error
                                                                + "): it is off now, and this turn goes on without it.");
                        }
                    }
                    body = claude::toJson(buildRequest(beta));
                    --attempt;
                    continue;
                }
                if (fastRefused || fastBusyAgain)
                {
                    {
                        const std::lock_guard<std::mutex> lock(mutex_);
                        turnFast_ = false;
                        if (fastRefused)
                        {
                            fast_ = false;
                            chatstore::saveChoice({ model_, effort_, fast_, web_, planner_ });
                            addEntry(ChatEntry::Kind::note, "Fast mode is not open to this request (" + error
                                                                + "): it is off now, and this turn goes on at "
                                                                  "standard speed.");
                        }
                        else
                            addEntry(ChatEntry::Kind::note,
                                     "Fast mode is busy; this turn goes on at standard speed.");
                    }
                    body = claude::toJson(buildRequest(beta));
                    --attempt;                     // the standard request gets attempts of its own
                    continue;
                }
                const bool retryable = brokeOff || status == 0 || status == 429 || status == 529 || status >= 500;
                if (!retryable || attempt + 1 == kMaxAttempts)
                    break;
                if (brokeOff)
                {
                    // What the broken attempt showed goes; the next attempt streams the answer again.
                    const std::lock_guard<std::mutex> lock(mutex_);
                    if (entries_.size() > shown)
                        entries_.resize(shown);
                    addEntry(ChatEntry::Kind::note, "The answer broke off; asking again.");
                }
                const int wait = retryAfter > 0 ? std::min(retryAfter, 60) : (2 << attempt);
                for (int s = wait; s > 0 && !cancel_.load(); --s)
                {
                    setActivity("Retrying in " + std::to_string(s) + " s");
                    for (int k = 0; k < 10 && !cancel_.load(); ++k)
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }

            const bool stopped = cancel_.load();
            if (!ok && !stopped)
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                addEntry(ChatEntry::Kind::error, error.empty() ? "The request failed." : error);
                break;                             // the user's message stays last: the next one joins it
            }

            // The answer's blocks, as they go back. After a fallback block the blocks before it keep only their text
            // (the API's rule for echoing a fallback turn); the block itself is an audit marker and is dropped.
            juce::Array<juce::var> blocks;
            const juce::var content = builder != nullptr ? builder->content() : juce::var();
            if (const auto* arr = content.getArray())   // the var holds the array: never getArray() on a temporary
                blocks = *arr;
            int lastFallback = -1;
            for (int i = 0; i < blocks.size(); ++i)
                if (str(blocks[i], "type") == "fallback")
                    lastFallback = i;
            if (lastFallback >= 0)
            {
                juce::Array<juce::var> kept;
                for (int i = 0; i < blocks.size(); ++i)
                    if (i > lastFallback
                        || (i < lastFallback && (str(blocks[i], "type") == "text" || str(blocks[i], "type") == "compaction")))
                        kept.add(blocks[i]);
                blocks = kept;
            }
            const std::string stopReason = builder != nullptr ? builder->stopReason() : std::string();
            const bool complete = builder != nullptr && builder->finished() && builder->error().empty();

            if (builder != nullptr)
            {
                const claude::Usage& u = builder->usage();
                const std::lock_guard<std::mutex> lock(mutex_);
                usage_ = "Last reply: " + thousands(u.input + u.cacheRead + u.cacheWrite) + " tokens in ("
                       + thousands(u.cacheRead) + " cached), " + thousands(u.output) + " out"
                       + (u.speed == "fast" ? ", fast" : "")
                       + (u.webSearches > 0 ? ", " + std::to_string(u.webSearches) + (u.webSearches == 1 ? " search" : " searches")
                                            : std::string())
                       + (u.compactions > 0 ? "; the conversation was summarized" : "")
                       + (turnPlanUsage_.empty() ? std::string() : "; " + turnPlanUsage_);
                touch();
            }

            if (stopped || !complete || stopReason == "refusal")
            {
                // The turn ends here. The answer goes back as its text alone (an empty one as a short note), so the
                // history stays valid and the next message can follow it.
                juce::Array<juce::var> textOnly;
                bool hasText = false;
                for (const auto& b : blocks)
                {
                    const std::string type = str(b, "type");
                    if (type == "compaction")
                        textOnly.add(b);                   // a summary made in this request stays (no second one)
                    else if (type == "text" && !str(b, "text").empty())
                    {
                        // Its citations point at search results that do not go back: the text alone.
                        textOnly.add(claude::object({ { "type", "text" }, { "text", b.getProperty("text", "") } }));
                        hasText = true;
                    }
                }
                if (!hasText)
                    textOnly.add(text(stopped ? "(stopped by the user)" : "(no answer)"));
                const std::lock_guard<std::mutex> lock(mutex_);
                appendAnswer(textOnly);
                if (stopped)
                    addEntry(ChatEntry::Kind::note, "Stopped.");
                else if (stopReason == "refusal")
                    addEntry(ChatEntry::Kind::error, "Claude declined this request.");
                else
                    addEntry(ChatEntry::Kind::error, builder != nullptr && !builder->error().empty()
                                                         ? "The answer broke off: " + builder->error()
                                                         : "The connection ended before the answer did.");
                break;
            }

            if (lastSources != nullptr)
                for (const auto& s : *lastSources)
                    if (std::none_of(cited.begin(), cited.end(), [&](const auto& c) { return c.first == s.first; }))
                        cited.push_back(s);
            std::vector<juce::var> toolUses;
            for (const auto& b : blocks)
                if (str(b, "type") == "tool_use")
                    toolUses.push_back(b);
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                appendAnswer(blocks);
                touch();
            }
            // A reply cut off at max_tokens (its thinking counts toward it): the turn goes on, Claude told so, unless
            // the reply before it was cut off too, since a reply that only thinks costs a whole reply each time.
            const bool cutOff = stopReason == "max_tokens";
            cutOffs = cutOff ? cutOffs + 1 : 0;
            const bool endsHere = cutOff && cutOffs >= kMaxCutOffs;
            if (cutOff)
            {
                const std::string limit = "Claude's reply reached its length limit (" + thousands(maxTokens)
                                        + " tokens, its thinking included)";
                const std::lock_guard<std::mutex> lock(mutex_);
                addEntry(ChatEntry::Kind::note,
                         endsHere ? limit + " twice in a row, so the turn ends here. Send \"continue\" to let it go "
                                            "on. A lower thinking level leaves it more room to write: at Max its "
                                            "thinking has no limit of its own."
                                  : limit + ", so it goes on from where it stopped.");
            }
            if (stopReason == "pause_turn" && !cutOff)
                continue;                          // the API paused its own tools' loop: the reply goes back as it is
            // A reply that ends the turn while Claude's checks fail: the turn goes on with their results (ChatAgent.h).
            if (toolUses.empty() && !cutOff && stopReason == "end_turn" && worked && !cancel_.load()
                && checkNudges < kMaxCheckNudges)
            {
                bool checking = false;
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    checking = !checks_.empty();
                }
                if (checking)
                {
                    const std::string results = evaluateChecks();
                    bool failing = false;
                    {
                        const std::lock_guard<std::mutex> lock(mutex_);
                        failing = checksFailing_;
                    }
                    if (failing && !cancel_.load())
                    {
                        ++checkNudges;
                        const std::string line =
                            "(From the plugin: your checks, measured on the files and dials as they are now.\n" + results
                            + "Fix what fails, or change a check with set_checks if it asks for the wrong thing, or say why "
                              "it cannot pass. Your turn ends when you answer without a tool call"
                            + (checkNudges == kMaxCheckNudges ? std::string(", and the plugin will not ask again this turn.)")
                                                              : std::string(".)"));
                        const std::lock_guard<std::mutex> lock(mutex_);
                        messages_.add(claude::object({ { "role", "user" }, { "content", juce::Array<juce::var>{ text(line) } } }));
                        addEntry(ChatEntry::Kind::note, "A check fails, so Claude goes on (" + std::to_string(checkNudges) + " of "
                                                            + std::to_string(kMaxCheckNudges) + ").");
                        touch();
                        continue;
                    }
                }
            }
            if (toolUses.empty() && (!cutOff || endsHere))
                break;                             // end_turn, or cut off again

            // The tools, in order, their results in one user message; after a reply cut off without a tool call, a
            // line that says so. A question (ask_user) waits for the user: the turn ends with its entry open.
            juce::Array<juce::var> results;
            std::string askId;
            std::vector<ChatQuestion> askQuestions;
            for (const auto& use : toolUses)
            {
                const std::string id = str(use, "id");
                bool invalid = false;
                std::string raw;
                for (const auto& [badId, badJson] : builder->invalidToolInputs())
                    if (badId == id)
                    {
                        invalid = true;
                        raw = badJson;
                    }
                if (cutOff || invalid || cancel_.load())
                {
                    const std::string why = cutOff
                                                ? "Your reply reached its limit of about " + thousands(maxTokens)
                                                      + " tokens, your thinking included, and was cut off before this "
                                                        "call was complete, so it did not run. Do the work in smaller "
                                                        "steps (edit_file, or several files)."
                                            : invalid ? claude::toJson(claude::object({ { "INVALID_JSON",
                                                                                          juce::String::fromUTF8(raw.c_str()) } }))
                                                      : "Stopped by the user before this ran.";
                    results.add(claude::object({ { "type", "tool_result" },
                                                 { "tool_use_id", juce::String(id) },
                                                 { "is_error", true },
                                                 { "content", juce::String::fromUTF8(why.c_str()) } }));
                    continue;
                }
                if (str(use, "name") == "ask_user")
                {
                    // A question: its result is the user's answer, which ends the wait (answer(), or send()).
                    std::vector<ChatQuestion> qs;
                    std::string why;
                    if (!askId.empty())
                        why = "Only one ask_user call is taken in a reply: put every question (up to "
                            + std::to_string(kAskMaxQuestions) + ") in one call.";
                    else if (parseQuestions(use.getProperty("input", juce::var()), qs, why))
                    {
                        askId = id;
                        askQuestions = std::move(qs);
                        continue;
                    }
                    results.add(claude::object({ { "type", "tool_result" },
                                                 { "tool_use_id", juce::String(id) },
                                                 { "is_error", true },
                                                 { "content", juce::String::fromUTF8(why.c_str()) } }));
                    continue;
                }
                const juce::var result = runTool(use);
                if (!static_cast<bool>(result.getProperty("is_error", false)))
                    for (const char* c : kChangingTools)
                        worked = worked || str(use, "name") == c;
                results.add(result);
            }
            if (toolUses.empty())
                results.add(text(cutOffNote(maxTokens)));
            // A summary in this request left the files out of what Claude has: the state goes with the results, after
            // them, so that the turn goes on with the code in front of it.
            bool resend = false;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                resend = std::exchange(resendState_, false);
            }
            if (resend)
                if (const std::string context = contextBlock(); !context.empty())
                    results.add(text(context));
            if (!askId.empty())
            {
                // The other results wait in the history; the answer joins them (deliver).
                const std::lock_guard<std::mutex> lock(mutex_);
                if (!results.isEmpty())
                    messages_.add(claude::object({ { "role", "user" }, { "content", results } }));
                ChatEntry q;
                q.kind = ChatEntry::Kind::question;
                q.text = questionsText(askQuestions);
                q.questions = std::move(askQuestions);
                q.answers.assign(q.questions.size(), std::string());
                q.open = true;
                entries_.push_back(std::move(q));
                asking_ = askId;
                touch();
                break;
            }
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                messages_.add(claude::object({ { "role", "user" }, { "content", results } }));
                touch();
            }
            if (cancel_.load())
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                addEntry(ChatEntry::Kind::note, "Stopped.");
                break;
            }
            if (endsHere)
                break;                             // the results are in the history; the next message joins them
            if (round + 1 == kMaxRounds)
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                addEntry(ChatEntry::Kind::note, "Claude used this turn's " + std::to_string(kMaxRounds)
                                                    + " requests; send a message to let it carry on.");
            }
        }

        if (!cited.empty())
        {
            // The sources, under the turn's last answer (the web search docs: citations go with what is shown).
            std::string list = "**Sources**";
            for (std::size_t k = 0; k < cited.size(); ++k)
                list += "\n" + std::to_string(k + 1) + ". "
                      + (cited[k].second.empty() ? cited[k].first : cited[k].second + " -- " + cited[k].first);
            const std::lock_guard<std::mutex> lock(mutex_);
            bool placed = false;
            for (std::size_t i = entries_.size(); i-- > turnStart;)
                if (entries_[i].kind == ChatEntry::Kind::assistant)
                {
                    entries_[i].text += "\n\n" + list;
                    placed = true;
                    break;
                }
            if (!placed)
                addEntry(ChatEntry::Kind::note, list);
            touch();
        }
        const std::lock_guard<std::mutex> lock(mutex_);
        busy_ = false;
        activity_.clear();
        touch();
    }

    // ---- the planner (setPlanner) -------------------------------------------------------------------------------------
    std::string ChatAgent::consultPlanner(const juce::Array<juce::var>& history, const std::string& brief, bool answer,
                                          std::string& why)
    {
        std::string planner, name, effort;
        int maxTokens = 0;
        std::size_t at = 0;                        // its entry
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            planner = planner_;
            const auto m = std::find_if(models_.begin(), models_.end(), [&](const ChatModel& x) { return x.id == planner; });
            if (m == models_.end())
            {
                why = "the planning model (" + planner + ") is not one your key offers";
                return {};
            }
            name = m->name;
            const auto takes = [&](const std::string& e) { return std::find(m->efforts.begin(), m->efforts.end(), e) != m->efforts.end(); };
            effort = takes(turnEffort_) ? turnEffort_ : takes("high") ? std::string("high") : m->efforts.empty() ? std::string() : m->efforts.front();
            maxTokens = maxTokensFor(planner);
            ChatEntry e;
            e.kind = ChatEntry::Kind::plan;
            e.from = name;
            e.answer = answer;
            at = entries_.size();
            entries_.push_back(std::move(e));
            touch();
        }
        juce::Array<juce::var> messages = forAnotherModel(history);
        if (messages.isEmpty() || str(messages.getLast(), "role") != "user")
            messages.add(claude::object({ { "role", "user" }, { "content", juce::Array<juce::var>{ text(brief) } } }));
        else
        {
            const juce::var last = messages.getLast();
            juce::Array<juce::var> content;
            if (const auto* arr = last.getProperty("content", juce::var()).getArray())
                content = *arr;
            else
                content.add(text(last.getProperty("content", "").toString().toStdString()));
            content.add(text(brief));
            last.getDynamicObject()->setProperty("content", content);   // forAnotherModel's own object
        }
        juce::Array<juce::var> system;
        system.add(claude::object({ { "type", "text" },
                                    { "text", juce::String::fromUTF8(systemPrompt().c_str()) },
                                    { "cache_control", claude::object({ { "type", "ephemeral" }, { "ttl", "1h" } }) } }));
        juce::var body = claude::object({ { "model", juce::String(planner) },
                                          { "max_tokens", maxTokens },
                                          { "stream", true },
                                          { "system", system },
                                          { "tools", toolDefinitions() },
                                          { "tool_choice", claude::object({ { "type", "none" } }) },
                                          { "messages", messages },
                                          { "thinking", claude::object({ { "type", "adaptive" }, { "display", "summarized" } }) } });
        if (!effort.empty())
            body.getDynamicObject()->setProperty("output_config", claude::object({ { "effort", juce::String(effort) } }));
        body.getDynamicObject()->setProperty("cache_control", claude::object({ { "type", "ephemeral" } }));
        const std::string json = claude::toJson(body);

        std::string out;
        for (int attempt = 0; attempt < kMaxAttempts && !cancel_.load(); ++attempt)
        {
            setActivity(answer ? "Asking the planner (" + name + ")" : "Planning (" + name + ")");
            claude::MessageBuilder::Callbacks cb;
            cb.text = [this, at](std::string_view t) {
                const std::lock_guard<std::mutex> lock(mutex_);
                if (at < entries_.size())
                    entries_[at].text += std::string(t);
                touch();
            };
            cb.thinking = [this, name](std::string_view) { setActivity("Planning: " + name + " thinks"); };
            claude::MessageBuilder b(std::move(cb));
            int status = 0, retryAfter = -1;
            std::string error;
            const bool ok = streamOnce(json, std::string(), b, status, error, retryAfter);
            if (cancel_.load())
                break;
            const std::string& et = b.errorType();
            const bool brokeOff = ok && !b.finished() && (b.error().empty() || et == "overloaded_error" || et == "api_error");
            if (ok && !brokeOff)
            {
                const juce::var content = b.content();   // held: never getArray() on a temporary
                if (const auto* blocks = content.getArray())
                    for (const juce::var& x : *blocks)
                        if (str(x, "type") == "text")
                            out += str(x, "text");
                const claude::Usage& u = b.usage();
                const std::lock_guard<std::mutex> lock(mutex_);
                turnPlanUsage_ += (turnPlanUsage_.empty() ? "planner " : ", ") + thousands(u.input + u.cacheRead + u.cacheWrite)
                                + " in, " + thousands(u.output) + " out";
                if (b.stopReason() == "refusal")
                {
                    why = name + " declined to plan this";
                    out.clear();
                }
                break;
            }
            const bool retryable = brokeOff || status == 0 || status == 429 || status == 529 || status >= 500;
            if (!retryable || attempt + 1 == kMaxAttempts)
            {
                why = error.empty() ? std::string("the request failed") : error;
                break;
            }
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                if (at < entries_.size())
                    entries_[at].text.clear();     // what the broken attempt showed goes
                touch();
            }
            const int wait = retryAfter > 0 ? std::min(retryAfter, 60) : (2 << attempt);
            for (int s = wait; s > 0 && !cancel_.load(); --s)
            {
                setActivity("Retrying the planner in " + std::to_string(s) + " s");
                for (int k = 0; k < 10 && !cancel_.load(); ++k)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        if (cancel_.load() && why.empty())
            why = "stopped by the user";
        if (out.find_first_not_of(" \t\n") == std::string::npos)
        {
            out.clear();
            if (why.empty())
                why = "it wrote nothing";
            const std::lock_guard<std::mutex> lock(mutex_);
            if (at < entries_.size() && entries_[at].kind == ChatEntry::Kind::plan)
                entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(at));   // no empty card
            touch();
        }
        return out;
    }

    void ChatAgent::planTurn()
    {
        juce::Array<juce::var> history;
        std::string builder;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            history = messages_;
            const auto m = std::find_if(models_.begin(), models_.end(), [&](const ChatModel& x) { return x.id == turnModel_; });
            builder = m != models_.end() ? m->name : turnModel_;
        }
        std::string why;
        const std::string plan = consultPlanner(history, plannerBrief(builder), false, why);
        const std::lock_guard<std::mutex> lock(mutex_);
        std::string from = planner_;
        for (const ChatModel& m : models_)
            if (m.id == planner_)
                from = m.name;
        if (plan.empty())
        {
            if (!cancel_.load())
                addEntry(ChatEntry::Kind::note, "The planner did not plan this (" + why + "): " + builder
                                                    + " goes on without a plan.");
            return;
        }
        // The plan joins the user's message, which the builder has not seen yet: the history stays append-only.
        if (!messages_.isEmpty() && str(messages_.getLast(), "role") == "user")
            if (auto* o = messages_.getLast().getDynamicObject())
            {
                juce::Array<juce::var> content;
                if (const auto* arr = o->getProperty("content").getArray())
                    content = *arr;
                content.add(text("<plan from=\"" + from + "\">\n" + plan + "\n</plan>\n(From the plugin: the user chose " + from
                                 + " to plan and you to build. Build this plan; where the code or a measurement shows it "
                                   "is wrong, change it and say why. ask_planner asks the planner again.)"));
                o->setProperty("content", content);
            }
        touch();
    }

    // ---- the checks (set_checks) --------------------------------------------------------------------------------------
    std::map<std::string, float> ChatAgent::currentDials() const
    {
        std::map<std::string, float> out;
        const Controls controls = host_.chatDials();
        for (int c = 0; c < kMaxControls; ++c)
            if (controls[static_cast<std::size_t>(c)].used)
                out[controls[static_cast<std::size_t>(c)].name] = host_.chatDialValue(c);
        return out;
    }

    std::string ChatAgent::evaluateChecks()
    {
        std::vector<measure::Check> checks;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            checks = checks_;
        }
        if (checks.empty())
            return {};
        setActivity("Checking");
        std::string errors;
        const std::vector<measure::CheckResult> results = measure::evaluate(host_.chatDraft().files, checks, currentDials(), &errors);
        std::vector<ChatCheck> rows;
        int pass = 0;
        std::string lines;
        for (std::size_t i = 0; i < results.size() && i < checks.size(); ++i)
        {
            const measure::CheckResult& r = results[i];
            rows.push_back({ checks[i].label, r.text, r.measured, r.pass });
            pass += r.measured && r.pass ? 1 : 0;
            lines += std::string(!r.measured ? "- NOT MEASURED " : r.pass ? "- pass " : "- FAIL ") + checks[i].label + " ("
                   + checks[i].goal.metric + "): " + r.text + "\n";
        }
        const std::string head = "Checks: " + std::to_string(pass) + " of " + std::to_string(results.size()) + " pass.";
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            checksFailing_ = pass < static_cast<int>(results.size());
            // Their entry: the one in this turn (since the user's last message) brought up to date, else a new one.
            std::size_t at = entries_.size();
            for (std::size_t i = entries_.size(); i-- > 0;)
            {
                if (entries_[i].kind == ChatEntry::Kind::checks)
                {
                    at = i;
                    break;
                }
                if (entries_[i].kind == ChatEntry::Kind::user)
                    break;
            }
            if (at == entries_.size())
            {
                ChatEntry e;
                e.kind = ChatEntry::Kind::checks;
                entries_.push_back(std::move(e));
            }
            entries_[at].text = head + "\n" + lines;
            entries_[at].checks = std::move(rows);
            touch();
        }
        return head + "\n" + lines + (errors.empty() ? "" : "The program does not compile:\n" + capped(errors));
    }

    void ChatAgent::clearChecks()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (checks_.empty())
            return;
        checks_.clear();
        checksFailing_ = false;
        addEntry(ChatEntry::Kind::note, "The checks are cleared: the plugin no longer measures them.");
        touch();
    }

    // ---- MCP ------------------------------------------------------------------------------------------------------------
    namespace
    {
        // What Claude Desktop shows of each tool (its permission prompts, its tool list): a title in words, whether it
        // only looks, whether it takes something away; and the guide's part that gets what its MCP description leaves
        // out (mcp/McpGuide.h; "start" unless said). A new tool gets a line here.
        struct Face
        {
            const char* name;
            const char* title;
            bool readOnly, destructive;
            const char* part = "start";
        };
        constexpr Face kFaces[] = {
            { "read_file", "Read a file", true, false },          { "write_file", "Write a file", false, false },
            { "edit_file", "Edit a file", false, false },         { "rename_file", "Rename a file", false, false },
            { "delete_file", "Delete a file", false, true },      { "add_library_file", "Add a building block", false, false, "library" },
            { "run_program", "Run the program", false, false },   { "set_dial", "Turn a dial", false, false },
            { "measure_output", "Measure the sound", true, false }, { "view_panel", "Look at the panel", true, false, "interface" },
            { "view_sound", "Look at the sound", true, false },   { "set_checks", "Set checks on the sound", false, false },
            { "tune_dials", "Tune the dials", false, false },     { "get_status", "Check the plugin", true, false },
        };

        const Face* faceOf(const juce::String& name)
        {
            for (const Face& f : kFaces)
                if (name == f.name)
                    return &f;
            return nullptr;
        }

        // The tools the MCP link runs, as the plugin's chat has them (toolDefinitions): all but the chat's own.
        bool mcpOffers(const juce::String& name) { return name != "ask_user" && name != "ask_planner"; }

        // A tool's description for MCP and what it leaves out: its first paragraph and the rest; one paragraph, its first
        // sentence and the rest. (tools.json goes into every one of Claude Desktop's conversations, the guide only into
        // those that work in the plugin.)
        std::pair<std::string, std::string> leadOf(const std::string& d)
        {
            std::size_t cut = d.find("\n\n");
            std::size_t next = cut == std::string::npos ? std::string::npos : cut + 2;
            if (cut == std::string::npos)
                for (std::size_t p = d.find(". "); p != std::string::npos; p = d.find(". ", p + 1))
                    if (p + 2 < d.size() && std::isupper(static_cast<unsigned char>(d[p + 2])) != 0)
                    {
                        cut = p + 1;
                        next = p + 2;
                        break;
                    }
            if (cut == std::string::npos)
                return { d, {} };
            return { d.substr(0, cut), d.substr(next) };
        }

        std::vector<uint8_t> fromBase64(const juce::String& b64)
        {
            juce::MemoryOutputStream out;
            if (!juce::Base64::convertFromBase64(out, b64))
                return {};
            const auto* p = static_cast<const uint8_t*>(out.getData());
            return { p, p + out.getDataSize() };
        }
    }

    juce::var ChatAgent::mcpTools()
    {
        juce::Array<juce::var> out;
        const juce::var all = toolDefinitions();         // held: getArray() points into it
        if (const juce::Array<juce::var>* tools = all.getArray())
            for (const juce::var& t : *tools)
            {
                const juce::String name = t["name"].toString();
                if (!mcpOffers(name))
                    continue;
                const Face* f = faceOf(name);
                const juce::String title = f != nullptr ? juce::String(f->title) : name;
                const juce::var schema = t["input_schema"];
                if (name == "view_panel" || name == "view_sound")
                    if (juce::DynamicObject* props = schema["properties"].getDynamicObject())
                        props->setProperty("detail", claude::object({ { "type", "string" },
                                                                      { "enum", juce::Array<juce::var>{ "high" } },
                                                                      { "description", "high: the picture at its full size, "
                                                                                       "which may not fit (by default it is "
                                                                                       "made smaller to fit)" } }));
                out.add(claude::object({ { "name", name },
                                         { "title", title },
                                         { "description", juce::String::fromUTF8(leadOf(t["description"].toString().toStdString()).first.c_str()) },
                                         { "inputSchema", schema },
                                         { "annotations", claude::object({ { "title", title },
                                                                           { "readOnlyHint", f != nullptr && f->readOnly },
                                                                           { "destructiveHint", f != nullptr && f->destructive },
                                                                           { "openWorldHint", false } }) } }));
            }
        return out;
    }

    std::string ChatAgent::mcpGuide()
    {
        std::string notes;
        const juce::var all = toolDefinitions();
        if (const juce::Array<juce::var>* tools = all.getArray())
            for (const juce::var& t : *tools)
            {
                const juce::String name = t["name"].toString();
                const std::string rest = leadOf(t["description"].toString().toStdString()).second;
                if (!mcpOffers(name) || rest.empty())
                    continue;
                const Face* f = faceOf(name);
                notes += "<tool name=\"" + name.toStdString() + "\" part=\"" + (f != nullptr ? f->part : "start") + "\">" + rest + "</tool>\n";
            }
        return std::string(mcp::guide::kOutside) + "\n\n" + "You are driving " + product::kName + " (an " + (kInstrument ? "instrument" : "effect")
             + " plugin) through MCP, from Claude Desktop, as its own chat would: the tools are the chat's, but ask_user and "
               "ask_planner (ask the user here instead) and the web tools (use your own). The user hears the plugin in "
               "their DAW (or its Standalone app) and sees what you do in its editor; its chat's card notes each tool you "
               "use. The plugin does not send you its state with each message as it does its chat (the <plugin_state> "
               "below): call get_status for the files, the dials and the last run, and read_file before editing a file. "
               "With more than one instance of the plugin open, every tool takes `instance` (list_instances names them). "
               "This guide comes in parts (playground_guide's part): \"start\" is what every piece of work needs, and its end "
               "names the others and when to read them; a section it calls \"below\" that is not in the part you read is in "
               "another part. The tools' descriptions here are short: what they leave out is in " + mcp::guide::kNotes
             + ", at the end of the part that needs it. What follows is the guide the plugin's chat works from.\n\n"
             + systemPrompt() + "\n" + mcp::guide::kNotes + "\n\n" + notes;
    }

    std::string ChatAgent::mcpGuidePart(const std::string& part, bool& found)
    {
        return mcp::guide::part(mcpGuide(), part,
                                [](const std::string& file) {
                                    const LibraryFile* f = findLibraryFile(file);
                                    return f != nullptr ? std::string(f->source) : std::string();
                                },
                                found);
    }

    bool ChatAgent::mcpLive(const std::string& name, const juce::var& arguments)
    {
        const juce::String action = arguments.getProperty("action", juce::var()).toString();
        return name == "get_status"                                              // ChatHost's locked getters, atomics
            || name == "listen"                                                  // the live state (wave 1, lane "live")
            || (name == "capture" && (action == "status" || action == "stop"));  // the recorder (wave 1, lane "capture")
    }

    void ChatAgent::mcpCancel(uint64_t call)
    {
        if (call == 0)
            return;
        mcpCancelled_.store(call);
        const std::lock_guard<std::mutex> lock(mutex_);
        if (mcpRunning_ == call)
        {
            cancel_.store(true);
            activity_ = "Stopping";
            touch();
        }
    }

    ChatAgent::McpResult ChatAgent::mcpCall(const std::string& name, const juce::var& arguments, uint64_t call)
    {
        McpResult r;
        const auto words = [](const std::string& t) {
            juce::Array<juce::var> a;
            a.add(claude::object({ { "type", "text" }, { "text", juce::String::fromUTF8(t.c_str()) } }));
            return juce::var(a);
        };
        bool known = false;
        const juce::var mine = mcpTools();
        if (const juce::Array<juce::var>* tools = mine.getArray())
            for (const juce::var& t : *tools)
                known = known || t["name"].toString().toStdString() == name;
        if (!known)
        {
            r.content = words("There is no tool " + name + " here (ask_user and ask_planner are the plugin chat's own).");
            r.isError = true;
            return r;
        }
        // A copy: `detail` is MCP's own (the pictures' size), not the tool's.
        juce::var input = arguments.isObject() ? juce::JSON::parse(juce::JSON::toString(arguments)) : juce::var(new juce::DynamicObject());
        const bool whole = input.getProperty("detail", juce::var()).toString() == "high";
        if (name == "view_panel" || name == "view_sound")
            input.getDynamicObject()->removeProperty("detail");
        const bool live = mcpLive(name, input);
        if (!live)
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (busy_)
            {
                r.content = words("The plugin's own chat is working on a request: wait for it to finish (or stop it in the "
                                  "plugin), then call again.");
                r.isError = true;
                return r;
            }
            busy_ = true;
            mcpRunning_ = call;
            cancel_.store(call != 0 && mcpCancelled_.load() == call);   // a cancel that came before it started
            if (!mcpNoted_)
            {
                addEntry(ChatEntry::Kind::note, "Claude Desktop is working on the program through MCP: what it does shows here.");
                mcpNoted_ = true;
            }
            activity_ = "Claude Desktop is at work";
            touch();
        }
        const juce::var block = runTool(claude::object({ { "type", "tool_use" }, { "id", "mcp" }, { "name", juce::String(name) }, { "input", input } }));
        if (!live)
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            busy_ = false;
            mcpRunning_ = 0;
            activity_.clear();
            touch();
        }
        const juce::var content = block.getProperty("content", juce::var());
        juce::Array<juce::var> out;
        if (content.isString())
            out.add(claude::object({ { "type", "text" }, { "text", content } }));
        else if (const juce::Array<juce::var>* parts = content.getArray())
            for (const juce::var& b : *parts)
            {
                const juce::String type = b["type"].toString();
                if (type == "image")
                {
                    // At most kMcpPictureBytes, unless asked whole: Claude Desktop caps a tool's result.
                    const juce::String data = b["source"]["data"].toString();
                    if (whole || static_cast<std::size_t>(data.length()) <= kMcpPictureBytes / 3 * 4)
                    {
                        out.add(claude::object({ { "type", "image" }, { "data", data }, { "mimeType", b["source"]["media_type"] } }));
                        if (whole)
                            out.add(text("(The picture is sent whole, as asked: " + thousands(data.length()) + " characters.)"));
                        continue;
                    }
                    const chatimages::Fitted f = chatimages::fit(fromBase64(data), kMcpPictureBytes);
                    out.add(claude::object({ { "type", "image" },
                                             { "data", juce::Base64::toBase64(f.data.data(), f.data.size()) },
                                             { "mimeType", juce::String(f.mediaType) } }));
                    out.add(text("(The picture is sent smaller, to fit a tool's result: "
                                 + std::string(f.mediaType == "image/png" ? "a PNG" : "a JPEG") + " of " + std::to_string(f.width) + " x "
                                 + std::to_string(f.height) + ", from " + std::to_string(f.fromWidth) + " x "
                                 + std::to_string(f.fromHeight) + ". detail \"high\" sends it whole.)"));
                }
                else if (type == "text")
                    out.add(claude::object({ { "type", "text" }, { "text", b["text"] } }));
            }
        if (out.isEmpty())
            out.add(claude::object({ { "type", "text" }, { "text", "(no result)" } }));
        r.content = out;
        r.isError = static_cast<bool>(block.getProperty("is_error", false));
        return r;
    }

    // ---- the tools ----------------------------------------------------------------------------------------------------
    juce::var ChatAgent::runTool(const juce::var& use)
    {
        const std::string name = str(use, "name");
        const juce::var input = use.getProperty("input", juce::var());
        if (name == "view_panel")
        {
            // A picture of the panel: an image block and a line of what it shows (the tool_result's content).
            setActivity("Looking at the panel");
            const std::string view = str(input, "view");
            const PanelPicture pic = host_.chatPicture(view == "window", str(input, "page"));
            juce::var block = claude::object({ { "type", "tool_result" }, { "tool_use_id", use.getProperty("id", "") } });
            if (pic.png.empty())
            {
                block.getDynamicObject()->setProperty("content", juce::String::fromUTF8(("No picture: " + pic.why).c_str()));
                block.getDynamicObject()->setProperty("is_error", true);
                return block;
            }
            juce::Array<juce::var> content;
            content.add(claude::object({ { "type", "image" },
                                         { "source", claude::object({ { "type", "base64" },
                                                                      { "media_type", "image/png" },
                                                                      { "data", juce::Base64::toBase64(pic.png.data(), pic.png.size()) } }) } }));
            content.add(claude::object({ { "type", "text" }, { "text", juce::String::fromUTF8(pic.about.c_str()) } }));
            block.getDynamicObject()->setProperty("content", content);
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                addEntry(ChatEntry::Kind::tool, view == "window" ? "Looked at the editor" : "Looked at the panel");
            }
            return block;
        }
        if (name == "view_sound")
        {
            // A picture of the sound: an image block and what it shows; the chat shows the picture too.
            setActivity("Drawing the sound");
            measure::ViewRequest vr = soundpicture::request();
            vr.render = renderOf(input);
            vr.render.current = currentDials();
            if constexpr (!kInstrument)
            {
                const std::string in = str(input, "input");
                vr.input = in == "noise"     ? measure::ViewInput::noise
                         : in == "impulse"   ? measure::ViewInput::impulse
                         : in == "sine"      ? measure::ViewInput::sine
                         : in == "silence"   ? measure::ViewInput::silence
                                             : measure::ViewInput::sweep;
                const juce::var f = input.getProperty("frequency", juce::var());
                if (isNumber(f))
                    vr.frequency = static_cast<float>(static_cast<double>(f));
            }
            const juce::var from = input.getProperty("from_seconds", juce::var()), to = input.getProperty("to_seconds", juce::var());
            if (isNumber(from) && isNumber(to))
            {
                vr.from = static_cast<float>(static_cast<double>(from));
                vr.to = static_cast<float>(static_cast<double>(to));
            }
            const measure::View v = measure::view(host_.chatDraft().files, vr);
            juce::var block = claude::object({ { "type", "tool_result" }, { "tool_use_id", use.getProperty("id", "") } });
            if (!v.compiled)
            {
                block.getDynamicObject()->setProperty(
                    "content", juce::String::fromUTF8(("It did not compile, so nothing was drawn:\n" + capped(v.errors)).c_str()));
                block.getDynamicObject()->setProperty("is_error", true);
                const std::lock_guard<std::mutex> lock(mutex_);
                addEntry(ChatEntry::Kind::tool, "looked at the sound: it did not compile");
                return block;
            }
            setActivity("Looking at the sound");
            const soundpicture::Drawn d = soundpicture::draw(v);
            juce::Array<juce::var> content;
            content.add(claude::object({ { "type", "image" },
                                         { "source", claude::object({ { "type", "base64" },
                                                                      { "media_type", "image/png" },
                                                                      { "data", juce::Base64::toBase64(d.png.data(), d.png.size()) } }) } }));
            content.add(claude::object({ { "type", "text" }, { "text", juce::String::fromUTF8(d.about.c_str()) } }));
            block.getDynamicObject()->setProperty("content", content);
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                addEntry(ChatEntry::Kind::tool, "looked at the sound");
                entries_.back().pictures.push_back(d.thumb);
                touch();
            }
            return block;
        }
        bool isError = false;
        const std::string result = toolText(name, input, isError);
        juce::var block = claude::object({ { "type", "tool_result" },
                                           { "tool_use_id", use.getProperty("id", "") },
                                           { "content", juce::String::fromUTF8(result.c_str()) } });
        if (isError)
            block.getDynamicObject()->setProperty("is_error", true);
        return block;
    }

    std::string ChatAgent::toolText(const std::string& name, const juce::var& input, bool& isError)
    {
        const auto fail = [&](std::string why) {
            isError = true;
            return why;
        };
        const auto note = [this](std::string t) {
            const std::lock_guard<std::mutex> lock(mutex_);
            addEntry(ChatEntry::Kind::tool, std::move(t));
        };
        const auto stringArg = [&](const char* k, std::string& out) {
            const juce::var v = input.getProperty(k, juce::var());
            if (!v.isString())
                return false;
            out = v.toString().toStdString();
            return true;
        };
        const auto findFile = [](const Draft& d, const std::string& n) {
            for (std::size_t i = 0; i < d.files.size(); ++i)
                if (d.files[i].name == n)
                    return static_cast<int>(i);
            return -1;
        };
        const auto names = [](const Draft& d) {
            std::string out;
            for (const SourceFile& f : d.files)
                out += (out.empty() ? "" : ", ") + f.name;
            return out;
        };

        if (name == "read_file")
        {
            std::string file;
            if (!stringArg("name", file))
                return fail("read_file needs a name");
            const Draft d = host_.chatDraft();
            const int i = findFile(d, file);
            if (i < 0)
                return fail("There is no file " + file + ". The files: " + names(d));
            setActivity("Reading " + file);
            const std::string& t = d.files[static_cast<std::size_t>(i)].text;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                markSeen(file, t);
            }
            return t;
        }
        if (name == "write_file")
        {
            std::string file, content;
            if (!stringArg("name", file) || !stringArg("content", content))
                return fail("write_file needs a name and the content");
            if (!validFileName(file))
                return fail("'" + file + "' cannot name a file: use letters, digits, _, - and ., not starting with "
                            "'.', at most " + std::to_string(kMaxNameLength) + " characters");
            Draft d = host_.chatDraft();
            int i = findFile(d, file);
            if (i < 0)
            {
                if (d.files.size() >= kMaxFiles)
                    return fail("The editor holds at most " + std::to_string(kMaxFiles) + " files; delete one first");
                d.files.push_back({ file, content });
                i = static_cast<int>(d.files.size()) - 1;
            }
            else
            {
                d.files[static_cast<std::size_t>(i)].text = content;
            }
            d.active = i;
            setActivity("Writing " + file);
            host_.chatSetDraft(d);
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                markSeen(file, content);
            }
            const int lines = lineCount(content);
            const std::string count = std::to_string(lines) + (lines == 1 ? " line" : " lines");
            note("wrote " + file + " (" + count + ")");
            std::string out = "Wrote " + file + " (" + count + ").";
            bool included = i == 0;
            for (const SourceFile& f : d.files)
                if (f.text.find("#include \"" + file + "\"") != std::string::npos)
                    included = true;
            if (!included)
                out += " No file #includes it yet.";
            return out;
        }
        if (name == "edit_file")
        {
            std::string file, oldText, newText;
            if (!stringArg("name", file) || !stringArg("old_text", oldText) || !stringArg("new_text", newText))
                return fail("edit_file needs name, old_text and new_text");
            Draft d = host_.chatDraft();
            const int i = findFile(d, file);
            if (i < 0)
                return fail("There is no file " + file + ". The files: " + names(d));
            if (oldText.empty())
                return fail("old_text is empty: give the exact passage to replace");
            std::string& t = d.files[static_cast<std::size_t>(i)].text;
            std::size_t count = 0, at = std::string::npos;
            for (std::size_t p = t.find(oldText); p != std::string::npos; p = t.find(oldText, p + 1))
            {
                if (count == 0)
                    at = p;
                ++count;
            }
            if (count == 0)
                return fail("old_text does not occur in " + file + " (check whitespace and the latest text: "
                            "read_file shows it)");
            if (count > 1)
                return fail("old_text occurs " + std::to_string(count) + " times in " + file
                            + ": include more of the surrounding lines so it occurs once");
            t.replace(at, oldText.size(), newText);
            d.active = i;
            setActivity("Editing " + file);
            host_.chatSetDraft(d);
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                markSeen(file, t);
            }
            note("edited " + file);
            return "Edited " + file + ".";
        }
        if (name == "rename_file")
        {
            std::string file, to;
            if (!stringArg("name", file) || !stringArg("new_name", to))
                return fail("rename_file needs name and new_name");
            Draft d = host_.chatDraft();
            const int i = findFile(d, file);
            if (i < 0)
                return fail("There is no file " + file + ". The files: " + names(d));
            if (!validFileName(to))
                return fail("'" + to + "' cannot name a file");
            if (findFile(d, to) >= 0)
                return fail("There is a file " + to + " already");
            d.files[static_cast<std::size_t>(i)].name = to;
            host_.chatSetDraft(d);
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                if (const auto it = seen_.find(file); it != seen_.end())
                {
                    seen_[to] = it->second;
                    seen_.erase(file);
                }
            }
            note("renamed " + file + " to " + to);
            return "Renamed " + file + " to " + to + ". #include lines naming " + file + " still name it.";
        }
        if (name == "delete_file")
        {
            std::string file;
            if (!stringArg("name", file))
                return fail("delete_file needs a name");
            Draft d = host_.chatDraft();
            const int i = findFile(d, file);
            if (i < 0)
                return fail("There is no file " + file);
            if (i == 0)
                return fail("The first file is the program and cannot be deleted");
            d.files.erase(d.files.begin() + i);
            d.active = std::min(d.active, static_cast<int>(d.files.size()) - 1);
            host_.chatSetDraft(d);
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                seen_.erase(file);
            }
            note("deleted " + file);
            return "Deleted " + file + ".";
        }
        if (name == "add_library_file")
        {
            std::string file;
            if (!stringArg("name", file))
                return fail("add_library_file needs a name");
            const LibraryFile* lib = findLibraryFile(file);
            if (lib == nullptr)
            {
                std::string have;
                for (const LibraryFile& f : kLibrary)
                    have += (have.empty() ? "" : ", ") + std::string(f.name);
                return fail("The library has no file " + file + ". Its files: " + have);
            }
            const std::string text = lib->source;
            const int lines = lineCount(text);
            const std::string count = std::to_string(lines) + (lines == 1 ? " line" : " lines");
            Draft d = host_.chatDraft();
            if (const int i = findFile(d, file); i >= 0)
            {
                if (d.files[static_cast<std::size_t>(i)].text != text)
                    return "The editor has a file " + file + " already, changed from the library's; it is left as it "
                           "is (read_file shows it, write_file replaces it).";
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    markSeen(file, text);
                }
                return "The editor has " + file + " already, as the library has it (" + count + "):\n" + text;
            }
            if (d.files.size() >= kMaxFiles)
                return fail("The editor holds at most " + std::to_string(kMaxFiles) + " files; delete one first");
            setActivity("Adding " + file);
            d.files.push_back({ file, text });
            d.active = static_cast<int>(d.files.size()) - 1;
            host_.chatSetDraft(d);
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                markSeen(file, text);
            }
            note("added " + file + " from the library");
            return "Added " + file + " (" + count + "). #include \"" + file + "\" in the program to use it. Its text:\n"
                   + text;
        }
        if (name == "run_program")
        {
            setActivity("Compiling");
            const Draft d = host_.chatDraft();
            const CompileStatus s = host_.chatRun(d.files);
            if (s.ok)
            {
                int dials = 0;
                for (const Control& c : s.controls)
                    dials += c.used ? 1 : 0;
                note("ran it: compiled in " + number(std::round(s.ms)) + " ms, " + std::to_string(dials) + " dial"
                     + (dials == 1 ? "" : "s"));
                const std::string checks = evaluateChecks();
                return "Compiled in " + number(std::round(s.ms)) + " ms; it plays now.\nIts dials:\n"
                     + dialLines(s.controls, host_) + panelLine(s.panel, s.controls)
                     + (s.notes.empty() ? "" : "Notes:\n" + capped(s.notes)) + (checks.empty() ? "" : "\n" + checks);
            }
            int errors = 0;
            for (std::size_t p = s.errors.find(": error:"); p != std::string::npos; p = s.errors.find(": error:", p + 1))
                ++errors;
            note("ran it: " + std::to_string(std::max(errors, 1)) + " error" + (errors == 1 ? "" : "s"));
            isError = true;
            return "It did not compile; the program that played before plays on.\n" + capped(s.errors + s.notes);
        }
        if (name == "set_dial")
        {
            std::string dial;
            const juce::var value = input.getProperty("value", juce::var());
            if (!stringArg("name", dial) || !(value.isDouble() || value.isInt() || value.isInt64() || value.isBool()))
                return fail("set_dial needs a name and a number");
            const Controls controls = host_.chatDials();
            for (int c = 0; c < kMaxControls; ++c)
            {
                const Control& k = controls[static_cast<std::size_t>(c)];
                if (!k.used || !juce::String(k.name).equalsIgnoreCase(juce::String(dial)))
                    continue;
                float v = std::clamp(static_cast<float>(static_cast<double>(value)), k.min, k.max);
                if (k.type != ControlType::real)
                    v = std::round(v);
                host_.chatSetDial(c, v);
                note("set " + std::string(k.name) + " to " + number(v));
                return std::string(k.name) + " is set to " + number(v) + " (its range " + number(k.min) + " to "
                     + number(k.max) + ").";
            }
            std::string have;
            for (const Control& k : controls)
                if (k.used)
                    have += (have.empty() ? "" : ", ") + std::string(k.name);
            return fail("The running program has no dial " + dial + ". Its dials: " + (have.empty() ? "none" : have));
        }
        if (name == "measure_output")
        {
            setActivity("Measuring the sound");
            measure::Request req = renderOf(input);
            req.current = currentDials();
            const std::string report = measure::run(host_.chatDraft().files, req);
            note(report.rfind("It did not compile", 0) == 0 ? "measured the sound: it did not compile"
                                                             : "measured the sound");
            return report;
        }
        if (name == "set_checks")
        {
            const juce::var list = input.getProperty("checks", juce::var());
            const auto* arr = list.getArray();
            if (arr == nullptr)
                return fail("set_checks needs checks: a list of them (empty to clear them)");
            if (arr->size() > kMaxChecks)
                return fail("At most " + std::to_string(kMaxChecks) + " checks");
            std::vector<measure::Check> checks;
            for (int i = 0; i < arr->size(); ++i)
            {
                const juce::var& c = (*arr)[i];
                measure::Check k;
                std::string why;
                if (!goalOf(c, k.goal, why))
                    return fail("Check " + std::to_string(i + 1) + ": " + why + " Nothing was set.");
                k.label = str(c, "label");
                if (k.label.empty())
                    k.label = k.goal.metric;
                k.request = renderOf(c);
                checks.push_back(std::move(k));
            }
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                checks_ = checks;
            }
            if (checks.empty())
            {
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    checksFailing_ = false;
                }
                note("cleared the checks");
                return "The checks are cleared: nothing is measured after a run.";
            }
            note("set " + std::to_string(checks.size()) + " check" + (checks.size() == 1 ? "" : "s"));
            return "Set " + std::to_string(checks.size()) + " check" + (checks.size() == 1 ? "" : "s")
                 + ". The plugin measures them after every run_program and when you end your turn.\n" + evaluateChecks();
        }
        if (name == "tune_dials")
        {
            measure::TuneRequest q;
            q.render = renderOf(input);
            q.render.current = currentDials();
            if (const auto* tune = input.getProperty("tune", juce::var()).getArray())
                for (const auto& t : *tune)
                    if (t.isString())
                        q.dials.push_back(t.toString().toStdString());
            if (const auto* goals = input.getProperty("goals", juce::var()).getArray())
                for (int i = 0; i < goals->size(); ++i)
                {
                    measure::Goal g;
                    std::string why;
                    if (!goalOf((*goals)[i], g, why))
                        return fail("Goal " + std::to_string(i + 1) + ": " + why);
                    q.goals.push_back(std::move(g));
                }
            const juce::var budget = input.getProperty("budget", juce::var());
            if (isNumber(budget))
                q.budget = std::clamp(static_cast<int>(std::lround(static_cast<double>(budget))), 1, measure::kMaxBudget);
            const juce::var applyVar = input.getProperty("apply", juce::var());
            const bool apply = applyVar.isBool() ? static_cast<bool>(applyVar) : true;
            const juce::var seconds = input.getProperty("seconds", juce::var());
            q.seconds = isNumber(seconds) ? std::clamp(static_cast<double>(seconds), 1.0, measure::kMaxSeconds) : measure::kDefaultSeconds;
            setActivity("Tuning the dials");
            q.progress = [this](int done, int all) {
                setActivity("Tuning: " + std::to_string(done) + " of " + std::to_string(all) + " renders");
                return !cancel_.load();
            };
            const measure::TuneResult t = measure::tune(host_.chatDraft().files, q);
            if (!t.error.empty())
            {
                note("tuning failed");
                return fail(t.error);
            }
            std::string out = t.text;
            std::string tuned;
            for (const auto& [dial, v] : t.best)
                tuned += (tuned.empty() ? "" : ", ") + dial;
            const int met = static_cast<int>(std::count(t.bestMet.begin(), t.bestMet.end(), true));
            bool dropped = false;                        // Claude Desktop cancelled the call (mcpCancel)
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                dropped = mcpRunning_ != 0 && mcpCancelled_.load() == mcpRunning_;
            }
            if (dropped)
                out += "Claude Desktop stopped this call, so the dials are left exactly as they were (set_dial sets the "
                       "values above).";
            else if (apply && t.bestScore < t.startScore)
            {
                const Controls controls = host_.chatDials();
                for (const auto& [dial, v] : t.best)
                    for (int c = 0; c < kMaxControls; ++c)
                        if (controls[static_cast<std::size_t>(c)].used && dial == controls[static_cast<std::size_t>(c)].name)
                            host_.chatSetDial(c, v);
                out += t.timedOut ? "The dials are set to the best values found before the time limit (the user can move them)."
                                  : "The dials are set to the best values (the user can move them).";
            }
            else if (apply)
                out += "The dials are left as they were: nothing better was found.";
            else
                out += "The dials are left as they were (apply was false): set_dial sets them.";
            note("tuned " + tuned + ": " + std::to_string(met) + " of " + std::to_string(t.bestMet.size()) + " goals met in "
                 + std::to_string(t.renders) + " renders" + (dropped ? ", stopped by Claude Desktop (the dials kept)"
                                                             : t.timedOut ? ", stopped at the time limit" : ""));
            return out;
        }
        if (name == "ask_planner")
        {
            std::string question, context;
            stringArg("question", question);
            stringArg("context", context);
            if (question.empty())
                return fail("ask_planner needs a question");
            juce::Array<juce::var> history;
            std::string builder;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                if (planner_.empty())
                    return fail("No planning model is chosen (the model menu's Plan with): decide yourself.");
                if (plannerAsks_ >= kMaxPlannerAsks)
                    return fail("The planner was asked " + std::to_string(kMaxPlannerAsks)
                                + " times this turn: decide yourself.");
                ++plannerAsks_;
                // The history up to the reply that asks (the last message, whose tool calls have no results yet).
                history = messages_;
                if (!history.isEmpty() && str(history.getLast(), "role") == "assistant")
                    history.removeLast();
                const auto m = std::find_if(models_.begin(), models_.end(), [&](const ChatModel& x) { return x.id == turnModel_; });
                builder = m != models_.end() ? m->name : turnModel_;
            }
            const std::string brief = "(From the plugin: " + builder + ", which builds this with the tools, asks you, its "
                                      "planner: " + question
                                    + (context.empty() ? std::string() : "\nWhat it says of where it is: " + context)
                                    + "\nAnswer as the planner: the decision, briefly, and why.)";
            std::string why;
            const std::string answer = consultPlanner(history, brief, true, why);
            if (answer.empty())
                return fail("The planner did not answer: " + why + ". Decide yourself.");
            return "The planner answers:\n" + answer;
        }
        if (name == "get_status")
        {
            const Draft d = host_.chatDraft();
            const Sources running = host_.chatRunning();
            std::string out = "Files: ";
            for (std::size_t i = 0; i < d.files.size(); ++i)
                out += (i == 0 ? "" : ", ") + d.files[i].name + " (" + std::to_string(lineCount(d.files[i].text))
                     + " lines" + (static_cast<int>(i) == d.active ? ", shown" : "") + ")";
            out += "\nWhat plays: ";
            out += running == d.files ? "these files\n" : "an earlier version (the files were edited since the run)\n";
            out += "Last compile: " + compileLine(host_.chatLastStatus());
            out += "Dials:\n" + dialLines(host_.chatDials(), host_);
            out += "CPU: " + number(std::round(host_.chatLoad() * 1000.0f) / 10.0) + "% of the audio thread's time "
                   "(the busiest recent block)\n";
            const uint32_t a = host_.chatAlerts();
            out += "Output guard, last two seconds: ";
            out += a == 0 ? "nothing to report" : ((a & 1u) != 0 ? "NaN or inf samples replaced by 0" : "");
            if ((a & 2u) != 0)
                out += std::string((a & 1u) != 0 ? "; " : "") + "samples beyond +-4 clipped";
            return out + "\n";
        }
        return fail("There is no tool " + name);
    }
}
