// Source/plugin/ProcessorAb.cpp: the Processor's A/B listening (EditorPort::ab; Processor.h's AbRun; editor/AbCard.h).
//
// A side plays as a run of its files (Compiler::request): the Engine takes it as any program (the effect crossfades over
// 20 ms, the synth's old voices fade and the held keys start again on it), and the editor's draft stays as it is, so
// what plays and what the editor shows differ until a side is kept. With each side its own dials, they land by name
// when its compile does (a version's are, poll()'s abDialsRequest_), and the side left keeps the dials as the user left
// them there. The loudness is measured offline (AbMatch.h) and the louder side turned down on the audio thread by the
// program that plays (trimOutput: keyed by Patch::program, so the trim changes the block the program does).
//
// A program that lands from anything else (Claude's run, the folder's run on save, a session loaded, a version or an
// example opened) ends it: that program plays on, nothing restored (AbState::ended says so). Keeping a side makes its
// files the draft (an open editor merges them as one undo step) and, a version's, the project open; endAb puts back
// what played before (and its dials, when each side had its own). Message thread, but chatAb (any thread).
#include "plugin/Processor.h"

#include "fminus/CirDoc.h"
#include "fminus/Circuits.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace playground
{
    namespace
    {
        constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

        std::map<std::string, float> byName(const std::vector<projectstore::Dial>& dials)
        {
            std::map<std::string, float> out;
            for (const projectstore::Dial& d : dials)
                out[d.name] = d.value;
            return out;
        }

        std::string fixed(const char* format, double v)
        {
            char buf[48];
            std::snprintf(buf, sizeof buf, format, v);
            return buf;
        }

        const char* letter(int side) { return side == 0 ? "A" : "B"; }
    }

    // ---- EditorPort --------------------------------------------------------------------------------------------------
    AbState Processor::ab() const { return ab_.st; }

    bool Processor::abSide(const projectstore::Project* p, int version, int k, std::string& why)
    {
        const auto i = static_cast<std::size_t>(k);
        AbSide side;
        if (version <= 0)
        {
            side.label = "Current";
            side.note = "the editor's files as they were";
            side.files = ab_.currentFiles;
            ab_.dials[i] = ab_.saved[i] = ab_.currentDials;
            ab_.answers[i].clear();
            ab_.active[i] = ab_.currentActive;
        }
        else
        {
            const projectstore::Version* v = p != nullptr ? p->version(version) : nullptr;
            if (v == nullptr)
            {
                why = p == nullptr ? "no project to take version " + std::to_string(version) + " from"
                                   : "\"" + p->name + "\" keeps no version " + std::to_string(version);
                return false;
            }
            side.version = v->number;
            side.label = "v" + std::to_string(v->number);
            side.note = v->note;
            side.files = tidyFiles(v->files);
            if (side.files.empty())
            {
                why = "\"" + p->name + "\" version " + std::to_string(v->number) + " has no program";
                return false;
            }
            ab_.dials[i] = ab_.saved[i] = v->dials;
            ab_.answers[i] = v->circuits;
            ab_.active[i] = std::clamp(v->active, 0, static_cast<int>(side.files.size()) - 1);
        }
        ab_.st.sides[i] = std::move(side);
        ab_.request[i] = ab_.program[i] = 0;
        return true;
    }

    bool Processor::startAb(const std::string& project, int a, int b, std::string& why)
    {
        if (ab_.st.on && projectstore::sameName(project, ab_.st.project))
            return setAbSides(a, b, why);
        std::optional<projectstore::Project> p;
        if (a > 0 || b > 0)
        {
            p = projectstore::load(project, &why);
            if (!p)
                return false;
        }
        // Another project's while one is on: the sides anew, Current and what played before it began as they were.
        const bool again = ab_.st.on;
        const AbRun old = ab_;
        const uint64_t revision = ab_.st.revision;
        AbRun run;
        Sources nowFiles;
        uint64_t nowProgram = 0;
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            run.currentFiles = draft_.files;
            run.currentActive = draft_.active;
            run.beforeFiles = nowFiles = running_;
            run.beforeProgram = nowProgram = program_;
        }
        run.currentDials = dialsNow();
        run.beforeDials = run.currentDials;
        if (again)
        {
            run.currentFiles = old.currentFiles;
            run.currentActive = old.currentActive;
            run.currentDials = old.currentDials;
            run.beforeFiles = old.beforeFiles;
            run.beforeProgram = old.beforeProgram;
            run.beforeDials = old.beforeDials;
            run.requests = old.requests;
            run.seenRequest = old.seenRequest;
        }
        run.latest = p ? p->latest().number : 0;
        run.st.project = p ? p->name : std::string();
        run.st.lufs = { kNaN, kNaN };
        ab_ = std::move(run);
        ab_.st.revision = revision + 1;
        if (!abSide(p ? &*p : nullptr, a, 0, why) || !abSide(p ? &*p : nullptr, b, 1, why))
        {
            ab_ = again ? old : AbRun{};
            ab_.st.revision = revision + 1;
            return false;
        }
        ab_.st.on = true;
        // The newest request known now: a program that lands past it, and is none of ours, ends the A/B.
        if (!again)
            ab_.seenRequest = std::max({ compiler_.status().request, userRun_.load(), dialsRequest_, defaultsRequest_ });
        const auto words = [this](int k) {
            const AbSide& s = ab_.st.sides[static_cast<std::size_t>(k)];
            return s.version > 0 ? "version " + std::to_string(s.version) : std::string("the editor's files");
        };
        journal_.note(Journal::Who::user, "started A/B listening" + (ab_.st.project.empty() ? std::string() : " in \"" + ab_.st.project + "\"")
                                              + ": A " + words(0) + ", B " + words(1));
        // A side that plays already plays on; else A plays.
        bool plays = false;
        for (int k = 0; k < 2 && !plays; ++k)
            if (!nowFiles.empty() && nowProgram != 0 && cir::sameProgram(ab_.st.sides[static_cast<std::size_t>(k)].files, nowFiles))
            {
                ab_.st.playing = k;
                ab_.request[static_cast<std::size_t>(k)] = ab_.program[static_cast<std::size_t>(k)] = nowProgram;
                plays = true;
            }
        if (!plays)
            abPlay(0, false);
        abMeasure();
        abPublish();
        return true;
    }

    bool Processor::setAbSides(int a, int b, std::string& why)
    {
        if (!ab_.st.on)
        {
            why = "no A/B listening is on";
            return false;
        }
        if (ab_.st.sides[0].version == std::max(0, a) && ab_.st.sides[1].version == std::max(0, b))
            return true;                                         // the same sides: nothing to play or measure again
        std::optional<projectstore::Project> p;
        if (a > 0 || b > 0)
        {
            p = projectstore::load(ab_.st.project, &why);
            if (!p)
                return false;
        }
        const AbRun before = ab_;
        const int playing = ab_.st.playing, pending = ab_.st.pending;
        if (ab_.st.ownDials && playing >= 0 && pending < 0)
            ab_.dials[static_cast<std::size_t>(playing)] = dialsNow();
        bool moved = false;
        const std::array<int, 2> wanted{ a, b };
        for (int k = 0; k < 2; ++k)
        {
            const auto i = static_cast<std::size_t>(k);
            if (ab_.st.sides[i].version == std::max(0, wanted[i]))
                continue;
            if (!abSide(p ? &*p : nullptr, wanted[i], k, why))
            {
                const uint64_t revision = ab_.st.revision;
                ab_ = before;
                ab_.st.revision = revision;
                return false;
            }
            moved = moved || k == playing || k == pending;
        }
        if (p)
            ab_.latest = p->latest().number;
        ++ab_.st.revision;
        // The side that played (or was on its way) is another program now: it plays as such.
        if (moved)
        {
            const int k = pending >= 0 ? pending : playing;
            ab_.st.playing = ab_.st.pending = -1;
            ab_.pendingRequest = 0;
            abPlay(k, false);
        }
        abMeasure();
        abPublish();
        return true;
    }

    void Processor::playAb(int side) { abPlay(side, true); }

    void Processor::abPlay(int side, bool journal)
    {
        if (!ab_.st.on || side < 0 || side > 1)
            return;
        if (ab_.st.pending == side || (ab_.st.playing == side && ab_.st.pending < 0))
            return;
        const auto i = static_cast<std::size_t>(side);
        // The side left keeps the dials as the user left them there.
        if (ab_.st.ownDials && ab_.st.playing >= 0 && ab_.st.playing != side && ab_.st.pending < 0)
            ab_.dials[static_cast<std::size_t>(ab_.st.playing)] = dialsNow();
        for (const projectstore::Answer& a : ab_.answers[i])         // a version's netlists' answers, before it compiles
            circuits::restore(a.key, a.text);
        const uint64_t id = compiler_.request(ab_.st.sides[i].files);
        ab_.requests.push_back(id);
        ab_.request[i] = ab_.program[i] = id;                    // Patch::program is its request: the trim's key at once
        ab_.st.pending = side;
        ab_.pendingRequest = id;
        if (ab_.st.ownDials)
        {
            abDials_ = ab_.dials[i];
            abDialsRequest_ = id;
        }
        else
        {
            abDials_.clear();
            abDialsRequest_ = 0;
        }
        ab_.st.why.clear();
        ++ab_.st.revision;
        abPublish();
        if (journal)
            journal_.note(Journal::Who::user, std::string("A/B: switched to ") + letter(side) + " ("
                                                  + (ab_.st.sides[i].version > 0 ? "version " + std::to_string(ab_.st.sides[i].version)
                                                                                 : std::string("the editor's files as they were"))
                                                  + ")");
    }

    void Processor::setAbOwnDials(bool own)
    {
        if (!ab_.st.on || own == ab_.st.ownDials)
            return;
        const int playing = ab_.st.playing, pending = ab_.st.pending;
        if (own)
        {
            if (pending >= 0)
            {
                abDials_ = ab_.dials[static_cast<std::size_t>(pending)];
                abDialsRequest_ = ab_.pendingRequest;
            }
            else if (playing >= 0)
            {
                Controls cs;
                std::array<std::vector<std::string>, kMaxControls> options;
                {
                    const std::lock_guard<std::mutex> lock(programMutex_);
                    cs = controls_;
                    options = panel_.options;
                }
                setDialsByName(cs, options, ab_.dials[static_cast<std::size_t>(playing)]);
            }
        }
        else
        {
            if (playing >= 0 && pending < 0)
                ab_.dials[static_cast<std::size_t>(playing)] = dialsNow();
            if (pending >= 0 && abDialsRequest_ == ab_.pendingRequest)
            {
                abDials_.clear();
                abDialsRequest_ = 0;
            }
        }
        ab_.st.ownDials = own;
        ++ab_.st.revision;
        abMeasure();
        abPublish();
    }

    void Processor::setAbMatch(bool on)
    {
        if (!ab_.st.on || on == ab_.st.match)
            return;
        ab_.st.match = on;
        ++ab_.st.revision;
        abPublish();
    }

    bool Processor::keepAb(int side, std::string& why)
    {
        if (!ab_.st.on)
        {
            why = "no A/B listening is on";
            return false;
        }
        if (side < 0 || side > 1)
        {
            why = "a side is A or B";
            return false;
        }
        const auto i = static_cast<std::size_t>(side);
        const AbSide kept = ab_.st.sides[i];
        Draft d;
        d.files = tidyFiles(kept.files);
        if (d.files.empty())
        {
            why = std::string(letter(side)) + " has no program";
            return false;
        }
        d.active = std::clamp(ab_.active[i], 0, static_cast<int>(d.files.size()) - 1);
        abPlay(side, false);                                     // nothing when it plays already
        {
            const std::lock_guard<std::mutex> lock(programMutex_);
            if (draft_.files != d.files)
            {
                draft_ = d;
                ++draftRevision_;                                // an open editor merges it as one undo step
            }
            if (kept.version > 0)
            {
                projectName_ = ab_.st.project;
                projectVersion_ = kept.version;
                projectLatest_ = std::max(ab_.latest, kept.version);
                projectPreset_.clear();
                projectFiles_ = d.files;
                baseDials_ = ab_.saved[i];
            }
        }
        const std::string what = kept.version > 0 ? "version " + std::to_string(kept.version) + " of \"" + ab_.st.project + "\""
                                                  : std::string("the editor's files as they were");
        if (ab_.st.ownDials)
            journal_.dialsNote(Journal::Who::user, std::string("kept ") + letter(side) + " of the A/B listening (" + what + ", its own dials)");
        else
            journal_.note(Journal::Who::user, std::string("kept ") + letter(side) + " of the A/B listening (" + what + ")");
        abStop();
        return true;
    }

    void Processor::endAb()
    {
        if (!ab_.st.on)
            return;
        const int playing = ab_.st.playing;
        const bool same = ab_.st.pending < 0 && playing >= 0 && ab_.program[static_cast<std::size_t>(playing)] == ab_.beforeProgram;
        if (!same && !ab_.beforeFiles.empty())
        {
            const uint64_t id = compiler_.request(ab_.beforeFiles);
            if (ab_.st.ownDials)
            {
                abDials_ = ab_.beforeDials;
                abDialsRequest_ = id;
            }
            else
            {
                abDials_.clear();
                abDialsRequest_ = 0;
            }
        }
        else if (ab_.st.ownDials)
        {
            Controls cs;
            std::array<std::vector<std::string>, kMaxControls> options;
            {
                const std::lock_guard<std::mutex> lock(programMutex_);
                cs = controls_;
                options = panel_.options;
            }
            setDialsByName(cs, options, ab_.beforeDials);
        }
        journal_.note(Journal::Who::user, "ended the A/B listening: what played before plays again");
        abStop();
    }

    // ---- poll()'s ----------------------------------------------------------------------------------------------------
    void Processor::abLanded(const CompileStatus& s)
    {
        if (!ab_.st.on)
            return;
        bool changed = false;
        if (ab_.pendingRequest != 0 && s.request == ab_.pendingRequest && !s.refined)
        {
            const int k = ab_.st.pending;
            const auto i = static_cast<std::size_t>(k);
            ab_.pendingRequest = 0;
            ab_.st.pending = -1;
            if (s.ok)
            {
                ab_.st.playing = k;
                ab_.program[i] = s.program;
                ab_.st.why.clear();
            }
            else
            {
                ab_.program[i] = 0;
                const std::string first = s.errors.substr(0, s.errors.find('\n'));
                ab_.st.why = std::string(letter(k)) + " (" + ab_.st.sides[i].label + ") does not compile"
                           + (first.empty() ? std::string() : ": " + first);
            }
            changed = true;
        }
        else if (s.ok && s.refined && ab_.st.playing >= 0)
        {
            // The self-test's refine of the side that plays (Compiler.h): a new program, the same side.
            const auto i = static_cast<std::size_t>(ab_.st.playing);
            if (s.request == ab_.request[i] && s.program != ab_.program[i])
            {
                ab_.program[i] = s.program;
                changed = true;
            }
        }
        else if (s.ok && !s.refined && s.request > ab_.seenRequest
                 && std::find(ab_.requests.begin(), ab_.requests.end(), s.request) == ab_.requests.end())
        {
            abStop();
            ab_.st.ended = "Another program runs now (Claude's, a session's, a version or an example opened): the A/B ended";
            return;
        }
        if (changed)
        {
            ++ab_.st.revision;
            abPublish();
        }
    }

    void Processor::abMatched()
    {
        ab::Result r;
        if (!abMatcher_.take(r) || !ab_.st.on || r.id != ab_.job)
            return;
        ab_.st.lufs = r.lufs;
        ab_.st.trimDb = r.trimDb;
        ab_.st.unmatched = r.why;
        ab_.st.measuring = false;
        ++ab_.st.revision;
        abPublish();
    }

    void Processor::abMeasure()
    {
        ab::Job job;
        job.id = ++abJobs_;
        ab_.job = job.id;
        const std::vector<projectstore::Dial> now = dialsNow();
        job.current = byName(now);
        for (std::size_t k = 0; k < 2; ++k)
        {
            job.files[k] = ab_.st.sides[k].files;
            if (ab_.st.ownDials)
                job.dials[k] = byName(static_cast<int>(k) == ab_.st.playing && ab_.st.pending < 0 ? now : ab_.dials[k]);
        }
        const double rate = engine_.rate();
        job.rate = rate >= 8000.0 ? rate : 48000.0;
        ab_.st.measuring = true;
        abMatcher_.start(std::move(job));
    }

    void Processor::abStop()
    {
        abMatcher_.cancel();
        ab_.st.on = false;
        ab_.st.pending = -1;
        ab_.st.measuring = false;
        ab_.st.ended.clear();
        ab_.pendingRequest = 0;
        ab_.requests.clear();
        ++ab_.st.revision;
        abPublish();
    }

    void Processor::abPublish()
    {
        const AbState& st = ab_.st;
        for (std::size_t k = 0; k < 2; ++k)
        {
            const float g = st.on && st.match ? std::pow(10.0f, std::min(0.0f, st.trimDb[k]) / 20.0f) : 1.0f;
            abTrim_[k].gain.store(g, std::memory_order_relaxed);
            abTrim_[k].program.store(st.on ? ab_.program[k] : 0, std::memory_order_release);
        }
        std::string line;
        if (st.on)
        {
            const auto side = [&](std::size_t k) {
                const AbSide& s = st.sides[k];
                return s.version > 0 ? "version " + std::to_string(s.version) + " of \"" + st.project + "\" (" + s.note + ")"
                                     : std::string("the editor's files as they were when it began");
            };
            line = "A/B listening (the user's, in PROJECTS; nothing kept yet): A is " + side(0) + ", B is " + side(1) + "; ";
            if (st.pending >= 0)
                line += std::string(letter(st.pending)) + " is compiling";
            else if (st.playing >= 0)
                line += std::string(letter(st.playing)) + " plays" + (st.playing == 1 || st.sides[0].version > 0 ? " (not the editor's files)" : "");
            else
                line += "neither plays";
            line += st.ownDials ? "; each side its own dials" : "; the same dials for both";
            if (!st.match)
                line += "; loudness not matched (off)";
            else if (st.measuring)
                line += "; measuring the loudness";
            else if (!st.unmatched.empty())
                line += "; loudness not matched: " + st.unmatched;
            else if (std::isfinite(st.lufs[0]) && std::isfinite(st.lufs[1]))
            {
                const int down = st.trimDb[0] < st.trimDb[1] ? 0 : 1;
                line += "; loudness matched (A " + fixed("%.1f", st.lufs[0]) + " LUFS, B " + fixed("%.1f", st.lufs[1]) + " LUFS"
                      + (st.trimDb[static_cast<std::size_t>(down)] < 0.0f
                             ? std::string(", ") + letter(down) + " turned down " + fixed("%.1f", -st.trimDb[static_cast<std::size_t>(down)]) + " dB)"
                             : std::string(")"));
            }
            if (!st.why.empty())
                line += "; " + st.why;
            line += ".";
        }
        const std::lock_guard<std::mutex> lock(abLineMutex_);
        abLine_ = std::move(line);
    }

    std::string Processor::chatAb() const
    {
        const std::lock_guard<std::mutex> lock(abLineMutex_);
        return abLine_;
    }

    // ---- the audio thread ----------------------------------------------------------------------------------------------
    // The Engine's output turned by the trim of the program that plays (none: 1), the gain ramping over 20 ms when it
    // changes; untouched while the gain is 1 and still.
    void Processor::trimOutput(float* const* channels, int numChannels, int numSamples) noexcept
    {
        const uint64_t program = engine_.playingProgram();
        float target = 1.0f;
        if (program != 0)
            for (const AbTrim& t : abTrim_)
                if (t.program.load(std::memory_order_acquire) == program)
                {
                    target = t.gain.load(std::memory_order_relaxed);
                    break;
                }
        if (!(target >= 0.0f && target <= 1.0f))
            target = 1.0f;
        if (!trimSet_)                                           // the first block after prepareToPlay: no ramp
        {
            trimFrom_ = trimTo_ = trimNow_ = target;
            trimSet_ = true;
        }
        else if (target != trimTo_)
        {
            trimFrom_ = trimNow_;
            trimTo_ = target;
            trimFade_.lin.cur = 0.0f;
            trimFade_.setTarget(1.0f);
        }
        if (numSamples <= 0)
            return;
        if (!trimFade_.moving())
        {
            trimNow_ = trimTo_;
            if (trimNow_ == 1.0f)
                return;
            for (int c = 0; c < numChannels; ++c)
                for (int i = 0; i < numSamples; ++i)
                    channels[c][i] *= trimNow_;
            return;
        }
        for (int i = 0; i < numSamples; ++i)
            weight_[static_cast<std::size_t>(i)] = trimFrom_ + (trimTo_ - trimFrom_) * trimFade_.tick();
        trimNow_ = weight_[static_cast<std::size_t>(numSamples - 1)];
        for (int c = 0; c < numChannels; ++c)
            for (int i = 0; i < numSamples; ++i)
                channels[c][i] *= weight_[static_cast<std::size_t>(i)];
    }
}
