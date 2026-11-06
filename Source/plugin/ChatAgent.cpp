// Source/plugin/ChatAgent.cpp: see ChatAgent.h.
#include "plugin/ChatAgent.h"

#include "plugin/ChatCapture.h"
#include "plugin/ChatImages.h"
#include "plugin/ChatScenario.h"

#include "plugin/ChatStore.h"

#include "dsp/Goals.h"
#include "dsp/Measure.h"
#include "dsp/SoundView.h"
#include "dsp/Tune.h"
#include "dsp/TextDiff.h"
#include "fminus/CirDoc.h"
#include "fminus/Circuits.h"

#include "plugin/SoundPicture.h"

#include "mcp/McpGuide.h"
#include "mcp/McpSchema.h"

#include "Library.h"
#include "Product.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
        // The tools that change the program, the dials or the checks: a turn that used none is never sent on by them
        // (set_checks without checks only lists them, and tune_dials with a sweep only measures).
        constexpr const char* kChangingTools[] = { "write_file", "edit_file", "edit_circuit", "rename_file", "delete_file",
                                                   "add_library_file", "run_program", "set_dial", "tune_dials", "set_checks" };
        constexpr int kMaxCircuitOps = 64;         // edit_circuit's commands a call
        constexpr std::size_t kCircuitTextBytes = 4000;   // edit_circuit gives a netlist this size or less back whole
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

        // A compile's notes as run_program reports them: its warnings (FMinus's, a netlist's) on one line, each where it
        // is and what it says, clang's note after one ("add missing switch cases") left out; the other notes as they are.
        struct Reported
        {
            std::string warnings;                // "Warning (it compiles ...): a.fm:7:13: enumeration value ... [-Wswitch]\n"
            std::string notes;
            int count = 0;
        };

        Reported reported(const std::string& notes)
        {
            Reported r;
            std::string list, lastAt;
            std::size_t from = 0;
            while (from < notes.size())
            {
                std::size_t end = notes.find('\n', from);
                if (end == std::string::npos)
                    end = notes.size();
                const std::string line = notes.substr(from, end - from);
                from = end + 1;
                const std::size_t w = line.find(": warning: ");
                if (w != std::string::npos)
                {
                    list += (list.empty() ? "" : "; ") + line.substr(0, w) + ": " + line.substr(w + 11);
                    lastAt = line.substr(0, w);
                    ++r.count;
                    continue;
                }
                const std::size_t n = line.find(": note: ");
                if (n != std::string::npos && !lastAt.empty() && line.substr(0, n) == lastAt)
                    continue;                    // the warning's own note, at its place
                lastAt.clear();
                if (!line.empty())
                    r.notes += line + "\n";
            }
            if (r.count > 0)
                r.warnings = std::string(r.count == 1 ? "Warning" : "Warnings") + " (it compiles and plays as written): " + list + "\n";
            return r;
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

        // The dials of `controls` with their values, one a line: the range in the parameter's own numbers (set_dial's),
        // the default and the value now as the panel says them (their units, their options' names; the number after
        // one the panel scales, as 35 % (0.35)).
        std::string dialLines(const Controls& controls, const ChatHost& host)
        {
            std::string out;
            const std::array<std::vector<std::string>, kMaxControls> options = host.chatDialOptions();
            for (int c = 0; c < kMaxControls; ++c)
            {
                const Control& k = controls[static_cast<std::size_t>(c)];
                if (!k.used)
                    continue;
                const std::vector<std::string>& names = options[static_cast<std::size_t>(c)];
                const auto said = [&](float v) {
                    const std::string t = dialText(k, v, names);
                    return k.type == ControlType::real && k.scale != 1.0f ? t + " (" + number(v) + ")" : t;
                };
                std::string range = k.type == ControlType::toggle ? std::string() : " [" + number(k.min) + ", " + number(k.max) + "]";
                if (k.type == ControlType::real && k.unit[0] != '\0' && k.scale == 1.0f)
                    range += std::string(" ") + k.unit;
                if (!names.empty())
                {
                    range += " (";
                    for (std::size_t i = 0; i < names.size(); ++i)
                        range += (i == 0 ? "" : " ") + names[i];
                    range += ")";
                }
                out += std::to_string(c + 1) + " " + k.name + ": " + controlType(k) + range + " default " + said(k.def)
                     + ", now " + said(host.chatDialValue(c)) + "\n";
            }
            return out.empty() ? "none\n" : out;
        }

        // The dials' declarations (each used one's name, type, range, taper, default, unit, scale and options' names),
        // without their values: run_program lists the dials again only when these changed.
        std::string dialDeclarations(const Controls& controls, const std::array<std::vector<std::string>, kMaxControls>& options)
        {
            std::string out;
            for (int c = 0; c < kMaxControls; ++c)
            {
                const Control& k = controls[static_cast<std::size_t>(c)];
                if (!k.used)
                    continue;
                char buf[160];
                std::snprintf(buf, sizeof buf, "%d %s %d %.9g %.9g %.9g %d %s %d %.9g", c, k.name, static_cast<int>(k.type),
                              static_cast<double>(k.min), static_cast<double>(k.max), static_cast<double>(k.def),
                              static_cast<int>(k.taper), k.unit, static_cast<int>(k.decimals), static_cast<double>(k.scale));
                out += buf;
                for (const std::string& o : options[static_cast<std::size_t>(c)])
                    out += '\x1f' + o;
                out += '\n';
            }
            return out;
        }

        // What a run's new declarations did to the dials' values (Controls.h, "a dial's value across a declaration
        // change"): each dial of the name its slot had whose range, taper, type or options changed, its value before and
        // as the new program keeps it ("size: 9.68 L -> 9.68 L (range changed)", "drive: PITCH -> PITCH (option
        // moved)"), and each that moved to another slot, which starts there at its default; "" when none.
        std::string carriedLines(const Controls& before, const std::array<std::vector<std::string>, kMaxControls>& beforeOptions,
                                 const std::array<float, kMaxControls>& was, const Controls& now,
                                 const std::array<std::vector<std::string>, kMaxControls>& options, const ChatHost& host)
        {
            std::string out;
            for (int c = 0; c < kMaxControls; ++c)
            {
                const auto i = static_cast<std::size_t>(c);
                const Control& k = now[i];
                if (!k.used)
                    continue;
                const float after = host.chatDialValue(c);
                if (before[i].used && sameName(before[i], k))
                {
                    const std::string why = carryWhy(before[i], beforeOptions[i], k, options[i], was[i], after);
                    if (!why.empty())
                        out += std::string(k.name) + ": " + dialText(before[i], was[i], beforeOptions[i]) + " -> "
                             + dialText(k, after, options[i]) + " (" + why + ")\n";
                    continue;
                }
                for (int o = 0; o < kMaxControls; ++o)
                    if (const Control& b = before[static_cast<std::size_t>(o)]; b.used && sameName(b, k))
                    {
                        out += std::string(k.name) + ": " + dialText(b, was[static_cast<std::size_t>(o)], beforeOptions[static_cast<std::size_t>(o)])
                             + " -> " + dialText(k, after, options[i]) + " (moved from dial " + std::to_string(o + 1) + " to "
                             + std::to_string(c + 1) + ": its default)\n";
                        break;
                    }
            }
            return out.empty() ? out : "What the new declarations did to the dials (each kept in its units):\n" + out;
        }

        // The checks a run's new declarations left naming a dial (in their dials or their scenario's moves) that the
        // program before had and this one has not, renamed or removed: each fails until it is changed (Goals.h). Then
        // the checks from a snapshot that holds such a dial: they do not fail for it, but measure another sound, the
        // snapshot's old dial gone and each dial new to the program that the snapshot lacks (and the check does not
        // fix) at its default. "" when none.
        std::string orphanLines(const std::vector<measure::Check>& checks, const Controls& before, const Controls& now)
        {
            const auto has = [](const Controls& controls, const std::string& name) {
                return std::any_of(controls.begin(), controls.end(), [&](const Control& k) { return k.used && name == k.name; });
            };
            const auto label = [&](std::size_t i) { return "check " + std::to_string(i + 1) + " (\"" + checks[i].label + "\"): "; };
            std::string out, shots;
            for (std::size_t i = 0; i < checks.size(); ++i)
            {
                const measure::Check& c = checks[i];
                std::set<std::string> names;
                for (const auto& [n, v] : c.request.dials)
                    names.insert(n);
                for (const measure::DialMove& m : c.request.automate)
                    names.insert(m.dial);
                std::string lost;
                for (const std::string& n : names)
                    if (has(before, n) && !has(now, n))
                        lost += (lost.empty() ? "" : ", ") + n;
                if (!lost.empty())
                    out += label(i) + lost + "\n";
                if (c.dialsFrom != measure::DialsFrom::snapshot)
                    continue;
                std::string gone, fresh;
                int goneCount = 0, freshCount = 0;
                for (const auto& [n, v] : c.snapshot)
                    if (has(before, n) && !has(now, n))
                    {
                        gone += (gone.empty() ? "" : ", ") + n;
                        ++goneCount;
                    }
                if (gone.empty())
                    continue;
                for (const Control& k : now)
                    if (const std::string n = k.name; k.used && !has(before, n) && c.snapshot.count(n) == 0 && c.request.dials.count(n) == 0)
                    {
                        fresh += (fresh.empty() ? "" : ", ") + n;
                        ++freshCount;
                    }
                shots += label(i) + "its snapshot's " + gone + (goneCount == 1 ? " is" : " are") + " gone"
                       + (fresh.empty() ? "" : ", " + fresh + (freshCount == 1 ? " measures at its default" : " measure at their defaults"))
                       + "\n";
            }
            return (out.empty() ? out
                                : "Checks that name a dial the program no longer has (each fails until set_checks changes or "
                                  "removes it):\n" + out)
                 + (shots.empty() ? shots
                                  : "Checks from a snapshot that has a dial the program no longer has (measured without it; "
                                    "set_checks gives one a new snapshot):\n" + shots);
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

        // An effect's input for a sound's render (view_sound's, view_panel's at): what goes in (a sweep unless said),
        // and a sine's frequency.
        void soundInput(const juce::var& input, measure::ViewRequest& vr)
        {
            if constexpr (!kInstrument)
            {
                const std::string in = input.getProperty("input", juce::var()).toString().toStdString();
                vr.input = in == "noise"     ? measure::ViewInput::noise
                         : in == "impulse"   ? measure::ViewInput::impulse
                         : in == "sine"      ? measure::ViewInput::sine
                         : in == "silence"   ? measure::ViewInput::silence
                                             : measure::ViewInput::sweep;
                const juce::var f = input.getProperty("frequency", juce::var());
                if (isNumber(f))
                    vr.frequency = static_cast<float>(static_cast<double>(f));
            }
            else
            {
                static_cast<void>(input);
                static_cast<void>(vr);
            }
        }

        // view_panel's at: a render watched up to its moment `end` (a sample), offline, in the Session's own Engine:
        // the meters after the call at or after each of the last frames' times (60 a second, the last at `end`, at
        // most a field scope's 2.5 s), numbered afresh; at `end`, the scopes and the dials. Its reader comes before the
        // first call, so the levels and the scopes are measured from the first sample (an impulse's at 0 too), and the
        // calls are view_sound's but the last, cut at `end`.
        class MomentHooks final : public measure::RenderHooks
        {
        public:
            static constexpr std::size_t kFrames = 150;

            MomentHooks(std::size_t end, double rate, const std::atomic<bool>& cancel) : end_(end), cancel_(cancel)
            {
                every_ = std::max(1.0, rate / 60.0);
                count_ = std::min<std::size_t>(kFrames, 1 + static_cast<std::size_t>(static_cast<double>(end) / every_));
            }

            void started(Engine& engine) override
            {
                if (!reading_)
                    engine.addReader();
                reading_ = true;
            }

            int callLength(std::size_t at) override
            {
                return at < end_ ? static_cast<int>(std::min<std::size_t>(end_ - at, 1u << 20)) : 0;
            }

            bool afterCall(const measure::CallInfo& call) override
            {
                const std::size_t done = call.at + static_cast<std::size_t>(call.length);
                while (frames.size() < count_ && markOf(frames.size()) <= done)
                {
                    Meters m;
                    if (!call.engine.meters(m) && !frames.empty())
                        m = frames.back();
                    m.frame = static_cast<uint32_t>(frames.size() + 1);
                    frames.push_back(m);
                }
                if (done < end_)
                    return !cancel_.load();
                reached = true;
                constexpr int n = PictureState::kScopeSamples;
                scopeOut.assign(n, 0.0f);
                call.engine.scope(false, scopeOut.data(), n);
                if constexpr (!kInstrument)
                {
                    scopeIn.assign(n, 0.0f);
                    call.engine.scope(true, scopeIn.data(), n);
                }
                call.engine.dials(dials);
                call.engine.removeReader();
                reading_ = false;
                return false;
            }

            std::vector<Meters> frames;
            std::vector<float> scopeIn, scopeOut;
            DialState dials;
            bool reached = false;

        private:
            std::size_t markOf(std::size_t k) const
            {
                const double back = static_cast<double>(count_ - 1 - k) * every_;
                return static_cast<std::size_t>(std::max(0.0, static_cast<double>(end_) - std::round(back)));
            }

            std::size_t end_;
            const std::atomic<bool>& cancel_;
            double every_ = 800.0;
            std::size_t count_ = 1;
            bool reading_ = false;
        };

        // The render's settings as a tool's input schema has them (measure_output's own, which the others share); with
        // `metrics` (the tools that measure them: not view_sound) an instrument's base_hz too.
        void addRenderProps(juce::var& props, bool metrics = true)
        {
            auto* o = props.getDynamicObject();
            if constexpr (kInstrument)
            {
                o->setProperty("notes", claude::object({ { "type", "array" },
                                                         { "items", claude::object({ { "type", "integer" } }) },
                                                         { "description", "MIDI notes played together (69 is A, 440 Hz). "
                                                                          "Default: one note, 48 (C3)." } }));
                o->setProperty("velocity", prop("number", "0 to 1 (default 0.8)"));
                o->setProperty("modwheel", prop("number", "0 to 1, from the start (default 0)"));
                o->setProperty("pitchbend", prop("number", "-1 to 1, from the start (default 0)"));
                if (metrics)
                    o->setProperty("base_hz", prop("number", "off_order_db's base (default: the expected pitch)"));
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
                                                                      "the parameter's own units." } }));
            o->setProperty("bpm", prop("number", "The host's tempo (host.fm); default 120"));
            o->setProperty("scenario", scenario::schema());
            o->setProperty("window", prop("object", "{from, to}: the seconds measured, not the held part"));
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
        // `bare`: a metric alone is one too (a tune_dials sweep's, reported and not judged).
        bool goalOf(const juce::var& in, measure::Goal& g, std::string& why, bool bare = false)
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
            why = measure::invalid(g, bare);
            return why.empty();
        }

        // set_checks' dials_from (Goals.h's DialsFrom): its words in the schema and the session, and back (absent:
        // current); false for a word it is not.
        constexpr const char* kDialsFrom[] = { "current", "snapshot", "defaults" };   // DialsFrom's order

        bool dialsFromOf(const juce::var& v, measure::DialsFrom& out)
        {
            out = measure::DialsFrom::current;
            if (v.isVoid() || v.isUndefined())
                return true;
            for (int k = 0; k < 3; ++k)
                if (v.isString() && v.toString() == kDialsFrom[k])
                {
                    out = static_cast<measure::DialsFrom>(k);
                    return true;
                }
            return false;
        }

        // How a check list shows a check's dials_from ("" for current, the default).
        std::string dialsFromWords(measure::DialsFrom f)
        {
            return f == measure::DialsFrom::snapshot ? "dials as set" : f == measure::DialsFrom::defaults ? "dials at defaults" : "";
        }

        // A float as the shortest decimal that reads back as it (0.8f as 0.8, not 0.800000011920929), for JSON.
        double tidy(float f)
        {
            for (int digits = 6; digits < 9; ++digits)
            {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.*g", digits, static_cast<double>(f));
                if (const double d = std::strtod(buf, nullptr); static_cast<float>(d) == f)
                    return d;
            }
            return static_cast<double>(f);
        }

        // A check as set_checks takes it (an item of its list, with its dials_from and, from a snapshot, the snapshot):
        // what the session keeps (saveState; loadState reads it back) and what set_checks lists. `listing`: without
        // the render settings only an instrument's schema has (an effect's checks never set them), and with its
        // dials_from even when current, so a listed check given back keeps its own whatever the call's says (the
        // session leaves current out, as it always has).
        juce::var checkVar(const measure::Check& c, bool listing)
        {
            const auto byName = [](const std::map<std::string, float>& m) {
                juce::var v = claude::object({});
                for (const auto& [k, x] : m)
                    v.getDynamicObject()->setProperty(juce::Identifier(juce::String::fromUTF8(k.c_str())), tidy(x));
                return v;
            };
            juce::var v = claude::object({ { "label", juce::String::fromUTF8(c.label.c_str()) },
                                           { "metric", juce::String(c.goal.metric) } });
            auto* o = v.getDynamicObject();
            if (listing || c.dialsFrom != measure::DialsFrom::current)
                o->setProperty("dials_from", kDialsFrom[static_cast<int>(c.dialsFrom)]);
            if (c.dialsFrom == measure::DialsFrom::snapshot)
                o->setProperty("snapshot", byName(c.snapshot));
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
            const bool instrument = kInstrument || !listing;
            if (instrument)
                o->setProperty("velocity", tidy(r.velocity));
            o->setProperty("hold_seconds", tidy(r.holdSeconds));
            o->setProperty("tail_seconds", tidy(r.tailSeconds));
            o->setProperty("bpm", tidy(r.bpm));
            if (instrument)
            {
                o->setProperty("modwheel", tidy(r.modwheel));
                o->setProperty("pitchbend", tidy(r.pitchbend));
            }
            if (const juce::var sc = scenario::toVar(r); !sc.isVoid())
                o->setProperty("scenario", sc);
            if (const juce::var w = scenario::windowVar(r.window); !w.isVoid())
                o->setProperty("window", w);
            if (r.baseHz > 0.0)
                o->setProperty("base_hz", r.baseHz);
            if (!r.dials.empty())
                o->setProperty("dials", byName(r.dials));
            return v;
        }

        // The check of the list a merge or remove item means: the one at its index (1 up) when it gives one, else the
        // one labelled `label` (case aside: the item's own, or for merge its metric in its place). The caller weighs a
        // label that is not the indexed check's (remove refuses it, merge names both). -1 when none has the label (or
        // the item gives neither); -2, with why, for an index past the list or a label several checks share.
        int checkAt(const std::vector<measure::Check>& checks, const juce::var& item, const juce::String& label, std::string& why)
        {
            const juce::var index = item.getProperty("index", juce::var());
            if (index.isDouble() || index.isInt() || index.isInt64())
            {
                const double n = static_cast<double>(index);
                if (n >= 1.0 && n <= static_cast<double>(checks.size()) && n == std::floor(n))
                    return static_cast<int>(n) - 1;
                why = "there is no check " + number(n) + " (" + std::to_string(checks.size()) + " are set)";
                return -2;
            }
            if (label.isEmpty())
                return -1;
            int at = -1, count = 0;
            for (std::size_t i = 0; i < checks.size(); ++i)
                if (label.equalsIgnoreCase(juce::String::fromUTF8(checks[i].label.c_str())))
                {
                    at = at < 0 ? static_cast<int>(i) : at;
                    ++count;
                }
            if (count > 1)
            {
                why = std::to_string(count) + " checks are labelled \"" + label.toStdString() + "\": give the one meant by its index";
                return -2;
            }
            return at;
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

        // A goal's properties in a schema: set_checks' (the metrics' names as an enum) or tune_dials' (`tune`: its
        // metric a plain string, which goalOf checks, naming the metrics when it is none of them; the weight). The
        // metric's words are the same in both, so MCP's copy gives them once (dropRepeated).
        juce::var goalProps(bool tune)
        {
            constexpr const char* what = "What to measure (CHECKING YOUR WORK lists them)";
            juce::var p = claude::object({ { "metric", tune ? prop("string", what) : metricEnum(what) },
                                           { "min", prop("number", "At least this, in the metric's unit") },
                                           { "max", prop("number", "At most this, in the metric's unit") },
                                           { "target", prop("number", "Near this (instead of min and max)") },
                                           { "tolerance", prop("number", "How near the target counts, in the metric's "
                                                                         "unit (default: the metric's own)") } });
            if (tune)
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
            addEntry(ChatEntry::Kind::note,
                     std::string("API key saved, encrypted, for both playground plugins on ") + kThisMachine + ".");
        }
        return true;
    }

    void ChatAgent::forgetKey()
    {
        chatstore::forgetKey();
        const std::lock_guard<std::mutex> lock(mutex_);
        key_.clear();
        keyProblem_.clear();
        addEntry(ChatEntry::Kind::note, std::string("API key removed from ") + kThisMachine + ".");
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
        busy_ = mcpWorking_.load();                // a call of Claude Desktop's keeps it busy until it ends (mcpCall)
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
                checks.add(checkVar(c, false));
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
        busy_ = mcpWorking_.load();                // a call of Claude Desktop's keeps it busy until it ends (mcpCall)
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
                k.request = scenario::renderOf(c);
                if (!dialsFromOf(c.getProperty("dials_from", juce::var()), k.dialsFrom))
                    k.dialsFrom = measure::DialsFrom::current;
                if (const auto* shot = c.getProperty("snapshot", juce::var()).getDynamicObject())
                    for (const auto& kv : shot->getProperties())
                        if (isNumber(kv.value) || kv.value.isBool())
                            k.snapshot[kv.name.toString().toStdString()] = static_cast<float>(static_cast<double>(kv.value));
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

    // What Claude is told of a file and remembered to have seen: a netlist's text without its drawing's *@ lines
    // (cir::stripLayout: they change nothing the program does), any other file's whole.
    static std::string seenText(const std::string& name, const std::string& text)
    {
        return isCircuitFile(name) ? cir::stripLayout(text) : text;
    }

    static int layoutLines(const std::string& text)
    {
        int n = 0;
        std::size_t start = 0;
        while (start <= text.size())
        {
            const std::size_t end = std::min(text.find('\n', start), text.size());
            n += cir::isLayoutLine(std::string_view(text).substr(start, end - start)) ? 1 : 0;
            start = end + 1;
        }
        return n;
    }

    void ChatAgent::markSeen(const std::string& name, const std::string& text)
    {
        if (!mcpWorking_.load())                   // Claude Desktop's views are its own (Live.cpp's marks)
            seen_[name] = hashText(seenText(name, text));
    }

    void ChatAgent::carrySeen(const std::string& name, const std::string& before, const std::string& after)
    {
        if (const auto it = seen_.find(name); it != seen_.end() && it->second == hashText(seenText(name, before)))
            markSeen(name, after);
    }

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
                         + std::string(cir::sameProgram(running, d.files) ? "these files"
                                                                           : "an earlier version of the files (they "
                                                                             "were edited since the last run)")
                         + "\nLast compile: " + compileLine(status) + "Dials of the program that plays:\n"
                         + dialLines(host_.chatDials(), host_) + (status.ok ? panelLine(status.panel, status.controls) : "");
        const uint32_t a = host_.chatAlerts();
        tail += "Output alerts: ";
        tail += a == 0 ? "none" : ((a & 1u) != 0 ? ((a & 2u) != 0 ? "NaN or inf replaced, clipped" : "NaN or inf replaced")
                                                 : "clipped (beyond +-4)");
        // A netlist counts without its *@ lines: a change to its drawing alone sends no new block.
        uint64_t signature = hashText(tail + '\x1f' + shown);
        for (const SourceFile& f : d.files)
            signature = (signature ^ hashText(f.name + '\x1f' + seenText(f.name, f.text))) * 1099511628211ull;

        const std::lock_guard<std::mutex> lock(mutex_);
        if (signature == lastSignature_)
            return {};
        lastSignature_ = signature;
        std::string c = "<plugin_state>\nThe plugin's files, a tab each in the editor (the program is the first; the "
                        "editor shows " + shown + "):\n";
        bool unchanged = false;
        for (const SourceFile& f : d.files)
        {
            const std::string text = seenText(f.name, f.text);
            const auto it = seen_.find(f.name);
            if (it != seen_.end() && it->second == hashText(text))
            {
                c += "<file name=\"" + f.name + "\" lines=\"" + std::to_string(lineCount(f.text)) + "\" unchanged=\"true\"/>\n";
                unchanged = true;
                continue;
            }
            c += "<file name=\"" + f.name + "\">\n" + text + (text.empty() || text.back() == '\n' ? "" : "\n") + "</file>\n";
            // a netlist's drawing (the schematic's *@ lines) left out, and said
            if (const int layout = isCircuitFile(f.name) ? layoutLines(f.text) : 0; layout > 0)
                c += "(" + std::to_string(layout) + " layout lines left out: read_file shows them)\n";
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
                       "Read one of the plugin's files as it is now in the editor, or search every file (find). Call "
                       "this before editing a file you "
                       "have not seen in its current form (the user may have typed in it since the last plugin_state). "
                       "Its first line gives the file's line count and version (edit_file's expected); a part (from, "
                       "lines, find) comes with its lines numbered.",
                       // find with no name searches every file, so nothing is required (ChatFiles.cpp).
                       claude::object({ { "name", prop("string", "The file's name, such as program.fm") },
                                        { "from", prop("integer", "The first line to read, from 1") },
                                        { "lines", prop("integer", "How many lines (default: to the end)") },
                                        { "find", prop("string", "A text: the lines that hold it, 2 around each (no name: in "
                                                                 "every file)") },
                                        { "regex", prop("boolean", "find is a regex (RE2's syntax)") } }),
                       {}));
        tools.add(tool("write_file",
                       "Create a file or replace a file's whole text. Use it for a new program or a large rewrite; use "
                       "edit_file for a change to part of a file. The editor shows the file and the user can undo the "
                       "change there. A write does not play anything: call run_program after the files are ready.",
                       claude::object({ { "name", prop("string", "The file's name: letters, digits, _, - and ., not "
                                                                  "starting with '.', such as program.fm or "
                                                                  "filters.fm") },
                                        { "content", prop("string", "The file's whole text") } }),
                       { "name", "content" }));
        {
            // files: several files' edits as one change (each entry as the tool's own name, edits and expected), or a
            // unified diff of them (ChatFiles.cpp); name is then left out, so none is required. Its two forms as anyOf,
            // the form the API's JSON Schema reference names (strict tools too).
            juce::Array<juce::var> listOrDiff;
            listOrDiff.add(claude::object({ { "type", "array" } }));
            listOrDiff.add(claude::object({ { "type", "string" } }));
            tools.add(tool("edit_file",
                           "Replace one exact passage of a file with new text. old_text must occur exactly once in the "
                           "file, with the same whitespace; include enough surrounding lines to make it unique. Prefer "
                           "this to write_file for changes to part of a file. A passage that does not occur gets the "
                           "closest one back, numbered, and what differs. A diff's hunk goes where its old lines occur "
                           "nearest its @@ line.",
                           claude::object({ { "name", prop("string", "The file's name") },
                                            { "old_text", prop("string", "The passage as it is now, exactly") },
                                            { "new_text", prop("string", "What replaces it") },
                                            { "edits", claude::object({ { "type", "array" },
                                                                        { "description", "[{old_text, new_text}] in place of those "
                                                                                         "two: in order, all or none" } }) },
                                            { "expected", prop("string", "read_file's version: write only if the file is still "
                                                                         "it") },
                                            { "files", claude::object({ { "anyOf", juce::var(listOrDiff) },
                                                                        { "description", "[{name, edits, expected}] or a unified "
                                                                                         "diff, in place of name: all or none, one "
                                                                                         "undo step" } }) } }),
                           {}));
        }
        {
            juce::var ops = claude::object({ { "type", "array" },
                                             { "items", claude::object({ { "type", "string" } }) },
                                             { "maxItems", kMaxCircuitOps },
                                             { "description", "One command a line, at most 64 (the CIRCUITS guide has the grammar)" } });
            tools.add(tool("edit_circuit",
                           "Change a .cir netlist the way the schematic editor does: add, place, connect, disconnect, set, "
                           "rename, delete and lay out parts, one command a line in ops, applied together as one step the "
                           "user can undo. All or none: the first command that fails is named and nothing changes. A part "
                           "added without at is placed beside what it connects to, and every wire is routed for you; the "
                           "netlist lines change as little as they can, and the drawing (its *@ lines) is written again. "
                           "Its result says what each command did, the netlist lines it changed, the circuit compiler's "
                           "errors and warnings on the new text, and the whole netlist when it is short. Prefer it to "
                           "edit_file for a netlist's parts and wiring; a write that clashes with the user drawing at that "
                           "moment fails, saying so: try again. Run the program afterwards to hear it.",
                           claude::object({ { "name", prop("string", "The file's name") }, { "ops", ops } }), { "name", "ops" }));
        }
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
                           "Add a ready-made building block (THE LIBRARY in your instructions: FMinus files, and .cir "
                           "netlists that become modules) to the editor as a tab, and get its whole text back; #include "
                           "it in the program to use it. A file of that name "
                           "in the editor already is left as it is.",
                           claude::object({ { "name", name } }), { "name" }));
        }
        tools.add(tool("run_program",
                       "Compile the files as they are now and, when they compile, play them (as the editor's RUN "
                       "does). Returns the compiler's messages, or the new program's dials when their declarations "
                       "changed. Call it after every set of changes, and fix any error it reports before you finish.",
                       claude::object({}), {}));
        tools.add(tool("set_dial",
                       "Set one of the running program's dials (its parameters) to a value in its range, in the "
                       "parameter's own units. Use it to leave a new sound at good settings, like a preset. The user "
                       "can move the dial afterwards.",
                       claude::object({ { "name", prop("string", "The parameter's name, as declared") },
                                        { "value", prop("number", "The value, inside the parameter's range") } }),
                       { "name", "value" }));
        {
            // The render settings the others take (addRenderProps: an instrument's notes, velocity and wheels in an
            // instrument only), in words of its own where it renders otherwise (an effect's sine, noise and impulse).
            juce::var props = claude::object({});
            addRenderProps(props);
            auto* o = props.getDynamicObject();
            if constexpr (!kInstrument)
            {
                o->setProperty("hold_seconds", prop("number", "How long the sine and the noise play; default 1, "
                                                              "at most 10"));
                o->setProperty("tail_seconds", prop("number", "How long to go on after the impulse; default 1.5, "
                                                              "at most 10"));
            }
            o->setProperty("try_edits", scenario::trialSchema());
            o->setProperty("dials_from", scenario::dialsFromSchema(true));
            // The report's band levels as data (Measure.h's Request::spectrum).
            o->setProperty("spectrum", claude::object({ { "enum", juce::Array<juce::var>{ "octave", "third", "semitone" } },
                                                        { "description", kInstrument ? "Band levels while held, as data"
                                                                                     : "The noise's gain in bands, as data" } }));
            tools.add(tool("measure_output",
                           "Render the program in the editor offline and measure what comes out. Its report gives "
                           "the metrics (CHECKING YOUR WORK) in words, with the strongest partials and the largest "
                           "steps' nearest events: an instrument's notes held and released (one note: also a chord), "
                           "an effect's sine, noise, impulse and silence. It compiles the files as they are now (they "
                           "need not be running) and changes nothing that plays.",
                           props, {}));
        }
        tools.add(tool("view_panel",
                       "See the plugin's interface: a picture of the program's panel as the user sees it in the PANEL "
                       "view (its panel.gui, or its dials), drawn from the program that plays, or of a .cir file's "
                       "schematic. Use it after "
                       "you write or change panel.gui and run it, to check the layout as a designer would: crowding, "
                       "overlaps, text cut off, ragged rows, sizes and colours that do not read, a page left empty; "
                       "then fix what you see. view \"window\" shows the whole editor instead (the code and the "
                       "chat). view \"schematic\" shows a netlist's drawing (name, else the first .cir) as the user's "
                       "Schematic view draws it, at 100 %, its text listing each part's place, orientation and pins' "
                       "points, the nets and the problems: look at it before you move parts with edit_circuit. page "
                       "names a tabs element's page to show (the text says which pages there are). "
                       "Shaders are drawn as the user's screen draws them (a striped placeholder only where the GPU "
                       "cannot draw the picture, which its text then says) and the displays show the sound playing "
                       "now. A picture costs about 2,000 input tokens: look once or twice for a new interface, not "
                       "after every small change. To see the panel in a state of your choosing (a dashboard in 12th "
                       "gear at -40 degrees, a warning lit, a meter near the top), values draws dials (in their units, "
                       "or an int's option by its name) and fields (a display's, a live= mark's or a shader's input, as "
                       "panel.gui names them: rpm, shared->gear) at the values given, and in and out at a level in "
                       "dBFS; nothing that plays changes. at draws the state the program that plays reaches at a "
                       "moment: it renders it offline with view_sound's settings ({seconds, notes, dials, scenario, "
                       "...}: seconds from the start, at most 20) and draws its displays, its shaders' inputs and its "
                       "dials as they are then (a field's scope its last 2.5 s, a VU's needle on its way), values over "
                       "it. With either, the text gives what each display shown reads.",
                       claude::object({ { "view", claude::object({ { "type", "string" },
                                                                   { "enum", juce::Array<juce::var>{ "panel", "window", "schematic" } },
                                                                   { "description", "panel (the default), window (the whole "
                                                                                    "editor) or schematic (the drawing of "
                                                                                    "the .cir file name, else the first)" } }) },
                                        { "page", prop("string", "A tabs page to show by its title, else the user's") },
                                        { "name", prop("string", "The file's name") },
                                        { "values", claude::object({ { "type", "object" }, { "description", mcp::kPanelValues } }) },
                                        { "at", claude::object({ { "type", "object" },
                                                                 { "description", "{seconds, view_sound's settings}: the state "
                                                                                  "an offline render of what plays reaches "
                                                                                  "then" } }) } }),
                       {}));
        {
            juce::var props = claude::object({});
            addRenderProps(props, false);
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
            props.getDynamicObject()->setProperty("try_edits", scenario::trialSchema());
            props.getDynamicObject()->setProperty("dials_from", scenario::dialsFromSchema(true));
            tools.add(tool("view_sound",
                           kInstrument
                               ? "See the sound: a PNG of the program rendered offline as measure_output renders it, "
                                 "drawn as the waveform and level over time, a spectrogram, the average spectrum while "
                                 "held (harmonics marked) and a close-up of a few cycles. When to look: CHECKING YOUR "
                                 "WORK. It compiles the files as they are now and changes nothing that plays; the user sees "
                                 "the picture too."
                               : "See the sound: a PNG of the program rendered offline with an input (a sweep by "
                                 "default), drawn as the waveform and level over time, a spectrogram, the average "
                                 "spectrum while the input plays and a close-up of a few cycles. You cannot "
                                 "hear the plugin: look when the sound's shape matters, as a filter's response (a "
                                 "sweep: where it is cut), what a drive adds (a sweep's or a sine's harmonics, aliasing "
                                 "that folds back down), a reverb's or a delay's tail (an impulse), a modulation's "
                                 "motion, a click. It compiles the files as they are now and changes nothing that "
                                 "plays; the user sees the picture too.",
                           props, {}));
        }
        {
            juce::var item = goalProps(false);
            item.getDynamicObject()->setProperty("label", prop("string", "What it checks, in a few words the user reads "
                                                                         "(\"Releases within two seconds\")"));
            addRenderProps(item);
            item.getDynamicObject()->setProperty("index", prop("integer", "merge, remove: its number (remove: its label must match)"));
            juce::var checks = claude::object({ { "type", "array" },
                                                { "maxItems", kMaxChecks },
                                                { "items", claude::object({ { "type", "object" },
                                                                            { "properties", item },
                                                                            { "required", juce::Array<juce::var>{ "label" } } }) },
                                                { "description", "Each a metric with min, max or target, in a render "
                                                                 "of its own (measure_output's settings; defaults when "
                                                                 "left out). Empty: none; left out: lists them" } });
            tools.add(tool("set_checks",
                           "Set the checks this work must pass: what the user asked for, as numbers the plugin "
                           "measures (the metrics in CHECKING YOUR WORK), each with min, max or a target. Without "
                           "checks it lists them, each as set_checks takes it. The list replaces the checks set before "
                           "(an empty one clears them), or merges or removes. The plugin measures them now, "
                           "then as CHECKING YOUR WORK says; its results give the count and each check that fails, "
                           "naming the dials it took that are off their defaults, or a dial it names that the program lacks.",
                           claude::object({ { "checks", checks },
                                            { "mode", claude::object({ { "type", "string" },
                                                                       { "enum", juce::Array<juce::var>{ "replace", "merge", "remove" } },
                                                                       { "description", "merge: each replaces the check of its "
                                                                                        "label or index, or is added; remove: "
                                                                                        "those (default: replace all)" } }) },
                                            { "dials_from", claude::object({ { "type", "string" },
                                                                             { "enum", juce::Array<juce::var>{ kDialsFrom[0], kDialsFrom[1], kDialsFrom[2] } },
                                                                             { "description", "The dials a check does not name: current "
                                                                                              "(the default: as they are when "
                                                                                              "measured), snapshot (as they are "
                                                                                              "now) or defaults" } }) } }),
                           {}));
        }
        {
            juce::var props = claude::object({});
            addRenderProps(props);
            props.getDynamicObject()->setProperty(
                "tune", claude::object({ { "type", "array" },
                                         { "items", claude::object({ { "type", "string" } }) },
                                         { "minItems", 1 },
                                         { "maxItems", measure::kMaxTuned },
                                         { "description", "The float dials to tune, by name (a sweep's: one dial, of "
                                                          "any type)" } }));
            props.getDynamicObject()->setProperty(
                "goals", claude::object({ { "type", "array" },
                                          { "minItems", 1 },
                                          { "maxItems", measure::kMaxGoals },
                                          { "items", claude::object({ { "type", "object" },
                                                                      { "properties", goalProps(true) },
                                                                      { "required", juce::Array<juce::var>{ "metric" } } }) },
                                          { "description", "What the measurements should come to" } }));
            props.getDynamicObject()->setProperty(
                "sweep", claude::object({ { "type", "array" },
                                          { "items", claude::object({ { "type", "number" } }) },
                                          { "description", "Values of the one dial, up to 32, each measured in turn; "
                                                           "no search, nothing set" } }));
            props.getDynamicObject()->setProperty("budget", prop("integer", "Renders at most (default 150, at most 400)"));
            props.getDynamicObject()->setProperty("apply", prop("boolean", "Set the dials to the best values found "
                                                                           "(default true)"));
            props.getDynamicObject()->setProperty("seconds", prop("number", "Seconds it may search (default 120, at most "
                                                                             "170), then the best so far"));
            tools.add(tool("tune_dials",
                           "Find values of up to eight float dials that bring measurements to goals by rendering "
                           "offline (as measure_output does) and searching, instead of guessing; or sweep one dial "
                           "over values: the goals' metrics at each, side by side. Goals are "
                           "metrics (CHECKING YOUR WORK) with min, max "
                           "or a target (a sweep's may be a metric alone). The best values go on the dials, as set_dial "
                           "sets them, unless apply is false; the result gives the dials and the measurements before "
                           "and after. Dials not tuned keep their values (or those in dials). It changes no file.",
                           props, { "tune", "goals" }));
        }
        tools.add(compareDefinition());
        tools.add(traceDefinition());
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
        tools.add(statusDefinition());               // get_status (Live.cpp)
        tools.add(listenDefinition());               // listen (Live.cpp)
        tools.add(captureToolDefinition());          // capture (ChatCapture.cpp)
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
                                : name == "edit_circuit"                    ? std::string("Drawing the circuit")
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
                bool checking = false, current = true;   // current: every check takes the dials as they are now
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    checking = !checks_.empty();
                    for (const measure::Check& c : checks_)
                        current = current && c.dialsFrom == measure::DialsFrom::current;
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
                            std::string("(From the plugin: your checks, measured on the files")
                            + (current ? " and dials as they are now" : " as they are now, each with the dials it takes")
                            + ".\n" + results
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
                const juce::var asked = use.getProperty("input", juce::var()).getProperty("checks", juce::var());
                const bool listing = str(use, "name") == "set_checks" && (asked.isVoid() || asked.isUndefined());
                const juce::var swept = use.getProperty("input", juce::var()).getProperty("sweep", juce::var());
                const bool sweep = str(use, "name") == "tune_dials" && !swept.isVoid() && !swept.isUndefined();
                // set_checks' list and tune_dials' sweep change nothing (the handlers' own tests)
                if (!static_cast<bool>(result.getProperty("is_error", false)) && !listing && !sweep)
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
        const std::vector<measure::CheckResult> results = scenario::evaluate(host_.chatDraft().files, checks, currentDials(), host_, &errors);
        // A failing check's dials off their reference, as the panel says them (the options of the program that plays).
        const Controls playing = host_.chatDials();
        const std::array<std::vector<std::string>, kMaxControls> options = host_.chatDialOptions();
        const auto said = [&](const measure::DialAway& a, float v) {
            for (int c = 0; c < kMaxControls; ++c)
                if (playing[static_cast<std::size_t>(c)].used && a.name == playing[static_cast<std::size_t>(c)].name)
                    return dialText(a.control, v, options[static_cast<std::size_t>(c)]);
            return dialText(a.control, v);
        };
        // A failing check at the defaults takes none of the plugin's dials: moving them changes nothing it measures.
        const auto awayText = [&](const measure::Check& k, const measure::CheckResult& r) {
            if (k.dialsFrom == measure::DialsFrom::defaults && r.unknown.empty())
                return std::string("; the dials as they are now do not affect it");
            if (r.away.empty())
                return std::string();
            constexpr std::size_t most = 6;
            const bool snapshot = k.dialsFrom == measure::DialsFrom::snapshot;
            std::string list;
            for (std::size_t i = 0; i < r.away.size() && i < most; ++i)
                list += (i == 0 ? "" : ", ") + r.away[i].name + " " + said(r.away[i], r.away[i].now) + " ("
                      + (snapshot ? "then " : "default ") + said(r.away[i], r.away[i].reference) + ")";
            if (r.away.size() > most)
                list += " and " + std::to_string(r.away.size() - most) + " more";
            return std::string(snapshot ? "; it takes the dials as they were when set, and these moved since: "
                                        : "; it takes the dials as they are now, these off their defaults: ")
                 + list;
        };
        std::vector<ChatCheck> rows;
        int pass = 0;
        std::string lines, failing;                      // every check's line (the card's text), and those that fail
        for (std::size_t i = 0; i < results.size() && i < checks.size(); ++i)
        {
            const measure::CheckResult& r = results[i];
            // Its window, its base and where it takes its dials from, as its line and the card say them.
            const measure::Window& w = checks[i].request.window;
            std::string from;
            for (const std::string& part : { w.set() ? measure::windowText(w) : std::string(),
                                             checks[i].request.baseHz > 0.0 ? "base_hz " + number(checks[i].request.baseHz) : std::string(),
                                             dialsFromWords(checks[i].dialsFrom) })
                if (!part.empty())
                    from += (from.empty() ? "" : "; ") + part;
            rows.push_back({ checks[i].label, r.text + (from.empty() ? "" : "; " + from), r.measured, r.pass });
            pass += r.measured && r.pass ? 1 : 0;
            const std::string line = std::string(!r.measured ? "- NOT MEASURED " : r.pass ? "- pass " : "- FAIL ") + checks[i].label
                                   + " (" + checks[i].goal.metric + (from.empty() ? "" : "; " + from) + "): " + r.text
                                   + (r.measured && !r.pass ? awayText(checks[i], r) : std::string()) + "\n";
            lines += line;
            if (!(r.measured && r.pass))
                failing += line;
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
        // Claude gets the count and what fails: the user's card shows every check.
        return head + "\n" + failing + (errors.empty() ? "" : "The program does not compile:\n" + capped(errors));
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
            { "edit_file", "Edit a file", false, false },         { "edit_circuit", "Edit a circuit", false, false, "circuits" },
            { "rename_file", "Rename a file", false, false },
            { "delete_file", "Delete a file", false, true },      { "add_library_file", "Add a building block", false, false, "library" },
            { "run_program", "Run the program", false, false },   { "set_dial", "Turn a dial", false, false },
            { "measure_output", "Measure the sound", true, false }, { "view_panel", "Look at the panel", true, false, "interface" },
            { "view_sound", "Look at the sound", true, false },   { "set_checks", "Set checks on the sound", false, false },
            { "tune_dials", "Tune the dials", false, false },     { "get_status", "Check the plugin", true, false },
            { "listen", "Listen live", true, false, "debugging" },
            { "compare", "Compare two sounds", true, false, "debugging" },
            { "trace", "Trace a problem", true, false, "debugging" },
            { "capture", "Record what you play", true, false, "debugging" },
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

    // What the MCP server's gui_sandbox (mcp/Main.cpp; its helper, Source/sandbox/) does past its short description: the
    // guide's interface part carries it with the plugin's own tools' notes (mcpGuide), so that it reaches Claude Desktop
    // from a plugin open or from the guide.md one left. The plugin's own chat has no sandbox (it has view_panel and its
    // live panel), and its prompt says nothing of one.
    namespace
    {
        constexpr const char* kSandboxNotes =
            "The sandbox, the server's own tool, works with or without the plugin open. Use it to design or rework a panel: a "
            "new one before the program has one, another look for the user's, a panel for a program the user named on disk, "
            "trying a design out before the user commits to it. view_panel is for what the user has now: the panel of the "
            "program that plays, as they see it. Nothing the sandbox does changes what the plugin plays or shows, until apply.\n"
            "- open: from \"blank\" (a small panel of the dials you give), \"instance\" (the open plugin's panel.gui and "
            "program files, read only; a program without a .gui starts from a blank panel of its dials), \"file\" (path: a "
            ".gui file or a program's folder the user named, its .fm, .gui and .sc files, read only, 256 KB a file), or text "
            "alone (that .gui). The dials come from the program's param declarations (read, never compiled: their ranges and "
            "defaults) and from the dials you give, yours replacing a declaration of the same name: [{ name, range: [lo, hi], "
            "default, type: float, int or bool, unit, options: the names of an int's values, style: knob, slider, switch, "
            "buttons or menu, the control a blank panel gives it }]; a unit shows only as the .gui's unit= (as in the "
            "plugin), which a blank panel writes for a float's knob or slider. With no program, the fields the displays name "
            "are mocked as floats. It answers as edit does, after a line of what it opened and the sandbox's number: pass "
            "that number as sandbox in every later call.\n"
            "- edit: text (the .gui whole), or edits ([{ old_text, new_text }], each old_text found exactly once, all or "
            "none), and dials to change. It answers with the plugin's notes on the .gui, by line, and a layout report: the "
            "natural size against the area (render's last size; after open the PANEL view at the plugin's default size, "
            "853 x 612), a line an element (its line in the file, kind and name, x,y wxh in the area), then overlaps, what "
            "sticks out of its container, text cut, what scrolls, controls that name no dial and dials no control names; "
            "detail \"high\" lists every line.\n"
            "- render: a PNG of the panel as the plugin's PANEL view draws it (the same layout, controls, looks, fonts and "
            "shaders, drawn by the GPU), width x height px (the last render's, 853 x 612 after open; 160 to 2400), theme "
            "graphite or paper (else the user's, as the plugin shows it), a tabs page by its title, and the dials at the "
            "values you give ({ \"cutoff\": 2000, \"wave\": \"SAW\" }: numbers in their units, an int's or a bool's by "
            "its names), the rest at their defaults. The displays show demo values (a steady tone, fields moving across "
            "their ranges, a live= mark under its knob), not the user's sound, but the fields values names (a display's, "
            "a live= mark's or a shader's input, as the .gui names them: { \"shared->gear\": 12 }) and in and out (a "
            "level in dBFS), drawn at those values, as view_panel's values draws them. The picture is made smaller to "
            "fit, unless detail \"high\".\n"
            "- apply: only when the user asks for the design in their plugin, with sandbox (its number: needed). It writes "
            "the .gui into the open plugin's program (its own .gui file when it has one, else panel.gui) and runs it "
            "there, which the user sees at once; it says what changed and what the run said. With no plugin open it gives "
            "the .gui's text back: tell the user to open the plugin and ask again, or to paste it in place of the "
            "program's .gui (a new tab panel.gui when it has none).\n"
            "- close: when you are done. Claude Desktop's conversations share the sandbox's server: each open makes a "
            "sandbox of its own number, the newest eight are kept, and the number you pass keeps yours apart from "
            "another conversation's (without it, edit and render act on the one used last). The helper quits after ten "
            "idle minutes, and comes back on the next call with the panel as it was.";
    }

    juce::var ChatAgent::mcpTools()
    {
        // MCP's copy of the schemas (mcp/McpSchema.h): a property's words given once unless they hold a number (the
        // render settings view_sound describes, again in a check's and in tune_dials'; a goal's, set_checks', again in
        // tune_dials'), and the annotations only what MCP's defaults do not say.
        juce::Array<juce::var> out;
        std::set<std::pair<juce::String, juce::String>> described;   // properties described so far (dropRepeated)
        const juce::var all = toolDefinitions();         // held: getArray() points into it
        if (const juce::Array<juce::var>* tools = all.getArray())
            for (const juce::var& t : *tools)
            {
                const juce::String name = t["name"].toString();
                if (!mcpOffers(name))
                    continue;
                const Face* f = faceOf(name);
                const juce::String title = f != nullptr ? juce::String(f->title) : name;
                const juce::var schema = juce::JSON::parse(juce::JSON::toString(t["input_schema"]));   // a copy of its own
                if (name == "view_panel" || name == "view_sound")
                    if (juce::DynamicObject* props = schema["properties"].getDynamicObject())
                        props->setProperty("detail", claude::object({ { "type", "string" },
                                                                      { "enum", juce::Array<juce::var>{ "high" } },
                                                                      { "description", "high: the picture whole (else made "
                                                                                       "smaller to fit)" } }));
                mcp::dropRepeated(schema, described);
                out.add(claude::object({ { "name", name },
                                         { "title", title },
                                         { "description", juce::String::fromUTF8(leadOf(t["description"].toString().toStdString()).first.c_str()) },
                                         { "inputSchema", schema },
                                         { "annotations", mcp::annotations(f != nullptr && f->readOnly, f != nullptr && f->destructive) } }));
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
        // gui_sandbox, the MCP server's own tool (mcp/Main.cpp, Source/sandbox/): its notes go with the interface's.
        notes += std::string("<tool name=\"gui_sandbox\" part=\"interface\">") + kSandboxNotes + "</tool>\n";
        return std::string(mcp::guide::kOutside) + "\n\n" + "You are driving " + product::kName + " (an " + (kInstrument ? "instrument" : "effect")
             + " plugin) through MCP, from Claude Desktop, as its own chat would: the tools are the chat's, but ask_user and "
               "ask_planner (ask the user here instead) and the web tools (use your own). The user hears the plugin in "
               "their DAW (or its Standalone app) and sees what you do in its editor; its chat's card notes each tool you "
               "use. The plugin does not send you its state with each message, as it does its own chat (its "
               "<plugin_state>): call get_status for the files, the dials and the last run, and read_file before editing a file. "
               "With more than one instance of the plugin open, every tool takes `instance`: the one meant, by its "
               "name (\"" + std::string(kInstrument ? "Synth" : "Effect") + " 2\"), its track or its project, in the "
               "user's words (list_instances names them). "
               "This guide comes in parts (playground_guide's part): \"start\" is what every piece of work needs, and its end "
               "names the others and when to read them; a section it calls \"below\" that is not in the part you read is in "
               "another part. The tools' descriptions here are short: what they leave out is in " + mcp::guide::kNotes
             + ", at the end of the part that needs it; and a setting described in one tool's schema without a number is "
               "not described again in a later one's (a check's and tune_dials' render settings are as measure_output's, "
               "their goals as set_checks', whose metric lists the names tune_dials' takes). "
               "What follows is the guide the plugin's chat works from, but for what only that chat has.\n\n"
             + guidePrompt() + "\n" + mcp::guide::kNotes + "\n\n" + notes
             // Last, in part "all" alone (every other part ends before it): the lines only the plugin's chat needs.
             + "\n" + mcp::guide::kChatOnly + "\n\n"
               "The plugin's own chat also works from these lines of How you work; they are not for you here (none of what "
               "they name reaches you through MCP):\n"
             + chatOnlyLines();
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
            || (name == "capture" && (action == "status" || action == "stop")    // the recorder (wave 1, lane "capture"),
                && !static_cast<bool>(arguments.getProperty("picture", false))); // but a picture, drawn on the worker
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
        else
        {
            // A listen (running or not yet started), or a call about to start: each cancel is kept, however many come
            // before it looks (mcpCall's start, Live.cpp's listen), the newest kMcpStopped.
            const std::lock_guard<std::mutex> live(liveMutex_);
            mcpStopped_.insert(call);
            while (mcpStopped_.size() > kMcpStopped)
                mcpStopped_.erase(mcpStopped_.begin());
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
            mcpWorking_.store(true);
            mcpRunning_ = call;
            bool early = false;
            {
                const std::lock_guard<std::mutex> stopped(liveMutex_);
                early = mcpStopped_.erase(call) != 0;
            }
            cancel_.store(call != 0 && (early || mcpCancelled_.load() == call));   // a cancel that came before it started
            if (!mcpNoted_)
            {
                addEntry(ChatEntry::Kind::note, "Claude Desktop is working on the program through MCP: what it does shows here.");
                mcpNoted_ = true;
            }
            activity_ = "Claude Desktop is at work";
            touch();
        }
        const juce::var use = claude::object({ { "type", "tool_use" }, { "id", "mcp" }, { "name", juce::String(name) }, { "input", input } });
        const juce::var block = live ? liveTool(use, { true, call }) : runTool(use);
        if (!live)
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            busy_ = false;
            mcpWorking_.store(false);
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
                                 + std::to_string(f.height)
                                 + (f.width == f.fromWidth && f.height == f.fromHeight
                                        ? std::string(", its full size")
                                        : ", from " + std::to_string(f.fromWidth) + " x " + std::to_string(f.fromHeight))
                                 + ". detail \"high\" sends it whole.)"));
                }
                else if (type == "text")
                    out.add(claude::object({ { "type", "text" }, { "text", b["text"] } }));
            }
        if (out.isEmpty())
            out.add(claude::object({ { "type", "text" }, { "text", "(no result)" } }));
        if (name != "get_status")                        // what the user changed since MCP's last result (Live.cpp)
            if (const std::string hint = mcpHint(); !hint.empty())
                out.insert(0, text(hint));
        r.content = out;
        r.isError = static_cast<bool>(block.getProperty("is_error", false));
        return r;
    }

    // ---- view_panel's state ---------------------------------------------------------------------------------------------
    std::string ChatAgent::panelState(const juce::var& input, PictureState& state, bool& defaults)
    {
        state = {};
        defaults = false;
        // values: by name, a number (a dial's plain value, a field's, a level's dBFS) or a dial's option by name.
        const juce::var values = input.getProperty("values", juce::var());
        if (const juce::DynamicObject* o = values.getDynamicObject())
            for (const auto& kv : o->getProperties())
            {
                PictureValue v;
                v.name = kv.name.toString().toStdString();
                if (isNumber(kv.value) || kv.value.isBool())
                    v.number = static_cast<float>(static_cast<double>(kv.value));
                else if (kv.value.isString())
                {
                    v.text = kv.value.toString().trim().toStdString();
                    v.isText = true;
                }
                else
                    return "values takes a number for each name (a dial's option may be its name): " + v.name + " has none.";
                state.values.push_back(std::move(v));
            }
        else if (!values.isVoid())
            return "values is an object: {\"name\": number}.";
        const juce::var at = input.getProperty("at", juce::var());
        if (at.isVoid())
            return {};
        if (!at.isObject())
            return "at is an object: {seconds, and view_sound's settings}.";
        // What plays, as it plays: a trial change is view_sound's alone (the panel drawn is the running program's), and
        // seconds is the one moment.
        if (at.hasProperty("try_edits"))
            return "at renders the program that plays: try a change with view_sound, or run it first.";
        if (at.hasProperty("from_seconds") || at.hasProperty("to_seconds") || at.hasProperty("window"))
            return "at takes no from_seconds, to_seconds or window: seconds is the moment it draws.";
        if (at.hasProperty("base_hz"))
            return "at takes no base_hz: it measures nothing.";
        // at: the program that plays, rendered offline with view_sound's settings to its moment.
        constexpr double kMaxAt = 20.0;
        const juce::var sec = at.getProperty("seconds", juce::var());
        const double seconds = isNumber(sec) ? static_cast<double>(sec) : -1.0;
        if (!(seconds > 0.0) || seconds > kMaxAt)
            return "at needs seconds: the moment to draw, from the render's start, more than 0 and at most 20.";
        const Sources files = host_.chatRunning();
        if (files.empty())
            return "Nothing plays yet: at renders the program that plays (run_program plays the editor's files).";
        measure::ViewRequest vr;
        std::string why;
        vr.render = scenario::renderOf(at, &why);
        if (why.empty() && scenario::dialsFrom(at, defaults, why) && !defaults)   // as view_sound reads it
            vr.render.current = currentDials();
        soundInput(at, vr);
        if (why.empty())
            scenario::resolve(vr.render, host_, why);
        if (!why.empty())
            return "at: " + why;
        defaults = defaults && !vr.render.fromCapture;   // a recording plays its own dials (view_sound's line)
        setActivity("Rendering the program to its moment");
        measure::Session session(files, vr.render.rate);
        if (!session.compiled())
            return "the program that plays did not compile for the render:\n" + capped(session.errors());
        // As view_sound renders it (its words say so), and on past its end, silent, to a later moment.
        const std::size_t end = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(seconds * session.rate())));
        if (end > session.lengthOf(vr.render))
            vr.length = end;
        MomentHooks hooks(end, session.rate(), cancel_);
        const measure::Sound sound = measure::renderSound(session, vr, &hooks);
        if (cancel_.load())
            return "stopped.";
        if (!hooks.reached || hooks.frames.empty())
            return "the render ended before its moment.";
        state.sources = files;                    // the picture draws it only if this is still what plays
        state.frames = std::move(hooks.frames);
        state.scopeIn = std::move(hooks.scopeIn);
        state.scopeOut = std::move(hooks.scopeOut);
        const Controls& cs = session.controls();
        for (int c = 0; c < kMaxControls; ++c)
            if (cs[static_cast<std::size_t>(c)].used)
                state.dials.emplace_back(cs[static_cast<std::size_t>(c)].name, hooks.dials.plain[static_cast<std::size_t>(c)]);
        char when[32];
        std::snprintf(when, sizeof when, "%.2f s", seconds);
        state.about = std::string(when) + " into an offline render of the program that plays (" + sound.title
                    + "; nothing that plays changed)";
        return {};
    }

    // ---- the tools ----------------------------------------------------------------------------------------------------
    juce::var ChatAgent::runTool(const juce::var& use)
    {
        const std::string name = str(use, "name");
        const juce::var input = use.getProperty("input", juce::var());
        if (name == "listen" || name == "get_status")   // a turn's (Live.cpp); MCP's come through mcpCall
            return liveTool(use, {});
        if (name == "compare")
            return compareTool(use);
        if (name == "trace")
            return traceTool(use);
        if (name == "capture")
            return captureTool(use);                              // ChatCapture.cpp
        if (name == "view_panel")
        {
            // A picture of the panel: an image block and a line of what it shows (the tool_result's content).
            const std::string view = str(input, "view");
            const PictureView pv = view == "window" ? PictureView::window : view == "schematic" ? PictureView::schematic : PictureView::panel;
            juce::var block = claude::object({ { "type", "tool_result" }, { "tool_use_id", use.getProperty("id", "") } });
            std::string what = str(input, "page");
            if (pv == PictureView::schematic)
            {
                // a netlist's drawing: the one named, else the first .cir
                what = str(input, "name");
                const Draft d = host_.chatDraft();
                std::string cirs;
                for (const SourceFile& f : d.files)
                    if (isCircuitFile(f.name))
                    {
                        cirs += (cirs.empty() ? "" : ", ") + f.name;
                        if (what.empty())
                            what = f.name;
                    }
                const bool there = std::any_of(d.files.begin(), d.files.end(), [&](const SourceFile& f) { return f.name == what; });
                if (what.empty() || !isCircuitFile(what) || !there)
                {
                    block.getDynamicObject()->setProperty(
                        "content", juce::String::fromUTF8((cirs.empty() ? std::string("No picture: the plugin has no .cir file to draw")
                                                                        : "No picture: " + (what.empty() ? std::string("name a netlist") : what + " is not one of its netlists")
                                                                              + " (its .cir files: " + cirs + ")").c_str()));
                    block.getDynamicObject()->setProperty("is_error", true);
                    return block;
                }
                setActivity("Looking at " + what);
            }
            else
                setActivity("Looking at the panel");
            // A state of Claude's choosing (values), or the one the program reaches at a moment (at): the panel's.
            PictureState state;
            bool defaults = false;                       // at's dials not named at their defaults: the first line says so
            std::string note;
            if (pv == PictureView::schematic)
            {
                if (input.hasProperty("values") || input.hasProperty("at"))
                    note = " (values and at draw the panel's displays and dials: the schematic shows the netlist as it is.)";
            }
            else if (const std::string why = panelState(input, state, defaults); !why.empty())
            {
                block.getDynamicObject()->setProperty("content", juce::String::fromUTF8(("No picture: " + why).c_str()));
                block.getDynamicObject()->setProperty("is_error", true);
                return block;
            }
            if (state.rendered())
                setActivity("Looking at the panel");
            const PanelPicture pic = host_.chatPicture(pv, what, state);
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
            content.add(claude::object({ { "type", "text" },
                                         { "text", juce::String::fromUTF8((scenario::dialsLine(defaults) + pic.about + note).c_str()) } }));
            block.getDynamicObject()->setProperty("content", content);
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                addEntry(ChatEntry::Kind::tool, pv == PictureView::window      ? std::string("Looked at the editor")
                                                : pv == PictureView::schematic ? "Looked at the schematic of " + what
                                                                               : std::string("Looked at the panel"));
            }
            return block;
        }
        if (name == "view_sound")
        {
            // A picture of the sound: an image block and what it shows; the chat shows the picture too.
            setActivity("Drawing the sound");
            measure::ViewRequest vr = soundpicture::request();
            std::string why;
            vr.render = scenario::renderOf(input, &why);
            bool defaults = false;
            if (why.empty() && scenario::dialsFrom(input, defaults, why) && !defaults)
                vr.render.current = currentDials();
            soundInput(input, vr);
            const juce::var from = input.getProperty("from_seconds", juce::var()), to = input.getProperty("to_seconds", juce::var());
            if (isNumber(from) && isNumber(to))
            {
                vr.from = static_cast<float>(static_cast<double>(from));
                vr.to = static_cast<float>(static_cast<double>(to));
            }
            Sources files = host_.chatDraft().files;   // a copy: the trial edits (ChatScenario.h) go there
            int trials = 0;
            if (why.empty() && scenario::resolve(vr.render, host_, why))
                scenario::tryEdits(input, files, trials, why);
            juce::var block = claude::object({ { "type", "tool_result" }, { "tool_use_id", use.getProperty("id", "") } });
            if (!why.empty())
            {
                block.getDynamicObject()->setProperty("content", juce::String::fromUTF8(why.c_str()));
                block.getDynamicObject()->setProperty("is_error", true);
                return block;
            }
            const measure::View v = measure::view(files, vr);
            if (!v.compiled)
            {
                block.getDynamicObject()->setProperty(
                    "content", juce::String::fromUTF8((scenario::trialLine(trials) + "It did not compile, so nothing was drawn:\n"
                                                       + capped(v.errors)).c_str()));
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
            content.add(claude::object({ { "type", "text" },
                                         { "text", juce::String::fromUTF8((scenario::trialLine(trials)
                                                                           + scenario::dialsLine(defaults && !vr.render.fromCapture)
                                                                           + d.about).c_str()) } }));
            block.getDynamicObject()->setProperty("content", content);
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                addEntry(ChatEntry::Kind::tool, "looked at the sound");
                if (!vr.render.fromCapture)                     // the user's recording: no thumbnail kept in the session
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
            return readFileText(input, isError);           // ChatFiles.cpp
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
            int keptLines = 0;
            if (i < 0)
            {
                if (d.files.size() >= kMaxFiles)
                    return fail("The editor holds at most " + std::to_string(kMaxFiles) + " files; delete one first");
                d.files.push_back({ file, content });
                i = static_cast<int>(d.files.size()) - 1;
            }
            else
            {
                // A netlist written without its drawing keeps the old one (its *@ lines, put back before .end): the
                // places the user gave its parts stay; a line naming a part that is gone goes at the next drawing.
                const std::string& old = d.files[static_cast<std::size_t>(i)].text;
                if (isCircuitFile(file) && layoutLines(content) == 0 && (keptLines = layoutLines(old)) > 0)
                {
                    std::string block;
                    std::size_t at = 0;
                    while (at <= old.size())
                    {
                        const std::size_t end = std::min(old.find('\n', at), old.size());
                        std::string_view line = std::string_view(old).substr(at, end - at);
                        if (!line.empty() && line.back() == '\r')
                            line.remove_suffix(1);
                        if (cir::isLayoutLine(line))
                            block += std::string(line) + "\n";
                        at = end + 1;
                    }
                    content = cir::withBlock(content, block);
                }
                d.files[static_cast<std::size_t>(i)].text = content;
            }
            d.active = i;
            setActivity("Writing " + file);
            host_.chatSetDraft(d);
            std::string version;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                markSeen(file, content);
                version = keepVersion(file, content);
            }
            const int lines = lineCount(content);
            const std::string count = std::to_string(lines) + (lines == 1 ? " line" : " lines");
            note("wrote " + file + " (" + count + ")");
            std::string out = "Wrote " + file + " (" + count + "; version " + version + ").";
            if (keptLines > 0)
                out += " Kept the drawing's " + std::to_string(keptLines) + " *@ lines; send edit_circuit 'layout' to redraw it.";
            bool included = i == 0;
            for (const SourceFile& f : d.files)
                if (f.text.find("#include \"" + file + "\"") != std::string::npos)
                    included = true;
            if (!included)
                out += " No file #includes it yet.";
            return out;
        }
        if (name == "edit_file")
            return editFileText(input, isError);           // ChatFiles.cpp
        if (name == "edit_circuit")
        {
            std::string file;
            if (!stringArg("name", file))
                return fail("edit_circuit needs the netlist's name and its ops");
            const juce::var opsVar = input.getProperty("ops", juce::var());
            const juce::Array<juce::var>* list = opsVar.getArray();
            if (list == nullptr || list->isEmpty())
                return fail("edit_circuit needs ops: one command a line (THE SCHEMATIC, in CIRCUITS, has the grammar)");
            if (list->size() > kMaxCircuitOps)
                return fail("edit_circuit takes at most " + std::to_string(kMaxCircuitOps) + " commands a call: send the rest in another");
            if (!isCircuitFile(file))
                return fail(file + " is not a .cir netlist: edit_circuit changes a netlist (edit_file any file)");
            std::vector<cir::Op> ops;
            for (int k = 0; k < list->size(); ++k)
            {
                const juce::var& v = (*list)[k];
                const std::string line = v.isString() ? v.toString().toStdString() : std::string();
                std::string why;
                const std::optional<cir::Op> op = v.isString() ? cir::parseOp(line, why) : std::nullopt;
                if (!op)
                    return fail("op " + std::to_string(k + 1) + " (" + (v.isString() ? line : std::string("not a string")) + "): "
                                + (why.empty() ? std::string("one command a line") : why) + ". Nothing was changed.");
                ops.push_back(*op);
            }
            setActivity("Drawing the circuit");
            // Read, apply on a copy (all or none; the layout runs here too, pure), and write only while the netlist is
            // still the text read: a gesture of the user's that synced meanwhile is never overwritten. The commands run
            // once more on the new text; a second clash gives up.
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                Draft d = host_.chatDraft();
                const int i = findFile(d, file);
                if (i < 0)
                    return fail("There is no file " + file + ". The files: " + names(d));
                const std::string before = d.files[static_cast<std::size_t>(i)].text;
                const cir::SessionPins pins = cir::readLayout(host_.chatSchematicPins(file));
                const cir::Applied a = cir::apply(before, cir::reconcile(before, pins), ops, pins);
                if (!a.error.empty())
                    return fail(a.error + ". Nothing was changed.");
                d.files[static_cast<std::size_t>(i)].text = a.text;
                d.active = i;
                if (circuitWriteHook_)
                    circuitWriteHook_(attempt);
                if (!host_.chatSetDraftIf(d, { { file, before } }))
                    continue;
                // What changed, in words: each command's line, the netlist's lines (its drawing aside), what the
                // circuit compiler says of the new text, and the text itself when it is short. Its version, as a write's:
                // edit_file's `expected` takes it, and one from before this edit is told the change was Claude's own.
                std::string version;
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    version = keepVersion(file, a.text);
                }
                std::string out = "Edited " + file + ": " + std::to_string(ops.size()) + (ops.size() == 1 ? " command" : " commands")
                                + ", one step the user can undo; version " + version + ".\n";
                for (const std::string& said : a.said)
                    out += "- " + said + "\n";
                const std::string was = cir::stripLayout(before), now = cir::stripLayout(a.text);
                const std::vector<std::string_view> la = textdiff::lines(was), lb = textdiff::lines(now);
                std::string changed;
                int shown = 0, more = 0;
                for (const textdiff::Edit& e : textdiff::diff(la, lb))
                {
                    if (e.op == textdiff::Op::same)
                        continue;
                    if (shown >= 60)
                    {
                        ++more;
                        continue;
                    }
                    changed += (e.op == textdiff::Op::removed ? "- " + std::string(la[e.a]) : "+ " + std::string(lb[e.b])) + "\n";
                    ++shown;
                }
                out += changed.empty() ? std::string("The netlist's lines are as they were (only the drawing moved).\n")
                                       : "The netlist's lines changed (the drawing's *@ lines aside):\n" + changed
                                             + (more > 0 ? "(" + std::to_string(more) + " more)\n" : std::string());
                {
                    const Sources one{ { file, a.text } };
                    circuits::Generator gen(one);
                    gen.module(file);
                    const std::string said = gen.errors() + gen.warnings();
                    out += said.empty() ? std::string("The circuit compiler on the new text: no error, no warning.\n")
                                        : "The circuit compiler on the new text:\n" + capped(said)
                                              + (said.back() == '\n' ? "" : "\n");
                }
                if (a.text.size() <= kCircuitTextBytes)
                {
                    out += "Its whole text now:\n" + a.text + (a.text.empty() || a.text.back() == '\n' ? "" : "\n");
                    const std::lock_guard<std::mutex> lock(mutex_);
                    markSeen(file, a.text);
                }
                else
                {
                    out += "(" + std::to_string(a.text.size()) + " bytes: read_file shows it whole.)\n";
                    // Every line it changed is listed above: seen as an edit_file is, only if the text it edited was
                    // (its drawing's *@ lines aside, which no state sends).
                    if (more == 0)
                    {
                        const std::lock_guard<std::mutex> lock(mutex_);
                        carrySeen(file, before, a.text);
                    }
                }
                note("drew " + file + " (" + std::to_string(ops.size()) + (ops.size() == 1 ? " change)" : " changes)"));
                return out;
            }
            return fail(file + " changed while drawing: try again");
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
                    if (!mcpWorking_.load())       // Claude Desktop's rename: the chat's Claude is sent the file anew
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
            // The dials as the program that played declared them, and their values: listed again only when the new one's
            // differ, with what that did to the values.
            const Controls before = host_.chatDials();
            const std::array<std::vector<std::string>, kMaxControls> beforeOptions = host_.chatDialOptions();
            const std::string declared = dialDeclarations(before, beforeOptions);
            std::array<float, kMaxControls> was{};
            for (int c = 0; c < kMaxControls; ++c)
                was[static_cast<std::size_t>(c)] = before[static_cast<std::size_t>(c)].used ? host_.chatDialValue(c) : 0.0f;
            const CompileStatus s = host_.chatRun(d.files);
            if (s.ok)
            {
                int dials = 0;
                for (const Control& c : s.controls)
                    dials += c.used ? 1 : 0;
                const Reported told = reported(s.notes);
                note("ran it: compiled in " + number(std::round(s.ms)) + " ms, " + std::to_string(dials) + " dial"
                     + (dials == 1 ? "" : "s")
                     + (told.count > 0 ? ", " + std::to_string(told.count) + " warning" + (told.count == 1 ? "" : "s") : ""));
                const std::string checks = evaluateChecks();
                const bool same = dialDeclarations(s.controls, s.panel.options) == declared;
                std::vector<measure::Check> set;
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    set = checks_;
                }
                return "Compiled in " + number(std::round(s.ms)) + " ms; it plays now.\n"
                     + (same ? "Its dials: unchanged (" + std::to_string(dials) + ").\n"
                             : "Its dials:\n" + dialLines(s.controls, host_)
                                   + carriedLines(before, beforeOptions, was, s.controls, s.panel.options, host_)
                                   + orphanLines(set, before, s.controls))
                     + panelLine(s.panel, s.controls) + capped(told.warnings)
                     + (told.notes.empty() ? "" : "Notes:\n" + capped(told.notes)) + (checks.empty() ? "" : "\n" + checks);
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
                const std::string said = dialText(k, v, host_.chatDialOptions()[static_cast<std::size_t>(c)]);
                note("set " + std::string(k.name) + " to " + said);
                return std::string(k.name) + " is set to " + said + (said == number(v) ? "" : " (" + number(v) + ")")
                     + " (its range " + number(k.min) + " to " + number(k.max) + ").";
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
            std::string why;
            measure::Request req = scenario::renderOf(input, &why);
            bool defaults = false;
            if (why.empty() && scenario::dialsFrom(input, defaults, why) && !defaults)
                req.current = currentDials();
            if (const juce::var sp = input.getProperty("spectrum", juce::var()); why.empty() && !sp.isVoid() && !sp.isUndefined())
            {
                const juce::String kind = sp.toString();
                req.spectrum = !sp.isString()     ? measure::Bands::none
                             : kind == "octave"   ? measure::Bands::octave
                             : kind == "third"    ? measure::Bands::third
                             : kind == "semitone" ? measure::Bands::semitone
                                                  : measure::Bands::none;
                if (req.spectrum == measure::Bands::none)
                    why = "spectrum is octave, third or semitone: the bands' width.";
            }
            Sources files = host_.chatDraft().files;   // a copy: the trial edits (ChatScenario.h) go there
            int trials = 0;
            if (!why.empty() || !scenario::resolve(req, host_, why) || !scenario::tryEdits(input, files, trials, why))
                return fail(why);
            const std::string report = measure::run(files, req);
            note(report.rfind("It did not compile", 0) == 0 ? "measured the sound: it did not compile"
                                                             : "measured the sound");
            return scenario::trialLine(trials) + scenario::dialsLine(defaults && !req.fromCapture) + report;
        }
        if (name == "set_checks")
        {
            const juce::var list = input.getProperty("checks", juce::var());
            const juce::var modeVar = input.getProperty("mode", juce::var());
            const std::string mode = modeVar.isVoid() || modeVar.isUndefined() ? "replace" : modeVar.toString().toStdString();
            if (mode != "replace" && mode != "merge" && mode != "remove")
                return fail("mode is replace, merge or remove. Nothing was changed.");
            std::vector<measure::Check> was;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                was = checks_;
            }
            // Without checks: the list, each check as set_checks takes it (what the session keeps), numbered as merge
            // and remove take an index.
            if (list.isVoid() || list.isUndefined())
            {
                if (mode != "replace")
                    return fail("set_checks' " + mode + " needs checks: the ones to " + mode + ". Nothing was changed.");
                if (was.empty())
                    return "No checks are set.";
                std::string out = std::to_string(was.size()) + (was.size() == 1 ? " check is set" : " checks are set")
                                + ", each as set_checks takes it, numbered as merge and remove take index:\n";
                for (std::size_t i = 0; i < was.size(); ++i)
                    out += std::to_string(i + 1) + ". " + claude::toJson(checkVar(was[i], true)) + "\n";
                return out;
            }
            const auto* arr = list.getArray();
            if (arr == nullptr)
                return fail("set_checks' checks is a list of them (empty to clear them; left out, it lists them)");
            if (mode != "replace" && arr->isEmpty())
                return fail("set_checks' " + mode + " needs checks: the ones to " + mode + ". Nothing was changed.");
            if (arr->size() > kMaxChecks)
                return fail("At most " + std::to_string(kMaxChecks) + " checks");
            measure::DialsFrom from = measure::DialsFrom::current;
            if (!dialsFromOf(input.getProperty("dials_from", juce::var()), from))
                return fail("dials_from is current, snapshot or defaults. Nothing was changed.");
            // The dials now, for a snapshot: read once, when a check needs them.
            std::map<std::string, float> now;
            bool read = false;
            const auto dialsNow = [&]() -> const std::map<std::string, float>& {
                if (!std::exchange(read, true))
                    now = currentDials();
                return now;
            };
            // An item as a check (replace and merge), or why not; `words`, how it takes the dials it does not name. An
            // item's own dials_from (and, from a snapshot, its snapshot), as the list gives it, stands for the call's: a
            // listed check given back is the same check (the listing names every check's, current too). `overridden`:
            // an item's own stood for another the call gave, which the result says.
            const juce::var callFrom = input.getProperty("dials_from", juce::var());
            const bool callGave = !(callFrom.isVoid() || callFrom.isUndefined());
            bool overridden = false;
            constexpr const char* kWhenMeasured = "as they are when measured";
            const auto checkOf = [&](int i, measure::Check& k, std::string& words) -> std::string {
                const juce::var& c = (*arr)[i];
                k.dialsFrom = from;
                if (const juce::var own = c.getProperty("dials_from", juce::var()); !(own.isVoid() || own.isUndefined()))
                    if (!dialsFromOf(own, k.dialsFrom))
                        return "Check " + std::to_string(i + 1) + ": dials_from is current, snapshot or defaults. Nothing was changed.";
                overridden = overridden || (callGave && k.dialsFrom != from);
                words = k.dialsFrom == measure::DialsFrom::defaults ? "at their defaults" : kWhenMeasured;
                if (k.dialsFrom == measure::DialsFrom::snapshot)
                {
                    if (const auto* shot = c.getProperty("snapshot", juce::var()).getDynamicObject())
                    {
                        for (const auto& kv : shot->getProperties())
                            if (isNumber(kv.value) || kv.value.isBool())
                                k.snapshot[kv.name.toString().toStdString()] = static_cast<float>(static_cast<double>(kv.value));
                        words = "as their snapshot gives them";
                    }
                    else
                    {
                        k.snapshot = dialsNow();
                        words = "as they are now";
                    }
                }
                std::string why;
                if (!goalOf(c, k.goal, why))
                    return "Check " + std::to_string(i + 1) + ": " + why + " Nothing was changed.";
                k.label = str(c, "label");
                if (k.label.empty())
                    k.label = k.goal.metric;
                k.request = scenario::renderOf(c, &why);
                if (!why.empty())
                    return "Check " + std::to_string(i + 1) + ": " + why + " Nothing was changed.";
                if (k.request.fromCapture)                   // gone after a reload: it could never pass again
                    return "Check " + std::to_string(i + 1) + ": a check cannot play the recording (\"capture\"), which "
                           "the plugin does not keep: give it a phrase or an input of its own. Nothing was changed.";
                return {};
            };
            const auto named = [](std::size_t at, const measure::Check& k) {
                return "check " + std::to_string(at + 1) + " (\"" + k.label + "\")";
            };
            const auto sameLabel = [](const juce::String& a, const std::string& b) {
                return a.equalsIgnoreCase(juce::String::fromUTF8(b.c_str()));
            };
            std::vector<measure::Check> checks;
            std::string did;                                 // merge and remove: what changed, in words
            std::map<std::size_t, std::string> sources;      // replace, merge: each check set (by its place), its words
            if (mode == "replace")
                for (int i = 0; i < arr->size(); ++i)
                {
                    measure::Check k;
                    std::string words;
                    if (const std::string why = checkOf(i, k, words); !why.empty())
                        return fail(why);
                    sources[checks.size()] = words;
                    checks.push_back(std::move(k));
                }
            else if (mode == "merge")
            {
                // Each item in turn: it replaces the check of its index or label (its metric when it has none), or is
                // added at the end. One it replaces under another label is named with both: a stale index shows.
                checks = was;
                for (int i = 0; i < arr->size(); ++i)
                {
                    const juce::var& item = (*arr)[i];
                    std::string why;
                    const std::string label = str(item, "label");
                    const int at = checkAt(checks, item, juce::String::fromUTF8((label.empty() ? str(item, "metric") : label).c_str()), why);
                    if (at == -2)
                        return fail("Check " + std::to_string(i + 1) + ": " + why + ". Nothing was changed.");
                    measure::Check k;
                    std::string words;
                    if (const std::string bad = checkOf(i, k, words); !bad.empty())
                        return fail(bad);
                    if (at < 0 && static_cast<int>(checks.size()) >= kMaxChecks)
                        return fail("At most " + std::to_string(kMaxChecks) + " checks: remove one first. Nothing was changed.");
                    const std::size_t to = at < 0 ? checks.size() : static_cast<std::size_t>(at);
                    if (at >= 0 && !sameLabel(juce::String::fromUTF8(k.label.c_str()), checks[to].label))
                        did += std::string(did.empty() ? "" : ", ") + "check " + std::to_string(to + 1) + " (\"" + checks[to].label
                             + "\" -> \"" + k.label + "\") replaced";
                    else
                        did += (did.empty() ? "" : ", ") + named(to, k) + (at < 0 ? " added" : " replaced");
                    sources[to] = words;                     // a check merged twice takes the last item's way
                    if (at < 0)
                        checks.push_back(std::move(k));
                    else
                        checks[to] = std::move(k);
                }
            }
            else
            {
                // The checks the items name, in the list as it was; then those removed. An index whose check has
                // another label than the item gives is refused: the list moved since it was read.
                std::set<int> gone;
                for (int i = 0; i < arr->size(); ++i)
                {
                    std::string why;
                    const juce::String label = juce::String::fromUTF8(str((*arr)[i], "label").c_str());
                    int at = checkAt(was, (*arr)[i], label, why);
                    if (at == -1)
                        why = label.isEmpty() ? std::string("give the check's label or its index")
                                              : "no check is labelled \"" + label.toStdString() + "\" (without checks, set_checks lists them)";
                    else if (at >= 0 && label.isNotEmpty() && !sameLabel(label, was[static_cast<std::size_t>(at)].label))
                    {
                        why = "check " + std::to_string(at + 1) + " is \"" + was[static_cast<std::size_t>(at)].label + "\", not \""
                            + label.toStdString() + "\" (without checks, set_checks lists them)";
                        at = -2;
                    }
                    if (at < 0)
                        return fail("Check " + std::to_string(i + 1) + ": " + why + ". Nothing was changed.");
                    gone.insert(at);
                }
                for (std::size_t i = 0; i < was.size(); ++i)
                    if (gone.count(static_cast<int>(i)) == 0)
                        checks.push_back(was[i]);
                    else
                        did += (did.empty() ? "" : ", ") + named(i, was[i]);
            }
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                checks_ = checks;
            }
            const std::string count = std::to_string(checks.size()) + " check" + (checks.size() == 1 ? "" : "s");
            if (checks.empty())
            {
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    checksFailing_ = false;
                }
                note("cleared the checks");
                return (did.empty() ? "" : "Removed " + did + ". ") + "The checks are cleared: nothing is measured after a run.";
            }
            // How the checks this call set take the dials they do not name, from each one's own (not the call's, which
            // an item's own stands for): nothing for the default alone; one way in words, or each with its checks.
            std::string fromWords;
            {
                std::vector<std::string> ways;               // in the order of each one's first check
                std::map<std::string, std::vector<std::size_t>> by;
                for (const auto& [at, words] : sources)
                {
                    if (by.count(words) == 0)
                        ways.push_back(words);
                    by[words].push_back(at);
                }
                if (ways.size() > 1 || (ways.size() == 1 && (ways[0] != kWhenMeasured || overridden)))
                    fromWords = ", the dials they do not name ";
                if (ways.size() == 1 && !fromWords.empty())
                    fromWords += ways[0];
                else if (ways.size() > 1)
                    for (std::size_t w = 0; w < ways.size(); ++w)
                    {
                        const std::vector<std::size_t>& ats = by[ways[w]];
                        fromWords += (w == 0 ? "" : "; ") + ways[w] + " (check" + (ats.size() == 1 ? " " : "s ");
                        for (std::size_t a = 0; a < ats.size(); ++a)
                            fromWords += (a == 0 ? "" : ", ") + std::to_string(ats[a] + 1);
                        fromWords += ")";
                    }
                if (overridden)
                    fromWords += " (an item's own dials_from stands for the call's)";
            }
            if (mode == "replace")
            {
                note("set " + count);
                return "Set " + count + fromWords
                     + ". The plugin measures them after every run_program and when you end your turn.\n" + evaluateChecks();
            }
            if (mode == "merge")
            {
                note("merged checks (" + count + " now)");
                return "Merged: " + did + fromWords + ". " + count + (checks.size() == 1 ? " is" : " are")
                     + " set; the plugin measures them after every run_program and when you end your turn.\n" + evaluateChecks();
            }
            const std::size_t removed = was.size() - checks.size();
            note("removed " + std::to_string(removed) + " check" + (removed == 1 ? "" : "s"));
            return "Removed " + did + ". " + count + " left.\n" + evaluateChecks();
        }
        if (name == "tune_dials")
        {
            measure::TuneRequest q;
            std::string unfit;
            q.render = scenario::renderOf(input, &unfit);
            q.render.current = currentDials();
            if (!unfit.empty() || !scenario::resolve(q.render, host_, unfit))
                return fail(unfit);
            if (const auto* tune = input.getProperty("tune", juce::var()).getArray())
                for (const auto& t : *tune)
                    if (t.isString())
                        q.dials.push_back(t.toString().toStdString());
            // A sweep (Tune.h): the one dial at each value, measured; no search, nothing set.
            const juce::var sweep = input.getProperty("sweep", juce::var());
            const bool sweeping = !sweep.isVoid() && !sweep.isUndefined();
            if (sweeping)
            {
                const auto* values = sweep.getArray();
                if (values == nullptr || values->isEmpty() || values->size() > measure::kMaxSweep)
                    return fail("sweep is a list of 1 to " + std::to_string(measure::kMaxSweep) + " values of the one dial in tune.");
                for (const auto& v : *values)
                {
                    if (!isNumber(v) && !v.isBool())
                        return fail("sweep's values are numbers: the dial's own (a bool's 0 or 1).");
                    q.sweep.push_back(static_cast<float>(static_cast<double>(v)));
                }
            }
            if (const auto* goals = input.getProperty("goals", juce::var()).getArray())
                for (int i = 0; i < goals->size(); ++i)
                {
                    measure::Goal g;
                    std::string why;
                    if (!goalOf((*goals)[i], g, why, sweeping))
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
            setActivity(sweeping ? "Sweeping a dial" : "Tuning the dials");
            q.progress = [this, sweeping](int done, int all) {
                setActivity((sweeping ? "Sweeping: " : "Tuning: ") + std::to_string(done) + " of " + std::to_string(all) + " renders");
                return !cancel_.load();
            };
            const measure::TuneResult t = measure::tune(host_.chatDraft().files, q);
            if (!t.error.empty())
            {
                note(sweeping ? "sweeping failed" : "tuning failed");
                return fail(t.error);
            }
            if (sweeping)
            {
                note("swept " + q.dials.front() + " over " + std::to_string(t.sweep.size()) + " value" + (t.sweep.size() == 1 ? "" : "s"));
                return t.text + "No dial was set: a sweep only measures (set_dial sets one).";
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
                     + (lineCount(d.files[i].text) == 1 ? " line" : " lines") + (static_cast<int>(i) == d.active ? ", shown" : "") + ")";
            out += "\nWhat plays: ";
            out += cir::sameProgram(running, d.files) ? "these files\n"
                                                      : "an earlier version (the files were edited since the run)\n";
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
