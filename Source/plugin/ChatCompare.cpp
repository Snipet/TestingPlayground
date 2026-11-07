// Source/plugin/ChatCompare.cpp: the chat's compare (ChatAgent.h): two of the program's sources rendered offline
// alike and compared (dsp/Compare.h), read-only. A side is the editor's files, what plays, what played before the last
// run that changed it (previous_run, ChatHost::chatPrevious), a saved version of the project open (or another
// project's), a preset of it or a dial setting over the editor's files, the user's recording ("capture",
// ChatScenario.h), or a target sound ("wav:": a WAV, AIFF or FLAC on this Mac by its path, or one of the program's
// samples by its name, read with plugin/AssetFiles.h and resampled to the render's rate; local files only: nothing is
// fetched, kept or written, and no thumbnail of it stays in the chat); each renders with its own dials (a version's, a
// preset's) over the plugin's, unless `dials` is
// "current" (the plugin's; a preset's and a {dials} side's still apply) or "defaults" (its own over the program's
// defaults, the plugin's aside). `b_rate` renders B at another rate than A (a rate check, both rates 40 kHz or more:
// Compare.h compares the levels, bands and metrics then, not samples; never a recording, whose events are at its own
// rate). Each side renders with its own samples (dsp/Assets.h): what plays and previous_run the ones they compiled
// with, a version its own (as opening it makes them), the others the plugin's as they are. Its result is at most
// fourteen lines (each side's render hash one of them; a target sound's spectra another), and a picture when asked
// (SoundPicture.h's drawPair).
#include "plugin/ChatAgent.h"

#include "plugin/AssetFiles.h"
#include "plugin/CaptureReplay.h"
#include "plugin/ChatScenario.h"
#include "plugin/ProjectStore.h"
#include "plugin/SoundPicture.h"

#include "dsp/Compare.h"
#include "dsp/Export.h"
#include "dsp/SoundView.h"
#include "fminus/CirDoc.h"

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
            bool given = false;                   // its dials are what it is ({dials}, preset:): "current" keeps them
            bool playing = false, capture = false;
            // A target sound (wav:): its file's sound, rendered against in place of a program.
            std::shared_ptr<const assets::Asset> target;
            // The samples it compiled with or keeps (what plays, previous_run, a version), in place of the plugin's
            // as they are now (scenario::resolve's): `own`.
            Assets samples;
            bool own = false;
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
            const std::string forms = "editor, playing, previous_run, version:N, project:Name@N, preset:Name, capture, wav:path, "
                                      "or {\"dials\": {...}}";
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
                out.given = true;
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
                out.files = host.chatPlaying(out.samples);
                out.own = true;
                out.playing = true;
                out.words = "the program that plays";
                if (out.files.empty())
                {
                    why = "Nothing plays yet (run_program plays the editor's files).";
                    return false;
                }
                return true;
            }
            if (s == "previous_run")
            {
                out.files = host.chatPrevious(out.samples);
                out.own = true;
                out.words = "what played before the last run";
                if (out.files.empty())
                {
                    why = "Nothing played before the last run: no run has changed the program since the plugin opened or "
                          "loaded its session (previous_run is the program a run replaced).";
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
            if (s.rfind("wav:", 0) == 0)
            {
                // A target sound: a file on this Mac by its whole path, or one of the program's samples by its name.
                std::string path = juce::String(s.substr(4)).trim().toStdString();
                if (path.find('/') == std::string::npos && path != "~")
                {
                    const Assets samples = host.chatAssets();
                    out.target = samples != nullptr ? samples->find(path) : nullptr;
                    if (out.target == nullptr)
                    {
                        std::string have;
                        if (samples != nullptr)
                            for (const auto& a : samples->items)
                                have += (have.empty() ? "" : ", ") + a->name();
                        why = std::string(which) + " is " + s + ": the program has no sample " + path + " ("
                            + (have.empty() ? std::string("it has none") : "its samples: " + have)
                            + "). A file on this Mac is wav: and its whole path (/Users/... or ~/...).";
                        return false;
                    }
                    out.words = "the target sound " + path + " (a sample of the program's: " + out.target->describe() + ")";
                    return true;
                }
                if (path.rfind("~/", 0) == 0)
                    path = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getFullPathName().toStdString() + path.substr(1);
                if (!juce::File::isAbsolutePath(juce::String::fromUTF8(path.c_str())))
                {
                    why = std::string(which) + " is " + s + ": wav: takes a file on this Mac, whole (/Users/... or ~/...), or a sample's name.";
                    return false;
                }
                std::string readWhy;
                out.target = assetfiles::read(nullptr, path, {}, readWhy);
                if (out.target == nullptr)
                {
                    why = std::string(which) + " is " + s + ", which cannot be read: " + readWhy + ".";
                    return false;
                }
                out.words = "the target sound " + juce::File(juce::String::fromUTF8(path.c_str())).getFileName().toStdString() + " ("
                          + out.target->describe() + ")";
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
                std::vector<std::string> refused;           // its own samples, as opening it would make them
                out.samples = host.chatVersionSamples(*p, *ver, refused);
                out.own = true;
                out.words = "version " + std::to_string(ver->number) + " of " + p->name + " (" + dateOf(ver->savedMs) + ", "
                          + ver->note + ")"
                          + (refused.empty() ? std::string() : "; left out of its samples: " + refused.front());
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
                out.given = true;
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
            { { "a", claude::object({ { "description", "editor, playing, previous_run, version:N, project:Name@N, "
                                                       "preset:Name, capture, wav:path or {dials}" } }) },
              { "b", claude::object({ { "description", "Like a" } }) },
              { "b_rate", claude::object({ { "type", "number" } }) },
              { "scenario", scenario::schema() },
              { "try_edits", scenario::trialSchema() },
              { "dials", claude::object({ { "enum", juce::Array<juce::var>{ "own", "current", "defaults" } } }) },
              { "picture", claude::object({ { "type", "boolean" } }) },
              { "detail", claude::object({ { "type", "boolean" } }) } });
        juce::var schema = claude::object({ { "type", "object" }, { "properties", props } });
        schema.getDynamicObject()->setProperty("required", juce::Array<juce::var>{ "a", "b" });
        return claude::object(
            { { "name", "compare" },
              { "description",
                "Compare two versions of the sound rendered offline alike: identical bits, or how and where they differ "
                "in level, delay, bands, metrics and code.\n\n"
                "It changes nothing. "
                "a, b: playing is the running program's own code (not the frozen copy it plays once its dials hold still); "
                "previous_run what played before the last run that changed it ({a: \"previous_run\", b: \"playing\"}: "
                "what that run changed); version:N is the open project's (one not kept lists those kept); project:Name@N, "
                "@N left out its newest; preset:Name and {dials: {...}} lay dials over the editor's files; capture is the "
                "user's recording; wav:path is a target sound, a WAV, AIFF or FLAC on this Mac by its whole path (one the user "
                "named or gave you; nothing else is read, and nothing of it is kept) or one of the program's samples by its "
                "name: the other side renders as long as it is (60 s at most, a picture's 10), at the render's rate (the file "
                "resampled), and the levels, the gain and lag that fit, the octave bands and both spectra's centroid say how "
                "near it comes (no metrics: the file is no render); an effect plays its scenario's input into it. dials: "
                "own (each side's saved values over the plugin's, the default), current (the plugin's; a preset's and "
                "{dials} still apply) or defaults (each side's own over the program's defaults). "
                "An effect's input: the scenario's, else noise. try_edits apply to b; detail adds the difference over "
                "time; picture draws both (about 1,000 tokens). b_rate renders b at another rate (40000 to 192000 Hz; "
                "both \"editor\" at 96000: does it sound the same?), compared by the levels, octave bands and metrics "
                "(those the rate moves by definition aside), not sample by sample; an effect's input then the sweep; "
                "never with the recording." },
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
        const bool target = a.target != nullptr || b.target != nullptr;
        if ((a.capture || a.target != nullptr) && (b.capture || b.target != nullptr))
            return done("Neither a nor b is a program: compare the recording or a target sound (wav:) with one (editor, "
                        "playing, version:N, ...).", true);
        if (input.hasProperty("window"))
            return done("compare takes no window: it compares the whole renders (measure_output, view_sound, a check and "
                        "tune_dials measure one).", true);
        // Its numbers alone render up to 300 s; with the picture, a picture's 10 s (Measure.h, "How long a render may be").
        const bool picture = static_cast<bool>(input.getProperty("picture", false));
        measure::Request req = picture ? measure::Request{} : scenario::forNumbers();
        if (!scenario::read(input, req, &why))
            return done(why, true);
        scenario::Captured recording;
        if (a.capture || b.capture)
        {
            if (!scenario::captured(host_, recording, why, req))
                return done((a.capture ? "a" : "b") + std::string(" is the recording, but ") + why + ".", true);
            req.fromCapture = true;
        }
        if (!scenario::resolve(req, host_, why, a.capture || b.capture ? &recording : nullptr))   // one recording for both
            return done(why, true);
        if (a.capture || b.capture)
            req.tailSeconds = 0.0f;                          // as long as the recording, no more
        // B at another rate (a rate check): both rates 40 kHz or more (Compare.h), and never with the recording, whose
        // events and blocks are at its own rate. b_rate at A's own rate is the ordinary comparison.
        double rateB = req.rate;
        if (const juce::var r = input.getProperty("b_rate", juce::var()); !r.isVoid() && !r.isUndefined())
        {
            if (!(r.isDouble() || r.isInt() || r.isInt64()) || !(static_cast<double>(r) >= scenario::kMinRate)
                || !(static_cast<double>(r) <= scenario::kMaxRate))
                return done("b_rate is B's rate in Hz, 8000 to 192000 (40000 or more when it differs from A's).", true);
            rateB = static_cast<double>(r);
        }
        if (rateB != req.rate)
        {
            if (std::min(rateB, req.rate) < measure::kMinRateAcross)
                return done("At two rates both are 40000 Hz or more, so that both hold 20 Hz to 20 kHz: A renders at "
                            + number(req.rate) + " Hz and b_rate is " + number(rateB) + ".", true);
            if (a.capture || b.capture || req.fromCapture)
                return done("b_rate takes no recording: what played, and its events, are at its own rate (compare two "
                            "programs, or the same one twice, at two rates).", true);
            if (target)
                return done("b_rate takes no target sound (wav:): it is compared at the render's rate, resampled to it.", true);
            if (req.signal == static_cast<int>(measure::ViewInput::noise) || req.signal == static_cast<int>(measure::ViewInput::impulse))
                return done("b_rate takes the sweep (its input at two rates), a sine or silence: white noise's and an "
                            "impulse's level per hertz change with the rate, so every band would differ.", true);
        }
        const juce::String which = input.getProperty("dials", juce::var()).toString();
        if (which.isNotEmpty() && which != "own" && which != "current" && which != "defaults")
            return done("dials is own (each side's saved values over the plugin's), current (the plugin's) or defaults (each "
                        "side's own over the program's defaults).", true);
        const bool current = which == "current", defaults = which == "defaults";
        // A target sound at the render's rate (resampled once a rate, Assets.h), as long as a render may be; mono twice.
        struct Target
        {
            std::vector<float> l, r;
            bool cut = false;
        };
        Target targetA, targetB;
        for (const auto& [side, into] : { std::pair<const Side*, Target*>{ &a, &targetA }, { &b, &targetB } })
            if (side->target != nullptr)
            {
                const std::shared_ptr<const assets::Audio> sound = side->target->at(req.rate);
                if (sound == nullptr || sound->frames <= 0)
                    return done("The target sound " + side->target->name() + " holds no sound.", true);
                const auto most = static_cast<std::size_t>(std::floor(std::min(req.longest, measure::kMaxRenderSamples / req.rate) * req.rate));
                const auto frames = static_cast<std::size_t>(sound->frames);
                const std::size_t n = std::min(frames, most);
                into->cut = n < frames;
                into->l.assign(sound->data.begin(), sound->data.begin() + static_cast<std::ptrdiff_t>(n));
                into->r = sound->channels > 1 ? std::vector<float>(sound->data.begin() + static_cast<std::ptrdiff_t>(frames),
                                                                   sound->data.begin() + static_cast<std::ptrdiff_t>(frames + n))
                                              : into->l;
            }
        measure::CompareSide A, B;
        const std::map<std::string, float> now = defaults ? std::map<std::string, float>{} : currentDials();
        for (const auto& [side, from] : { std::pair<measure::CompareSide*, Side*>{ &A, &a }, { &B, &b } })
        {
            side->files = from->files;
            side->request = req;
            if (from->own)
                side->request.assets = from->samples;
            side->request.current = now;                 // none: the dials no side sets at the program's defaults
            if (!current || from->given)                 // over the scenario's (a recording's, at its start)
                for (const auto& [name, x] : from->dials)
                    side->request.dials[name] = x;
            if (from->capture)
            {
                side->recordedL = &recording.l;
                side->recordedR = &recording.r;
            }
            if (from->target != nullptr)
            {
                const Target& t = from == &a ? targetA : targetB;
                side->recordedL = &t.l;
                side->recordedR = &t.r;
            }
        }
        B.request.rate = rateB;
        if (!scenario::fits(B.request, why))             // resolve() held A's to A's rate; B's at its own
            return done("B at b_rate: " + why, true);
        int trials = 0;
        if (input.hasProperty("try_edits") && (b.capture || b.target != nullptr))
            return done(std::string("try_edits apply to b, which is ") + (b.capture ? "the recording" : "the target sound")
                            + ": put the program in b.", true);
        if (!scenario::tryEdits(input, B.files, trials, why))
            return done(why, true);
        measure::CompareOptions options;
        options.detail = static_cast<bool>(input.getProperty("detail", false));
        options.cancel = renderCancel("Comparing the sounds");
        const measure::Comparison c = measure::compare(A, B, options);
        if (!c.error.empty())
            return done(scenario::trialLine(trials) + c.error, true);

        const measure::Sound& rendered = a.capture || a.target != nullptr ? c.b : c.a;
        std::string out = scenario::trialLine(trials) + scenario::dialsLine(defaults && !req.fromCapture);
        out += "Compared offline (nothing that plays changed): A is " + a.words + "; B is " + b.words + (trials > 0 ? " with the trial edits" : "")
             + ". Both: " + rendered.title + ", "
             + (c.twoRates() ? "A at " + number(c.rate) + " Hz and B at " + number(c.rateB) + " Hz," : "at " + number(c.rate) + " Hz")
             + " in blocks of " + (req.blocks.empty() ? std::to_string(req.block) : std::string("the recording's sizes")) + ".\n";
        if (a.playing || b.playing)
            out += "(\"playing\" is the running program's own code; once its dials hold still the plugin plays a frozen copy "
                   "whose last bits differ, which this does not render.)\n";
        if ((a.capture || b.capture) && rendered.title.find(recording.about) == std::string::npos)
            out += "(The recording: " + recording.about + ")\n";
        for (const Target* t : { &targetA, &targetB })
            if (!t->l.empty() && !t->cut)
                out += "(Rendered as long as the target sound: " + number(static_cast<double>(t->l.size()) / req.rate) + " s.)\n";
            else if (t->cut)
                out += "(The target sound is longer than a render may be here: its first " + number(static_cast<double>(t->l.size()) / req.rate)
                     + " s are compared" + (picture ? std::string("; without the picture, up to 60 s") : std::string()) + ".)\n";
        for (const std::string& line : c.lines)
            out += line + "\n";
        if (target)
        {
            // How near the program comes to the target in its spectrum's balance (no metrics: the file is no render).
            const capture::Spectrum sa = capture::spectrumOf(c.a.l, c.a.r, c.rate), sb = capture::spectrumOf(c.b.l, c.b.r, c.rateB);
            const auto words = [](const capture::Spectrum& x) {
                char buf[96];
                std::snprintf(buf, sizeof buf, "centroid %.0f Hz, left/right correlation %.2f", x.centroidHz, x.correlation);
                return x.silent ? std::string("silent") : std::string(buf);
            };
            out += "Spectra: A " + words(sa) + "; B " + words(sb) + ".\n";
        }
        // What each rendered, as bits (Export.h): a render elsewhere with either hash is the same.
        out += "Render hashes: A " + c.hashA.substr(0, measure::kHashShown) + ", B " + c.hashB.substr(0, measure::kHashShown)
             + " (SHA-256 of each side's float32 samples, L R interleaved).\n";
        if (!a.capture && !b.capture && !target)
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
            // a netlist's *@ lines (its drawing) are no change to the program (cir::sameProgram)
            std::string files = "the same files";
            if (!(A.files == B.files) && cir::sameProgram(A.files, B.files))
                files = "the same program (only a netlist's drawing, its *@ lines, differs)";
            else if (!(A.files == B.files))
            {
                projectstore::Version before;
                before.files = A.files;
                files = projectstore::describeChange(&before, B.files, {});
            }
            out += "Source, B against A: " + files + "; " + (dials.empty() ? std::string("the same dials") : "dials " + dials) + ".\n";
        }
        if (!picture)
            return done(out, false);

        // The picture: both levels over time and both spectra, on one scale.
        setActivity("Drawing the comparison");
        const measure::ViewRequest shape = soundpicture::request();
        const measure::View va = measure::analyse(c.a.l, c.a.r, c.rate, c.a.heldUntil, shape, c.a.cycleHz, false, c.a.fromStart);
        const measure::View vb = measure::analyse(c.b.l, c.b.r, c.rateB, c.b.heldUntil, shape, c.b.cycleHz, false, c.b.fromStart);
        const std::vector<double> bands(c.bandsDb.begin(), c.bandsDb.end());
        const soundpicture::Drawn d = soundpicture::drawPair(
            va, vb, c.twoRates() ? std::vector<float>(static_cast<std::size_t>(shape.columns), -240.0f) : differenceLevel(c.a, c.b, shape.columns),
            bands, "A: " + a.words + "   B: " + b.words,
            std::string("The picture: the levels over time (RMS over 10 ms, dBFS, A blue, B orange) and their difference's level in pink ")
                + (c.twoRates() ? "(none at two rates: no sample lines up)" : "(no line where they are the same)")
                + ", then both average spectra on one scale (dB under the louder), and the octave bands' differences along its foot.");
        juce::Array<juce::var> content;
        content.add(claude::object({ { "type", "image" },
                                     { "source", claude::object({ { "type", "base64" },
                                                                  { "media_type", "image/png" },
                                                                  { "data", juce::Base64::toBase64(d.png.data(), d.png.size()) } }) } }));
        content.add(claude::object({ { "type", "text" }, { "text", juce::String::fromUTF8((out + d.about).c_str()) } }));
        block.getDynamicObject()->setProperty("content", content);
        const std::lock_guard<std::mutex> lock(mutex_);
        addEntry(ChatEntry::Kind::tool, "compared two sounds");
        if (!req.fromCapture && !a.capture && !b.capture && !target)   // the user's recording or file: no thumbnail kept
            entries_.back().pictures.push_back(d.thumb);
        touch();
        return block;
    }
}
