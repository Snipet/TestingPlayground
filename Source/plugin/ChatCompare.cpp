// Source/plugin/ChatCompare.cpp: the chat's compare (ChatAgent.h): two of the program's sources rendered offline
// alike and compared (dsp/Compare.h), read-only. A side is the editor's files, what plays, a saved version of the
// project open (or another project's), a preset of it or a dial setting over the editor's files, or the user's
// recording ("capture", ChatScenario.h); each renders with its own dials (a version's, a preset's) unless `dials` is
// "current". Its result is at most twelve lines, and a picture when asked (SoundPicture.h's drawPair).
#include "plugin/ChatAgent.h"

#include "plugin/ChatScenario.h"
#include "plugin/ProjectStore.h"
#include "plugin/SoundPicture.h"

#include "dsp/Compare.h"
#include "dsp/SoundView.h"

#include "Product.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace playground
{
    namespace
    {
        constexpr std::size_t kMaxOutput = 12000;

        std::string number(double v)
        {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%g", v);
            return buf;
        }

        std::string dateOf(int64_t ms) { return juce::Time(ms).formatted("%Y-%m-%d %H:%M").toStdString(); }

        // One side of the comparison.
        struct Side
        {
            std::string words;                    // what it is, in words
            Sources files;
            std::map<std::string, float> dials;   // its own (a version's, a preset's, those given)
            bool playing = false, capture = false;
        };

        // The project's kept versions, the newest first (at most twelve), for an error.
        std::string keptVersions(const projectstore::Project& p)
        {
            std::string out;
            int shown = 0;
            for (auto v = p.versions.rbegin(); v != p.versions.rend() && shown < 12; ++v, ++shown)
                out += (out.empty() ? "" : "; ") + std::to_string(v->number) + " (" + dateOf(v->savedMs) + ", " + v->note + ")";
            if (p.versions.size() > 12)
                out += "; and " + std::to_string(p.versions.size() - 12) + " older";
            return out;
        }

        std::map<std::string, float> dialsOf(const std::vector<projectstore::Dial>& ds)
        {
            std::map<std::string, float> out;
            for (const projectstore::Dial& d : ds)
                out[d.name] = d.value;
            return out;
        }

        bool sideOf(const juce::var& v, const char* which, ChatHost& host, Side& out, std::string& why)
        {
            const std::string forms = "editor, playing, version:N, project:Name@N, preset:Name, capture, or {\"dials\": {...}}";
            if (const auto* o = v.getDynamicObject(); o != nullptr)
            {
                const juce::var dials = v.getProperty("dials", juce::var());
                const auto* d = dials.getDynamicObject();
                if (d == nullptr || o->getProperties().size() != 1)
                {
                    why = std::string(which) + " as an object is {\"dials\": {name: value, ...}} over the editor's files";
                    return false;
                }
                for (const auto& kv : d->getProperties())
                    if (kv.value.isDouble() || kv.value.isInt() || kv.value.isInt64() || kv.value.isBool())
                        out.dials[kv.name.toString().toStdString()] = static_cast<float>(static_cast<double>(kv.value));
                out.files = host.chatDraft().files;
                std::string list;
                for (const auto& [name, x] : out.dials)
                    list += (list.empty() ? "" : ", ") + name + " " + number(x);
                out.words = "the editor's files with " + (list.empty() ? std::string("its dials") : list);
                return true;
            }
            const std::string s = v.toString().toStdString();
            if (s == "editor")
            {
                out.files = host.chatDraft().files;
                out.words = "the editor's files";
                return true;
            }
            if (s == "playing")
            {
                out.files = host.chatRunning();
                out.playing = true;
                out.words = "the program that plays";
                if (out.files.empty())
                {
                    why = "Nothing plays yet (run_program plays the editor's files).";
                    return false;
                }
                return true;
            }
            if (s == "capture")
            {
                out.capture = true;
                out.words = "the user's recording (what played)";
                return true;
            }
            const auto version = [&](const std::string& project, const std::string& n) {
                std::string loadWhy;
                const std::optional<projectstore::Project> p = projectstore::load(project, &loadWhy);
                if (!p)
                {
                    why = "No project " + project + " (" + loadWhy + ").";
                    return false;
                }
                const projectstore::Version* ver = nullptr;
                if (n.empty())
                    ver = &p->latest();
                else
                {
                    const int k = juce::String(n).getIntValue();
                    ver = juce::String(n).containsOnly("0123456789") ? p->version(k) : nullptr;
                }
                if (ver == nullptr)
                {
                    why = "The project " + p->name + " keeps no version " + n + ". Kept: " + keptVersions(*p) + ".";
                    return false;
                }
                out.files = ver->files;
                out.dials = dialsOf(ver->dials);
                out.words = "version " + std::to_string(ver->number) + " of " + p->name + " (" + dateOf(ver->savedMs) + ", "
                          + ver->note + ")";
                return true;
            };
            if (s.rfind("version:", 0) == 0)
            {
                const std::string project = host.chatProject();
                if (project.empty())
                {
                    why = std::string(which) + " is " + s + ", but no project is open in the editor (PROJECTS): name one, as "
                          "project:Name@N.";
                    return false;
                }
                return version(project, s.substr(8));
            }
            if (s.rfind("project:", 0) == 0)
            {
                const std::string rest = s.substr(8);
                const std::size_t at = rest.rfind('@');
                return version(at == std::string::npos ? rest : rest.substr(0, at), at == std::string::npos ? "" : rest.substr(at + 1));
            }
            if (s.rfind("preset:", 0) == 0)
            {
                const std::string project = host.chatProject(), name = s.substr(7);
                if (project.empty())
                {
                    why = std::string(which) + " is " + s + ", but no project is open in the editor (its presets are a project's).";
                    return false;
                }
                std::string loadWhy;
                const std::optional<projectstore::Project> p = projectstore::load(project, &loadWhy);
                const projectstore::Preset* pr = p ? p->preset(name) : nullptr;
                if (pr == nullptr)
                {
                    std::string have;
                    if (p)
                        for (const projectstore::Preset& x : p->presets)
                            have += (have.empty() ? "" : ", ") + x.name;
                    why = "The project " + project + " has no preset " + name + (p ? ". Its presets: " + (have.empty() ? std::string("none") : have)
                                                                                  : " (" + loadWhy + ")")
                        + ".";
                    return false;
                }
                out.files = host.chatDraft().files;
                out.dials = dialsOf(pr->dials);
                out.words = "the preset " + pr->name + " (of " + p->name + ") over the editor's files";
                return true;
            }
            why = std::string(which) + " is one of: " + forms + ".";
            return false;
        }

        // The difference's level per picture column (10 ms of RMS at each column's middle), dBFS; -240 where none.
        std::vector<float> differenceLevel(const measure::Sound& a, const measure::Sound& b, int columns)
        {
            std::vector<float> out(static_cast<std::size_t>(columns), -240.0f);
            const std::size_t n = std::min(a.l.size(), b.l.size());
            const std::size_t w = std::max<std::size_t>(1, static_cast<std::size_t>(0.01 * a.rate));
            for (int c = 0; c < columns && n > 0; ++c)
            {
                const std::size_t mid = n * (2 * static_cast<std::size_t>(c) + 1) / (2 * static_cast<std::size_t>(columns));
                const std::size_t from = mid > w / 2 ? mid - w / 2 : 0, to = std::min(n, from + w);
                double e = 0.0;
                for (std::size_t i = from; i < to; ++i)
                {
                    const double dl = static_cast<double>(a.l[i]) - b.l[i], dr = static_cast<double>(a.r[i]) - b.r[i];
                    e += 0.5 * (dl * dl + dr * dr);
                }
                const double rms = to > from ? std::sqrt(e / static_cast<double>(to - from)) : 0.0;
                out[static_cast<std::size_t>(c)] = rms > 1e-12 ? static_cast<float>(20.0 * std::log10(rms)) : -240.0f;
            }
            return out;
        }
    }

    juce::var ChatAgent::compareDefinition()
    {
        juce::var props = claude::object(
            { { "a", claude::object({ { "description", "editor, playing, version:N, project:Name@N, preset:Name, capture or "
                                                       "{dials}" } }) },
              { "b", claude::object({ { "description", "Like a" } }) },
              { "scenario", scenario::schema() },
              { "try_edits", scenario::trialSchema() },
              { "dials", claude::object({ { "enum", juce::Array<juce::var>{ "own", "current" } } }) },
              { "picture", claude::object({ { "type", "boolean" } }) },
              { "detail", claude::object({ { "type", "boolean" } }) } });
        juce::var schema = claude::object({ { "type", "object" }, { "properties", props } });
        schema.getDynamicObject()->setProperty("required", juce::Array<juce::var>{ "a", "b" });
        return claude::object(
            { { "name", "compare" },
              { "description",
                "Compare two versions of the sound rendered offline alike: identical bits, or how and where they differ "
                "in level, delay, bands, metrics and code. It changes nothing.\n\n"
                "a, b: playing is the running program's own code (not the frozen copy it plays once its dials hold still); "
                "version:N is the open project's (one not kept lists those kept); project:Name@N, @N left out its newest; "
                "preset:Name and {dials: {...}} lay dials over the editor's files; capture is the user's recording. dials: "
                "own (each side's saved values, the default) or current (the plugin's). An effect's input: the scenario's, "
                "else noise. try_edits apply to b; detail adds the difference over time; picture draws both (about 1,000 "
                "tokens)." },
              { "eager_input_streaming", true },
              { "input_schema", schema } });
    }

    juce::var ChatAgent::compareTool(const juce::var& use)
    {
        const juce::var input = use.getProperty("input", juce::var());
        juce::var block = claude::object({ { "type", "tool_result" }, { "tool_use_id", use.getProperty("id", "") } });
        const auto done = [&](const std::string& text, bool error) {
            const std::string t = text.size() > kMaxOutput ? text.substr(0, kMaxOutput) + "\n(cut)" : text;
            block.getDynamicObject()->setProperty("content", juce::String::fromUTF8(t.c_str()));
            if (error)
                block.getDynamicObject()->setProperty("is_error", true);
            const std::lock_guard<std::mutex> lock(mutex_);
            addEntry(ChatEntry::Kind::tool, error ? "compared two sounds: " + t.substr(0, t.find('\n')) : "compared two sounds");
            return block;
        };
        setActivity("Comparing the sounds");
        Side a, b;
        std::string why;
        if (!sideOf(input.getProperty("a", juce::var()), "a", host_, a, why) || !sideOf(input.getProperty("b", juce::var()), "b", host_, b, why))
            return done(why, true);
        if (a.capture && b.capture)
            return done("a and b are both the recording: compare it with a program (editor, playing, version:N, ...).", true);
        measure::Request req;
        if (!scenario::read(input, req, &why))
            return done(why, true);
        scenario::Captured recording;
        if (a.capture || b.capture)
        {
            if (!scenario::captured(host_, recording, why, req.captureFrom, req.captureTo))
                return done((a.capture ? "a" : "b") + std::string(" is the recording, but ") + why + ".", true);
            req.fromCapture = true;
        }
        if (!scenario::resolve(req, host_, why))
            return done(why, true);
        if (a.capture || b.capture)
            req.tailSeconds = 0.0f;                          // as long as the recording, no more
        const bool current = input.getProperty("dials", juce::var()).toString() == "current";
        measure::CompareSide A, B;
        const std::map<std::string, float> now = currentDials();
        for (const auto& [side, from] : { std::pair<measure::CompareSide*, Side*>{ &A, &a }, { &B, &b } })
        {
            side->files = from->files;
            side->request = req;
            side->request.current = now;
            if (!current)
                side->request.dials = from->dials;
            if (from->capture)
            {
                side->recordedL = &recording.l;
                side->recordedR = &recording.r;
            }
        }
        int trials = 0;
        if (input.hasProperty("try_edits") && b.capture)
            return done("try_edits apply to b, which is the recording: put the program in b.", true);
        if (!scenario::tryEdits(input, B.files, trials, why))
            return done(why, true);
        measure::CompareOptions options;
        options.detail = static_cast<bool>(input.getProperty("detail", false));
        const measure::Comparison c = measure::compare(A, B, options);
        if (!c.error.empty())
            return done(scenario::trialLine(trials) + c.error, true);

        const measure::Sound& rendered = a.capture ? c.b : c.a;
        std::string out = scenario::trialLine(trials);
        out += "Compared offline (nothing that plays changed): A is " + a.words + "; B is " + b.words + (trials > 0 ? " with the trial edits" : "")
             + ". Both: " + rendered.title + ", at " + number(c.rate) + " Hz in blocks of "
             + (req.blocks.empty() ? std::to_string(req.block) : std::string("the recording's sizes")) + ".\n";
        if (a.playing || b.playing)
            out += "(\"playing\" is the running program's own code; once its dials hold still the plugin plays a frozen copy "
                   "whose last bits differ, which this does not render.)\n";
        if ((a.capture || b.capture) && rendered.title.find(recording.about) == std::string::npos)
            out += "(The recording: " + recording.about + ")\n";
        for (const std::string& line : c.lines)
            out += line + "\n";
        if (!a.capture && !b.capture)
        {
            std::string dials;
            int moved = 0;
            for (const auto& [name, x] : c.b.values)
            {
                const auto o = std::find_if(c.a.values.begin(), c.a.values.end(), [&](const auto& p) { return p.first == name; });
                if (o == c.a.values.end() || o->second == x)
                    continue;
                if (++moved <= 6)
                    dials += (dials.empty() ? "" : ", ") + name + " " + number(o->second) + " -> " + number(x);
            }
            if (moved > 6)
                dials += ", and " + std::to_string(moved - 6) + " more";
            std::string files = "the same files";
            if (!(A.files == B.files))
            {
                projectstore::Version before;
                before.files = A.files;
                files = projectstore::describeChange(&before, B.files, {});
            }
            out += "Source, B against A: " + files + "; " + (dials.empty() ? std::string("the same dials") : "dials " + dials) + ".\n";
        }
        if (!static_cast<bool>(input.getProperty("picture", false)))
            return done(out, false);

        // The picture: both levels over time and both spectra, on one scale.
        setActivity("Drawing the comparison");
        const measure::ViewRequest shape = soundpicture::request();
        const measure::View va = measure::analyse(c.a.l, c.a.r, c.rate, c.a.heldUntil, shape, c.a.cycleHz, false, c.a.fromStart);
        const measure::View vb = measure::analyse(c.b.l, c.b.r, c.rate, c.b.heldUntil, shape, c.b.cycleHz, false, c.b.fromStart);
        const std::vector<double> bands(c.bandsDb.begin(), c.bandsDb.end());
        const soundpicture::Drawn d = soundpicture::drawPair(
            va, vb, differenceLevel(c.a, c.b, shape.columns), bands, "A: " + a.words + "   B: " + b.words,
            "The picture: the levels over time (RMS over 10 ms, dBFS, A blue, B orange) and their difference's level in pink "
            "(no line where they are the same), then both average spectra on one scale (dB under the louder), and the octave "
            "bands' differences along its foot.");
        juce::Array<juce::var> content;
        content.add(claude::object({ { "type", "image" },
                                     { "source", claude::object({ { "type", "base64" },
                                                                  { "media_type", "image/png" },
                                                                  { "data", juce::Base64::toBase64(d.png.data(), d.png.size()) } }) } }));
        content.add(claude::object({ { "type", "text" }, { "text", juce::String::fromUTF8((out + d.about).c_str()) } }));
        block.getDynamicObject()->setProperty("content", content);
        const std::lock_guard<std::mutex> lock(mutex_);
        addEntry(ChatEntry::Kind::tool, "compared two sounds");
        if (!a.capture && !b.capture)                        // the user's recording: no thumbnail kept in the session
            entries_.back().pictures.push_back(d.thumb);
        touch();
        return block;
    }
}
