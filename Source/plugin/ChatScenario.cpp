// Source/plugin/ChatScenario.cpp: see ChatScenario.h.
#include "plugin/ChatScenario.h"

#include "plugin/ClaudeApi.h"

#include "Product.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace playground::scenario
{
    namespace
    {
        constexpr bool kInstrument = product::kIsInstrument;
        constexpr int kMaxTrialEdits = 32;
        constexpr double kMinRate = 8000.0, kMaxRate = 192000.0;
        constexpr const char* kInputs[] = { "sweep", "noise", "impulse", "sine", "silence" };   // ViewInput's order
        constexpr const char* kWheels[] = { "mod", "bend", "pressure", "sustain" };            // measure::Wheel's

        bool isNumber(const juce::var& v) { return v.isDouble() || v.isInt() || v.isInt64(); }

        std::string str(const juce::var& v, const char* key) { return v.getProperty(key, "").toString().toStdString(); }

        std::string number(double v)
        {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%g", v);
            return buf;
        }

        // Reads one part of a scenario; `fail` says why it cannot be (and the read stops).
        class Reader
        {
        public:
            explicit Reader(std::string& why) : why_(why) {}

            bool fail(const std::string& where, const std::string& what)
            {
                why_ = "scenario" + (where.empty() ? std::string() : "." + where) + ": " + what;
                return false;
            }

            // The keys of `o` are among `allowed`, else why.
            bool keys(const juce::var& o, const std::string& where, std::initializer_list<const char*> allowed)
            {
                const auto* d = o.getDynamicObject();
                if (d == nullptr)
                    return fail(where, "an object");
                for (const auto& kv : d->getProperties())
                {
                    const std::string k = kv.name.toString().toStdString();
                    if (std::none_of(allowed.begin(), allowed.end(), [&](const char* a) { return k == a; }))
                    {
                        std::string list;
                        for (const char* a : allowed)
                            list += (list.empty() ? "" : ", ") + std::string(a);
                        return fail(where, "it has no '" + k + "' (it takes " + list + ")");
                    }
                }
                return true;
            }

            // A number at `key` within [lo, hi] (absent: `def`, or a failure when there is none).
            bool num(const juce::var& o, const char* key, const std::string& where, double lo, double hi, double& out,
                     bool required = false)
            {
                const juce::var v = o.getProperty(key, juce::var());
                if (v.isVoid() || v.isUndefined())
                    return required ? fail(where, std::string(key) + " is needed") : true;
                if (!isNumber(v) && !v.isBool())
                    return fail(where, std::string(key) + " is a number");
                const double x = static_cast<double>(v);
                if (!std::isfinite(x) || x < lo || x > hi)
                    return fail(where, std::string(key) + " is " + number(lo) + " to " + number(hi));
                out = x;
                return true;
            }

        private:
            std::string& why_;
        };

        bool readPhrase(const juce::var& v, measure::Request& r, Reader& in)
        {
            if (v.toString() == "capture" && v.isString())
            {
                r.fromCapture = true;
                return true;
            }
            const auto* arr = v.getArray();
            if (arr == nullptr)
                return in.fail("phrase", "a list of notes, [{note, at, len, vel}], or \"capture\"");
            if (static_cast<std::size_t>(arr->size()) > measure::kMaxPhrase)
                return in.fail("phrase", "at most " + std::to_string(measure::kMaxPhrase) + " notes");
            for (int i = 0; i < arr->size(); ++i)
            {
                const juce::var& n = (*arr)[i];
                const std::string where = "phrase[" + std::to_string(i) + "]";
                double note = 48.0, at = 0.0, len = 0.5, vel = 0.8;
                if (!in.keys(n, where, { "note", "at", "len", "vel" }) || !in.num(n, "note", where, 0, 127, note, true)
                    || !in.num(n, "at", where, 0.0, measure::kMaxPlaySeconds, at)
                    || !in.num(n, "len", where, 0.0001, 3600.0, len) || !in.num(n, "vel", where, 0.0, 1.0, vel))
                    return false;
                r.phrase.push_back({ static_cast<int>(std::lround(note)), at, len, static_cast<float>(vel) });
            }
            return true;
        }

        bool readAutomate(const juce::var& v, measure::Request& r, Reader& in)
        {
            const auto* arr = v.getArray();
            if (arr == nullptr)
                return in.fail("automate", "a list of moves, [{dial, at, to, over}]");
            if (static_cast<std::size_t>(arr->size()) > measure::kMaxAutomate)
                return in.fail("automate", "at most " + std::to_string(measure::kMaxAutomate) + " moves");
            for (int i = 0; i < arr->size(); ++i)
            {
                const juce::var& m = (*arr)[i];
                const std::string where = "automate[" + std::to_string(i) + "]";
                double at = 0.0, to = 0.0, over = 0.0;
                if (!in.keys(m, where, { "dial", "at", "to", "over" }))
                    return false;
                const std::string dial = str(m, "dial");
                if (dial.empty())
                    return in.fail(where, "dial (a parameter's name) is needed");
                if (!in.num(m, "at", where, 0.0, measure::kMaxPlaySeconds, at) || !in.num(m, "to", where, -1e9, 1e9, to, true)
                    || !in.num(m, "over", where, 0.0, measure::kMaxPlaySeconds, over))
                    return false;
                r.automate.push_back({ dial, at, static_cast<float>(to), over });
            }
            return true;
        }

        bool readWheels(const juce::var& v, measure::Request& r, Reader& in)
        {
            const auto* arr = v.getArray();
            if (arr == nullptr)
                return in.fail("wheels", "a list, [{wheel, at, value}]");
            if (static_cast<std::size_t>(arr->size()) > measure::kMaxWheels)
                return in.fail("wheels", "at most " + std::to_string(measure::kMaxWheels) + " moves");
            for (int i = 0; i < arr->size(); ++i)
            {
                const juce::var& w = (*arr)[i];
                const std::string where = "wheels[" + std::to_string(i) + "]";
                if (!in.keys(w, where, { "wheel", "at", "value" }))
                    return false;
                const std::string name = str(w, "wheel");
                int kind = -1;
                for (int k = 0; k < 4; ++k)
                    if (name == kWheels[k])
                        kind = k;
                if (kind < 0)
                    return in.fail(where, "wheel is mod, bend, pressure or sustain");
                double at = 0.0, value = 0.0;
                if (!in.num(w, "at", where, 0.0, measure::kMaxPlaySeconds, at)
                    || !in.num(w, "value", where, kind == 1 ? -1.0 : 0.0, 1.0, value, true))
                    return false;
                r.wheels.push_back({ static_cast<measure::Wheel>(kind), at, static_cast<float>(value) });
            }
            return true;
        }

        bool readHost(const juce::var& h, measure::Request& r, Reader& in)
        {
            if (!in.keys(h, "host", { "bpm", "stop_at", "jump_to_beat", "jump_at" }))
                return false;
            double bpm = r.bpm, stop = -1.0, beat = 0.0, jump = -1.0;
            if (!in.num(h, "bpm", "host", 20.0, 400.0, bpm) || !in.num(h, "stop_at", "host", 0.0, measure::kMaxPlaySeconds, stop)
                || !in.num(h, "jump_to_beat", "host", -1e6, 1e6, beat) || !in.num(h, "jump_at", "host", 0.0, measure::kMaxPlaySeconds, jump))
                return false;
            if (h.hasProperty("jump_to_beat") && jump < 0.0)
                jump = 0.0;                                  // the song from there
            r.bpm = static_cast<float>(bpm);
            r.hostEdits = { stop, jump, beat };
            return true;
        }
    }

    juce::var schema()
    {
        return claude::object({ { "type", "object" }, { "description", "How it is played (CHECKING YOUR WORK)" } });
    }

    bool read(const juce::var& input, measure::Request& request, std::string* whyOut)
    {
        const juce::var sc = input.getProperty("scenario", juce::var());
        if (sc.isVoid() || sc.isUndefined())
            return true;
        std::string why;
        Reader in(why);
        measure::Request r = request;
        const bool ok = [&] {
            if (!in.keys(sc, "", { "phrase", "automate", "wheels", "host", "rate", "block", "input", "from", "to" }))
                return false;
            if (!kInstrument && (sc.hasProperty("phrase") || sc.hasProperty("wheels")))
                return in.fail(sc.hasProperty("phrase") ? "phrase" : "wheels", "an effect plays no notes (an instrument's)");
            if (kInstrument && sc.hasProperty("input"))
                return in.fail("input", "an effect's (an instrument plays its phrase)");
            if (sc.hasProperty("phrase") && !readPhrase(sc["phrase"], r, in))
                return false;
            if (sc.hasProperty("automate") && !readAutomate(sc["automate"], r, in))
                return false;
            if (sc.hasProperty("wheels") && !readWheels(sc["wheels"], r, in))
                return false;
            if (sc.hasProperty("host") && !readHost(sc["host"], r, in))
                return false;
            if (const juce::var rate = sc["rate"]; rate.isString() && rate.toString() == "host")
                r.hostRate = true;
            else if (sc.hasProperty("rate"))
            {
                double x = 48000.0;
                if (!in.num(sc, "rate", "rate", kMinRate, kMaxRate, x))
                    return false;
                r.rate = x;
            }
            if (const juce::var block = sc["block"]; block.isString() && block.toString() == "host")
                r.hostBlock = true;
            else if (sc.hasProperty("block"))
            {
                double x = measure::kDefaultBlock;
                if (!in.num(sc, "block", "block", 1.0, measure::kMaxBlock, x))
                    return false;
                r.block = static_cast<int>(std::lround(x));
            }
            if (sc.hasProperty("input"))
            {
                const std::string name = sc["input"].toString().toStdString();
                if (name == "capture")
                    r.fromCapture = true;
                else
                {
                    r.signal = -1;
                    for (int k = 0; k < 5; ++k)
                        if (name == kInputs[k])
                            r.signal = k;
                    if (r.signal < 0)
                        return in.fail("input", "sweep, noise, impulse, sine, silence or \"capture\"");
                }
            }
            if (sc.hasProperty("from") || sc.hasProperty("to"))
            {
                double from = 0.0, to = -1.0;
                if (!r.fromCapture)
                    return in.fail(sc.hasProperty("from") ? "from" : "to", std::string("a part of the recording: with ")
                                                                         + (kInstrument ? "phrase" : "input") + " \"capture\"");
                if (!in.num(sc, "from", "from", 0.0, 3600.0, from) || !in.num(sc, "to", "to", 0.0, 3600.0, to))
                    return false;
                if (to >= 0.0 && to <= from)
                    return in.fail("to", "after from");
                r.captureFrom = from;
                r.captureTo = to;
            }
            return true;
        }();
        if (!ok)
        {
            if (whyOut != nullptr)
                *whyOut = why;
            return false;
        }
        request = std::move(r);
        return true;
    }

    juce::var toVar(const measure::Request& r)
    {
        juce::var out(new juce::DynamicObject());
        auto* o = out.getDynamicObject();
        if (kInstrument && r.fromCapture)
            o->setProperty("phrase", "capture");
        else if (!r.phrase.empty())
        {
            juce::Array<juce::var> notes;
            for (const measure::PhraseNote& n : r.phrase)
                notes.add(claude::object({ { "note", n.note }, { "at", n.at }, { "len", n.length }, { "vel", n.velocity } }));
            o->setProperty("phrase", notes);
        }
        if (!r.automate.empty())
        {
            juce::Array<juce::var> moves;
            for (const measure::DialMove& m : r.automate)
                moves.add(claude::object({ { "dial", juce::String::fromUTF8(m.dial.c_str()) }, { "at", m.at }, { "to", m.to }, { "over", m.over } }));
            o->setProperty("automate", moves);
        }
        if (!r.wheels.empty())
        {
            juce::Array<juce::var> wheels;
            for (const measure::WheelMove& w : r.wheels)
                wheels.add(claude::object({ { "wheel", kWheels[static_cast<int>(w.wheel)] }, { "at", w.at }, { "value", w.value } }));
            o->setProperty("wheels", wheels);
        }
        if (r.hostEdits.stopAt >= 0.0 || r.hostEdits.jumpAt >= 0.0)
        {
            juce::var h(new juce::DynamicObject());
            if (r.hostEdits.stopAt >= 0.0)
                h.getDynamicObject()->setProperty("stop_at", r.hostEdits.stopAt);
            if (r.hostEdits.jumpAt >= 0.0)
            {
                h.getDynamicObject()->setProperty("jump_to_beat", r.hostEdits.jumpToBeat);
                h.getDynamicObject()->setProperty("jump_at", r.hostEdits.jumpAt);
            }
            o->setProperty("host", h);
        }
        if (r.hostRate)
            o->setProperty("rate", "host");
        else if (r.rate != 48000.0)
            o->setProperty("rate", r.rate);
        if (r.hostBlock)
            o->setProperty("block", "host");
        else if (r.block != measure::kDefaultBlock)
            o->setProperty("block", r.block);
        if (!kInstrument && r.fromCapture)
            o->setProperty("input", "capture");
        if (r.fromCapture && r.captureFrom > 0.0)
            o->setProperty("from", r.captureFrom);
        if (r.fromCapture && r.captureTo >= 0.0)
            o->setProperty("to", r.captureTo);
        else if (!kInstrument && r.signal >= 0 && r.signal < 5)
            o->setProperty("input", kInputs[r.signal]);
        return o->getProperties().isEmpty() ? juce::var() : out;
    }

    bool resolve(measure::Request& r, ChatHost& host, std::string& why)
    {
        if (r.hostRate)
            if (const double rate = host.chatHostRate(); rate > 1000.0)
                r.rate = std::clamp(rate, kMinRate, kMaxRate);
        if (r.hostBlock)
            if (const int block = host.chatHostBlock(); block > 0)
                r.block = std::clamp(block, 1, measure::kMaxBlock);
        if (r.fromCapture)
        {
            Captured c;
            if (!captured(host, c, why, r.captureFrom, r.captureTo))
                return false;
            const measure::Request& k = c.request;
            r.rate = k.rate;
            r.block = k.block;
            r.blocks = k.blocks;
            r.events = k.events;
            r.transport = k.transport;
            r.input = k.input;
            r.frames = k.frames;                             // its dials, clock and parts: a replay gives what played
            r.frameControls = k.frameControls;
            r.clock = k.clock;
            r.part = k.part;
            r.recording = k.recording;
            r.modwheel = k.modwheel;
            r.pitchbend = k.pitchbend;
            for (const auto& [name, v] : k.current)          // its dials at its start, for the report's line
                r.current[name] = v;
            r.phrase.clear();
            std::vector<measure::DialMove> moves = k.automate;   // the recording's, then the request's own
            moves.insert(moves.end(), r.automate.begin(), r.automate.end());
            r.automate = std::move(moves);
        }
        return true;
    }

    std::vector<measure::CheckResult> evaluate(const Sources& files, const std::vector<measure::Check>& checks,
                                               const std::map<std::string, float>& current, ChatHost& host, std::string* errors)
    {
        std::vector<measure::CheckResult> out(checks.size());
        std::vector<measure::Check> ready;
        std::vector<int> at(checks.size(), -1);
        for (std::size_t i = 0; i < checks.size(); ++i)
        {
            measure::Check k = checks[i];
            std::string why;
            if (!resolve(k.request, host, why))
            {
                out[i].text = "not measured: " + why;
                continue;
            }
            at[i] = static_cast<int>(ready.size());
            ready.push_back(std::move(k));
        }
        const std::vector<measure::CheckResult> measured = measure::evaluate(files, ready, current, errors);
        for (std::size_t i = 0; i < checks.size(); ++i)
            if (at[i] >= 0 && static_cast<std::size_t>(at[i]) < measured.size())
                out[i] = measured[static_cast<std::size_t>(at[i])];
        return out;
    }

    // ---- try_edits ------------------------------------------------------------------------------------------------------
    juce::var trialSchema()
    {
        return claude::object({ { "type", "array" }, { "description", "Trial edits on a copy (CHECKING YOUR WORK)" } });
    }

    bool tryEdits(const juce::var& input, Sources& files, int& count, std::string& why)
    {
        count = 0;
        const juce::var edits = input.getProperty("try_edits", juce::var());
        if (edits.isVoid() || edits.isUndefined())
            return true;
        const auto* arr = edits.getArray();
        if (arr == nullptr)
        {
            why = "try_edits is a list: [{file, old_text, new_text}] or [{name, content}]";
            return false;
        }
        if (arr->size() > kMaxTrialEdits)
        {
            why = "try_edits takes at most " + std::to_string(kMaxTrialEdits) + " edits";
            return false;
        }
        const auto names = [&] {
            std::string out;
            for (const SourceFile& f : files)
                out += (out.empty() ? "" : ", ") + f.name;
            return out;
        };
        for (int i = 0; i < arr->size(); ++i)
        {
            const juce::var& e = (*arr)[i];
            const std::string label = "Trial edit " + std::to_string(i + 1);
            if (e.hasProperty("old_text") || e.hasProperty("file"))
            {
                const std::string file = str(e, "file");
                const juce::var oldV = e.getProperty("old_text", juce::var()), newV = e.getProperty("new_text", juce::var());
                if (file.empty() || !oldV.isString() || !newV.isString())
                {
                    why = label + ": it needs file, old_text and new_text";
                    return false;
                }
                const std::string oldText = oldV.toString().toStdString(), newText = newV.toString().toStdString();
                auto f = std::find_if(files.begin(), files.end(), [&](const SourceFile& x) { return x.name == file; });
                if (f == files.end())
                {
                    why = label + ": there is no file " + file + " (the files: " + names() + ")";
                    return false;
                }
                if (oldText.empty())
                {
                    why = label + " (" + file + "): old_text is empty: give the exact passage to replace";
                    return false;
                }
                std::size_t found = 0, where = std::string::npos;     // edit_file's rule: exactly once
                for (std::size_t p = f->text.find(oldText); p != std::string::npos; p = f->text.find(oldText, p + 1))
                {
                    if (found == 0)
                        where = p;
                    ++found;
                }
                if (found != 1)
                {
                    why = label + " (" + file + "): old_text " + (found == 0 ? std::string("does not occur in it (check whitespace and the "
                                                                                           "latest text: read_file shows it)")
                                                                             : "occurs " + std::to_string(found)
                                                                                   + " times: include more of the surrounding lines");
                    return false;
                }
                f->text.replace(where, oldText.size(), newText);
            }
            else if (e.hasProperty("content"))
            {
                const std::string name = str(e, "name");
                const juce::var content = e.getProperty("content", juce::var());
                if (!validFileName(name) || !content.isString())
                {
                    why = label + ": a whole file needs a name (letters, digits, _, - and .) and its content";
                    return false;
                }
                auto f = std::find_if(files.begin(), files.end(), [&](const SourceFile& x) { return x.name == name; });
                if (f != files.end())
                    f->text = content.toString().toStdString();
                else if (files.size() >= kMaxFiles)
                {
                    why = label + ": the program holds at most " + std::to_string(kMaxFiles) + " files";
                    return false;
                }
                else
                    files.push_back({ name, content.toString().toStdString() });
            }
            else
            {
                why = label + ": give file, old_text and new_text (a passage), or name and content (a whole file)";
                return false;
            }
            ++count;
        }
        return true;
    }

    std::string trialLine(int count)
    {
        if (count <= 0)
            return {};
        return "With " + std::to_string(count) + (count == 1 ? " trial edit" : " trial edits") + "; nothing in the plugin changed.\n";
    }
}
