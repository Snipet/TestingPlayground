// Source/plugin/ChatScenario.cpp: see ChatScenario.h.
#include "plugin/ChatScenario.h"

#include "plugin/ClaudeApi.h"

#include "Product.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <set>

namespace playground::scenario
{
    namespace
    {
        constexpr bool kInstrument = product::kIsInstrument;
        constexpr int kMaxTrialEdits = 32;
        constexpr const char* kInputs[] = { "sweep", "noise", "impulse", "sine", "silence" };   // ViewInput's order
        constexpr const char* kWheels[] = { "mod", "bend", "pressure", "sustain", "steering", "throttle", "brake", "clutch",
                                            "gear", "buttons" };                            // measure::Wheel's
        static_assert(std::size(kWheels) == static_cast<std::size_t>(measure::kWheelKinds));

        bool isNumber(const juce::var& v) { return v.isDouble() || v.isInt() || v.isInt64(); }

        std::string str(const juce::var& v, const char* key) { return v.getProperty(key, "").toString().toStdString(); }

        std::string number(double v)
        {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%g", v);
            return buf;
        }

        // What a time past a render's cap adds (Measure.h, "How long a render may be"): a picture's names the numbers'.
        std::string capWords(const measure::Request& r)
        {
            return r.longest < measure::kMaxLongSeconds
                       ? " (a picture's render: measure_output, a check, tune_dials, trace and compare without its picture "
                         "take up to " + number(measure::kMaxLongSeconds) + " s)"
                       : " (a render that returns numbers)";
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

            // A time at `key`, in seconds from the render's start: 0 to the request's longest (Measure.h).
            bool time(const juce::var& o, const char* key, const std::string& where, const measure::Request& r, double& out)
            {
                const juce::var v = o.getProperty(key, juce::var());
                if (isNumber(v) && std::isfinite(static_cast<double>(v)) && static_cast<double>(v) > r.longest)
                    return fail(where, std::string(key) + " is 0 to " + number(r.longest) + " s" + capWords(r));
                return num(o, key, where, 0.0, r.longest, out);
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
                    || !in.time(n, "at", where, r, at)
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
                if (!in.time(m, "at", where, r, at) || !in.num(m, "to", where, -1e9, 1e9, to, true)
                    || !in.time(m, "over", where, r, over))
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
                for (int k = 0; k < measure::kWheelKinds; ++k)
                    if (name == kWheels[k])
                        kind = k;
                if (kind < 0)
                    return in.fail(where, "wheel is mod, bend, pressure, sustain, steering, throttle, brake, clutch, gear or buttons");
                const auto wheel = static_cast<measure::Wheel>(kind);
                const bool signedOne = wheel == measure::Wheel::bend || wheel == measure::Wheel::steering;
                const bool gear = wheel == measure::Wheel::gear, buttons = wheel == measure::Wheel::buttons;
                double at = 0.0, value = 0.0;
                if (!in.time(w, "at", where, r, at)
                    || !in.num(w, "value", where, gear || signedOne ? -1.0 : 0.0,
                               gear ? 7.0 : buttons ? static_cast<double>(measure::kMaxButtons) : 1.0, value, true))
                    return false;
                if (buttons && value != std::floor(value))
                    return in.fail(where, "the buttons' value is a whole number, a bit a button (host->buttons)");
                r.wheels.push_back({ static_cast<measure::Wheel>(kind), at, static_cast<float>(value) });
            }
            return true;
        }

        bool readHost(const juce::var& h, measure::Request& r, Reader& in)
        {
            if (!in.keys(h, "host", { "bpm", "stop_at", "jump_to_beat", "jump_at" }))
                return false;
            double bpm = r.bpm, stop = -1.0, beat = 0.0, jump = -1.0;
            if (!in.num(h, "bpm", "host", 20.0, 400.0, bpm) || !in.time(h, "stop_at", "host", r, stop)
                || !in.num(h, "jump_to_beat", "host", -1e6, 1e6, beat) || !in.time(h, "jump_at", "host", r, jump))
                return false;
            if (h.hasProperty("jump_to_beat") && jump < 0.0)
                jump = 0.0;                                  // the song from there
            r.bpm = static_cast<float>(bpm);
            r.hostEdits = { stop, jump, beat };
            return true;
        }
    }

    measure::Request forNumbers()
    {
        measure::Request r;
        r.longest = measure::kMaxLongSeconds;
        return r;
    }

    measure::Request renderOf(const juce::var& input, std::string* why, measure::Request req)
    {
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
        // The times, refused past their caps (Measure.h, "How long a render may be"; read without why, a session's, as
        // they are: a render clamps them).
        const auto timeArg = [&](const char* k, float& out, double most, const std::string& words) {
            const juce::var v = input.getProperty(k, juce::var());
            if (!isNumber(v))
                return;
            if (static_cast<double>(v) > most + 1e-9 && why != nullptr)
                *why = std::string(k) + " is at most " + number(most) + " s" + words;
            else
                out = static_cast<float>(static_cast<double>(v));
        };
        numberArg("velocity", req.velocity);
        timeArg("hold_seconds", req.holdSeconds, req.longest, capWords(req));
        timeArg("tail_seconds", req.tailSeconds, measure::kMaxTailSeconds, " (after the notes, or the input, end)");
        numberArg("bpm", req.bpm);
        numberArg("modwheel", req.modwheel);
        numberArg("pitchbend", req.pitchbend);
        const juce::var dials = input.getProperty("dials", juce::var());
        if (const auto* o = dials.getDynamicObject())
            for (const auto& kv : o->getProperties())
                if (isNumber(kv.value) || kv.value.isBool())
                    req.dials[kv.name.toString().toStdString()] = static_cast<float>(static_cast<double>(kv.value));
        if (std::string bad; !readWindow(input, req.window, bad, req.endSeconds()) && why != nullptr)
            *why = bad;
        // off_order_db's base, an instrument's (an effect's orders are its sine's, 220 Hz).
        if (const juce::var base = input.getProperty("base_hz", juce::var()); kInstrument && !base.isVoid() && !base.isUndefined())
        {
            if (isNumber(base) && static_cast<double>(base) >= 1.0 && static_cast<double>(base) <= measure::kBandHighHz)
                req.baseHz = static_cast<double>(base);
            else if (why != nullptr)
                *why = "base_hz: the orders' base, 1 to 20000 Hz";
        }
        read(input, req, why);
        return req;
    }

    bool readWindow(const juce::var& input, measure::Window& window, std::string& why, double end)
    {
        const juce::var w = input.getProperty("window", juce::var());
        if (w.isVoid() || w.isUndefined())
            return true;
        const auto* o = w.getDynamicObject();
        const juce::var from = o != nullptr ? o->getProperty("from") : juce::var();
        const juce::var to = o != nullptr ? o->getProperty("to") : juce::var();
        const bool only = o != nullptr && std::all_of(o->getProperties().begin(), o->getProperties().end(), [](const auto& kv) {
            return kv.name.toString() == "from" || kv.name.toString() == "to";
        });
        const double a = isNumber(from) ? static_cast<double>(from) : -1.0, b = isNumber(to) ? static_cast<double>(to) : -1.0;
        // At least kMinWindowSeconds long, a rounding's worth under it allowed (0.03 - 0.02 is 0.00999...).
        if (!only || !(a >= 0.0) || !(b - a >= measure::kMinWindowSeconds - 1e-9) || !(b <= end))
        {
            why = "window: {from, to}, seconds of the render, 0 <= from < to <= " + number(end)
                + ", at least " + number(measure::kMinWindowSeconds) + " s long";
            return false;
        }
        window = { a, b };
        return true;
    }

    juce::var windowVar(const measure::Window& w)
    {
        return w.set() ? claude::object({ { "from", w.from }, { "to", w.to } }) : juce::var();
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

    bool fits(const measure::Request& r, std::string& why)
    {
        // Two decimals, rounded down for a cap (so that what it names fits) and to the nearest for what was asked.
        const auto down = [](double x) { return std::floor(x * 100.0 + 1e-6) / 100.0; };
        const auto near = [](double x) { return std::round(x * 100.0) / 100.0; };
        const double holds = measure::kMaxRenderSamples / r.rate;
        // Its times end by measure::longestAt its rate, where a Session would cut them (above 96 kHz: 145 s at 192 kHz).
        if (const double last = measure::latestTime(r), most = measure::longestAt(r.longest, r.rate); last > most + 1e-9)
        {
            why = "At " + number(r.rate) + " Hz a render's times (hold_seconds, the scenario's) end by " + number(down(most))
                + " s, so that it holds " + number(down(holds)) + " s with the tail (" + number(measure::kMaxLongSeconds) + " s and "
                + number(measure::kMaxRenderSamples / 96000.0) + " s up to 96000 Hz); this one's last is at " + number(near(last))
                + " s: render it shorter, or at a lower rate.";
            return false;
        }
        // A render holds measure::kMaxRenderSamples a channel at most: a long one at a high rate is refused.
        if (const double seconds = measure::renderSeconds(r); seconds * r.rate > measure::kMaxRenderSamples)
        {
            why = "The render would last " + number(near(seconds)) + " s at " + number(r.rate) + " Hz, past the "
                + number(down(holds)) + " s a render holds at that rate (" + number(measure::kMaxRenderSamples / 96000.0)
                + " s up to 96000 Hz): render it shorter, or at a lower rate.";
            return false;
        }
        return true;
    }

    // ---- dials_from ----------------------------------------------------------------------------------------------------
    juce::var dialsFromSchema(bool described)
    {
        juce::var p = claude::object({ { "enum", juce::Array<juce::var>{ "current", "defaults" } } });
        if (described)
            p.getDynamicObject()->setProperty("description", "Dials not named: current (the default) or defaults");
        return p;
    }

    bool dialsFrom(const juce::var& input, bool& defaults, std::string& why)
    {
        defaults = false;
        const juce::var v = input.getProperty("dials_from", juce::var());
        if (v.isVoid() || v.isUndefined() || (v.isString() && v.toString() == "current"))
            return true;
        if (v.isString() && v.toString() == "defaults")
        {
            defaults = true;
            return true;
        }
        why = "dials_from is current (the dials not named as they are now) or defaults (at the program's defaults).";
        return false;
    }

    std::string dialsLine(bool defaults) { return defaults ? "The dials not named are at their defaults.\n" : ""; }

    // ---- spectrum --------------------------------------------------------------------------------------------------
    bool spectrumOf(const juce::var& input, measure::Bands& bands, std::string& why)
    {
        const juce::var sp = input.getProperty("spectrum", juce::var());
        if (sp.isVoid() || sp.isUndefined())
            return true;
        const juce::String kind = sp.toString();
        bands = !sp.isString()     ? measure::Bands::none
              : kind == "octave"   ? measure::Bands::octave
              : kind == "third"    ? measure::Bands::third
              : kind == "semitone" ? measure::Bands::semitone
                                   : measure::Bands::none;
        if (bands != measure::Bands::none)
            return true;
        why = "spectrum is octave, third or semitone: the bands' width.";
        return false;
    }

    // ---- trace's fields, voice and export ----------------------------------------------------------------------------
    bool watchOf(const juce::var& input, measure::TraceRequest& q, std::string& why)
    {
        if (const juce::var fields = input.getProperty("fields", juce::var()); !fields.isVoid())
        {
            const juce::Array<juce::var>* list = fields.getArray();
            if (list == nullptr || list->size() > static_cast<int>(measure::kMaxTracePatterns))
            {
                why = "fields is a list of at most 8 paths or globs (\"filter.*\", \"osc[1].phase\"); list: true names the "
                      "program's fields.";
                return false;
            }
            for (const juce::var& f : *list)
                if (f.isString() && f.toString().isNotEmpty())
                    q.fields.push_back(f.toString().toStdString());
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
            {
                why = "voice is newest, all or a MIDI note (0 to 127).";
                return false;
            }
        }
        return true;
    }

    bool exportOf(const juce::var& in, ExportAsk& ask, std::string& why)
    {
        juce::var v = in.getProperty("export", juce::var());
        const auto off = [](const juce::var& x) { return x.isVoid() || x.isUndefined() || (x.isBool() && !static_cast<bool>(x)); };
        if (off(v))
            return true;
        if (v.isString() && off(v = juce::JSON::fromString(v.toString())) && !v.isVoid())   // any JSON value, "false" too
            return true;
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

    std::size_t everyOf(std::size_t asked, double rate)
    {
        const double r = rate > 0.0 ? rate : 48000.0;
        const std::size_t every = asked > 0 ? asked : 16 * static_cast<std::size_t>(std::max(1L, std::lround(r / 16000.0)));
        return std::max<std::size_t>(16, (every + 8) / 16 * 16);
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
