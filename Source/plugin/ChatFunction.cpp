// Source/plugin/ChatFunction.cpp: the chat's run_function (ChatAgent.h): one of the program's functions called by name
// with Claude's arguments, through FMinus's call by signature (fminus/Patch.h, "one function called by name"), and its
// result, or what went wrong, in words. Never on the audio thread and never through the program that plays: the call
// runs on the tool's thread in a compile of its own (the loop budget and the bounds record are a Program's, which the
// audio thread's calls would otherwise share), either of the editor's files on a fresh instance ("fresh"), or of the
// program that plays on a copy of its state, which the audio thread makes at a block's end (Engine::Snapshot,
// ChatHost::chatSnapshot: "playing"); what plays is never written. It changes nothing in the plugin and writes no file.
#include "plugin/ChatAgent.h"

#include "Product.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#if defined(_WIN32)
#include <malloc.h>                                  // _aligned_malloc
#endif

namespace playground
{
    namespace
    {
        constexpr bool kInstrument = product::kIsInstrument;
        constexpr const char* kModule = kInstrument ? "Voice" : "Effect";
        constexpr std::size_t kShownElements = 512;       // an array's elements shown at most
        constexpr std::size_t kMaxOutput = 12000;

        bool isNumber(const juce::var& v) { return v.isDouble() || v.isInt() || v.isInt64(); }

        // One argument of Claude's as Patch::call takes it ("" or why it cannot be one).
        std::string argOf(const juce::var& v, std::size_t k, Patch::CallArg& out)
        {
            const std::string which = "args[" + std::to_string(k) + "]";
            const auto elements = [&](const juce::var& list) -> std::string {
                const juce::Array<juce::var>* a = list.getArray();
                if (a == nullptr)
                    return which + "'s values must be an array of numbers";
                if (static_cast<std::size_t>(a->size()) > Patch::kMaxCallElements)
                    return which + " has more than " + std::to_string(Patch::kMaxCallElements) + " elements";
                for (int i = 0; i < a->size(); ++i)
                {
                    const juce::var& e = a->getReference(i);
                    if (e.isArray())
                        return which + " is an array of arrays: give the elements in one array, row after row, as C lays a "
                                       "2-D array out";
                    if (!isNumber(e) && !e.isBool())
                        return which + "[" + std::to_string(i) + "] is no number";
                    out.elements.push_back(static_cast<double>(e));
                }
                return {};
            };
            if (v.isBool())
            {
                out.kind = Patch::CallArg::Kind::boolean;
                out.number = static_cast<bool>(v) ? 1.0 : 0.0;
                return {};
            }
            if (isNumber(v))
            {
                out.kind = Patch::CallArg::Kind::number;
                out.number = static_cast<double>(v);
                return std::isfinite(out.number) ? std::string() : which + " is not a finite number";
            }
            if (v.isArray())
            {
                out.kind = Patch::CallArg::Kind::array;
                return elements(v);
            }
            if (const juce::DynamicObject* o = v.getDynamicObject(); o != nullptr && !v.isArray())
            {
                // {extent, values}: a buffer of extent elements, the values first and zeros after.
                out.kind = Patch::CallArg::Kind::array;
                for (const auto& p : o->getProperties())
                    if (p.name.toString() != "extent" && p.name.toString() != "values")
                        return which + " has '" + p.name.toString().toStdString() + "': an array is [numbers] or {extent, values}";
                const juce::var e = o->getProperty("extent");
                if (!e.isVoid())
                {
                    if (!isNumber(e) || static_cast<double>(e) < 0.0 || static_cast<double>(e) != std::floor(static_cast<double>(e))
                        || static_cast<double>(e) > static_cast<double>(Patch::kMaxCallElements))
                        return which + "'s extent is a whole number of elements, at most " + std::to_string(Patch::kMaxCallElements);
                    out.extent = static_cast<std::size_t>(static_cast<double>(e));
                }
                const juce::var values = o->getProperty("values");
                if (!values.isVoid())
                    if (const std::string why = elements(values); !why.empty())
                        return why;
                if (out.extent == 0 && out.elements.empty())
                    return which + " is an empty array: give its extent";
                return {};
            }
            return which + " is no number, bool or array";
        }

        std::string number(double v)
        {
            char b[40];
            std::snprintf(b, sizeof b, "%.9g", v);
            return b;
        }

        // An array as the result shows it: its elements, kShownElements at most, then how many more and their range.
        std::string listed(const std::vector<double>& e)
        {
            std::string out = "[";
            const std::size_t shown = std::min(e.size(), kShownElements);
            for (std::size_t i = 0; i < shown; ++i)
                out += (i == 0 ? "" : ", ") + number(e[i]);
            if (shown < e.size())
            {
                double lo = e[shown], hi = e[shown];
                for (std::size_t i = shown; i < e.size(); ++i)
                {
                    lo = std::min(lo, e[i]);
                    hi = std::max(hi, e[i]);
                }
                out += ", ... " + std::to_string(e.size() - shown) + " more, from " + number(lo) + " to " + number(hi);
            }
            return out + "]";
        }

        // An aligned block for the copy of the state that plays (Engine::Snapshot's buffers), freed with it. Written
        // whole here, on the asker's thread: a large block comes from the kernel untouched, and the audio thread's copy
        // into it would otherwise take its page faults.
        struct Aligned
        {
            void* p = nullptr;
            Aligned(std::size_t align, std::size_t bytes)
            {
                if (bytes == 0)
                    return;
                const std::size_t a = std::max(align, alignof(std::max_align_t));
                const std::size_t n = (bytes + a - 1) / a * a;
#if defined(_WIN32)
                p = _aligned_malloc(n, a);
#else
                p = std::aligned_alloc(a, n);
#endif
                if (p != nullptr)
                    std::memset(p, 0, n);
            }
            ~Aligned()
            {
#if defined(_WIN32)
                _aligned_free(p);
#else
                std::free(p);
#endif
            }
            Aligned(const Aligned&) = delete;
            Aligned& operator=(const Aligned&) = delete;
        };
    }

    juce::var ChatAgent::functionDefinition()
    {
        juce::Array<juce::var> on;
        on.add("fresh");
        on.add("playing");
        return claude::object(
            { { "name", "run_function" },
              { "description",
                juce::String("Call one of the program's functions by name with arguments and get its result.\n\n"
                "It runs in a compile of its own, off the audio thread, changes nothing that plays and writes no file. name: a function (softclip), a method of the main "
                "module or of Shared (") + kModule + "::level, Shared::build), a method of a module the main module holds, by "
                "the field's path (filter.tick, osc[2].env.next, shared->table.read), or another module's (Svf::tick: on an "
                "instance of its own, zeroed, its params at their defaults). args: its arguments in order, as declared: a "
                "number for a float, a double or an int (whole), true or false for a bool, and for an int*, float* or double* "
                "an array, [0.5, 1] or {extent: 512, values: [1]} (the values first, then zeros), which the result gives back "
                "as the function left it. Pass a buffer's length too, as C does: each array then holds at least that many "
                "elements, then zeros. on: fresh (the default: the editor's files compiled now, a new instance, the dials as "
                "they are, init run) or playing (the program that plays, on a copy of its state the audio thread makes at a "
                "block's end: the effect's instance, or the newest sounding voice's, with Shared and Host; what plays is never "
                "written). playing is refused while no voice sounds, and when the state holds a pointer that is not null (a "
                "block init allocated: the copy would share it). The loop budget and the bounds checks apply: a loop that "
                "never ends stops after about a second, saying so, and an index past an array names the access. The result "
                "says what it ran on, what it returned, each array after the call and the fields a method changed. To call "
                "several methods in a row on one instance (start, then process), write a method that calls them, and call "
                "that." },
              { "eager_input_streaming", true },
              { "input_schema",
                claude::object({ { "type", "object" },
                                 { "properties",
                                   claude::object({ { "name", claude::object({ { "type", "string" },
                                                                               { "description", "A function, Module::method, or "
                                                                                                "a field's method (filter.tick)" } }) },
                                                    { "args", claude::object({ { "type", "array" },
                                                                               { "description", "In order: numbers, true or false, "
                                                                                                "arrays" } }) },
                                                    { "on", claude::object({ { "enum", on } }) } }) },
                                 { "required", juce::Array<juce::var>{ "name" } } }) } });
    }

    std::string ChatAgent::runFunction(const juce::var& input, bool& isError)
    {
        const auto fail = [&](std::string why) {
            isError = true;
            const std::lock_guard<std::mutex> lock(mutex_);
            addEntry(ChatEntry::Kind::tool, "called a function: " + why.substr(0, why.find('\n')));
            return why;
        };
        const juce::var nameVar = input.getProperty("name", juce::var());
        if (!nameVar.isString() || nameVar.toString().isEmpty())
            return fail("run_function needs the function's name");
        const std::string name = nameVar.toString().toStdString();
        const std::string on = input.getProperty("on", "fresh").toString().toStdString();
        if (on != "fresh" && on != "playing")
            return fail("on is fresh or playing, not '" + on + "'");
        const bool playing = on == "playing";
        std::vector<Patch::CallArg> args;
        const juce::var argsVar = input.getProperty("args", juce::var());
        if (!argsVar.isVoid())
        {
            const juce::Array<juce::var>* list = argsVar.getArray();
            if (list == nullptr)
                return fail("args is a list of the arguments, in order");
            for (int k = 0; k < list->size(); ++k)
            {
                Patch::CallArg a;
                if (const std::string why = argOf(list->getReference(k), static_cast<std::size_t>(k), a); !why.empty())
                    return fail(why);
                args.push_back(std::move(a));
            }
        }
        setActivity("Calling " + name);

        // The files: the editor's, or what plays (with the configuration its netlists were generated with, so that the
        // compile is laid out as the program that plays is), all of one compile's status: `program`, what the copy
        // must be of.
        const std::string changed = "The program that plays changed as it was copied (a newer run was on its way in, or its "
                                    "state is laid out otherwise): call it again.";
        Sources files;
        CircuitPins pins;
        uint64_t program = 0;
        if (playing)
        {
            const CompileStatus st = host_.chatLastStatus();
            files = host_.chatRunning();
            if (files.empty())
                return fail("Nothing plays yet: run_program plays the editor's files (or call it on: fresh).");
            if (host_.chatLastStatus().revision != st.revision)
                return fail(changed);
            pins = st.circuits;
            program = st.program;
        }
        else
            files = host_.chatDraft().files;
        const double hostRate = host_.chatHostRate();
        const float rate = hostRate > 0.0 ? static_cast<float>(hostRate) : 48000.0f;
        Patch::Result compiled = Patch::compile(files, rate, false, playing && !pins.empty() ? &pins : nullptr);
        if (!compiled.patch)
            return fail(std::string(playing ? "The program that plays" : "The editor's files") + " did not compile here, so nothing "
                        "was called:\n" + compiled.errors.substr(0, kMaxOutput));
        Patch& patch = *compiled.patch;

        // What a method runs on: with on: playing, a copy of the state that plays, asked of the audio thread only once
        // the call is checked and runs on that state (Patch::call's onState): a function, an instance of its own or a
        // call that is wrong anyway costs the audio thread nothing.
        std::string where, refusal;
        std::function<std::string()> onState;
        if (playing)
        {
            onState = [&]() -> std::string {
                // A copy of the state that plays, made by the audio thread at a block's end (Engine::Snapshot).
                const Patch::Layout l = patch.layout();
                if (l.instance + l.shared + l.host > Engine::kMaxSnapshot)
                    return refusal = "The state that plays is " + std::to_string((l.instance + l.shared + l.host) >> 10) + " KB, past "
                                     "the " + std::to_string(Engine::kMaxSnapshot >> 20) + " MB the audio thread copies: call it on: fresh.";
                Aligned instance(l.instanceAlign, l.instance), shared(l.sharedAlign, l.shared), hostBlock(l.hostAlign, l.host);
                if (instance.p == nullptr || (l.shared > 0 && shared.p == nullptr) || (l.host > 0 && hostBlock.p == nullptr))
                    return refusal = "No memory for a copy of the state that plays.";
                Engine::Snapshot s;
                s.instance = instance.p;
                s.shared = shared.p;
                s.host = hostBlock.p;
                s.instanceBytes = l.instance;
                s.sharedBytes = l.shared;
                s.hostBytes = l.host;
                std::string why;
                if (!host_.chatSnapshot(s, why))
                    return refusal = "Nothing was called: " + why + ". on: fresh runs it on a new instance.";
                switch (s.status)
                {
                    case Engine::Snapshot::Status::copied:
                        break;
                    case Engine::Snapshot::Status::noProgram:
                        return refusal = "Nothing plays now, so there is no state to copy: on: fresh runs it on a new instance.";
                    case Engine::Snapshot::Status::noVoice:
                        return refusal = "No voice sounds now, so there is no voice's state to copy: hold a note and call it "
                                         "again, or on: fresh.";
                    case Engine::Snapshot::Status::otherLayout:
                    case Engine::Snapshot::Status::waiting:
                        return refusal = changed;
                }
                // Of the program whose files were compiled here (a run the audio thread has not taken yet, or one it
                // took since, is another program, though laid out alike: its state under this code fits neither).
                if (s.layout != l.hash || s.program != program)
                    return refusal = changed;
                if (const std::string refused = patch.adopt(s.instance, s.shared, s.host); !refused.empty())
                    return refusal = "Not on a copy of what plays: " + refused + ". on: fresh runs it on a new instance.";
                where = kInstrument ? "a copy of the voice playing note " + std::to_string(s.key) + " (the newest sounding), made "
                                      "by the audio thread at a block's end"
                                    : std::string("a copy of the effect's state, made by the audio thread at a block's end");
                where += " (the program that plays, compiled again for the call; what plays is untouched)";
                return {};
            };
        }
        else
        {
            // A fresh instance of the editor's files: as compile() makes it (init run), with the dials as they are.
            const std::map<std::string, float> dials = currentDials();
            const Controls& cs = patch.controls();
            for (int c = 0; c < kMaxControls; ++c)
            {
                const Control& k = cs[static_cast<std::size_t>(c)];
                if (!k.used)
                    continue;
                if (const auto it = dials.find(std::string(k.name)); it != dials.end())
                    patch.setValue(c, it->second, 0);
            }
            char r[32];
            std::snprintf(r, sizeof r, "%.0f", static_cast<double>(rate));
            where = "a fresh " + std::string(kModule) + " of the editor's files (compiled now: the dials as they are, init(" + r
                  + ") run)";
        }

        const Patch::CallOutcome o = patch.call(name, args, onState);
        if (!refusal.empty())
            return fail(refusal);
        std::string text;
        using On = Patch::CallOutcome::On;
        const std::string ranOn = o.where == On::function ? std::string("a function: on its arguments, in ")
                                                              + (playing ? "the program that plays" : "the editor's files, compiled now")
                                : o.where == On::own   ? std::string(std::string("AEIOUaeiou").find(o.on.empty() ? 'x' : o.on[0])
                                                                             != std::string::npos ? "an " : "a ")
                                                             + o.on + " of its own (zeroed, its params at their defaults), in "
                                                           + (playing ? "the program that plays" : "the editor's files")
                                : o.where == On::shared ? "the " + o.on + " of " + where
                                : o.where == On::field  ? "its field " + o.on + ", in " + where
                                                        : where;
        if (!o.ran)
        {
            text = "Nothing was called: " + o.error + ".";
            if (!o.signature.empty() && o.error.find(o.signature) == std::string::npos)
                text += "\nIts declaration: " + o.signature;
            return fail(text);
        }
        text = "Ran " + o.signature + " on " + ranOn + ".\n";
        if (o.stopped)
            text += "It stopped: " + o.error + (o.error.empty() || o.error.back() != '?' ? "." : "") + " What follows is as it "
                    "left things.\n";
        else if (o.returned)
            text += "Returned " + o.value + ".\n";
        else
            text += "Returned nothing (void).\n";
        for (const Patch::CallOutcome::Array& a : o.arrays)
        {
            text += (a.name.empty() ? std::string("an array") : a.name) + " (" + a.type + ")" + (a.written ? ", after the call: " : ": ")
                  + listed(a.elements) + "\n";
            if (a.past)
                text += "  It wrote past the " + std::to_string(a.elements.size()) + " elements given, into the zeros after them: "
                        "give it more (extent), or check its loop's bounds.\n";
        }
        if (!o.changed.empty())
        {
            text += "Fields it changed:";
            for (const std::string& c : o.changed)
                text += "\n  " + c;
            if (o.moreChanged > 0)
                text += "\n  and " + std::to_string(o.moreChanged) + " more";
            text += "\n";
        }
        else if (o.where != On::function && o.where != On::own && !o.stopped)
            text += "It changed no field of the instance, Shared or Host that trace can read.\n";
        if (text.size() > kMaxOutput)
            text = text.substr(0, kMaxOutput) + "\n(cut)";
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            addEntry(ChatEntry::Kind::tool, "called " + name + (o.stopped ? ": it stopped" : o.returned ? ": " + o.value : std::string()));
        }
        if (o.stopped)
            isError = true;
        return text;
    }
}
